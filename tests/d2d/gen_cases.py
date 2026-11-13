#!/usr/bin/env python3
"""Cases for our natives from the user's own Game.exe (issue #109).

Each native's game function runs under Unicorn on chosen inputs, through
d2decomped's harness (tools/emu/emu.py, GPL-3.0-or-later), imported from the
prepared work tree build/d2d/d2d at run time, never copied. The inputs aim
at the edges: off the grid, empty lists, the largest radii, keys at both ends.

    python3 -I tests/d2d/gen_cases.py <Game.exe> <out dir> [--only NAME,...] [--count N]

A case is the memory the generator wrote before the call (the records it
built, and the game's globals it set), the registers and stack arguments,
then the game's EAX and every byte the game wrote, its stack aside. The
records sit in a window just above Game.exe's image (CASE_LO), so the
runner (native_cases.cpp) can hold the same addresses in a small arena.
A case whose game run writes anywhere else, or faults, is left out and
counted.

The files (<out dir>/<Name>.cases) are made from the user's Game.exe: they
stay in build/, never in the repository.

File format, little-endian: "D2NC", version 1, the name's length and the
name, flags (1: compare EAX), the case count; then per case: eax ecx edx ebx
ebp esi edi, the argument count and arguments, the writes before (count;
va, length, bytes each), the game's EAX, the game's writes (the same form).
"""
import argparse
import os
import random
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EMU_DIR = os.path.join(ROOT, "build", "d2d", "d2d", "tools", "emu")
sys.path.insert(0, EMU_DIR)
try:
    import emu   # noqa: E402  (d2d's harness, from the work tree)
except ImportError as error:
    sys.exit(f"gen_cases: d2d's tools/emu not importable from {EMU_DIR} ({error}): run make -C tests/d2d prepare")
from unicorn import UC_HOOK_MEM_WRITE, UC_PROT_ALL   # noqa: E402

CASE_LO, CASE_SIZE = 0x01000000, 0x00f00000   # the records' window; the runner's stack is above it
COMPARE_EAX = 1
REGS = ("eax", "ecx", "edx", "ebx", "ebp", "esi", "edi")


class Gen:
    """One Emu for all of a native's cases; memory put back after each."""

    def __init__(self, exe):
        self.e = emu.Emu(exe=exe)
        self.e.mu.mem_map(CASE_LO, CASE_SIZE, UC_PROT_ALL)
        # The image's extent, as emu.py mapped it.
        self.image_lo = self.e.base
        self.image_hi = self.image_lo + ((self._size_of_image(exe) + 0xfff) & ~0xfff)
        self.pristine = self.e.read(self.image_lo, self.image_hi - self.image_lo)
        self.writes, self.game = [], []
        self.e.mu.hook_add(UC_HOOK_MEM_WRITE, self._on_write)
        self.recording = False
        self.brk = CASE_LO

    @staticmethod
    def _size_of_image(exe):
        data = open(exe, "rb").read(0x1000)
        pe = struct.unpack_from("<I", data, 0x3c)[0]
        return struct.unpack_from("<I", data, pe + 24 + 56)[0]

    def _on_write(self, mu, access, addr, size, value, _):
        if self.recording:
            self.game.append((addr, size))

    # ---- building a case's memory ---------------------------------------
    def alloc(self, n, align=16):
        a = (self.brk + align - 1) & ~(align - 1)
        self.brk = a + max(n, 1)
        if self.brk > CASE_LO + CASE_SIZE:
            raise MemoryError("case window full")
        return a

    def put(self, va, data):
        data = bytes(data)
        self.e.mu.mem_write(va, data)
        self.writes.append((va, len(data)))

    def w8(self, va, v): self.put(va, struct.pack("<B", v & 0xff))
    def w16(self, va, v): self.put(va, struct.pack("<H", v & 0xffff))
    def w32(self, va, v): self.put(va, struct.pack("<I", v & 0xffffffff))

    def block(self, n):
        """A zeroed record of n bytes in the window (zeroed in the case too)."""
        a = self.alloc(n)
        self.put(a, b"\0" * n)
        return a

    # ---- one case -----------------------------------------------------------
    def run(self, addr, args=(), **regs):
        """The game's function on the memory built so far; returns the case,
        or None if the game faulted or wrote outside the image and the window."""
        e = self.e
        before = [(a, e.read(a, n)) for a, n in merge(self.writes)]   # as the game is given it
        self.game = []
        self.recording = True
        try:
            eax = e.call(addr, *args, ecx=regs.get("ecx", 0), edx=regs.get("edx", 0),
                         regs={k: v for k, v in regs.items() if k not in ("ecx", "edx")})
        except Exception:
            self.recording = False
            self.reset()
            return None
        self.recording = False
        stack = (emu.STACK, emu.STACK + emu.STACK_SIZE)
        game = merge([(a, n) for a, n in self.game if not stack[0] <= a < stack[1]])
        if any(not (self.image_lo <= a and a + n <= self.image_hi or CASE_LO <= a and a + n <= CASE_LO + CASE_SIZE)
               for a, n in game):
            self.reset()
            return None
        case = dict(regs={r: regs.get(r, 0) for r in REGS}, args=list(args),
                    before=before,
                    eax=eax, after=[(a, e.read(a, n)) for a, n in game])
        self.reset()
        return case

    def begin(self):
        self.writes = []
        self.brk = CASE_LO

    def reset(self):
        """Puts back what the case and the game wrote: the image from its
        pristine copy, the window to zeros."""
        touched = merge(self.writes + self.game)
        for a, n in touched:
            if self.image_lo <= a < self.image_hi:
                hi = min(a + n, self.image_hi)
                self.e.mu.mem_write(a, self.pristine[a - self.image_lo:hi - self.image_lo])
            elif CASE_LO <= a < CASE_LO + CASE_SIZE:
                self.e.mu.mem_write(a, b"\0" * min(n, CASE_LO + CASE_SIZE - a))
        self.writes, self.game = [], []


