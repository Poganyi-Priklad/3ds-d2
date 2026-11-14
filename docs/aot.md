# Ahead-of-time compilation: design note

Status: reviewed (issue #112, part of #111), and its spike built (#113, [below](#the-spike-113)). Figures marked *estimate* are worked out from the measurements named next to them; the spike's measurements replace some of them, and say so.

## The idea in one paragraph

Today Box86 translates Game.exe's x86 code to ARM while the game runs. The translation layer costs about 31 MiB of the app's 124 MiB, and the code it makes is 7.22 times the size of the x86 it came from. An ahead-of-time (AOT) recompiler would run once, inside `scripts/docker.sh package-cia`, on the user's own Game.exe. It would write each game function out as C, compile it with devkitARM's gcc, and link it into the user's own CIA. Box86 stays in the app for whatever the recompiler missed. **We distribute the recompiler, never its output.**

## What we start from

All counts are for 1.14d's Game.exe, measured on 2026-11-13.

| What | Figure | Source |
| --- | --- | --- |
| `.text` | 2,928,640 bytes at `0x401000`–`0x6cc000` | the PE headers |
| Function starts, ours | 13,170 | `tools/profile_report.py`'s `Binary` (call targets, relocated pointers, the natives' sites) |
| Functions with sizes, d2d's | 13,048, 2,600,618 bytes in all (median 106 bytes, the largest 15,513) | `third_party/d2d/docs/research/re/game-functions.tsv` at `0a70175` |
| In both lists / only ours / only d2d's | 11,365 / 1,805 / 1,683 | `tools/function_starts.py` (#107) |
| Instructions in d2d's functions | 878,540 (2.96 bytes on average) | linear disassembly with Capstone, function by function |
| Direct calls, indirect calls, `ret`s | 72,481 / 4,622 / 22,243 | the same pass |
| Indirect `jmp`s, of them through a `[reg*4 + table]` | 710 / 550 | the same pass |
| x87 instructions | 4,223 | the same pass |
| Functions whose bytes don't decode linearly to their end | 374 | the same pass: data in the code, mostly switch tables |
| Imports | 371 | `profile_report.py`'s import table |
| Natives | 49 sites | [natives.md](natives.md) |

What the translation layer costs today ([memory-budget.md](memory-budget.md), "The heap's last MiB is shared" and the ledger above it):

| Part | Size |
| --- | --- |
| Code pool (`dyn86_ctr_jitpool`, `src/app/ctr_game.cpp`) | 14 MiB |
| Jump tables (256 KiB per 64 KiB of guest code that ever ran) | about 12 MiB in play |
| Block records (`meta=`) | 3–4 MiB, growing through a session |
| Translation scratch and first-level tables (`static_insts`, `box86_jmptbl`, `box86_jmptbl_default`) | about 1.4 MiB |
| **In all** | **about 31 MiB** |

The Box86 port's code is 7.22 times its x86 (town scenario, [architecture.md](architecture.md) §7a: 1.29 MB of x86 became 9.31 MB of ARMv6).

## Function discovery

The recompiler needs to know where functions start, where they end, and which bytes in `.text` are not code. Each source below adds starts; none of them is trusted alone.

- **Direct calls.** Every `call rel32` target in `.text`. `profile_report.py` already does this, and it finds strays: an `E8` byte inside another instruction. A start is kept only if its function decodes cleanly from it.
- **Relocations.** Game.exe has 63,121 base relocations.
  - 4,026 of them place a pointer into `.text` in a data section: vtables, callback tables, thread entries.
  - 1,357 are the immediate of a `push` or `mov` in `.text`: callbacks and thread entries passed as arguments.
  - The other 5,926 relocated words in `.text` that point into `.text` are mostly switch-table entries (below). They are jump targets inside a function, not function starts.
