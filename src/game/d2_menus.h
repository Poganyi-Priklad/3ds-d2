// The windows Diablo II 1.14d has open, for menu navigation (menu_nav.h):
// where their cells are, from the game's own tables and from the frame.
//
// Facts for Game.exe 1.14d, checked on the Linux lane:
//   0x007D55BC   out of the game, D2Win's current screen: its first
//                control, +0x3C the next (menu_nav.h's D2WinControl; read
//                only out of the game: it is stale in it).
#ifndef D2_GAME_D2_MENUS_H
#define D2_GAME_D2_MENUS_H

#include "game/menu_nav.h"

#include <cstdint>

namespace game {

// A finished frame (the present callback). Later commits look for an NPC
// menu box here; for now this is a no-op hook so the app can call it.
void menus_frame(const uint8_t *pixels, const uint32_t *palette, uint32_t width, uint32_t height, int32_t stride);

// The open windows for the controls (Controls::set_menu_nav). Out of the
// game this is D2Win's current screen; in the game, later commits fill it.
MenuWorld &game_menu_world();

}  // namespace game

#endif
