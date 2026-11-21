# Fork: menu navigation (flicks in menus and panels)

This branch builds on the [auto-target](fork-auto-target.md) branch. It lets you
move through the game's menus with quick flicks of the circle pad, one cell or
option at a time, the way console games usually work, instead of dragging a
free cursor across them. A slow tilt still moves the cursor freely, as in the
original port.

![Menu navigation: one-cell flicks, free movement on a slow tilt, and a flick across to a neighbouring window](images/fork-menu-nav.png)

The full rules are in [controls.md](controls.md#menu-navigation); this page is
the short version and a map of the code.

This first part covers the screens before the game, and the shared flick core
the later parts use.

## How to use it

**Flick** the circle pad (a quick push out of the centre and back; the same
flick the auto-target uses, set by `aim_flick_ms` and `aim_flick_light`) up,
down, left or right:

- **In open panels** (inventory and equipment, stash, shop, Horadric Cube,
  trade, the mercenary, the belt, the character screen and its "+" buttons,
  the skill tree, the quest log, the waypoints): the cursor moves one cell.
  At a window's edge it goes into the window next to it at the same relative
  place, or comes round to the far side. With an item on the cursor, a flick
  off the window puts the cursor on the free half of the screen to drop it.
  In the inventory, the weapon swap tabs drawn as "I" and "II" over each
  weapon are one button per weapon (either swaps the weapons, as W does): a
  flick up from a weapon reaches the tab over it.
- **In an NPC's menu**: up and down move between its options.
- **In the game menu** (Start) and its submenus: a push of the circle pad up
  or down (half way out, back to the centre between two) is the game's own
  Up or Down arrow key, and the cursor follows the lit row, so **A** takes
  it; left and right change a choice or a slider.
- **Before the game** (main menu, character select, class screen, difficulty,
  multiplayer screens): the cursor jumps to the next enabled button, text
  field, list, character slot or hero. On the character screen with more than
  eight heroes, a flick right from the right column reaches the scroll bar's
  up and down buttons, and A scrolls one row a press.

## Button mapping (what this commit changes)

| Button | Before | With menu navigation |
| --- | --- | --- |
| Circle pad flick, panel or menu open | Moves the free cursor a little | Moves the cursor one cell or option |
| Circle pad slow tilt, panel or menu open | Moves the free cursor | Unchanged |
| B, panel or NPC menu open | Right mouse button | **Escape**: closes the panel or the NPC menu, goes one menu back |
| B, game submenu | Right mouse button | **Previous** (one menu back); in the top menu, Escape |
| A, the game's interface (bottom bar, globes, mini-panel) | Left button, possibly aimed | A plain click where the cursor is |
| B, before the game (character select, class screen, difficulty, multiplayer screens) | Right mouse button | **Escape**: back to the screen before; on the main menu B does nothing |

The flick itself uses the auto-target's `aim_flick_ms` and `aim_flick_light`.

## How it works

Menu navigation, like the auto-target, only produces mouse and keyboard input.
It reads the game's memory to know where things are and never writes it.

| File | What it does |
| --- | --- |
| `src/game/menu_nav.h`, `.cpp` | The rules, as pure geometry: which cell a flick goes to, between windows, round the edges, the scroll column; tested with made-up windows |
| `src/game/d2_menus.h`, `.cpp` | Reads where the open windows and their cells are: `inventory.txt` and `belts.txt` rows, the NPC menu, the game menu's rows and lit row, D2Win's control list before the game (with the frame's pixels as a fallback for the NPC menu box) |
| `src/game/controls.cpp` | Runs it: flicks to cells, B as Escape or Previous, the arrow keys in the game menu, plain clicks on the interface |

`game.log` gets `menu:` lines saying where each flick went.

When a text field (a hero name, a game name) loses the focus, the on-screen
keyboard closes by itself. While it is open, the sticks and A still reach the
game (`keyboard_pass_through`).

## Tests

```
scripts/docker.sh test menu       # menu navigation against made-up windows (tests/menu)
scripts/docker.sh test aim        # the auto-target, the in-world walk, and the flick this branch shares
scripts/docker.sh test settings   # settings.txt
```