- **d2d's list (#107).** Its 13,048 functions come with sizes, which our finder doesn't have. The recompiler reads `game-functions.tsv` from the submodule at build time when it is there. It never copies it into our tree.
- **What the game was seen to run.** The lane's block counts (`WX86_HOTPROF`) and Box86's jump tables after a session list the entry points that actually ran, indirect targets included.
- **The cross-check.** A start that only one source has is reported (`tools/function_starts.py`, #107), and checked by hand while the coverage grows (#115).
  - Of d2d's 1,683 starts that ours lacks, 245 are reached through a relocated pointer in a `mov [mem], imm32`, which our finder doesn't take yet, and 185 only by a `jmp`. The recompiler's finder should take both.

**Switch tables and data in the code.** MSVC puts a switch's jump table (and sometimes a byte index table before it) in `.text`, right after the function. The 550 `jmp [reg*4 + table]` instructions are those switches.
- The recompiler reads each table's entries through its relocations. It bounds the table by the `cmp`/`ja` that guards the index.
- The table's bytes are marked as data, so disassembly stops there. That is most of the 374 functions that don't decode to their end.
- A switch is translated to a C `switch` over the same targets. An index the guard didn't bound goes to the fallback.

**Thunks.** A function that is only a `jmp` to another is compiled as a call to its target. An import is called through its IAT slot (`call [slot]`, or `jmp [slot]` as a thunk). That call goes to our runtime's shim directly (see Calls).

**Patched code.** The runtime changes Game.exe's code before it runs. It writes the hooks' `jmp`s and the in-line sites' `call`s, an immediate (`set_sprite_cache_floor`), stubs in padding (`install_pool_release`, `set_pool_block_size`), and `install_fatal_exit` and `install_ftol`. The recompiler has to see the same code the game runs.
- So these sites need to become one table that the app's installers and the recompiler both read: address, expected bytes, and what replaces them. Today they are spread over `install_*` functions in `src/game/`.
- A site the recompiler doesn't understand leaves its function to Box86.

## Translation model

One x86 function becomes one C function. The C is plain and boring, so that gcc does the optimising.

**Registers.** The eight x86 registers are C locals (`uint32_t eax, ...`). gcc keeps them in ARM registers across the function, which Box86 can't do across its blocks.

**The register interface between compiled functions** (the review's first point, built in the spike). The game passes arguments in registers of its own choosing (`UnitRelation` takes a and b in `ecx` and `edx`, A*'s helpers take theirs in `eax`, `esi`, `edi`), so MSVC's conventions can't be assumed. Each compiled function gets a signature from a liveness pass across functions:
- **in:** the registers it reads before writing them, given its callees' signatures;
- **out:** the registers its compiled callers read after calling it; for a function Box86 calls, MSVC's rule (eax and the callee-saved registers it doesn't keep);
- **preserved:** the registers equal to their entry values at every `ret`, from tracking pushes and pops through the frame.

They become C arguments and results: `esp` and the ins as arguments, one out as the return value, two as a `uint64_t`, more through a pointer. `esp` after the call is the caller's own arithmetic (`+4 + ret N`), checked at run time at each `ret`. The two passes run to a joint fixed point from nothing up.
- **Precision that mattered.** Liveness works on half registers: MSVC's `mov cx, [..]; movzx ecx, cx` reads no more of `ecx` than its low half, and counting the partial write as a read made `ecx` an output of most of A*'s functions. The prologue's saves of callee-saved registers aren't counted as reads (the slot is written only in the context build; see below), but only the entry block's leading run, each popped back into the same register: a `push esi` just before a call is an argument when `esi` still holds its entry value.
- **What other code is assumed to do.** For code Box86 runs, the pass reads its summary where it can, and otherwise assumes MSVC's ABI: `ebx`, `ebp`, `esi` and `edi` survive any call (link-time code generation moves arguments, not those). An indirect call reads at most `ecx` and `edx` (a function whose address is taken keeps a standard convention). An import reads no register; its `ret N` comes from our shims' declared argument counts (`src/runtime/rt_shims_*.cpp`, 296 of the 371 imports, and the `_Name@N` decorations). The game's fatal-error and exit paths (calls that never return, including the C runtime's `exit`, 0x681e09) read nothing. The lane's check compares the callee-saved registers after every checked call, which tests these assumptions.
- **The safe fallback** passes all eight registers through a context struct at every call and stores every push. Both are built from the same pass, and the spike measures the difference.

**Memory.** The same guest memory model as today: the game's address space is one block, and host = guest + base ([natives.md](natives.md): `H(va)`, `U32(va)`, `put32(va, v)` in `src/game/d2_native.cpp`; `g_base` there). Every load and store goes through those helpers, with `memcpy` so unaligned access is legal on the ARM11. The game's structures keep their x86 layout, because the 3DS is 32-bit and little-endian too. There is no other memory model to keep in step with Box86, natives and shims.

