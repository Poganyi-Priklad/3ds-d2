// The windows Diablo II 1.14d has open, for menu navigation (menu_nav.h):
// where their cells are, from the game's own tables and from the frame.
//
// Facts for Game.exe 1.14d, checked on the Linux lane:
//   0x007BC938   the game menu (Escape) and its submenus while the UI table
//                has 0x09: the lit row; 0x007BC93C the open menu's header
//                (+0x00 its rows, +0x04 pixels between them); 0x007BC940
//                its rows, 0x550 bytes each: +0x00 the kind (-1 a title, 0
//                a button, 1 a choice, 2 a slider), +0x0C its name
//                ("Previous", "SPrevious"... go one menu back). Stale once
//                the menu is closed. Configure Controls is the UI table's
//                0x0B, with 0x09 off.
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
