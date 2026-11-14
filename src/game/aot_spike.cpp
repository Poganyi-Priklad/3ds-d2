// The AOT spike's runtime side (issue #113, aot_spike.h, aot_rt.h). The
// compiled functions come from build/aot/ (tools/aot/recompile.py, the
// user's Game.exe), linked only when the build is given them: their tables
// are weak here.
#include "game/aot_spike.h"
#include "game/aot_rt.h"

#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/guest_thread.h"
#include "runtime/rt_host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifdef __3DS__
#include <3ds/svc.h>
#endif

extern "C" {
uint8_t *aot_mem = nullptr;
// From the generated file, compiled three ways (aot_rt.h): absent in a build
// without it.
extern const AotRoot aot_roots[] __attribute__((weak));
extern const AotRoot aotn_roots[] __attribute__((weak));
extern const AotRoot aotj_roots[] __attribute__((weak));
}

using d2rt::Cpu;

namespace game {
uint8_t *aot_guest_base();   // d2_native.cpp: its g_base
namespace {

Cpu *g_cpu = nullptr;
AotMode g_mode = AotMode::Off, g_wanted = AotMode::Off;

uint64_t ticks()
{
#ifdef __3DS__
    return svcGetSystemTick();   // 268,111,856 Hz
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
#ifdef __3DS__
constexpr double kTicksPerUs = 268.111856;
#else
constexpr double kTicksPerUs = 1000.0;
#endif

// What Bench times: Box86 (the original from its trampoline, nested), the
// four builds of the compiled code, and our native where there is one.
enum Variant { kBox86, kSig, kCtx, kSigCalls, kCtxCalls, kNative, kVariants };
constexpr const char *kVariantNames[kVariants] = {"box86", "sig", "ctx", "sig-calls", "ctx-calls", "native"};

struct Root {
    const AotRoot *r = nullptr, *calls = nullptr, *journal = nullptr;
    uint32_t tramp = 0;          // the game's first instructions, then a jump back
    uint32_t trap = 0;           // our native entry
    uint8_t before[5] = {};      // what was at the entry before our jump
    bool hooked = false;
    AotNativeFn native = nullptr;
    // Per heartbeat window.
    uint32_t calls_n[kVariants] = {};
    uint64_t ticks_n[kVariants] = {};
    uint32_t checked = 0, differ = 0, nested = 0;
    // Since the start (the check's totals).
    uint32_t checked_all = 0, differ_all = 0;
    uint32_t turn = 0;
};
Root g_roots[8];
int g_root_count = 0;
uint32_t g_empty = 0;            // a ret, for the cost of a nested run
uint32_t g_empty_n = 0;
uint64_t g_empty_ticks = 0;
uint32_t g_nested = 0;           // nested runs from compiled code (callees not compiled)
uint32_t g_bad = 0;

struct NativeAt {
    uint32_t va;
    AotNativeFn fn;
};
NativeAt g_natives[8];
int g_native_count = 0;

// The registers of a nested run, and the guest call itself.
void nested_run(Cpu &c, uint32_t va, uint32_t *r)
{
    d2rt::X86Context saved;
    c.save_context(saved);
    const uint32_t esp = r[4];
    const uint32_t ra = c.read_u32(esp);   // the compiled caller's return address
    c.write_u32(esp, rt::bridge().sentinel());
    for (int k = 0; k < 8; k++)
        c.set_reg(k, r[k]);
    const char *fault = nullptr;
    if (!c.run(va, &fault))
        rt::logf("aot: nested run of %08x faulted: %s at %08x", va, fault ? fault : "?", c.fault_addr());
    c.regs_gp(r);
    c.write_u32(esp, ra);   // as Box86 would have left it
    c.load_context(saved);
}

// The journal of AOT_JOURNAL's stores: where, and what was there.
struct Store {
    uint32_t va, n, at;   // at: its old bytes in g_old
};
std::vector<Store> g_journal;
std::vector<uint8_t> g_old;

// D2_AOT=5: the compiled code journaled, its writes put back, then the
// original under Box86 from the same registers and esp; compared: eax, the
// callee-saved registers, and every byte the compiled code wrote, except the
// stack below the entry's esp + 4 (dead once the call returns). A byte only
// Box86 wrote isn't seen here; the host cases (Unicorn) compare every byte.
void check(Root &R, Cpu &c, uint32_t *regs)
{
    uint32_t a[8], b[8];
    std::memcpy(a, regs, sizeof a);
    std::memcpy(b, regs, sizeof b);
    g_journal.clear();
    g_old.clear();
    R.journal->sig(a);
    std::vector<uint8_t> now(g_old.size());
    for (const Store &s : g_journal)
        std::memcpy(&now[s.at], aot_mem + s.va, s.n);
    for (size_t k = g_journal.size(); k-- > 0;)
        std::memcpy(aot_mem + g_journal[k].va, &g_old[g_journal[k].at], g_journal[k].n);
    nested_run(c, R.tramp, b);
    const uint32_t esp = regs[4];
    bool bad = a[0] != b[0] || a[3] != b[3] || a[5] != b[5] || a[6] != b[6] || a[7] != b[7];
    uint32_t first = 0;
    for (const Store &s : g_journal) {
        for (uint32_t k = 0; k < s.n; k++) {
            const uint32_t va = s.va + k;
            if (va < esp + 4 && va + 0x40000 > esp)
                continue;   // the dead stack
            // A later store to the same byte wins: compare against the last.
            if (aot_mem[va] != now[s.at + k]) {
                bool later = false;
                for (const Store &t : g_journal)
                    if (&t > &s && va >= t.va && va < t.va + t.n)
                        later = true;
                if (!later && !bad) {
                    bad = true;
                    first = va;
                }
            }
        }
    }
    R.checked++;
    R.checked_all++;
    if (bad) {
        R.differ++;
        if (++R.differ_all <= 10)
            rt::logf("aot check: %s differs (call %u): eax %08x / Box86 %08x, ebx %08x/%08x esi %08x/%08x "
                     "edi %08x/%08x ebp %08x/%08x, first byte %08x",
                     R.r->name, R.checked_all, a[0], b[0], a[3], b[3], a[6], b[6], a[7], b[7], a[5], b[5], first);
    }
    if ((R.checked_all & (R.checked_all - 1)) == 0)
        rt::logf("aot check: %s %u calls, %u differ", R.r->name, R.checked_all, R.differ_all);
    std::memcpy(regs, b, sizeof b);   // the game's result is kept
}

uint32_t enter(Root &R, Cpu &c)
{
    uint32_t regs[8];
    c.regs_gp(regs);
    g_cpu = &c;
    AotMode m = g_mode;
    Variant v = kSig;
    if (m == AotMode::Bench) {
        const int n = R.native ? kVariants : kNative;
        v = (Variant)(R.turn++ % (uint32_t)n);
        if (v == kBox86 && (R.turn & 63) == 1 && g_empty) {   // now and then, the cost of a nested run itself
            uint32_t e[8];
            std::memcpy(e, regs, sizeof e);
            const uint64_t t0 = ticks();
            nested_run(c, g_empty, e);
            g_empty_ticks += ticks() - t0;
            g_empty_n++;
        }
    }
    const uint64_t t0 = ticks();
    switch (m) {
    case AotMode::Sig: R.r->sig(regs); break;
    case AotMode::Ctx: R.r->ctx(regs); break;
    case AotMode::SigCalls: R.calls->sig(regs); break;
    case AotMode::CtxCalls: R.calls->ctx(regs); break;
    case AotMode::Check: check(R, c, regs); break;
    case AotMode::Bench:
        switch (v) {
        case kBox86: nested_run(c, R.tramp, regs); break;
        case kSig: R.r->sig(regs); break;
        case kCtx: R.r->ctx(regs); break;
        case kSigCalls: R.calls->sig(regs); break;
        case kCtxCalls: R.calls->ctx(regs); break;
        default: {
            // The native reads the Cpu; its result is eax, and it may write
            // others (a hand-back runs the game's code on from the entry:
            // not timed as the native's, counted as Box86's).
            const uint32_t eax = R.native(c);
            if (rt::bridge().take_redirect()) {
                // It handed the call back (resume_original; nothing written
                // but below esp): the whole call runs nested from the
                // trampoline instead, and counts as Box86's.
                for (int k = 0; k < 8; k++)
                    c.set_reg(k, regs[k]);
                nested_run(c, R.tramp, regs);
                v = kBox86;
            } else {
                c.regs_gp(regs);
                regs[0] = eax;
            }
        }
        }
        break;
    default: nested_run(c, R.tramp, regs); break;
    }
    const uint64_t t1 = ticks();
    R.calls_n[m == AotMode::Bench ? v : kSig]++;
    R.ticks_n[m == AotMode::Bench ? v : kSig] += t1 - t0;
    for (int k = 0; k < 8; k++)
        if (k != 4)
            c.set_reg(k, regs[k]);
    return regs[0];
}

void hook(Root &R, Cpu &c)
{
    if (R.hooked)
        return;
    c.read(R.r->va, R.before, 5);
    uint8_t jmp[5] = {0xE9};
    const uint32_t rel = R.trap - (R.r->va + 5);
    std::memcpy(jmp + 1, &rel, 4);
    c.write(R.r->va, jmp, 5);
    c.invalidate_code(R.r->va, 5);
    R.hooked = true;
}

void unhook(Root &R, Cpu &c)
{
    if (!R.hooked)
        return;
    c.write(R.r->va, R.before, 5);
    c.invalidate_code(R.r->va, 5);
    R.hooked = false;
}

}  // namespace

bool aot_linked() { return aot_roots != nullptr && aot_roots[0].va != 0; }
AotMode aot_mode() { return g_mode; }

void aot_set_native(uint32_t va, AotNativeFn fn)
{
    for (int k = 0; k < g_root_count; k++)
        if (g_roots[k].r->va == va)
            g_roots[k].native = fn;
    if (g_native_count < 8)
        g_natives[g_native_count++] = {va, fn};
}

void set_aot_wanted(AotMode mode) { g_wanted = mode; }

int install_aot(Cpu &c)
{
    AotMode mode = g_wanted;
    if (const char *e = std::getenv("D2_AOT"))
        mode = (AotMode)std::atoi(e);
    if (!aot_linked()) {
        if (mode != AotMode::Off)
            rt::logf("aot: this build has no compiled code (build it with D2_AOT=1, docs/aot.md)");
        return 0;
    }
    aot_mem = aot_guest_base();   // guest 0 in host memory, as the natives have it
    const uint32_t at = rt::va_alloc(0x1000, 0x0067b850);
    if (!at) {
        rt::logf("aot: no memory for the trampolines");
        return 0;
    }
    g_empty = at + 0xff0;
    const uint8_t ret = 0xc3;
    c.write(g_empty, &ret, 1);
    int n = 0;
    for (int k = 0; aot_roots[k].va && g_root_count < 8; k++) {
        const AotRoot *r = &aot_roots[k];
        uint8_t have[16];
        c.read(r->va, have, 16);
        if (std::memcmp(have, r->head, 16) != 0) {
            rt::logf("aot: %s at %08x: the bytes differ from the build's Game.exe, not hooked", r->name, r->va);
            continue;
        }
        Root &R = g_roots[g_root_count++];
        R.r = r;
        R.calls = aotn_roots ? &aotn_roots[k] : r;
        R.journal = aotj_roots ? &aotj_roots[k] : nullptr;
        R.tramp = at + 32 * k;
        uint8_t t[32];
        std::memcpy(t, r->head, r->keep);
        t[r->keep] = 0xe9;
        const uint32_t rel = (r->va + r->keep) - (R.tramp + r->keep + 5);
        std::memcpy(t + r->keep + 1, &rel, 4);
        c.write(R.tramp, t, r->keep + 5u);
        Root *Rp = &R;
        R.trap = rt::native_entry(r->name, r->pops / 4, [Rp](Cpu &cc) { return enter(*Rp, cc); });
        for (int j = 0; j < g_native_count; j++)
            if (g_natives[j].va == r->va)
                R.native = g_natives[j].fn;
        n++;
    }
    c.invalidate_code(at, 0x1000);
    set_aot_mode(c, mode);
    rt::logf("aot: %d compiled roots%s, mode %d", n, aotj_roots ? " (with the check build)" : "", (int)mode);
    return n;
}

void set_aot_mode(Cpu &c, AotMode mode)
{
    if (mode == AotMode::Check && !aotj_roots)
        mode = AotMode::Sig;
    g_mode = mode;
    for (int k = 0; k < g_root_count; k++) {
        if (mode == AotMode::Off)
            unhook(g_roots[k], c);
        else
            hook(g_roots[k], c);
    }
}

std::vector<std::string> aot_lines()
{
    std::vector<std::string> out;
    const double empty_us = g_empty_n ? (double)g_empty_ticks / g_empty_n / kTicksPerUs : 0;
    for (int k = 0; k < g_root_count; k++) {
        Root &R = g_roots[k];
        uint32_t all = 0;
        for (uint32_t x : R.calls_n)
            all += x;
        if (!all)
            continue;
        char line[320];
        int p = std::snprintf(line, sizeof line, "aot: %s %u calls:", R.r->name, all);
        for (int v = 0; v < kVariants; v++)
            if (R.calls_n[v] && p < (int)sizeof line)
                p += std::snprintf(line + p, sizeof line - p, " %s %u %.2f us", kVariantNames[v], R.calls_n[v],
                                   (double)R.ticks_n[v] / R.calls_n[v] / kTicksPerUs);
        if (g_mode == AotMode::Bench && p < (int)sizeof line)
            p += std::snprintf(line + p, sizeof line - p, "; a nested run alone %.2f us (%u)", empty_us, g_empty_n);
        if (R.checked && p < (int)sizeof line)
            p += std::snprintf(line + p, sizeof line - p, "; checked %u, differ %u", R.checked, R.differ);
        out.push_back(line);
        std::memset(R.calls_n, 0, sizeof R.calls_n);
        std::memset(R.ticks_n, 0, sizeof R.ticks_n);
        R.checked = R.differ = 0;
    }
    if (g_nested) {
        char line[96];
        std::snprintf(line, sizeof line, "aot: %u nested runs of callees not compiled", g_nested);
        out.push_back(line);
        g_nested = 0;
    }
    g_empty_n = 0;
    g_empty_ticks = 0;
    return out;
}

}  // namespace game

// ---- what the generated C calls (aot_rt.h) ----

// The host test (tests/d2d) runs no game code: it sets this to count a
// nested run as left to the game.
void (*aot_guest_hook)(uint32_t va, AotRegs *r) = nullptr;

extern "C" {

uint32_t aot_fs_base(void) { return rt::current_teb(); }

void aot_guest_call(uint32_t va, AotRegs *r)
{
    game::g_nested++;
    if (aot_guest_hook) {
        aot_guest_hook(va, r);
        return;
    }
    game::nested_run(*game::g_cpu, va, r->r);
}

void aot_bad_ret(uint32_t fn, uint32_t esp0, uint32_t esp)
{
    rt::logf("aot: %08x returned with esp %08x, entered with %08x: the static stack doesn't hold", fn, esp, esp0);
    std::abort();
}

void aot_unreachable(uint32_t va)
{
    rt::logf("aot: reached %08x, which the recompiler took for unreachable", va);
    std::abort();
}

void aot_fault(uint32_t va)
{
    rt::logf("aot: a division by zero at %08x", va);
    std::abort();
}

void aot_journal(uint32_t va, uint32_t n)
{
    game::g_journal.push_back({va, n, (uint32_t)game::g_old.size()});
    game::g_old.insert(game::g_old.end(), aot_mem + va, aot_mem + va + n);
}

}  // extern "C"