**Flags.** They are lazy, and only computed where they are read.
- Per function, a liveness pass finds which instruction reads which flag. `cmp`/`test` followed by `jcc` or `setcc` becomes a C comparison, and no flags are kept.
- Flags that live across a basic block are kept as the last operation's operands and result, and worked out from those when read, as Box86 does.
- 1,880 `adc`/`sbb` instructions read the carry flag, and only 15 instructions move flags to a register (`pushfd`, `popfd`, `lahf`, `sahf`). Those get the full computation.
- At a call, a `ret` or an exit to the dispatcher, the flags are assumed dead, as MSVC's code expects. Over the whole binary, 12 of d2d's functions read flags that a call or their caller left (the spike's survey, below). Those stay with Box86.
- **Which instructions write flags** comes from a table of our own, not Capstone's alone: Capstone 5 reports none for `test` with a memory operand (opcodes 84 and 85, 547 of the 164,075 flag-setting instructions in d2d's functions). Trusting it made a `jne` after `test word ptr [eax], si` read an older `cmp`'s flags, which the host cases caught.
- In the spike's code every reader is a `jcc` after `cmp`, `sub`, `test`, `and` or `add`, or an `sbb` after `neg`. The reader's condition is a compare on the defining instruction's operands, which gcc folds.

**x87.** The game takes its x87 and integer paths, because the port reports no SSE (`RunOptions::cpuid_edx`). So the 4,223 x87 instructions matter, though they are 0.5% of the code.
- The register stack is a small array of `double`s with a top-of-stack index. Within a function whose stack depth is known at each instruction (MSVC's code keeps it balanced), the index is resolved at compile time, and the values become plain C locals.
- Where the depth isn't known, the array is indexed at run time.
- Precision is the same as Box86's on this port: 64-bit doubles on VFPv2, not 80-bit. The game itself sets the x87 to 53-bit precision (MSVC's runtime does, #125 found it: Unicorn starts at 64), so for ordinary arithmetic 64-bit doubles match a real CPU too, not only Box86. They differ where the exponent range does (the x87 keeps its 15-bit exponent in registers even at 53-bit precision: denormals and overflow), and an `fldcw` that changes the precision or rounding has to be honoured.
- Rounding and the `fist`/`fistp` conversions follow Box86's x87 handling on this port, which the check mode compares against.

**Instruction kinds the note didn't cover** (the review's fourth point; counted over d2d's 13,048 functions):

| Instructions | Count | What AOT code does |
| --- | --- | --- |
| `fs:` accesses | 268, in 83 functions | the running thread's TEB through `aot_fs_base()` (`rt::current_teb()`); SEH frames (`fs:[0]`) are memory like any other |
| `rep movs`, `rep stos` | 608 | the copy or fill, element by element where it overlaps forward, with the direction flag known at each instruction (clear at entry and after calls, MSVC's rule; `std`/`cld` change it; a function where it isn't known stays with Box86) |
| `fldcw` | 40 | not in the spike (no x87 there): the function stays with Box86 |
| `cpuid` | 6 | to answer as `RunOptions::cpuid_edx` (not in the spike) |
| `rdtsc` | 1 | from the same source as Box86's (not in the spike) |

The spike's two trees have none of these. The recompiler emits the `fs:` and `rep` forms, but they haven't run on game code yet; x87, `cpuid` and `rdtsc` leave their function to Box86.

**The C runtime's quirks.**
- `_ftol2` (`0x682fd0`): the port already jumps over its SSE2 check, so the `cvttsd2si` path always runs (`install_ftol`). The recompiler turns a call to it into a truncating conversion in line.
- The C runtime's `memcpy` and `memset`: calls to the memory intrinsics' native copies (`D2_MEMINTRIN`, backend patch 0016; `dyn86_mi_arm` in `d2_native.cpp`).
- Stack probes (`_chkstk`/`_alloca_probe`) touch each 4 KiB page of a large frame. In our arena that is only a `sub esp`. The recompiler keeps the touches, which are cheap, so the memory stays the same as under Box86.
- `assert_caller_address` (d2d's name, `0x408090`) reads its own return address. Code like this is why return addresses stay on the guest stack (see Calls).

**Faults.** A guest fault in AOT code (an access outside the arena, a division by zero) has no exact x86 state at the faulting instruction. AOT code reports the function and the offset, and stops the game as an emulator gap does today. The runtime's own `RaiseException` already stops the game. If the lane finds a function that relies on Windows exception handling for a fault, it stays with Box86.

## Calls and returns

- **Direct calls are C calls.** The caller still pushes the x86 return address onto the guest stack, because the game may read it (`assert_caller_address` above) and the fatal-exit native logs its caller from it. Arguments stay on the guest stack. The C call passes nothing but the registers that are live.
- **A `ret` checks before it returns in C.** It pops the return address and compares it with what its C caller pushed. If they match, it is a C return. If they don't (a `longjmp`, an unusual stack), the function exits to the dispatcher with that address. Box86's `callret` mode does the same check.
- **Indirect calls and jumps go through a sparse target table.** It maps a guest address to its compiled entry: about 13,000 function entries, plus the return sites (below). It is an open-addressed hash table, so its size follows the entries and not the address range, unlike Box86's jump tables. *Estimate:* 90,000 entries of 8 bytes, about 0.7 MiB.
- **Return sites are entries too.** After a thread switch or a fallback, a thread can come back to a compiled function in the middle, just after a call. So each call's return site is an entry that continues the function from there. There are about 77,000 call sites. Whether every one needs an entry, or only those a thread can be switched under, is for the spike.
- **Imports go straight to our shims.** The recompiler resolves each IAT slot at build time to the shim's tag. It calls the shim through the same interface the Bridge uses, so a shim doesn't know who called it. The clock reads and the lock calls take the lock-free path first, as from translated code today (below).
- **Natives are C calls.** A call to a hooked function calls its native directly: no trap, no Bridge.
  - In the spike, the call goes to the native's own C function with the registers and stack words it takes, as `aot_native_<va>()` in `d2_native.cpp`: `room_find(ecx, edx, y)` for RoomFind, which covers every call, and `collision_cross(...)` for CollisionCross. Only `eax` changes, as through the trap.
  - A native that hands the call back (CollisionCross on a room's edge) returns "not covered", and the compiled original (0x64d4e0) runs instead: nothing to resume. For a native without a compiled original, the original runs nested under Box86.
  - `D2_NATIVE_MASK` keeps working: with CollisionCross masked off, the compiled 0x64d4e0 runs every time. (A general form would call through a `fn(c)` table, as first planned here; the spike didn't need it.)
- **A callee not compiled runs nested under Box86** in the spike, as natives call the game's code (`rt::guest_call`): the compiled caller writes every register to the context, puts the sentinel where its return address is, runs Box86 from the callee to the sentinel, then reads the registers back and puts its return address back. That's why a root is hooked as a native not served in line. In CofParams' tree these nested runs are frequent (see the spike's results); a full build would compile them or call them through the target table instead.
- **A miss falls back to Box86.** An indirect target that isn't in the table, an index outside a switch's guard, or a function the recompiler left out all end the same way. AOT code writes its state to the CPU context, sets `eip`, and exits to the dispatcher, which runs Box86 from there.
- **Box86 calls into AOT code** through the backend's translation-time intrinsics (`dyn86_intrin.h`).
  - When Box86 translates a block that starts at a compiled function's entry, it emits a direct call to it, with every register in `inmask` and `regmask`.
  - That module's rule applies: it captures a function only if every way into it starts a block at its first byte. Its fallback rule fits too: a helper that can't do the call returns 0, and Box86 runs its own translation.

**Exits to the dispatcher drop the C stack.** When AOT code can't go on (a miss, a yield, the end of a slice), it writes all of its state to the CPU context. That is the eight registers, the flags as `EFLAGS`, the x87 stack in Box86's format, and `eip`. Then it `longjmp`s back to the dispatcher that entered it, and every C frame of compiled callers in between is dropped.
- That is safe because each of those frames' state is on the guest stack already: the x86 return addresses, and anything the x86 code had spilled.
- When the thread runs again, its pending returns land on return-site entries (or on Box86) instead of C returns.
- So the C stack never holds anything a thread switch needs. There is no host stack per guest thread, and no change to the scheduler's single-host-thread model.

## Threads and scheduling

The game's threads (four in play, [memory-budget.md](memory-budget.md)) take turns on core 0 under WinVita's `CooperativeScheduler`, all on one host thread. Two things make a thread stop. Its slice runs out: the quantum counts guest instructions (300,000; less for background threads, `RunOptions::background_quantum`), and Box86 checks the budget in every block's prologue (backend patch 0004). Or a shim must wait: a contended lock, a `Sleep`, a wait on an event.

AOT code keeps both:
- **Preemption points.** Each basic block subtracts its x86 instruction count from the same budget Box86 uses. The budget is checked at function entries and loop back-edges: a function without a loop ends soon anyway. When it runs out, AOT code exits to the dispatcher at that point, with the state written back.
  - On the lane's virtual clock, the switch points decide the map seed and the whole world ([hardware-test.md](hardware-test.md), the `M4_NATIVE=late` note).
  - So AOT code will not switch threads at the same instruction as Box86. A lane run on AOT is compared with frame hashes against another AOT run, and checked against Box86 per function (#114), not frame by frame.
  - A check-every-block mode, if needed, would make AOT switch where Box86 does.
- **Shims that wait.** A shim called from AOT code yields as it does from translated code: it asks the Bridge to stop. On return, AOT code sees the stop request, writes its state back with `eip` at the return site, and exits to the dispatcher.
- **The in-line intrinsics.** Today the clock reads (`GetTickCount`, `timeGetTime`) are served lock-free in line (`set_intrinsic_nogil`, `rt_host.cpp`). The lock calls (`EnterCriticalSection`, `LeaveCriticalSection`) take a lock-free path when uncontended (#48, backend patch 0055), and otherwise the Bridge's in-line shim path. AOT code calls the same functions in the same order: first the lock-free function with an `IntrinFrame`; if it declines, the shim through the Bridge.
  - The "GIL" in these names is inert on the console. The cooperative scheduler never enables it (`gil.h` in WinVita). What the two paths differ in there is the Bridge's work, not a lock.
- **Natives that call back into the game** (`rt::guest_call`) run the callee nested, and it must not yield, as now. A nested callee may be AOT code. An exit to the dispatcher from inside a nested run ends that nested run, as a yield does under Box86 today.

## Self-checks

AOT code has to be checked function by function, as natives are (`D2_NATIVE_CHECK`, [natives.md](natives.md)). #114 builds this lane.

- **Against Box86 on the lane.** Each AOT function runs first, with its stores journaled (a check build of the C goes through a journaling `put32`). Memory is then put back, and the original runs from a trampoline under Box86 on the same input. The lane compares registers, the flags a caller reads, and every byte written, and reports the first difference with its function and input.
- **Against Unicorn on the host (#109).** The generated C is portable: `H()` is base + va on any 32- or 64-bit host. So the same C can be compiled for the hosttest container and run against Unicorn's run of the stock function, on #108's and #109's cases. That needs no console and no lane. It runs in `scripts/docker.sh test`, on the user's Game.exe, with the output kept under `build/`.
- **Per run.** Each AOT function can be switched off at run time (a mask like `D2_NATIVE_MASK`), so a difference found in play can be bisected without a rebuild.

## Building it

- **Where:** in `scripts/docker.sh package-cia <game-dir>`, in a container with networking off, as now.
  - The recompiler (Python, Capstone, the same image as the d2d harness) reads Game.exe, mounted read-only as the art step does. It writes C into `build/aot/<key>/`.
  - devkitARM compiles that C in the toolchain image, and `tests/cpu/ctr/Makefile` links it into `3ds-d2-game-cia.elf`.
- **The cache key:** Game.exe's SHA-256, the recompiler's version, the site table, and the toolchain lock. Packaging again with the same files reuses the objects.
- **The app checks the Game.exe it runs.** The AOT code is only valid for the bytes it was made from. The app hashes the Game.exe it loads, and if it doesn't match the CIA's key it runs on Box86 alone and says so. `src/game/game_exe.cpp` already tells 1.14d from the others (#77).
- **A JIT-only build stays** (#116): `package-cia` without AOT, for comparison and for releases.
- **The build ID** has to cover the AOT step: the recompiler's version and a short hash of the key, never Game.exe itself.
- **Game.exe from the official patch, optionally** (#116): taking Game.exe out of a `LODPatch_114d.exe` the user supplies, so a user with another version only needs their MPQs. Its format isn't studied yet (see Open questions).

**Build time.** *Estimate:* 5 to 30 minutes on a 4–8 core PC for all 878,540 instructions, once per Game.exe. Basis: each x86 instruction becomes one to three lines of C, about 1 to 3 million lines in all, and gcc -O2 on large generated functions manages tens of thousands of lines per core-minute. The spike measures seconds per 1,000 x86 instructions on its subsystem and scales from that.

**CIA size.** *Estimate:* 5 to 11 MB more code, for a CIA of about 7 to 13 MB in sdcard mode (about 2 MB now). Basis: 1.5 to 3 ARM instructions (4 bytes each) per x86 instruction of 2.96 bytes is 2.0 to 4.1 ARM bytes per x86 byte, against Box86's 7.22, over d2d's 2.6 MB of functions.

## Memory

The 3DS loads an app's whole code segment at start, so the compiled code is resident. In exchange, most of the translation layer goes away.

| Part | Today | With AOT (*estimate*) | Basis |
| --- | --- | --- | --- |
| Game code | 14 MiB pool, filled as code runs | 5–11 MiB in the image, all of it | the CIA size above |
| Box86's code pool, for the fallback | (above) | 2–4 MiB | the lane's `fight.txt` ran exactly with a 6 MiB pool and thrashed at 4 MiB ([memory-budget.md](memory-budget.md)); the fallback runs a small share of that |
| Jump tables | about 12 MiB | 1–3 MiB | 256 KiB per 64 KiB page the fallback runs in; how many is for #115 to measure |
| Block records | 3–4 MiB | about 0.5 MiB | the fallback's blocks only |
| Scratch and first-level tables | about 1.4 MiB | about 1.4 MiB | unchanged |
| Sparse target table | none | about 0.7 MiB | 90,000 entries of 8 bytes |
| **In all** | **about 31 MiB** | **about 11–21 MiB** | |

*Estimate:* AOT would save about 10 to 20 MiB. It also ends the pool filling in long sessions (#23) and the hitches of translating new code. Where the saving goes (the guest arena, the sprite cache, the port's buffers) is for after #115's measurements.

## Distribution

- **We distribute the recompiler, never its output.** The recompiler is our code. Its output is the user's Game.exe in another form: it is made on the user's machine, from the user's own file, and goes only into the user's own CIA. That is the same act the JIT performs at run time (#111).
- **An AOT CIA is personal,** like a slim or full CIA. Even in sdcard mode it holds code made from Game.exe, so it is never shared, posted or attached. `package-cia` says so when it builds one.
- **No generated C, object or table** goes into the repository, a container image, a release or a problem report. They stay under `build/` (git-ignored) and in the user's CIA.
- **Releases stay JIT-only.** The project's own CIA and `.3dsx` never contain AOT code.
- **d2d's list** is read from its submodule at build time and never copied. The CIA contains no d2d code either way. Whether its GPL-3.0 list should be a build input at all, or only a cross-check, is an open question (below).

## The spike's scope (#113)

Proposed for agreement:

1. **The subsystem:** the monsters' A* search, `0x67b850` and its helpers (`0x67b200`, `0x67b440`, `0x67ae00`, `0x67ae50`). It is self-contained, it has a hand-written native to compare against (`PathSearch`), and d2d checks its own version against it in Unicorn (`tools/emu/moves.py`), which #108 brings to our natives.
2. **The recompiler's first cut:** Python with Capstone, and only what this call tree needs: integer instructions, the flags pass, direct calls, `ret`, and the guest memory helpers. No switch tables, no x87, no thread switches inside (the tree doesn't block).
3. **Linked as a native:** the compiled entry is registered as an in-line native at `0x67b850`, through the existing `HOOK` path. No dispatcher or target table yet.
4. **Checked:** against Box86 on the lane (`fight.txt`, `act2-east.txt`) with the native check's trampoline, and against Unicorn on the host with #108's cases.
5. **Measured:**
   - guest bytes on the lane (`WX86_HOTPROF`);
   - time per call on the console against Box86 and against `PathSearch`;
   - the compiled code's size against Box86's for the same functions;
   - the compile time per 1,000 x86 instructions.
6. **Answered from the whole binary, by the recompiler's analysis alone:**
   - how many functions have flags live at a call or a `ret`;
   - how many switch tables its guard rule bounds;
   - how many functions have an x87 stack depth that isn't known at each instruction.

Not in the spike: the target table, exits to the dispatcher, return-site entries, imports, packaging. Those come with #115 and #116 once the spike shows a gain.

## Open questions

- **Is the gain there?** Box86 with `callret` and its register cache is not naive. The spike's time per call decides whether to go on.
- **The site table:** the hooks and code patches move from `install_*` code into one table that both the app and the recompiler read. Is that change acceptable before the spike, or only after it?
- **Does the game write its own code?** If anything other than our patches writes into `.text` at run time, the AOT code for it is stale. Box86's invalidations of `.text` on the lane answer this.
- **Exceptions:** does any function rely on Windows exception handling for a fault (an access violation caught by `__try`)? If one does, it stays with Box86.
- **Return sites:** does every call's return site need an entry, or only those a thread can be switched or fall back under?
- **d2d's list as a build input:** read from the submodule at build time, or only used to check our finder? The recompiler is ours either way, and the output is never distributed.
- **`LODPatch_114d.exe`:** how its Game.exe is stored, and whether extracting it needs anything we would rather not depend on.
- **Faults:** is a report with the function and offset enough, or does the check build need precise x86 state at each memory access?
