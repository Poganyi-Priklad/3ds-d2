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

#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <set>

#ifdef __3DS__
extern "C" {
#include <3ds/svc.h>   // svcGetSystemTick (a C header without its own extern "C")
}
#endif

extern "C" {
uint8_t *aot_mem = nullptr;
// From the generated file, compiled three ways (aot_rt.h): absent in a build
// without it.
extern const AotRoot aot_roots[] __attribute__((weak));
extern const AotRoot aotn_roots[] __attribute__((weak));
extern const AotFunc aot_funcs[] __attribute__((weak));
extern const AotFunc aotj_funcs[] __attribute__((weak));
}

using d2rt::Cpu;

namespace game {
uint8_t *aot_guest_base();   // d2_native.cpp: its g_base
namespace {

// Per host thread (issue #115): on the console each guest thread runs on a
// host thread of its own, and one blocked in a shim lets another run, which
// may enter compiled code too; the first's calls to Box86 must still go
// through its own Cpu.
__thread Cpu *g_cpu = nullptr;
uint32_t g_fns_first = 0, g_fns_last = UINT32_MAX;   // set_aot_fns
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
// kHandedBack: the native's turns it handed back (they then ran nested under
// Box86, the attempt included), kept apart from Box86's own.
enum Variant { kBox86, kSig, kCtx, kSigCalls, kCtxCalls, kNative, kHandedBack, kVariants };
constexpr const char *kVariantNames[kVariants] = {"box86", "sig", "ctx", "sig-calls", "ctx-calls", "native",
                                                  "handed-back"};

struct Root {
    const AotRoot *r = nullptr, *calls = nullptr;
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
std::map<uint32_t, uint32_t> g_nested_by;   // ... by callee, this window

struct NativeAt {
    uint32_t va;
    AotNativeFn fn;
};
NativeAt g_natives[8];
int g_native_count = 0;

uint32_t g_nested_yields = 0;   // shims inside a nested run that asked to yield (logged)

// The registers of a nested run, and the guest call itself.
//
// run() returns at every stop, not only at the sentinel: on the console the
// profile sampler and the stall probe zero the block budget from their own
// threads (rt_host.cpp's nudge_thread_budget), and a shim may ask to yield.
// Each comes back with EIP at the resume point, so this runs on until EIP
// is the sentinel, as the backend's own nested calls do (cpu_box86.cpp, the
// SEH dispatch). Taking the first stop for the end would leave the game's
// thread with registers from the middle of the callee. A nudge taken here
// is sent again after, so the outer slice still stops for it. (A nested
// run must never come from a trap served in line, inside the dynarec's own
// run: its stop at the sentinel would end that run too. install_aot names
// the roots' traps apart for this.) A yield can't be honoured inside a
// nested run (the scheduler can't switch away from it): an advisory one is
// resumed; a wait that blocked the thread is done in place (rt::
// finish_nested_block), else it would go on with the wait's placeholder for a
// result. A wait on another guest thread can't end there: the recompiler
// leaves to Box86 what could reach one (recompile.py's may_block).
void nested_run(Cpu &c, uint32_t va, uint32_t *r)
{
    d2rt::X86Context saved;
    c.save_context(saved);
    const uint32_t esp = r[4];
    const uint32_t ra = c.read_u32(esp);   // the compiled caller's return address
    const uint32_t sentinel = rt::bridge().sentinel();
    c.write_u32(esp, sentinel);
    for (int k = 0; k < 8; k++)
        c.set_reg(k, r[k]);
    const char *fault = nullptr;
    bool stopped = false;
    for (uint32_t eip = va;;) {
        if (!c.run(eip, &fault)) {
            rt::logf("aot: nested run of %08x faulted: %s at %08x", va, fault ? fault : "?", c.fault_addr());
            break;
        }
        eip = c.reg(d2rt::R_EIP);
        if (eip == sentinel)
            break;
        stopped = true;
        const bool lim = c.take_limit_hit();
        const bool yielded = rt::bridge().take_yield();
        // A shim that blocked the thread (the I/O worker's read on the
        // console) left a placeholder in EAX for the scheduler's wake: the
        // wait is done here, in place (issue #115: Storm's reads got 0).
        const uint32_t waited_us = yielded ? rt::finish_nested_block(c) : 0;
        if (yielded)
            g_nested_yields++;
        static uint32_t logged = 0;
        if (++logged <= 10)
            rt::logf("aot: a nested run of %08x stopped at %08x (%s), resumed", va, eip,
                     lim ? "slice" : waited_us ? "a wait, done in place" : yielded ? "a shim's yield" : "no reason given");
    }
    if (stopped)
        c.nudge_thread_budget(nullptr);
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

// D2_AOT=5, the check (issue #114): every compiled function whose entry can
// be hooked is, and each of its calls, from the game's code or from inside
// another check's run of the original, runs twice. First its compiled code,
// journaled, with its writes put back after; then the original from its
// trampoline under Box86, from the same registers and esp. Compared: the
// registers its compiled callers read after it (out), those it keeps
// (preserved), and every byte the compiled code wrote, except the stack below
// the entry's esp + 4 (dead once the call returns). A byte only Box86 wrote
// isn't seen here; the host cases (Unicorn) compare every byte. The game
// keeps Box86's result, so a call inside the original's run is checked in its
// turn, at every depth.
//
// The compiled code's own calls to compiled functions are C calls, checked
// as part of it. A call it makes to Box86 (a callee not compiled) writes
// memory the journal doesn't see: that call keeps the compiled result,
// nothing is put back or run twice, and it counts as unchecked.
struct Fn {
    const AotFunc *f = nullptr;    // aot_funcs': what Sig runs
    const AotFunc *fj = nullptr;   // aotj_funcs' (journaled): what Check runs
    uint32_t tramp = 0, trap = 0;
    uint32_t trap_in_line = 0;     // a leaf's (AotFunc::leaf), served in line: what Sig hooks
    uint8_t before[5] = {};
    bool hooked = false;
    uint32_t checked = 0, differ = 0, unchecked = 0;           // this window
    uint32_t entered = 0;                                      // traps from Box86, this window
    uint64_t ticks_in = 0;                                     // D2_AOT_PROF: time inside, this window
    uint32_t probe = 0;                                        // entries seen since turned on (Sig)
    uint64_t probe_ticks = 0;                                  // ... the timed ones' time inside
    uint64_t probe_nested_ticks = 0;                           // ... of it, in nested runs (Box86's)
    uint32_t probe_nested = 0;                                 // ... and their number
    bool dropped = false;                                      // left to Box86 after its probe
    uint32_t direct = 0;                                       // natives' guest_calls run in C, this window
    uint32_t checked_all = 0, differ_all = 0, unchecked_all = 0;
};
std::vector<Fn> g_fns;           // sized once at install: entries keep their address
std::unordered_map<uint32_t, uint32_t> g_fn_at;   // va -> index in g_fns
__thread int g_journaling = 0;      // inside a journaled compiled run (this thread's)
__thread bool g_impure = false;     // ... which called Box86
uint32_t g_differ_logged = 0;

const char *reg_name(int k)
{
    static const char *const n[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    return n[k & 7];
}

__thread int g_prof_depth = 0;   // D2_AOT_PROF: compiled code entered from Box86, inside one another

// An exit (issue #115, aot_exit): compiled code hands the rest of its call
// to Box86 at a call that may block, an import or an indirect call. It
// comes back here, to the trap that entered it, with Box86's target and the
// registers there; the C frames between are compiled code's, dropped.
struct ExitJump {   // on the stack at each trap: left uninitialised
    std::jmp_buf jb;
    uint32_t target;
    uint32_t r[8];
};
__thread ExitJump *g_exit_jump = nullptr;
uint32_t g_exits = 0;                       // this window
std::map<uint32_t, uint32_t> g_exits_to;    // ... by Box86's target
// Callouts (aot_call_out), this window: our shims and compiled functions
// called in C; those that came back other than assumed (an exit at the
// return), by callee.
bool g_callouts = true;   // D2_AOT_NOCALLOUT (the lane): every callout an exit
// Compiled code entered where there's no trap to exit to (a native's
// guest_call, aot_run_compiled): only functions without hard exits, and a
// callout's fallback a nested run, as Box86's own call would be.
__thread int g_no_exit = 0;
uint32_t g_handed = 0;   // natives' hand-backs to compiled code (aot_trap_for), this window
uint32_t g_callout_shims = 0, g_callout_fns = 0, g_callout_back = 0;
std::map<uint32_t, uint32_t> g_callout_back_by;

// fn(regs), an exit caught: false when the code exited (into x).
bool run_catching(AotRootFn fn, uint32_t *regs, ExitJump &x)
{
    ExitJump *volatile prev = g_exit_jump;
    const volatile int depth = g_prof_depth, journaling = g_journaling, no_exit = g_no_exit;
    g_exit_jump = &x;
    g_no_exit = 0;   // a trap of its own (in a nested run under one that can't exit)
    if (setjmp(x.jb)) {
        g_exit_jump = prev;
        g_prof_depth = depth;
        g_journaling = journaling;
        g_no_exit = no_exit;
        g_exits++;
        g_exits_to[x.target]++;
        return false;
    }
    fn(regs);
    g_exit_jump = prev;
    g_no_exit = no_exit;
    return true;
}

// F's compiled code on regs: true when it returned, false when it exited.
// Only a function that may exit pays for the catch.
inline bool run_fn(const AotFunc *f, uint32_t *regs, ExitJump &x)
{
    if (!f->may_exit) {
        f->sig(regs);
        return true;
    }
    return run_catching(f->sig, regs, x);
}

// Check: false when the compiled code exited; Box86 goes on at x.target with
// x.r (from the function's entry, x.target its trampoline, when the
// journal put back all it wrote).
bool check_fn(Fn &F, Cpu &c, uint32_t *regs, ExitJump &x)
{
    if (g_journaling) {   // from Box86 inside a journaled run: the original, unchecked
        g_impure = true;
        nested_run(c, F.tramp, regs);
        return true;
    }
    uint32_t a[8], b[8];
    std::memcpy(a, regs, sizeof a);
    std::memcpy(b, regs, sizeof b);
    g_journal.clear();
    g_old.clear();
    g_impure = false;
    g_journaling++;
    const bool done = F.fj->may_exit ? run_catching(F.fj->sig, a, x) : (F.fj->sig(a), true);
    g_journaling = 0;
    if (!done) {
        // Exited: unchecked. With nothing but its own stores to put back,
        // Box86 runs the whole call from the entry; else on from the exit.
        F.unchecked++;
        F.unchecked_all++;
        if (!g_impure) {
            for (size_t k = g_journal.size(); k-- > 0;)
                std::memcpy(aot_mem + g_journal[k].va, &g_old[g_journal[k].at], g_journal[k].n);
            x.target = F.tramp;
            std::memcpy(x.r, b, sizeof b);
        }
        return false;
    }
    if (g_impure) {
        F.unchecked++;
        F.unchecked_all++;
        std::memcpy(regs, a, sizeof a);
        return true;
    }
    // This check's journal, apart: a check inside the original's run below
    // starts one of its own.
    std::vector<Store> journal;
    std::vector<uint8_t> old, now(g_old.size());
    journal.swap(g_journal);
    old.swap(g_old);
    for (const Store &st : journal)
        std::memcpy(&now[st.at], aot_mem + st.va, st.n);
    for (size_t k = journal.size(); k-- > 0;)
        std::memcpy(aot_mem + journal[k].va, &old[journal[k].at], journal[k].n);
    nested_run(c, F.tramp, b);
    const uint32_t esp = regs[4];
    int bad_reg = -1;
    for (int k = 0; k < 8 && bad_reg < 0; k++)
        if (k != 4 && ((F.f->out | F.f->preserved) >> k & 1) && a[k] != b[k])
            bad_reg = k;
    uint32_t first = 0;
    uint8_t mine = 0;
    bool bad_mem = false;
    for (size_t j = 0; j < journal.size() && !bad_mem; j++) {
        const Store &st = journal[j];
        for (uint32_t k = 0; k < st.n && !bad_mem; k++) {
            const uint32_t va = st.va + k;
            if (va < esp + 4 && va + 0x40000 > esp)
                continue;   // the dead stack
            if (aot_mem[va] == now[st.at + k])
                continue;
            bool later = false;   // a later store to the same byte wins: compare against the last
            for (size_t t = j + 1; t < journal.size() && !later; t++)
                later = va >= journal[t].va && va < journal[t].va + journal[t].n;
            if (!later) {
                bad_mem = true;
                first = va;
                mine = now[st.at + k];
            }
        }
    }
    F.checked++;
    F.checked_all++;
    if (bad_reg >= 0 || bad_mem) {
        F.differ++;
        F.differ_all++;
        if (++g_differ_logged <= 20) {
            char what[96];
            if (bad_reg >= 0)
                std::snprintf(what, sizeof what, "%s %08x, Box86 %08x", reg_name(bad_reg), a[bad_reg], b[bad_reg]);
            else
                std::snprintf(what, sizeof what, "the byte at %08x: %02x, Box86 %02x", first,
                              mine, aot_mem[first]);
            rt::logf("aot check: %s (%08x) differs on its call %u: %s; in eax %08x ecx %08x edx %08x ebx %08x "
                     "esp %08x ebp %08x esi %08x edi %08x",
                     F.f->name, F.f->va, F.checked_all, what, regs[0], regs[1], regs[2], regs[3], regs[4], regs[5],
                     regs[6], regs[7]);
        }
    }
    std::memcpy(regs, b, sizeof b);   // the game's result is kept
    return true;
}

void unhook_at(Cpu &c, uint32_t va, const uint8_t *before, bool &hooked);

// D2_AOT_PROF=1 (the lane, issue #115): each entry from Box86 timed, for
// which functions are worth a trap; the outermost ones' total against the
// window's own time.
const bool g_prof = std::getenv("D2_AOT_PROF") != nullptr;
// The probe (issue #115): a function whose compiled code takes less than
// this per call from Box86 costs more in the trap than it saves, and is left
// to Box86 (unhooked; compiled callers still call it in C). Its entries
// kProbeSkip on are timed, kProbeN of them. 0: every function stays hooked.
// (Box86's own time for it, run nested, can't be the measure: a nested run
// costs 8-10 us on the lane, and more than an empty one's for the function's
// own: 11-29 us against compiled code's 0.6-2.7 for the fight's hottest.)
constexpr uint32_t kProbeSkip = 32, kProbeN = 256;
double g_min_us = 8.0;   // issue #115: with exits, 8 did better than 2 on both lane scenes
uint32_t g_dropped = 0, g_dropped_window = 0;
// The nested runs compiled code starts (aot_guest_call): their time and
// number, this thread's, for the probe.
__thread uint64_t g_nested_ticks = 0;
__thread uint32_t g_nested_n = 0;
uint64_t g_prof_outer = 0, g_prof_window_t0 = 0;
// ... and where Box86 called from (the return address), this window: the
// callers that would save the most crossings compiled.
std::map<uint64_t, uint32_t> g_prof_sites;

uint32_t enter_fn(Fn &F, Cpu &c)
{
    uint32_t regs[8];
    c.regs_gp(regs);
    g_cpu = &c;
    F.entered++;
    ExitJump x;
    bool done = true;
    if (g_mode == AotMode::Check && F.fj)
        done = check_fn(F, c, regs, x);
    else if (g_mode == AotMode::Sig && F.probe < kProbeSkip + kProbeN && g_min_us > 0) {
        if (F.probe++ < kProbeSkip) {
            done = run_fn(F.f, regs, x);
        } else {
            const uint64_t t0 = ticks(), n0 = g_nested_ticks;
            const uint32_t c0 = g_nested_n;
            done = run_fn(F.f, regs, x);
            F.probe_ticks += ticks() - t0;
            F.probe_nested_ticks += g_nested_ticks - n0;
            F.probe_nested += g_nested_n - c0;
            // What it saves is its compiled code's time, less a crossing's
            // worth for each nested run it starts (issue #115: 4f8850, two
            // crossings a call, looked worth its trap with the run counted in).
            const double own_us = (double)(F.probe_ticks - F.probe_nested_ticks) / kProbeN / kTicksPerUs -
                                  (double)F.probe_nested / kProbeN * g_min_us;
            if (F.probe == kProbeSkip + kProbeN && g_prof)
                rt::logf("aot probe: %08x %.2f us in compiled code, %.2f nested runs a call: %s", F.f->va, own_us,
                         (double)F.probe_nested / kProbeN, own_us < g_min_us ? "left to Box86" : "kept");
            if (F.probe == kProbeSkip + kProbeN && own_us < g_min_us) {
                // Box86 runs it from its next call (the page's code marked
                // changed, as the game's own writes are).
                unhook_at(c, F.f->va, F.before, F.hooked);
                F.dropped = true;
                g_dropped++;
                g_dropped_window++;
            }
        }
    } else if (g_mode == AotMode::Sig && g_prof) {
        g_prof_sites[(uint64_t)F.f->va << 32 | aot_ld32(aot_mem, regs[4])]++;
        const uint64_t t0 = ticks();
        g_prof_depth++;
        done = run_fn(F.f, regs, x);
        g_prof_depth--;
        const uint64_t dt = ticks() - t0;
        F.ticks_in += dt;
        if (!g_prof_depth)
            g_prof_outer += dt;
    } else if (g_mode == AotMode::Sig)
        done = run_fn(F.f, regs, x);
    else
        nested_run(c, F.tramp, regs);   // hooked only in Sig and Check; between modes, the original
    if (!done) {
        // Box86 goes on at the exit's target with its registers: esp as the
        // trap's tail leaves it once it has taken off what its ret pops.
        for (int k = 0; k < 8; k++)
            if (k != 4)
                c.set_reg(k, x.r[k]);
        c.set_reg(4, x.r[4] - (4u + F.f->pops));
        rt::bridge().redirect_next(x.target);
        return x.r[0];
    }
    for (int k = 0; k < 8; k++)
        if (k != 4)
            c.set_reg(k, regs[k]);
    return regs[0];
}

// A native's guest_call (issue #115: the floor's interpolated light calls
// 4f8850 about 100k times a fight window) into a compiled function: in C,
// without the nested run. Sig only (Check's goes through the trap and is
// checked); a function the natives changed since it was compiled isn't
// hooked, and stays nested; one the probe left to Box86 still runs in C here
// (the run costs more than any compiled function).
bool direct_fn(Cpu &c, uint32_t va, uint32_t *regs)
{
    if (g_mode != AotMode::Sig)
        return false;
    const auto it = g_fn_at.find(va);
    if (it == g_fn_at.end())
        return false;
    Fn &F = g_fns[it->second];
    if ((!F.hooked && !F.dropped) || F.f->may_hard)   // a hard exit needs a trap to go back to
        return false;
    g_cpu = &c;
    F.direct++;
    g_no_exit++;
    F.f->sig(regs);
    g_no_exit--;
    return true;
}


uint32_t enter(Root &R, Cpu &c)
{
    uint32_t regs[8];
    c.regs_gp(regs);
    g_cpu = &c;
    AotMode m = g_mode;
    Variant v = kSig;
    if (m == AotMode::Bench) {
        const int n = R.native ? kHandedBack : kNative;
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
    ExitJump x;
    bool done = true;
    switch (m) {
    case AotMode::Sig: done = run_catching(R.r->sig, regs, x); break;
    case AotMode::Ctx: done = run_catching(R.r->ctx, regs, x); break;
    case AotMode::SigCalls: done = run_catching(R.calls->sig, regs, x); break;
    case AotMode::CtxCalls: done = run_catching(R.calls->ctx, regs, x); break;
    case AotMode::Bench:
        switch (v) {
        case kBox86: nested_run(c, R.tramp, regs); break;
        case kSig: done = run_catching(R.r->sig, regs, x); break;
        case kCtx: done = run_catching(R.r->ctx, regs, x); break;
        case kSigCalls: done = run_catching(R.calls->sig, regs, x); break;
        case kCtxCalls: done = run_catching(R.calls->ctx, regs, x); break;
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
                v = kHandedBack;
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
    if (!done) {   // an exit (issue #115): Box86 goes on there, as enter_fn's
        for (int k = 0; k < 8; k++)
            if (k != 4)
                c.set_reg(k, x.r[k]);
        c.set_reg(4, x.r[4] - (4u + R.r->pops));
        rt::bridge().redirect_next(x.target);
        return x.r[0];
    }
    for (int k = 0; k < 8; k++)
        if (k != 4)
            c.set_reg(k, regs[k]);
    return regs[0];
}

// A jump to `trap` at va, what was there kept in before[5].
void hook_at(Cpu &c, uint32_t va, uint32_t trap, uint8_t *before, bool &hooked)
{
    if (hooked)
        return;
    c.read(va, before, 5);
    uint8_t jmp[5] = {0xE9};
    const uint32_t rel = trap - (va + 5);
    std::memcpy(jmp + 1, &rel, 4);
    c.write(va, jmp, 5);
    c.invalidate_code(va, 5);
    hooked = true;
}

void unhook_at(Cpu &c, uint32_t va, const uint8_t *before, bool &hooked)
{
    if (!hooked)
        return;
    c.write(va, before, 5);
    c.invalidate_code(va, 5);
    hooked = false;
}

void hook(Root &R, Cpu &c) { hook_at(c, R.r->va, R.trap, R.before, R.hooked); }
void unhook(Root &R, Cpu &c) { unhook_at(c, R.r->va, R.before, R.hooked); }

}  // namespace

bool aot_linked() { return (aot_roots && aot_roots[0].va) || (aot_funcs && aot_funcs[0].va); }
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
void set_aot_min_us(double us) { g_min_us = us; }

uint32_t aot_trap_for(uint32_t va)
{
    if (g_mode != AotMode::Sig && g_mode != AotMode::Check)
        return 0;
    const auto it = g_fn_at.find(va);
    if (it == g_fn_at.end())
        return 0;
    g_handed++;
    return g_fns[it->second].trap;
}
void set_aot_fns(uint32_t first, uint32_t last)
{
    g_fns_first = first;
    g_fns_last = last;
}

namespace {
// The functions' entries (issues #114, #115): one per compiled function
// whose entry can be hooked, each with a trampoline and a trap of its own.
// Hooked in Sig (the compiled code from Box86's calls) and Check
// (set_aot_mode).
void install_fns(Cpu &c)
{
    const AotFunc *funcs = aot_funcs ? aot_funcs : nullptr;
    if (!funcs)
        return;
    size_t n = 0, all = 0;
    for (const AotFunc *f = funcs; f->va; f++, all++)
        n += f->keep ? 1 : 0;
    if (!n)
        return;
    const uint32_t bytes = (uint32_t)((32 * n + 0xfff) & ~(size_t)0xfff);
    const uint32_t at = rt::va_alloc(bytes, 0x0067b850);
    if (!at) {
        rt::logf("aot check: no memory for %u trampolines", (unsigned)n);
        return;
    }
    g_fns.reserve(n);
    std::string skipped;
    uint32_t moved = 0, index = 0, left_out = 0, in_line = 0;
    // D2_AOT_FNS=<first>-<last> (the lane, issue #115): only the hookable
    // functions with those indexes (from 0, in the table's order), to find
    // by halves one that goes wrong where the check doesn't see it.
    uint32_t first = g_fns_first, last = g_fns_last;
    if (const char *m = std::getenv("D2_AOT_MIN_US"))   // the lane: the probe's threshold (set_aot_min_us)
        g_min_us = std::atof(m);
    if (const char *r = std::getenv("D2_AOT_FNS"))
        std::sscanf(r, "%u-%u", &first, &last);
    for (const AotFunc *f = funcs; f->va; f++) {
        const AotFunc *fj = aotj_funcs && aotj_funcs[f - funcs].va == f->va ? &aotj_funcs[f - funcs] : nullptr;
        if (!f->keep) {
            if (skipped.size() < 160)
                skipped += (skipped.empty() ? "" : ", ") + std::string(f->name);
            continue;
        }
        if (index < first || index > last) {
            index++;
            left_out++;
            continue;
        }
        index++;
        uint8_t have[16];
        c.read(f->va, have, 16);
        if (std::memcmp(have, f->head, f->keep) != 0) {   // a native's hook there already, or another Game.exe
            moved++;
            continue;
        }
        g_fns.emplace_back();
        Fn &F = g_fns.back();
        F.f = f;
        F.fj = fj;
        F.tramp = at + 32 * (uint32_t)(g_fns.size() - 1);
        uint8_t t[32];
        std::memcpy(t, f->head, f->keep);
        t[f->keep] = 0xe9;
        const uint32_t rel = (f->va + f->keep) - (F.tramp + f->keep + 5);
        std::memcpy(t + f->keep + 1, &rel, 4);
        c.write(F.tramp, t, f->keep + 5u);
        Fn *Fp = &F;
        const std::string trap_name = std::string("aotfn:") + f->name;
        F.trap = rt::native_entry(trap_name.c_str(), f->pops / 4, [Fp](Cpu &cc) { return enter_fn(*Fp, cc); });
        // A leaf (issue #115) never runs Box86 nested, so Box86 can enter it
        // in line, without leaving its run: a trap of its own, for Sig. The
        // check runs the original nested, through the other.
        if (f->leaf && !std::getenv("D2_AOT_NOINLINE")) {
            F.trap_in_line = rt::native_entry((trap_name + "/in line").c_str(), f->pops / 4,
                                              [Fp](Cpu &cc) { return enter_fn(*Fp, cc); });
            if (F.trap_in_line)
                c.set_inline_shim(F.trap_in_line);
            in_line++;
        }
    }
    c.invalidate_code(at, bytes);
    for (uint32_t k = 0; k < g_fns.size(); k++)
        g_fn_at[g_fns[k].f->va] = k;
    // D2_AOT_NODIRECT (the lane): the natives' guest_calls nested, as before.
    if (!std::getenv("D2_AOT_NODIRECT"))
        rt::set_guest_direct(direct_fn);
    g_callouts = !std::getenv("D2_AOT_NOCALLOUT");
    rt::logf("aot: %u of %u compiled functions can be hooked (%u entered in line)%s%s%s", (unsigned)g_fns.size(),
             (unsigned)all, (unsigned)in_line, moved ? " (some had other bytes at their entry)" : "", skipped.empty() ? "" : "; not: ",
             skipped.c_str());
    if (left_out)
        rt::logf("aot: %u left out (D2_AOT_FNS %u-%u)", (unsigned)left_out, (unsigned)first, (unsigned)last);
}
}  // namespace

namespace {
AotMode g_pending = AotMode::Off;   // the lane's D2_AOT, until the natives are in
}  // namespace

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
        // D2_AOT_ONLY=<name,...> (the lane): only those roots. Each hooked
        // root changes the lane's thread switches from its first call on,
        // and with them the world the virtual clock makes.
        if (const char *only = std::getenv("D2_AOT_ONLY")) {
            const size_t len = std::strlen(r->name);
            const char *at = std::strstr(only, r->name);
            if (!at || (at[len] != 0 && at[len] != ',')) {
                rt::logf("aot: %s not hooked (D2_AOT_ONLY)", r->name);
                continue;
            }
        }
        Root &R = g_roots[g_root_count++];
        R.r = r;
        R.calls = aotn_roots ? &aotn_roots[k] : r;
        R.tramp = at + 32 * k;
        uint8_t t[32];
        std::memcpy(t, r->head, r->keep);
        t[r->keep] = 0xe9;
        const uint32_t rel = (r->va + r->keep) - (R.tramp + r->keep + 5);
        std::memcpy(t + r->keep + 1, &rel, 4);
        c.write(R.tramp, t, r->keep + 5u);
        Root *Rp = &R;
        // A name of its own: the bridge keeps one trap per name, and a native
        // on the same entry (PathSearch) would otherwise get this one back and
        // mark it in line (hook_function), with the nested runs then inside
        // the dynarec's own run (issue #113: the console's bench froze).
        const std::string trap_name = std::string("aot:") + r->name;
        R.trap = rt::native_entry(trap_name.c_str(), r->pops / 4, [Rp](Cpu &cc) { return enter(*Rp, cc); });
        for (int j = 0; j < g_native_count; j++)
            if (g_natives[j].va == r->va)
                R.native = g_natives[j].fn;
        n++;
    }
    c.invalidate_code(at, 0x1000);
    install_fns(c);
    // Sig and Check hook every compiled function, which the natives' hooks
    // must go in before (issue #115: on the console they're in before the
    // OPTIONS row turns it on); aot_after_natives() turns them on.
    if (mode == AotMode::Sig || mode == AotMode::Check) {
        g_pending = mode;
        set_aot_mode(c, AotMode::Off);
    } else {
        set_aot_mode(c, mode);
    }
    rt::logf("aot: %d compiled roots%s, mode %d", n, aotj_funcs ? " (with the check build)" : "", (int)mode);
    return n;
}

void aot_after_natives(Cpu &c)
{
    if (g_pending == AotMode::Off)
        return;
    set_aot_mode(c, g_pending);
    g_pending = AotMode::Off;
}

namespace {
// FNV-1a over a compiled function's code as it is now, as recompile.py's
// code_hash() took it from the Game.exe it compiled.
uint32_t code_hash(Cpu &c, const AotFunc *f)
{
    uint32_t h = 0x811c9dc5u;
    uint8_t buf[256];
    for (uint32_t k = 0; k < f->nranges; k++)
        for (uint32_t va = f->ranges[2 * k], hi = f->ranges[2 * k + 1]; va < hi;) {
            const uint32_t n = std::min<uint32_t>(hi - va, sizeof buf);
            c.read(va, buf, n);
            for (uint32_t j = 0; j < n; j++)
                h = (h ^ buf[j]) * 0x01000193u;
            va += n;
        }
    return h;
}

// The compiled functions to leave to Box86 (issue #115): those whose code
// changed since it was compiled (a native's patch: its hook or code it
// replaced in line, which the compiled code would skip), and every compiled
// function that reaches one in C. With every function unhooked.
std::vector<uint8_t> stale_funcs(Cpu &c, uint32_t &changed, std::string &names)
{
    size_t n = 0;
    while (aot_funcs[n].va)
        n++;
    std::vector<uint8_t> bad(n);
    changed = 0;
    for (size_t k = 0; k < n; k++)
        if (aot_funcs[k].nranges && code_hash(c, &aot_funcs[k]) != aot_funcs[k].hash) {
            bad[k] = 1;
            changed++;
            if (names.size() < 240)
                names += std::string(names.empty() ? "" : " ") + aot_funcs[k].name;
        }
    for (bool more = changed != 0; more;) {
        more = false;
        for (size_t k = 0; k < n; k++)
            for (uint32_t j = 0; j < aot_funcs[k].ncallees && !bad[k]; j++)
                if (bad[aot_funcs[k].callees[j]]) {
                    bad[k] = 1;
                    more = true;
                }
    }
    return bad;
}
}  // namespace

void set_aot_mode(Cpu &c, AotMode mode)
{
    if (mode == AotMode::Check && (g_fns.empty() || !g_fns[0].fj))
        mode = AotMode::Sig;
    g_mode = mode;
    // Sig and Check hook every function they can (the roots among them); the
    // spike's other modes the roots alone.
    const bool fns = (mode == AotMode::Sig || mode == AotMode::Check) && !g_fns.empty();
    for (Fn &F : g_fns)
        if (!fns)
            unhook_at(c, F.f->va, F.before, F.hooked);
    for (int k = 0; k < g_root_count; k++) {
        if (mode == AotMode::Off || fns)
            unhook(g_roots[k], c);
        else
            hook(g_roots[k], c);
    }
    if (!fns)
        return;
    // Each time it's turned on: the natives may have hooked since, and what
    // they changed (and whatever reaches it) stays with Box86.
    for (Fn &F : g_fns)
        unhook_at(c, F.f->va, F.before, F.hooked);
    uint32_t changed = 0;
    std::string names;
    const std::vector<uint8_t> bad = stale_funcs(c, changed, names);
    uint32_t hooked = 0, left = 0;
    g_dropped = 0;
    for (Fn &F : g_fns) {
        F.probe = 0;
        F.probe_ticks = F.probe_nested_ticks = 0;
        F.probe_nested = 0;
        F.dropped = false;
        if (bad[F.f - aot_funcs]) {
            left++;
            continue;
        }
        hook_at(c, F.f->va, mode == AotMode::Sig && F.trap_in_line ? F.trap_in_line : F.trap, F.before, F.hooked);
        hooked++;
    }
    rt::logf("aot: %u functions hooked; %u changed since compiled (natives%s%s), %u hookable left to Box86 with them",
             (unsigned)hooked, (unsigned)changed, names.empty() ? "" : ": ", names.c_str(), (unsigned)left);
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
    if (g_mode == AotMode::Check && !g_fns.empty()) {
        uint32_t calls = 0, differ = 0, unchecked = 0, fns = 0, calls_all = 0, differ_all = 0, unchecked_all = 0,
                 fns_all = 0;
        for (Fn &F : g_fns) {
            calls += F.checked;
            differ += F.differ;
            unchecked += F.unchecked;
            fns += (F.checked || F.unchecked) ? 1 : 0;
            calls_all += F.checked_all;
            differ_all += F.differ_all;
            unchecked_all += F.unchecked_all;
            fns_all += (F.checked_all || F.unchecked_all) ? 1 : 0;
            F.checked = F.differ = F.unchecked = 0;
        }
        char line[256];
        std::snprintf(line, sizeof line,
                      "aot check: %u calls checked (%u functions), %u differ, %u unchecked (a call to Box86 inside); "
                      "since the start %u calls of %u of %u functions, %u differ, %u unchecked",
                      calls, fns, differ, unchecked, calls_all, fns_all, (unsigned)g_fns.size(), differ_all,
                      unchecked_all);
        out.push_back(line);
    }
    {
        uint32_t traps = 0;
        std::vector<std::pair<uint32_t, const Fn *>> by;
        for (Fn &F : g_fns) {
            if (F.entered)
                by.push_back({F.entered, &F});
            traps += F.entered;
        }
        if (traps) {
            char line[512];
            int p = std::snprintf(line, sizeof line, "aot: %u calls from Box86 into compiled code:", traps);
            std::sort(by.begin(), by.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            for (size_t k = 0; k < by.size() && k < (g_prof ? 12 : 6) && p < (int)sizeof line; k++) {
                p += std::snprintf(line + p, sizeof line - p, " %08x %u", by[k].second->f->va, by[k].first);
                if (g_prof && p < (int)sizeof line)
                    p += std::snprintf(line + p, sizeof line - p, " (%.2f us)",
                                       (double)by[k].second->ticks_in / by[k].first / kTicksPerUs);
            }
            out.push_back(line);
        }
        if (!g_prof_sites.empty()) {
            std::vector<std::pair<uint32_t, uint64_t>> sites;
            for (const auto &kv : g_prof_sites)
                sites.push_back({kv.second, kv.first});
            std::sort(sites.begin(), sites.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            char line[512];
            int p = std::snprintf(line, sizeof line, "aot: calls from Box86 by return address:");
            for (size_t k = 0; k < sites.size() && k < 10 && p < (int)sizeof line; k++)
                p += std::snprintf(line + p, sizeof line - p, " %08x<-%08x %u", (uint32_t)(sites[k].second >> 32),
                                   (uint32_t)sites[k].second, sites[k].first);
            out.push_back(line);
            g_prof_sites.clear();
        }
        // The window's own time, in every mode (the lane's virtual clock
        // keeps the game's work per window the same: its host time compares
        // the modes); with D2_AOT_PROF, the time inside compiled code.
        const uint64_t now = ticks();
        if (g_prof_window_t0 && !g_fns.empty()) {
            char line[160];
            if (g_prof)
                std::snprintf(line, sizeof line, "aot time: %.0f ms this window, %.0f inside compiled code",
                              (double)(now - g_prof_window_t0) / kTicksPerUs / 1000,
                              (double)g_prof_outer / kTicksPerUs / 1000);
            else
                std::snprintf(line, sizeof line, "aot time: %.0f ms this window",
                              (double)(now - g_prof_window_t0) / kTicksPerUs / 1000);
            out.push_back(line);
        }
        g_prof_window_t0 = now;
        g_prof_outer = 0;
        std::vector<std::pair<uint32_t, const Fn *>> direct;
        uint32_t directs = 0;
        for (Fn &F : g_fns) {
            if (F.direct)
                direct.push_back({F.direct, &F}), directs += F.direct;
            F.entered = 0, F.ticks_in = 0, F.direct = 0;
        }
        if (directs) {
            std::sort(direct.begin(), direct.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            char line[320];
            int p = std::snprintf(line, sizeof line, "aot: %u calls from natives run in C, not nested:", directs);
            for (size_t k = 0; k < direct.size() && k < 6 && p < (int)sizeof line; k++)
                p += std::snprintf(line + p, sizeof line - p, " %08x %u", direct[k].second->f->va, direct[k].first);
            out.push_back(line);
        }
    }
    if (g_dropped_window) {
        char line[160];
        std::snprintf(line, sizeof line, "aot: %u functions left to Box86 after their first calls (under %.2f us each), "
                      "%u since turned on", g_dropped_window, g_min_us, g_dropped);
        out.push_back(line);
        g_dropped_window = 0;
    }
    if (g_exits) {
        char line[256];
        int p = std::snprintf(line, sizeof line, "aot: %u exits to Box86 (the rest of a call):", g_exits);
        std::vector<std::pair<uint32_t, uint32_t>> to(g_exits_to.begin(), g_exits_to.end());
        std::sort(to.begin(), to.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        for (size_t k = 0; k < to.size() && k < 6 && p < (int)sizeof line; k++)
            p += std::snprintf(line + p, sizeof line - p, " %08x %u", to[k].first, to[k].second);
        out.push_back(line);
        g_exits = 0;
        g_exits_to.clear();
    }
    if (g_handed) {
        char line[96];
        std::snprintf(line, sizeof line, "aot: %u natives' hand-backs to compiled code", g_handed);
        out.push_back(line);
        g_handed = 0;
    }
    if (g_callout_shims || g_callout_fns || g_callout_back) {
        char line[256];
        int p = std::snprintf(line, sizeof line,
                              "aot: callouts in C: %u to shims, %u to compiled functions; %u back to Box86 at the return:",
                              g_callout_shims, g_callout_fns, g_callout_back);
        std::vector<std::pair<uint32_t, uint32_t>> to(g_callout_back_by.begin(), g_callout_back_by.end());
        std::sort(to.begin(), to.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        for (size_t k = 0; k < to.size() && k < 6 && p < (int)sizeof line; k++)
            p += std::snprintf(line + p, sizeof line - p, " %08x %u", to[k].first, to[k].second);
        out.push_back(line);
        g_callout_shims = g_callout_fns = g_callout_back = 0;
        g_callout_back_by.clear();
    }
    if (g_nested) {
        char line[256];
        int p = std::snprintf(line, sizeof line, "aot: %u nested runs of callees not compiled:", g_nested);
        std::vector<std::pair<uint32_t, uint32_t>> by;
        for (const auto &kv : g_nested_by)
            by.push_back({kv.second, kv.first});
        std::sort(by.rbegin(), by.rend());
        for (size_t k = 0; k < by.size() && k < 6 && p < (int)sizeof line; k++)
            p += std::snprintf(line + p, sizeof line - p, " %08x %u", by[k].second, by[k].first);
        out.push_back(line);
        g_nested = 0;
        g_nested_by.clear();
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
    game::g_nested_by[va]++;
    if (game::g_journaling)
        game::g_impure = true;   // the check (issue #114): Box86's writes aren't journaled
    if (aot_guest_hook) {
        aot_guest_hook(va, r);
        return;
    }
    const uint64_t t0 = game::ticks();
    game::nested_run(*game::g_cpu, va, r->r);
    game::g_nested_ticks += game::ticks() - t0;
    game::g_nested_n++;
}

void aot_exit(uint32_t target, uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx, uint32_t esp, uint32_t ebp,
              uint32_t esi, uint32_t edi)
{
    game::ExitJump *x = game::g_exit_jump;
    if (!x || game::g_no_exit) {   // compiled code entered other than by a trap (AotFunc::may_exit not honoured): a bug of ours
        rt::logf("aot: an exit to %08x with no trap to go back to", target);
        aot_unreachable(target);
    }
    x->target = target;
    const uint32_t r[8] = {eax, ecx, edx, ebx, esp, ebp, esi, edi};
    std::memcpy(x->r, r, sizeof r);
    std::longjmp(x->jb, 1);
}

// Box86 goes on at the call's return with what the callee left: esp, or a
// callee-saved register, not what the compiled caller assumed; or a shim that
// asked to yield (the trap's tail that entered compiled code does it) or to
// run guest code first (rd).
[[noreturn]] static void call_out_back(uint32_t x, uint32_t to, const uint32_t *r)
{
    game::g_callout_back++;
    game::g_callout_back_by[x]++;
    aot_exit(to, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
}

void aot_call_out(uint32_t x, AotRegs *t, uint32_t pops)
{
    using namespace game;
    uint32_t *r = t->r;
    const uint32_t esp0 = r[4], ra = aot_ld32(aot_mem, esp0), want = esp0 + 4u + pops;
    const uint32_t keep[4] = {r[3], r[5], r[6], r[7]};
    if (g_no_exit) {
        // No trap to exit to: a compiled callee in C if it can't exit
        // either, else Box86's call, nested (a shim's wait done in place).
        const auto it = g_callouts ? g_fn_at.find(x) : g_fn_at.end();
        const Fn *F = it == g_fn_at.end() ? nullptr : &g_fns[it->second];
        if (F && (F->hooked || F->dropped) && !F->f->may_hard && g_mode == AotMode::Sig) {
            F->f->sig(r);
            r[4] = esp0 + 4u + F->f->pops;
            g_callout_fns++;
        } else
            aot_guest_call(x, t);
        if (r[4] != want || r[3] != keep[0] || r[5] != keep[1] || r[6] != keep[2] || r[7] != keep[3]) {
            rt::logf("aot: a callout to %08x (return %08x) came back with esp %08x, not %08x, where nothing can "
                     "exit: the compiled caller can't go on", x, ra, r[4], want);
            std::abort();
        }
        return;
    }
    // The check's journaled run too: a compiled callee's journaled code, and
    // a shim's call unchecked (its writes aren't in the journal).
    if ((g_mode == AotMode::Sig || (g_mode == AotMode::Check && g_journaling)) && g_callouts) {
        if (const d2rt::Shim *sh = rt::bridge().slot_shim(x)) {
            if (g_journaling)
                g_impure = true;
            // As the trap would: the CPU's registers as at the call, the
            // shim, then its ret's pops. (The trap's own context restored
            // after: its tail goes on from it.)
            const uint32_t popped = 4u + (sh->stdcall_cleanup ? 4u * sh->argc : 0u);
            // Only the trap's general registers and eip kept: the FPU's
            // state is the shim's to leave (compiled code has no x87, and a
            // float result in st0 is the caller's), flags don't outlive a
            // call (and reading them makes Box86 compute its deferred ones),
            // and the full context's 768-byte blob each way cost more than
            // the call.
            Cpu &c = *g_cpu;
            uint32_t saved[9];
            c.regs_gp(saved);
            saved[8] = c.reg(d2rt::R_EIP);
            for (int k = 0; k < 8; k++)
                c.set_reg(k, r[k]);
            {   // each shim's first callout, named (the lane)
                static const d2rt::Shim *last;
                static std::set<const d2rt::Shim *> seen;
                if (sh != last && seen.insert(last = sh).second)
                    rt::logf("aot: callout to %s from %08x", sh->tag.c_str(), ra);
            }
            const uint32_t eax = sh->fn(c);
            c.regs_gp(r);
            r[0] = eax;
            r[4] += popped;   // the trap's tail: from esp as the shim left it
            const uint32_t rd = rt::bridge().take_redirect();
            const bool yield = rt::bridge().yield_requested();
            for (int k = 0; k < 8; k++)
                c.set_reg(k, saved[k]);
            c.set_reg(d2rt::R_EIP, saved[8]);
            g_callout_shims++;
            if (rd || yield || r[4] != want)
                call_out_back(x, rd ? rd : ra, r);
        } else {
            const auto it = g_fn_at.find(x);
            if (it == g_fn_at.end())
                aot_exit(x, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
            Fn &F = g_fns[it->second];
            const AotFunc *af = g_journaling ? F.fj : F.f;
            if ((!F.hooked && !F.dropped) || !af)   // changed by a native since it was compiled
                aot_exit(x, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
            af->sig(r);   // an exit inside goes back to our own trap, as from a direct call
            r[4] = esp0 + 4u + F.f->pops;
            g_callout_fns++;
            if (r[4] != want)
                call_out_back(x, ra, r);
        }
        if (r[3] != keep[0] || r[5] != keep[1] || r[6] != keep[2] || r[7] != keep[3])
            call_out_back(x, ra, r);
        return;
    }
    aot_exit(x, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
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