def merge(spans):
    out = []
    for a, n in sorted(spans):
        if out and a <= out[-1][0] + out[-1][1]:
            lo, ln = out[-1]
            out[-1] = (lo, max(lo + ln, a + n) - lo)
        else:
            out.append((a, n))
    return out


def write_cases(path, name, flags, cases):
    with open(path, "wb") as f:
        nb = name.encode()
        f.write(b"D2NC" + struct.pack("<II", 1, len(nb)) + nb + struct.pack("<II", flags, len(cases)))
        for c in cases:
            f.write(struct.pack("<7I", *(c["regs"][r] & 0xffffffff for r in REGS)))
            f.write(struct.pack("<I", len(c["args"])) + b"".join(struct.pack("<I", v & 0xffffffff) for v in c["args"]))
            for spans in (c["before"],):
                f.write(struct.pack("<I", len(spans)))
                for a, data in spans:
                    f.write(struct.pack("<II", a, len(data)) + data)
            f.write(struct.pack("<I", c["eax"] & 0xffffffff))
            f.write(struct.pack("<I", len(c["after"])))
            for a, data in c["after"]:
                f.write(struct.pack("<II", a, len(data)) + data)


# ---- the natives' inputs -------------------------------------------------------
# Each generator yields cases: g.begin(), build the memory, then g.run().

def storm_hash(g, rng, count):
    """415330(eax=name, [8]=type): the crypt table at [0x779048] (random
    words: both sides read the same), names of every byte but 0, empty, and
    a null name; types 0..4 (the table's 0x500 words)."""
    for i in range(count):
        g.begin()
        table = g.alloc(0x500 * 4)
        g.put(table, bytes(rng.getrandbits(8) for _ in range(0x500 * 4)))
        g.w32(0x00779048, table)
        kind = i % 6
        if kind == 0:
            name = 0
        else:
            n = 0 if kind == 1 else rng.randint(1, 80)
            text = bytes(rng.choice((rng.randint(1, 255), rng.randint(0x61, 0x7a), rng.randint(0x41, 0x5a), 0x5c, 0x2e))
                         for _ in range(n))
            name = g.alloc(len(text) + 1)
            g.put(name, text + b"\0")
        yield g.run(0x00415330, (rng.randint(0, 4),), eax=name)


