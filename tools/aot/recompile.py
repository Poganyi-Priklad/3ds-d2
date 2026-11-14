#!/usr/bin/env python3
"""The AOT spike's recompiler (issue #113, docs/aot.md): x86 functions of the
user's Game.exe written out as C, for devkitARM's gcc (or the host test's).

    python3 -I tools/aot/recompile.py Game.exe --out build/aot/spike \
        --root 67b850:PathSearch --compile 67aed0,67af40,... [--functions game-functions.tsv]
    python3 -I tools/aot/recompile.py Game.exe --survey --functions game-functions.tsv

Each compiled function becomes one C function, twice:
  - aot_sig_<va>: with a register signature from a liveness pass across
    functions (the registers it reads, the ones it preserves, and the ones
    its callers read after it), passed as C arguments and results;
  - aot_ctx_<va>: the safe fallback, all eight registers through a context
    struct at every call.
A callee that isn't compiled runs nested under Box86 (aot_guest_call); a
native of ours is called in C first (aot_native_call) and hands back to the
compiled function (or Box86) when it doesn't cover the call.

The output is the user's Game.exe in another form: it goes under build/ and
into the user's own build only, never into the repository or a release.
"""
import argparse
import collections
import json
import os
import sys
import time

import capstone
from capstone import x86_const as X
import pefile

REGS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI = range(8)
ALL = frozenset(range(8))
# name -> (full register, bits, shift)
SUBREG = {}
for i, n in enumerate(REGS):
    SUBREG["e" + n[1:]] = (i, 32, 0)
    SUBREG[n[1:]] = (i, 16, 0)
for i, n in enumerate(["al", "cl", "dl", "bl"]):
    SUBREG[n] = (i, 8, 0)
    SUBREG[n[0] + "h"] = (i, 8, 8)

# Flags: carry, parity, adjust, zero, sign, overflow.
FC, FP, FA, FZ, FS, FO = (1 << k for k in range(6))
FALL = 0x3f
EFL = [  # capstone eflags bits -> ours, (modify-ish, test)
    (FC, X.X86_EFLAGS_MODIFY_CF | X.X86_EFLAGS_RESET_CF | X.X86_EFLAGS_SET_CF | X.X86_EFLAGS_UNDEFINED_CF,
     X.X86_EFLAGS_TEST_CF),
    (FP, X.X86_EFLAGS_MODIFY_PF | X.X86_EFLAGS_RESET_PF | X.X86_EFLAGS_SET_PF | X.X86_EFLAGS_UNDEFINED_PF,
     X.X86_EFLAGS_TEST_PF),
    (FA, X.X86_EFLAGS_MODIFY_AF | X.X86_EFLAGS_RESET_AF | X.X86_EFLAGS_SET_AF | X.X86_EFLAGS_UNDEFINED_AF,
     X.X86_EFLAGS_TEST_AF),
    (FZ, X.X86_EFLAGS_MODIFY_ZF | X.X86_EFLAGS_RESET_ZF | X.X86_EFLAGS_SET_ZF | X.X86_EFLAGS_UNDEFINED_ZF,
     X.X86_EFLAGS_TEST_ZF),
    (FS, X.X86_EFLAGS_MODIFY_SF | X.X86_EFLAGS_RESET_SF | X.X86_EFLAGS_SET_SF | X.X86_EFLAGS_UNDEFINED_SF,
     X.X86_EFLAGS_TEST_SF),
    (FO, X.X86_EFLAGS_MODIFY_OF | X.X86_EFLAGS_RESET_OF | X.X86_EFLAGS_SET_OF | X.X86_EFLAGS_UNDEFINED_OF,
     X.X86_EFLAGS_TEST_OF),
]
CC_FLAGS = {"o": FO, "no": FO, "b": FC, "ae": FC, "e": FZ, "ne": FZ, "be": FC | FZ, "a": FC | FZ,
            "s": FS, "ns": FS, "p": FP, "np": FP, "l": FS | FO, "ge": FS | FO, "le": FZ | FS | FO,
            "g": FZ | FS | FO}
CC_ALIAS = {"c": "b", "nae": "b", "nc": "ae", "nb": "ae", "z": "e", "nz": "ne", "na": "be", "nbe": "a",
            "pe": "p", "po": "np", "nge": "l", "nl": "ge", "ng": "le", "nle": "g"}
CC_NUM = {c: k for k, c in enumerate(["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np",
                                       "l", "ge", "le", "g"])}
# Flag kinds, as aot_rt.h numbers them. INC and DEC leave CF alone.
KIND = {"add": 0, "sub": 1, "cmp": 1, "and": 2, "or": 2, "xor": 2, "test": 2, "inc": 3, "dec": 4,
        "neg": 5, "shl": 6, "sal": 6, "shr": 7, "sar": 8, "imul": 9, "mul": 9, "adc": 10, "sbb": 11}
KIND_DEFS = {3: FALL & ~FC, 4: FALL & ~FC}
X87_PREFIX = "f"


# The C runtime's exit(code), 681e09: doexit(code, 0, 0) at 681d27, whose
# retcaller = 0 ends in ExitProcess. Its ret only serves other arguments.
NORETURN_FUNCS = {0x00681e09}
NORETURN_IMPORTS = {"ExitProcess", "ExitThread", "FatalAppExitA", "FatalExit", "TerminateThread"}


class Refused(Exception):
    """A function the spike leaves to Box86, and why."""


def cc_of(mnemonic, prefix):
    c = mnemonic[len(prefix):]
    return CC_ALIAS.get(c, c)


def import_pops(imports):
    """What each import's ret pops: from our runtime's shims, which declare
    their argument counts (src/runtime/rt_shims_*.cpp, WinVita's
    src/runtime), and from decorated names (_BinkOpen@8). wsprintfA is
    cdecl. None where neither says."""
    import glob
    import re
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
    pat = re.compile(r'\b[A-Z][A-Za-z0-9_]*\(\s*"([A-Za-z0-9_]+)",\s*(\d+)\s*,')
    argc = {}
    for f in sorted(glob.glob(os.path.join(root, "src/runtime/rt_shims_*.cpp")) +
                    glob.glob(os.path.join(root, "third_party/winx86/src/runtime/*.cpp"))):
        with open(f, errors="replace") as fh:
            for m in pat.finditer(fh.read()):
                argc.setdefault(m.group(1), int(m.group(2)))
    out = {}
    for slot, (dll, name) in imports.items():
        if name in ("wsprintfA", "wsprintfW"):
            out[slot] = 0
        elif "@" in name and name.rsplit("@", 1)[1].isdigit():
            out[slot] = int(name.rsplit("@", 1)[1])
        elif name in argc:
            out[slot] = 4 * argc[name]
        elif name == "timeGetTime":
            out[slot] = 0
        else:
            out[slot] = None
    return out


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
                                                    pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.mem = self.pe.get_memory_mapped_image()
        text = next(s for s in self.pe.sections if s.Name.rstrip(b"\0") == b".text")
        self.text_lo = self.base + text.VirtualAddress
        self.text_hi = self.text_lo + text.Misc_VirtualSize
        self.imports = {}
        for d in getattr(self.pe, "DIRECTORY_ENTRY_IMPORT", []):
            for imp in d.imports:
                self.imports[imp.address] = (d.dll.decode(), (imp.name or b"#%d" % imp.ordinal).decode())
        self.import_pops = import_pops(self.imports)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.cache = {}

    def u32(self, va):
        o = va - self.base
        return int.from_bytes(self.mem[o:o + 4], "little")

    def u8(self, va):
        return self.mem[va - self.base]

    def in_text(self, va):
        return self.text_lo <= va < self.text_hi

    def insn(self, va):
        i = self.cache.get(va)
        if i is None:
            if not self.in_text(va):
                raise Refused("code outside .text at %08x" % va)
            o = va - self.base
            i = next(self.md.disasm(self.mem[o:o + 16], va), None)
            if i is None:
                raise Refused("no instruction decodes at %08x" % va)
            self.cache[va] = i
        return i


def reg_full(insn, rid):
    n = insn.reg_name(rid)
    s = SUBREG.get(n)
    return s


def regs_rw(insn):
    """The full registers an instruction reads and writes (a partial write
    reads the rest of its register)."""
    rd, wr = insn.regs_access()
    reads, writes = set(), set()
    for r in rd:
        s = reg_full(insn, r)
        if s:
            reads.add(s[0])
    for r in wr:
        s = reg_full(insn, r)
        if s:
            writes.add(s[0])
            if s[1] < 32:
                reads.add(s[0])
    m = insn.mnemonic
    ops = insn.operands
    if m in ("xor", "sub", "sbb") and len(ops) == 2 and ops[0].type == X.X86_OP_REG and \
            ops[1].type == X.X86_OP_REG and ops[0].reg == ops[1].reg and m != "sbb":
        s = reg_full(insn, ops[0].reg)
        if s and s[1] == 32:
            reads.discard(s[0])
    return reads, writes


def P(regs):
    """Liveness works on half registers: 2r the low 16 bits, 2r+1 the high."""
    out = set()
    for r in regs:
        out |= {2 * r, 2 * r + 1}
    return out


