// What the PAD window (pad_story.h) reads from Diablo II 1.14d's memory,
// and its art (pad_art.h) loaded from the player's own MPQs. Checked on the
// Linux lane (and in the PAD runs):
//   0x007BF1C0  1 while an NPC's talk text scrolls, 0 after
//   0x007BF0A4  the talk's text: {its first line's node, its line count,
//               how far it has scrolled in 1/1024 pixels (as fast as its
//               speech: Warriv about 10.7 pixels a second, Akara slower)};
//               a node is {UTF-16 text, next node, 0, 0}, the next 0 at the
//               end. The whole talk is there from its start, wrapped as the
//               game shows it (blank lines between paragraphs)
//   0x007BF250  the pool the nodes come from (+0x08 a 16-byte header, its
//               first slot right after it): the first line's node the first
//               time only; a talk told again may reuse its address
//   0x007C0E54  the NPC menu while its box shows: +0x68 its title's own
//               UTF-16 text, the NPC's name (a submenu's title, or the chosen
//               line for a moment, otherwise)
//   0x007A6A94, 0x007A6A8C, 0x007A6A78: the unit under the cursor (on, its
//               type and id; d2_units.h)
//   a monster's data (unit +0x14) +0x2C: its name, UTF-16 (checked on the lane with a Fallen)
//   an object's data +0x00: its objects.txt row, whose +0x40 is its name,
//               UTF-16 (checked with a waypoint)
//   unit +0x5C the stat list (d2_units.h's player_stat): a monster's 6 and
//               7 are its life and its most (32768 when whole: the client
//               keeps them as a fraction)
//   0x00841D8C  the font folder the game picked for its language ("CYR\")
#ifndef D2_GAME_PAD_GAME_H
#define D2_GAME_PAD_GAME_H

#include "game/d2_state.h"
#include "game/pad_art.h"
#include "game/pad_story.h"

namespace game {

// One poll: a few dwords, the hovered unit's name and life, and the talk's
// lines only when a new talk starts.
void read_pad_game(const GameState &gs, PadGame &out);

class SecondScreen;
// Each poll before SecondScreen::update, while PAD shows: the game read,
// the art handed over once, and the window's changes logged (its mode, the
// name, a story's lines, an enemy's life at most twice a second).
void pad_poll(SecondScreen &screen, const GameState &gs, uint64_t now_ms);

// The art, decoded on the first call (tens of milliseconds, once) and
// kept; nullptr if the files could not be read (logged once).
const PadArt *pad_art();

}  // namespace game

#endif
