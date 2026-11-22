// The windows Diablo II 1.14d has open, for menu navigation (menu_nav.h):
// where their cells are, from the game's own tables and from the frame.
//
// Facts for Game.exe 1.14d, checked on the Linux lane:
//   0x0096D4F4   inventory.txt's rows, 0xF0 bytes each: +0x00 the panel
//                (left, right, top, bottom), +0x10 the grid's columns and
//                rows (bytes), +0x14 the grid (left, right, top, bottom),
//                +0x28 ten equipment slots of 0x14 bytes (left, right, top,
//                bottom). Rows 0-4 the classes Amazon to Barbarian, 5 a
//                shop, 6 and 7 a trade, 9 the Horadric Cube, 12 the stash,
//                13 the mercenary, 14 Druid, 15 Assassin; at 800x600 the
//                same 16 rows again from row 16. A shop's row and the
//                class's row at both sizes give how far 800x600 moves the
//                left and right halves (PanelShift: 80, 60 both).
//   0x0096D4F8   belts.txt's rows, 0x108 bytes each: +0x04 the number of
//                boxes, +0x08 sixteen boxes (left, right, top, bottom).
//                Rows 0-6 at 640x480 (2: no belt, four boxes), 7-13 at
//                800x600.
//   0x00722354   the worn belt's belts.txt row (0-6; 2 with no belt), which
//                the game sets each frame from the equipped belt (items.txt
//                +0x130).
//   0x007BEFA0   non-zero while the belt's upper rows are shown: the belt
//                open (0x007BEF98: the cursor on it, or it clicked) and a
//                belt of more than one row.
//   0x007C0E54   an NPC's menu while its box is shown (0 during a talk's
//                text and once closed): +0x24 the NPC's x on the screen,
//                +0x2C the box's left, +0x30 top, +0x34 width, +0x38
//                height, +0x4C the options (Cancel too), +0x50 the lines
//                (the title too), +0x68 two string numbers a line.
//   0x007A27C0   the UI table (d2_state.h): 0x08 an NPC's menu or talk
//   0x007BC938   the game menu (Escape) and its submenus while the UI table
//                has 0x09: the lit row; 0x007BC93C the open menu's header
//                (+0x00 its rows, +0x04 pixels between them); 0x007BC940
//                its rows, 0x550 bytes each: +0x00 the kind (-1 a title, 0
//                a button, 1 a choice, 2 a slider), +0x0C its name
//                ("Previous", "SPrevious"... go one menu back). Stale once
//                the menu is closed. Configure Controls is the UI table's
//                0x0B, with 0x09 off.
//   0x007A279C   the gold dialog's form, kept once it closes: 1 drop, 2 a
//                trade, 3 deposit, 4 withdraw (menu_nav.h's GoldDialog);
//                0x007A27A0..0x007A27B0 its five parts while it is open,
//                0x007A27B4 1 while it is open. Its third part is the
//                amount field (vtable 0x006DA7CC): +0x40 its characters
//                (UTF-16), +0x48 the most it takes (the game clamps typing
//                to it), +0x50 its length << 16 | its longest (10). Its
//                fourth and fifth are OK and Cancel (vtable 0x006DA72C,
//                +0x20 x, +0x24 the bottom edge, +0x30 width, +0x34
//                height). All read on the Linux lane in the drop, deposit
//                and withdraw forms.
//   0x007C0D43   the client's quest record: [+0] its flags, 16 a quest;
//                flag 0 of quests 7, 15, 23, 26: acts II to V reached (the
//                waypoints' and the quest log's tabs, Game.exe 0x49cbaa).
//   0x007D55BC   out of the game, D2Win's current screen: its first
//                control, +0x3C the next (menu_nav.h's D2WinControl; read
//                only out of the game: it is stale in it).
//   Should the menu's pointer not read, the box is found in the frame: a
//   box over the NPC with a two-pixel light border (dark inside).
#ifndef D2_GAME_D2_MENUS_H
#define D2_GAME_D2_MENUS_H

#include "game/menu_nav.h"

#include <cstdint>

namespace game {

// A finished frame (the present callback): looks for an NPC's menu box
// while the UI table has one open and the game's own menu doesn't read.
// Cheap otherwise (a few reads).
void menus_frame(const uint8_t *pixels, const uint32_t *palette, uint32_t width, uint32_t height, int32_t stride);

// The NPC menu's box: the game's own, else the one the last frame showed;
// false if none.
bool npc_menu_box(NavRect &out);

// The open windows for the controls (Controls::set_menu_nav).
MenuWorld &game_menu_world();

}  // namespace game

#endif
