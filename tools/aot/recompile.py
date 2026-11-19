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

The output is Game.exe in another form: it goes under build/ and into the
CIA (which may be published, docs/aot.md's Distribution), never into the
repository or a container image.
"""
import argparse
import collections
import json
import os
import re
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
        "neg": 5, "shl": 6, "sal": 6, "shr": 7, "sar": 8, "imul": 9, "mul": 9, "adc": 10, "sbb": 11,
        "bsf": 2, "bsr": 2}   # bsf/bsr: ZF from the source, as a logic op's from its result (the rest undefined)
KIND_DEFS = {3: FALL & ~FC, 4: FALL & ~FC}
X87_PREFIX = "f"


# The C runtime's exit(code), 681e09: doexit(code, 0, 0) at 681d27, whose
# retcaller = 0 ends in ExitProcess. Its ret only serves other arguments.
NORETURN_FUNCS = {0x00681e09}
NORETURN_IMPORTS = {"ExitProcess", "ExitThread", "FatalAppExitA", "FatalExit", "TerminateThread"}
# Imports whose shim may block the calling thread until another one runs: a
# wait, a sleep, a lock, a read (the I/O worker on the console), the message
# queue, the network (issue #115: compiled code calls Box86 nested, and there
# the scheduler can't switch away).
BLOCKING_IMPORTS = ("Wait", "Sleep", "EnterCriticalSection", "ReadFile", "GetOverlappedResult", "GetMessage",
                    "MsgWait", "SignalObjectAndWait", "DeviceIoControl", "recv", "send", "select", "connect",
                    "accept", "gethostbyname", "DirectSound", "DirectDraw", "SwitchToThread")
# The game's fatal-error report (Fog's assert message, its string lookup):
# calls to them are on paths that end the game.
ERROR_PATH_FUNCS = {0x00408090, 0x00408a60}
# Indirect and import calls as callouts (issue #115, aot_call_out), not plain
# exits: --no-callouts turns them off, for comparing.
CALLOUTS = True


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


class PlainRet:
    """`rep ret` (F3 C3, AMD's two-byte return, as in MSVC's
    __security_check_cookie, 681a48): a ret; the prefix does nothing (issue
    #135: refused, its 625 compiled call sites each ran it nested by Box86)."""

    def __init__(self, i):
        self._i = i

    mnemonic = "ret"

    def __getattr__(self, name):
        return getattr(self._i, name)


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
            if i.mnemonic in ("repz ret", "rep ret"):
                i = PlainRet(i)
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


# x87 (issue #136): Capstone 5's operand access is wrong for most of it (fst,
# fstp and fistp m32/m64 and fnstcw read, frstor written) and so is fnsave's
# size (4), so what an x87 instruction stores is by mnemonic here.
X87_STORES = {"fst", "fstp", "fist", "fistp", "fisttp", "fnstsw", "fnstcw", "fbstp", "fnsave", "fnstenv"}
X87_MEM_SIZE = {"fnsave": 108, "frstor": 108, "fnstenv": 28, "fldenv": 28}


def mem_written(i):
    """(operand, bytes) for each memory operand the instruction writes."""
    m = i.mnemonic
    mems = [op for op in i.operands if op.type == X.X86_OP_MEM]
    if m.startswith("f"):
        return [(op, X87_MEM_SIZE.get(m, op.size)) for op in mems] if m in X87_STORES else []
    return [(op, op.size) for op in mems if op.access & capstone.CS_AC_WRITE]


class Func:
    """One function's code: its instructions and basic blocks, from its entry."""

    def __init__(self, img, entry, starts):
        self.img = img
        self.entry = entry
        self.insns = {}          # va -> insn
        self.succ = {}           # va -> [va] (inside the function)
        self.calls = {}          # va -> target (direct), or None (indirect)
        self.tails = {}          # va -> target: a jmp to another function's entry; None through a pointer, ("import", slot)
        self.switches = {}       # va -> (index reg, [targets], (byte table va or None))
        self.rets = {}           # va -> bytes popped
        self.noreturn = set()    # call sites not coming back
        self.exits = frozenset() # compiled (issue #115): call sites that hand the rest of the call to Box86
        self.callouts = {}       # exits that come back when they can (issue #115): va -> the bytes the callee is taken to pop
        self.plain = False       # its indirect and import calls plain exits (a callout's pops threw its stack off)
        self.data = set()        # table bytes
        self.refused = None
        self.stats = collections.Counter()
        try:
            self._walk(starts)
        except Refused as e:
            self.refused = str(e)
        # What its rets pop and where it tail-jumps, as decoded (issue #138:
        # exits cut the paths behind them, rets and tails too; a function
        # whose every ret was cut still pops what they say when it returns).
        self.ret_pops = frozenset(self.rets.values())
        self.tail_targets = frozenset(self.tails.values())

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
                    if sw is None:
                        # Not through a table: a tail call through a pointer
                        # (issue #138: a vtable's method, a callback, an
                        # import's thunk), esp checked at run time.
                        op = ops[0]
                        imp = op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and \
                            op.mem.disp in img.imports
                        self.tails[va] = ("import", op.mem.disp) if imp else None
                        self.succ[va] = []
                        break
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
        return self.cut(hit)

    def set_exits(self, sites, callouts=True, tail_pops=None):
        """Compiled, the call sites (and tail jumps) whose callee Box86 runs
        from the site on (issue #115): its compiled code ends there, so their
        paths end too, as at a call that never returns. An indirect or import
        call is a callout instead (aot_call_out): our shim or a compiled
        function called in C, the code after it compiled, and an exit only
        where neither (or where what came back isn't what was assumed)."""
        self.exits = frozenset(sites)
        self.plain = not callouts
        if CALLOUTS and callouts:
            self.callouts = {va: self.callout_pops(va) for va in sites
                             if va in self.calls and va not in self.noreturn and
                             (self.calls[va] is None or isinstance(self.calls[va], tuple))}
            # A tail call through a pointer (issue #138): a callout from esp
            # as at our entry, the target taken to pop what our rets do, or
            # a thunk's what Program.thunk_pops says; else it stays an exit.
            pops = next(iter(self.ret_pops)) if len(self.ret_pops) == 1 else \
                tail_pops if not self.ret_pops else None
            if pops is not None:
                self.callouts.update({va: pops for va in sites if va in self.tails and
                                      (self.tails[va] is None or isinstance(self.tails[va], tuple))})
        self.cut([va for va in sites if va in self.calls and va not in self.noreturn and va not in self.callouts])

    def callout_pops(self, va):
        """What a callout's callee is taken to pop (checked at run time: an
        exit at the return address where it differs). An import's, where the
        image knows it; none where the caller cleans up (add esp after);
        else the arguments pushed just before it, as stdcall and thiscall do."""
        t = self.calls[va]
        if isinstance(t, tuple) and self.img.import_pops.get(t[1]) is not None:
            return self.img.import_pops[t[1]]
        i = self.insns[va]
        nxt = self.insns.get(va + i.size)
        if nxt is not None and nxt.mnemonic == "add" and nxt.op_str.startswith("esp, "):
            return 0
        ends = {k + j.size: k for k, j in self.insns.items()}
        n, cur = 0, va
        while cur not in self.leaders and cur in ends:
            k = ends[cur]
            j = self.insns[k]
            if j.mnemonic == "push":
                n += 1
            elif j.mnemonic in ("call", "pop", "ret", "leave", "pushal", "popal") or \
                    j.op_str.split(",")[0].strip() == "esp":
                break
            cur = k
        return 4 * n

    def cut(self, hit):
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
        or through a byte index table first (movzx reg, byte [reg + bytes]).
        None when it isn't through a table (a tail call through a pointer)."""
        img = self.img
        op = i.operands[0]
        if op.type != X.X86_OP_MEM or op.mem.base != 0 or op.mem.scale != 4 or op.mem.index == 0:
            return None
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
        self.intrinsics = dict(intrinsics)    # va -> name
        self.frame_consts = {}
        for va, (name, want) in FRAME_HELPERS.items():   # issue #138: only where the code is what we model
            got = frame_helper_consts(img, va, want)
            if got is not None:
                self.intrinsics[va] = name
                self.frame_consts[name] = got
        self.garbage = {}               # va -> ecx/edx it leaves as a library call's garbage (Analysis.garbage)
        self.thunk_pops = None          # va -> what a thunk through a pointer pops (issue #138, _thunk_pops)
        self.funcs = {}
        self.summ = {}
        self.compiled = set()   # what compile_set compiles: their summaries are proven, not assumed
        self.may_exit = set()   # compiled functions that may hand a call to Box86 midway (theirs or a callee's exit)
        self.may_hard = set()   # ... at other than a callout (apply_exits)
        self.plain = set()      # compiled functions whose indirect and import calls are plain exits (Func.plain)

    def func(self, va):
        f = self.funcs.get(va)
        if f is None:
            f = self.funcs[va] = Func(self.img, va, self.starts)
        return f

    def closure(self, roots, limit=30000):
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
            if name in ("seh_prolog4", "eh_prolog3_catch"):   # saves ebx, esi, edi and ebp in the frame it builds
                return Summary(reads=(EBX, EBP, ESI, EDI, ESP) + ((EAX,) if name == "eh_prolog3_catch" else ()),
                               preserved=(ECX, EDX, EBX, ESI, EDI), pops=None, known=True)
            if name in ("seh_epilog4", "eh_epilog3"):          # restores them from it
                return Summary(reads=(EBP, ESP), preserved=(EAX, EDX), pops=None, known=True)
            return Summary(reads=(ESP,), preserved=(EBX, EBP, ESI, EDI), pops=0, known=True)
        s = self.summ.get(t, UNKNOWN)
        if t not in self.compiled:
            # Box86 runs it: MSVC's ABI keeps ebx, ebp, esi and edi across any
            # call (link-time code generation changes where arguments go, not
            # these). Proven for compiled functions instead; the lane's check
            # compares them with Box86's after every checked call.
            s = Summary(s.reads, set(s.preserved) | {EBX, EBP, ESI, EDI}, s.pops, s.known)
        return s

    def exit_sites(self, f, blocks):
        """A compiled function's exits (issue #115): its indirect calls, its
        imports, its calls and tail jumps to a function left to Box86 that
        may block. Box86 runs those from the call on (aot_exit): a nested run
        can't block, the scheduler can't switch away from it."""
        out = set()
        for site, t in list(f.calls.items()) + list(f.tails.items()):
            if site in f.noreturn and site not in f.exits:
                continue
            if t is None or isinstance(t, tuple):
                out.add(site)
                continue
            if t in self.compiled or t in self.intrinsics or t in ERROR_PATH_FUNCS or t in NORETURN_FUNCS:
                continue
            if t in self.natives and not self.natives[t]["handback"]:
                continue
            if blocks.get(t, True):
                out.add(site)
        return out

    def _thunk_pops(self):
        """What each thunk through a pointer pops (issue #138): a function
        with no ret of its own whose every tail call is through a pointer
        (004baea0: mov ecx, [ecx + 8]; mov eax, [ecx]; mov edx, [eax + 4];
        jmp edx). Its imports' pops where it jumps through the IAT; else
        what its callers push for it (Func.callout_pops), where every call
        site agrees. A guess: the callout checks esp at the return and Box86
        goes on from there where it's wrong. From the functions as decoded,
        before exits cut any call site."""
        img = self.img
        thunks = {}
        for va, f in self.funcs.items():
            if f.refused or f.ret_pops or not f.tail_targets or \
                    not all(t is None or isinstance(t, tuple) for t in f.tail_targets):
                continue
            imp = {img.import_pops.get(t[1]) if isinstance(t, tuple) else -1 for t in f.tail_targets}
            thunks[va] = set() if imp == {-1} else imp
        guess = collections.defaultdict(set)
        for va, f in self.funcs.items():
            if f.refused:
                continue
            for site, t in f.calls.items():
                if isinstance(t, int) and t in thunks and not thunks[t]:
                    guess[t].add(f.callout_pops(site))
            for t in f.tails.values():
                if isinstance(t, int) and t in thunks:
                    guess[t].add(None)   # ours to know from our caller's: not tried
        out = {}
        for va, ps in thunks.items():
            ps = ps or guess.get(va, set())
            if len(ps) == 1 and None not in ps and -1 not in ps:
                out[va] = ps.pop()
        return out

    def make_plain(self, va):
        """A function whose callouts' pops threw its stack off: its indirect
        and import calls plain exits instead. False if they were already."""
        f = self.func(va)
        if f.plain or not f.callouts:
            return False
        self.plain.add(va)
        g = self.funcs[va] = Func(self.img, va, self.starts)
        g.set_exits(f.exits, callouts=False, tail_pops=self.thunk_pops.get(va))
        return True

    def apply_exits(self, blocks):
        """Each compiled function's exits for the compiled set as it is now;
        a function left to Box86 decoded again without them. The number whose
        exits changed (their summaries are then to do again)."""
        if self.thunk_pops is None:   # compile_set sets it first
            self.thunk_pops = self._thunk_pops()
        n = 0
        for va in list(self.funcs):
            f = self.funcs[va]
            if f.refused:
                continue
            want = self.exit_sites(Func(self.img, va, self.starts) if f.exits else f, blocks) \
                if va in self.compiled else set()
            if frozenset(want) == f.exits and f.plain == (va in self.plain):
                continue
            g = self.funcs[va] = Func(self.img, va, self.starts)
            if want:
                g.set_exits(want, callouts=va not in self.plain, tail_pops=self.thunk_pops.get(va))
            n += 1
        # Who may exit: an exit of its own, or a compiled callee (in C) that may.
        me = {va for va in self.compiled if self.func(va).exits}
        changed = True
        while changed:
            changed = False
            for va in self.compiled:
                if va in me:
                    continue
                f = self.func(va)
                if any(isinstance(t, int) and t in me for t in list(f.calls.values()) + list(f.tails.values())):
                    me.add(va)
                    changed = True
        if me != self.may_exit:
            n += 1
        self.may_exit = me
        # Who may exit other than at a callout (a hard exit: the rest of its
        # code cut): its own, or a compiled callee's. One that can't runs in
        # C where there's no trap to exit to (a native's guest_call), its
        # callouts' fallbacks nested runs there.
        hard = {va for va in self.compiled if set(self.func(va).exits) - set(self.func(va).callouts)}
        changed = True
        while changed:
            changed = False
            for va in self.compiled:
                if va in hard:
                    continue
                f = self.func(va)
                if any(isinstance(t, int) and t in hard for t in list(f.calls.values()) + list(f.tails.values())):
                    hard.add(va)
                    changed = True
        self.may_hard = hard
        return n

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
                if not f.rets and not f.exits and all(t in nr for t in f.tails.values()):
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
            if not pops and f.exits and not f.refused:   # every ret behind an exit (issue #138)
                pops = set(f.ret_pops)
            if not pops and not f.refused and va in (self.thunk_pops or {}):   # a thunk through a pointer (#138)
                pops = {self.thunk_pops[va]}
            self.cfg_pops[va] = pops.pop() if len(pops) == 1 else None
            self.summ[va] = Summary(reads=(), preserved=ALL, pops=self.cfg_pops[va],
                                    known=not f.refused and len(pops) <= 1)
        # A function with no ret of its own that only tail-jumps (a thunk:
        # 420290, push ebp; mov ebp, esp; pop ebp; jmp) pops what the
        # functions it jumps to pop, where they agree (issue #138). Its tail
        # jumps' esp is checked against that as any tail call's.
        changed = True
        while changed:
            changed = False
            for va in vas:
                f = self.func(va)
                if self.cfg_pops[va] is not None or f.refused or f.ret_pops or not f.tail_targets or \
                        va in self.noreturn_funcs:
                    continue
                ps = {self.cfg_pops.get(t) if isinstance(t, int) else None for t in f.tail_targets}
                if len(ps) == 1 and None not in ps:
                    self.cfg_pops[va] = ps.pop()
                    self.summ[va] = Summary(reads=(), preserved=ALL, pops=self.cfg_pops[va], known=True)
                    changed = True
        # A worklist, callees first (issue #115): a function is analysed again
        # only when the summary of one it calls changed.
        vas = list(vas)
        inset = set(vas)
        callers = collections.defaultdict(set)
        for va in vas:
            f = self.func(va)
            if not f.refused:
                for t in list(f.calls.values()) + list(f.tails.values()):
                    if isinstance(t, int) and t in inset:
                        callers[t].add(va)
        order = self.callees_first(vas)
        work = collections.deque(order)
        queued = set(order)
        budget = rounds * max(1, len(vas))
        while work:
            budget -= 1
            if budget < 0:
                raise RuntimeError("summaries didn't settle")
            va = work.popleft()
            queued.discard(va)
            f = self.func(va)
            if f.refused or not self.summ[va].known:
                s = UNKNOWN
            else:
                try:
                    a = Analysis(self, f, save_rule=True)
                    s = Summary(a.live_in, a.preserved, None if a.esp_moved else self.cfg_pops[va], True)
                    self.why.pop(va, None)
                except Refused as e:
                    self.why[va] = str(e)
                    s = Summary(ALL, (), self.cfg_pops[va], True) if va in self.noreturn_funcs else UNKNOWN
            if s != self.summ[va]:
                self.summ[va] = s
                for c in callers[va]:
                    if c not in queued:
                        queued.add(c)
                        work.append(c)

    def callees_first(self, vas):
        """The functions in post-order of their direct calls: a callee before
        its callers, where there's no cycle."""
        inset, seen, out = set(vas), set(), []
        for root in sorted(vas):
            if root in seen:
                continue
            stack = [(root, iter(()))]
            seen.add(root)
            f = self.func(root)
            stack[-1] = (root, iter([] if f.refused else list(f.calls.values()) + list(f.tails.values())))
            while stack:
                va, it = stack[-1]
                nxt = next((t for t in it if isinstance(t, int) and t in inset and t not in seen), None)
                if nxt is None:
                    out.append(va)
                    stack.pop()
                    continue
                seen.add(nxt)
                g = self.func(nxt)
                stack.append((nxt, iter([] if g.refused else list(g.calls.values()) + list(g.tails.values()))))
        return out


class Analysis:
    """One function's stack offsets, preserved registers, register liveness
    and flags. Raises Refused for what the spike doesn't take."""

    def __init__(self, prog, f, outs=None, save_rule=False):
        self.p, self.f = prog, f
        self.img = prog.img
        # Where a call may exit (issue #115), Box86 runs the rest of this
        # function from its own code: every save is stored, as the original's.
        if f.entry in prog.may_exit:
            save_rule = False
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
        tail_states = []   # a jmp to another function: an exit through it (issue #115)
        visited = collections.Counter()
        esp_conflict = set()   # where two paths brought different known esps: not known, for good
        self.esp_lost = {}     # va -> why esp stopped being known there (issue #138: the census's causes)
        self.esp_conflict = esp_conflict
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
                before = regs[ESP]
                self._step(i, regs, slots)
                if before is not None and regs[ESP] is None:
                    self.esp_lost.setdefault(va, self._esp_cause(i, before))
                if va in f.rets:
                    ret_states.append((va, dict(regs)))
                if va in f.tails and (va not in f.exits or va in f.callouts):   # a callout comes back (#138)
                    tail_states.append((va, dict(regs)))
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
                        # A library call's garbage on one path is garbage after the
                        # merge (issue #115: else a function returning it on one path
                        # and a value on another returns it, and the check compares it).
                        for k in (ECX, EDX):
                            if ("clob",) in (r0[k], regs[k]):
                                r1[k] = ("clob",)
                        # esp: where one path knows it and another doesn't (a call
                        # whose ret isn't known statically), trust the known one, as
                        # MSVC's balanced stack allows; compiled code checks esp at
                        # each ret at run time.
                        # Where two known esps differed, it stays not known (issue
                        # #115: else a known one coming back flips it, and the walk
                        # never settles).
                        if r0[ESP] is not None and regs[ESP] is not None and r0[ESP] != regs[ESP]:
                            esp_conflict.add(t)
                        if (r0[ESP] is None or regs[ESP] is None) and t not in esp_conflict:
                            r1[ESP] = r0[ESP] or regs[ESP]
                        # Both paths' slot, with one value (a slot not known on one
                        # path isn't, issue #115: get()'s None matched a slot whose
                        # value isn't known, and the walk never settled).
                        s1 = {k: v for k, v in slots.items() if k in s0 and s0[k] == v}
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
        self.ret_states = ret_states
        # Registers that at some ret may hold a library call's garbage: no
        # value of the function's then, by MSVC's ABI (issue #115: one path's
        # memset leftovers made ecx an out, and the check compared them).
        # Through a tail jump: the target's garbage, or ours where it keeps the register.
        def junk(r, va, regs):
            t = self.f.tails[va]
            return r in p.garbage.get(t, ()) or (regs[r] == ("clob",) and r in p.callee_summary(t).preserved)
        self.garbage = {r for r in (ECX, EDX) if (ret_states or tail_states) and
                        (any(regs[r] == ("clob",) for _, regs in ret_states) or
                         any(junk(r, va, regs) for va, regs in tail_states))}
        # esp at a ret known to be something else than the entry's: a helper
        # that moves its caller's frame (MSVC's SEH and EH epilogues: mov esp,
        # ebp from the caller's ebp). Its ret's pops don't tell its caller esp.
        self.esp_moved = any(regs[ESP] is not None and regs[ESP] != ("sp", 0) for _, regs in ret_states)
        for va, regs in ret_states:
            for r in range(8):
                if r != ESP and regs[r] != ("in", r):
                    pre.discard(r)
        # Through a tail jump, a register is kept when it holds its entry
        # value there and the function jumped to keeps it (46a530: a switch of
        # jmps to the string table's lookup, no ret of its own, was taken for
        # one that never returns and kept everything, eax among them).
        tail_keeps = {va: p.callee_summary(self.f.tails[va]).preserved for va, _ in tail_states}
        for va, regs in tail_states:
            for r in range(8):
                if r != ESP and (regs[r] != ("in", r) or r not in tail_keeps[va]):
                    pre.discard(r)
        if not ret_states and not tail_states:
            pre = set(range(8))   # never returns
        # A callee-saved register whose value at a ret is only not known (a
        # slot lost at an indirect call, then popped) and never known to be
        # another: kept, per MSVC's ABI, as for the functions Box86 runs (issue
        # #115: unproven, it cost every compiled caller its frame). The check
        # (D2_AOT=5) compares them with Box86's after every checked call.
        # Not for a helper that moves its caller's frame: it sets ebp on purpose.
        for r in (EBX, EBP, ESI, EDI):
            if not self.esp_moved and all(regs[r] in (("in", r), None) for _, regs in ret_states) and \
                    all(regs[r] in (("in", r), None) and r in tail_keeps[va] for va, regs in tail_states):
                pre.add(r)
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
            if t in p.intrinsics and p.intrinsics[t] in FRAME_NAMES:
                self._frame_helper(p.intrinsics[t], i, regs, slots)
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
            if i.address in self.f.callouts:   # what it's taken to pop, checked at run time
                s = Summary(s.reads, s.preserved, self.f.callouts[i.address], True)
            slots[sp - 4] = ("k", i.address + i.size)
            for r in range(8):
                if r != ESP and r not in s.preserved:
                    regs[r] = None
            # After memset, memcpy or one of our natives, ecx and edx hold what
            # the library's code left (the CRT's paths differ; with the native
            # off, the game's function runs): garbage by MSVC's ABI, and
            # ("clob",) here, so a function returning them as they are leaves
            # them out of its outs (issue #115).
            nat = p.natives.get(t)
            if (t in p.intrinsics and p.intrinsics[t] in ("memset", "memcpy")) or (nat and not nat["handback"]):
                regs[ECX] = regs[EDX] = ("clob",)
            for r in p.garbage.get(t, ()):   # a function that returns them as such (issue #115)
                regs[r] = ("clob",)
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
        for op, n in mem_written(i):
            self._kill_mem(i, op, regs, slots, n)

    def _frame_helper(self, name, i, regs, slots):
        """A call to one of the CRT's frame helpers (issue #138, FRAME_HELPERS),
        esp at the call s (before it pushes its return address)."""
        s = regs[ESP][1]
        ra = ("k", i.address + i.size)
        if name.endswith("prolog4") or name == "eh_prolog3_catch":
            seh = name == "seh_prolog4"
            fp = s + 4 if seh else s           # the caller's new ebp: the slot of its pushed size (seh) or of it (eh)
            n = slots.get(fp if seh else s)
            saved = {EBX: regs[EBX], ESI: regs[ESI], EDI: regs[EDI]}
            old_ebp = regs[EBP]
            for k in list(slots):
                if k <= fp:
                    del slots[k]
            regs[EBP] = ("sp", fp)
            regs[EAX] = ("sp", s - 12)
            slots[fp] = old_ebp
            if not n or n[0] != "k" or n[1] >= 0x10000000:
                regs[ESP] = None
                slots.clear()
                return
            e = s - 12 - n[1]
            slots[e - 4], slots[e - 8], slots[e - 12] = saved[EBX], saved[ESI], saved[EDI]
            regs[ESP] = ("sp", e - 16)
            return
        # An epilog: the registers from the frame, esp the frame's top.
        fp = regs[EBP]
        regs[ECX] = ra
        for r, k in ((EDI, 4), (ESI, 8), (EBX, 12)):
            regs[r] = slots.get(s + k)
        if not fp or fp[0] != "sp":
            regs[EBP] = regs[ESP] = None
            slots.clear()
            return
        regs[EBP] = slots.get(fp[1])
        regs[ESP] = ("sp", fp[1] + 4)
        for k in list(slots):
            if k < fp[1] + 4:
                del slots[k]

    def _esp_cause(self, i, before):
        """Why an instruction made esp not known (issue #138)."""
        m = i.mnemonic
        p = self.p
        if m != "call":
            if m == "leave" or (m == "mov" and i.operands[1].type == X.X86_OP_REG):
                return "%s from a register not known as a frame address" % m
            return "%s of esp" % m
        t = self.f.calls[i.address]
        if t is None:
            return "an indirect call whose pops aren't known"
        if isinstance(t, tuple):
            return "an import whose pops aren't known (%s)" % t[1]
        if t in p.intrinsics:
            return "%s of a size not known" % p.intrinsics[t]
        g = p.func(t)
        if g.refused:
            return "a call to a function the decoder refuses"
        if len(set(g.rets.values())) > 1:
            return "a call to a function whose rets pop different amounts"
        if t in getattr(p, "why", {}):
            return "a call to a function whose own analysis refuses: " + p.why[t]
        s = p.summ.get(t)
        if s is not None and s.known and s.pops is None and getattr(p, "cfg_pops", {}).get(t) is not None:
            return "a call to a helper that moves its caller's frame"
        if not g.rets:
            return "a call to a function with no ret"
        return "a call to a function whose pops aren't known"

    def esp_why(self):
        """Why esp isn't the entry's at a ret (issue #138): the first cause
        on the way, a merge of two different esps, or a ret that pops another
        amount."""
        for va, regs in self.ret_states:
            v = regs[ESP]
            if v is not None and v != ("sp", 0):
                return "esp at the ret at %08x is the entry's %+d" % (va, v[1])
        if self.esp_lost:
            va = min(self.esp_lost)
            return "%s at %08x" % (self.esp_lost[va], va)
        if self.esp_conflict:
            return "two paths bring different esps to %08x" % min(self.esp_conflict)
        return "no cause found"

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
            t = self.f.calls[i.address]
            if t in self.p.intrinsics and self.p.intrinsics[t] in ("seh_epilog4", "eh_epilog3"):
                v = regs[EBP]
                regs[ECX] = ("k", i.address + i.size)
                regs[EBX] = regs[ESI] = regs[EDI] = regs[EBP] = None
                regs[ESP] = ("sp", v[1] + 4) if v and v[0] == "sp" else None
                return
            s = self._callee(t)
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

    def _kill_mem(self, i, op, regs, slots, size=None):
        a = self._addr(i, op, regs)
        if a is not None:
            for k in range(a - 3, a + (size or op.size)):
                slots.pop(k, None)

    def _callee(self, t):
        return self.p.callee_summary(t)

    # ---- register liveness, backward over the CFG ----
    def uses_defs(self, va, outs, skip_saves):
        f, p = self.f, self.p
        i = f.insns[va]
        m = i.mnemonic
        if va in f.exits:        # Box86 goes on from here (issue #115): every register as the original has it
            return P(ALL), P(ALL)
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
            # What the target reads, and what it hands back to our caller
            # untouched: an output of ours it preserves.
            s = self._callee(f.tails[va])
            return P(set(s.reads) | (set(outs) & set(s.preserved)) | {ESP}), P(ALL)
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
        self.index = None   # compiled function -> its row in the functions' table (live)
    def name(self, va):
        return "AOT_SYM(%s_%08x)" % (self.mode, va)

    # Operands
    def reg_get(self, i, rid):
        full = reg_full(i, rid)
        if full is None:   # a segment or system register
            raise Refused("%s %s at %08x" % (i.mnemonic, i.op_str, i.address))
        r, bits, sh = full
        n = REGS[r]
        if bits == 32:
            return n
        if bits == 16:
            return "(%s & 0xffffu)" % n
        if sh == 0:
            return "(%s & 0xffu)" % n
        return "((%s >> 8) & 0xffu)" % n

    def reg_set(self, i, rid, v):
        full = reg_full(i, rid)
        if full is None:   # a segment or system register
            raise Refused("%s %s at %08x" % (i.mnemonic, i.op_str, i.address))
        r, bits, sh = full
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
        if any(self.x87_op(i) for i in f.insns.values()):
            decl.append("AotFpu *const F = aot_fpu;")
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
        return chk + self.ret_outs()

    def call_compiled(self, t, ra, after=True):
        """A call to a compiled function; the return address is pushed.
        after=False (a tail call): esp isn't moved past the callee's ret."""
        if self.mode == "ctx":
            fill = ", ".join(REGS[r] for r in range(8))
            back = " ".join("%s = t.r[%d];" % (REGS[r], r) for r in range(8))
            return "{ AotRegs t = {{%s}}; if (AOT_STALE(%du)) aot_call_stale(0x%08xu, &t); else %s(&t); %s }" % (
                fill, self.row(t), t, self.name(t), back)
        sig = self.sigs[t]
        args = ", ".join(["esp"] + [REGS[r] for r in sig["in"]])
        outs = sig["out"]
        pops = self.p.summ[t].pops
        tail = " esp += %du;" % (4 + pops) if pops is not None and after else ""
        if not outs:
            return "%s(%s);%s" % (self.wrap(t), args, tail)
        if len(outs) == 1:
            return "%s = %s(%s);%s" % (REGS[outs[0]], self.wrap(t), args, tail)
        if len(outs) == 2:
            return "{ uint64_t t = %s(%s); %s = (uint32_t)t; %s = (uint32_t)(t >> 32); }%s" % (
                self.wrap(t), args, REGS[outs[0]], REGS[outs[1]], tail)
        back = " ".join("%s = o[%d];" % (REGS[r], k) for k, r in enumerate(outs))
        return "{ uint32_t o[%d]; %s(%s, o); %s }%s" % (len(outs), self.wrap(t), args, back, tail)

    def row(self, t):
        """t's row in the functions' table (aot_stale's index)."""
        if self.index is None:
            self.index = {v: k for k, v in enumerate(sorted(self.compiled))}
        return self.index[t]

    def wrap(self, t):
        """What a compiled caller calls for t (issue #115): t's C, unless the
        app found t changed since compiled (a native's hook or patch:
        aot_stale), then the native (stale_, cold, out of the way)."""
        return "AOT_SYM(call_%08x)" % t

    def wrappers(self, va):
        """call_ and stale_ for va, before the functions (sig mode): the
        same signature as va's own, so a call site's registers flow the
        same either way."""
        sig = self.sigs[va]
        params = ["uint32_t esp"] + ["uint32_t %s" % REGS[r] for r in sig["in"]]
        args = ["esp"] + [REGS[r] for r in sig["in"]]
        outs = sig["out"]
        if not outs:
            ret = "void"
        elif len(outs) == 1:
            ret = "uint32_t"
        elif len(outs) == 2:
            ret = "uint64_t"
        else:
            ret = "void"
            params.append("uint32_t *o")
            args.append("o")
        fill = ", ".join(REGS[r] if (r == ESP or r in sig["in"]) else "0u" for r in range(8))
        if not outs:
            back = "return;"
        elif len(outs) == 1:
            back = "return t.r[%d];" % outs[0]
        elif len(outs) == 2:
            back = "return (uint64_t)t.r[%d] | (uint64_t)t.r[%d] << 32;" % (outs[0], outs[1])
        else:
            back = " ".join("o[%d] = t.r[%d];" % (k, r) for k, r in enumerate(outs)) + " return;"
        stale = ("static AOT_COLD %s AOT_SYM(stale_%08x)(%s)\n{\n    AotRegs t = {{%s}};\n"
                 "    aot_call_stale(0x%08xu, &t);\n    %s\n}" % (ret, va, ", ".join(params), fill, va, back))
        ret_kw = "" if ret == "void" else "return "
        tail = "\n        return;" if ret == "void" else ""
        call = ("static inline %s AOT_SYM(call_%08x)(%s)\n{\n    if (AOT_STALE(%du)) {\n        %sAOT_SYM(stale_%08x)(%s);%s\n    }\n"
                "    %s%s(%s);\n}" % (ret, va, ", ".join(params), self.row(va), ret_kw, va, ", ".join(args), tail,
                                     ret_kw, self.name(va), ", ".join(args)))
        return stale + "\n" + call

    def call_guest(self, t, va):
        """Box86 runs the callee nested; every register through the context."""
        fill = ", ".join(REGS[r] for r in range(8))
        if self.mode == "sig":
            s = self.p.callee_summary(t)
            back = " ".join("%s = t.r[%d];" % (REGS[r], r) for r in range(8) if r == ESP or r not in s.preserved)
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
        if m in ("bsf", "bsr"):
            # A zero source leaves the destination as it was (as Intel and AMD do).
            a = self.get(i, ops[1])
            r = "__builtin_ctz(a)" if m == "bsf" else "31 - __builtin_clz(a)"
            return "{ uint32_t a = %s; %sif (a) { %s } }" % (a, self.flag_store(va, "a", "0u", "a"),
                                                              self.put(i, ops[0], r))
        if m == "cdq":
            return "edx = (eax & 0x80000000u) ? 0xffffffffu : 0u;"
        if m == "cwde":
            return "eax = (uint32_t)(int32_t)(int16_t)eax;"
        if m in ("cld", "std"):
            return ";"
        if m == "wait" or self.x87_op(i):
            return self.x87(i)
        if m.startswith("rep") or m in ("stosd", "stosb", "stosw", "movsd", "movsb", "movsw"):
            return self.string(i)
        raise Refused("%s at %08x" % (m, va))

    # x87 (issue #136), by opcode and ModR/M rather than Capstone's mnemonics
    # (its fsub/fsubr and fdiv/fdivr forms on the register stack are a known
    # muddle): on the CPU's own state (aot_rt.h's AotFpu), as Box86's dynarec
    # does each one.
    X87_PREFIX = {0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65}

    @staticmethod
    def x87_op(i):
        """(opcode, modrm) of an x87 instruction, else None."""
        b = bytes(i.bytes)
        k = 0
        while k < len(b) and b[k] in Emitter.X87_PREFIX:
            k += 1
        if k + 1 < len(b) and 0xd8 <= b[k] <= 0xdf:
            return b[k], b[k + 1]
        return None

    def x87_next(self, i):
        """The bytes of the instruction after i (Box86 looks at them)."""
        n = self.p.img.insn(i.address + i.size)
        return bytes(n.bytes) if n else b""

    def x87_f2xm1_pair(self, va):
        """f2xm1 at va, then fld1 and faddp st(1): Box86 makes the three one exp2."""
        return all(self.f.insns.get(a) is not None and bytes(self.f.insns[a].bytes) == b
                   for a, b in ((va, b"\xd9\xf0"), (va + 2, b"\xd9\xe8"), (va + 4, b"\xde\xc1")))

    def x87(self, i):
        va = i.address
        if i.mnemonic == "wait":
            return ";"
        op = self.x87_op(i)
        if bytes(i.bytes)[0] in (0x66, 0x67, 0xf2, 0xf3):
            raise Refused("x87 (%s) with a prefix at %08x" % (i.mnemonic, va))
        o, mr = op
        reg = (mr >> 3) & 7
        rm = mr & 7
        G, S = "aot_fget(F, %d)", "aot_fset(F, %d, %s);"
        ARITH = {0: "+", 1: "*", 4: "-", 5: "-r", 6: "/", 7: "/r"}

        def arith(opn, a, b):   # a op b, "-r" and "/r" reversed
            if opn.endswith("r"):
                return "(%s) %s (%s)" % (b, opn[0], a)
            return "(%s) %s (%s)" % (a, opn, b)

        if mr < 0xc0:   # a memory operand
            mem = [x for x in i.operands if x.type == X.X86_OP_MEM]
            a = self.addr(i, mem[0])
            if o in (0xd8, 0xdc, 0xda):
                v = {0xd8: "aot_ldf32(M, %s)", 0xdc: "aot_ldf64(M, %s)",
                     0xda: "(double)(int32_t)LD32(%s)"}[o] % a
                if reg in ARITH:
                    return "{ double v = %s; %s }" % (v, S % (0, arith(ARITH[reg], G % 0, "v")))
                if reg in (2, 3):
                    if o == 0xda:
                        raise Refused("x87 (ficom) at %08x" % va)
                    return "aot_fcom(F, %s, %s);%s" % (G % 0, v, " aot_fpop(F);" if reg == 3 else "")
            if o == 0xd9:
                if reg == 0:
                    return "aot_fpush(F, aot_ldf32(M, %s));" % a
                if reg in (2, 3):
                    return "aot_stf32(M, %s, %s);%s" % (a, G % 0, " aot_fpop(F);" if reg == 3 else "")
                if reg == 5:
                    return "F->cw = (uint16_t)LD16(%s);" % a
                if reg == 7:
                    return "ST16(%s, F->cw);" % a
            if o == 0xdd:
                if reg == 0:
                    return "aot_fpush(F, aot_ldf64(M, %s));" % a
                if reg in (2, 3):
                    return "aot_stf64(M, %s, %s);%s" % (a, G % 0, " aot_fpop(F);" if reg == 3 else "")
                if reg == 7:
                    return "ST16(%s, aot_fnstsw(F));" % a
            if o == 0xdb:
                if reg == 0:
                    return "aot_fpush(F, (double)(int32_t)LD32(%s));" % a
                if reg in (2, 3):
                    return "ST32(%s, aot_fist32(F, %s));%s" % (a, G % 0, " aot_fpop(F);" if reg == 3 else "")
                if reg == 5:
                    n = self.x87_next(i)
                    if len(n) > 1 and n[0] == 0xdb and (n[1] >> 3) & 7 == 7:
                        # Box86 copies the 10 bytes to the fstp's address, nothing pushed, and goes on after it.
                        raise Refused("x87 (fld m80 then fstp m80) at %08x" % va)
                    return "aot_fld80(F, M, %s);" % a
                if reg == 7:
                    return "aot_fstp80(F, M, %s);" % a
            if o == 0xdf:
                if reg == 0:
                    return "aot_fpush(F, (double)(int16_t)LD16(%s));" % a
                if reg in (2, 3):
                    return "ST16(%s, aot_fist16(F, %s));%s" % (a, G % 0, " aot_fpop(F);" if reg == 3 else "")
                if reg == 5:
                    n = self.x87_next(i)
                    if len(n) > 1 and n[0] == 0xdf and (n[1] >> 3) & 7 == 7:
                        raise Refused("x87 (fild m64 then fistp m64) at %08x" % va)
                    return "aot_fild64(F, aot_ld64(M, %s));" % a
                if reg == 7:
                    return "aot_st64(M, %s, aot_fist64(F, %s)); aot_fpop(F);" % (a, G % 0)
            raise Refused("x87 (%s) at %08x" % (i.mnemonic, va))
        # The register stack.
        if (o, mr) == (0xde, 0xc1) and self.x87_f2xm1_pair(va - 4):
            return "; /* in the f2xm1 before it, as Box86 runs it */"
        if o in (0xd8, 0xdc, 0xde) and reg in ARITH:
            if o == 0xd8:   # st0 = st0 op st(i)
                return S % (0, arith(ARITH[reg], G % 0, G % rm))
            # dc, de: st(i) = st(i) op st0, the r forms swapped (dc e0 is fsubr st(i), st0)
            opn = {0: "+", 1: "*", 4: "-r", 5: "-", 6: "/r", 7: "/"}[reg]
            s = S % (rm, arith(opn, G % rm, G % 0))
            return s + (" aot_fpop(F);" if o == 0xde else "")
        if o == 0xd8 and reg in (2, 3):
            return "aot_fcom(F, %s, %s);%s" % (G % 0, G % rm, " aot_fpop(F);" if reg == 3 else "")
        if o == 0xdd and reg in (4, 5):   # fucom, fucomp
            return "aot_fcom(F, %s, %s);%s" % (G % 0, G % rm, " aot_fpop(F);" if reg == 5 else "")
        if (o, mr) in ((0xde, 0xd9), (0xda, 0xe9)):   # fcompp, fucompp
            return "aot_fcom(F, %s, %s); aot_fpop(F); aot_fpop(F);" % (G % 0, G % 1)
        if o == 0xd9 and reg == 0:   # fld st(i)
            return "{ double v = %s; aot_fpush(F, v); }" % (G % rm)
        if o == 0xd9 and reg == 1:   # fxch
            return "{ double a = %s, b = %s; %s %s }" % (G % 0, G % rm, S % (0, "b"), S % (rm, "a"))
        if o == 0xdd and reg in (2, 3):   # fst st(i), fstp st(i)
            return (S % (rm, G % 0) if rm else ";") + (" aot_fpop(F);" if reg == 3 else "")
        if o == 0xdf and mr == 0xe0:   # fnstsw ax
            return "eax = (eax & 0xffff0000u) | aot_fnstsw(F);"
        if o == 0xdb and mr == 0xe2:   # fnclex
            return "F->sw &= (uint16_t)~0x80ffu;"
        if o == 0xd9:
            K = {0xe8: "1.0", 0xe9: "3.3219280948873623", 0xea: "1.4426950408889634", 0xeb: "3.14159265358979323846",
                 0xec: "0.3010299956639812", 0xed: "0.69314718055994531", 0xee: "0.0"}
            if mr in K:
                return "aot_fpush(F, %s);" % K[mr]
            one = {0xe0: "-%s", 0xe1: "__builtin_fabs(%s)", 0xfa: "__builtin_sqrt(%s)"}
            if mr in one:
                return S % (0, one[mr] % (G % 0))
            if mr in (0xfe, 0xff):   # fsin, fcos: C1 C2 cleared
                return "%s F->sw &= (uint16_t)~(C87_C1 | C87_C2);" % (
                    S % (0, "%s(%s)" % ("__builtin_sin" if mr == 0xfe else "__builtin_cos", G % 0)))
            if mr == 0xf1:   # fyl2x
                return "%s aot_fpop(F);" % (S % (1, "(%s) * __builtin_log2(%s)" % (G % 1, G % 0)))
            if mr == 0xfd:   # fscale
                return S % (0, "__builtin_ldexp(%s, aot_cvt_s32(%s))" % (G % 0, G % 1))
            if mr == 0xfc:   # frndint: NaN and zero as they are
                return "{ double v = %s; if (v == v && v != 0.0) %s }" % (G % 0, S % (0, "aot_fround(F, v)"))
            if mr == 0xf5:
                return "aot_fprem1(F);"
            if mr == 0xf0:   # f2xm1; with fld1, faddp st(1) after it, exp2 (Box86 runs those 4 bytes in it)
                if self.x87_f2xm1_pair(va):
                    return S % (0, "__builtin_exp2(%s)" % (G % 0))
                return S % (0, "__builtin_expm1(0.69314718055994531 * %s)" % (G % 0))
            if mr == 0xe8 and self.x87_f2xm1_pair(va - 2):
                return "; /* in the f2xm1 before it, as Box86 runs it */"
        raise Refused("x87 (%s) at %08x" % (i.mnemonic, va))

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
        size = {"b": 1, "w": 2, "d": 4}.get(op[-1])
        if size is None or not op.startswith(("movs", "stos")) or m.startswith(("repe ", "repne ")):
            raise Refused("%s at %08x" % (m, va))
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

    def exit(self, target):
        """Box86 runs on from here (issue #115): every register to the trap
        that entered compiled code, which resumes Box86 at the target; the
        rest of this function, and of each compiled caller above it in C,
        runs in Box86 from the return addresses on the guest stack."""
        return "aot_exit(%s, eax, ecx, edx, ebx, esp, ebp, esi, edi);" % target

    def tail_call(self, i, t):
        """A jmp to another function's entry (issue #115): the target returns
        to our caller for us. Called with esp as at our entry (checked), its
        return address our caller's; what it pops is what we pop, so the
        compiled caller's esp += 4 + pops stays right."""
        va = i.address
        f, p = self.f, self.p
        if va in f.callouts:
            return self.tail_call_out(i)
        if va in f.exits:
            # The target first: an operand may read esp.
            if t is None or isinstance(t, tuple):
                return "{ uint32_t x = %s; %s }" % (self.get(i, i.operands[0]), self.exit("x"))
            return self.exit("0x%08xu" % t)
        if t in p.intrinsics or p.natives.get(t) or isinstance(t, tuple) or t is None:
            raise Refused("tail call at %08x to an import, a native or an intrinsic" % va)
        ours, theirs = p.summ[f.entry].pops, p.callee_summary(t).pops
        if ours is None or theirs is None or ours != theirs:
            raise Refused("tail call at %08x: pops %s, the target's %s" % (va, ours, theirs))
        chk = "if (esp != esp0 || LD32(esp) != ra0) aot_bad_ret(0x%08xu, esp0, esp); " % f.entry
        if t in self.compiled:
            # call_compiled's esp += after the call is dropped: esp is ours to hand back.
            inner = "\n#ifdef AOT_SPLIT\n    %s\n#else\n    %s\n#endif\n    " % (
                self.call_guest(t, va), self.call_compiled(t, va, after=False))
        else:
            inner = self.call_guest(t, va)
        if va in f.noreturn:
            return chk + inner + " aot_unreachable(0x%08xu);" % va
        if self.mode == "ctx":
            # esp after the target's ret, from its context (call_compiled's and call_guest's).
            sets = " ".join("R->r[%d] = %s;" % (r, REGS[r]) for r in range(8) if r != ESP)
            return chk + inner + " %s R->r[4] = esp; return;" % sets
        return chk + inner + " " + self.ret_outs()

    def ret_outs(self):
        """A sig function's return of its outputs."""
        outs = self.sig["out"]
        if not outs:
            return "return;"
        if len(outs) == 1:
            return "return %s;" % REGS[outs[0]]
        if len(outs) == 2:
            return "return (uint64_t)%s | (uint64_t)%s << 32;" % (REGS[outs[0]], REGS[outs[1]])
        return " ".join("o[%d] = %s;" % (k, REGS[r]) for k, r in enumerate(outs)) + " return;"

    def tail_call_out(self, i):
        """A tail call through a pointer (issue #138: jmp eax, jmp [vtable +
        n], an import's thunk) as a callout with our caller's return address:
        what our rets pop is what it's taken to pop (checked by aot_call_out,
        which exits to that return address where it isn't). Where esp isn't
        our entry's, it isn't a tail call: Box86 goes on at the target."""
        va = i.address
        f, p = self.f, self.p
        pops = p.summ[f.entry].pops
        if pops is None or pops != f.callouts[va]:
            raise Refused("tail call through a pointer at %08x: pops %s, the rets' %s" % (va, pops, f.callouts[va]))
        s = ("{ uint32_t x = %s; if (esp != esp0 || LD32(esp) != ra0) { %s } "
             "AotRegs t = {{eax, ecx, edx, ebx, esp, ebp, esi, edi}}; aot_call_out(x, &t, %du); "
             "eax = t.r[0]; ecx = t.r[1]; edx = t.r[2]; esp = t.r[4]; } "
             % (self.get(i, i.operands[0]), self.exit("x"), pops))
        if self.mode == "ctx":
            sets = " ".join("R->r[%d] = %s;" % (r, REGS[r]) for r in range(8) if r != ESP)
            return s + "%s R->r[4] = esp; return;" % sets
        return s + self.ret_outs()

    def frame_helper(self, name, ra):
        """The CRT's frame helpers in line (issue #138, FRAME_HELPERS): the
        same stores, in the same places, as their code; fs:[0] the SEH chain."""
        k = self.p.frame_consts[name] if name in self.p.frame_consts else {}
        fs0 = "(uint32_t)(aot_fs_base() + 0x0u)"
        if name == "seh_prolog4":
            return ("{ uint32_t s = esp, n = LD32(s + 4u), e; ST32(s - 4u, 0x%08xu); ST32(s - 8u, 0x%08xu); "
                    "ST32(s - 12u, LD32(%s)); ST32(s + 4u, ebp); ebp = s + 4u; e = s - 12u - n; "
                    "ST32(e - 4u, ebx); ST32(e - 8u, esi); ST32(e - 12u, edi); "
                    "{ uint32_t c = LD32(0x%08xu); ST32(ebp - 4u, LD32(ebp - 4u) ^ c); ST32(e - 16u, c ^ ebp); } "
                    "esp = e - 16u; ST32(ebp - 0x18u, esp); ST32(esp - 4u, LD32(ebp - 8u)); "
                    "ST32(ebp - 8u, LD32(ebp - 4u)); ST32(ebp - 4u, 0xfffffffeu); eax = ebp - 0x10u; "
                    "ST32(%s, eax); }" % (ra, k["h"], fs0, k["c"], fs0))
        if name == "eh_prolog3_catch":
            return ("{ uint32_t s = esp, n = LD32(s), e; ST32(s - 4u, 0x%08xu); ST32(s - 8u, eax); "
                    "ST32(s - 12u, LD32(%s)); e = s - 12u - n; "
                    "ST32(e - 4u, ebx); ST32(e - 8u, esi); ST32(e - 12u, edi); ST32(s, ebp); ebp = s; "
                    "ST32(e - 16u, LD32(0x%08xu) ^ ebp); esp = e - 16u; ST32(ebp - 0x10u, esp); "
                    "ST32(esp - 4u, LD32(ebp - 4u)); ST32(ebp - 4u, 0xffffffffu); eax = ebp - 0xcu; "
                    "ST32(%s, eax); }" % (ra, fs0, k["c"], fs0))
        off = 0x10 if name == "seh_epilog4" else 0xc
        return ("{ uint32_t s = esp, f = ebp; ST32(s - 4u, 0x%08xu); ST32(%s, LD32(f - 0x%xu)); ecx = 0x%08xu; "
                "edi = LD32(s + 4u); esi = LD32(s + 8u); ebx = LD32(s + 12u); ebp = LD32(f); ST32(f, ecx); "
                "esp = f + 4u; }" % (ra, fs0, off, ra))

    def call(self, i):
        va = i.address
        f, p = self.f, self.p
        tail = va in f.tails
        t = f.tails[va] if tail else f.calls[va]
        ra = va + i.size
        if tail:
            return self.tail_call(i, t)
        push = "esp -= 4u; ST32(esp, 0x%08xu); " % ra
        if va in f.callouts:
            # Our shim or a compiled function, in C, when the target is one
            # (aot_call_out): else, or where esp or a callee-saved register
            # isn't what it's taken to be after, Box86 goes on from there.
            return ("{ uint32_t x = %s; %sAotRegs t = {{eax, ecx, edx, ebx, esp, ebp, esi, edi}}; "
                    "aot_call_out(x, &t, %du); eax = t.r[0]; ecx = t.r[1]; edx = t.r[2]; esp = t.r[4]; }"
                    % (self.get(i, i.operands[0]), push, f.callouts[va]))
        if va in f.exits:
            # The target first: an operand may read esp.
            x = self.get(i, i.operands[0]) if t is None or isinstance(t, tuple) else "0x%08xu" % t
            return "{ uint32_t x = %s; %s%s }" % (x, push, self.exit("x"))
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
            if name in FRAME_NAMES:
                return self.frame_helper(name, ra)
            raise Refused("intrinsic %s" % name)
        if isinstance(t, tuple) or t is None:
            raise Refused("an import or indirect call at %08x" % va)
        s = push
        nat = p.natives.get(t)
        if t in self.compiled:
            # AOT_SPLIT (the lane, issue #115): every call to a compiled
            # function through Box86 instead, which enters it as Box86's own
            # callers do (hooked, or run by Box86 when it isn't): a fault
            # found by halves among the hooked functions is then one
            # function's own code, not a call between two.
            inner = "\n#ifdef AOT_SPLIT\n    %s\n#else\n    %s\n#endif\n    " % (
                self.call_guest(t, va), self.call_compiled(t, ra))
        else:
            inner = self.call_guest(t, va)
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

# The CRT's frame helpers (issue #138): they build or tear down their
# caller's SEH or C++ EH frame, so esp after the call is the caller's frame,
# not what a ret pops. Compiled in line (Emitter.frame_helper, modelled in
# Analysis._frame_helper), each only where its code is this, instruction by
# instruction; {h} is the handler, {c} the security cookie's address.
_EPILOG = ["mov ecx, dword ptr [ebp - {off}]", "mov dword ptr fs:[0], ecx", "pop ecx", "pop edi", "pop edi",
           "pop esi", "pop ebx", "mov esp, ebp", "pop ebp", "push ecx", "ret "]
FRAME_NAMES = ("seh_prolog4", "seh_epilog4", "eh_prolog3_catch", "eh_epilog3")
FRAME_HELPERS = {
    0x00687fb0: ("seh_prolog4", [
        "push {h}", "push dword ptr fs:[0]", "mov eax, dword ptr [esp + 0x10]", "mov dword ptr [esp + 0x10], ebp",
        "lea ebp, [esp + 0x10]", "sub esp, eax", "push ebx", "push esi", "push edi", "mov eax, dword ptr [{c}]",
        "xor dword ptr [ebp - 4], eax", "xor eax, ebp", "push eax", "mov dword ptr [ebp - 0x18], esp",
        "push dword ptr [ebp - 8]", "mov eax, dword ptr [ebp - 4]", "mov dword ptr [ebp - 4], 0xfffffffe",
        "mov dword ptr [ebp - 8], eax", "lea eax, [ebp - 0x10]", "mov dword ptr fs:[0], eax", "ret "]),
    0x00687ff5: ("seh_epilog4", [x.replace("{off}", "0x10") for x in _EPILOG]),
    0x0069d4df: ("eh_prolog3_catch", [
        "push eax", "push dword ptr fs:[0]", "lea eax, [esp + 0xc]", "sub esp, dword ptr [esp + 0xc]", "push ebx",
        "push esi", "push edi", "mov dword ptr [eax], ebp", "mov ebp, eax", "mov eax, dword ptr [{c}]",
        "xor eax, ebp", "push eax", "mov dword ptr [ebp - 0x10], esp", "push dword ptr [ebp - 4]",
        "mov dword ptr [ebp - 4], 0xffffffff", "lea eax, [ebp - 0xc]", "mov dword ptr fs:[0], eax", "ret "]),
    0x0069d584: ("eh_epilog3", [x.replace("{off}", "0xc") for x in _EPILOG]),
}


def frame_helper_consts(img, va, want):
    """{h: handler, c: cookie address} where the code at va is want, else None."""
    got = {}
    try:
        for w in want:
            i = img.insn(va)
            text = "%s %s" % (i.mnemonic, i.op_str)
            pat = "^" + re.escape(w).replace(re.escape("{h}"), "(?P<h>0x[0-9a-f]+)").replace(
                re.escape("{c}"), "(?P<c>0x[0-9a-f]+)") + "$"
            m = re.match(pat, text)
            if not m:
                return None
            got.update({k: int(v, 16) for k, v in m.groupdict().items()})
            va += i.size
    except Exception:
        return None
    return got


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


def rel32_targets(img, want):
    """Of the addresses in `want`, those a call, jmp or jcc with a 32-bit
    displacement anywhere in .text could reach: a raw sweep, so it
    over-approximates (a byte sequence that only looks like one counts too)."""
    hit = set()
    lo = img.text_lo - img.base
    mem = img.mem[lo:lo + (img.text_hi - img.text_lo)]
    for k in range(len(mem) - 5):
        b = mem[k]
        if b in (0xE8, 0xE9):
            n = k + 5
            d = int.from_bytes(mem[k + 1:k + 5], "little", signed=True)
        elif b == 0x0F and 0x80 <= mem[k + 1] <= 0x8F and k + 6 <= len(mem):
            n = k + 6
            d = int.from_bytes(mem[k + 2:k + 6], "little", signed=True)
        else:
            continue
        t = (img.text_lo + n + d) & 0xffffffff
        if t in want:
            hit.add(t)
    return hit


def leaves(prog, compiled):
    """The compiled functions whose C, with every compiled callee's, never
    calls Box86 (issue #115): no nested run, so Box86 can enter them in line
    (the backend's in-line shim) without leaving its run. The game's
    fatal-error and exit paths count as calls to Box86."""
    def direct(va):
        f = prog.func(va)
        for site, t in list(f.calls.items()) + list(f.tails.items()):
            if t in compiled or t in prog.intrinsics or (t in prog.natives and not prog.natives[t]["handback"]):
                continue
            return True
        return False
    out = {va for va in compiled if not direct(va)}
    changed = True
    while changed:
        changed = False
        for va in sorted(out):
            f = prog.func(va)
            if any(isinstance(t, int) and t in compiled and t not in out
                   for t in list(f.calls.values()) + list(f.tails.values())):
                out.discard(va)
                changed = True
    return out


def may_block(img, prog, vas):
    """Of the functions in vas, those that may block their thread when Box86
    runs them nested for compiled code (issue #115): a call to a blocking
    import, an indirect call (whatever it reaches), a function not decoded,
    or a call to one that may block. Calls to the fatal-error report and the
    game's exit don't count: those paths end the game."""
    out = {}
    def direct(va):
        f = prog.func(va)
        if f.refused:
            return True
        for site, t in list(f.calls.items()) + list(f.tails.items()):
            if (site in f.noreturn and site not in f.exits) or t in ERROR_PATH_FUNCS or t in NORETURN_FUNCS:
                continue
            if t is None:
                return True
            if isinstance(t, tuple):
                if img.imports.get(t[1], ("", ""))[1].startswith(BLOCKING_IMPORTS):
                    return True
        return False
    work = sorted(vas)
    for va in work:
        out[va] = direct(va)
    changed = True
    while changed:
        changed = False
        for va in work:
            if out[va]:
                continue
            f = prog.func(va)
            for site, t in list(f.calls.items()) + list(f.tails.items()):
                if (site in f.noreturn and site not in f.exits) or not isinstance(t, int) or t in ERROR_PATH_FUNCS \
                        or t in NORETURN_FUNCS:
                    continue
                if t in prog.intrinsics or (t in prog.natives and not prog.natives[t]["handback"]):
                    continue
                if out.get(t, True):
                    out[va] = changed = True
                    break
    return out


def flags_read_after(img, vas, steps=12):
    """Of vas, the functions whose flags a caller may read after they return
    (issue #115): compiled code hands Box86 its registers, not its flags.
    Each direct call (a raw sweep of .text) is followed: an instruction that
    reads a flag before one writes it counts, and so does a call straight on
    to a function that reads flags on entry (the CRT's math helpers: 6987d8
    tests a double's exponent, 6879dd's first jump reads the result). A ret
    or a jump ends the search: MSVC's C passes no flags through them."""
    def reads_first(va, depth):
        for _ in range(steps):
            i = img.insn(va)
            d, u = flags_rw(i)
            if u:
                return True
            if i.mnemonic == "call":
                op = i.operands[0]
                if depth == 0 and op.type == X.X86_OP_IMM and reads_first(op.imm & 0xffffffff, 1):
                    return True
                return False     # a call: its callee's flags, not ours, from here
            if d or i.mnemonic.startswith(("ret", "jmp")) or capstone.CS_GRP_JUMP in i.groups:
                return False
            va += i.size
        return False
    hit = {}
    lo = img.text_lo - img.base
    mem = img.mem[lo:lo + (img.text_hi - img.text_lo)]
    for k in range(len(mem) - 5):
        if mem[k] != 0xE8:
            continue
        t = (img.text_lo + k + 5 + int.from_bytes(mem[k + 1:k + 5], "little", signed=True)) & 0xffffffff
        if t not in vas or t in hit:
            continue
        try:
            if reads_first(img.text_lo + k + 5, 0):
                hit[t] = img.text_lo + k
        except Refused:
            pass    # not code after all (the sweep over-approximates)
    return hit


def code_ranges(f):
    """A function's instructions as [lo, hi) runs of bytes, in order."""
    ranges = []
    for a in f.order:
        e = a + f.insns[a].size
        if ranges and ranges[-1][1] == a:
            ranges[-1][1] = e
        else:
            ranges.append([a, e])
    return ranges


def code_hash(img, ranges):
    """FNV-1a over those bytes, as aot_spike.cpp's code_hash() takes it."""
    h = 0x811c9dc5
    for lo, hi in ranges:
        for b in img.mem[lo - img.base:hi - img.base]:
            h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h


def hookable(img, prog, compiled, sweep):
    """Each compiled function's entry, for the check (issue #114): (keep,
    head) when its first whole instructions (5 bytes or more) can go to a
    trampoline and a jump can take their place, else the reason why not:
    a branch among them, a ret whose pops aren't known, or a branch from
    anywhere into the bytes the jump would cover."""
    heads, why = {}, {}
    for va in sorted(compiled):
        try:
            keep, head = root_head(img, va)
        except Refused as e:
            why[va] = str(e)
            continue
        if prog.summ[va].pops is None:
            why[va] = "its ret pops aren't known"
            continue
        heads[va] = (keep, head)
    inner = {va + k: va for va, (keep, _) in heads.items() for k in range(1, keep)}
    region = {**inner, **{va: va for va in heads}}   # the entry too: its own fall-through is no branch
    for f in prog.funcs.values():
        for src, succ in f.succ.items():
            i = f.insns.get(src)
            for t in succ:
                if t in inner and not (i is not None and t == src + i.size and region.get(src) == inner[t]):
                    why[inner[t]] = "a branch from %08x into its first bytes (%08x)" % (src, t)
        for src, t in list(f.calls.items()) + list(f.tails.items()):
            if isinstance(t, int) and t in inner:
                why[inner[t]] = "a call or jump from %08x into its first bytes (%08x)" % (src, t)
    for t in sweep(set(inner)):
        why.setdefault(inner[t], "a 32-bit branch somewhere in .text could reach its first bytes (%08x)" % t)
    return {va: h for va, h in heads.items() if va not in why}, why


class Progress:
    """A line on stderr every AOT_PROGRESS seconds (30; 0: off) in the long
    loops, so a big set (the census, #135) shows how far it is."""
    every = float(os.environ.get("AOT_PROGRESS", "30"))

    def __init__(self, t0):
        self.t0 = t0
        self.last = time.time()

    def __call__(self, what, done, total=None):
        now = time.time()
        if self.every <= 0 or now - self.last < self.every:
            return
        self.last = now
        of = " of %d (%.0f%%)" % (total, 100.0 * done / total) if total else ""
        print("aot: %s: %d%s, %.0f s" % (what, done, of, now - self.t0), file=sys.stderr, flush=True)


def compile_set(img, starts, roots, extra, mode_list, outdir, names=None, keep=None):
    names = names or {}
    t0 = time.time()
    progress = Progress(t0)
    prog = Program(img, starts | set(roots) | set(extra), NATIVES, INTRINSICS)
    if keep is not None:
        keep["prog"] = prog
    want = list(roots) + [v for v in extra if v not in roots]
    reach = prog.closure(want) | set(want)   # the listed functions always, whatever the limit cut
    print("aot: %d functions to compile, %d with their callees, %.0f s" % (len(want), len(reach), time.time() - t0),
          file=sys.stderr, flush=True)
    compiled = prog.compiled
    refused = {}
    # A thunk through a pointer whose pops nothing tells (issue #138: no ret
    # of its own, a tail call through a pointer among its ends): its
    # jmp would always be a plain exit, and its callers would lose esp after
    # calling it in C (no ret of its own); left to Box86, a call to it is an
    # exit, as before. Refused as its decoding would be, so its summary is
    # not known either (else a known one with no pops cost its callers esp).
    prog.thunk_pops = prog._thunk_pops()
    for va in reach:
        f = prog.func(va)
        ptr = sorted(k for k, t in f.tails.items() if t is None or isinstance(t, tuple))
        if not f.refused and not f.ret_pops and ptr and va not in prog.thunk_pops:
            f.refused = "a thunk through a pointer at %08x whose pops aren't known" % ptr[0]
    for va in want:
        f = prog.func(va)
        if f.refused:
            refused[va] = f.refused
            continue
        compiled.add(va)
    # A call that may block, an import or an indirect call: Box86 runs on from
    # it (issue #115: an exit, Program.exit_sites), as code left to Box86 can
    # block where a nested run can't. Again each time the compiled set shrinks:
    # a function the analysis, the signatures or the emitter refuse later
    # becomes a Box86 callee of its compiled callers then (6bead0, a lock).
    blocks = may_block(img, prog, reach)
    prog.apply_exits(blocks)
    prog.summarize(sorted(reach))
    # What the analysis refuses whatever the signatures, screened once (issue
    # #115: a refusal inside the loops below starts them again, which over
    # hundreds of functions was most of the run).
    # Callees first: a caller returning a callee's garbage as it is leaves it out too.
    garbage = prog.garbage
    screen = prog.callees_first(sorted(compiled))
    for n, va in enumerate(screen):
        progress("screened", n, len(screen))
        for attempt in (0, 1):
            try:
                a = Analysis(prog, prog.func(va), outs={EAX, ECX, EDX, EBX, EBP, ESI, EDI}, save_rule=True)
                if not a.esp_ok:
                    raise Refused("esp at a ret isn't known to be the entry's: " + a.esp_why())
                Flags(prog.func(va))
                garbage[va] = a.garbage
                # Made plain (attempt 1): its summary was made with its
                # callouts' guessed pops, which may have moved esp at its ret
                # (its pops lost) or lost a callee-saved register; its
                # callers, screened next, need it as summarize would make it
                # now (issue #138: 688b46's TlsGetValue, taken to pop 8,
                # cost 688bb4; 405c30 lost ebp the same way).
                if attempt:
                    s2 = Analysis(prog, prog.func(va), save_rule=True)
                    prog.summ[va] = Summary(s2.live_in, s2.preserved,
                                            None if s2.esp_moved else prog.cfg_pops.get(va), True)
                break
            except Refused as e:
                if attempt == 0 and str(e).startswith("esp at a ret") and prog.make_plain(va):
                    continue
                refused[va] = str(e)
                break
    for va, site in flags_read_after(img, compiled).items():
        refused[va] = "its flags are read after the call at %08x" % site
    compiled -= set(refused)
    prog.apply_exits(blocks)
    prog.summarize(sorted(reach))
    rounds = 0
    while True:
        rounds += 1
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
                # Box86 enters every compiled function it calls (issue #115:
                # each is hooked), so each has its outs for Box86's callers:
                # eax, ecx and edx (edx:eax for a 64-bit result; ecx, which
                # the game's own conventions sometimes return in) and the
                # callee-saved registers it doesn't keep.
                # Not ecx or edx where they're only a library call's garbage
                # at every ret (Box86 keeps its own then, as the ABI allows).
                outs = {va: ({EAX, ECX, EDX} - garbage.get(va, set())) |
                        ({EBX, EBP, ESI, EDI} - prog.summ[va].preserved) for va in compiled}
                # A worklist (issue #115): ins and outs only grow from nothing,
                # so a function goes back on it when a compiled callee's ins
                # grew (its callers) or its outs did (itself).
                callers = collections.defaultdict(set)
                for va in compiled:
                    g = prog.func(va)
                    for t in list(g.calls.values()) + list(g.tails.values()):
                        if isinstance(t, int) and t in compiled:
                            callers[t].add(va)
                order = prog.callees_first(sorted(compiled))
                work, queued = collections.deque(order), set(order)
                budget = 100 * max(1, len(compiled))
                settled = 0
                # A pass's refusals together (issue #135: one at a time, each
                # starting the whole worklist again, took hours on d2d's list).
                # A function whose direct callee was refused in this pass and
                # that is refused itself is only put off: the next pass, with
                # that callee left to Box86, decides it.
                hit, put_off = {}, set()
                while work:
                    settled += 1
                    progress("signatures, round %d: %d analysed, %d refused this pass, still queued" % (
                        rounds, settled, len(hit)), len(work))
                    budget -= 1
                    if budget < 0:
                        raise RuntimeError("signatures didn't settle")
                    va = work.popleft()
                    queued.discard(va)
                    if va in hit:
                        continue
                    f = prog.func(va)
                    try:
                        a = Analysis(prog, f, outs=outs[va], save_rule=True)
                        if not a.esp_ok:
                            raise Refused("esp at a ret isn't known to be the entry's: " + a.esp_why())
                        Flags(f)
                    except Refused as e:
                        if any(isinstance(t, int) and t in hit for t in list(f.calls.values()) + list(f.tails.values())):
                            put_off.add(va)
                        else:
                            hit[va] = str(e)
                        continue
                    put_off.discard(va)
                    s = prog.summ[va]
                    if frozenset(a.live_in) != s.reads:
                        prog.summ[va] = Summary(a.live_in, s.preserved, s.pops, True)
                        for c in callers[va]:
                            if c not in queued:
                                queued.add(c)
                                work.append(c)
                    # A compiled tail target returns our outs for us: what it doesn't keep, it must hand back.
                    for site, t in list(f.calls.items()) + list(f.tails.items()):
                        if t in compiled:
                            live = outs[va] if site in f.tails else a.live_out[site]
                            # Not its garbage (issue #115: an exit site reads every
                            # register, a library call's leftovers too; Box86's own are as good).
                            add = set(live) - prog.summ[t].preserved - {ESP} - set(garbage.get(t, ()))
                            if not add <= outs[t]:
                                outs[t] |= add
                                if t not in queued:
                                    queued.add(t)
                                    work.append(t)
                if not hit and not put_off:
                    break
                if not hit:   # put off with nothing refused: can't happen, but never loop on it
                    raise RuntimeError("signatures: %d functions put off with no refusal" % len(put_off))
                for va, why in hit.items():
                    if why.startswith("esp at a ret") and prog.make_plain(va):
                        continue
                    refused[va] = why
                    compiled.discard(va)
                print("aot: signatures, round %d: a pass refused %d (%d put off), %.0f s" % (
                    rounds, len(hit), len(put_off), time.time() - t0), file=sys.stderr, flush=True)
                prog.apply_exits(blocks)
                prog.summarize(sorted(reach))   # the summaries as for functions left to Box86
            except Refused as e:   # from outside a function's own analysis
                raise RuntimeError("signatures: %s" % e)
        sigs = {va: {"in": sorted(prog.summ[va].reads - {ESP}), "out": sorted(outs[va]),
                     "preserved": sorted(prog.summ[va].preserved)} for va in compiled}
        # Every function emitted once, both ways, before writing anything: what
        # the emitter refuses goes to Box86, and the signatures again.
        # All of a round's refusals at once, then the signatures again.
        bad = {}
        for mode in mode_list:
            em = Emitter(prog, compiled, mode, sigs)
            for n, va in enumerate(sorted(compiled)):
                progress("emitter check, round %d, %s" % (rounds, mode), n, len(compiled))
                if va in bad:
                    continue
                try:
                    em.ana = Analysis(prog, prog.func(va), outs=set(sigs[va]["out"]), save_rule=(mode == "sig"))
                    em.function(va)
                except Refused as e:
                    bad[va] = str(e)
        if not bad:
            break
        refused.update(bad)
        compiled -= set(bad)
        prog.apply_exits(blocks)
        prog.summarize(sorted(reach))
        print("aot: round %d: %d refused by the emitter, %.0f s" % (rounds, len(bad), time.time() - t0),
              file=sys.stderr)
    # Each compiled function's own analysis with its outs, then the C.
    t1 = time.time()
    head = ['/* Generated by tools/aot/recompile.py from Game.exe: never commit it (docs/aot.md). */',
            '#include "game/aot_rt.h"', ""]
    chunks = list(head)
    report = {"functions": {}, "refused": {"%08x" % k: v for k, v in refused.items()}, "fallbacks": {}}
    # The functions' C goes into parts (issue #135): aot_spike_NN.c, each
    # up to AOT_PART_MB of C, so that no one file takes more of gcc's memory
    # than a lane's -j2 has (the coverage set's 44 MB file took over 4 GB),
    # in the order that leaves the fewest calls across parts (gcc inlines a
    # callee only within its file; the orders are below). aot_spike.c keeps
    # the tables and the roots.
    texts = collections.defaultdict(dict)   # va -> mode -> its C
    ems = {}
    for mode in mode_list:
        em = ems[mode] = Emitter(prog, compiled, mode, sigs)
        chunks += [em.proto(va) + ";" for va in sorted(compiled)] + [""]
        for n, va in enumerate(sorted(compiled)):
            progress("writing the C, %s" % mode, n, len(compiled))
            f = prog.func(va)
            em.ana = Analysis(prog, f, outs=set(sigs[va]["out"]), save_rule=(mode == "sig"))
            texts[va][mode] = em.function(va)
    part_max = int(float(os.environ.get("AOT_PART_MB", "8")) * 1e6)
    callees_of = {va: {t for t in list(prog.func(va).calls.values()) + list(prog.func(va).tails.values())
                       if isinstance(t, int) and t in compiled} for va in compiled}

    def cut(order):
        parts, cur, size = [], [], 0
        for va in order:
            n = sum(len(x) for x in texts[va].values())
            if cur and size + n > part_max:
                parts.append(cur)
                cur, size = [], 0
            cur.append(va)
            size += n
        if cur:
            parts.append(cur)
        where = {va: k for k, part in enumerate(parts) for va in part}
        return sum(1 for va in compiled for t in callees_of[va] if where[t] != where[va]), parts

    def clusters():
        """Functions merged along their calls, the pairs with the most call
        sites between them first, while a cluster fits in a part; the
        clusters then packed into parts, largest first."""
        size = {va: sum(len(x) for x in texts[va].values()) for va in compiled}
        w = collections.Counter()
        for va in compiled:
            g = prog.func(va)
            for t in list(g.calls.values()) + list(g.tails.values()):
                if isinstance(t, int) and t in compiled and t != va:
                    w[(min(va, t), max(va, t))] += 1
        up = {va: va for va in compiled}
        csize = dict(size)
        members = {va: [va] for va in compiled}

        def find(x):
            while up[x] != x:
                up[x] = up[up[x]]
                x = up[x]
            return x
        for (a, b), _ in sorted(w.items(), key=lambda kv: (-kv[1], kv[0])):
            ra, rb = find(a), find(b)
            if ra == rb or csize[ra] + csize[rb] > part_max:
                continue
            if csize[ra] < csize[rb]:
                ra, rb = rb, ra
            up[rb] = ra
            csize[ra] += csize.pop(rb)
            members[ra] += members.pop(rb)
        bins = []   # [size, [va...]]
        for r in sorted(members, key=lambda r: (-csize[r], r)):
            for b in bins:
                if b[0] + csize[r] <= part_max:
                    b[0] += csize[r]
                    b[1] += members[r]
                    break
            else:
                bins.append([csize[r], list(members[r])])
        order = [va for b in bins for va in sorted(b[1])]
        return order

    # Of three orders, the one with fewer calls across parts: by address,
    # the call graph's post-order (each function after its callees), or
    # clusters along the calls.
    cands = {"address": cut(sorted(compiled)), "post-order": cut(prog.callees_first(sorted(compiled))),
             "clusters": cut(clusters())}
    order_name = min(cands, key=lambda k: (cands[k][0], len(cands[k][1])))
    print("aot: calls across parts by order: %s" % ", ".join("%s %d (%d parts)" % (k, v[0], len(v[1]))
                                                         for k, v in cands.items()), file=sys.stderr, flush=True)
    _, parts = cands[order_name]
    parts = [sorted(part) for part in parts]
    part_of = {va: k for k, part in enumerate(parts) for va in part}
    cross = local = 0
    part_texts = []
    for k, part in enumerate(parts):
        out = list(head)
        callees = set()
        for va in part:
            g = prog.func(va)
            for t in list(g.calls.values()) + list(g.tails.values()):
                if isinstance(t, int) and t in compiled:
                    callees.add(t)
                    if part_of[t] == k:
                        local += 1
                    else:
                        cross += 1
        need = sorted(set(part) | callees)
        for mode in mode_list:
            em = ems[mode]
            out += [em.proto(va) + ";" for va in need] + [""]
            if mode == "sig":
                out += [em.wrappers(va) for va in sorted(callees)] + [""]
            for va in part:
                out += [texts[va][mode], ""]
        part_texts.append(out)
    report["parts"] = {"files": len(parts), "calls_local": local, "calls_across": cross, "order": order_name}
    for mode in mode_list:
        em = ems[mode]
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
    # The functions' table (issues #114, #115), in every build: every
    # compiled function with a register entry of its own, the registers its
    # compiled callers read after it (out) and those it keeps (preserved),
    # and where it can be hooked. The app hooks each from it (aot_funcs);
    # the check uses the journaled build's (aotj_funcs).
    heads, why = hookable(img, prog, compiled, lambda w: rel32_targets(img, w))
    if "sig" in mode_list:
        em = Emitter(prog, compiled, "sig", sigs)
        for va in sorted(compiled):
            sig = sigs[va]
            args = ", ".join(["r[4]"] + ["r[%d]" % x for x in sig["in"]])
            outs_ = sig["out"]
            if len(outs_) == 1:
                body = "r[%d] = %s(%s);" % (outs_[0], em.name(va), args)
            elif len(outs_) == 2:
                body = "uint64_t t = %s(%s); r[%d] = (uint32_t)t; r[%d] = (uint32_t)(t >> 32);" % (
                    em.name(va), args, outs_[0], outs_[1])
            else:
                body = "uint32_t o[8]; %s(%s%s); %s" % (em.name(va), args, ", o" if outs_ else "",
                                                        " ".join("r[%d] = o[%d];" % (x, k) for k, x in enumerate(outs_)))
            chunks.append("static uint32_t AOT_SYM(fn_%08x)(uint32_t *r) { %s return r[0]; }" % (va, body))
        # Each function's code as compiled and its compiled callees (issue
        # #115): the app leaves to Box86 what a native's patch changed since.
        index = {va: k for k, va in enumerate(sorted(compiled))}
        for va in sorted(compiled):
            f = prog.func(va)
            rg = code_ranges(f)
            cl = sorted({index[t] for t in list(f.calls.values()) + list(f.tails.values())
                         if isinstance(t, int) and t in compiled})
            chunks.append("static const uint32_t AOT_SYM(rg_%08x)[] = {%s};" % (
                va, ", ".join("0x%08xu, 0x%08xu" % (lo, hi) for lo, hi in rg)))
            if cl:
                chunks.append("static const uint16_t AOT_SYM(cl_%08x)[] = {%s};" % (va, ", ".join(map(str, cl))))
        leaf = leaves(prog, compiled)
        report["leaves"] = len(leaf)
        report["x87"] = sum(1 for va in compiled if any(Emitter.x87_op(i) for i in prog.func(va).insns.values()))
        report["exits"] = {"functions": sum(1 for va in compiled if prog.func(va).exits),
                           "sites": sum(len(prog.func(va).exits) for va in compiled),
                           "may_exit": len(prog.may_exit & compiled),
                           "callouts": sum(len(prog.func(va).callouts) for va in compiled),
                           "plain": len(prog.plain & compiled)}
        chunks.append("const AotFunc AOT_SYM(funcs)[] = {")
        for va in sorted(compiled):
            keep, head = heads.get(va, (0, [0] * 16))
            mask = lambda regs: sum(1 << x for x in regs if x != ESP)
            f = prog.func(va)
            rg = code_ranges(f)
            ncl = len({t for t in list(f.calls.values()) + list(f.tails.values())
                       if isinstance(t, int) and t in compiled})
            chunks.append("    {0x%08xu, AOT_SYM(fn_%08x), \"%s\", %d, %d, 0x%02x, 0x%02x, %d, {%s}, 0x%08xu, %d, %d, "
                          "AOT_SYM(rg_%08x), %s, AOT_LEAF(%d), AOT_EXIT(%d, %d), AOT_EXIT(%d, %d), %d}," % (
                              va, va, names.get(va, "%08x" % va), keep, prog.summ[va].pops or 0,
                              mask(sigs[va]["out"]), mask(sigs[va]["preserved"]), 1 if va in roots else 0,
                              ", ".join("0x%02x" % b for b in head), code_hash(img, rg), len(rg), ncl, va,
                              "AOT_SYM(cl_%08x)" % va if ncl else "0", 1 if va in leaf else 0,
                              1 if f.exits else 0, 1 if va in prog.may_exit else 0,
                              1 if set(f.exits) - set(f.callouts) else 0, 1 if va in prog.may_hard else 0,
                              1 if any(Emitter.x87_op(i) for i in f.insns.values()) else 0))
        chunks.append("    {0, 0, 0, 0, 0, 0, 0, 0, {0}, 0, 0, 0, 0, 0, 0, 0, 0, 0}")
        chunks.append("};")
    report["hooks"] = {"hookable": len(heads), "not": {"%08x" % k: v for k, v in sorted(why.items())}}
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
    for name in os.listdir(outdir):   # an earlier run's parts (the makefiles take every aot_spike*.c)
        if re.match(r"^aot_spike_\d+\.c$", name):
            os.remove(os.path.join(outdir, name))
    with open(os.path.join(outdir, "aot_spike.c"), "w") as fh:
        fh.write("\n".join(chunks) + "\n")
    for k, out in enumerate(part_texts):
        with open(os.path.join(outdir, "aot_spike_%02d.c" % (k + 1)), "w") as fh:
            fh.write("\n".join(out) + "\n")
    # The report: per function its size, its signature, its calls out.
    insns = 0
    for va in sorted(compiled):
        f = prog.func(va)
        insns += f.stats["insns"]
        falls = sorted({t for t in list(f.calls.values()) + list(f.tails.values()) if isinstance(t, int) and t not in compiled
                        and t not in INTRINSICS})
        ranges = code_ranges(f)
        report["functions"]["%08x" % va] = {
            "insns": f.stats["insns"], "bytes": f.stats["bytes"], "sig": sigs[va], "ranges": ranges,
            "switches": len(f.switches), "natives": sorted("%08x" % t for t in falls if t in NATIVES),
            "box86": sorted("%08x" % t for t in falls if t not in NATIVES)}
    trees = {}
    for r in roots:
        seen, work = set(), [r]
        while work:
            v = work.pop()
            if v in seen or v not in compiled:
                continue
            seen.add(v)
            work += [t for t in list(prog.func(v).calls.values()) + list(prog.func(v).tails.values())
                     if isinstance(t, int)]
        trees["%08x" % r] = sorted("%08x" % v for v in seen)
    report["trees"] = trees
    report["x86_insns"] = insns
    report["x86_bytes"] = sum(prog.func(va).stats["bytes"] for va in compiled)
    report["analysis_s"] = round(t1 - t0, 3)
    report["rounds"] = rounds
    report["emit_s"] = round(time.time() - t1, 3)
    report["closure"] = len(reach)
    with open(os.path.join(outdir, "aot_spike.json"), "w") as fh:
        json.dump(report, fh, indent=1, sort_keys=True)
    return report


HEX = re.compile(r"\b[0-9a-f]{8}\b")
HOT_BLOCK = re.compile(r"\[hotprof\]\s+\d+ ([0-9a-f]{8}) \+(\d+)\s+x(\d+)")


def refusal_kind(reason):
    """A refusal's cause without its addresses: an instruction the emitter
    has no C for by its kind (x87, a segment or system register, ...)."""
    r = HEX.sub("X", reason)
    if r.startswith("x87 ("):
        return r.replace(" at X", "")
    m = re.match(r"^(\S+)( \S.*)? at X$", r)
    if m and not r.startswith(("a ", "indirect", "branch", "tail", "flags", "direction", "esp", "code", "no ",
                                "stack", "imul", "an ", "its ")):
        mn = m.group(1)
        if mn in ("rep", "repz", "repe", "repne", "repnz", "lock") and m.group(2):   # capstone's prefixed mnemonics
            return "instruction %s %s" % (mn, m.group(2).split()[0])
        if mn.startswith("f"):
            return "x87 (%s)" % mn
        if m.group(2):
            return "segment or system register (%s)" % mn
        return "instruction %s" % mn
    if r.startswith("segment "):
        return "segment %s" % r.split()[1]
    r = re.sub(r"imul of \d+ bytes", "imul of 8 or 16 bits", r)
    r = re.sub(r"^flags of \S+ read", "flags of an instruction read", r)
    r = re.sub(r"^\S+ reads flags", "an instruction reads flags", r)
    r = re.sub(r"tail call at X: pops .*", "tail call at X: pops differ from the target's", r)
    r = re.sub(r"tail call through a pointer at X: pops .*", "tail call through a pointer: pops not known", r)
    r = re.sub(r"^a thunk through a pointer at X .*", "a thunk through a pointer whose pops aren't known", r)
    r = re.sub(r"^its flags are read after the call at X", "its flags are read after a call to it", r)
    r = re.sub(r"is the entry's [+-]\d+$", "is the entry's +n", r)
    return r.replace(" at X", "").replace(" (X)", "")


def load_hot(logs, sizes):
    """x86 bytes executed per function (d2d's starts and sizes) from the
    lane's [hotprof] lines (WX86_HOTPROF=1 D2_NOLINK=1)."""
    import bisect
    order = sorted(sizes)
    hot = collections.Counter()
    for log in logs:
        for line in open(log, encoding="utf-8", errors="replace"):
            m = HOT_BLOCK.search(line)
            if not m:
                continue
            addr, n = int(m.group(1), 16), int(m.group(2)) * int(m.group(3))
            k = bisect.bisect_right(order, addr) - 1
            f = order[k] if k >= 0 and addr < order[k] + max(sizes[order[k]], 1) else None
            hot[f] += n
    return hot


def census(img, starts, tsv, outdir, hot_logs, mode_list):
    """Issue #135: every function on d2d's list given to the compiler; what it
    refuses, by cause, and what compiled code still hands to Box86 (exits and
    nested runs), by kind; weighted by size and by the lane's executed bytes."""
    sizes, fnames = {}, {}
    with open(tsv) as fh:
        next(fh)
        for line in fh:
            a = line.rstrip("\n").split("\t")
            try:
                sizes[int(a[0], 16)] = int(a[2])
                fnames[int(a[0], 16)] = a[1]
            except (ValueError, IndexError):
                pass
    hot = load_hot(hot_logs, sizes) if hot_logs else collections.Counter()
    hot_total = sum(hot.values()) or 1
    keep = {}
    rep = compile_set(img, starts, [], sorted(sizes), mode_list, outdir, fnames, keep)
    prog = keep["prog"]
    compiled = {int(k, 16) for k in rep["functions"]}
    refused = {int(k, 16): v for k, v in rep["refused"].items()}

    def row():
        return {"functions": 0, "sites": 0, "bytes": 0, "hot": 0, "examples": [], "_seen": set()}

    def add(d, key, va):
        """One site of key's in function va: its bytes and hot bytes once per function."""
        r = d.setdefault(key, row())
        r["sites"] += 1
        if va in r["_seen"]:
            return
        r["_seen"].add(va)
        r["functions"] += 1
        r["bytes"] += sizes.get(va, 0)
        r["hot"] += hot.get(va, 0)
        if len(r["examples"]) < 8:
            r["examples"].append(va)

    by_refusal = {}
    for va, why in refused.items():
        add(by_refusal, refusal_kind(why), va)
    # Instruction kinds present, whether or not they were the first refusal.
    kinds = {}
    for va in sizes:
        f = prog.func(va)
        if f.refused and not f.insns:
            continue
        seen = set()
        for i in f.insns.values():
            m = i.mnemonic
            if m.startswith("f"):
                seen.add("x87")
            if m.startswith("rep"):
                seen.add("rep string")
            if m in ("cpuid", "rdtsc", "int", "int3", "in", "out", "sti", "cli", "hlt"):
                seen.add("instruction %s" % m)
            if " fs:" in " " + i.op_str or "fs:" in i.op_str:
                seen.add("fs: (thread block)")
        for k in seen:
            add(kinds, k, va)
    # What compiled code hands to Box86.
    exits, nested, imports = {}, {}, collections.Counter()
    for va in compiled:
        f = prog.func(va)
        for site in f.exits:
            t = f.calls.get(site, f.tails.get(site))
            how = "callout" if site in f.callouts else "plain exit"
            if site in f.tails and (t is None or isinstance(t, tuple)):
                k = "tail call through a pointer (%s)" % how
            elif t is None:
                k = "indirect call (%s)" % how
            elif isinstance(t, tuple):
                dll, name = img.imports[t[1]]
                imports["%s!%s" % (dll, name)] += 1
                k = "import (callout)" if site in f.callouts else "import (plain exit)"
            else:
                k = "call to a function left to Box86 that may block"
            add(exits, k, va)
        for site, t in list(f.calls.items()) + list(f.tails.items()):
            if site in f.exits or not isinstance(t, int) or t in compiled or t in INTRINSICS:
                continue
            if t in NATIVES:
                k = "a native of ours"
            elif t in ERROR_PATH_FUNCS or t in NORETURN_FUNCS:
                k = "an error path or exit (no return)"
            elif t in refused:
                k = "refused callee: " + refusal_kind(refused[t])
            else:
                k = "callee not on the list"
            add(nested, k, va)
    total_b = sum(sizes.values())
    comp_b = sum(sizes.get(va, 0) for va in compiled)
    out = {
        "functions": len(sizes), "bytes": total_b, "hot_listed": sum(hot.values()),
        "compiled": {"functions": len(compiled), "bytes": comp_b, "hot": sum(hot.get(va, 0) for va in compiled),
                     "x86_bytes": rep["x86_bytes"], "may_exit": rep.get("exits", {}).get("may_exit", 0),
                     "leaves": rep.get("leaves", 0)},
        "refused": by_refusal, "kinds": kinds, "exits": exits, "nested": nested,
        "imports": dict(imports.most_common()), "hot_logs": [os.path.basename(x) for x in hot_logs or []],
        "rounds": rep["rounds"], "analysis_s": rep["analysis_s"], "emit_s": rep["emit_s"],
        "hot_unlisted": hot.get(None, 0),
    }
    for d in (by_refusal, kinds, exits, nested):
        for r in d.values():
            r["examples"] = ["%08x" % v for v in r["examples"]]
            del r["_seen"]
    with open(os.path.join(outdir, "census.json"), "w") as fh:
        json.dump(out, fh, indent=1, sort_keys=True)

    def pct(n, d):
        return "%.1f%%" % (100.0 * n / d) if d else "-"

    lines = ["census (#135): %d functions on d2d's list, %d bytes; the lane's hot bytes from %s" % (
        len(sizes), total_b, ", ".join(out["hot_logs"]) or "no logs"),
        "compiled: %d functions (%s), %d bytes (%s), %s of the hot bytes; %d may hand a call to Box86" % (
            len(compiled), pct(len(compiled), len(sizes)), comp_b, pct(comp_b, total_b),
            pct(out["compiled"]["hot"], hot_total), out["compiled"]["may_exit"])]
    for title, d in (("refused, by first cause", by_refusal), ("instruction kinds present", kinds),
                     ("exits in compiled functions (sites)", exits),
                     ("calls from compiled code run nested by Box86 (sites)", nested)):
        lines.append("")
        lines.append("%s:" % title)
        lines.append("  %6s %6s %9s %7s %7s  %s" % ("funcs", "sites", "bytes", "bytes%", "hot%", "cause"))
        for k, r in sorted(d.items(), key=lambda kv: (-kv[1]["hot"], -kv[1]["bytes"], -kv[1]["functions"])):
            lines.append("  %6d %6d %9d %7s %7s  %s  [%s]" % (
                r["functions"], r["sites"], r["bytes"], pct(r["bytes"], total_b), pct(r["hot"], hot_total), k,
                " ".join(r["examples"][:4])))
    lines.append("")
    lines.append("imports called from compiled code (sites): " + ", ".join(
        "%s %d" % kv for kv in imports.most_common(40)))
    text = "\n".join(lines) + "\n"
    with open(os.path.join(outdir, "census.txt"), "w") as fh:
        fh.write(text)
    print(text)


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
    flags_live = []
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
                flags_live.append(va)
            else:
                res["flags_other"] += 1
                why["flags: " + s.split(" at ")[0]] += 1
    # x87: is the stack depth known at each instruction? Each function's
    # depth at its rets (a float result leaves one) to a fixed point first.
    deltas = {}
    for _ in range(20):
        changed = False
        for va in sorted(sizes):
            f = prog.func(va)
            if f.refused:
                continue
            _, dl = x87_depth(f, prog, deltas)
            if deltas.get(va, 0) != dl:
                deltas[va] = dl
                changed = True
        if not changed:
            break
    for va in sorted(sizes):
        f = prog.func(va)
        if f.refused:
            continue
        st, _ = x87_depth(f, prog, deltas)
        if st is None:
            x87_unknown.append(va)
        elif st:
            res["x87_known"] += 1
    # Switches the guard rule doesn't bound, from the refusals.
    res["switches_unbounded"] = why["a switch"] if "a switch" in why else sum(
        v for k, v in why.items() if k.startswith("a switch"))
    res["x87_unknown"] = len(x87_unknown)
    return res, why, x87_unknown, flags_live


X87_PUSH = {"fld", "fild", "fld1", "fldz", "fldpi", "fldl2e", "fldln2", "fldlg2", "fldl2t"}
X87_POP1 = {"fstp", "fistp", "faddp", "fsubp", "fsubrp", "fmulp", "fdivp", "fdivrp", "fcomp", "ficomp",
            "fucomp", "fcomip", "fucomip", "fisttp"}
X87_POP2 = {"fcompp", "fucompp"}


def x87_depth(f, prog, deltas):
    """(state, delta): state 0 no x87 at all, 1 the depth is known at every
    instruction (0 at entry; a call moves it by its callee's delta, known
    from deltas), None not known somewhere; delta the depth at its rets
    (None: not known, or rets that disagree)."""
    has = any(i.mnemonic.startswith("f") for i in f.insns.values())
    if not has:
        # No x87 of its own: a float a callee left would stay on the stack,
        # which MSVC's code never does (it pops an unused result), so its
        # callees' results are popped where they're made: 0 at its rets.
        return 0, 0
    depth = {f.entry: 0}
    work = [f.entry]
    at_ret = set()
    while work:
        b = work.pop()
        d = depth[b]
        va = b
        while True:
            i = f.insns[va]
            m = i.mnemonic
            if m in X87_PUSH:
                d += 1
            elif m in X87_POP2:
                d -= 2
            elif m in X87_POP1 or (m.startswith("f") and m.endswith("p") and m not in X87_POP2):
                d -= 1
            elif m in ("fninit", "finit"):
                d = 0
            elif va in f.calls:
                t = f.calls[va]
                if isinstance(t, int) and t not in prog.intrinsics:
                    dt = deltas.get(t, 0)
                    if dt is None:
                        return None, None
                    d += dt
            if va in f.rets:
                at_ret.add(d)
            if d < 0 or d > 8:
                return None, None
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
                    return None, None
            break
    delta = at_ret.pop() if len(at_ret) == 1 else (0 if not at_ret else None)
    return (1 if has else 0), delta


def explain(img, starts, want, vas):
    """Why each of vas is left to Box86, for the set's summaries (issue #115):
    the analysis' refusal, or for esp not known at a ret, the calls whose
    callee's pops aren't known, each with why."""
    prog = Program(img, starts | set(want) | set(vas), NATIVES, INTRINSICS)
    reach = prog.closure(want + vas) | set(want) | set(vas)
    prog.compiled.update(v for v in want if not prog.func(v).refused)   # as compile_set has them
    prog.summarize(sorted(reach))
    kinds = collections.Counter()
    for va in vas:
        f = prog.func(va)
        sm = prog.summ.get(va)
        if sm:
            print("%08x: summary reads %s, preserved %s, pops %s%s; %d rets%s" % (
                va, ",".join(REGS[r] for r in sorted(sm.reads)), ",".join(REGS[r] for r in sorted(sm.preserved)),
                sm.pops, "" if sm.known else " (not known)", len(f.rets),
                ", never returns" if va in prog.noreturn_funcs else ""))
        if f.refused:
            print("%08x: %s" % (va, f.refused))
            continue
        try:
            a = Analysis(prog, f, save_rule=True)
        except Refused as e:
            print("%08x: %s" % (va, e))
            continue
        if a.esp_ok:
            print("%08x: esp known at every ret" % va)
            continue
        sites = []
        for site, t in sorted(f.calls.items()):
            if site in f.noreturn or prog.callee_summary(t).pops is not None:
                continue
            if t is None:
                k = "indirect"
            elif isinstance(t, tuple):
                k = "import %s, pops not known" % (t[1],)
            else:
                g = prog.func(t)
                rets = sorted(set(g.rets.values())) if not g.refused else []
                k = ("%08x refused: %s" % (t, g.refused) if g.refused else
                     "%08x rets pop %s" % (t, rets) if len(rets) > 1 else
                     "%08x: %s" % (t, prog.why.get(t, "summary not known")))
            sites.append("%08x %s" % (site, k))
            kinds[re.sub(r"[0-9a-f]{8}", "", k)[:60]] += 1
        for site, t in sorted(f.calls.items()):
            if isinstance(t, int) and t in prog.compiled and EBP not in prog.callee_summary(t).preserved \
                    and site not in f.noreturn:
                k = "%08x compiled, ebp not shown preserved (%s)" % (t, prog.why.get(t, "its analysis"))
            else:
                continue
            kinds[re.sub(r"[0-9a-f]{8}", "", k)[:60]] += 1
            sites.append("%08x %s" % (site, k))
        print("%08x: esp not known at a ret; %d calls to blame:" % (va, len(sites)))
        for x in sites[:12]:
            print("    " + x)
    print("kinds:", ", ".join("%s %d" % kv for kv in kinds.most_common()))


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
    ap.add_argument("--compile", action="append", default=[], help="more functions to compile, va,va,...")
    ap.add_argument("--tree", action="append", default=[],
                    help="va:depth: also compile what va calls, directly, to that depth")
    ap.add_argument("--functions", default="third_party/d2d/docs/research/re/game-functions.tsv")
    ap.add_argument("--modes", default="sig,ctx")
    ap.add_argument("--callees", type=int, default=0,
                    help="N: also the functions the set calls, to depth N (issue #115: a callee left to Box86 "
                         "costs a nested run per call); those that don't compile stay with Box86")
    ap.add_argument("--no-callouts", action="store_true",
                    help="indirect and import calls as plain exits to Box86, not callouts (issue #115)")
    ap.add_argument("--survey", action="store_true")
    ap.add_argument("--census", action="store_true",
                    help="issue #135: compile every function on the list and report what's refused and why")
    ap.add_argument("--hot", action="append", default=[],
                    help="with --census: the lane's stderr with [hotprof] lines, to weight the causes")
    ap.add_argument("--explain", help="va,va,...: why each is left to Box86 (the calls whose pops aren't "
                                      "known), with the set's summaries; nothing written")
    a = ap.parse_args()
    global CALLOUTS
    CALLOUTS = not a.no_callouts
    img = Image(a.exe)
    starts = load_starts(img, a.functions)
    if a.census:
        census(img, starts, a.functions, a.out, a.hot, a.modes.split(","))
        return
    if a.survey:
        res, why, x87u, live = survey(img, starts, a.functions)
        for k in sorted(res):
            print("%-28s %d" % (k, res[k]))
        print("flags live at a call or ret:", " ".join("%08x" % v for v in live))
        print("x87 depth not known:", " ".join("%08x" % v for v in x87u[:40]), "..." if len(x87u) > 40 else "")
        print("refusals:")
        for k, v in why.most_common(20):
            print("  %6d  %s" % (v, k))
        return
    roots, names = [], {}
    for r in a.root:
        v, n = parse_vas(r)
        roots += v
        names.update(n)
    extra, _ = parse_vas(",".join(a.compile))
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
    if a.callees:
        prog = Program(img, starts, NATIVES, INTRINSICS)
        seen = set(extra) | set(roots)
        lvl = set(seen)
        for _ in range(a.callees):
            nxt = set()
            for g in sorted(lvl):
                f = prog.func(g)
                if f.refused:
                    continue
                for c in list(f.calls.values()) + list(f.tails.values()):
                    if isinstance(c, int) and c not in seen and c not in INTRINSICS and c not in NATIVES:
                        nxt.add(c)
            seen |= nxt
            lvl = nxt
        print("aot: %d callees added (--callees %d)" % (len(seen - set(extra) - set(roots)), a.callees))
        extra += sorted(seen - set(extra) - set(roots))
    if a.explain:
        explain(img, starts, roots + [v for v in extra if v not in roots], parse_vas(a.explain)[0])
        return
    rep = compile_set(img, starts, roots, extra, a.modes.split(","), a.out, names)
    n = len(rep["functions"])
    ex = rep.get("exits", {})
    print("aot: %d functions compiled (%d x86 instructions, %d bytes; %d never call Box86; %d hand Box86 the rest "
          "of a call at %d sites, %d with their callers; %d of the sites callouts, plain exits in %d functions), "
          "%d left to Box86, in %s"
          % (n, rep["x86_insns"], rep["x86_bytes"], rep.get("leaves", 0), ex.get("functions", 0), ex.get("sites", 0),
             ex.get("may_exit", 0), ex.get("callouts", 0), ex.get("plain", 0), len(rep["refused"]), a.out))
    pt = rep.get("parts")
    if pt:
        print("aot: the C in %d parts (AOT_PART_MB, by %s); calls between compiled functions: %d in their part, "
              "%d across" % (pt["files"], pt.get("order", "?"), pt["calls_local"], pt["calls_across"]))
    for k, v in sorted(rep["refused"].items()):
        print("  left to Box86: %s: %s" % (k, v))


if __name__ == "__main__":
    main()
