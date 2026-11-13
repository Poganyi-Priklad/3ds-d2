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

MASKS = (0x1, 0x4, 0x804, 0x1805, 0x3c01, 0x1c09, 0x3f11, 0xffff)


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