def stat_get(g, rng, count):
    """625480([8]=unit, [c]=stat, [10]=layer): the stat table ([0x744304]:
    +0xbcc rows of 0x144, +0xbd4 their count), a unit (+0x5c its holder),
    the holder's base list (+0x24) or full list (+0x48, when +0x10's sign
    bit is set) of sorted (stat << 16 | layer, value) pairs (values often at
    their row's minimum and either side of it), its owner
    (+0x44, of type 0..3), the rows' minimum (+0x2c), shift (+0x18) and
    flags (+5) against [0x6ce270]. Keys asked: present, absent, at both
    ends, out of range, negative; null unit and holder."""
    for i in range(count):
        g.begin()
        nrows = rng.choice((1, 8, 40, 400))
        tables = g.block(0xc00)
        rows = g.block(nrows * 0x144)
        g.w32(0x00744304, tables)
        g.w32(tables + 0xbcc, rows)
        g.w32(tables + 0xbd4, nrows)
        mins = [rng.randint(-50, 300) for _ in range(nrows)]
        for r in range(nrows):
            g.w8(rows + r * 0x144 + 5, rng.getrandbits(8))
            g.w8(rows + r * 0x144 + 0x18, rng.randint(0, 40))
            g.w32(rows + r * 0x144 + 0x2c, mins[r])
        g.w8(0x006ce270, rng.getrandbits(8))
        unit = g.block(0x100)
        holder = g.block(0x80) if i % 9 else 0
        g.w32(unit + 0x5c, holder)
        keys = []
        if holder:
            full = rng.random() < 0.5
            g.w32(holder + 0x10, (0x80000000 if full else 0) | rng.getrandbits(16))
            n = rng.choice((0, 1, 2, 7, 30, 200))
            keys = sorted({(rng.randrange(nrows + 2) << 16 | rng.randrange(4)) for _ in range(n)})
            pairs = g.block(max(len(keys), 1) * 8)
            for k, key in enumerate(keys):
                g.w32(pairs + 8 * k, key)
                near = mins[key >> 16] + rng.randint(-1, 1) if (key >> 16) < nrows else 0   # at the row's minimum
                g.w32(pairs + 8 * k + 4, rng.choice((rng.randint(-100, 100), rng.getrandbits(32), near, near)))
            lst = holder + (0x48 if full else 0x24)
            g.w32(lst, pairs)
            g.w16(lst + 4, len(keys))
            if rng.random() < 0.8:
                owner = g.block(0x10)
                g.w32(owner, rng.randint(0, 3))
                g.w32(holder + 0x44, owner)
        pick = rng.random()
        if keys and pick < 0.6:
            key = rng.choice(keys if pick < 0.4 else (keys[0], keys[-1]))
            stat, layer = key >> 16, key & 0xffff
        else:
            stat = rng.choice((rng.randrange(nrows + 3), -1, nrows, nrows - 1, 0))
            layer = rng.randrange(4)
        yield g.run(0x00625480, (0 if i % 23 == 0 else unit, stat, layer))


# The light grid's globals (d2_native.cpp): its origin and last cell in
# subtiles and cells, the 48 x 48 cells of 8 bytes, coloured light, and the
# 256 reciprocals for the colour mix.
GRID_X, GRID_Y, GRID_MAX_X, GRID_MAX_Y = 0x007b0a54, 0x007b0a58, 0x007b0a5c, 0x007b0a60
LIGHT_GRID, COLORED, RECIP, PLAYER = 0x007b0e68, 0x00712b8c, 0x007b0a68, 0x007a6a70


