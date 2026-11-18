/* What the AOT spike's generated C (issue #113, tools/aot/recompile.py)
 * calls and includes. The generated file is made from the user's own
 * Game.exe under build/aot/ and is never committed; this header and
 * aot_spike.cpp are ours.
 *
 * The guest's memory model is the natives': guest address va is host
 * aot_mem + va (d2_native.cpp's H()), little-endian, unaligned access
 * through memcpy. Registers are uint32_t locals. Flags are kept lazily: a
 * flag-setting instruction whose flags are read stores its operands and
 * result (F_A, F_B, F_R, and F_C for a carry in or an overflow), and the
 * reader works the condition out with aot_cc() for that instruction's
 * kind and width, constant at compile time, so gcc folds it into a
 * compare.
 *
 * The generated files (aot_spike.c, the tables; aot_spike_NN.c, the
 * functions, issue #135) are each compiled several times (the build's
 * AOT_VARIANT):
 *   default: aot_* symbols;
 *   AOT_JOURNAL: aotj_*, every store journaled first (the check mode);
 *   AOT_NOINLINE: aotn_*, no compiled function inlined into another, so
 *   every call between compiled functions is a real call (to measure what
 *   the signatures save).
 */
#ifndef D2_GAME_AOT_RT_H
#define D2_GAME_AOT_RT_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t r[8];   /* eax ecx edx ebx esp ebp esi edi */
} AotRegs;
typedef uint32_t (*AotRootFn)(uint32_t *r);
typedef struct {
    uint32_t va;
    AotRootFn sig, ctx;   /* with register signatures; all eight through a context */
    const char *name;
    uint8_t keep;         /* the whole instructions (5 bytes or more) a trampoline copies */
    uint8_t pops;         /* the bytes its ret pops after the return address */
    uint8_t head[16];     /* the game's first bytes there */
} AotRoot;
/* The functions' table (issues #114, #115; AOT_SYM(funcs) in each build):
 * every compiled function, entered with all eight registers as a root is.
 * keep 0: its entry can't be hooked (recompile.py's hookable(): a branch in
 * its first bytes or into them, or pops not known). */
typedef struct {
    uint32_t va;
    AotRootFn sig;        /* its register-signature code (journaled in aotj_funcs) */
    const char *name;
    uint8_t keep, pops;
    uint8_t out;          /* bit k: r[k] is an output its compiled callers read */
    uint8_t preserved;    /* bit k: r[k] comes back as it went in */
    uint8_t root;         /* also in aot_roots */
    uint8_t head[16];
    /* Its code as compiled (issue #115): FNV-1a of the bytes of its ranges
     * (lo, hi pairs), and the indexes in this table of the compiled
     * functions it calls in C. A function whose bytes changed since (a
     * native's patch) runs under Box86, with every compiled caller that
     * reaches it in C. */
    uint32_t hash;
    uint16_t nranges, ncallees;
    const uint32_t *ranges;
    const uint16_t *callees;
    /* 1: its C never calls Box86, nor does any compiled callee's (no nested
     * run), so Box86 may enter it in line (the backend's in-line shim). 0
     * under AOT_SPLIT, where every call between compiled functions is
     * Box86's. */
    uint8_t leaf;
    /* 1: its C, or a compiled callee's, may exit (aot_exit, issue #115):
     * hand the rest of the call to Box86 at a call that may block, an
     * import or an indirect call. Its own exits alone under AOT_SPLIT. */
    uint8_t may_exit;
    /* 1: ... other than at a callout (aot_call_out), the rest of the code
     * cut there. One that can't may run where there's no trap to exit to (a
     * native's guest_call), its callouts' fallbacks nested runs there. */
    uint8_t may_hard;
} AotFunc;
#ifdef AOT_SPLIT
#define AOT_LEAF(x) 0
#define AOT_EXIT(own, tree) (own)
#else
#define AOT_LEAF(x) (x)
#define AOT_EXIT(own, tree) (tree)
#endif

