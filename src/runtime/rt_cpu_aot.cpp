// See rt_cpu_aot.h.
#include "runtime/rt_cpu_aot.h"

#include "runtime/rt_host.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

#if defined(__linux__)
#include <sys/mman.h>
#include <ucontext.h>
#define CPU_AOT_FIBERS 1
#endif

namespace d2rt {

#ifdef CPU_AOT_FIBERS
namespace {

constexpr size_t kFiberStack = 1u << 20;   // measured on the lane first, then cut (docs/aot.md)

struct Fiber {
    ucontext_t ctx;
    void *stack = nullptr;
    uint32_t entry = 0;
    bool started = false, done = false, stopped = false;
    alignas(8) uint8_t fpu[416] = {};   // AotFpu holds doubles
};

CpuAotDispatch g_dispatch = nullptr;

class CpuAot : public Cpu {
public:
    CpuAot()
    {
        // The guest's whole span as one block, guest 0 at base_ (WX86_ARENA, as
        // the Box86 backend takes it).
        const char *as = std::getenv("WX86_ARENA");
        span_ = as ? (uint32_t)std::strtoul(as, nullptr, 16) : 0x10000000u;
        void *p = mmap(nullptr, span_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            std::fprintf(stderr, "[cpu_aot] no %u MiB for the guest's memory\n", (unsigned)(span_ >> 20));
            std::abort();
        }
        base_ = (uint8_t *)p;
    }

    bool map(uint32_t va, uint32_t size, const void *init, int) override
    {
        if (!in(va, size))
            return false;
        if (init)
            std::memcpy(base_ + va, init, size);
        return true;
    }
    bool protect(uint32_t, uint32_t, int) override { return true; }
    bool read(uint32_t va, void *dst, uint32_t n) override
    {
        if (!in(va, n))
            return false;
        std::memcpy(dst, base_ + va, n);
        return true;
    }
    bool write(uint32_t va, const void *src, uint32_t n) override
    {
        if (!in(va, n))
            return false;
        std::memcpy(base_ + va, src, n);
        return true;
    }
    void *hostptr(uint32_t va, uint32_t n) override { return in(va, n) ? base_ + va : nullptr; }
    bool mapped(uint32_t va) override { return va < span_; }
    void invalidate_code(uint32_t, uint32_t) override { code_changed_ = true; }

    uint32_t reg(int r) override { return regs_[r]; }
    void set_reg(int r, uint32_t v) override { regs_[r] = v; }
    void set_fs_base(uint32_t base) override { fs_ = base; }
    bool fs_base_is_direct() const override { return true; }

    void set_trap(uint32_t lo, uint32_t hi, TrapFn fn) override
    {
        trap_lo_ = lo;
        trap_hi_ = hi;
        trap_ = std::move(fn);
    }
    void request_stop() override {}

    // Preemption (docs/aot.md): the scheduler gives each slice its length in
    // guest instructions (300,000; less for a background thread). Compiled
    // code doesn't count them, so a slice ends after a share of that in trap
    // calls with no wait between (one for each 1,024 instructions): a thread
    // that polls the clock in a loop still gives the others their turn.
    void set_run_limit(uint64_t n) override { limit_ = n; }
    bool take_limit_hit() override
    {
        const bool h = hit_;
        hit_ = false;
        return h;
    }
    bool slice_over()
    {
        if (!limit_ || !cur_ || ++calls_ < std::max<uint64_t>(16, limit_ >> 10))
            return false;
        hit_ = true;
        return true;
    }