def light_globals(g, rng):
    gx, gy = rng.randint(0, 400), rng.randint(0, 400)
    g.w32(GRID_X, gx)
    g.w32(GRID_Y, gy)
    g.w32(GRID_MAX_X, gx + 47)
    g.w32(GRID_MAX_Y, gy + 47)
    g.w32(COLORED, rng.random() < 0.6)
    g.put(RECIP, b"".join(struct.pack("<I", 65536 // n if n else 0) for n in range(256)))
    cells = bytearray(48 * 48 * 8)
    for k in range(48 * 48):
        if rng.random() < 0.7:
            cells[8 * k + 4:8 * k + 8] = bytes(rng.getrandbits(8) for _ in range(4))
    g.put(LIGHT_GRID, cells)
    return gx, gy


def light_add(g, rng, count):
    """4747c0(eax=x, ecx=y, [8]=intensity, [c..14]=r, g, b): cells on the
    grid, on its edges and just off it, intensities to saturation."""
    for _ in range(count):
        g.begin()
        gx, gy = light_globals(g, rng)
        x = (gx + rng.randint(-2, 49)) * 8 + rng.randint(0, 7)
        y = (gy + rng.randint(-2, 49)) * 8 + rng.randint(0, 7)
        v = rng.choice((0, 1, rng.randint(0, 255), 255, 300))
        yield g.run(0x004747c0, (v, rng.getrandbits(8), rng.getrandbits(8), rng.getrandbits(8)), eax=x, ecx=y)


def light_record(g, rng, gx, gy, occluded):
    light = g.block(0x40)
    g.w32(light, rng.choice((0, 1, 2, 3, 4)))
    g.w32(light + 4, rng.randint(0, 3))
    g.w32(light + 0x10, (gx + rng.randint(-8, 56)) * 8 + rng.randint(0, 7))
    g.w32(light + 0x14, (gy + rng.randint(-8, 56)) * 8 + rng.randint(0, 7))
    g.w32(light + 0x18, rng.choice((-1, 0, 1, 7, 8, 16, 17, rng.randint(1, 120), 0xfe, 0xff, 0x100)))
    g.put(light + 0x24, bytes(rng.getrandbits(8) for _ in range(4)))   # intensity, r, g, b
    if occluded:
        radius = max(0, min(0xff, struct.unpack("<i", g.e.read(light + 0x18, 4))[0]))
        n = (2 * radius >> 3) + 1
        occ = g.block(4 * n * n)
        g.put(occ, b"".join(struct.pack("<i", rng.choice((0, 0, 1, 5, 15, 16, 30))) for _ in range(n * n)))
        g.w32(light + 0x30, occ)
        g.w32(light + 0x2c, 1)
    return light


def light_stamp(g, rng, count):
    """4748d0(esi=light, [8]=as is): types 0..4, the player's and not
    (light +4 against the player unit's +0xc), radii from none to past 0xff,
    the square on, across and off the grid."""
    for _ in range(count):
        g.begin()
        gx, gy = light_globals(g, rng)
        if rng.random() < 0.8:
            player = g.block(0x20)
            g.w32(player + 0xc, rng.randint(0, 3))
            g.w32(PLAYER, player)
        light = light_record(g, rng, gx, gy, False)
        yield g.run(0x004748d0, (rng.randint(0, 1),), esi=light)


def light_stamp_occluded(g, rng, count):
    """475420(edi=light): the light's occlusion map (+0x30), a dword per cell
    of its square, from open to dark (16 and more)."""
    for _ in range(count):
        g.begin()
        gx, gy = light_globals(g, rng)
        light = light_record(g, rng, gx, gy, True)
        yield g.run(0x00475420, (), edi=light)


# name: (generator, flags). EAX is dead after the light stamps (their one
# caller overwrites it), so it is not compared there.
NATIVES = {
    "StormHash": (storm_hash, COMPARE_EAX),
    "StatGet": (stat_get, COMPARE_EAX),
    "LightAdd": (light_add, 0),
    "LightStamp": (light_stamp, 0),
    "LightStampOccluded": (light_stamp_occluded, 0),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("exe")
    ap.add_argument("out")
    ap.add_argument("--only", help="comma-separated natives (default: all)")
    ap.add_argument("--count", type=int, default=400, help="cases per native")
    ap.add_argument("--seed", type=int, default=109)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    names = args.only.split(",") if args.only else list(NATIVES)
    for name in names:
        fn, flags = NATIVES[name]
        g = Gen(args.exe)
        rng = random.Random(f"{args.seed}:{name}")
        cases, dropped = [], 0
        for case in fn(g, rng, args.count):
            if case is None:
                dropped += 1
            else:
                cases.append(case)
        write_cases(os.path.join(args.out, f"{name}.cases"), name, flags, cases)
        print(f"{name}: {len(cases)} cases" + (f", {dropped} left out (the game faulted or wrote outside)" if dropped else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