def R_of(parts):
    return {q // 2 for q in parts}


def parts_rw(insn):
    """The half registers an instruction reads and writes: a 16-bit write
    leaves the high half alone, an 8-bit write reads the rest of the low."""
    rd, wr = insn.regs_access()
    reads, writes = set(), set()
    for r in rd:
        s = reg_full(insn, r)
        if s:
            reads |= P([s[0]]) if s[1] == 32 else {2 * s[0]}
    for r in wr:
        s = reg_full(insn, r)
        if s:
            if s[1] == 32:
                writes |= P([s[0]])
            else:
                writes.add(2 * s[0])
                if s[1] == 8:
                    reads.add(2 * s[0])
    m = insn.mnemonic
    ops = insn.operands
    if m in ("xor", "sub") and len(ops) == 2 and ops[0].type == X.X86_OP_REG and \
            ops[1].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
        s = reg_full(insn, ops[0].reg)
        if s and s[1] == 32:
            reads -= P([s[0]])
    return reads, writes


# The flags an instruction writes, by mnemonic, besides what Capstone says:
# Capstone 5 leaves them out for `test` with a memory operand (opcodes 84 and
# 85: 547 of the 164,075 flag-setting instructions in d2d's functions), which
# made a jcc after one read an older instruction's flags.
FLAG_DEFS = {m: FALL for m in ("add", "sub", "cmp", "and", "or", "xor", "test", "neg", "adc", "sbb", "imul",
                               "mul", "shl", "sal", "shr", "sar")}
FLAG_DEFS.update(inc=FALL & ~FC, dec=FALL & ~FC)


def flags_rw(insn):
    e = insn.eflags
    d = u = 0
    for ours, mod, test in EFL:
        if e & mod:
            d |= ours
        if e & test:
            u |= ours
    m = insn.mnemonic
    d |= FLAG_DEFS.get(m, 0)
    if m.startswith("j") and m not in ("jmp", "jecxz", "jcxz"):
        u = CC_FLAGS[cc_of(m, "j")]
    elif m.startswith("set"):
        u = CC_FLAGS[cc_of(m, "set")]
    elif m.startswith("cmov"):
        u = CC_FLAGS[cc_of(m, "cmov")]
    return d, u


class Func:
    """One function's code: its instructions and basic blocks, from its entry."""

    def __init__(self, img, entry, starts):
        self.img = img
        self.entry = entry
        self.insns = {}          # va -> insn
        self.succ = {}           # va -> [va] (inside the function)
        self.calls = {}          # va -> target (direct), or None (indirect)
        self.tails = {}          # va -> target: a jmp to another function's entry
        self.switches = {}       # va -> (index reg, [targets], (byte table va or None))
        self.rets = {}           # va -> bytes popped
        self.noreturn = set()    # call sites not coming back
        self.data = set()        # table bytes
        self.refused = None
        self.stats = collections.Counter()
        try:
            self._walk(starts)
        except Refused as e:
            self.refused = str(e)

    def _walk(self, starts):
        img = self.img
        work = [self.entry]
        while work:
            va = work.pop()
            while va not in self.insns:
                i = img.insn(va)
                self.insns[va] = i
                m = i.mnemonic
                nxt = va + i.size
                ops = i.operands
                if m == "ret":
                    self.rets[va] = ops[0].imm if ops else 0
                    self.succ[va] = []
                    break
                if m in ("int3", "hlt", "ud2"):
                    self.succ[va] = []
                    break
                if m == "jmp":
                    if ops[0].type == X.X86_OP_IMM:
                        t = ops[0].imm
                        if t != self.entry and t in starts:
                            self.tails[va] = t
                            self.succ[va] = []
                        else:
                            self.succ[va] = [t]
                            work.append(t)
                        break
                    sw = self._switch(va, i)
                    self.switches[va] = sw
                    self.succ[va] = sorted(set(sw[1] + [sw[3]]))
                    work.extend(self.succ[va])
                    break
                if m.startswith("j") or m.startswith("loop"):
                    if ops[0].type != X.X86_OP_IMM:
                        raise Refused("indirect conditional jump at %08x" % va)
                    t = ops[0].imm
                    self.succ[va] = [t, nxt]
                    work.append(t)
                    va = nxt
                    continue
                if m == "call":
                    if ops[0].type == X.X86_OP_IMM:
                        self.calls[va] = ops[0].imm
                    elif ops[0].type == X.X86_OP_MEM and ops[0].mem.base == 0 and ops[0].mem.index == 0 and \
                            ops[0].mem.disp in img.imports:
                        self.calls[va] = ("import", ops[0].mem.disp)
                    else:
                        self.calls[va] = None
                    # A call that never comes back: padding or another function follows.
                    t = self.calls[va]
                    if isinstance(t, tuple) and img.imports[t[1]][1] in NORETURN_IMPORTS:
                        self.noreturn.add(va)
                        self.succ[va] = []
                        break
                    try:
                        n = img.insn(nxt)
                        dead = n.mnemonic in ("int3",) or (nxt in starts and nxt != self.entry)
                    except Refused:
                        dead = True
                    if dead:
                        self.noreturn.add(va)
                        self.succ[va] = []
                        break
                    self.succ[va] = [nxt]
                    va = nxt
                    continue
                self.succ[va] = [nxt]
                va = nxt
        for va in self.insns:
            for t in self.succ[va]:
                if t not in self.insns:
                    raise Refused("branch into the middle of an instruction at %08x" % t)
        for d in self.data:
            if d in self.insns:
                raise Refused("a switch table overlaps code at %08x" % d)
        self._blocks()

    def prune(self, noreturn_funcs):
        """Calls to functions that never return end their path; code only
        they reached goes."""
        hit = [va for va, t in self.calls.items() if t in noreturn_funcs and va not in self.noreturn]
        if not hit:
            return False
        for va in hit:
            self.noreturn.add(va)
            self.succ[va] = []
        seen, work = set(), [self.entry]
        while work:
            va = work.pop()
            if va in seen:
                continue
            seen.add(va)
            work.extend(self.succ[va])
        for d in (self.insns, self.succ, self.calls, self.rets, self.switches, self.tails):
            for va in [k for k in d if k not in seen]:
                del d[va]
        self.noreturn &= seen
        self._blocks()
        return True

    def _blocks(self):
        # Blocks: leaders are the entry, branch targets and what follows a branch.
        leaders = {self.entry}
        for va, s in self.succ.items():
            i = self.insns[va]
            if len(s) != 1 or s[0] != va + i.size:
                leaders.update(s)
                if va + i.size in self.insns:
                    leaders.add(va + i.size)
        self.leaders = leaders
        self.order = sorted(self.insns)
        self.stats["insns"] = len(self.insns)
        self.stats["bytes"] = sum(i.size for i in self.insns.values())

    def _switch(self, va, i):
        """jmp [reg*4 + table], bounded by the cmp/ja that guards the index;
        or through a byte index table first (movzx reg, byte [reg + bytes])."""
        img = self.img
        op = i.operands[0]
        if op.type != X.X86_OP_MEM or op.mem.base != 0 or op.mem.scale != 4 or op.mem.index == 0:
            raise Refused("indirect jump at %08x" % va)
        idx = reg_full(i, op.mem.index)[0]
        table = op.mem.disp & 0xffffffff
        # Walk back over the instructions just before, in address order.
        back = sorted(a for a in self.insns if a < va)
        bytes_table = None
        bound = None
        default = None
        cur = idx
        k = len(back) - 1
        while k >= 0 and bound is None:
            a = back[k]
            p = self.insns[a]
            if a + p.size != (back[k + 1] if k + 1 < len(back) else va):
                break
            m = p.mnemonic
            ops = p.operands
            if m == "movzx" and ops[0].type == X.X86_OP_REG and reg_full(p, ops[0].reg)[0] == cur and \
                    ops[1].type == X.X86_OP_MEM and ops[1].size == 1 and ops[1].mem.index == 0 and \
                    ops[1].mem.base != 0:
                bytes_table = ops[1].mem.disp & 0xffffffff
                cur = reg_full(p, ops[1].mem.base)[0]
            elif m == "ja" and default is None:
                default = ops[0].imm
            elif m == "cmp" and default is not None and ops[0].type == X.X86_OP_REG and \
                    reg_full(p, ops[0].reg)[0] == cur and ops[1].type == X.X86_OP_IMM:
                bound = ops[1].imm
            else:
                _, w = regs_rw(p)
                if cur in w or (bytes_table is None and idx in w):
                    break
            k -= 1
        if bound is None or default is None or bound > 512:
            raise Refused("a switch at %08x the guard rule doesn't bound" % va)
        if bytes_table is not None:
            n = 1 + max(img.u8(bytes_table + j) for j in range(bound + 1))
            self.data.update(range(bytes_table, bytes_table + bound + 1))
        else:
            n = bound + 1
        targets = [img.u32(table + 4 * j) for j in range(n)]
        self.data.update(range(table, table + 4 * n))
        for t in targets:
            if not img.in_text(t):
                raise Refused("a switch target outside .text at %08x" % va)
        return (idx, targets, bytes_table, default, bound)


# ---- analysis ------------------------------------------------------------

class Summary:
    """What a caller needs to know of a function: the registers it reads
    before writing them, the ones it preserves, the bytes its ret pops."""

    def __init__(self, reads=ALL, preserved=frozenset(), pops=None, known=False):
        self.reads, self.preserved, self.pops, self.known = frozenset(reads), frozenset(preserved), pops, known

    def __eq__(self, o):
        return (self.reads, self.preserved, self.pops, self.known) == (o.reads, o.preserved, o.pops, o.known)


UNKNOWN = Summary()


class Program:
    def __init__(self, img, starts, natives, intrinsics):
        self.img = img
        self.starts = starts
        self.natives = natives          # va -> dict (name, args: [regs], stack, glue)
        self.intrinsics = intrinsics    # va -> name
        self.funcs = {}
        self.summ = {}
        self.compiled = set()   # what compile_set compiles: their summaries are proven, not assumed

    def func(self, va):
        f = self.funcs.get(va)
        if f is None:
            f = self.funcs[va] = Func(self.img, va, self.starts)
        return f

    def closure(self, roots, limit=4000):
        """Every function the roots reach by direct calls (for the summaries)."""
        seen, work = set(), list(roots)
        while work and len(seen) < limit:
            va = work.pop()
            if va in seen or va in self.intrinsics:
                continue
            seen.add(va)
            f = self.func(va)
            if f.refused:
                continue
            for t in list(f.calls.values()) + list(f.tails.values()):
                if isinstance(t, int):
                    work.append(t)
        return seen

    def callee_summary(self, t):
        if t is None:
            # An indirect call: the callee's address is taken, so it keeps one of
            # MSVC's standard conventions (link-time code generation only changes
            # those of functions nobody takes the address of). It reads at most
            # ecx and edx (thiscall, fastcall) and the stack, and keeps the
            # callee-saved registers.
            return Summary(reads=(ECX, EDX, ESP), preserved=(EBX, EBP, ESI, EDI), pops=None, known=True)
        if isinstance(t, tuple):              # an import: our shim, stdcall per MSVC; reads no register
            return Summary(reads=(ESP,), preserved=(EBX, EBP, ESI, EDI), pops=self.img.import_pops.get(t[1]),
                           known=True)
        n = self.natives.get(t)
        if n and not n["handback"]:   # our native, called in C: only eax changes
            return Summary(reads=set(n["reads"]) | {ESP}, preserved=ALL - {EAX}, pops=4 * n["stack"], known=True)
        if t in self.intrinsics:
            name = self.intrinsics[t]
            if name == "alloca_probe":
                return Summary(reads=(EAX, ECX, ESP), preserved=(ECX, EDX, EBX, EBP, ESI, EDI), pops=0, known=True)
            return Summary(reads=(ESP,), preserved=(EBX, EBP, ESI, EDI), pops=0, known=True)
        s = self.summ.get(t, UNKNOWN)
        if t not in self.compiled:
            # Box86 runs it: MSVC's ABI keeps ebx, ebp, esi and edi across any
            # call (link-time code generation changes where arguments go, not
            # these). Proven for compiled functions instead; the lane's check
            # compares them with Box86's after every checked call.
            s = Summary(s.reads, set(s.preserved) | {EBX, EBP, ESI, EDI}, s.pops, s.known)
        return s

    def mark_noreturn(self, vas):
        """Functions with no ret left once their calls to such functions end
        their paths (the game's fatal-error and exit paths)."""
        nr = set(NORETURN_FUNCS)
        changed = True
        while changed:
            changed = False
            for va in vas:
                f = self.func(va)
                if f.refused or va in nr:
                    continue
                if f.prune(nr):
                    changed = True
                if not f.rets and all(t in nr for t in f.tails.values()):
                    nr.add(va)
                    changed = True
        self.noreturn_funcs = nr
        return nr

    def summarize(self, vas, rounds=50):
        """Summaries to a fixed point: reads from nothing up, preserved from
        everything down."""
        self.mark_noreturn(vas)
        self.why = {}
        self.cfg_pops = {}
        for va in vas:
            f = self.func(va)
            pops = set(f.rets.values()) if not f.refused else set()
            self.cfg_pops[va] = pops.pop() if len(pops) == 1 else None
            self.summ[va] = Summary(reads=(), preserved=ALL, pops=self.cfg_pops[va],
                                    known=not f.refused and len(pops) <= 1)
        for _ in range(rounds):
            changed = False
            for va in vas:
                f = self.func(va)
                if f.refused or not self.summ[va].known:
                    s = UNKNOWN
                else:
                    try:
                        a = Analysis(self, f, save_rule=True)
                        s = Summary(a.live_in, a.preserved, self.cfg_pops[va], True)
                        self.why.pop(va, None)
                    except Refused as e:
                        self.why[va] = str(e)
                        s = Summary(ALL, (), self.cfg_pops[va], True) if va in self.noreturn_funcs else UNKNOWN
                if s != self.summ[va]:
                    self.summ[va] = s
                    changed = True
            if not changed:
                return
        raise RuntimeError("summaries didn't settle")


class Analysis:
    """One function's stack offsets, preserved registers, register liveness
    and flags. Raises Refused for what the spike doesn't take."""

    def __init__(self, prog, f, outs=None, save_rule=False):
        self.p, self.f = prog, f
        self.img = prog.img
        self._stack()
        self._liveness(outs, save_rule)

    # Abstract values: ("in", r) a register's value at entry, ("sp", k) the
    # entry esp + k, ("k", c) a constant, None unknown.
    def _stack(self):
        f = self.f
        p = self.p
        states = {f.entry: ({r: ("in", r) for r in range(8)} | {ESP: ("sp", 0)}, {})}
        work = [f.entry]
        self.sp_at = {}
        self.const_eax_at = {}
        self.save_push = {}     # va -> register saved at entry value, in the prologue
        self.save_slot = {}     # va -> its slot (entry esp + k)
        self._prologue = True
        ret_states = []
        visited = collections.Counter()
        while work:
            b = work.pop()
            regs, slots = states[b]
            regs, slots = dict(regs), dict(slots)
            va = b
            while True:
                i = f.insns[va]
                visited[va] += 1
                if visited[va] > 200:
                    raise Refused("stack analysis doesn't settle at %08x" % va)
                sp = regs[ESP]
                self.sp_at[va] = sp[1] if sp and sp[0] == "sp" else None
                if va in f.calls or b != f.entry:
                    self._prologue = False
                self._step(i, regs, slots)
                if va in f.rets:
                    ret_states.append((va, dict(regs)))
                succ = f.succ[va]
                nxt = va + i.size
                if len(succ) == 1 and succ[0] == nxt and nxt not in f.leaders:
                    va = nxt
                    continue
                for t in succ:
                    if t not in states:
                        states[t] = (dict(regs), dict(slots))
                        work.append(t)
                    else:
                        r0, s0 = states[t]
                        r1 = {k: (v if r0[k] == v else None) for k, v in regs.items()}
                        # esp: where one path knows it and another doesn't (a call
                        # whose ret isn't known statically), trust the known one, as
                        # MSVC's balanced stack allows; compiled code checks esp at
                        # each ret at run time.
                        if r0[ESP] is None or regs[ESP] is None:
                            r1[ESP] = r0[ESP] or regs[ESP]
                        s1 = {k: v for k, v in slots.items() if s0.get(k) == v}
                        if r1 != r0 or s1 != s0:
                            if r1[ESP] is None:
                                s1 = {}
                            states[t] = (r1, s1)
                            work.append(t)
                break
        pre = set(range(8))
        # esp_ok: every ret pops what it says from the entry's esp, so a caller
        # knows esp after the call without asking (what a compiled callee needs).
        self.esp_ok = all(regs[ESP] == ("sp", 0) for _, regs in ret_states)
        for va, regs in ret_states:
            for r in range(8):
                if r != ESP and regs[r] != ("in", r):
                    pre.discard(r)
        if not ret_states:
            pre = set(range(8))   # never returns
        self.preserved = frozenset(pre)

    def _val(self, i, op, regs, slots):
        if op.type == X.X86_OP_IMM:
            return ("k", op.imm & 0xffffffff)
        if op.type == X.X86_OP_REG:
            s = reg_full(i, op.reg)
            if s and s[1] == 32:
                return regs[s[0]]
            return None
        if op.type == X.X86_OP_MEM and op.size == 4:
            a = self._addr(i, op, regs)
            if a is not None:
                return slots.get(a)
        return None

    def _addr(self, i, op, regs):
        m = op.mem
        if m.segment != 0 or m.index != 0 or m.base == 0:
            return None
        s = reg_full(i, m.base)
        if not s:
            return None
        v = regs[s[0]]
        if v and v[0] == "sp":
            return v[1] + m.disp
        return None

    def _step(self, i, regs, slots):
        m = i.mnemonic
        ops = i.operands
        p = self.p
        sp = regs[ESP][1] if regs[ESP] and regs[ESP][0] == "sp" else None
        if sp is None:
            return self._step_nosp(i, regs, slots)
        if m == "push":
            v = self._val(i, ops[0], regs, slots)
            r = reg_full(i, ops[0].reg)[0] if ops[0].type == X.X86_OP_REG else None
            # The prologue's saves: the entry block's leading pushes of
            # callee-saved registers still holding their entry values (an
            # argument push or a call ends the run: a push of esi just before
            # a call may be its argument).
            if self._prologue and r in (EBX, EBP, ESI, EDI) and v == ("in", r):
                self.save_push[i.address] = r
                self.save_slot[i.address] = sp - 4
            else:
                self._prologue = False
            slots[sp - 4] = v
            regs[ESP] = ("sp", sp - 4)
            return
        if m == "pop":
            v = slots.get(sp)
            regs[ESP] = ("sp", sp + 4)
            if ops[0].type == X.X86_OP_REG:
                regs[reg_full(i, ops[0].reg)[0]] = v
            else:
                self._kill_mem(i, ops[0], regs, slots)
            return
        if m == "leave":
            v = regs[EBP]
            if not v or v[0] != "sp":
                regs[EBP] = regs[ESP] = None
                return
            regs[EBP] = slots.get(v[1])
            regs[ESP] = ("sp", v[1] + 4)
            return
        if m == "call":
            t = self.f.calls[i.address]
            self.const_eax_at[i.address] = regs[EAX]
            if i.address in self.f.noreturn:
                return
            if t in p.intrinsics and p.intrinsics[t] == "alloca_probe":
                n = regs[EAX]
                if not n or n[0] != "k":
                    regs[ESP] = regs[EAX] = None
                    return
                slots[sp - 4] = ("k", i.address + i.size)   # its return address, rewritten
                regs[ESP] = ("sp", sp - n[1])
                regs[EAX] = None
                for k in list(slots):
                    if k < sp:
                        del slots[k]
                slots[sp - n[1]] = ("k", i.address + i.size)
                return
            s = self._callee(t)
            slots[sp - 4] = ("k", i.address + i.size)
            for r in range(8):
                if r != ESP and r not in s.preserved:
                    regs[r] = None
            if s.pops is None:      # esp comes back at run time: not known here
                regs[ESP] = None
                slots.clear()
                return
            regs[ESP] = ("sp", sp + s.pops)
            for k in list(slots):
                if k < sp + s.pops:
                    del slots[k]
            return
        if m == "ret" or i.address in self.f.tails:
            return
        if m == "mov" and ops[0].type == X.X86_OP_REG and reg_full(i, ops[0].reg)[1] == 32:
            regs[reg_full(i, ops[0].reg)[0]] = self._val(i, ops[1], regs, slots)
            return
        if m == "mov" and ops[0].type == X.X86_OP_MEM:
            a = self._addr(i, ops[0], regs)
            if a is not None:
                if ops[0].size == 4:
                    slots[a] = self._val(i, ops[1], regs, slots)
                else:
                    for k in range(a - 3, a + ops[0].size):
                        slots.pop(k, None)
            return
        if m == "lea" and reg_full(i, ops[0].reg)[1] == 32:
            r = reg_full(i, ops[0].reg)[0]
            mm = ops[1].mem
            v = None
            if mm.index == 0 and mm.base != 0 and mm.segment == 0:
                b = regs[reg_full(i, mm.base)[0]]
                if mm.disp == 0:
                    v = b
                elif b and b[0] == "sp":
                    v = ("sp", b[1] + mm.disp)
                elif b and b[0] == "k":
                    v = ("k", (b[1] + mm.disp) & 0xffffffff)
            regs[r] = v
            return
        if m in ("add", "sub") and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_IMM and \
                reg_full(i, ops[0].reg)[1] == 32:
            r = reg_full(i, ops[0].reg)[0]
            v = regs[r]
            d = ops[1].imm if m == "add" else -ops[1].imm
            if v and v[0] == "sp":
                regs[r] = ("sp", v[1] + d)
            elif v and v[0] == "k":
                regs[r] = ("k", (v[1] + d) & 0xffffffff)
            else:
                regs[r] = None
            if r == ESP:
                for k in list(slots):
                    if regs[ESP] and k < regs[ESP][1]:
                        del slots[k]
            return
        # Anything else: what it writes becomes unknown; stores into the frame forget the slot.
        _, w = regs_rw(i)
        for r in w:
            if r == ESP and m not in ("push", "pop"):
                if m.startswith("rep") or m in ("stosd", "movsd", "stosb", "movsb", "stosw", "movsw"):
                    continue
                slots.clear()
            regs[r] = None
        if m == "xchg" and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG:
            a, b = reg_full(i, ops[0].reg), reg_full(i, ops[1].reg)
            if a[1] == 32 and b[1] == 32 and ESP not in (a[0], b[0]):
                pass
        for op in ops:
            if op.type == X.X86_OP_MEM and (op.access & capstone.CS_AC_WRITE):
                self._kill_mem(i, op, regs, slots)

    def _step_nosp(self, i, regs, slots):
        """esp not known: registers only. `mov esp, ebp` or `leave` can make
        it known again."""
        m = i.mnemonic
        ops = i.operands
        if m == "leave" or (m == "mov" and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG and
                            reg_full(i, ops[0].reg)[0] == ESP and reg_full(i, ops[1].reg)[0] == EBP):
            v = regs[EBP]
            regs[ESP] = v if v and v[0] == "sp" else None
            if m == "leave":
                if regs[ESP]:
                    regs[EBP] = slots.get(regs[ESP][1])
                    regs[ESP] = ("sp", regs[ESP][1] + 4)
                else:
                    regs[EBP] = None
            return
        if m == "call":
            if i.address in self.f.noreturn:
                return
            s = self._callee(self.f.calls[i.address])
            for r in range(8):
                if r not in s.preserved:
                    regs[r] = None
            return
        if m in ("ret",) or i.address in self.f.tails:
            return
        if m == "pop" and ops[0].type == X.X86_OP_REG:
            regs[reg_full(i, ops[0].reg)[0]] = None
            return
        _, w = regs_rw(i)
        for r in w:
            regs[r] = None

    def _kill_mem(self, i, op, regs, slots):
        a = self._addr(i, op, regs)
        if a is not None:
            for k in range(a - 3, a + op.size):
                slots.pop(k, None)

    def _callee(self, t):
        return self.p.callee_summary(t)

    # ---- register liveness, backward over the CFG ----
    def uses_defs(self, va, outs, skip_saves):
        f, p = self.f, self.p
        i = f.insns[va]
        m = i.mnemonic
        if va in f.noreturn:     # the game's fatal-error and exit paths: read nothing we must pass
            return P({ESP}), P(ALL)
        if va in f.calls:
            t = f.calls[va]
            s = self._callee(t)
            if t in p.natives and not p.natives[t]["handback"]:
                return P(set(p.natives[t]["reads"]) | {ESP}), P({EAX, ESP})
            if t in p.natives:   # the native first, the function when it hands back
                return P(set(p.natives[t]["reads"]) | set(s.reads) | {ESP}), P((ALL - s.preserved) | {EAX, ESP})
            return P(set(s.reads) | {ESP}), P((ALL - s.preserved) | {ESP})
        if va in f.tails:
            s = self._callee(f.tails[va])
            return P(set(s.reads) | {ESP}), P(ALL)
        if m == "ret":
            return P(set(outs) | {ESP}), P({ESP})
        r, w = parts_rw(i)
        if skip_saves and va in self.save_push and self.save_push[va] in skip_saves:
            r -= P([self.save_push[va]])
        return r, w

    def _liveness(self, outs, save_rule):
        f = self.f
        if outs is None:
            outs = set(ALL - self.preserved) - {ESP}
        self.outs = frozenset(outs)
        skip = set()
        if save_rule:
            popped = set()
            for va, i in f.insns.items():
                if i.mnemonic == "pop" and i.operands[0].type == X.X86_OP_REG and self.sp_at.get(va) is not None:
                    popped.add((reg_full(i, i.operands[0].reg)[0], self.sp_at[va]))
            skip = {r for va, r in self.save_push.items()
                    if r in self.preserved and (r, self.save_slot[va]) in popped}
            # Every push of a skipped register that isn't one of its saves stores.
            self.save_push = {va: r for va, r in self.save_push.items() if r in skip}
        for _ in range(4):
            live_out = self._solve(outs, skip)
            # A save the rule skips must not be read back live.
            bad = set()
            for va, i in f.insns.items():
                if i.mnemonic == "pop" and i.operands[0].type == X.X86_OP_REG:
                    r = reg_full(i, i.operands[0].reg)[0]
                    if r in skip and P([r]) & live_out[va]:
                        bad.add(r)
            if not bad:
                break
            skip -= bad
        self.skip_saves = frozenset(skip)
        self.live_out = {va: R_of(v) for va, v in live_out.items()}
        u, d = self.uses_defs(f.entry, outs, skip)
        self.live_in = frozenset(R_of((live_out[f.entry] - d) | u)) - {ESP}

    def _solve(self, outs, skip):
        f = self.f
        live_out = {va: set() for va in f.insns}
        preds = collections.defaultdict(list)
        for va, s in f.succ.items():
            for t in s:
                preds[t].append(va)
        ud = {va: self.uses_defs(va, outs, skip) for va in f.insns}
        work = collections.deque(sorted(f.insns, reverse=True))
        inq = set(work)
        live_in = {va: set() for va in f.insns}
        while work:
            va = work.popleft()
            inq.discard(va)
            lo = set()
            for t in f.succ[va]:
                lo |= live_in[t]
            live_out[va] = lo
            u, d = ud[va]
            li = (lo - d) | u
            if li != live_in[va]:
                live_in[va] = li
                for q in preds[va]:
                    if q not in inq:
                        work.append(q)
                        inq.add(q)
        self.live_in_at = live_in
        return live_out


class Flags:
    """Which instruction's flags each reader reads, and which definers store
    their operands for it. Flags are assumed dead at calls and rets."""

    def __init__(self, f):
        self.f = f
        defs, uses = {}, {}
        self.kind = {}
        for va, i in f.insns.items():
            d, u = flags_rw(i)
            if va in f.calls:
                d, u = FALL, 0
            defs[va], uses[va] = d, u
            if u:
                self.need_reader(i)
        # Reaching definitions per flag bit.
        ENTRY = -1
        inb = {f.entry: tuple(frozenset([ENTRY]) for _ in range(6))}
        work = [f.entry]
        reach_at = {}
        preds_done = set()
        while work:
            b = work.pop()
            st = list(inb[b])
            va = b
            while True:
                reach_at[va] = tuple(st)
                d = defs[va]
                for k in range(6):
                    if d & (1 << k):
                        st[k] = frozenset([va])
                i = f.insns[va]
                succ = f.succ[va]
                nxt = va + i.size
                if len(succ) == 1 and succ[0] == nxt and nxt not in f.leaders:
                    va = nxt
                    continue
                for t in succ:
                    old = inb.get(t)
                    new = tuple(st) if old is None else tuple(old[k] | st[k] for k in range(6))
                    if new != old:
                        inb[t] = new
                        work.append(t)
                break
        self.readers = {}
        self.live_defs = set()
        self.dynamic = set()      # definers that also store their kind
        self.cross_call = []      # readers of flags a call or the entry left
        for va, u in uses.items():
            if not u:
                continue
            src = set()
            per = []
            for k in range(6):
                if u & (1 << k):
                    per.append(reach_at[va][k])
                    src |= reach_at[va][k]
            if any(x == ENTRY or x in f.calls for x in src):
                self.cross_call.append(va)
                raise Refused("flags read at %08x come from a call or the entry" % va)
            if any(p != per[0] for p in per):
                raise Refused("flags read at %08x come from different instructions" % va)
            kinds = {self.kind_of(x) for x in src}
            if len(kinds) > 1:
                self.dynamic |= src
            self.readers[va] = (sorted(src), kinds.pop() if len(kinds) == 1 else None)
            self.live_defs |= src
        for d in self.live_defs:
            if self.kind_of(d) is None:
                raise Refused("flags of %s read at %08x" % (f.insns[d].mnemonic, d))
        # The shared operand slots: no other storing definer between a definer and its reader.
        self._check_slots(reach_at, uses)

    def need_reader(self, i):
        m = i.mnemonic
        if m.startswith("j") or m.startswith("set") or m.startswith("cmov") or m in ("adc", "sbb"):
            return
        raise Refused("%s reads flags at %08x" % (m, i.address))

    def kind_of(self, va):
        i = self.f.insns[va]
        m = i.mnemonic
        k = KIND.get(m)
        if k is None:
            return None
        if m in ("shl", "sal", "shr", "sar"):
            if i.operands[1].type != X.X86_OP_IMM:
                return None
        if m in ("imul", "mul") and len(i.operands) == 1 and i.operands[0].size != 4:
            return None
        w = i.operands[0].size
        return (k, w)

    def _check_slots(self, reach_at, uses):
        f = self.f
        # Forward: which storing definer last wrote the slots, per point.
        ENTRY = -1
        inb = {f.entry: frozenset([ENTRY])}
        work = [f.entry]
        last = {}
        while work:
            b = work.pop()
            st = inb[b]
            va = b
            while True:
                last[va] = st
                if va in self.live_defs:
                    st = frozenset([va])
                i = f.insns[va]
                succ = f.succ[va]
                nxt = va + i.size
                if len(succ) == 1 and succ[0] == nxt and nxt not in f.leaders:
                    va = nxt
                    continue
                for t in succ:
                    old = inb.get(t)
                    new = st if old is None else old | st
                    if new != old:
                        inb[t] = new
                        work.append(t)
                break
        for va, (src, _) in self.readers.items():
            if set(last[va]) != set(src):
                raise Refused("flags at %08x: another instruction's operands are in the way" % va)


# ---- C emission ----------------------------------------------------------

SIZES = {1: 8, 2: 16, 4: 32}
MASK = {1: "0xffu", 2: "0xffffu", 4: "0xffffffffu"}


class Emitter:
    def __init__(self, prog, compiled, mode, sigs):
        self.p, self.compiled, self.mode, self.sigs = prog, compiled, mode, sigs
    def name(self, va):
        return "AOT_SYM(%s_%08x)" % (self.mode, va)

    # Operands
    def reg_get(self, i, rid):
        r, bits, sh = reg_full(i, rid)
        n = REGS[r]
        if bits == 32:
            return n
        if bits == 16:
            return "(%s & 0xffffu)" % n
        if sh == 0:
            return "(%s & 0xffu)" % n
        return "((%s >> 8) & 0xffu)" % n

    def reg_set(self, i, rid, v):
        r, bits, sh = reg_full(i, rid)
        n = REGS[r]
        if bits == 32:
            return "%s = (uint32_t)(%s);" % (n, v)
        if bits == 16:
            return "%s = (%s & 0xffff0000u) | ((uint32_t)(%s) & 0xffffu);" % (n, n, v)
        if sh == 0:
            return "%s = (%s & 0xffffff00u) | ((uint32_t)(%s) & 0xffu);" % (n, n, v)
        return "%s = (%s & 0xffff00ffu) | (((uint32_t)(%s) & 0xffu) << 8);" % (n, n, v)

    def addr(self, i, op):
        m = op.mem
        parts = []
        if m.base:
            parts.append(REGS[reg_full(i, m.base)[0]])
        if m.index:
            parts.append("%s * %du" % (REGS[reg_full(i, m.index)[0]], m.scale))
        if m.disp or not parts:
            parts.append("0x%xu" % (m.disp & 0xffffffff))
        a = " + ".join(parts)
        seg = i.reg_name(m.segment) if m.segment else ""
        if seg == "fs":
            a = "aot_fs_base() + " + a
        elif seg not in ("", "ds", "es", "ss"):
            raise Refused("segment %s at %08x" % (seg, i.address))
        return "(uint32_t)(%s)" % a

    def get(self, i, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_get(i, op.reg)
        if op.type == X.X86_OP_IMM:
            return "0x%xu" % (op.imm & ((1 << (8 * size)) - 1))
        return "LD%d(%s)" % (SIZES[size], self.addr(i, op))

    def put(self, i, op, v):
        if op.type == X.X86_OP_REG:
            return self.reg_set(i, op.reg, v)
        return "ST%d(%s, %s);" % (SIZES[op.size], self.addr(i, op), v)

    def flag_store(self, va, a, b, r, c="0"):
        fl = self.flags
        if va not in fl.live_defs:
            return ""
        s = "F_A = %s; F_B = %s; F_R = %s; " % (a, b, r)
        if c != "0" or fl.kind_of(va)[0] in (9, 10, 11):
            s += "F_C = %s; " % c
        if va in fl.dynamic:
            k, w = fl.kind_of(va)
            s += "F_K = %d; " % (k * 8 + w)
        return s

    def cond(self, va, cc):
        src, kind = self.flags.readers[va]
        if kind is None:
            return "aot_cc_dyn(%d, F_K, F_A, F_B, F_R, F_C)" % CC_NUM[cc]
        k, w = kind
        return "aot_cc(%d, %d, %d, F_A, F_B, F_R, F_C)" % (CC_NUM[cc], k, w)

    def carry(self, va):
        src, kind = self.flags.readers[va]
        if kind is None:
            return "aot_cc_dyn(2, F_K, F_A, F_B, F_R, F_C)"
        return "aot_cc(2, %d, %d, F_A, F_B, F_R, F_C)" % kind

    # One function
    def function(self, va):
        p = self.p
        f = p.func(va)
        self.f = f
        self.flags = Flags(f)
        sig = self.sigs[va]
        self.sig = sig
        self.df = self._direction(f)
        out = []
        out.append(self.proto(va) + "\n{")
        decl = []
        for r in range(8):
            if self.mode == "ctx":
                decl.append("uint32_t %s = R->r[%d];" % (REGS[r], r))
            elif r == ESP or r in sig["in"]:
                continue
            else:
                decl.append("uint32_t %s = 0;" % REGS[r])
        decl.append("uint32_t F_A = 0, F_B = 0, F_R = 0, F_C = 0; int F_K = 0; (void)F_A; (void)F_B; (void)F_R; "
                    "(void)F_C; (void)F_K;")
        decl.append("uint8_t *const M = aot_mem; const uint32_t esp0 = esp, ra0 = LD32(esp);")
        out.append("    " + " ".join(decl))
        out.append("    AOT_TRACE(0x%08xu, eax, ecx, edx, ebx, esp, ebp, esi, edi);" % va)
        # Unused locals in some functions: tell gcc they may be.
        out.append("    " + " ".join("(void)%s;" % REGS[r] for r in range(8)))
        out.append("    goto L_%08x;" % f.entry)
        for a in f.order:
            i = f.insns[a]
            if a in f.leaders:
                out.append("L_%08x:" % a)
            out.append("    /* %08x %s %s */" % (a, i.mnemonic, i.op_str))
            if os.environ.get("AOT_TRACE_ALL") and int(os.environ["AOT_TRACE_ALL"], 16) == f.entry:
                out.append("    AOT_TRACE(0x%08xu, eax, ecx, edx, ebx, esp, ebp, esi, edi);" % a)
            code = self.insn(i)
            out.append("    " + code)
            nxt = a + i.size
            succ = f.succ[a]
            if len(succ) == 1 and succ[0] == nxt and not (i.mnemonic == "jmp"):
                if nxt in f.leaders:
                    out.append("    goto L_%08x;" % nxt)
        out.append("}")
        return "\n".join(out)

    def _direction(self, f):
        """The direction flag at each instruction: clear at entry and after
        calls (MSVC), std/cld change it."""
        df = {}
        inb = {f.entry: 0}
        work = [f.entry]
        while work:
            b = work.pop()
            d = inb[b]
            va = b
            while True:
                df[va] = d
                i = f.insns[va]
                if i.mnemonic == "std":
                    d = 1
                elif i.mnemonic == "cld" or va in f.calls:
                    d = 0
                succ = f.succ[va]
                nxt = va + i.size
                if len(succ) == 1 and succ[0] == nxt and nxt not in f.leaders:
                    va = nxt
                    continue
                for t in succ:
                    o = inb.get(t, d)
                    nv = d if o == d else None
                    if t not in inb or inb[t] != nv:
                        inb[t] = nv
                        work.append(t)
                break
        return df

    def proto(self, va):
        if self.mode == "ctx":
            return "AOT_FN void %s(AotRegs *R)" % self.name(va)
        sig = self.sigs[va]
        args = ["uint32_t esp"] + ["uint32_t %s" % REGS[r] for r in sig["in"]]
        outs = sig["out"]
        if not outs:
            ret = "void"
        elif len(outs) == 1:
            ret = "uint32_t"
        elif len(outs) == 2:
            ret = "uint64_t"
        else:
            ret = "void"
            args.append("uint32_t *o")
        return "AOT_FN %s %s(%s)" % (ret, self.name(va), ", ".join(args))

    def ret_stmt(self, pops):
        chk = "if (esp != esp0 || LD32(esp) != ra0) aot_bad_ret(0x%08xu, esp0, esp); " % self.f.entry
        if self.mode == "ctx":
            sets = " ".join("R->r[%d] = %s;" % (r, REGS[r]) for r in range(8) if r != ESP)
            return "%s%s R->r[4] = esp + %du; return;" % (chk, sets, 4 + pops)
        outs = self.sig["out"]
        if not outs:
            return chk + "return;"
        if len(outs) == 1:
            return chk + "return %s;" % REGS[outs[0]]
        if len(outs) == 2:
            return chk + "return (uint64_t)%s | (uint64_t)%s << 32;" % (REGS[outs[0]], REGS[outs[1]])
        return chk + " ".join("o[%d] = %s;" % (k, REGS[r]) for k, r in enumerate(outs)) + " return;"

    def call_compiled(self, t, ra):
        """A call to a compiled function; the return address is pushed."""
        if self.mode == "ctx":
            fill = ", ".join(REGS[r] for r in range(8))
            back = " ".join("%s = t.r[%d];" % (REGS[r], r) for r in range(8))
            return "{ AotRegs t = {{%s}}; %s(&t); %s }" % (fill, self.name(t), back)
        sig = self.sigs[t]
        args = ", ".join(["esp"] + [REGS[r] for r in sig["in"]])
        outs = sig["out"]
        pops = self.p.summ[t].pops
        tail = " esp += %du;" % (4 + pops) if pops is not None else ""
        if not outs:
            return "%s(%s);%s" % (self.name(t), args, tail)
        if len(outs) == 1:
            return "%s = %s(%s);%s" % (REGS[outs[0]], self.name(t), args, tail)
        if len(outs) == 2:
            return "{ uint64_t t = %s(%s); %s = (uint32_t)t; %s = (uint32_t)(t >> 32); }%s" % (
                self.name(t), args, REGS[outs[0]], REGS[outs[1]], tail)
        back = " ".join("%s = o[%d];" % (REGS[r], k) for k, r in enumerate(outs))
        return "{ uint32_t o[%d]; %s(%s, o); %s }%s" % (len(outs), self.name(t), args, back, tail)

    def call_guest(self, t, va):
        """Box86 runs the callee nested; every register through the context."""
        fill = ", ".join(REGS[r] for r in range(8))
        if self.mode == "sig":
            s = self.p.callee_summary(t)
            back = " ".join("%s = t.r[%d];" % (REGS[r], r) for r in range(8) if r not in s.preserved)
        else:
            back = " ".join("%s = t.r[%d];" % (REGS[r], r) for r in range(8))
        return "{ AotRegs t = {{%s}}; aot_guest_call(0x%08xu, &t); %s }" % (fill, t, back)

    def insn(self, i):
        va = i.address
        m = i.mnemonic
        ops = i.operands
        f = self.f
        if va in f.calls or va in f.tails:
            return self.call(i)
        if m == "ret":
            return self.ret_stmt(ops[0].imm if ops else 0)
        if m in ("int3", "hlt", "ud2"):
            return "aot_unreachable(0x%08xu);" % va
        if m == "nop":
            return ";"
        if m == "jmp":
            if va in f.switches:
                idx, targets, btab, default, bound = f.switches[va]
                # Re-read the index as the table entry the guard bounded.
                s = "switch (%s) { " % REGS[idx]
                for k, t in enumerate(targets):
                    s += "case %d: goto L_%08x; " % (k, t)
                s += "default: aot_unreachable(0x%08xu); }" % va
                return s
            return "goto L_%08x;" % ops[0].imm
        if m.startswith("j"):
            cc = cc_of(m, "j")
            if cc in ("ecxz", "cxz"):
                return "if (ecx == 0) goto L_%08x; goto L_%08x;" % (ops[0].imm, va + i.size)
            return "if (%s) goto L_%08x; goto L_%08x;" % (self.cond(va, cc), ops[0].imm, va + i.size)
        if m.startswith("set"):
            return self.put(i, ops[0], "(%s) ? 1u : 0u" % self.cond(va, cc_of(m, "set")))
        if m.startswith("cmov"):
            return "if (%s) { %s }" % (self.cond(va, cc_of(m, "cmov")), self.put(i, ops[0], self.get(i, ops[1])))
        if m == "mov":
            return self.put(i, ops[0], self.get(i, ops[1]))
        if m in ("movzx", "movsx"):
            v = self.get(i, ops[1])
            if m == "movsx":
                t = "int8_t" if ops[1].size == 1 else "int16_t"
                v = "(uint32_t)(int32_t)(%s)(%s)" % (t, v)
            return self.put(i, ops[0], v)
        if m == "lea":
            return self.put(i, ops[0], self.addr(i, ops[1]).replace("aot_fs_base() + ", ""))
        if m == "push":
            v = self.get(i, ops[0], 4) if ops[0].type != X.X86_OP_IMM else "0x%xu" % (ops[0].imm & 0xffffffff)
            if self.mode == "sig" and va in self.ana.save_push and self.ana.save_push[va] in self.ana.skip_saves:
                return "esp -= 4u; /* saves %s, which it preserves: not stored */" % REGS[self.ana.save_push[va]]
            return "{ uint32_t v = %s; esp -= 4u; ST32(esp, v); }" % v
        if m == "pop":
            if ops[0].type == X.X86_OP_REG:
                r = reg_full(i, ops[0].reg)[0]
                if r == ESP:
                    return "esp = LD32(esp);"
                return "{ uint32_t v = LD32(esp); esp += 4u; %s }" % self.put(i, ops[0], "v")
            return "{ uint32_t v = LD32(esp); esp += 4u; %s }" % self.put(i, ops[0], "v")
        if m == "leave":
            return "esp = ebp; ebp = LD32(esp); esp += 4u;"
        if m == "xchg":
            a, b = self.get(i, ops[0]), self.get(i, ops[1])
            return "{ uint32_t a = %s, b = %s; %s %s }" % (a, b, self.put(i, ops[0], "b"), self.put(i, ops[1], "a"))
        if m in ("add", "sub", "cmp", "and", "or", "xor", "test", "adc", "sbb"):
            return self.alu(i)
        if m in ("inc", "dec", "neg", "not"):
            w = ops[0].size
            a = self.get(i, ops[0])
            if m == "not":
                return "{ uint32_t a = %s; %s }" % (a, self.put(i, ops[0], "~a & %s" % MASK[w]))
            if m == "neg":
                r = "(0u - a) & %s" % MASK[w]
            else:
                r = "(a %s 1u) & %s" % ("+" if m == "inc" else "-", MASK[w])
            return "{ uint32_t a = %s, r = %s; %s%s }" % (a, r, self.flag_store(va, "a", "1u", "r"),
                                                         self.put(i, ops[0], "r"))
        if m in ("shl", "sal", "shr", "sar", "rol", "ror"):
            return self.shift(i)
        if m == "imul":
            return self.imul(i)
        if m == "mul" and ops[0].size == 4:
            v = self.get(i, ops[0])
            return "{ uint64_t p = (uint64_t)eax * (uint64_t)(%s); eax = (uint32_t)p; edx = (uint32_t)(p >> 32); %s}" \
                % (v, self.flag_store(va, "0u", "0u", "eax", "edx != 0u"))
        if m in ("div", "idiv") and ops[0].size == 4:
            v = self.get(i, ops[0])
            if m == "div":
                return "{ uint64_t n = (uint64_t)edx << 32 | eax; uint32_t d = %s; if (!d) aot_fault(0x%08xu); " \
                       "eax = (uint32_t)(n / d); edx = (uint32_t)(n %% d); }" % (v, va)
            return "{ int64_t n = (int64_t)((uint64_t)edx << 32 | eax); int32_t d = (int32_t)(%s); " \
                   "if (!d) aot_fault(0x%08xu); eax = (uint32_t)(int32_t)(n / d); edx = (uint32_t)(int32_t)(n %% d); }" \
                % (v, va)
        if m == "cdq":
            return "edx = (eax & 0x80000000u) ? 0xffffffffu : 0u;"
        if m == "cwde":
            return "eax = (uint32_t)(int32_t)(int16_t)eax;"
        if m in ("cld", "std"):
            return ";"
        if m.startswith("rep") or m in ("stosd", "stosb", "stosw", "movsd", "movsb", "movsw"):
            return self.string(i)
        raise Refused("%s at %08x" % (m, va))

    def alu(self, i):
        va = i.address
        m = i.mnemonic
        ops = i.operands
        w = ops[0].size
        a, b = self.get(i, ops[0]), self.get(i, ops[1], w)
        M = MASK[w]
        if ops[1].type == X.X86_OP_IMM:
            b = "0x%xu" % (ops[1].imm & ((1 << (8 * w)) - 1))
        if m in ("xor", "sub") and ops[1].type == X.X86_OP_REG and ops[0].type == X.X86_OP_REG and \
                ops[0].reg == ops[1].reg:
            return "{ uint32_t a = 0u, r = 0u; %s%s }" % (self.flag_store(va, "a", "a", "r"), self.put(i, ops[0], "0u"))
        if m == "add":
            r = "(a + b) & %s" % M
        elif m in ("sub", "cmp"):
            r = "(a - b) & %s" % M
        elif m in ("and", "test"):
            r = "a & b"
        elif m == "or":
            r = "a | b"
        elif m == "xor":
            r = "a ^ b"
        elif m == "adc":
            r = "(a + b + c) & %s" % M
        else:
            r = "(a - b - c) & %s" % M
        c = ""
        cv = "0"
        if m in ("adc", "sbb"):
            c = ", c = %s ? 1u : 0u" % self.carry(va)
            cv = "c"
        st = self.flag_store(va, "a", "b", "r", cv)
        wr = "" if m in ("cmp", "test") else self.put(i, ops[0], "r")
        return "{ uint32_t a = %s, b = %s%s, r = %s; %s%s }" % (a, b, c, r, st, wr)

    def shift(self, i):
        va = i.address
        m = i.mnemonic
        ops = i.operands
        w = ops[0].size
        bits = 8 * w
        a = self.get(i, ops[0])
        if len(ops) == 1:
            n = "1u"
        elif ops[1].type == X.X86_OP_IMM:
            n = "%du" % (ops[1].imm & 31)
        else:
            n = "(ecx & 31u)"
        M = MASK[w]
        if m in ("shl", "sal"):
            r = "(n ? (a << n) & %s : a)" % M
        elif m == "shr":
            r = "(n ? (a >> n) : a)"
        elif m == "sar":
            st = {1: "int8_t", 2: "int16_t", 4: "int32_t"}[w]
            r = "(n ? (uint32_t)((int32_t)(%s)a >> n) & %s : a)" % (st, M)
        elif m == "rol":
            r = "(n %% %d ? ((a << (n %% %d)) | (a >> (%d - n %% %d))) & %s : a)" % (bits, bits, bits, bits, M)
        else:
            r = "(n %% %d ? ((a >> (n %% %d)) | (a << (%d - n %% %d))) & %s : a)" % (bits, bits, bits, bits, M)
        st = self.flag_store(va, "a", "n", "r")
        return "{ uint32_t a = %s, n = %s, r = %s; %s%s }" % (a, n, r, st, self.put(i, ops[0], "r"))

    def imul(self, i):
        va = i.address
        ops = i.operands
        if len(ops) == 1:
            if ops[0].size != 4:
                raise Refused("imul of %d bytes at %08x" % (ops[0].size, va))
            v = self.get(i, ops[0])
            return "{ int64_t p = (int64_t)(int32_t)eax * (int64_t)(int32_t)(%s); eax = (uint32_t)p; " \
                   "edx = (uint32_t)((uint64_t)p >> 32); %s}" % (
                       v, self.flag_store(va, "0u", "0u", "eax", "(p != (int64_t)(int32_t)p)"))
        w = ops[0].size
        if w != 4:
            raise Refused("imul of %d bytes at %08x" % (w, va))
        a = self.get(i, ops[1] if len(ops) == 3 else ops[0])
        b = self.get(i, ops[2] if len(ops) == 3 else ops[1], 4)
        return "{ int64_t p = (int64_t)(int32_t)(%s) * (int64_t)(int32_t)(%s); uint32_t r = (uint32_t)p; %s%s }" % (
            a, b, self.flag_store(va, "0u", "0u", "r", "(p != (int64_t)(int32_t)p)"), self.put(i, ops[0], "r"))

    def string(self, i):
        va = i.address
        m = i.mnemonic
        rep = m.startswith("rep ")
        op = m.split()[-1]
        d = self.df.get(va)
        if d is None:
            raise Refused("direction flag not known at %08x" % va)
        size = {"b": 1, "w": 2, "d": 4}[op[-1]]
        step = -size if d else size
        if op.startswith("movs"):
            body = "ST%d(edi, LD%d(esi)); esi += %du; edi += %du;" % (SIZES[size], SIZES[size], step & 0xffffffff,
                                                                      step & 0xffffffff)
            fast = "aot_movs(edi, esi, ecx, %d, %d);" % (size, d)
        elif op.startswith("stos"):
            v = {1: "(eax & 0xffu)", 2: "(eax & 0xffffu)", 4: "eax"}[size]
            body = "ST%d(edi, %s); edi += %du;" % (SIZES[size], v, step & 0xffffffff)
            fast = "aot_stos(edi, eax, ecx, %d, %d);" % (size, d)
        else:
            raise Refused("%s at %08x" % (m, va))
        if not rep:
            return "{ %s }" % body
        n = "ecx"
        adv = "{ uint32_t n = ecx; %s edi += n * %du; %s ecx = 0; }" % (
            fast, step & 0xffffffff, ("esi += n * %du;" % (step & 0xffffffff)) if op.startswith("movs") else "")
        return adv

    def call(self, i):
        va = i.address
        f, p = self.f, self.p
        tail = va in f.tails
        t = f.tails[va] if tail else f.calls[va]
        ra = va + i.size
        if tail:
            # A jmp to another function: a call from here that returns for us.
            # The return address our caller pushed stays where it is.
            raise Refused("tail call at %08x" % va)
        push = "esp -= 4u; ST32(esp, 0x%08xu); " % ra
        if t in p.intrinsics:
            name = p.intrinsics[t]
            if name == "memset":
                return push + "{ uint32_t d = LD32(esp + 4u); aot_memset(d, LD32(esp + 8u), LD32(esp + 12u)); " \
                              "eax = d; esp += 4u; }"
            if name == "memcpy":
                return push + "{ uint32_t d = LD32(esp + 4u); aot_memmove(d, LD32(esp + 8u), LD32(esp + 12u)); " \
                              "eax = d; esp += 4u; }"
            if name == "alloca_probe":
                return push + "ST32(esp - 4u, ecx); ST32(esp - eax, 0x%08xu); esp = esp - eax + 4u; " \
                              "eax = 0x%08xu;" % (ra, ra)
            raise Refused("intrinsic %s" % name)
        if isinstance(t, tuple) or t is None:
            raise Refused("an import or indirect call at %08x" % va)
        s = push
        nat = p.natives.get(t)
        inner = self.call_compiled(t, ra) if t in self.compiled else self.call_guest(t, va)
        if va in f.noreturn:
            inner += " aot_unreachable(0x%08xu);" % va
        if nat and not nat["handback"]:
            args = ", ".join([REGS[r] for r in nat["args"]] + ["LD32(esp + %du)" % (4 + 4 * k)
                                                            for k in range(nat["stack"])])
            return s + "eax = (uint32_t)%s(%s); esp += %du;" % (nat["glue"], args, 4 + 4 * nat["stack"])
        if nat:
            args = ", ".join([REGS[r] for r in nat["args"]] + ["LD32(esp + %du)" % (4 + 4 * k)
                                                            for k in range(nat["stack"])])
            return s + "{ uint64_t n = %s(%s); if ((uint32_t)(n >> 32)) { eax = (uint32_t)n; esp += %du; } " \
                       "else { %s } }" % (nat["glue"], args, 4 + 4 * nat["stack"], inner)
        return s + inner


# ---- the program ---------------------------------------------------------

NATIVES = {  # our natives the spike's code calls in C (src/game/d2_native.cpp, aot_native_*)
    # RoomFind covers every call (pure, exact); CollisionCross hands a room's
    # edge back, to the compiled 64d4e0.
    0x00463740: {"name": "RoomFind", "glue": "aot_native_463740", "args": [ECX, EDX], "stack": 1,
                 "reads": [ECX, EDX], "handback": False},
    0x0064d4e0: {"name": "CollisionCross", "glue": "aot_native_64d4e0", "args": [ESI], "stack": 3,
                 "reads": [ESI], "handback": True},
}
INTRINSICS = {0x00681ef0: "memset", 0x006829c0: "memcpy", 0x006831b0: "alloca_probe"}


def load_starts(img, tsv):
    starts = set()
    if tsv and os.path.exists(tsv):
        with open(tsv) as fh:
            next(fh)
            for line in fh:
                a = line.split("\t")
                try:
                    starts.add(int(a[0], 16))
                except ValueError:
                    pass
    return starts


def root_head(img, va):
    """The whole instructions a trampoline copies (5 bytes or more), with no
    branch among them (they'd need relocating), and the game's 16 bytes."""
    keep = 0
    while keep < 5:
        i = img.insn(va + keep)
        if i.mnemonic in ("call", "jmp", "ret") or i.mnemonic.startswith("j") or \
                any(o.type == X.X86_OP_MEM and o.mem.base == X.X86_REG_EIP for o in i.operands):
            raise Refused("a branch in the first bytes at %08x: no trampoline" % va)
        keep += i.size
    return keep, [img.u8(va + k) for k in range(16)]


def compile_set(img, starts, roots, extra, mode_list, outdir, names=None):
    names = names or {}
    t0 = time.time()
    prog = Program(img, starts | set(roots) | set(extra), NATIVES, INTRINSICS)
    want = list(roots) + [v for v in extra if v not in roots]
    reach = prog.closure(want)
    compiled = prog.compiled
    refused = {}
    for va in want:
        f = prog.func(va)
        if f.refused:
            refused[va] = f.refused
            continue
        compiled.add(va)
    prog.summarize(sorted(reach))
    while True:
        # Signatures: a joint fixed point, from nothing up, of each compiled
        # function's inputs (its live-in, given its outputs and its callees'
        # inputs) and outputs (what its compiled callers read after calling it;
        # for a root, MSVC's rule for Box86's callers: eax and the callee-saved
        # registers it doesn't preserve).
        while True:
            try:
                for va in compiled:
                    s = prog.summ[va]
                    prog.summ[va] = Summary((), s.preserved, s.pops, True)
                outs = {va: set() for va in compiled}
                for va in roots:
                    if va in compiled:
                        outs[va] = {EAX} | ({EBX, EBP, ESI, EDI} - prog.summ[va].preserved)
                for _ in range(100):
                    anas = {}
                    for va in sorted(compiled):
                        f = prog.func(va)
                        cur = va
                        a = Analysis(prog, f, outs=outs[va], save_rule=True)
                        if not a.esp_ok:
                            raise Refused("esp at a ret isn't known to be the entry's")
                        Flags(f)
                        anas[va] = a
                        s = prog.summ[va]
                        prog.summ[va] = Summary(a.live_in, s.preserved, s.pops, True)
                    new = {va: set(o) for va, o in outs.items()}
                    for va, a in anas.items():
                        for site, t in prog.func(va).calls.items():
                            if t in compiled:
                                new[t] |= a.live_out[site] - prog.summ[t].preserved - {ESP}
                    ins = {va: prog.summ[va].reads for va in compiled}
                    if new == outs and ins == getattr(compile_set, "_ins", None):
                        break
                    compile_set._ins = ins
                    outs = new
                break
            except Refused as e:
                refused[cur] = str(e)
                compiled.discard(cur)
                prog.summarize(sorted(reach))   # the summaries as for a function left to Box86
        sigs = {va: {"in": sorted(prog.summ[va].reads - {ESP}), "out": sorted(outs[va]),
                     "preserved": sorted(prog.summ[va].preserved)} for va in compiled}
        # Every function emitted once, both ways, before writing anything: what
        # the emitter refuses goes to Box86, and the signatures again.
        bad = None
        for mode in mode_list:
            em = Emitter(prog, compiled, mode, sigs)
            for va in sorted(compiled):
                try:
                    em.ana = Analysis(prog, prog.func(va), outs=set(sigs[va]["out"]), save_rule=(mode == "sig"))
                    em.function(va)
                except Refused as e:
                    bad = (va, str(e))
                    break
            if bad:
                break
        if not bad:
            break
        refused[bad[0]] = bad[1]
        compiled.discard(bad[0])
        prog.summarize(sorted(reach))
    # Each compiled function's own analysis with its outs, then the C.
    t1 = time.time()
    chunks = ['/* Generated by tools/aot/recompile.py from the user\'s Game.exe: never commit or distribute. */',
              '#include "game/aot_rt.h"', ""]
    report = {"functions": {}, "refused": {"%08x" % k: v for k, v in refused.items()}, "fallbacks": {}}
    for mode in mode_list:
        em = Emitter(prog, compiled, mode, sigs)
        protos = [em.proto(va) + ";" for va in sorted(compiled)]
        chunks += protos + [""]
        for va in sorted(compiled):
            f = prog.func(va)
            em.ana = Analysis(prog, f, outs=set(sigs[va]["out"]), save_rule=(mode == "sig"))
            chunks.append(em.function(va))
            chunks.append("")
        for va in roots:
            if va not in compiled:
                continue
            body = []
            if mode == "ctx":
                body.append("AotRegs t; for (int k = 0; k < 8; k++) t.r[k] = r[k]; %s(&t);" % em.name(va))
                body.append("for (int k = 0; k < 8; k++) if (k != 4) r[k] = t.r[k];")
            else:
                sig = sigs[va]
                args = ", ".join(["r[4]"] + ["r[%d]" % x for x in sig["in"]])
                outs_ = sig["out"]
                if len(outs_) == 1:
                    body.append("r[%d] = %s(%s);" % (outs_[0], em.name(va), args))
                elif len(outs_) == 2:
                    body.append("uint64_t t = %s(%s); r[%d] = (uint32_t)t; r[%d] = (uint32_t)(t >> 32);" % (
                        em.name(va), args, outs_[0], outs_[1]))
                else:
                    body.append("uint32_t o[8]; %s(%s%s);" % (em.name(va), args, ", o" if outs_ else ""))
                    body.append(" ".join("r[%d] = o[%d];" % (x, k) for k, x in enumerate(outs_)))
            chunks.append("static uint32_t AOT_SYM(%s_root_%08x)(uint32_t *r)\n{\n    %s\n    return r[0];\n}\n" % (
                mode, va, "\n    ".join(body)))
    chunks.append("const AotRoot AOT_SYM(roots)[] = {")
    for va in roots:
        if va in compiled:
            keep, head = root_head(img, va)
            chunks.append("    {0x%08xu, %s, %s, \"%s\", %d, %d, {%s}}," % (
                va, "AOT_SYM(sig_root_%08x)" % va if "sig" in mode_list else "0",
                "AOT_SYM(ctx_root_%08x)" % va if "ctx" in mode_list else "0",
                names.get(va, "%08x" % va), keep, prog.summ[va].pops, ", ".join("0x%02x" % b for b in head)))
    chunks.append("    {0, 0, 0, 0, 0, 0, {0}}")
    chunks.append("};")
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "aot_spike.c"), "w") as fh:
        fh.write("\n".join(chunks) + "\n")
    # The report: per function its size, its signature, its calls out.
    insns = 0
    for va in sorted(compiled):
        f = prog.func(va)
        insns += f.stats["insns"]
        falls = sorted({t for t in f.calls.values() if isinstance(t, int) and t not in compiled
                        and t not in INTRINSICS})
        report["functions"]["%08x" % va] = {
            "insns": f.stats["insns"], "bytes": f.stats["bytes"], "sig": sigs[va],
            "switches": len(f.switches), "natives": sorted("%08x" % t for t in falls if t in NATIVES),
            "box86": sorted("%08x" % t for t in falls if t not in NATIVES)}
    report["x86_insns"] = insns
    report["x86_bytes"] = sum(prog.func(va).stats["bytes"] for va in compiled)
    report["analysis_s"] = round(t1 - t0, 3)
    report["emit_s"] = round(time.time() - t1, 3)
    report["closure"] = len(reach)
    with open(os.path.join(outdir, "aot_spike.json"), "w") as fh:
        json.dump(report, fh, indent=1, sort_keys=True)
    return report


