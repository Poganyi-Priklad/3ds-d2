# Fork: PAD (a bottom-screen window in the game's own art)

This branch builds on the [menu navigation](fork-menu-nav.md) branch. PAD is a
new bottom-screen tab that turns the whole lower screen into a window drawn in
Diablo II's own style: its background, borders, scroll column, close button and
fonts. It is here purely to give the port a high-end look; it doesn't change
how the game plays. This branch also adds the **SCREEN** page, which lets you
turn off each automatic bottom-screen switch; without this branch, menu
navigation keeps every switch on, as the original port did.

Nothing of the game is stored in this repository. The art is read at run time
from **your own** MPQ files, the first time the tab opens, in your game's
language (with LATIN as the fallback for the fonts).

## How to use it

Tap **PAD** on the bottom screen's tab bar. The window covers the whole bottom
screen (the tabs are hidden) and follows what is under the cursor:

- an **NPC**: its name in gold;
- an **object** (a waypoint, a shrine, a chest...): its name in white;
- a **monster**: its name in red with its life bar.

In an NPC's dialog, PAD shows the NPC's name and, as a talk scrolls on the top
screen, its text line by line at the game's own pace (the talk's scroll, which
follows the speech). The text stays after the talk ends, and each later talk in
the same dialog is added below the earlier ones. It all clears when the dialog
closes, a shop or trade opens, or a fight takes over.

| Touch | Action |
| --- | --- |
| Anywhere in the window | The touchpad, as on the other touchpad tabs (drag moves the cursor, a short tap clicks) |
| The scroll column's arrows, bar, or a drag on the bar | Scrolls the text; while you have not scrolled up, it follows the newest line |
| The close button (bottom right) | Back to the tab shown before |

While PAD is shown, panels, NPC menus and the panel side never switch the
bottom screen away from it.

## Bottom screen switches (SCREEN page)

The original port changes the bottom screen by itself in a few cases (a panel
opening shows the PANEL tab, a text field opens the keyboard, and so on). This
branch adds a page that lets you turn each of those off: **INFO -> options ->
screen switches**. Each box is applied at once and saved in `settings.txt` as
`screenauto=` (a `-name` entry for one that is off). All are **on** by
default, so nothing changes unless you choose it:

| settings.txt name | On (default) | Off |
| --- | --- | --- |
| `panels` | A panel opening shows PANEL | PANEL is only offered on the tab bar |
| `npcmenu` | An NPC's menu or talk shows PANEL, centred | The tab shown stays |
| `panelside` | With panels on both halves, PANEL follows the cursor's half | It stays on its half until PANEL is tapped |
| `keyboard` | A focused text field opens the on-screen keyboard | R + Select opens it |
| `video` | A cinematic shows its own panel | The tab stays |

The full map is also in [controls.md](controls.md#bottom-screen).

## The `D2_DEFAULT_NO_PANEL_AUTO` build switch

I prefer to use the bottom screen permanently as a touchpad. When I zoom in on
the top screen, the automatic bottom-screen switching (PANEL popping up by
itself when a panel or an NPC's menu opens, and following the cursor between
the two halves) sometimes gets in the way. So when I made PAD, I moved those
behaviours onto the SCREEN page (above) and added a build switch that starts
them off:

```
D2_DEFAULT_NO_PANEL_AUTO=1 scripts/docker.sh package-cia --full <game-dir>
```

A build made this way starts with `panels`, `npcmenu` and `panelside` **off**,
and `keyboard` and `video` **on**, as long as `settings.txt` has no
`screenauto=` line yet. Once you change a box on the SCREEN page, your choice is
saved and wins in either kind of build. Such a build gets `-nopanelauto` in its
build ID, so it is never mistaken for a regular one.

**The repository's default stays all on**: without the switch, the port
behaves exactly as before. The switch only changes the starting defaults; it
removes nothing.

## How it works

PAD, like the rest of this fork, only reads the game's memory and never writes
it or calls game code.

| File | What it does |
| --- | --- |
| `src/game/mpq.h`, `.cpp`, `pklib_explode.*` | Reads a whole file out of an MPQ archive (sector table, encrypted files, PKWARE-compressed and imploded sectors), so the art can come from the player's own MPQs |
| `src/game/pad_art.h`, `.cpp` | Decodes DC6 sprites and the game's fonts, and composes the window's backgrounds and sprites once; draws a frame of the window |
| `src/game/pad_story.h`, `.cpp` | The window's state: the dialog's talks, how many lines are revealed, wrapping, scrolling, the hovered unit; tested without the game |
| `src/game/pad_game.h`, `.cpp` | Reads the talk, the NPC menu's title and the hovered unit from Diablo II 1.14d's memory each poll |
| `src/game/d2_units.cpp` | The unit under the cursor: its kind (NPC, enemy, object), its name and a monster's life |
| `src/game/second_screen.cpp` | The PAD tab, its touches and its drawing |
| `src/game/screen_auto.h`, `second_screen.cpp` | The SCREEN page's switches and the `screenauto=` setting |
| `src/game/screen_auto.h`, `tests/cpu/ctr/Makefile`, `scripts/docker.sh` | The `D2_DEFAULT_NO_PANEL_AUTO` defaults and how the build passes them on |

## Tests

```
scripts/docker.sh test settings             # settings.txt, including screenauto=
scripts/docker.sh test padwin               # the window's story, tab and art decoders, and both SCREEN defaults
scripts/docker.sh test padwin <game-dir>    # ...and the art built from your own MPQs (read-only, nothing written)
```
