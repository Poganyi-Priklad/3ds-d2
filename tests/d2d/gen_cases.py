#!/usr/bin/env python3
"""Cases for our natives from the user's own Game.exe (issue #109).

Each native's game function runs under Unicorn on chosen inputs, through
d2decomped's harness (tools/emu/emu.py, GPL-3.0-or-later), imported from the
prepared work tree build/d2d/d2d at run time, never copied. The inputs aim
at the edges: off the grid, empty lists, the largest radii, keys at both ends.

    python3 -I tests/d2d/gen_cases.py <Game.exe> <out dir> [--only NAME,...] [--count N]

A case is the memory the generator wrote before the call (the records it
built, and the game's globals it set), the registers and stack arguments,
then the game's EAX and every byte the game wrote, its stack and its
allocator's heap (work buffers) aside. The
records sit in a window just above Game.exe's image (CASE_LO), so the
runner (native_cases.cpp) can hold the same addresses in a small arena.
A case whose game run writes anywhere else, faults, or takes the game's
fatal-error path (an assertion) is left out and counted.

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
import re
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
        # The emulator's own answers to the game's imports (wsprintfA's
        # output, say) write from Python, which the hook above doesn't see.
        mem_write = self.e.mu.mem_write

        def stub_write(addr, data):
            if self.recording:
                self.game.append((addr, len(data)))
            return mem_write(addr, data)
        self.e.mu.mem_write = stub_write
        self.recording = False
        self.brk = CASE_LO

    @staticmethod
    def _size_of_image(exe):
        data = open(exe, "rb").read(0x1000)
        pe = struct.unpack_from("<I", data, 0x3c)[0]
        return struct.unpack_from("<I", data, pe + 24 + 56)[0]

    def storm_heap(self):
        """Storm's SMemAlloc (413020) and SMemFree (412650), both (n or
        ptr, file, line, flags) stdcall, served by emu.py's heap: their
        bookkeeping in .bss (the last call's source file, at 0x7768a8) is
        the allocator's, not the decoder's, and stays out of the case."""
        if not getattr(self, "_storm_heap", False):
            self.e.hook(0x00413020, lambda e: e.alloc(e.arg(0)), 4)
            self.e.hook(0x00412650, lambda e: 1, 4)
            self._storm_heap = True

    def answer(self, name, fn):
        """The game's import `name` answered by fn(e) -> (eax, stack dwords)."""
        for a, (n, _) in list(self.e.slots.items()):
            if n == name:
                self.e.slots[a] = (n, fn)

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
        except KeyboardInterrupt:
            raise
        except BaseException as error:   # a fault, or the game's fatal-error path (emu.py exits on its imports)
            self.recording = False
            self.reset()
            if os.environ.get("GEN_CASES_DEBUG"):
                print(f"  left out: {type(error).__name__}: {error}", file=sys.stderr)
            return None
        self.recording = False
        # The game's stack, its thread block (fs:[0], an exception frame's
        # link) and its allocator's heap (emu.py serves Storm's and the CRT's
        # allocations there): scratch, never compared.
        scratch = ((emu.STACK, emu.STACK + emu.STACK_SIZE), (emu.TEB, emu.TEB + 0x10000),
                   (emu.HEAP, emu.HEAP + emu.HEAP_SIZE))
        game = merge([(a, n) for a, n in self.game if not any(lo <= a < hi for lo, hi in scratch)])
        outside = [(a, n) for a, n in game if not (self.image_lo <= a and a + n <= self.image_hi or
                                                    CASE_LO <= a and a + n <= CASE_LO + CASE_SIZE)]
        if outside:
            self.reset()
            if os.environ.get("GEN_CASES_DEBUG"):
                print(f"  left out: the game wrote {', '.join(f'{a:08x}+{n}' for a, n in outside[:4])}", file=sys.stderr)
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


# ---- rooms and collision ----------------------------------------------------------
# A room (d2_native.cpp, room_has): its neighbour list at +0 and their count
# at +0x24, its collision at +0x20 (+0 x, +4 y, +8 width, +0xc height, +0x20
# a word per subtile), its bounds in subtiles at +0x4c, +0x50, +0x54, +0x58.

MASKS = (0x1, 0x2, 0x4, 0x20, 0x22, 0x804, 0x1805, 0x3c01, 0x1c09, 0x3f11, 0xffff)


def world(g, rng):
    """Rooms side by side (a grid of columns and rows of random sizes), each
    with a collision grid of random walls, and neighbour lists of the rooms
    around it (sometimes itself, sometimes one left out). Some rooms have no
    collision, or a collision without its grid. Returns the rooms with their
    bounds, and the world's extent."""
    cols, rows = rng.randint(1, 3), rng.randint(1, 3)
    sizes = (5, 16, 40, 40, 80, 80) if rng.random() < 0.3 else (40, 80, 80)   # mostly as large as the game's
    widths = [rng.choice(sizes) for _ in range(cols)]
    heights = [rng.choice(sizes) for _ in range(rows)]
    ox, oy = rng.randint(200, 4000), rng.randint(200, 4000)
    density = rng.choice((0.0, 0.05, 0.2, 0.45))
    rooms = {}
    y = oy
    for r in range(rows):
        x = ox
        for c in range(cols):
            w, h = widths[c], heights[r]
            room = g.block(0x80)
            g.w32(room + 0x4c, x); g.w32(room + 0x50, y); g.w32(room + 0x54, w); g.w32(room + 0x58, h)
            kind = rng.random()
            if kind > 0.05:
                col = g.block(0x40)
                g.w32(room + 0x20, col)
                g.w32(col, x); g.w32(col + 4, y); g.w32(col + 8, w); g.w32(col + 0xc, h)
                if kind > 0.08:
                    words = g.alloc(2 * w * h, 4)
                    g.put(words, b"".join(struct.pack("<H", rng.choice(MASKS) if rng.random() < density else
                                                      rng.choice((0, 0, 0, 0x100, 0x8000))) for _ in range(w * h)))
                    g.w32(col + 0x20, words)
            rooms[(c, r)] = (room, x, y, w, h)
            x += w
        y += h
    for (c, r), (room, *_ ) in rooms.items():
        near = [rooms[(c + dc, r + dr)][0] for dc in (-1, 0, 1) for dr in (-1, 0, 1)
                if (dc or dr) and (c + dc, r + dr) in rooms]
        if rng.random() < 0.3:
            near.append(room)
        if near and rng.random() < 0.15:
            near.pop(rng.randrange(len(near)))
        rng.shuffle(near)
        lst = g.block(4 * max(len(near), 1))
        for k, n in enumerate(near):
            g.w32(lst + 4 * k, n)
        g.w32(room, lst)
        g.w32(room + 0x24, len(near))
    return list(rooms.values()), (ox, oy, ox + sum(widths), oy + sum(heights))


def room_find(g, rng, count):
    """463740(ecx=room, edx=x, [8]=y): points in the room, in a neighbour,
    on the edges, outside every room; a null room."""
    for i in range(count):
        g.begin()
        rooms, (x0, y0, x1, y1) = world(g, rng)
        room = rng.choice(rooms)[0] if i % 17 else 0
        x, y = rng.randint(x0 - 6, x1 + 5), rng.randint(y0 - 6, y1 + 5)
        yield g.run(0x00463740, (y,), ecx=room, edx=x)


def collision_cross(g, rng, count):
    """64d4e0([8]=room, [c]=x, [10]=y; esi=mask): the subtile and its four
    neighbours, inside a room and on its edges (where the next room's
    collision is asked: the native hands those to the game's code)."""
    for _ in range(count):
        g.begin()
        rooms, (x0, y0, x1, y1) = world(g, rng)
        room, rx, ry, w, h = rng.choice(rooms)
        if rng.random() < 0.7:
            x, y = rng.randint(rx, rx + w - 1), rng.randint(ry, ry + h - 1)
        else:
            x, y = rng.randint(x0 - 3, x1 + 2), rng.randint(y0 - 3, y1 + 2)
        yield g.run(0x0064d4e0, (room, x, y), esi=rng.choice(MASKS))


def walker(g, rng, rooms, extent, search):
    """A path (UnitWalk) or a search record (PathSearch): +0 the target or
    start, +4 where the unit is or the goal, +8 the room, +0x28 the size
    class (1, 3, 5: the cross test; others the game's), +0x2c the mask,
    +0x30 the unit."""
    x0, y0, x1, y1 = extent
    room, rx, ry, w, h = rng.choice(rooms)
    m = min(12, w // 3, h // 3) if rng.random() < 0.8 else 0   # mostly away from the room's edges
    ax, ay = rng.randint(rx + m, rx + w - 1 - m), rng.randint(ry + m, ry + h - 1 - m)
    spread = rng.choice((1, 3, 8, 20, 40))
    bx = max(x0 - 2, min(x1 + 1, ax + rng.randint(-spread, spread)))
    by = max(y0 - 2, min(y1 + 1, ay + rng.randint(-spread, spread)))
    rec = g.block(0x40)
    unit = g.block(0x100 + 4 * 78)
    if search:   # start where the unit is, the goal away
        g.w16(rec, ax); g.w16(rec + 2, ay); g.w16(rec + 4, bx); g.w16(rec + 6, by)
        g.w32(unit + 0x58, rng.random() < 0.3)
    else:        # the target away, the unit where it is
        g.w16(rec, bx); g.w16(rec + 2, by); g.w16(rec + 4, ax); g.w16(rec + 6, ay)
        inner = g.block(0x10)
        g.w32(inner, rng.choice((0, 0, 1, 2)))   # 0: a player's, which settles (648050)
        g.w32(unit + 0x30, inner)
        if rng.random() < 0.03:
            g.w32(unit + 0x34, 0x40000)
    g.w32(rec + 8, room)
    g.w32(rec + 0x28, rng.choice((1, 1, 1, 1, 1, 3, 5, 0, 2)))   # 1, 3, 5: the cross test; others the game's
    g.w32(rec + 0x2c, rng.choice(MASKS))
    g.w32(rec + 0x30, unit)
    return rec


def unit_walk(g, rng, count):
    """648120(eax=path): one step toward the target through the rooms'
    collision, a player's push-on (648050), targets reached, blocked, off the
    rooms."""
    for _ in range(count):
        g.begin()
        rooms, extent = world(g, rng)
        yield g.run(0x00648120, (), eax=walker(g, rng, rooms, extent, False))


def path_search(g, rng, count):
    """67b850(ecx=search): the A* toward the goal, near and far (a full pool
    of 200 nodes ends it), walls from none to many, the goal check (+0x58)."""
    for _ in range(count):
        g.begin()
        rooms, extent = world(g, rng)
        yield g.run(0x0067b850, (), ecx=walker(g, rng, rooms, extent, True))


# ---- the sound and archive decoders ------------------------------------------------
# Their game versions take work buffers from Storm's allocator, which emu.py
# serves from its own heap (emu.HEAP): those writes are the game's scratch,
# not compared (Gen.run).

def adpcm(g, rng, count):
    """41dce0(ecx=dst, eax=src bytes, [8]=dst bytes, [c]=src, [10]=channels):
    the shift at +1, a first sample per channel, then deltas and codes
    (bit 7: 0 repeat, 1 and 2 step moves); output room from none to more
    than enough, mono and stereo."""
    for _ in range(count):
        g.begin()
        channels = rng.choice((1, 2))
        n = rng.choice((0, 1, 2, 3, rng.randint(4, 64), rng.randint(64, 3000)))
        head = bytes((rng.getrandbits(8), rng.choice((rng.randint(0, 15), rng.getrandbits(8))))) + \
            bytes(rng.getrandbits(8) for _ in range(2 * channels))
        body = bytes(rng.choice((rng.getrandbits(7), 0x80, 0x81, 0x82, rng.getrandbits(8))) for _ in range(n))
        data = (head + body)[:rng.choice((len(head) + len(body), rng.randint(0, len(head) + len(body))))]
        src = g.alloc(len(data) + 16)
        g.put(src, data)
        room = rng.choice((0, 1, 2, 3, 2 * len(data), 4 * len(data) + 8, rng.randint(0, 8 * len(data) + 16)))
        dst = g.block(room + 16)
        yield g.run(0x0041dce0, (room, src, channels), ecx=dst, eax=len(data))


def huffman(g, rng, count):
    """41f9e0(ecx=dst, edx=&dst bytes, [8]=src): Storm's adaptive Huffman,
    the stream's first byte choosing the weight table; random bits after it
    (decoded until the end code or a full buffer), output room from none."""
    g.storm_heap()
    for _ in range(count):
        g.begin()
        n = rng.choice((4, 8, rng.randint(8, 256), rng.randint(256, 4096)))
        data = bytes((rng.choice((0, 1, 2, 3, 4, 5, 6, 7, 8, rng.randint(0, 8))),)) + bytes(rng.getrandbits(8) for _ in range(n))
        src = g.alloc(len(data) + 64)
        g.put(src, data + b"\0" * 64)
        room = rng.choice((0, 1, 16, rng.randint(1, 4096), 8192))
        dst = g.block(room + 16)
        size = g.block(4)
        g.w32(size, room)
        yield g.run(0x0041f9e0, (src,), ecx=dst, edx=size)


_PK = None


def pk_tables():
    """The DCL code tables, read from our own decoder (src/game/pklib_explode.cpp),
    so the streams are encoded with the codes it decodes."""
    global _PK
    if _PK is None:
        text = open(os.path.join(ROOT, "src", "game", "pklib_explode.cpp")).read()
        _PK = {}
        for name in ("kDistBits", "kDistCode", "kExLenBits", "kLenBase", "kLenBits", "kLenCode", "kChBitsAsc",
                     "kChCodeAsc"):
            m = re.search(name + r"\[[^\]]*\]\s*=\s*\{(.*?)\};", text, re.S)
            _PK[name] = [int(v, 0) for v in m.group(1).replace("\n", " ").split(",") if v.strip()]
    return _PK


def pk_stream(rng, ctype, dsize_bits, n_out, end):
    """A PKWARE DCL stream (Storm's "implode"): literals and repetitions whose
    distances stay within what is already out, as any real one; LSB first
    after the two header bytes. end: the end code (0x305) after the data."""
    t = pk_tables()
    bits, nbits, out = 0, 0, bytearray()

    def put(v, n):
        nonlocal bits, nbits
        bits |= (v & ((1 << n) - 1)) << nbits
        nbits += n
        while nbits >= 8:
            out.append(bits & 0xff)
            bits >>= 8
            nbits -= 8

    def length_code(rest):   # rest = repetition length - 2, or 0x205 for the end
        for i in range(16):
            if t["kLenBase"][i] <= rest < t["kLenBase"][i] + (1 << t["kExLenBits"][i]):
                put(1, 1)
                put(t["kLenCode"][i], t["kLenBits"][i])
                put(rest - t["kLenBase"][i], t["kExLenBits"][i])
                return
        raise ValueError(rest)

    done = 0
    while done < n_out:
        if done >= 2 and rng.random() < 0.45:
            length = rng.choice((2, 2, 3, rng.randint(3, 16), rng.randint(16, 0x206)))
            low_bits = 2 if length == 2 else dsize_bits
            distance = rng.randint(1, min(done, 64 << low_bits))
            length_code(length - 2)
            code, low = (distance - 1) >> low_bits, (distance - 1) & ((1 << low_bits) - 1)
            put(t["kDistCode"][code], t["kDistBits"][code])
            put(low, low_bits)
            done += length
        else:
            byte = rng.getrandbits(8)
            put(0, 1)
            if ctype == 0:
                put(byte, 8)
            else:
                put(t["kChCodeAsc"][byte], t["kChBitsAsc"][byte])
            done += 1
    if end:
        length_code(0x205)
    if nbits:
        out.append(bits & 0xff)
    return bytes((ctype, dsize_bits)) + bytes(out)


def pk_explode(g, rng, count):
    """41e000(ecx=dst, edx=&dst bytes, [8]=src, [c]=src bytes): PKWARE's
    explode on streams our encoder makes (binary and ASCII literals, short and
    long repetitions, dictionaries of 4, 5 and 6 bits), with and without the
    end code, cut short, with bad headers; outputs from none to more than
    enough. Random bits are no case: their repetitions reach before the
    output's start, into the work buffer's old contents, which differ by
    design (Storm's buffer is new each call, ours is kept)."""
    g.storm_heap()
    for _ in range(count):
        g.begin()
        ctype, dsize_bits = rng.choice((0, 0, 1)), rng.choice((4, 5, 6))
        n = rng.choice((0, 1, 5, rng.randint(5, 200), rng.randint(200, 6000)))
        data = pk_stream(rng, ctype, dsize_bits, n, rng.random() < 0.7)
        kind = rng.random()
        if kind < 0.15:
            data = data[:rng.randint(0, len(data))]              # cut short
        elif kind < 0.22:
            data = bytes((rng.choice((2, 0xff, ctype)), rng.choice((3, 7, 0, dsize_bits)))) + data[2:]   # a bad header
        src = g.alloc(len(data) + 64)
        g.put(src, data)
        room = rng.choice((0, 1, 16, n, n, max(0, n - 1), rng.randint(1, 4096), 0x4000))
        dst = g.block(room + 16)
        size = g.block(4)
        g.w32(size, room)
        yield g.run(0x0041e000, (src, len(data)), ecx=dst, edx=size)


# ---- units: their relation, skill levels and the cursor's filter ---------------------
# A unit: +0 type (0 player, 1 monster, 2 object, 3 missile, 4 item, 5 tile),
# +4 class, +0xc id, +0x2c its path (types 2, 4, 5: [path] the room; others
# [path+0x1c]), +0x5c its stat holder, +0x94, +0x98, +0xc4 and +0xc8 flags.
# A stat holder: +0x10 (sign bit: the full list at +0x48, else the base list
# at +0x24: (stat << 16 | layer, value) pairs sorted, (int16)[list+4] of
# them), +0x3c and +0x40 its stat lists (each a holder, +0x14 its state,
# +0x2c the next), +0x44 its owner, +0x58 the state bits.

TOWNS = (1, 40, 75, 103, 109)


def stat_rows(g, rng, tables, nrows):
    rows = g.block(nrows * 0x144)
    g.w32(tables + 0xbcc, rows)
    g.w32(tables + 0xbd4, nrows)
    for r in range(nrows):
        if rng.random() < 0.2:
            g.w8(rows + r * 0x144 + 5, rng.getrandbits(8))
            g.w8(rows + r * 0x144 + 0x18, rng.randint(0, 8))
            g.w32(rows + r * 0x144 + 0x2c, rng.randint(-5, 20))
    g.w8(0x006ce270, rng.getrandbits(8))


def stat_holder(g, rng, keys, full=None, owner_type=None):
    """A holder with values for the given keys (stat << 16 | layer), and a
    few others; its full or base list."""
    h = g.block(0x80)
    full = rng.random() < 0.6 if full is None else full
    g.w32(h + 0x10, (0x80000000 if full else 0) | rng.getrandbits(16))
    keys = sorted({k for k in keys if rng.random() < 0.7} | {rng.getrandbits(24) for _ in range(rng.randint(0, 3))})
    pairs = g.block(max(len(keys), 1) * 8)
    for k, key in enumerate(keys):
        g.w32(pairs + 8 * k, key)
        g.w32(pairs + 8 * k + 4, rng.choice((0, 1, 2, 3, 4, rng.randint(-3, 30), rng.randint(0, 0x7fff))))
    lst = h + (0x48 if full else 0x24)
    g.w32(lst, pairs)
    g.w16(lst + 4, len(keys))
    if owner_type is not None or rng.random() < 0.5:
        owner = g.block(0x10)
        g.w32(owner, rng.randint(0, 3) if owner_type is None else owner_type)
        g.w32(h + 0x44, owner)
    return h


def unit_record(g, rng, utype, room=None):
    u = g.block(0x100)
    g.w32(u, utype)
    g.w32(u + 4, rng.randint(0, 6) if utype == 0 else rng.randint(0, 40))
    g.w32(u + 0xc, rng.choice((1, 2, 3, rng.randint(0, 40), rng.getrandbits(32))))
    path = g.block(0x40)
    g.w32(u + 0x2c, path)
    if room is not None:
        g.w32(path if utype in (2, 4, 5) else path + 0x1c, room)
    return u


def relation_world(g, rng):
    """The tables the relation reads: monstats (+0xa78 rows of 0x1a8, +0xa80
    of them; +0x18 the index into +0xa90's rows of 0x134, +0xa98 of them,
    flags at +4), the stat table (stat 0xac), and a room on a level, a town
    sometimes."""
    tables = g.block(0xc00)
    g.w32(0x00744304, tables)
    nmon, nflag = rng.choice((0, 41, 41)), rng.choice((0, 20, 20))
    mon, flg = g.block(max(nmon, 1) * 0x1a8), g.block(max(nflag, 1) * 0x134)
    g.w32(tables + 0xa78, mon); g.w32(tables + 0xa80, nmon)
    g.w32(tables + 0xa90, flg); g.w32(tables + 0xa98, nflag)
    for k in range(nmon):
        g.w16(mon + k * 0x1a8 + 0x18, rng.choice((rng.randrange(max(nflag, 1)),) * 4 + (-1, nflag)))
    for k in range(nflag):
        g.w8(flg + k * 0x134 + 4, rng.choice((0, 0x10, 0x20, 0x30, rng.getrandbits(8))))
    stat_rows(g, rng, tables, rng.choice((0xad, 0xad, 0x10)))
    level = g.block(0x200)
    g.w32(level + 0x1d0, rng.choice(TOWNS) if rng.random() < 0.15 else rng.choice((0, 0x6e, rng.randint(1, 0x6d))))
    room2 = g.block(0x80)
    g.w32(room2 + 0x58, level)
    room = g.block(0x80)
    g.w32(room + 0x10, room2)
    return room


def relation_unit(g, rng, room):
    """A unit for the relation: any type; sometimes in the roster as a player,
    sometimes with +0xc8's bit 10 (a summoned unit, its owner at +0x98), and
    sometimes an alignment (stat 0xac of a state 0x69 list)."""
    u = unit_record(g, rng, rng.choice((0, 0, 1, 1, 1, 1, 2, 3, 4)), room)
    if rng.random() < 0.2:
        g.w32(u + 0xc8, 1 << 10)
    g.w32(u + 0x94, rng.choice((0, 0, 1)))
    g.w32(u + 0x98, rng.choice((1, 2, rng.randint(0, 40))))
    if rng.random() < 0.8:
        h = g.block(0x80)
        g.w32(h + 0x10, 0x80000000 if rng.random() < 0.8 else 0)
        g.w32(u + 0x5c, h)
        prev = None
        for at in (0x3c, 0x40):
            for _ in range(rng.choice((0, 1, 2))):
                lst = stat_holder(g, rng, [0xac << 16] * 3)
                g.w32(lst + 0x14, rng.choice((0x69, 0x69, 3, 0)))
                if prev is None:
                    g.w32(h + at, lst)
                else:
                    g.w32(prev + 0x2c, lst)
                prev = lst
            prev = None
    return u


def unit_relation(g, rng, count):
    """465c60(ecx=a, edx=b): units of every type, the same unit twice, ids
    alike and not, the roster ([0x7bb5bc]: +8 id, +0xc its player, +0x30 the
    next), monsters with flags 4 and 5, alignments; a town and two players
    (both left to the game's code), a null unit."""
    for i in range(count):
        g.begin()
        room = relation_world(g, rng)
        a = relation_unit(g, rng, room)
        b = a if rng.random() < 0.08 else relation_unit(g, rng, room)
        if rng.random() < 0.3:   # the same id
            g.put(b + 0xc, g.e.read(a + 0xc, 4))
        prev = None
        for _ in range(rng.choice((0, 0, 1, 3))):
            e = g.block(0x40)
            g.put(e + 8, g.e.read(rng.choice((a, b)) + 0xc, 4) if rng.random() < 0.6 else struct.pack("<I", rng.randint(0, 40)))
            g.w32(e + 0xc, rng.choice((1, 2, 3, rng.randint(0, 40))))
            if prev is None:
                g.w32(0x007bb5bc, e)
            else:
                g.w32(prev + 0x30, e)
            prev = e
        if i % 31 == 0:
            a = 0
        yield g.run(0x00465c60, (), ecx=a, edx=b)


def unit_pick_filter(g, rng, count):
    """466870(ecx=unit, edx, [8], [c]): units with and without +0xc4's bit 21
    and +0xc8's bit 7 (those the native answers; the rest go on to the
    game's code), a null unit; the player ([0x7a6a70]) in a room, which the
    game's code after the tests asks about."""
    for i in range(count):
        g.begin()
        room = relation_world(g, rng)
        player = unit_record(g, rng, 0, room)
        g.w32(player + 0xa8, g.block(0x80))   # its inventory (620190 asserts one)
        g.w32(PLAYER, player)
        u = unit_record(g, rng, rng.randint(0, 5), room)
        g.w32(u + 0xa8, g.block(0x80))
        g.w32(u + 0xc4, rng.getrandbits(32) & rng.choice((0xffffffff, ~(1 << 21) & 0xffffffff, 1 << 21)))
        g.w32(u + 0xc8, rng.getrandbits(32) & rng.choice((0xffffffff, ~0x80 & 0xffffffff, 0x80)))
        yield g.run(0x00466870, (rng.getrandbits(8), rng.getrandbits(8)), ecx=0 if i % 29 == 0 else u,
                    edx=rng.getrandbits(8))


def skill_level(g, rng, count):
    """644180(esi=unit, [8]=skill): the skill's base level (+0x2c), +0x28, its
    record ([skill]: the id at +0, the class at +0xc, +0x1dc), the skills
    table (+0xb98 rows of 0x23c, +0xba0 of them; +0x194 the description's
    index into +0xb8c's rows of 0x120, +0xb94 of them; the tab at +2), state
    0x86 (+0xc states: holder +0x58's bits), and the unit's stats 0x7f, 0x53,
    0xbc, 0x61, 0x7e, 0x6b; players of the skill's class and not, monsters,
    no description (left to the game's code), null records."""
    for i in range(count):
        g.begin()
        tables = g.block(0xc00)
        g.w32(0x00744304, tables)
        stat_rows(g, rng, tables, rng.choice((0xbd, 0xbd, 0x80)))
        g.w32(tables + 0xc4, rng.choice((0x87, 0x100, 0x40)))
        nskill, ndesc = rng.choice((1, 10, 40)), rng.choice((0, 5, 40))
        skills, descs = g.block(nskill * 0x23c), g.block(max(ndesc, 1) * 0x120)
        g.w32(tables + 0xb98, skills); g.w32(tables + 0xba0, nskill)
        g.w32(tables + 0xb8c, descs); g.w32(tables + 0xb94, ndesc)
        for k in range(nskill):
            row = skills + k * 0x23c
            g.w16(row, k)
            g.w8(row + 0xc, rng.choice((0, 1, 2, 3, 4, 5, 6, 0xff)) if rng.random() < 0.4 else 3)   # often the unit's
            g.w8(row + 0x1dc, rng.choice((0, 0, rng.randint(1, 40))))
            g.w16(row + 0x194, rng.choice((rng.randrange(max(ndesc, 1)), rng.randrange(max(ndesc, 1)), -1, ndesc)))
        for k in range(ndesc):
            g.w8(descs + k * 0x120 + 2, rng.choice((0, 1, 2, 3)))
        sid = rng.choice((rng.randrange(nskill), rng.randrange(nskill), -1, nskill))
        skill = g.block(0x40)
        if i % 19:
            rec = skills + (sid % nskill) * 0x23c if 0 <= sid < nskill else g.block(0x200)
            if not 0 <= sid < nskill:
                g.w16(rec, sid)
            g.w32(skill, rec)
        g.w32(skill + 0x2c, rng.randint(0, 30))
        g.w32(skill + 0x28, rng.choice((0, 0, 1, -1)))
        utype = rng.choice((0, 0, 0, 0, 1, 1, 3, 2))
        unit = unit_record(g, rng, utype)
        g.w32(unit + 4, rng.choice((0, 1, 2, 3, 4, 5, 6)) if rng.random() < 0.4 else 3)
        cls = struct.unpack("<I", g.e.read(unit + 4, 4))[0]
        keys = [0x7f << 16, 0x53 << 16 | cls] + [0xbc << 16 | (t + cls * 8 - 1) & 0xffff for t in (1, 2, 3)] + \
            [0x61 << 16 | (sid & 0xffff), 0x6b << 16 | (sid & 0xffff)] + [0x7e << 16 | k for k in range(1, 41)][:rng.randint(0, 40)]
        if rng.random() < 0.9:
            h = stat_holder(g, rng, keys)
            bits = g.block(0x40)
            g.put(bits, bytes(rng.getrandbits(8) for _ in range(0x40)))
            g.w32(h + 0x58, bits)
            g.w32(unit + 0x5c, h)
        yield g.run(0x00644180, (skill,), esi=0 if i % 37 == 0 else unit)


# ---- string tables ---------------------------------------------------------------------

def elf_hash(key, size):
    """5249e0: the ELF hash of the key's (signed) bytes, modulo size."""
    h = 0
    for b in key:
        h = ((h << 4) + (b - 256 if b >= 0x80 else b)) & 0xffffffff
        hi = h & 0xf0000000
        if hi:
            h = ((hi >> 24) ^ h) & 0x0fffffff
    return h % size


def lang_lookup(g, rng, count):
    """524c60(edi=table, [8]=string pointers, [c]=key, [10]=&string): a D2Lang
    table (+2 the index array's count, +4 the nodes, +0xd the probe limit; at
    +0x15 the index array, then nodes of 17 bytes: +0 used, +1 the string's
    index, +7 the key's offset from the table), filled as the game fills it
    (from the key's bucket to the next free node); keys present, missing,
    twice (the nearer one is the answer), far from their bucket (past the
    limit), with high bytes; limits from 0 to past the size. Each case's table
    sits at its own address: the native keeps an index per table address and
    shape, which the game's tables (loaded once) never change under it."""
    for i in range(count):
        g.begin()
        g.alloc(16 * (i + 1))   # a new address each case
        size = rng.choice((1, 2, 7, 64, 300, 1500))
        nkeys = rng.randint(0, size)
        words = []
        for _ in range(nkeys):
            n = rng.randint(1, 12)
            w = bytes(rng.choice((rng.randint(0x61, 0x7a), rng.randint(0x30, 0x39), 0x5f, rng.randint(0x80, 0xff)))
                      for _ in range(n))
            words.append(w if not words or rng.random() > 0.25 else rng.choice(words))   # some keys twice
        index_n = rng.randint(0, 8)
        table = g.alloc(0x15 + 2 * index_n + 17 * size + 16 + sum(len(w) + 1 for w in words) + 64)
        nodes = table + 0x15 + 2 * index_n
        text = nodes + 17 * size + 16
        body = bytearray(17 * size)
        used = [False] * size
        at = text
        dists = {}
        for k, w in enumerate(words):
            j = elf_hash(w, size)
            if rng.random() < 0.05:
                j = rng.randrange(size)   # somewhere else: out of its bucket's reach, perhaps
            for _ in range(size):
                if not used[j]:
                    break
                j = (j + 1) % size
            else:
                break
            used[j] = True
            g.put(at, w + b"\0")
            dists.setdefault(w, []).append((j - elf_hash(w, size)) % size)
            body[17 * j] = 1 if rng.random() < 0.97 else 2
            struct.pack_into("<H", body, 17 * j + 1, rng.getrandbits(16))
            struct.pack_into("<I", body, 17 * j + 7, at - table)
            at += len(w) + 1
        g.put(nodes, body)
        strings = g.alloc(4 * size)
        g.put(strings, b"".join(struct.pack("<I", rng.randint(0x01000000, 0x01ffffff)) for _ in range(size)))
        if words and rng.random() < 0.6:
            key_text = rng.choice(words)
        else:
            key_text = bytes(rng.randint(0x61, 0x7a) for _ in range(rng.randint(0, 10)))
        probes = rng.choice((0, 1, 3, size // 2, size, size, size + 5))
        if key_text in dists and rng.random() < 0.4:   # the limit at the key's nodes: just in reach or just not
            probes = rng.choice(dists[key_text]) + rng.choice((0, 1))
        hdr = bytearray(0x15 + 2 * index_n)
        struct.pack_into("<H", hdr, 2, index_n)
        struct.pack_into("<I", hdr, 4, size)
        struct.pack_into("<I", hdr, 0xd, probes)
        g.put(table, hdr)
        key = g.alloc(len(key_text) + 1)
        g.put(key, key_text + b"\0")
        out = g.block(4)
        yield g.run(0x00524c60, (strings, key, out), edi=table)


# ---- the sprite cache and the locked lookups -------------------------------------------

def cache_touch(g, rng, count):
    """5ff5b0(ecx=cache, edx=entry): the cache (+0xc on, +0x4c hits, +0x14 /
    +0x18 the main list's head and tail, +0x1c / +0x20 the other's) and its
    entries (+0xc flags: bit 0 the other list, bit 1 a pinned head; +0x14
    next, +0x18 previous, +0x10 the stamp): an entry at either list's head,
    tail or middle, alone, in no list; pinned heads; the cache off. The stamp
    is GetTickCount's (the emulator's 0; the runner takes the native's clock
    for it)."""
    for _ in range(count):
        g.begin()
        cache = g.block(0x60)
        g.w32(cache + 0xc, rng.random() < 0.9)
        g.w32(cache + 0x4c, rng.getrandbits(16))
        entries = []
        for head_at, flag in ((0x14, 0), (0x1c, 1)):
            n = rng.choice((0, 1, 2, 3, 8))
            lst = [g.block(0x20) for _ in range(n)]
            for k, e in enumerate(lst):
                g.w32(e + 0xc, flag | (2 if k == 0 and rng.random() < 0.2 else 0) | rng.getrandbits(4) << 4)
                g.w32(e + 0x10, rng.getrandbits(32))
                g.w32(e + 0x14, lst[k + 1] if k + 1 < n else 0)
                g.w32(e + 0x18, lst[k - 1] if k else 0)
            if lst:
                g.w32(cache + head_at, lst[0])
                g.w32(cache + head_at + 4, lst[-1])
            entries += lst
        loose = g.block(0x20)
        g.w32(loose + 0xc, rng.getrandbits(2))
        entry = rng.choice(entries) if entries and rng.random() < 0.9 else loose
        yield g.run(0x005ff5b0, (), ecx=cache, edx=entry)


USE_DCC, LOCALE_CHANGED, DC6_NAME = 0x00741efc, 0x0096dfe4, 0x006e363c


def sprite_path(g, rng, count):
    """5fe610(ebx=out, esi=sprite info, [8]=name): the file's path for units
    of every type (+8; 3, 4, 6 go on to the game's code), classes the
    monster tables single out (0xf2..0x16f, 0x209, 0x220..0x2c5) and the
    objects' (0x156, 0x233), mode 0 and not (+0x10), tokens with spaces
    (+0x18, +0x1c), .dcc on and off ([0x741efc]), the one .dc6 name in any
    case, an empty name."""
    dc6 = bytes(g.e.read(DC6_NAME, 32)).split(b"\0")[0]
    for _ in range(count):
        g.begin()
        g.w32(USE_DCC, rng.random() < 0.8)
        g.w32(LOCALE_CHANGED, rng.random() < 0.05)
        info = g.block(0x50)
        utype = rng.choice((0, 0, 1, 1, 1, 1, 2, 2, 3, 4, 5, 6))
        g.w32(info + 8, utype)
        if utype == 1:
            cls = rng.choice((rng.randint(0xf0, 0x171), rng.randint(0x21e, 0x2c7), 0x209, rng.randint(0, 0x300)))
        elif utype == 2:
            cls = rng.choice((0x156, 0x233, rng.randint(0, 0x300)))
        else:
            cls = rng.randint(0, 8)
        g.w32(info + 0xc, cls)
        g.w32(info + 0x10, rng.choice((0, 0, 1, rng.randint(2, 20))))
        tok = lambda: bytes(rng.choice((rng.randint(0x41, 0x5a), rng.randint(0x30, 0x39), 0x20)) for _ in range(3))
        g.put(info + 0x18, tok() + b"\0")
        g.put(info + 0x1c, tok() + b"\0")
        g.w32(info + 0x44, rng.getrandbits(4))
        kind = rng.random()
        if kind < 0.05:
            text = b""
        elif kind < 0.2:
            text = bytes(c ^ 0x20 if 0x41 <= (c | 0x20) - 0x20 <= 0x5a and rng.random() < 0.5 else c for c in dc6)
        else:
            text = bytes(rng.randint(0x61, 0x7a) for _ in range(rng.randint(1, 40)))
        name = g.alloc(len(text) + 1)
        g.put(name, text + b"\0")
        out = g.block(0x200)
        yield g.run(0x005fe610, (name,), ebx=out, esi=info)


LOCK_TABLE, CUR_ENTRY, CUR_ID = 0x00883d38, 0x00882d34, 0x00882d38


def lock_table(g, rng):
    """[0x883d38]: +0x1c buckets of 12 bytes (+0 the link's offset in an
    entry, +8 the first entry; the next at link + 4, a list ending at <= 0),
    +0x24 the mask; entries' id at +0, their lock at +0x18. Returns the ids."""
    if rng.random() < 0.05:
        return []
    table = g.block(0x60)
    g.w32(LOCK_TABLE, table)
    mask = rng.choice((0, 1, 7, 63))
    buckets = g.block(12 * (mask + 1))
    g.w32(table + 0x1c, buckets)
    g.w32(table + 0x24, mask)
    ids = []
    for b in range(mask + 1):
        off = rng.choice((0x20, 0x28, 0x30))
        g.w32(buckets + 12 * b, off)
        prev = None
        for _ in range(rng.choice((0, 0, 1, 2, 5))):
            e = g.block(0x40)
            eid = rng.randrange(1 << 16) * (mask + 1) + b
            ids.append(eid)
            g.w32(e, eid)
            g.w32(e + 0x18, g.block(0x18) if rng.random() < 0.97 else 0)
            g.w32(e + off + 4, rng.choice((0, 0, 0xfffffff0)))   # the end: 0 or below
            if prev is None:
                g.w32(buckets + 12 * b + 8, e)
            else:
                g.w32(prev + off + 4, e)
            prev = e
        if prev is None and rng.random() < 0.2:
            g.w32(buckets + 12 * b + 8, 0xffffff00)   # an empty bucket's end mark
    return ids


def locked_lookup(g, rng, count):
    """52e860(ecx=id): ids in the table, missing, in a bucket's list at
    either end; no table; an entry without its lock (the game's fatal
    error)."""
    for _ in range(count):
        g.begin()
        ids = lock_table(g, rng)
        eid = rng.choice(ids) if ids and rng.random() < 0.7 else rng.getrandbits(rng.choice((4, 20, 32)))
        yield g.run(0x0052e860, (), ecx=eid)


def current_lookup(g, rng, count):
    """52f6e0(): the current id ([0x882d38]; 0 and -1 none) looked up, the
    entry stored at [0x882d34]."""
    for _ in range(count):
        g.begin()
        ids = lock_table(g, rng)
        g.w32(CUR_ID, rng.choice(ids) if ids and rng.random() < 0.7 else rng.choice((0, 0xffffffff, rng.getrandbits(16))))
        g.w32(CUR_ENTRY, rng.getrandbits(32))
        yield g.run(0x0052f6e0, ())


def current_unlock(g, rng, count):
    """52df40(): LeaveCriticalSection on the current entry's lock; no entry
    is the game's fatal error."""
    for i in range(count):
        g.begin()
        e = g.block(0x40) if i % 13 else 0
        if e:
            g.w32(e + 0x18, g.block(0x18) if i % 7 else 0)
        g.w32(CUR_ENTRY, e)
        yield g.run(0x0052df40, ())


SPRITE_LIST, LOAD_MAGIC = 0x0089db7c, 0x41737960
# Two events the runner registers (WinVita's first handles): one never
# signalled, one signalled. Any other handle is unknown to both sides.
EVENT_WAITING, EVENT_SET = 0x82000000, 0x82000001


def loads_pending(g, rng, count):
    """5ff2a0(): the sprite loads not done, over the cache's list
    ([0x89db7c], next at +0x14): entries without a sprite, sprites marked
    (+0x20 bit 0), loads (+0xb0) done (+0x48: the magic, bit 0). A load not
    done polls its event (+0x38, the handle's address) with
    WaitForSingleObject(h, 0): the waiting event times out, and the native
    counts it; the signalled one, or a handle the native doesn't know, goes
    on to the game's code (which finishes the load)."""
    g.answer("WaitForSingleObject", lambda e: (0x102 if e.arg(0) == EVENT_WAITING else 0, 2))
    for _ in range(count):
        g.begin()
        prev = None
        for _ in range(rng.choice((0, 1, 3, 10, 40))):
            e = g.block(0x20)
            r = rng.random()
            if r > 0.1:
                sp = g.block(0xc0)
                g.w8(sp + 0x20, rng.random() < 0.3)
                ld = g.block(0x50)
                g.w32(sp + 0xb0, ld)
                g.w32(ld + 0x48, LOAD_MAGIC | (1 if rng.random() < 0.6 else 0) | rng.getrandbits(3) << 1)
                event = g.block(4)
                g.w32(event, rng.choice((EVENT_WAITING,) * 12 + (EVENT_SET, rng.randint(0x100, 0x1000))))
                g.w32(ld + 0x38, event)
                g.w32(e, sp)
            if prev is None:
                g.w32(SPRITE_LIST, e)
            else:
                g.w32(prev + 0x14, e)
            prev = e
        yield g.run(0x005ff2a0, ())


# ---- the blitters -----------------------------------------------------------------------
# A small frame buffer per case (random pixels: the blends read them), its
# rows FB_PITCH apart, and streams that stay inside it.

FB_PITCH = 512
PITCH, FB_BASE, ROW_OFF = 0x007d544c, 0x007d5448, 0x007c9828
CLIP_L, CLIP_R, CLIP_T, CLIP_B = 0x007ca318, 0x007d233c, 0x007d2334, 0x007c97a0


def frame_buffer(g, rng, rows, palette=None):
    fb = g.alloc(FB_PITCH * (rows + 4))
    n = FB_PITCH * (rows + 4)
    g.put(fb, bytes(rng.choice(palette) for _ in range(n)) if palette else bytes(rng.getrandbits(8) for _ in range(n)))
    return fb


def table(g, rng, n, palette=None):
    """A remap table (256), or a blend table (0x10000, indexed by the frame
    buffer's pixel * 256): with a palette (the frame buffer's only pixel
    values) only its rows are written, the rest left zero; the cases stay
    small, and both sides read the same table either way."""
    t = g.alloc(n)
    if n == 0x10000 and palette:
        for v in sorted(set(palette)):
            g.put(t + 256 * v, bytes(rng.getrandbits(8) for _ in range(256)))
    else:
        g.put(t, bytes(rng.getrandbits(8) for _ in range(n)))
    return t


def dc6_stream(rng, rows, width=300):
    """DC6 rows: literals (0x00..0x7f and their bytes; 0 among them), skips
    (0x81..0xff), each row ended by 0x80; at most `width` pixels a row."""
    out = bytearray()
    for _ in range(rows):
        x = 0
        for _ in range(rng.choice((0, 1, 2, 4, 8))):
            if rng.random() < 0.4:
                n = min(rng.choice((1, 2, 7, 8, 9, 50, 127)), width - x)
                if n < 1:
                    break
                out.append(0x80 | n)
            else:
                n = min(rng.choice((0, 1, 3, 7, 8, 9, 15, 16, 17, 40, 127)), width - x)
                out.append(n)
                out += bytes(rng.getrandbits(8) for _ in range(n))
            x += n
        out.append(0x80)
    return bytes(out)


def dc6_call(g, rng, clipped, palette=None):
    """A DC6 blit's common part: the stream, rows, the frame buffer and the
    destination (bottom-up rows: the pitch negative, as the game's; or not),
    and for the clipped ones x0, clipL, clipR around the row."""
    rows = rng.choice((1, 2, 5, 16, 40))
    fb = frame_buffer(g, rng, rows, palette)
    stream = dc6_stream(rng, rows)
    src = g.alloc(len(stream) + 16)
    g.put(src, stream)
    up = rng.random() < 0.7
    x = rng.randint(0, FB_PITCH - 300)
    dst = fb + FB_PITCH * ((rows + 1) if up else 1) + x
    pitch = -FB_PITCH if up else FB_PITCH
    args = [rows, pitch]
    if clipped:
        x0 = rng.randint(-100, 400)
        cl = rng.choice((0, rng.randint(-50, 300), x0, x0 + 5))
        cr = rng.choice((cl, cl + rng.randint(1, 300), rng.randint(-50, 640), 640))
        args += [x0, cl, cr]
    return src, dst, args


def dc6_blit(addr, clipped, tables):
    """A generator for one DC6 blitter at addr: tables are the sizes of its
    table arguments (256: a remap, 0x10000: a blend)."""
    def gen(g, rng, count):
        for _ in range(count):
            g.begin()
            palette = [rng.getrandbits(8) for _ in range(8)] if 0x10000 in tables else None
            src, dst, args = dc6_call(g, rng, clipped, palette)
            args += [table(g, rng, n, palette) for n in tables]
            yield g.run(addr, tuple(args), ecx=src, edx=dst)
    gen.__doc__ = f"{addr:x}(ecx=src, edx=dst, [8]=rows, [c]=pitch{', x0, clipL, clipR' if clipped else ''}" \
                  f"{''.join(', table' for _ in tables)})"
    return gen


def dc6_shadow(g, rng, count):
    """608d60(ecx=frame, edx=dst, [8]=x, [c]=clipL, [10]=clipR, [14]=width,
    [18]=rows to skip / 2, [1c]=rows, [20]=T, [24]=c): the frame's stream at
    +0x20 (signed: b >= 0 a literal, 0x81..0xff a skip, 0x80 a row's end),
    its first 2 * [18] rows passed over, then every other row drawn a row up
    and a pixel left; clips both 0 (none) or not; a blend table or none."""
    for _ in range(count):
        g.begin()
        rows, skip = rng.choice((1, 2, 5, 20)), rng.choice((0, 0, 1, 3))
        palette = [rng.getrandbits(8) for _ in range(8)]
        fb = frame_buffer(g, rng, rows, palette)
        stream = dc6_stream(rng, 2 * skip + 2 * rows + 1, 200)
        frame = g.alloc(0x20 + len(stream) + 16)
        g.put(frame + 0x20, stream)
        x = rng.randint(rows + 2, FB_PITCH - 210)
        dst = fb + FB_PITCH * (rows + 1) + x
        cl, cr = rng.choice(((0, 0), (0, 0), (rng.randint(0, 300), rng.randint(0, 640)), (x, x + 100)))
        t = table(g, rng, 0x10000, palette) if rng.random() < 0.6 else 0
        yield g.run(0x00608d60, (x, cl, cr, FB_PITCH, skip, rows, t, rng.getrandbits(8)), ecx=frame, edx=dst)


def floor_globals(g, rng, fb, rows):
    g.w32(PITCH, FB_PITCH)
    g.w32(FB_BASE, fb)
    g.w32(CLIP_L, rng.choice((0, rng.randint(0, 200))))
    g.w32(CLIP_R, rng.choice((640, rng.randint(100, 640))))
    g.w32(CLIP_T, rng.choice((0, rng.randint(0, rows))))
    g.w32(CLIP_B, rng.choice((480, rng.randint(0, rows + 4))))


def floor_iso_flat(g, rng, count):
    """4f69b0(ecx=src, edx=dst, [8]=tbl): a 256-byte iso block, 15 rows of
    the game's shape (its tables 0x72db48...), remapped, at the pitch
    [0x7d544c]."""
    for _ in range(count):
        g.begin()
        fb = frame_buffer(g, rng, 16)
        g.w32(PITCH, FB_PITCH)
        src = table(g, rng, 256)
        yield g.run(0x004f69b0, (table(g, rng, 256),), ecx=src, edx=fb + rng.randint(0, FB_PITCH - 80))


def rle_pairs(rng, rows, width=300):
    """The floor's RLE: (skip, run) pairs and the run's bytes; (0, 0) ends a
    row; runs of 0 and over 32 among them (drawn as nothing)."""
    out = bytearray()
    for _ in range(rows):
        x = 0
        for _ in range(rng.choice((0, 1, 2, 5))):
            skip = rng.choice((0, 1, 5, 30))
            run = rng.choice((0, 1, 4, 8, 31, 32, 33, 40))
            if skip == 0 and run == 0:
                run = 1
            if x + skip + run > width:
                break
            out += bytes((skip, run)) + bytes(rng.getrandbits(8) for _ in range(run))
            x += skip + run
        out += b"\0\0"
    return bytes(out)


def rle_flat(g, rng, count):
    """4f6f60(ecx=src, edx=dst, [8]=tbl, [c]=rows)."""
    for _ in range(count):
        g.begin()
        rows = rng.choice((1, 2, 8, 32))
        fb = frame_buffer(g, rng, rows)
        g.w32(PITCH, FB_PITCH)
        data = rle_pairs(rng, rows)
        src = g.alloc(len(data) + 16)
        g.put(src, data)
        yield g.run(0x004f6f60, (table(g, rng, 256), rows), ecx=src, edx=fb + rng.randint(0, FB_PITCH - 300))


def rle_flat_clipped(g, rng, count):
    """4f7580(ecx=x0, edx=y0, [8]=src, [c]=dst, [10]=tbl, [14]=rows): the
    window's clip ([0x7ca318] left, [0x7d233c] right, [0x7d2334] top,
    [0x7c97a0] bottom) across the block's rows and columns."""
    for _ in range(count):
        g.begin()
        rows = rng.choice((1, 2, 8, 32))
        fb = frame_buffer(g, rng, rows)
        floor_globals(g, rng, fb, rows)
        data = rle_pairs(rng, rows)
        src = g.alloc(len(data) + 16)
        g.put(src, data)
        x0 = rng.randint(-40, 400)
        yield g.run(0x004f7580, (src, fb + rng.randint(0, FB_PITCH - 300), table(g, rng, 256), rows),
                    ecx=x0, edx=rng.randint(-8, rows))


def wall_block_copy(g, rng, count):
    """4f7ea0(ecx=src, edx=x, [8]=y): a 32-row RLE block (runs copied as
    they are, any length) at (x, y) through the row offsets ([0x7c9828]
    [y]) from the frame buffer ([0x7d5448]); rejected outside one rectangle
    (0x7d232c, 0x7c978c, 0x7c9794, 0x7d2324), drawn unclipped inside another
    (0x7ca31c, 0x7c9790, 0x7d2328, 0x7c9784), else clipped by the window."""
    for _ in range(count):
        g.begin()
        fb = frame_buffer(g, rng, 32)
        floor_globals(g, rng, fb, 32)
        y = rng.randint(0, 40) - 4
        for k in range(32):   # [y + k]: the block's rows from the buffer's third
            g.w32(ROW_OFF + 4 * (y + k), (k + 2) * FB_PITCH)
        x = rng.randint(-20, FB_PITCH - 300)
        # each edge on the block's corner, a step either side, or far
        rect = lambda at: (rng.choice((at - 50, at - 1, at, at + 1)), rng.choice((at - 1, at, at + 1, at + 600)))
        t, b = rect(y) if rng.random() < 0.3 else (y - 50, y + 600)
        l, r = rect(x) if rng.random() < 0.3 else (x - 50, x + 600)
        g.w32(0x007d232c, t); g.w32(0x007c978c, b); g.w32(0x007c9794, l); g.w32(0x007d2324, r)
        it, ib = rect(y)
        il, ir = rect(x)
        g.w32(0x007ca31c, it); g.w32(0x007c9790, ib); g.w32(0x007d2328, il); g.w32(0x007c9784, ir)
        data = rle_pairs(rng, 32, 250)
        src = g.alloc(len(data) + 16)
        g.put(src, data)
        yield g.run(0x004f7ea0, (y,), ecx=src, edx=x)


# ---- the light grid's cells and the floor's tiles ---------------------------------------

def light_grid(g, rng, count):
    """4756d0(): the light grid's cells (48 x 48 from [0x7b0a54], [0x7b0a58])
    whose subtile's collision has 0x22 set, through the rooms from the
    player's ([0x7a6a70], its path's room): the grid on the rooms, across
    their edges, off them; no player, a player without a room."""
    for i in range(count):
        g.begin()
        rooms, (x0, y0, x1, y1) = world(g, rng)
        light_globals(g, rng)
        g.w32(GRID_X, (rng.randint(x0 - 60, x1 + 10)) >> 0)
        g.w32(GRID_Y, (rng.randint(y0 - 60, y1 + 10)) >> 0)
        if i % 23:
            player = unit_record(g, rng, 0, rng.choice(rooms)[0] if i % 29 else None)
            g.w32(PLAYER, player)
        yield g.run(0x004756d0, ())


def light_block(g, rng, count):
    """4ddef0(eax=dst, [8]=tx, [c]=ty, [10]=ambient 0): the light of 8 x 8
    subtiles around a tile from the light grid (byte 4, and 5-7 with
    coloured light), cells on, across and off the grid's edges. (With an
    ambient source the native calls two of the game's functions itself:
    not cased here.)"""
    for _ in range(count):
        g.begin()
        gx, gy = light_globals(g, rng)
        dst = g.alloc(64 * 12 + 16)
        g.put(dst, bytes(rng.getrandbits(8) for _ in range(64 * 12 + 16)))
        tx, ty = gx + rng.randint(-3, 50), gy + rng.randint(-3, 50)
        yield g.run(0x004ddef0, (tx, ty, 0), eax=dst)


SHADES, FLAT_LIGHT, NO_GOURAUD = 0x007d2348, 0x007d2340, 0x0072da50
REJ = (0x007d2330, 0x007d2338, 0x007c9788, 0x007c9798)      # top, bottom, left, right
INSIDE = (0x007c9780, 0x007c979c, 0x007c97a4, 0x007d2320)


def floor_tile(g, rng, count):
    """4f8b80(ecx=tile, edx=x, [8]=y, [c]=light buffer): a tile's blocks
    (+0x50 of them, 20 bytes each from +0x54: offset, light cell, flags 1
    drawn and 4 RLE, pixels at +0x10), iso and RLE, drawn or not; each
    rejected outside one rectangle, drawn unclipped inside another, else
    clipped by the window; flat light only (the buffer's cell, interpolation
    off, or the four neighbours within 10): interpolated light runs the
    game's own blitters from the native, which this runner cannot."""
    for _ in range(count):
        g.begin()
        rows = 60
        fb = frame_buffer(g, rng, rows)
        g.w32(PITCH, FB_PITCH)
        g.w32(FB_BASE, fb)
        g.put(SHADES, bytes(rng.getrandbits(8) for _ in range(32 * 256)))
        mode = rng.randrange(3)
        g.w32(FLAT_LIGHT, mode == 0)
        g.w32(NO_GOURAUD, mode == 1)
        base = rng.getrandbits(8)
        light = g.alloc(1072 + 16)
        g.put(light, bytes(max(0, min(255, base + rng.randint(0, 2))) if mode == 2 else rng.getrandbits(8)
                           for _ in range(1072 + 16)))
        x, y = rng.randint(0, 64), rng.randint(0, 8)
        n = rng.choice((0, 1, 4, 10))
        blocks = g.block(20 * max(n, 1))
        for k in range(n):
            e = blocks + 20 * k
            ex, ey = rng.randint(0, 200), rng.randint(0, 30)
            g.w16(e, ex); g.w16(e + 2, ey)
            g.w8(e + 6, rng.randint(0, 7)); g.w8(e + 7, rng.randint(0, 6))
            rle = rng.random() < 0.4
            g.w8(e + 8, (1 if rng.random() < 0.9 else 0) | (4 if rle else 0) | rng.getrandbits(1) << 1)
            if rle:
                data = rle_pairs(rng, 15, 64)
            else:
                data = bytes(rng.getrandbits(8) for _ in range(256))
            src = g.alloc(len(data) + 16)
            g.put(src, data)
            g.w32(e + 0x10, src)
        tile = g.block(0x60)
        g.w32(tile + 0x50, n)
        g.w32(tile + 0x54, blocks)
        sx, sy = x + 100, y + 15   # about the blocks' middle, in screen terms
        far = lambda at: (rng.choice((at - 400, at - rng.randint(0, 120))), rng.choice((at + 400, at + rng.randint(0, 120))))
        for (t, b), (l, r), where in ((far(sy), far(sx), REJ), (far(sy), far(sx), INSIDE)):
            g.w32(where[0], t); g.w32(where[1], b); g.w32(where[2], l); g.w32(where[3], r)
        g.w32(CLIP_L, rng.choice((0, rng.randint(0, 250))))
        g.w32(CLIP_R, rng.choice((640, rng.randint(50, 320))))
        g.w32(CLIP_T, rng.choice((0, rng.randint(0, 40))))
        g.w32(CLIP_B, rng.choice((480, rng.randint(5, 60))))
        yield g.run(0x004f8b80, (y, light), ecx=tile, edx=x)


# name: (generator, flags). EAX is dead after the light stamps (their one
# caller overwrites it), so it is not compared there.
NATIVES = {
    "StormHash": (storm_hash, COMPARE_EAX),
    "StatGet": (stat_get, COMPARE_EAX),
    "LightAdd": (light_add, 0),
    "LightStamp": (light_stamp, 0),
    "LightStampOccluded": (light_stamp_occluded, 0),
    "RoomFind": (room_find, COMPARE_EAX),
    "CollisionCross": (collision_cross, COMPARE_EAX),
    "UnitWalk": (unit_walk, COMPARE_EAX),
    "PathSearch": (path_search, COMPARE_EAX),
    "AdpcmDecode": (adpcm, COMPARE_EAX),
    # Both reached only through Storm's decompressor table (0x6cfed8,
    # 0x6cfee8), whose two callers overwrite EAX at once (0x41e69d,
    # 0x41e885): it is SMemFree's leftover in the game, not a result.
    "HuffmanDecode": (huffman, 0),
    "PkExplode": (pk_explode, 0),
    "UnitRelation": (unit_relation, COMPARE_EAX),
    "UnitPickFilter": (unit_pick_filter, COMPARE_EAX),
    "SkillLevel": (skill_level, COMPARE_EAX),
    "LangLookup": (lang_lookup, COMPARE_EAX),
    # Its only caller reads no register after it.
    "SpriteCacheTouch": (cache_touch, 0),
    "SpritePath": (sprite_path, COMPARE_EAX),
    "LockedLookup": (locked_lookup, COMPARE_EAX),
    "CurrentLookup": (current_lookup, COMPARE_EAX),
    "CurrentUnlock": (current_unlock, COMPARE_EAX),
    "LoadsPending": (loads_pending, COMPARE_EAX),
    # The floor's and walls' EAX is dead: their callers return it to
    # FloorTile's loop (4f8b80, overwritten at 4f8d8f) or to the renderer's
    # calls through the tile table (0x72da60, from [0x7c914c]: +4 at 513065
    # and 6c9365, +0x10 at 513287 and 6c9587, overwritten at once).
    "FloorIsoFlat": (floor_iso_flat, 0),
    "RleFlat": (rle_flat, 0),
    "RleFlatClipped": (rle_flat_clipped, 0),
    "WallBlockCopy": (wall_block_copy, 0),
    "Dc6Copy": (dc6_blit(0x00606d40, False, ()), COMPARE_EAX),
    "Dc6CopyClipped": (dc6_blit(0x00606da0, True, ()), COMPARE_EAX),
    "Dc6Blend": (dc6_blit(0x00606e40, False, (256, 0x10000)), COMPARE_EAX),
    "Dc6BlendClipped": (dc6_blit(0x00607060, True, (256, 0x10000)), COMPARE_EAX),
    "Dc6Remap": (dc6_blit(0x00607e20, False, (256,)), COMPARE_EAX),
    "Dc6RemapB": (dc6_blit(0x00607680, False, (256,)), COMPARE_EAX),
    "Dc6Remap2": (dc6_blit(0x006072f0, False, (256, 256)), COMPARE_EAX),
    "Dc6Blend1": (dc6_blit(0x00608110, False, (0x10000,)), COMPARE_EAX),
    "Dc6Shadow": (dc6_shadow, COMPARE_EAX),
    # Dead EAX too: LightGrid's one caller (475888) calls 475780 next, which
    # sets EAX first (4f5210); LightBlock's two (4de4d9, 4dec1b) overwrite it;
    # FloorTile is renderer entry 31, called only from 4f68e0, whose two
    # callers (4de5b2, 4dec9c) overwrite it before reading it.
    "LightGrid": (light_grid, 0),
    "LightBlock": (light_block, 0),
    "FloorTile": (floor_tile, 0),
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
        print(f"{name}: {len(cases)} cases" + (f", {dropped} left out (the game asserted, faulted or wrote outside)" if dropped else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
