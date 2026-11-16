// The AOT spike (issue #113, docs/aot.md): Game.exe functions compiled to C
// by tools/aot/recompile.py from the user's own Game.exe, linked in when the
// build is given them (build/aot/, never committed), and hooked at their
// entries as natives that aren't served in line.
#ifndef D2_GAME_AOT_SPIKE_H
#define D2_GAME_AOT_SPIKE_H

#include <cstdint>
#include <string>
#include <vector>

namespace d2rt { struct Cpu; }

namespace game {

enum class AotMode : int {
    Off = 0,
    Sig = 1,        // per-function register signatures
    Ctx = 2,        // all eight registers through a context at every call
    SigCalls = 3,   // as Sig, no compiled function inlined into another
    CtxCalls = 4,   // as Ctx, likewise
    Check = 5,      // Sig journaled, memory put back, Box86 runs the original, compared
    Bench = 6,      // each call by one of Box86, the four builds and the native, in turn, timed
};

// Whether this build has the compiled code (aot_roots linked).
bool aot_linked();
// The mode to start in (the app, before install_d2_native); D2_AOT on the
// lane overrides it.
void set_aot_wanted(AotMode mode);
// Only the hookable functions with these indexes (settings.txt's aotfns=, as
// the lane's D2_AOT_FNS; issue #115): before install_aot.
void set_aot_fns(uint32_t first, uint32_t last);
// From install_d2_native, before its hooks: with a mode other than Off the
// roots are hooked, and a native on the same entry (PathSearch) then isn't.
// Returns how many roots there are.
int install_aot(d2rt::Cpu &c);
// After every native's hook (the lane, issue #115): D2_AOT's Sig or Check
// turned on, as the console's OPTIONS row turns it on once they're in; what
// they changed stays with Box86 (set_aot_mode).
void aot_after_natives(d2rt::Cpu &c);
// The console's switch (the OPTIONS page): Bench or Off, at once, from
// the game's thread between frames. Off puts back what was at each entry
// (the game's bytes or a native's jump).
void set_aot_mode(d2rt::Cpu &c, AotMode mode);
AotMode aot_mode();
// The heartbeat's lines ("aot: ..."), each window's calls and times; empty
// when nothing ran.
std::vector<std::string> aot_lines();

// The natives' entry for Bench: called as the hook would call it.
using AotNativeFn = uint32_t (*)(d2rt::Cpu &);
void aot_set_native(uint32_t va, AotNativeFn fn);

}  // namespace game

#endif
