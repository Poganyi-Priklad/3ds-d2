# Fork: menu navigation (flicks in menus and panels)

This branch builds on the [auto-target](fork-auto-target.md) branch. It lets you
move through the game's menus, panels and out-of-game screens with quick
flicks of the circle pad, one cell or option at a time, the way console games
usually work, instead of dragging a free cursor across them. A slow tilt still
moves the cursor freely, as in the original port.

![Menu navigation: one-cell flicks, free movement on a slow tilt, and a flick across to a neighbouring window](images/fork-menu-nav.png)

The full rules are in [controls.md](controls.md#menu-navigation); this page is
the short version and a map of the code.

## How to use it

**Flick** the circle pad (a quick push out of the centre and back; the same
flick the auto-target uses, set by `aim_flick_ms` and `aim_flick_light`) up,
down, left or right:

- **In open panels** (inventory and equipment, stash, shop, Horadric Cube,
  trade, the mercenary, the belt, the character screen and its "+" buttons,
  the skill tree, the quest log, the waypoints): the cursor moves to the
  nearest cell that way, in whichever open window it is; with nothing that
  way it comes round to the far side, over all the open windows. With an
  item on the cursor and nothing that way, a flick puts the cursor on the free half of the screen to drop it.
  In the inventory, the weapon swap tabs drawn as "I" and "II" over each
  weapon are one button per weapon (either swaps the weapons, as W does): a
  flick up from a weapon reaches the tab over it.
  The panels' close buttons are cells too (inventory, stash, shop,
  character screen, skill tree, quest log, waypoints), so A there closes
  the panel. The gold coins are cells too: the stash's (withdraw) over its grid, the
  inventory's (drop, or deposit with the stash open) under it. A on a coin
  opens the game's gold dialog.
- **In the gold dialog** (drop, deposit, withdraw, trade): a flick goes
  between OK and Cancel. The D-pad sets the amount instead of using the
  belt: up and down +1 and -1, right and left +100 and -100. Held, it
  repeats faster and faster, and the step grows tenfold every 2 s (up to
  ×1000). The amount stays between 0 and the most the game allows. A on OK
  or R + Start (Enter) moves the gold; B (Escape) closes the dialog.
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
- **With one side panel open** (inventory or skill tree on the right; character
  screen, quest log, party or mercenary on the left): hold the circle pad out
  for `aim_panel_walk_ms` (280 ms; `0`: never) to walk the hero on the free
  half. The walk itself is the auto-target's (it keeps off units and warps,
  and stops when you let go). A shorter flick still moves between cells.

## Button mapping (what this branch changes)

| Button | Before | With menu navigation |
| --- | --- | --- |
| Circle pad flick, panel or menu open | Moves the free cursor a little | Moves the cursor one cell or option |
| Circle pad slow tilt, panel or menu open | Moves the free cursor | Unchanged |
| Circle pad held, one side panel open | Moves the free cursor | Held for `aim_panel_walk_ms` (280 ms): starts the auto-target's walk on the free half of the screen; a flick still moves between cells |
| A or B with the cursor on the free half, one side panel open | Plain button | Aims as with no panel, only at targets on the free half, from the hero's real place on the screen |
| B, panel or NPC menu open | Right mouse button | **Escape**: closes the panel or the NPC menu, goes one menu back |
| B, game submenu | Right mouse button | **Previous** (one menu back); in the top menu, Escape |
| B, before the game (character select, class screen, difficulty, multiplayer screens) | Right mouse button | **Escape**: back to the screen before; on the main menu B does nothing |
| D-pad, gold dialog open | Belt potions 1–4 | The amount: up/down +1/-1, right/left +100/-100, faster while held |
| A, the game's interface (bottom bar, globes, mini-panel) | Left button, possibly aimed | A plain click where the cursor is |
| R + B, ZR + B | As before | As before |

## Settings

| Key | Default | Meaning |
| --- | --- | --- |
| `aim_panel_walk_ms` | `280` | With one side panel open, holding the circle pad this long starts the auto-target's walk (`0`: never) |

The flick itself uses the auto-target's `aim_flick_ms` and `aim_flick_light`.

## How it works

Menu navigation, like the auto-target, only produces mouse and keyboard input.
It reads the game's memory to know where things are and never writes it.

| File | What it does |
| --- | --- |
| `src/game/menu_nav.h`, `.cpp` | The rules, as pure geometry: which cell a flick goes to, between windows, round the edges, the scroll column; tested with made-up windows |
| `src/game/d2_menus.h`, `.cpp` | Reads where the open windows and their cells are: `inventory.txt` and `belts.txt` rows, the NPC menu, the game menu's rows and lit row, D2Win's control list before the game (with the frame's pixels as a fallback for the NPC menu box) |
| `src/game/controls.cpp` | Runs it: flicks to cells, B as Escape or Previous, the arrow keys in the game menu, the gold dialog's D-pad (it types the amount over the field: Backspaces, then the digits, as the on-screen keyboard does), one-panel walk dispatch (hold delay and free-half clip, then the auto-target's walk), plain clicks on the interface |

`game.log` gets `menu:` lines saying where each flick went.

## Tests

```
scripts/docker.sh test menu       # menu navigation against made-up windows (tests/menu)
scripts/docker.sh test aim        # the auto-target, the in-world walk, and the flick this branch shares
scripts/docker.sh test settings   # settings.txt
```
