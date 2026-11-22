// See rt_cpu_aot.h.
#include "runtime/rt_cpu_aot.h"

#include "runtime/rt_host.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#if defined(__linux__) || defined(__3DS__)
#include <sys/mman.h>   // the 3DS's is the backend's (shim/ctr/mman_ctr.c: svcControlMemory)
#define CPU_AOT_MMAP 1
#endif
#include <malloc.h>

// A guest thread's host context is a stack and the registers a call keeps
// (AAPCS: r4-r11, lr, d8-d15), saved on it. One switch for the lane (ARMv6
// under qemu) and the 3DS: aot_ctx_switch(&from_sp, to_sp).
#if defined(__arm__)
extern "C" void aot_ctx_switch(void **save_sp, void *load_sp);
__asm__(".text\n.arm\n.fpu vfp\n.align 2\n.global aot_ctx_switch\n.type aot_ctx_switch, %function\n"
        "aot_ctx_switch:\n"
        "    push {r4-r11, lr}\n"
        "    vpush {d8-d15}\n"
        "    str sp, [r0]\n"
        "    mov sp, r1\n"
        "    vpop {d8-d15}\n"
        "    pop {r4-r11, pc}\n"
        ".size aot_ctx_switch, .-aot_ctx_switch\n");
#define CPU_AOT_FIBERS 1
#endif

namespace d2rt {

#ifdef CPU_AOT_FIBERS
namespace {

// A thread's host stack. The lane's is large and reports what was used
// (cpu_aot_lines), which sets the 3DS's.
#if defined(__linux__)
constexpr size_t kFiberStack = 1u << 20;
#else
constexpr size_t kFiberStack = 128u << 10;   // the lane's scripts use 10 KiB at most (docs/aot.md)
#endif
constexpr uint8_t kStackFill = 0xa5;

struct Fiber {
    void *sp = nullptr;      // its saved context, while it isn't running
    uint8_t *stack = nullptr;
    uint32_t tib = 0;
    uint32_t entry = 0;
    bool done = false;
    alignas(8) uint8_t fpu[416] = {};   // AotFpu holds doubles
};

CpuAotDispatch g_dispatch = nullptr;

class CpuAot : public Cpu {
public:
    CpuAot()
    {
        // The guest's whole span as one block, guest 0 at base_ (WX86_ARENA, as
        // the Box86 backend takes it).
        // (WX86_ARENA is the layout's span and 16 MiB the Box86 backend
        // aligns its block with; the span alone is taken here.)
        const char *as = std::getenv("WX86_ARENA");
        span_ = as ? (uint32_t)std::strtoul(as, nullptr, 16) : 0x10000000u;
        if (span_ > 0x1000000u)
            span_ -= 0x1000000u;
#ifdef CPU_AOT_MMAP
        void *p = mmap(nullptr, span_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED)
            p = nullptr;
#else
        void *p = std::calloc(1, span_);
#endif
        if (!p) {
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
#if defined(__linux__)
            void *st = mmap(nullptr, kFiberStack + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (st == MAP_FAILED)
                cpu_aot_stop("no host stack for a thread", eip);
            mprotect(st, 4096, PROT_NONE);   // a guard page under it
            f->stack = (uint8_t *)st + 4096;
#else
            f->stack = (uint8_t *)memalign(8, kFiberStack);
            if (!f->stack)
                cpu_aot_stop("no host stack for a thread", eip);
#endif
            std::memset(f->stack, kStackFill, kFiberStack);
            f->tib = fs_;
            const uint16_t cw = 0x27f;
            std::memcpy(f->fpu, &cw, 2);
            const uint32_t empty = 0xffffu;        // every x87 register empty
            std::memcpy(f->fpu + 140, &empty, 4);
            f->entry = eip;
            // What aot_ctx_switch pops: d8-d15, r4-r11, then pc.
            uint32_t *top = (uint32_t *)(((uintptr_t)f->stack + kFiberStack) & ~(uintptr_t)7) - 25;
            std::memset(top, 0, 25 * 4);
            top[24] = (uint32_t)(uintptr_t)&CpuAot::fiber_main;
            f->sp = top;
        }
        Fiber *f = slot.get();
        cur_ = f;
        calls_ = 0;
        aot_ctx_switch(&sched_sp_, f->sp);
        cur_ = nullptr;
        if (f->done) {
            rt::logf("cpu_aot: the thread at TIB %08x ended, %u KiB of its %u KiB host stack used", f->tib,
                     (unsigned)(used(*f) >> 10), (unsigned)(kFiberStack >> 10));
#if defined(__linux__)
            munmap(f->stack - 4096, kFiberStack + 4096);
#else
            std::free(f->stack);
#endif
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
        aot_ctx_switch(&f->sp, sched_sp_);
        cur_ = f;   // run() set it again; kept explicit
    }
    // Each live thread's host stack: the most of it used so far.
    void lines(std::vector<std::string> &out)
    {
        for (const auto &kv : fibers_) {
            char line[128];
            std::snprintf(line, sizeof line, "cpu_aot: the thread at TIB %08x: %u KiB of its %u KiB host stack used",
                          kv.second->tib, (unsigned)(used(*kv.second) >> 10), (unsigned)(kFiberStack >> 10));
            out.push_back(line);
        }
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
        aot_ctx_switch(&f->sp, self->sched_sp_);
        __builtin_unreachable();
    }
    static size_t used(const Fiber &f)
    {
        size_t k = 0;
        while (k < kFiberStack && f.stack[k] == kStackFill)
            k++;
        return kFiberStack - k;
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
    void *sched_sp_ = nullptr;
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
std::vector<std::string> cpu_aot_lines()
{
    std::vector<std::string> out;
    if (CpuAot::g_self)
        CpuAot::g_self->lines(out);
    return out;
}

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
std::vector<std::string> cpu_aot_lines() { return {}; }

#endif

void cpu_aot_stop(const char *what, uint32_t at)
{
    rt::logf("cpu_aot: STOP at %08x: %s", at, what);
    std::fprintf(stderr, "[rt] RESULT exited=0 timed_out=0 code=86 cpu_aot stopped at %08x: %s\n", at, what);
    std::fflush(stderr);
#ifdef __3DS__
    std::abort();   // the app's own end for a fatal error: its report and its screen
#else
    std::_Exit(86);
#endif
}

}  // namespace d2rt
