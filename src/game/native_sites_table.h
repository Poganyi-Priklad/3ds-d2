// The natives' sites (issue #138, docs/aot.md "The natives' site table"):
// every change a native makes to the game's code, in one table that the
// install code (native_sites.cpp) and the AOT recompiler
// (tools/aot/recompile, which compiles the code as changed) both read.
//
// Included with the three macros defined; the recompiler reads these lines
// as text, so each row stays on one line, with plain literals:
//
//   NATIVE(name, stack_dwords)   a native the sites call: rt::native_entry's
//                                name and the dwords its ret pops
//   GROUP(name, compiled)        sites applied together, by the install code
//                                that owns them; compiled 1: the recompiler
//                                compiles the game's code with them
//   SITE(group, va, "game bytes", "bytes written")
//
// Bytes are hex pairs. In the bytes written, {Name} is the rel32 to that
// native (4 bytes) and {=0x...} the rel32 to that address in the game's code.
// Nothing written depends on the run: a value the run chooses goes in data
// the site's code reads.

#ifndef NATIVE
#define NATIVE(name, stack_dwords)
#define GROUP(name, compiled)
#define SITE(group, va, game_bytes, bytes_written)
#define NATIVE_SITES_TABLE_DEFAULTS
#endif

// dcc_native.cpp: 0x60bff0's per-direction loop, 0x60c10a (21 bytes), becomes
// a call to the native decoder and the loop's compare.
NATIVE(DccDecode, 0)
GROUP(dcc, 1)
SITE(dcc, 0x0060c10a, "8b 45 14 3b 85 c0 fc ff ff 89 b5 8c fc ff ff 89 b5 d0 fc ff ff", "e8 {DccDecode} 3b 85 c0 fc ff ff 90 90 90 90 90 90 90 90 90 90")

// d2_memory.cpp: the frames of a decoded group, 0x60bff0's call to 0x40b430
// at 0x60c103, from the frames' own blocks.
NATIVE(SpriteFramesAlloc, 3)
GROUP(sprite_frames_alloc, 1)
SITE(sprite_frames_alloc, 0x0060c103, "e8 28 f3 df ff", "e8 {SpriteFramesAlloc}")

#ifdef NATIVE_SITES_TABLE_DEFAULTS
#undef NATIVE
#undef GROUP
#undef SITE
#undef NATIVE_SITES_TABLE_DEFAULTS
#endif