extern uint8_t *aot_mem;   /* host address of guest 0 */
/* By the functions' table's rows (issue #115): 1 when the app found that
 * function changed since compiled (a native's hook or patch). A compiled
 * call to it (call_, recompile.py) then calls the native (aot_call_stale,
 * from stale_, out of the way), and its callers stay compiled. */
extern uint8_t aot_stale[];
#define AOT_STALE(k) __builtin_expect(aot_stale[(k)], 0)
#define AOT_COLD __attribute__((cold, noinline))

/* aot_spike.cpp */
uint32_t aot_fs_base(void);                       /* the running thread's TEB */
void aot_guest_call(uint32_t va, AotRegs *r);     /* a callee not compiled: Box86, nested */
void aot_call_stale(uint32_t va, AotRegs *r);     /* a compiled callee a native changed (AOT_LIVE): its native */
__attribute__((noreturn)) void aot_bad_ret(uint32_t fn, uint32_t esp0, uint32_t esp);
__attribute__((noreturn)) void aot_unreachable(uint32_t va);
/* Box86 goes on at `target` with these registers (issue #115): back to the
 * trap that entered compiled code, the C frames above dropped. The guest's
 * stack holds each compiled caller's return address, so Box86 runs the rest
 * of each from the game's own code. */
__attribute__((noreturn)) void aot_exit(uint32_t target, uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx,
                                        uint32_t esp, uint32_t ebp, uint32_t esi, uint32_t edi);
/* An indirect or import call (issue #115): `x` called in C when it's our
 * shim or a compiled function, with *r as at the call (esp at the return
 * address pushed), *r then as after it. Else, or where the callee popped
 * other than `pops` bytes, changed a callee-saved register or asked to yield
 * or to run guest code, an exit: Box86 goes on at x, or at the return. */
void aot_call_out(uint32_t x, AotRegs *r, uint32_t pops);
__attribute__((noreturn)) void aot_fault(uint32_t va);
void aot_journal(uint32_t va, uint32_t n);        /* AOT_JOURNAL: before a store */
/* d2_native.cpp: our natives, called in C. The high word: 1 if the native
 * covered the call (the low word is its eax), 0 if it hands back. */
uint64_t aot_native_463740(uint32_t ecx, uint32_t edx, uint32_t y);
uint64_t aot_native_64d4e0(uint32_t esi, uint32_t room, uint32_t x, uint32_t y);

#if defined(AOT_JOURNAL)
#define AOT_SYM(x) aotj_##x
#define AOT_STORE_HOOK(a, n) aot_journal((a), (n))
#elif defined(AOT_NOINLINE)
#define AOT_SYM(x) aotn_##x
#define AOT_STORE_HOOK(a, n) ((void)0)
#else
#define AOT_SYM(x) aot_##x
#define AOT_STORE_HOOK(a, n) ((void)0)
#endif
/* Not static (issue #135): the functions are split over several files
 * (aot_spike_NN.c), and a call may go from one to another. Hidden, so the
 * three builds' symbols (AOT_SYM) stay inside the binary. */
#ifdef AOT_NOINLINE
#define AOT_FN __attribute__((visibility("hidden"), noinline))
#else
#define AOT_FN __attribute__((visibility("hidden")))
#endif

#define AOT_INL static inline __attribute__((always_inline))

/* AOT_TRACE_ON (debugging the recompiler): each compiled function's entry
 * and registers, to compare with the game's run under Unicorn. */
#ifdef AOT_TRACE_ON
void aot_trace(uint32_t va, uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx, uint32_t esp, uint32_t ebp,
               uint32_t esi, uint32_t edi);
#define AOT_TRACE(...) aot_trace(__VA_ARGS__)
#else
#define AOT_TRACE(...) ((void)0)
#endif

/* M: a function's local copy of aot_mem, so stores (char-typed, which may
 * alias anything) don't make gcc reload it. */