def survey(img, starts, tsv):
    """The note's whole-binary questions, over d2d's functions."""
    sizes = {}
    with open(tsv) as fh:
        next(fh)
        for line in fh:
            a = line.split("\t")
            try:
                sizes[int(a[0], 16)] = int(a[2])
            except (ValueError, IndexError):
                pass
    prog = Program(img, starts, {}, INTRINSICS)
    res = collections.Counter()
    why = collections.Counter()
    x87_unknown = []
    for va in sorted(sizes):
        f = prog.func(va)
        if f.refused:
            res["cfg_refused"] += 1
            why[f.refused.split(" at ")[0].split(" %")[0]] += 1
            continue
        res["functions"] += 1
        res["switches_bounded"] += len(f.switches)
        # Flags: a reader reached by a call's or the entry's flags.
        try:
            Flags(f)
            res["flags_ok"] += 1
        except Refused as e:
            s = str(e)
            if "from a call or the entry" in s:
                res["flags_live_at_call_or_ret"] += 1
            else:
                res["flags_other"] += 1
                why["flags: " + s.split(" at ")[0]] += 1
        # x87: is the stack depth known at each instruction?
        d = x87_depth(f, prog)
        if d is None:
            x87_unknown.append(va)
        elif d:
            res["x87_known"] += 1
    # Switches the guard rule doesn't bound, from the refusals.
    res["switches_unbounded"] = why["a switch"] if "a switch" in why else sum(
        v for k, v in why.items() if k.startswith("a switch"))
    res["x87_unknown"] = len(x87_unknown)
    return res, why, x87_unknown


