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
- **Where the stack lives** (#136): compiled code works on the CPU's own x87 state, Box86's `x86emu_t` fields from `cw` to `fpu_ll` (`Cpu::x87_state()`, backend patch 0065; `AotFpu` in `src/game/aot_rt.h` mirrors them, the offsets checked on both sides). Each push and pop moves `top` at run time, as Box86 does.
  - The depth needn't be known where the code is compiled. That matters: 75 of the 206 functions with x87 code have no fixed depth, and some of the game's own functions take or leave values on the stack (4b98f0 takes three, 4b97d0 leaves two), not only the C runtime's `_ftol`.
  - A callout, an exit to the dispatcher or Box86 entering compiled code finds the stack as Box86 left it, with nothing to convert. The dynarec writes its cached x87 registers back before an in-line call (`BARRIER_FLOAT` at the native-call stub), at a trap and at a block's end.
  - The cost is a load and a store through a pointer for each operand, where locals could have stayed in registers. x87 is about 1% of the hot bytes.
- **Precision** is Box86's on this port: 64-bit doubles on VFPv2, not 80-bit. The game sets the x87 to 53-bit precision (MSVC's runtime does; #125 found it: Unicorn starts at 64), so for ordinary arithmetic doubles match a real CPU too. They differ where the exponent range does (the x87 keeps a 15-bit exponent in its registers even at 53-bit precision: denormals and overflow).
- **Every value a double** (backend patch 0066, `box86_dynarec_x87double = 1`): before it, the dynarec could keep a value loaded from a float (`fld m32`, `fld1`, `fild m16`) in single precision through a block, by rules that depend on where the block ends. Compiled code can't follow those rules, and a real x87 at 53-bit precision computes in double.
- **Each instruction does what the dynarec does**, not what the interpreter's helpers do (the build has no interpreter):
  - The arithmetic runs in the FPSCR's mode on the same thread. `cw`'s rounding control counts only for `fist`/`fistp` and `frndint`, and its precision control for nothing.
  - `fist` gives 0x80000000 (m32), 0x8000 (m16) or 0x8000000000000000 (m64) for NaN or out of range.
  - `fild m64` builds its value from the high word times 2^32 plus the low word. A later `fistp m64` stores the original int64 if st0 is still that value. `fld m80` keeps its 10 bytes the same way for `fstp m80`, through Box86's own `LD2D`/`D2LD`.
  - `fsin`, `fcos`, `fyl2x` (log2), `f2xm1` (expm1) and `fscale` (ldexp) call libm, the same libm Box86 calls. `f2xm1` followed by `fld1` and `faddp st(1)` is one `exp2`, as Box86 makes it. `fprem1` rounds its quotient toward +infinity, as the dynarec's does.
  - `fnstsw` puts `top` in bits 11-13 of the value it stores and leaves `sw` as it is.
  - The x87 decoding goes by opcode and ModR/M, not by Capstone's mnemonics. What an x87 instruction stores comes from a table of our own: Capstone 5 calls `fst`, `fstp` and `fistp` m32/m64 and `fnstcw` reads, `frstor` a write, and `fnsave` 4 bytes.
  - Left to Box86 for now: `fld m80` followed by `fstp m80` and `fild m64` followed by `fistp m64` (Box86 copies the bytes without the value), `ficom`, the m16 integer arithmetic (which Box86 itself can't run), `fnsave`/`frstor`, `fxam`, `fptan`, `fpatan` and the `fcomi` forms. None of them was a function's first refusal on d2d's list.
- **The check** compares the x87 state after each call too: `cw`, the condition codes, the depth and each value on the stack, from each side's own `top` (the dynarec writes `top` 0 when its cache held all eight).

**Instruction kinds the note didn't cover** (the review's fourth point; counted over d2d's 13,048 functions):

| Instructions | Count | What AOT code does |
| --- | --- | --- |
| `fs:` accesses | 268, in 83 functions | the running thread's TEB through `aot_fs_base()` (`rt::current_teb()`); SEH frames (`fs:[0]`) are memory like any other |
| `rep movs`, `rep stos` | 608 | the copy or fill, element by element where it overlaps forward, with the direction flag known at each instruction (clear at entry and after calls, MSVC's rule; `std`/`cld` change it; a function where it isn't known stays with Box86) |
| `fldcw` | 40 | stores `cw` (#136); it changes only how `fist` and `frndint` round, as in Box86 |
| `cpuid` | 6 | to answer as `RunOptions::cpuid_edx` (not in the spike) |
| `rdtsc` | 1 | from the same source as Box86's (not in the spike) |

The spike's two trees have none of these. Since #136 x87 is compiled; `cpuid` and `rdtsc` still leave their function to Box86.

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
  - The recompiler (`tools/aot/recompile`, Rust with Capstone, built in the same image as the d2d harness; #144) reads Game.exe, mounted read-only as the art step does. It writes C into `build/aot/<key>/`.
  - devkitARM compiles that C in the toolchain image, and `tests/cpu/ctr/Makefile` links it into `3ds-d2-game-cia.elf`.
- **The C in parts** (#135): `aot_spike.c` holds the tables and the roots; the functions go into `aot_spike_NN.c`, each at most `AOT_PART_MB` of C (8). One file for the coverage set was 44 MB and took gcc over 4 GB; d2d's whole list (12,565 compiled at #138's census) would be about five times that. The recompiler groups the functions in the order that leaves the fewest calls across files (gcc inlines a callee only within its file): by address, callees next to their callers, or clusters along the calls. The makefiles compile every `aot_spike*.c` they find, and a compiled function is no longer `static`, so a call can cross files.
- **The cache key:** Game.exe's SHA-256, the recompiler's version, the site table, and the toolchain lock. Packaging again with the same files reuses the objects.
- **The app checks the Game.exe it runs.** The AOT code is only valid for the bytes it was made from. The app hashes the Game.exe it loads; if it doesn't match the CIA's key, a CIA without Box86 stops and says which Game.exe it needs (one built with the Box86 option runs on Box86 alone). `src/game/game_exe.cpp` already tells 1.14d from the others (#77).
- **Build options** (#116): the default has no Box86; a compile-time option keeps Box86 in as a fallback; a JIT-only build stays for comparison.
- **The build ID** has to cover the AOT step: the recompiler's version and a short hash of the key, never Game.exe itself.
- **Game.exe from the official patch, optionally** (#116): taking Game.exe out of a `LODPatch_114d.exe` the user supplies, so a user with another version only needs their MPQs. Its format isn't studied yet (see Open questions).

**Build time.** Measured by the spike (5,722 x86 instructions, one build of the C):
- **gcc** (devkitARM, -O2, ARM mode): 0.95 s per 1,000 x86 instructions for the signatures' C, 1.28 s for the context fallback's, on one core. For all 878,540 instructions that is about 14 minutes on one core, so 2 to 4 minutes on a 4–8 core PC, as files compile in parallel.
- **The recompiler, in Rust** (#144): d2d's whole list (13,048 functions, 12,784 compiled) in 36.5 s with a peak of 912 MB, and the coverage set (2,417) in 10.8 s with 384 MB. `recompile.py` took 481 s and 5,169 MB, and 152 s and 2,374 MB, for the same output byte for byte (`scripts/docker.sh test aot-compare`).
- **The recompiler, in Python, at the spike:** 0.07 s per 1,000 instructions to write the C, but 39 s of analysis, because the spike works out summaries for every function its trees reach (541) to a fixed point, again for each compiled set. Over the whole binary that analysis runs once: the survey's decoding and flags pass over d2d's 13,048 functions took 97 s. The summaries' fixed point needs a faster form (each function analysed once per round, only those whose callees changed) before #115.

**CIA size.** Measured by the spike: 2.6 (A*) and 3.1 (CofParams) ARM bytes per x86 byte with the signatures, 3.6 and 4.5 with the context fallback, against Box86's 7.8 and 10.3 for the same functions (devkitARM -O2, ARM mode; Box86's from its blocks on the lane, backend patch 0061). The review's band of 2 to 5 holds. With the signatures and x87, string and switch code still to come, *estimate* 3 to 4.5 ARM bytes per x86 byte: 8 to 12 MB more code over d2d's 2.6 MB of functions, for a CIA of about 11 to 15 MB in sdcard mode (about 3.4 MB now).

## Memory

The 3DS loads an app's whole code segment at start, so the compiled code is resident. In exchange, most of the translation layer goes away.

| Part | Today | With AOT (*estimate*) | Basis |
| --- | --- | --- | --- |
| Game code | 14 MiB pool, filled as code runs | 8–12 MiB in the image, all of it | the CIA size above |
| Box86's code pool, for the fallback | (above) | 2–4 MiB | the lane's `fight.txt` ran exactly with a 6 MiB pool and thrashed at 4 MiB ([memory-budget.md](memory-budget.md)); the fallback runs a small share of that |
| Jump tables | about 12 MiB | 1–3 MiB | 256 KiB per 64 KiB page the fallback runs in; how many is for #115 to measure |
| Block records | 3–4 MiB | about 0.5 MiB | the fallback's blocks only |
| Scratch and first-level tables | about 1.4 MiB | about 1.4 MiB | unchanged |
| Sparse target table | none | about 0.7 MiB | 90,000 entries of 8 bytes |
| **In all** | **about 31 MiB** | **about 14–22 MiB** | |

*Estimate:* AOT would save about 9 to 17 MiB. It also ends the pool filling in long sessions (#23) and the hitches of translating new code. Where the saving goes (the guest arena, the sprite cache, the port's buffers) is for after #115's measurements.

## Distribution

Updated 2026-11-17 (#111, the owner's decisions of 2026-11-15 on #25 and 2026-11-17):

- **AOT output may be distributed.** Public CIAs (dev snapshots, releases) may carry code compiled from Game.exe, as they carry the game art. Game.exe itself, the MPQs and saves never are; in CI they stay behind an access token.
- **The default CIA has no Box86 and no JIT.** The game runs from AOT-compiled code. Box86 kept in the CIA as a runtime fallback would keep the JIT layer's memory (the pool, the jump tables, the records: about 31 MiB), which is what this track gives back. Keeping Box86 is a compile-time option, for a fallback build.
- **Box86 stays as the reference:** in the recompile step and the test lanes (`test aot`'s differential check, #114).
- **No generated C, object or table** goes into the repository or a container image. They stay under `build/` (git-ignored) and in the CIA.
- **d2d's function list is the target** (#105, #107): the functions AOT compiles are d2d's, by address and name, so that game code can later be changed function by function. The list is read from the submodule at build time and never copied. The CIA contains no d2d code.
- **d2d's names may stay in the CIA** (owner, 2026-11-17): the compiled code's function table carries each function's name, d2d's (GPL-3.0-or-later) where it has one. If that turns out to be a problem, a distributed CIA carries addresses only.

## The spike (#113)

The scope agreed on #112 (the review's proposal): A* with its real call tree, plus one call-heavy hot function without a native, each built with register signatures and with the context fallback, checked on the host and on the lane, measured.

**What was compiled** (`tools/aot/spike.args`):
- **A*** (`0x67b850`, the root named `PathSearch`): its 21 functions as the review listed them, including the 0x64d910 chain and the switch tables in 0x64d910 and 0x64d4e0, plus 61a010, 64cc30 and 64cdf0 on the same paths. 23 compiled functions, 4,491 x86 bytes. RoomFind is called in C. CollisionCross is called in C and hands a room's edge back to the compiled 0x64d4e0. `_memset`, `_memcpy` (as `memmove`: MSVC's handles overlap) and `__alloca_probe` are written in line.
- **CofParams** (`0x4db7b0`, d2d's `cof_build_params`), picked from the lane's block counter (`fight.txt`). Its own share is 0.27%, but with its tree up to 3.9%, the most of any function without a native whose tree has no indirect calls. It makes 26 direct calls, and its hot callees are small item and inventory lookups (6335f0, 63add0, 63c050, 629fe0): the flat, many-small-calls code where the review said AOT has to win. Its tree to depth 3 plus the four callees the bench found it still ran nested (628700, 62a1e0, 6289c0, 628980): 95 compiled functions, 11,103 x86 bytes.
- **Left to Box86:** two error-path tail calls (408a60, 40d8d0) and the C runtime's 681d27. Neither tree has `fs:`, `rep`, `fldcw`, `cpuid`, `rdtsc` or x87 code (see Instruction kinds).
- **Hooked** at each root as a native not served in line, installed before the natives, so PathSearch's hook doesn't go in over it.

**Checked:**
- **On the host** (#125's case runner, Unicorn): the compiled A* on PathSearch's 400 cases, in all four builds (signatures, context, and both without inlining between compiled functions). 400 match the game's code, none left to the game, including the 168 that PathSearch itself hands back. The same with the compiled 0x64d4e0 in place of CollisionCross. CofParams has no cases: its inputs (a unit, its inventory, the item tables) need a generator of their own, so the lane checks it.
- **On the lane** (`D2_AOT=5`: the compiled code with its stores journaled, memory put back, the original from a trampoline under Box86, compared: eax, the callee-saved registers, every byte the compiled code wrote outside the dead stack). A byte only Box86 wrote isn't seen there; the host cases compare every byte.

| Run | Root | Calls checked | Differ |
| --- | --- | --- | --- |
| `fight.txt` | A* | 38,800 | 0 |
| `act2-east.txt` | A* | 20 (few monsters on the way) | 0 |
| `fight.txt` | CofParams | 243,115 | 0 |
| `act2-east.txt` | CofParams | 257,225 | 0 |

- **What the checks found:** Capstone's missing flags for `test` with memory (Flags, above), a push of `esi` taken for a save when it was an argument, and `esp` not read back after a nested run in the signatures' build. All three are fixed.

**Measured** (the lane's figures are qemu's ARM11 model: counts are exact, times only indicative; the console's are the bench's, below):

| | A* | CofParams |
| --- | --- | --- |
| Guest bytes on the lane (`fight.txt`, `WX86_HOTPROF`) | 44.15 G with Box86 (A*'s tree 75% of them), 9.57 G compiled: 78% of the run off Box86 | 12.45 G, 11.57 G compiled: 7% off (its callees are shared with code Box86 still runs) |
| Time per call, qemu: Box86 / signatures / context / signatures without inlining / context without inlining | 2,470 / 600 / 720 / 665 / 815 µs | 37.5 / 12.9 / 15.3 / 14.7 / 18.5 µs |
| Time per call on the console (median; A*'s per call, CofParams' per window): Box86 / signatures / context / signatures without inlining / context without inlining / native | 339 / 61 / 79 / 71 / 82 / 27 µs (9 calls each, 5 native) | 48.5 / 10.3 / 14.5 / 11.1 / 16.2 µs (194,559 calls each) |
| Code, ARM bytes per x86 byte: signatures / context / Box86 | 2.60 / 3.64 / 7.76 | 3.14 / 4.51 / 10.27 |
| Compile time per 1,000 x86 instructions | see Build time | |

- **The console's run** (build 3f6127ac0c48-aot81afeb, New 3DS XL, 6 minutes of fights in the Rocky Wastes, the bench switched on in game, Sentry 3DS-D2-1D, event 5d3d68c0): no faults or stops. Means for CofParams: Box86 49.8, signatures 10.7, context 15.1, signatures without inlining 11.6, context without inlining 17.0 µs. A*'s calls are few (PathSearch is called a few times in 10 seconds) and each finds a different path, so its medians are rough; Box86's first call (8.7 ms) translated the code.
- **Against Box86 on the console:** CofParams 4.7× with the signatures and 3.3× with the context fallback; less the nested run in Box86's figure (17.6 µs on the console), 3.0× and 2.1×. A* about 5× with the signatures and 4× with the context fallback. The lane's qemu understated CofParams' gain (1.6×).
- **Box86's time** in the bench includes one nested run, which costs 13–18 µs alone. For CofParams that is half of Box86's figure: without it, Box86 takes about 20 µs, the signatures 12.9.
- **What the calls cost:** on the lane, the context fallback is 20% slower than the signatures on A* and 19% on CofParams; on the console 30% on A* (medians) and 41% on CofParams. Without inlining between compiled functions, the signatures lose 11% and 14%, and the context fallback 13% and 21% more. gcc inlines most of CofParams' small callees, which is where the signatures matter most when it can't.
- **PathSearch on the console** times as `native` in the bench: 27 µs at the median, 2.3× the compiled A*. It handed 3 of its 8 turns back (435 µs each, run nested under Box86), cases the compiled A* runs itself. In the lane's A*-heavy world it handed every call back (another size class, or a room's edge), and the bench counts those apart (`handed-back`).
- **A nested Box86 run per call** cost the compiled CofParams more than it saved (50 µs against Box86's 36 before its four callees were compiled). A full build must compile what its functions call, or reach it through the target table, not a nested run.

**The whole binary** (`recompile.py --survey`, d2d's 13,048 functions; 12,864 decode with the spike's rules):
- **Flags live at a call or a `ret`:** 12 functions read flags their caller or a callee left (0050fad0, 0050fbd0, 00586d88, 005913c8, 006879dd, 00687b18, 00687c6d, 00687ef8, 006885e8, 00688718, 006bb870, 006bba00). They stay with Box86. 25 more read flags of instructions the spike doesn't model (`repe cmpsb`, `sahf`, `bt` and others).
- **Switch tables:** the guard rule bounds 490. 19 jumps through a table aren't bounded by it, and 9 tables overlap code as decoded. 155 functions have another indirect jump.
- **x87 depth:** of the 206 functions with x87 code, 131 have a known depth at every instruction (0 at entry, a call moving it by its callee's own depth at its rets) and 75 don't.

**Not in the spike,** as agreed: the target table, exits to the dispatcher, return-site entries, imports, packaging, and x87. The results and the go/no-go recommendation are on #113.

## The check (#114)

The spike's check compared the two roots only. `D2_AOT=5` now checks every compiled function whose entry can be hooked, at every call, wherever it comes from: the game's code, or the original's run inside another check.

- **The table:** the journaled build (`AOT_JOURNAL`) has `aotj_funcs`: each compiled function with a register entry of its own (its signature code, called with all eight registers as a root is), the registers its compiled callers read after it (out), those it keeps (preserved), and its entry's first whole instructions for a trampoline.
- **What can be hooked** (the recompiler's `hookable()`): a function whose first 5 bytes or more are whole instructions with no branch among them, whose ret pops a known amount, and whose first bytes no branch reaches. The last is checked against every decoded function's branches and a raw sweep of `.text` for 32-bit calls and jumps, which over-approximates. Of the spike's 117 functions, 99 qualify. The other 18 start with a call or a branch: their code is still checked inside their callers' checks.
- **One call checked:** the compiled code runs journaled. Its writes are put back, then the original runs from its trampoline under Box86 from the same registers. Compared: the out and preserved registers, and every byte the compiled code wrote outside the dead stack. The game keeps Box86's result, so a hooked function the original calls is checked in its turn: each check keeps its own journal.
- **Unchecked calls:** when the compiled code calls Box86 (a callee not compiled), Box86's writes aren't journaled, so nothing can be put back. That call keeps the compiled result, runs once, and counts as unchecked. In the spike's set, these are the allocator wrappers (`0040b380`, `0040b430`, which call `0040a080`).
- **The test:** `scripts/docker.sh test aot <game-dir> [<save-dir>]` (build-and-run.md).

| Script | Calls checked | Functions reached (of 98 hooked) | Differ | Unchecked |
| --- | --- | --- | --- | --- |
| `town-walk.txt` (70 s) | 12,080,093 | 83 | 0 | 21,791 |
| `fight.txt` (120 s) | 30,539,023 | 83 | 0 | 23,028 |
| `act2-east.txt` (150 s, BeforeCh1End) | 32,018,886 | 85 | 0 | 35,842 |

98 were hooked on the lane, not 99: one entry had other bytes there when the hooks went in.

## Coverage, stage 1 (#115)

`tools/aot/coverage.py` folds the lane's block counter (`WX86_HOTPROF=1 D2_NOLINK=1`) into functions and ranks them by the x86 bytes they ran. The 814 that ran 99.9% of the bytes over the town walk, the fight and Act II are `tools/aot/coverage.args` (`D2_AOT_SET=coverage`). Box86 enters every compiled function it calls: the functions' table is in every build, and Sig mode hooks each.

What the list found, in the recompiler and the runtime:

- **Refusals a round at a time:** one refusal used to start the whole fixed point again. Over 814 functions that was an hour; now 80 s.
- **The analysis:**
  - A stack slot unknown on one path was kept at a merge, and the walk never settled.
  - A function that leaves only by tail jumps was taken to keep every register, eax included.
  - Callee-saved registers lost at an indirect call are taken as kept (MSVC's ABI), unless the function moves its caller's frame (the SEH epilogues).
  - A helper whose flags its caller reads after the call stays with Box86. Found in the CRT's math helpers: `6987d8`, then `6879dd`'s first `je`.
  - ecx and edx after memset, memcpy or a native are the library's garbage, and leave the outs.
- **Natives:** a function whose bytes changed since it was compiled (a native's patch), and every compiled caller that reaches it in C, stays with Box86. This is checked each time the code is turned on (`code_hash`). On the lane, Sig and Check now go on after the natives, as on the console.
- **Waits:** a shim that blocks the thread inside a nested run left the wait's placeholder in EAX: on the console, Storm's reads got 0. A wait on the I/O worker is now done in place. A wait on another guest thread can't be: running the other threads from inside a nested run livelocked when one of them was waiting on a thread held lower on the host stack. So nothing is compiled that calls, under Box86, code that can reach a blocking import or an indirect call (`may_block`).
- **Finding a fault:** `AOT_MODE=1`, `AOT_SPLIT=1` and `D2_AOT_FNS` (or settings.txt's `aotfns=` in Azahar) halve the hooked set. `test aot-explain` says why a function is left to Box86.

Of the 814, 450 compile. The check passes on all three scripts: 21.1M, 46.1M and 40.1M calls checked, none differ. Box86's executed bytes with the 660-function set before `may_block` (Sig, the lane's profile; same frames in the town walk and the fight):

| Script | Box86 bytes, no AOT | With AOT | Off Box86 |
| --- | --- | --- | --- |
| Town walk | 2.48G | 1.75G | 30% |
| Fight | 7.78G | 3.27G | 58% |
| Act II east (per frame) | 2.54M | 0.61M | 76% |

With `may_block`, the compiled functions cover 49–52% of the bytes the profile ran, against 68–80% before.

The town walk's and the fight's rows may not be what they say: until stage 3, the game-state reader checked Game.exe's layout on the first byte of `45b440`, a function on the list, and with it hooked the lane's scripts never saw the game start (below).

**Azahar, Act II walking:** with the same CIA, the game runs at 25 fps with the code off and 12.4–13 fps with it on. Fewer bytes run under Box86, but each crossing costs more than it saves. Every call from Box86 into a compiled function is a trap, and every call from compiled code to a Box86 callee is a nested run: about 9,500 a second here (`00517abb`, `00609180`). Stage 1 is therefore off by default (the OPTIONS row). The crossings are stage 3's work: imports to shims, dispatcher exits, return sites.

## Crossings, stage 3 (#115)

What changed:

- **More compiled per crossing:** the list's callees, two calls deep, where they compile (`--callees 2`: 1,149 functions, 961 hookable). Tail jumps to other functions compile as calls with a return. `bsf` and `bsr` compile.
- **Leaves:** a function with no Box86 call anywhere under it is entered in line (the dynarec calls the trap without leaving its run): 668 of the 1,149.
- **The analysis:** ecx and edx a compiled callee leaves as garbage are its callers' garbage too. `may_block` is applied again after each round's other refusals. A segment register's `mov` is refused.
- **The probe:** each function's calls from Box86, 32 on, 256 of them, are timed. One whose compiled code takes less than the threshold a call (8 µs since exits, 2 before; its nested runs taken off, and the threshold counted for each) is left to Box86 from then on. settings.txt's `aotmin=`, the lane's `D2_AOT_MIN_US`. Box86's own time, run nested, can't be the measure: on the lane a nested run costs 8–10 µs empty, and 11–29 µs for functions whose compiled code takes 0.6–2.7.
- **The natives' calls:** a native's `rt::guest_call` into a compiled function runs it in C, not in a nested run. In the fight that's the floor's interpolated light, `4f8850` and `4f89f0` from `4f8b80`'s native: about 140,000 calls a window.
- **The lane's scripts:** the game-state reader's layout check is now an operand inside `45b440`, not its first byte (a hook's jump). `test aot` fails a script that never reached its last wait.

On the lane, host ms over windows 3 onward (10 s of game each, the virtual clock), each run in game to its script's end:

| Script | Off | On, the probe at 2 µs | On, every function hooked |
| --- | --- | --- | --- |
| Fight (8 windows) | 59,992 | 60,573 | 110,313 |
| Act II east (11 windows) | 57,909 | 61,031 | 132,321 |

With every function hooked, Box86 enters compiled code about 5 million times a fight window, and each costs about 1 µs more than Box86 running the function itself. The probe takes back the functions that aren't worth it, which leaves the code no faster than Box86. The calls come from callers that `may_block` keeps with Box86: `467ac0`, `466870`, `64f380`, `64f060`, `480810`, `4cb460` (each calls Box86 code with an indirect call or a blocking import under it). Compiling them needs one of:

- **A host stack per guest thread:** a nested run could then block, and the scheduler switch to another thread.
- **Exits:** compiled code that hands the rest of its call back to Box86 at a call that may block. The guest's registers and stack are whole at a call, but each compiled caller above it in C would have to exit too.

Exits were chosen.

### Exits

- **Where:** a compiled function's indirect calls, its import calls, and its calls or tail jumps to Box86 code that may block. Before exits, any of these left the function to Box86, and every caller that might reach one too.
- **How:** the call's return address is pushed as the original does. Then `aot_exit` hands over the target and all eight registers, and longjmps to the trap that entered compiled code. That trap gives Box86 those registers, with esp as at the call, and redirects it to the target. Box86 then runs the rest of the call, and every compiled caller's rest above it, from the original's code. Only the functions that may exit (their own sites, or a compiled callee's) pay for the `setjmp`.
- **The analysis:** an exit site ends its path, as a call that never returns does, and reads every register. A function that may exit stores every save its prologue makes, because Box86 restores them from the stack.
- **The check:** a call that exits is rolled back from the journal and run again in Box86 from its entry, and counted unchecked (3.5–4.4% of the lane's calls).
- **Garbage:** ecx and edx are no value of a function's when, on any path to one of its rets, they hold a library call's leftovers (memset's, a native's). Exit sites read every register, which made the check compare memset's leftovers in `0041d300` and `00412e40`.
- **Result:** 2,236 functions compiled (1,149 before), 568 of them with exits at 1,719 sites. 1,097 may exit, counting their callers. 179 of the closure are still left to Box86. The check lane: 0 differ on the three scenes (30, 56 and 88 million calls).

On the lane (host ms over windows 3 onward; off measured in the same session, because the host's speed moved about 10% between sessions):

| Script | Off | On, 2 µs | On, 8 µs | On, 16 µs |
| --- | --- | --- | --- | --- |
| Fight (8 windows) | 67,646 / 69,801 | 73,537 | 64,775 | 67,778 |
| Act II east (11 windows) | 65,147 / 64,669 / 65,447 | 75,892 | 68,700 | 69,283 |

At 2 µs, the traps from Box86 go up about 4× (300–500k a fight window), mostly into `00470860` from `UnitPickFilter`'s native, which stays with Box86. In Act II at 8 µs, compiled code takes about 200 ms of a 5.2 s window, with 20,000 traps and 1,800 exits. That's too little of the window for it to win much: the lane is at parity, within its noise.

**Azahar, Act II, walking in Lut Gholein** (the same CIA, `aotmin=8`; frames per 10 s window once in Act II):

| | Windows | Average |
| --- | --- | --- |
| Off | 241 234 238 250 246 247 251 246 | 24.4 fps |
| On | 248 235 243 250 247 250 250 251 | 24.7 fps |

At 50% emulated CPU (one run each, frames climbing through the walk in both modes), off got 41 40 78 70 91 148 146 150 159 188 frames, and on got 55 67 102 81 112 143 179 168 186 227: ahead in 9 of 10 windows, 19% more frames in all.

Stage 1 dropped to 12.4–13 fps here. With exits, the code no longer costs frames. It doesn't gain any either, because the game runs at 25. The window that loads Act II is slower with it on (171 frames against 206), while the probe times each function's first calls.

### Callouts

- **Where:** a compiled function's indirect and import calls (549 of the 1,707 exit sites). Since #138, its tail calls through a pointer too (Tail calls through a pointer, below). Instead of an exit, `aot_call_out` looks at the target first:
  - **One of our shims** (an import, or a pointer to one): its `Shim::fn` runs in C, with the CPU's registers set as at the call. Box86 takes over at the return only when the shim redirected, a yield is pending, or esp or a callee-saved register isn't what the call's pops guess says.
  - **A compiled function:** its signature function, the journaled one under the check.
  - **Anything else:** an exit, as before.
- **Pops guess:** the import's own, 0 when an `add esp` follows the call, else the pushes before it. A function whose guess leaves esp unknown at a ret keeps plain exits (39).
- **Hard exits:** `may_hard` marks the functions whose exits aren't all callouts. Natives' direct calls (no trap to exit to) refuse only those; there a callout's fallback is a nested run.
- **The shim's registers:** a callout keeps only the trap's general registers and eip, not the full context. The 768-byte FPU blob each way made Act II 30% slower with the code on than off. The FPU is the shim's to leave: compiled code has no x87.
- **The check:** what a callout's compiled callee leaves as garbage (neither out nor kept) isn't compared, since the caller can't know it is (`005a7ba0`'s ecx).

### Functions natives changed

A native's hook changes its function's bytes, so the function stays with Box86, and so does every compiled caller that reaches it (388 hookable functions on the lane, 490 with the console's settings). `D2_AOT_STALE=1` keeps those callers compiled:

- **Per function, not per call:** a compiled call goes through an inline `call_<va>`, which calls the function's C, or a cold `stale_<va>` when the app found it changed (`aot_stale`, by the table's row). `stale_` calls the native (`aot_call_stale`): its shim in C when it hooks the entry, Box86 nested on from where it hands back to the game's code, a wait done in place; else Box86's call, nested. A guard at each call site, with every register taken back in line, made Act II 25% slower: the C compiler couldn't hold the callee-saved registers across the calls.
- **One that may exit** (#138): what it hands Box86 may wait on another guest thread, which a nested run can't (`0040a960`'s `WaitForSingleObject` hung the fight). Where its native hands back or asks to yield, the call exits instead, as a callout's does: Box86 goes on at the hand-back's target, or at the return. Its callers stay compiled. Before, they stayed with Box86 (6,889 functions on d2d's whole list).
- **Hand-backs compiled** (#138): a native that did a function's first instructions and hands the rest back (`resume_original`) goes on in that function's compiled code, entered there. The recompiler reads the addresses from `d2_native.cpp` (`resume_original(c, 0x...`) and writes `resume_<va>`: the function's sig code, entered at `va` with all eight registers, and returning them as ctx mode does. It refuses one where a flag read there comes before anything sets it. `aot_resume` runs it in Sig mode, then Box86 goes on at the function's return, or at an exit's target. All 13 compile. `D2_AOT_RESUME=0` leaves them to Box86. The check doesn't run them; its natives hand back to Box86.
- **Loop steps in line** (#138): the natives that replace a loop's step inside a function (`install_slot_steps`: 467ac0's unit pick, the floor's and the walls' cells, 52fc20's slots, the weather) do what the loop does, only faster under Box86. `set_aot_inline_equivalent` makes the change check read the game's bytes at those sites, so the 8 functions stay compiled and hooked, and their C runs the original loop. The check compares that C with Box86 running the native: 0 differ on the three scripts.
- **Equivalent natives:** `set_aot_equivalent` names a native whose function's original compiled code does the same, registers included: compiled code calls the original's C, and its callers stay compiled with or without `D2_AOT_STALE`. `465c60` (UnitRelation) is one; its native's town case hands back to the same code anyway (200k calls a window in Act II). StatGet and UnitPickFilter aren't: the check differs on ecx and edx after them.

Off by default: on the lane the natives' calls from C cost more than the callers gained (Act II 63.7 s with it off against 67.2 on, fight 54.5 against 58.4).

**The lane, on against off** (host ms over windows 3 onward, the same session):

| Script | Off | On |
| --- | --- | --- |
| Fight (8 windows) | 60,961 | 53,242 |
| Act II east (11 windows) | 60,788 | 63,175 |

The fight gains 13%; Act II is 4% behind, as with exits alone in the same session (`f68fedc`: 61,821 against 59,243).

**Azahar, Act II at 50% emulated CPU** (CIA `cf8a9021f497-aota64c18`, `4575dbb`; frames per 10 s window once in Act II, two runs each):

| | First 10 windows | Last 5 |
| --- | --- | --- |
| Off | 1,564 / 1,577 | 968 / 982 |
| On | 1,610 / 1,665 | 1,025 / 1,062 |

That's 4–5% more frames over the walk with the code on, and 7% once it settles. An earlier pair of runs (f68fedc) gave 56%, but its off runs started unusually slowly (8, 7, 58 frames in their first windows, where these got 35, 119, 135), so that figure doesn't hold.

### The JIT pool

The code on doesn't shrink what Box86 translates much: after the same walk, 7.36 MiB of blocks with it on against 7.76 off, in 8 MiB of chunks either way (14 MiB pool). Box86 still translates the code around the compiled functions, and the functions it runs again when the probe drops them. So the pool stays at 14 MiB: nothing measured supports a smaller one.

## The whole list, census (#135)

`scripts/docker.sh test aot-census <game-dir>` gives the recompiler every function on d2d's list (13,048, 2,600,618 x86 bytes) and writes `build/aot/census/census.txt` (and `.json`): what it refuses, by first cause; the instruction kinds present; and what the compiled code still hands to Box86, by kind. Each line is weighted by function count, by bytes, and by the lane's executed bytes (`build/m4-prof/*.err`, the Box86 lane's `WX86_HOTPROF=1 D2_NOLINK=1` profile of town-walk, fight and Act II). The hot share of an exit or a nested call is that of the functions holding such sites. A run takes about 16 minutes and up to 5 GB.

**Compiled:** 11,673 functions (89.5%), 2,321,656 bytes (89.3%), 93.4% of the lane's hot bytes (`669c497`, d2d `f1533770`). 8,784 of them may hand a call to Box86 (their own exits or a compiled callee's).

**Refused (1,375), the main causes:**

| Cause | Functions | Bytes | Hot |
| --- | --- | --- | --- |
| esp at a ret not known to be the entry's | 652 | 148,930 (5.7%) | 3.3% |
| x87 (fild, fld, fldz, fstp, fmul, fabs, ...) | 155 | 51,156 (2.0%) | 1.0% |
| a tail call whose pops differ from the target's | 301 | 21,737 (0.8%) | 0.5% |
| an indirect jump that isn't a bounded switch | 155 | 4,865 (0.2%) | 0.2% |
| switches the guard rule doesn't bound, or tables over code | 28 | 28,691 (1.1%) | 0.0% |
| `rep ret` (`__security_check_cookie`; compiled since `52e8692`) | 1 | 15 | 0.1% |
| the rest: the C runtime's SSE2, `rcr`, `shrd`, `cpuid`, `rdtsc`, `stmxcsr`, segment moves, flag reads, 8- and 16-bit `imul` | 83 | 23,568 (0.9%) | 0.0% |

**Instruction kinds present** (whether or not they were the first cause): x87 in 206 functions (2.6% of the bytes, 1.0% of the hot bytes), `rep` strings in 286 (5.3%, 0.7%; compiled), `fs:` in 83 (0.8%; compiled), `cpuid`, `rdtsc` and `in` in 7.

**What compiled code still hands to Box86:**

| Kind | Sites | Functions | Hot |
| --- | --- | --- | --- |
| an import, as a callout (the shim in C) | 1,968 | 896 | 8.4% |
| an import, as a plain exit | 68 | 65 | 2.1% |
| an indirect call, as a callout | 1,757 | 731 | 5.9% |
| an indirect call, as a plain exit | 1,523 | 461 | 2.5% |
| a call to a function left to Box86 that may block | 1,814 | 1,340 | 6.7% |
| a nested run: the error paths and exit (408090, 408a60, 681e09; they don't return) | 1,726 | 1,723 | (callers' 39.2%) |
| a nested run: a refused callee (`__security_check_cookie` 625 sites, compiled since `52e8692`; the others 768) | 1,393 | | 2.5% |

The imports are 271 functions: 268 have a shim in our runtime or winx86. The three without are `ijl11.dll`'s ordinals (the JPEG library). The most called: `EnterCriticalSection` and `LeaveCriticalSection` (695 sites), `GetTickCount` (192), `wsprintfA` (104), `IsBadCodePtr` (72), `CloseHandle` (46).

**What it says for #111's stages:**
- **x87 (#136) is small:** 206 functions, 1% of the hot bytes. Few of them are in the game's frame (the hottest, 4623c0 at 0.5%).
- **The stack (#138's entries):** the largest refusal was esp not known at a ret (652 functions, 3.3% hot), then tail calls whose pops differ (301). Both are now nearly gone (below). Indirect jumps that aren't switches are 155 small functions.
- **Indirect calls (#138):** 3,280 sites in 1,192 functions. Without Box86 every one needs its targets compiled and found in C.
- **Blocking (#137):** 1,814 calls to functions left to Box86 that may block, and the shims that block (critical sections, `WaitForSingleObject`, `Sleep`).
- **The error paths** run nested today. They never return, so they cost nothing in a frame, but a build without Box86 has to compile them too.

### After #138's stack fixes

A refusal for esp now names its cause (the first instruction or call where esp stopped being known, two paths bringing different esps, or a ret popping another amount), so `census.txt` splits it. Most of the 652 were not the functions' own code:
- **Rets cut with their paths (408 functions):** an exit ends the compiled code's path, and with it the rets behind it. A function whose every ret was behind an exit lost its pops, and every caller lost esp. It now pops what its rets decode with.
- **Thunks:** a function with no ret of its own that only tail-jumps (420290: `push ebp; mov ebp, esp; pop ebp; jmp`) pops what its targets pop where they agree. This also took away nearly all of the 301 tail calls whose pops differed.
- **The CRT's frame helpers:** `__SEH_prolog4`, `__SEH_epilog4`, `__EH_prolog3_catch` and `__EH_epilog3` build or tear down their caller's SEH frame, so esp after the call is the caller's frame. They are compiled in line, with the same stores as their code, and only where their code is instruction for instruction what is modelled.
- **Stale summaries:** a function made plain in the screening (its callouts' guessed pops threw its stack off) kept the summary made with those guesses, which had moved esp at its ret or lost `ebp`, and its callers were refused on it. Its summary is now made again there.

**Compiled:** 12,565 functions (96.3%), 2,479,382 bytes (95.3%), 97.2% of the lane's hot bytes (`0b3a088`, d2d `f1533770`). 9,594 may hand a call to Box86.

**Refused (483), the main causes:**

| Cause | Functions | Bytes | Hot |
| --- | --- | --- | --- |
| x87 | 169 | 54,167 (2.1%) | 1.0% |
| an indirect jump that isn't a bounded switch | 155 | 4,865 (0.2%) | 0.2% |
| esp at a ret not known to be the entry's | 41 | 8,982 (0.3%) | 0.1% |
| switches the guard rule doesn't bound, or tables over code | 28 | 28,691 (1.1%) | 0.0% |
| a tail call whose pops differ, or to an import | 3 | 86 | 0.0% |
| the rest (the C runtime's SSE2, `rcr`, `shrd`, `stmxcsr`, flag reads, 8- and 16-bit `imul`, ...) | 87 | 24,445 (0.9%) | 0.0% |

The 41 left for esp: 18 rets that pop another amount than the entry's (the C runtime's), 7 merges of two esps, 6 calls to other frame-moving helpers, and a few `sub`, `push`, `xchg` or `leave` on esp (`__alloca_probe` and the frame helpers themselves among them).

x87 was then the largest refusal (#136 compiles it: After x87, below). On the coverage set (`D2_AOT_SET=coverage`) 2,325 functions compile and 90 are left to Box86 (2,188 and 227 before); the check is 0 differ on the three scripts. The generated C was one 44 MB file at that size; `make -j2` compiling two of them ran out of Docker's 8 GB, so the run used `AOT_JOBS=1`. The C is now split into parts (Building it).

### After x87 (#136)

x87 is compiled, on the CPU's own x87 state ([Translation model](#translation-model), x87). Backend patch 0066 makes every x87 value a double in Box86 too, so both sides compute the same thing.

**Compiled:** 12,731 functions on d2d's list (97.6%), 2,533,348 bytes (97.4%), 98.1% of the lane's hot bytes (12,565, 95.3% and 97.2% before). None is refused for x87 any more (169 before). The 317 left: 155 indirect jumps that aren't bounded switches, 28 switches the guard rule doesn't bound or whose tables overlap code, 41 for esp at a ret, and the C runtime's SSE2, `stmxcsr`, `rcr`, `shrd`, flag reads and 8- and 16-bit `imul`.

**The coverage set:** 2,361 functions compile and 54 are left to Box86 (2,325 and 90 before). 36 of them have x87 code, 35 of those hookable.

**The check** (`D2_AOT_SET=coverage scripts/docker.sh test aot dist/game dist/save/BeforeCh1End/save`) is 0 differ on the three scripts, the x87 state compared after each call:

| Script | Calls checked | In functions with x87 code |
| --- | --- | --- |
| town-walk | 29.1M | 290K, 12 functions |
| fight | 53.6M | 848K, 15 functions |
| Act II | 77.9M | 3.7K, 10 functions |

d2d's cases (`D2_AOT_BUILD=keep scripts/docker.sh test d2d dist/game`) pass: 48 natives, 18,902 cases, 0 differ. The fight in production mode (`AOT_MODE=1`) ran in 63 and 62 s, against 63–67 s before x87 and patch 0066 (wall time to the second, under the lane's virtual clock).

### Tail calls through a pointer (#138)

An indirect `jmp` that isn't through a switch table (`jmp eax`, `jmp [ecx + n]` into a vtable, `jmp [IAT]` in an import's thunk) was a refusal. With esp as at the function's entry it is a tail call: the target returns to our caller. It now compiles as a callout (`aot_call_out`) with that caller's return address on the stack. The target is taken to pop what the function's own rets pop, which the callout checks at the return, as for an indirect call: where it differs, Box86 goes on at that return address. Where esp at the `jmp` isn't the entry's, it wasn't a tail call, and Box86 goes on at the target.

A thunk with no ret of its own (`004baea0`: `mov ecx, [ecx + 8]; mov eax, [ecx]; mov edx, [eax + 4]; jmp edx`) has nothing to say what it pops. It takes its import's pops where it jumps through the IAT, or what every one of its callers pushes where they all agree, checked the same way. Where neither is known (102 functions, reached only through vtables), it stays refused: compiled, its `jmp` would always hand over to Box86, and its callers would lose esp after calling it.

**Compiled:** 12,784 functions on d2d's list (98.0%), 2,537,110 bytes (97.6%), 98.4% of the lane's hot bytes (12,731, 97.4% and 98.1% before). None is refused for an indirect jump (155 before). 58 tail calls through a pointer in 53 functions are callouts. Calls from compiled code to Box86 code that may block fell from 4.5% to 1.5% of the hot bytes: the thunks they went to are compiled.

**The coverage set:** 2,370 functions compile and 47 are left to Box86 (2,361 and 54 before). The check is 0 differ on the three scripts (town-walk 29.2M calls, fight 53.9M, Act II 77.9M). `004bdee0`, whose `jmp eax` dispatches through a table by unit type, was entered from Box86 69,296 times in the fight. d2d's cases pass: 48 natives, 18,902 cases, 0 differ. The fight in production mode, alternated with `development` in one session, took 63 and 62 s against 63 and 63 s.

### The recompiler in Rust (#144)

`tools/aot/recompile` is `recompile.py` in Rust, module by module, with the same command line and the same output byte for byte: the C (but for the first line, which names the recompiler), `aot_spike.json`, and the census's `census.txt` and `census.json` but for their timings. Once it matched, `recompile.py` was removed; a commit's copy is still reachable for comparisons (`py@<rev>` below).
- **Capstone** through `capstone-sys` 0.18.0 (Capstone 5.0.6), the same calls Python's binding makes (`cs_disasm`, `cs_regs_access`, the operands' access and the eflags). Our own x87 tables stay: Capstone 5's access is wrong for x87 (#136).
- **The PE image** as pefile maps it, with pefile's names for some DLLs' imports by ordinal (its `ordlookup` tables, generated into `ordlookup.rs`), since the may-block rule and the import pops go by name.
- **Order:** Python's dicts keep their insertion order, and some of the output depends on it (the census's examples, the refusals' order), so the port has an insertion-ordered map. Where `recompile.py`'s output depended on a set's order instead, which is CPython's hashing, `recompile.py` now sorts first. Its C didn't change on the coverage set or on d2d's whole list; 10 refusal messages name a lower address, and the census's examples are in address order.
- **Built** in the `hosttest` image, from the crates its `Cargo.lock` pins (each checked against the lock's sha256 when the image is built), and run by `scripts/docker.sh` wherever `recompile.py` ran. The build ID's AOT hash covers its sources. `scripts/docker.sh test aot-compare dist/game <a> <b> [census]` runs two recompilers on the same Game.exe and compares what they write, with each one's time and peak memory. A side is `rs` (this tree's), `rs@<rev>` (a commit's crate, built under `build/aot-rs/`) or `py@<rev>` (a commit's `recompile.py`, such as `py@633e69a`, the last with it).

| | `recompile.py` | Rust |
| --- | --- | --- |
| The coverage set (2,417 functions) | 152 s, 2,374 MB | 10.8 s, 384 MB |
| d2d's whole list (13,048 functions) | 481 s, 5,169 MB | 36.5 s, 912 MB |

### What Box86 still runs (#138)

The goal is a build without Box86, so the measure is how much of the game's x86 code Box86 still runs in production. `scripts/docker.sh test aot-left dist/game` reads the profiled production run (`AOT_MODE=1 AOT_PARALLEL=1 D2_AOT_SET=whole AOT_ENV="D2_AOT_STALE=1 WX86_HOTPROF=1 D2_NOLINK=1 M4_HOTPROF_TOP=20000"`). It gives the bytes Box86 ran as a share of what the Box86 lane runs on the same scripts, by function and by why. Each step below kept the check at 0 differ, with the whole list and `D2_AOT_STALE=1`. The lane's state lines (reads, files, heap, frames) stayed the same on the three scripts.

| Step | Box86's share |
| --- | --- |
| Tail calls through a pointer | 6.50% |
| A frame's `mov ebp, esp` ends the count of a callout's pushed arguments | 1.40% |
| Return-site entries | 0.63% |
| Code pointers in immediates | 0.39% |
| Trampolines that move a branch | 0.22% |
| The natives' site table, its first two rows (`60bff0`), with `M4_LAYOUT=ctr` | 0.19% (0.24% without them, in that layout) |
| `d2_memory`'s patches as rows (pools, sprite frames, cache counter, fatal exit), with `M4_LAYOUT=ctr` | 0.18% (36.3M bytes against 39.4M) |
| Code pointers in the game's data, with `M4_LAYOUT=ctr` | 0.17% (34.5M bytes) |
| Callouts to compiled functions that can't be hooked, with `M4_LAYOUT=ctr` | 0.16% (32.1M bytes) |
| A callout's saved registers not counted as its arguments, with `M4_LAYOUT=ctr` | 0.10% (19.8M bytes) |
| `automap`, `storm_priority`, `storm_direct` and `frame_reads` as rows, with `M4_LAYOUT=ctr` | 0.06% (13.4M bytes) |
| A callout's prologue saves not counted as its arguments, with `M4_LAYOUT=ctr` | 0.04% (8.9M bytes) |

- **A callout's pushed arguments:** the walk back from a callout counted the pushes before it as its arguments. It also counted `push ebp` before `mov ebp, esp`, so `renderer_line` (4f6380) looked like it passed 20 bytes for 16. Its calls through the renderer's table became plain exits, and Box86 ran the rest of its callers' loops. A frame's `mov ebp, esp` now ends the walk.
- **Return-site entries:** a function that may exit has one more entry, `reent_<va>(R, at)`, with all eight registers. It is a `switch` on the site, over the function's own code. The sites are the return addresses of its callouts and of its calls to compiled functions that may exit: 48,651 sites in 9,509 functions. Its signature entry `sig_<va>` calls it at the function's entry, so the code exists once and the check covers it. Then a shim yields at a callout, with the registers and esp as the caller has them. The callout exits to a trap (`aot:reenter`), with the site kept by esp. When the thread runs again, the trap runs the function from the site to its ret, then each caller from its return site, until one isn't a site. Before this, each wait left the frame to Box86. One example is the main loop's `Sleep(0)` at 44c713: Box86 ran from 44c715 up, 19.9k times a fight window. This answers the open question below: the entries are needed where a call may exit, not at every call. The lane build's AOT code is 21.5 MiB, about 13 MiB without the entries. `AOT_NO_REENT=1` (recompiler) and `D2_AOT_REENT=0` (runtime) turn them off.
- **Code pointers in immediates:** some callbacks are stored, not called: 501650 into `[841da0]`, 463d80 into `ebx`. They aren't on d2d's list, and a call through them was an exit to Box86. The whole list's build now also compiles every immediate in a listed function that points at an address in `.text` on 16 bytes, after an `int3` or `nop` pad, and not inside a listed function's code. That adds 350 functions.
- **Code pointers in the game's data:** the renderer's functions are reached only through its table (the GDI renderer's at `0x74c4a8`, through `[7c8cc0]`), and 6c8060, 6c8030 and 6c7df0 aren't on d2d's list. So 4f6430's call through the table, under 494020's loop in Act II, was an exit, and Box86 ran the rest of the callers up to 4940ec. The whole list's build now also reads Game.exe's base relocations: every relocated dword in `.rdata` or `.data` that points into `.text`, by the immediates' rule. That adds 749 functions (13,567 compiled, 13,524 hooked). The check: 0 differ (town-walk 13.3M calls, fight 112.3M, Act II 48.5M). Profiled production: Box86 ran 34,475,453 x86 bytes against 36,312,337, with the state lines the same on the three scripts. 494020 is still first (19.69%).
- **Callouts to functions that can't be hooked:** a call through a pointer from compiled code ran the callee's compiled code only if the callee was hooked (`g_fn_at`). The other 322 compiled functions, whose first bytes can't take the hook's `jmp`, were exits, and Box86 ran the rest of the caller. One is 6c8030, a lone `ret` in the renderer's table. The runtime already checks every compiled function's bytes against Game.exe at the start, so it now also keeps those that can't be hooked and match (`g_unhooked_at`), and a callout runs their compiled code, the journaled one in the check. The check: 0 differ (town-walk 13.3M calls, fight 112.2M, Act II 48.4M). Profiled production: Box86 ran 32,104,821 x86 bytes against 34,475,453, with the state lines the same on the three scripts apart from the fight's reads and `va`, one window ahead at 90 and 100 s and the same by 110 s. 6c8060 left Act II's exits. 6c8030 is now handed back at its return (250 a window): its call site expects the callee to pop its argument, and a lone `ret` pops none. 494020 is still first (21.15%, the same bytes as before). Its exits go to 6c8a60, which is hooked: Box86 calls it 750 times a window and each call exits back to it.
- **A callout's saved registers:** the walk back from a callout stops at the block's start, and 4f6300's block starts with `push edi`, a save of the register it loads its arguments through (`mov edi, [ebp+0x1c]`; `push edi`), popped after the call. So its call through the renderer's table (`[7c8cc0]+0xb8`, 6c8a60 in Act II) counted 20 bytes for 16, two paths brought different esps to 4f6331, and the function fell back to plain exits: 750 exits a window to 6c8a60, under 494020's loop. A pop just after the call takes a dword the callee left. So where the pops after the call match the walk's first pushes, register for register, and the register is written between its push and the call, those pushes are saves and aren't counted. The write matters: 52e590's argument is `push edi` and the `pop edi` after its call is the prologue's save; without that condition 13 functions that had callouts fell back to plain exits, and Box86 ran 216M bytes (4dea70 first, through exits to 4f8b80). 46 functions have plain exits, against 55 (4f6300, 4f6340, 44b120, 56d810, 581220, 5a2100 and 611620 among those off the list); `aot_spike.json` now has each one's refusal with its callouts (`exits.plain_why`). The check: 0 differ (town-walk 13.2M calls, fight 112.1M, Act II 47.9M). Profiled production: Box86 ran 19,771,009 x86 bytes against 32,104,821, with the state lines the same on the three scripts apart from the fight's reads and `va`, one window behind at 90 and 100 s (as two steps before) and the same by 110 s. 494020 is off the list; first now are the code outside Game.exe (21.73%), 4df1c0 (8.14%), 44efa0 (5.42%) and 61bb80 (5.18%, refused: a switch the guard rule doesn't bound).
- **A callout's prologue saves:** 410a80 pushes esi and edi, loads edi from GetTickCount's import slot and calls it (`call edi`): the two saves counted as 8 bytes of arguments for an import that pops none, and each call came back to Box86 at 410a90, once a frame. 476bc0 pushes them before its call to the thunk 4f65c0 (`jmp [renderer+0x54]`), whose pops come from its callers' counts: 8 for none, back to Box86 at 476be0. The first pushes walked of ebx, esi, edi or ebp as the caller left them (some path from the entry reaches the push through no instruction that names the register, but a push, cmp or test) are the prologue's saves, and aren't counted: a function doesn't pass its caller's register. 20 functions have plain exits, against 46; 4,730 callouts against 4,572. The check: 0 differ (town-walk 11.9M calls, fight 108.5M, Act II 47.5M; fewer calls from Box86, with the callouts' callees now inside their callers' runs). Profiled production: Box86 ran 8,905,839 x86 bytes against 13,378,639 (0.04% of its own lane's), with the state lines the same on the three scripts. First now: the code outside Game.exe (20.58%), 61bb80 (11.50%, refused: a switch the guard rule doesn't bound), 4101c0 (9.51%), 451bb0 (9.47%), 44efa0 (7.51%) and the C runtime's sine and cosine (688590, 6886c0 and their pieces, refused).
- **Trampolines that move a branch:** a hook writes a `jmp` over a function's first 5 bytes or more. Its trampoline runs those bytes elsewhere, then jumps back. A function whose first bytes had a call, `jmp` or `jcc` couldn't be hooked, and Box86 ran it whenever Box86 code called it. That was 1,435 functions, 8.4% of what Box86 still ran: 475800 starts `push esi; push edi; call`, and 463d80 starts `push ecx; call`. The recompiler now writes their trampolines (`aot_tramps`): a rel8 or rel32 call, `jmp` or `jcc` becomes a rel32 to the same target, and an indirect one is copied as it is. The runtime makes each target relative to where it puts the trampoline. A `ret`, `loop`, `jecxz`, or a `jmp` before the 5th byte still refuses the hook (`AOT_NO_RELOC=1`, running the recompiler directly: none moved). 12,931 of 13,143 compiled functions can be hooked (11,708 before), 1,223 of them through a moved branch. The check runs these trampolines too, since it runs the original from them. In production the trampoline area is 64 KiB larger (the state lines' `va`).
- **Leftovers found to a fixpoint:** the check found 55c270 returning 55c110's leftover ecx (garbage: scratch that callers don't read, which compiled code doesn't keep). It had never been checked, because it couldn't be hooked. Each function's leftovers were found once, in callees-first order, so in a call cycle a caller could be screened before its callee. 55c270 then claimed ecx as an output, and the check compared it. Leftovers are now found again until none change: 265 functions.

**Tried and dropped: a plain exit's callee returning through the trap.** After a plain exit, Box86 runs the callee and then the rest of the caller from the raw return site. 494020's loop at 494110 is an example: Box86 ran it 30,516 times in Act II, from 2,543 returns at 4940ec. Pointing the callee's return address at the return sites' trap brought about 750 returns a window back into compiled code. But some callees use their return address as data, as the C runtime's frame helpers do. One came back with another esp, then reached the trap where no exit had recorded a site, and the run aborted. Limited to compiled callees, whose code is known, no exit used it at all. These returns need the callee compiled (or bound, below), not a swapped return address.

The trap's own entry (`aot:reenter`) shows in the runtime's "exits to Box86" line, about 39,750 a fight window, one per re-entry. Box86 doesn't run any game code for those.

### The natives' site table (#138)

The natives change the game's code at install time. They do it in their own `install_*` code: about 80 writes across `d2_native`, `d2_memory`, `storm_reads`, `sprite_trace`, `dcc_native`, `frame_reads`, `automap` and `cinematics`. The recompiler can't see those writes. So a function a native changed runs under Box86 at run time (47 functions in production, `60bff0`'s DccDecode loop among them), and compiled callers reach it through `aot_call_stale`. After the steps above, that is most of what Box86 still runs.

The patches move into one table that both the install code and the recompiler read. Each row gives:
- **Where:** the address, and the game's bytes there, which are checked as the install code checks them today.
- **What goes there:** the bytes written. A rel32 to a native is named (`{DccDecode}`); a rel32 to the game's code is written as its address.
- **The native:** its name, the dwords its ret pops, and whether it may redirect (a hand-back) or wait. The recompiler compiles a call to a native as a callout, with all eight registers.
- **Its group:** the install code applies a group when it decides to (a mode, a setting). The recompiler compiles the groups a production build applies. A function whose bytes still differ at run time (another mode, the lane's switches) stays with Box86 as now, by its hash.

Entry hooks (`rt::hook_function`, 40 in `d2_native`) are rows too: a `jmp` to the native over the function's first bytes. A compiled caller calls the native where it called the function. That replaces the recompiler's hand-written `natives()` (RoomFind and CollisionCross today).

The order: the table and its two readers first, with `dcc_native`'s loop as the first row (`60bff0`, 19% of what Box86 still runs). Then the natives one file at a time, with the check lane and the production state lines after each.

Done so far, in `src/game/native_sites_table.h`:

| Group | Native | Site | Bytes |
| --- | --- | --- | --- |
| `dcc` | DccDecode | `60c10a`, `60bff0`'s per-direction loop | 21 |
| `sprite_frames` | SpriteFramesAlloc, SpriteFramesFree, SpriteFramesPoolGone, SpriteFramesHeld | `60c103`, `60bff0`'s call to `40b430`; jumps at `409ab0`, `409c80`, `5ff72d`, `60002d` to four stubs in slack | 150 |
| `decode_timer` | SpriteDecodeBegin, SpriteDecodeEnd | `5ff6e0` to a stub in slack that calls the function's first bytes, moved after it | 57 |
| `pool_release` | PoolReleaseNext | `40a080` to a stub in slack | 66 |
| `pool_block_size` | none | `409fa8` to a stub in slack | 21 |
| `sprite_cache_counter` | SpriteCacheInsert | `5ff476` to a stub in slack | 30 |
| `fatal_exit` | FatalExit | `681e09`, its body moved to two runs of `int3` padding | 38 |
| `automap` | AutomapDecide, AutomapBegin, AutomapEnd | `456f93`, `456ee0`'s automap calls, to a stub in slack; `45a860` through a unit filter in slack | 117 |
| `storm_priority` | none | `41aeb2`, SFileReadFile's deadline for a queued read, to a stub in slack | 28 |
| `storm_direct` | StormDirectRead | `41ace2`, SFileReadFile's check for an asynchronous archive; `41a66c` and `41a725`, the read thread's flag, to two stubs in slack | 70 |
| `frame_reads` | FrameRead | `41aad0`, SFileReadFile's first 19 bytes | 19 |

- **Stubs in slack:** the recompiler takes an applied site's bytes as code even past `.text`'s virtual size, where the stubs are. Otherwise a function that jumps to one is refused ("code outside .text"), as `409ab0` and `40a080` were in the first run of these rows.
- **What the run chooses** (the frames' granules, the pools' block size) goes in data dwords the stubs read, so the bytes written are the same on every run. The pool-release stub saves `eax`, `ecx` and `edx` itself, since the recompiler doesn't compile `pushad`.
- **A call into a site:** the decode timer's stub calls its own tail, which runs `5ff6e0`'s first bytes and jumps to the rest. The recompiler compiles each such call's target (a direct call in an applied site to another applied site) as a function of its own (`6cb774`), so the call is compiled code to compiled code.
- **Not yet a row:** the sprite cache's floor (it needs a new stub, and its slack is taken). `sprite_trace` (its natives keep 6001f0 and 5ff4a0 compiled another way), `cinematics`, the storm read thread's rate (an immediate the run chooses) and `d2_native`'s entry hooks.
- **Storm's flag:** the read thread's flag is in scratch memory the run allocates, since a write to a page of code throws its translations away. Its stubs had the flag's address in their bytes. They now read it from the data dword at `6cb7dc`, in the slack (`mov ebx, [P]; mov byte [ebx], 1`, and with `eax` saved around it in the stub that clears it), so the bytes are the same on every run.
- **`automap`, `storm_priority`, `storm_direct` and `frame_reads`, with `M4_LAYOUT=ctr`:** 34 sites, 617 bytes. The check: 0 differ (town-walk 13.1M calls, fight 111.8M, Act II 47.7M). `41a550`, `41aad0`, `456ee0` and `45a860` are off the changed list (40 functions, against 44). Profiled production: Box86 ran 13,378,639 x86 bytes against 19,771,009 (0.06% of its own lane's), with the state lines the same on the three scripts. The two lanes ran side by side (`AOT_OUT`, `AOT_BIN` in `scripts/docker.sh`: a run's folders of its own, with the binary another built), about 250 MiB an emulator.
- **At run time:** `apply_sites` (`native_sites.cpp`) checks every row's game bytes, then writes them with the native's real entry. The code hash reads each site as compiled, so a function changed only by applied rows still matches its compiled code.
- **In the recompiler:** native k is an import at `0xfee00000 + 16k`, with its pops. `aot_call_out` and `aot_exit` map that address to the native's entry. A `jmp` to a native is a tail call to that import.
- **A group compiled but not applied** leaves its functions changed since compiled, so Box86 runs them as before. The runtime names such groups after the changed list. The lane runs the PC layout by default, which doesn't install the sprite-frames allocator (only the 3DS layouts do, as the app always does). So `60bff0` stayed with Box86 there. The lane's checks of the table run with `M4_LAYOUT=ctr`.
- **The check lane with `M4_LAYOUT=ctr`:** 0 differ (town-walk 13.7M calls, fight 113.1M, Act II 49.2M), and `60bff0` is off the changed list on all three scripts. A call that reaches a native counts as unchecked: the compiled run's result is kept, so the native doesn't run twice.
- **Profiled production with `M4_LAYOUT=ctr`:** Box86 ran 0.19% of its own lane's x86 bytes, against 0.24% for the same build with both groups at `compiled 0`, where `60bff0` was 18.16% of what Box86 still ran. In the state lines, reads, files, heap, va and free are the same on the three scripts. `peeks` is 44 to 59 higher from the load window on, and Act II has one frame more at 50 s (1501, the same again by 60 s). The lane runs the DCC native off, so `60bff0` decodes with the game's code. In Box86 that work counts toward a thread's slice, compiled it doesn't: the load window had 39,904 thread switches against 39,912. The virtual clock moves 250 us a switch, so the game's idle loop peeks a few more times.
- **`d2_memory`'s rows, the check lane with `M4_LAYOUT=ctr`:** 0 differ (town-walk 13.3M calls, fight 112.3M, Act II 48.5M). 12,889 functions hooked, 6 more than before. `409ab0`, `409c80`, `409dd0`, `40a080`, `5ff3f0` and `681e09` are off the changed list. `5ff6e0` is on it now that it compiles, because the decode timer, not yet a row, still changes it.
- **The decode timer's rows, the check lane with `M4_LAYOUT=ctr`:** 0 differ (the same calls as above), 22 sites and 383 bytes, `5ff6e0` off the changed list. Not measured in production on its own: Box86 ran `5ff6e0` 6 times in the three scripts.
- **`d2_memory`'s rows, profiled production with `M4_LAYOUT=ctr`:** Box86 ran 36,312,337 x86 bytes, against 39,408,605 with the first two rows (0.18% of its own lane's, against 0.19%), in 459 hooked functions against 646. The state lines' frames, files and heap are the same on the three scripts. `peeks` differs by 3 to 25. In the fight, reads and va at 90 s and 100 s are ahead by up to 53 KiB and 192 KiB, and the same again by 110 s: the same reads, at other times on the virtual clock.

## Open questions

- **Is the gain there?** Yes, on the console: about 5× Box86 on A* and 3–4.7× on CofParams (#113). The lane's qemu showed 4.1× and 1.6×. What a full build gains depends on how much of a frame's code it covers (#115), and on its calls to code it doesn't compile going through the target table rather than a nested run.
- **The site table:** the hooks and code patches move from `install_*` code into one table that both the app and the recompiler read. Is that change acceptable before the spike, or only after it? Now: [The natives' site table](#the-natives-site-table-138).
- **Does the game write its own code?** If anything other than our patches writes into `.text` at run time, the AOT code for it is stale. Box86's invalidations of `.text` on the lane answer this.
- **Exceptions:** does any function rely on Windows exception handling for a fault (an access violation caught by `__try`)? If one does, it stays with Box86.
- **Return sites:** does every call's return site need an entry, or only those a thread can be switched or fall back under? Only those where a call may exit ([What Box86 still runs](#what-box86-still-runs-138)).
- **`LODPatch_114d.exe`:** how its Game.exe is stored, and whether extracting it needs anything we would rather not depend on.
- **Faults:** is a report with the function and offset enough, or does the check build need precise x86 state at each memory access?