AOT_INL uint32_t aot_ld8(uint8_t *m, uint32_t a) { return m[a]; }
AOT_INL uint32_t aot_ld16(uint8_t *m, uint32_t a)
{
    uint16_t v;
    memcpy(&v, m + a, 2);
    return v;
}
AOT_INL uint32_t aot_ld32(uint8_t *m, uint32_t a)
{
    uint32_t v;
    memcpy(&v, m + a, 4);
    return v;
}
AOT_INL void aot_st8(uint8_t *m, uint32_t a, uint32_t v)
{
    AOT_STORE_HOOK(a, 1);
    m[a] = (uint8_t)v;
}
AOT_INL void aot_st16(uint8_t *m, uint32_t a, uint32_t v)
{
    uint16_t x = (uint16_t)v;
    AOT_STORE_HOOK(a, 2);
    memcpy(m + a, &x, 2);
}
AOT_INL void aot_st32(uint8_t *m, uint32_t a, uint32_t v)
{
    AOT_STORE_HOOK(a, 4);
    memcpy(m + a, &v, 4);
}
#define LD8(a) aot_ld8(M, (a))
#define LD16(a) aot_ld16(M, (a))
#define LD32(a) aot_ld32(M, (a))
#define ST8(a, v) aot_st8(M, (a), (v))
#define ST16(a, v) aot_st16(M, (a), (v))
#define ST32(a, v) aot_st32(M, (a), (v))

/* The C runtime's memset and memcpy (MSVC's memcpy handles overlap as
 * memmove does), as the memory intrinsics do them. */
AOT_INL void aot_memset(uint32_t d, uint32_t v, uint32_t n)
{
    AOT_STORE_HOOK(d, n);
    memset(aot_mem + d, (int)(v & 0xff), n);
}
AOT_INL void aot_memmove(uint32_t d, uint32_t s, uint32_t n)
{
    AOT_STORE_HOOK(d, n);
    memmove(aot_mem + d, aot_mem + s, n);
}
/* rep movs / rep stos, element by element where the copy overlaps forward
 * (as the CPU does it), with the direction flag. */
AOT_INL void aot_movs(uint32_t di, uint32_t si, uint32_t n, int size, int down)
{
    const uint32_t bytes = n * (uint32_t)size;
    if (!down && (di <= si || di >= si + bytes)) {
        AOT_STORE_HOOK(di, bytes);
        memmove(aot_mem + di, aot_mem + si, bytes);
        return;
    }
    const int32_t step = down ? -size : size;
    for (uint32_t k = 0; k < n; k++, di += (uint32_t)step, si += (uint32_t)step) {
        AOT_STORE_HOOK(di, (uint32_t)size);
        memmove(aot_mem + di, aot_mem + si, (size_t)size);
    }
}
AOT_INL void aot_stos(uint32_t di, uint32_t v, uint32_t n, int size, int down)
{
    if (size == 1) {
        const uint32_t lo = down ? di - n + 1 : di;
        AOT_STORE_HOOK(lo, n);
        memset(aot_mem + lo, (int)(v & 0xff), n);
        return;
    }
    const int32_t step = down ? -size : size;
    for (uint32_t k = 0; k < n; k++, di += (uint32_t)step) {
        AOT_STORE_HOOK(di, (uint32_t)size);
        memcpy(aot_mem + di, &v, (size_t)size);
    }
}

/* Flag kinds (recompile.py's KIND) and condition codes (CC_NUM). */
enum { AOT_ADD, AOT_SUB, AOT_LOGIC, AOT_INC, AOT_DEC, AOT_NEG, AOT_SHL, AOT_SHR, AOT_SAR, AOT_MUL, AOT_ADC, AOT_SBB };
enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G };

AOT_INL uint32_t aot_mask(int w) { return w == 4 ? 0xffffffffu : (1u << (8 * w)) - 1; }
AOT_INL uint32_t aot_sbit(int w) { return 1u << (8 * w - 1); }
AOT_INL int32_t aot_sx(uint32_t v, int w) { return w == 4 ? (int32_t)v : w == 2 ? (int16_t)v : (int8_t)v; }