X87_PUSH = {"fld", "fild", "fld1", "fldz", "fldpi", "fldl2e", "fldln2", "fldlg2", "fldl2t"}
X87_POP1 = {"fstp", "fistp", "faddp", "fsubp", "fsubrp", "fmulp", "fdivp", "fdivrp", "fcomp", "ficomp",
            "fucomp", "fcomip", "fucomip", "fisttp"}
X87_POP2 = {"fcompp", "fucompp"}


def x87_depth(f, prog):
    """0: no x87; 1: the stack depth is known at every instruction (from 0 at
    entry, a call leaving it as it was); None: not known somewhere."""
    if not any(i.mnemonic.startswith("f") and i.mnemonic not in ("fs",) for i in f.insns.values()):
        return 0
    depth = {f.entry: 0}
    work = [f.entry]
    while work:
        b = work.pop()
        d = depth[b]
        va = b
        while True:
            i = f.insns[va]
            m = i.mnemonic
            if m in X87_PUSH:
                d += 1
            elif m in X87_POP1 or (m.startswith("f") and m.endswith("p") and m not in ("fstp",) and
                                   m not in X87_POP2 and m != "fxtractp"):
                d -= 1
            elif m in X87_POP2:
                d -= 2
            elif m in ("fninit", "finit"):
                d = 0
            elif va in f.calls:
                # A callee may return a float in st(0): not known without its type.
                t = f.calls[va]
                if isinstance(t, int) and t not in prog.intrinsics:
                    g = prog.func(t)
                    if not g.refused and any(x.mnemonic in X87_PUSH for x in g.insns.values()):
                        return None
            if d < 0 or d > 8:
                return None
            succ = f.succ[va]
            nxt = va + i.size
            if len(succ) == 1 and succ[0] == nxt and nxt not in f.leaders:
                va = nxt
                continue
            for t in succ:
                if t not in depth:
                    depth[t] = d
                    work.append(t)
                elif depth[t] != d:
                    return None
            break
    return 1


