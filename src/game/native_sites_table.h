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

// d2_memory.cpp, install_sprite_frames: the sprites' frames in blocks of
// their own (issue #44). 0x60bff0's call to 0x40b430 at 0x60c103 asks a
// native for the group's frames. 0x409ab0 (a pool's free) sends a pointer in a
// 64 KiB granule the frames hold (the byte map at 0x6cb8c0; the granules'
// base and length in the data dwords at 0x6cb8b8 and 0x6cb8bc) to a native.
// 0x409c80 (a pool system's destroy) tells a native first. 0x5ff6e0 counts a
// decoded group as the block its frames hold (0x5ff72d), and 0x5fffa0 a DCC
// file's bytes as none (0x60002d). The stubs are in .text's zero slack.
NATIVE(SpriteFramesAlloc, 3)
NATIVE(SpriteFramesFree, 1)
NATIVE(SpriteFramesPoolGone, 1)
NATIVE(SpriteFramesHeld, 2)
GROUP(sprite_frames, 1)
SITE(sprite_frames, 0x0060c103, "e8 28 f3 df ff", "e8 {SpriteFramesAlloc}")
SITE(sprite_frames, 0x006cb820, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "8b 02 2b 05 b8 b8 6c 00 3b 05 bc b8 6c 00 73 1e c1 e8 10 80 b8 c0 b8 6c 00 00 74 12 51 52 ff 32 e8 {SpriteFramesFree} 5a 59 85 c0 74 03 c2 08 00 55 8b ec 51 53 e9 {=0x409ab5}")
SITE(sprite_frames, 0x006cb860, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "51 52 ff 74 24 0c e8 {SpriteFramesPoolGone} 5a 59 55 8b ec 83 ec 08 e9 {=0x409c86}")
SITE(sprite_frames, 0x006cb880, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "ff 76 18 ff 76 14 e8 {SpriteFramesHeld} 6a 00 50 e9 {=0x5ff733}")
SITE(sprite_frames, 0x006cb8a0, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "50 8b 45 08 f6 00 02 58 74 02 33 c0 50 e8 {=0x5ff630} e9 {=0x600033}")
SITE(sprite_frames, 0x00409ab0, "55 8b ec 51 53", "e9 {=0x6cb820}")
SITE(sprite_frames, 0x00409c80, "55 8b ec 83 ec 08", "e9 {=0x6cb860} 90")
SITE(sprite_frames, 0x005ff72d, "8b 4e 18 6a 00 51", "e9 {=0x6cb880} 90")
SITE(sprite_frames, 0x0060002d, "50 e8 fd f5 ff ff", "e9 {=0x6cb8a0} 90")

// d2_memory.cpp, install_pool_release: 0x40a080 (the pool system's
// allocation) frees the blocks a native hands it first, while the flag byte
// at 0x6cb6a0 is set: VirtualFree and free (0x683fc0) for each.
NATIVE(PoolReleaseNext, 0)
GROUP(pool_release, 1)
SITE(pool_release, 0x006cb660, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "80 3d a0 b6 6c 00 00 74 28 50 51 52 e8 {PoolReleaseNext} 85 c0 74 19 52 68 00 80 00 00 6a 00 50 ff 15 20 c2 6c 00 e8 {=0x683fc0} 83 c4 04 eb de 5a 59 58 55 8b ec 53 8b d9 e9 {=0x40a086}")
SITE(pool_release, 0x0040a080, "55 8b ec 53 8b d9", "e9 {=0x6cb660} 90")

// d2_memory.cpp, set_pool_block_size: 0x409dd0's blocks of 0x80000 bytes
// (mov eax, 0x80000 at 0x409fa8) become the data dword's at 0x6cb6a4, or one
// unit where a unit is larger.
GROUP(pool_block_size, 1)
SITE(pool_block_size, 0x006cb6b0, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "a1 a4 b6 6c 00 3b f0 7e 02 8b c6 e9 {=0x409fad}")
SITE(pool_block_size, 0x00409fa8, "b8 00 00 08 00", "e9 {=0x6cb6b0}")

// d2_memory.cpp, install_sprite_cache_counter: 0x5ff3f0 counts a sprite into
// the cache at 0x5ff476; a native counts it too.
NATIVE(SpriteCacheInsert, 0)
GROUP(sprite_cache_counter, 1)
SITE(sprite_cache_counter, 0x006cb6c0, "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "e8 {SpriteCacheInsert} 8b 55 08 83 47 0c 01 01 57 08 e9 {=0x5ff480}")
SITE(sprite_cache_counter, 0x005ff476, "8b 55 08 83 47 0c 01 01 57 08", "e9 {=0x6cb6c0} 90 90 90 90 90")

// d2_memory.cpp, install_fatal_exit: exit (0x681e09) with -1 goes to a native
// that ends the run; any other code runs exit's body, moved to two runs of
// int3 padding (0x6814d1 and 0x681531).
NATIVE(FatalExit, 0)
GROUP(fatal_exit, 1)
SITE(fatal_exit, 0x006814d1, "cc cc cc cc cc cc cc cc cc cc cc cc cc", "6a 00 6a 00 ff 74 24 0c e9 {=0x681531}")
SITE(fatal_exit, 0x00681531, "cc cc cc cc cc cc cc cc cc", "e8 {=0x681d27} 83 c4 0c c3")
SITE(fatal_exit, 0x00681e09, "6a 00 6a 00 ff 74 24 0c e8 11 ff ff ff 83 c4 0c", "83 7c 24 04 ff 0f 85 {=0x6814d1} e9 {FatalExit}")

#ifdef NATIVE_SITES_TABLE_DEFAULTS
#undef NATIVE
#undef GROUP
#undef SITE
#undef NATIVE_SITES_TABLE_DEFAULTS
#endif