AOT_INL int aot_cf(int k, int w, uint32_t a, uint32_t b, uint32_t r, uint32_t c)
{
    switch (k) {
    case AOT_ADD: return r < a;
    case AOT_SUB: return a < b;
    case AOT_NEG: return a != 0;
    case AOT_SHL: return (int)((a >> (8 * w - b)) & 1);
    case AOT_SHR: return (int)((a >> (b - 1)) & 1);
    case AOT_SAR: return (int)(((uint32_t)(aot_sx(a, w) >> (b - 1))) & 1);
    case AOT_MUL: return c != 0;
    case AOT_ADC: return (uint64_t)a + b + c > aot_mask(w);
    case AOT_SBB: return (uint64_t)a < (uint64_t)b + c;
    default: return 0;   /* logic; inc/dec leave CF (never asked: recompile.py refuses) */
    }
}
AOT_INL int aot_of(int k, int w, uint32_t a, uint32_t b, uint32_t r, uint32_t c)
{
    const uint32_t s = aot_sbit(w);
    switch (k) {
    case AOT_ADD:
    case AOT_ADC: return (~(a ^ b) & (a ^ r) & s) != 0;
    case AOT_SUB:
    case AOT_SBB: return ((a ^ b) & (a ^ r) & s) != 0;
    case AOT_INC: return r == s;
    case AOT_DEC: return a == s;
    case AOT_NEG: return a == s;
    case AOT_SHL: return b == 1 ? (((r ^ (aot_cf(k, w, a, b, r, c) ? s : 0)) & s) != 0) : 0;
    case AOT_SHR: return b == 1 ? ((a & s) != 0) : 0;
    case AOT_MUL: return c != 0;
    default: return 0;
    }
}
AOT_INL int aot_cc(int cc, int k, int w, uint32_t a, uint32_t b, uint32_t r, uint32_t c)
{
    const uint32_t m = aot_mask(w);
    if (k == AOT_SUB) {   /* cmp/sub: compares, which gcc emits as one */
        switch (cc) {
        case CC_B: return a < b;
        case CC_AE: return a >= b;
        case CC_E: return a == b;
        case CC_NE: return a != b;
        case CC_BE: return a <= b;
        case CC_A: return a > b;
        case CC_L: return aot_sx(a, w) < aot_sx(b, w);
        case CC_GE: return aot_sx(a, w) >= aot_sx(b, w);
        case CC_LE: return aot_sx(a, w) <= aot_sx(b, w);
        case CC_G: return aot_sx(a, w) > aot_sx(b, w);
        default: break;
        }
    }
    if (k == AOT_LOGIC) {   /* and/or/xor/test: CF = OF = 0 */
        switch (cc) {
        case CC_E:
        case CC_BE: return (r & m) == 0;
        case CC_NE:
        case CC_A: return (r & m) != 0;
        case CC_S:
        case CC_L: return aot_sx(r, w) < 0;
        case CC_NS:
        case CC_GE: return aot_sx(r, w) >= 0;
        case CC_LE: return aot_sx(r, w) <= 0;
        case CC_G: return aot_sx(r, w) > 0;
        case CC_B:
        case CC_O: return 0;
        case CC_AE:
        case CC_NO: return 1;
        default: break;
        }
    }
    const int cf = aot_cf(k, w, a, b, r, c), zf = (r & m) == 0, sf = (r & aot_sbit(w)) != 0;
    const int of = aot_of(k, w, a, b, r, c), pf = !__builtin_parity(r & 0xff);
    switch (cc) {
    case CC_O: return of;
    case CC_NO: return !of;
    case CC_B: return cf;
    case CC_AE: return !cf;
    case CC_E: return zf;
    case CC_NE: return !zf;
    case CC_BE: return cf || zf;
    case CC_A: return !cf && !zf;
    case CC_S: return sf;
    case CC_NS: return !sf;
    case CC_P: return pf;
    case CC_NP: return !pf;
    case CC_L: return sf != of;
    case CC_GE: return sf == of;
    case CC_LE: return zf || sf != of;
    default: return !zf && sf == of;
    }
}
/* Several kinds reach the reader: F_K holds kind * 8 + width. */
static inline int aot_cc_dyn(int cc, int kw, uint32_t a, uint32_t b, uint32_t r, uint32_t c)
{
    return aot_cc(cc, kw >> 3, kw & 7, a, b, r, c);
}

#ifdef __cplusplus
}
#endif

#endif