    // The scheduler's: the thread whose TIB it set, on its fiber, until it
    // yields or ends. A run from inside a fiber (a shim's guest_call) is the
    // dispatcher, in place.
    bool run(uint32_t eip, const char **fault) override
    {
        if (fault)
            *fault = nullptr;
        if (!g_dispatch)
            cpu_aot_stop("no dispatcher (the AOT code isn't linked or installed)", eip);
        if (cur_)
            return g_dispatch(*this, eip, 0), true;
        std::unique_ptr<Fiber> &slot = fibers_[fs_];
        if (!slot) {
            slot.reset(new Fiber);
            Fiber *f = slot.get();
            f->stack = mmap(nullptr, kFiberStack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (f->stack == MAP_FAILED)
                cpu_aot_stop("no host stack for a thread", eip);
            mprotect(f->stack, 4096, PROT_NONE);   // a guard page under it
            const uint16_t cw = 0x27f;
            std::memcpy(f->fpu, &cw, 2);
            const uint32_t empty = 0xffffu;        // every x87 register empty
            std::memcpy(f->fpu + 140, &empty, 4);
            f->entry = eip;
            getcontext(&f->ctx);
            f->ctx.uc_stack.ss_sp = f->stack;
            f->ctx.uc_stack.ss_size = kFiberStack;
            f->ctx.uc_link = &sched_;
            makecontext(&f->ctx, (void (*)())&CpuAot::fiber_main, 0);
        }
        Fiber *f = slot.get();
        cur_ = f;
        calls_ = 0;
        swapcontext(&sched_, &f->ctx);
        cur_ = nullptr;
        if (f->done) {
            munmap(f->stack, kFiberStack);
            fibers_.erase(fs_);
        }
        return true;
    }
    uint32_t fault_addr() const override { return 0; }
    void *x87_state() override { return fpu(); }

    bool is_trap(uint32_t va) const { return va >= trap_lo_ && va < trap_hi_; }
    bool trap(uint32_t va) { return trap_(*this, va); }
    void yield()
    {
        Fiber *f = cur_;
        if (!f)
            cpu_aot_stop("a yield outside a thread's fiber", regs_[R_EIP]);
        swapcontext(&f->ctx, &sched_);
        cur_ = f;   // run() set it again; kept explicit
    }
    void *fpu() { return cur_ ? cur_->fpu : boot_fpu_; }
    bool take_code_changed()
    {
        const bool c = code_changed_;
        code_changed_ = false;
        return c;
    }

private:
    static void fiber_main()
    {
        CpuAot *self = g_self;
        Fiber *f = self->cur_;
        g_dispatch(*self, f->entry, 0);
        f->done = true;
        swapcontext(&f->ctx, &self->sched_);
    }
    bool in(uint32_t va, uint32_t n) const { return (uint64_t)va + n <= span_; }

    uint8_t *base_ = nullptr;
    uint32_t span_ = 0;
    uint32_t regs_[R_COUNT] = {};
    uint32_t fs_ = 0;
    uint32_t trap_lo_ = 0, trap_hi_ = 0;
    TrapFn trap_;
    std::map<uint32_t, std::unique_ptr<Fiber>> fibers_;   // by the thread's TIB
    Fiber *cur_ = nullptr;
    ucontext_t sched_;
    bool code_changed_ = true;
    uint64_t limit_ = 0, calls_ = 0;
    bool hit_ = false;
    alignas(8) uint8_t boot_fpu_[416] = {};

public:
    static CpuAot *g_self;
};
CpuAot *CpuAot::g_self = nullptr;

}  // namespace

Cpu *make_cpu_aot()
{
    if (!CpuAot::g_self)
        CpuAot::g_self = new CpuAot;
    return CpuAot::g_self;
}
bool cpu_is_aot(Cpu &c) { return CpuAot::g_self && &c == CpuAot::g_self; }
void cpu_aot_set_dispatch(CpuAotDispatch fn) { g_dispatch = fn; }
bool cpu_aot_trap(Cpu &c, uint32_t va) { return static_cast<CpuAot &>(c).trap(va); }
bool cpu_aot_is_trap(Cpu &c, uint32_t va) { return static_cast<CpuAot &>(c).is_trap(va); }
void cpu_aot_yield(Cpu &c) { static_cast<CpuAot &>(c).yield(); }
void *cpu_aot_fpu(Cpu &c) { return static_cast<CpuAot &>(c).fpu(); }
bool cpu_aot_take_code_changed(Cpu &c) { return static_cast<CpuAot &>(c).take_code_changed(); }
bool cpu_aot_slice_over(Cpu &c) { return static_cast<CpuAot &>(c).slice_over(); }

#else   // no fibers on this host yet (the 3DS: a context switch of its own, docs/aot.md)

Cpu *make_cpu_aot() { return nullptr; }
bool cpu_is_aot(Cpu &) { return false; }
void cpu_aot_set_dispatch(CpuAotDispatch) {}
bool cpu_aot_trap(Cpu &, uint32_t) { return false; }
bool cpu_aot_is_trap(Cpu &, uint32_t) { return false; }
void cpu_aot_yield(Cpu &) {}
void *cpu_aot_fpu(Cpu &) { return nullptr; }
bool cpu_aot_take_code_changed(Cpu &) { return false; }
bool cpu_aot_slice_over(Cpu &) { return false; }

#endif

void cpu_aot_stop(const char *what, uint32_t at)
{
    rt::logf("cpu_aot: STOP at %08x: %s", at, what);
    std::fprintf(stderr, "[rt] RESULT exited=0 timed_out=0 code=86 cpu_aot stopped at %08x: %s\n", at, what);
    std::fflush(stderr);
    std::_Exit(86);
}

}  // namespace d2rt