def parse_vas(s):
    out, names = [], {}
    for part in filter(None, (s or "").split(",")):
        a, _, n = part.partition(":")
        v = int(a, 16)
        out.append(v)
        if n:
            names[v] = n
    return out, names


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("exe")
    ap.add_argument("--out", default="build/aot/spike")
    ap.add_argument("--root", action="append", default=[], help="va[:name], a compiled entry Box86 calls")
    ap.add_argument("--compile", default="", help="more functions to compile, va,va,...")
    ap.add_argument("--tree", action="append", default=[],
                    help="va:depth: also compile what va calls, directly, to that depth")
    ap.add_argument("--functions", default="third_party/d2d/docs/research/re/game-functions.tsv")
    ap.add_argument("--modes", default="sig,ctx")
    ap.add_argument("--survey", action="store_true")
    a = ap.parse_args()
    img = Image(a.exe)
    starts = load_starts(img, a.functions)
    if a.survey:
        res, why, x87u = survey(img, starts, a.functions)
        for k in sorted(res):
            print("%-28s %d" % (k, res[k]))
        print("refusals:")
        for k, v in why.most_common(20):
            print("  %6d  %s" % (v, k))
        return
    roots, names = [], {}
    for r in a.root:
        v, n = parse_vas(r)
        roots += v
        names.update(n)
    extra, _ = parse_vas(a.compile)
    for t in a.tree:
        va, _, d = t.partition(":")
        prog = Program(img, starts, NATIVES, INTRINSICS)
        lvl, seen = {int(va, 16)}, {int(va, 16)}
        for _ in range(int(d or 2)):
            nxt = set()
            for g in lvl:
                f = prog.func(g)
                if f.refused:
                    continue
                for c in f.calls.values():
                    if isinstance(c, int) and c not in seen and c not in INTRINSICS and c not in NATIVES:
                        nxt.add(c)
            seen |= nxt
            lvl = nxt
        extra += sorted(seen - set(extra))
    rep = compile_set(img, starts, roots, extra, a.modes.split(","), a.out, names)
    n = len(rep["functions"])
    print("aot: %d functions compiled (%d x86 instructions, %d bytes), %d left to Box86, in %s" % (
        n, rep["x86_insns"], rep["x86_bytes"], len(rep["refused"]), a.out))
    for k, v in sorted(rep["refused"].items()):
        print("  left to Box86: %s: %s" % (k, v))


if __name__ == "__main__":
    main()
