// CpuAot (issue #138, docs/aot.md "Threads that wait in place"): a Cpu backend
// with no Box86. The game's code is the AOT recompiler's C; each guest thread
// runs it on a host stack of its own (a fiber), so a shim that waits stops
// the thread where it is. What it can't run (code that isn't compiled) stops
// the run with a report: the strict lane.
#pragma once

#include "runtime/cpu.h"

namespace d2rt {

// The backend (one a process), or nullptr where fibers aren't available.
Cpu *make_cpu_aot();
// Whether `c` is it.
bool cpu_is_aot(Cpu &c);

// The code at `eip` run until control is at `until` (0: until a trap stops
// the run): the game layer's (aot_spike.cpp), which has the compiled
// functions. False when a trap stopped the run (the thread's sentinel).
using CpuAotDispatch = bool (*)(Cpu &c, uint32_t eip, uint32_t until);
void cpu_aot_set_dispatch(CpuAotDispatch fn);

// The trap at `va` (the Bridge's: a shim's slot, the sentinel), with the
// registers as they are in `c`. False when it stops the run.
bool cpu_aot_trap(Cpu &c, uint32_t va);
// Whether `va` is in the trap range.
bool cpu_aot_is_trap(Cpu &c, uint32_t va);
// The running thread stops here until the scheduler runs it again (a yield or
// a wait the trap just asked for); the registers are `c`'s on both sides.
void cpu_aot_yield(Cpu &c);
// The running thread's x87 state (416 bytes, aot_rt.h's AotFpu).
void *cpu_aot_fpu(Cpu &c);
// Code changed since this was last asked (a native's hook): true once.
bool cpu_aot_take_code_changed(Cpu &c);
// After a trap that asked for no yield: whether the running thread's slice is
// over (its share of trap calls made). True once; cpu_aot_yield then.
bool cpu_aot_slice_over(Cpu &c);
// The run can't go on: `what` logged, then the process ends.
[[noreturn]] void cpu_aot_stop(const char *what, uint32_t at);

}  // namespace d2rt
