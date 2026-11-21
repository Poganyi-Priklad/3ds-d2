# Controls (3DS game app)

The mapping lives in `src/game/controls.cpp`. It follows the ideas D2Vita proved on the Vita, adapted to the 3DS's buttons and two screens:
- an analog stick walks the character;
- touch is a touchpad for the cursor;
- the face and shoulder buttons are the mouse buttons and D2's hotkeys.

## Screens

| Screen | Shows |
| --- | --- |
| Top | The whole 800×600 frame stretched over all 400×240 (2:1 across, 2.5:1 down), each pixel averaging a 2×2 block. **Select** switches to a 1:1 400×240 window that follows the cursor, for reading text. |
| Bottom | A row of tabs (MAP, KEYS, ZOOM, INFO, AIM, and PANEL while a panel is open) over a 320×222 area. See [Bottom screen](#bottom-screen) below. Until the game shows its first frame, it shows the startup messages. |

## Buttons

| Input | Action |
| --- | --- |
| Circle pad | In menus and with a panel open: move the cursor; a flick moves it one cell, option or button, also on the screens before the game (see [Menu navigation](#menu-navigation)). In the game world: walk. The cursor orbits the character in the stick's direction, the left button is held while the pad is pushed, and a fuller push reaches further. The walk never acts on anything: its point keeps off NPCs, monsters, objects, items and level warps (cave entrances, stairs, passages: the walk never takes the hero to another level; standing by one, he can walk away from it but not onto it). Let go, and the hero stops where he stands (a one-frame click on the next subtile ahead of him, so he stops facing the way he walked, mostly one or two subtiles on; none beside a warp; "Walk stop" off in AIM > SETTINGS, or `aim_walk_stop=0`, leaves the click out). |
| C-stick (New 3DS) | Move the cursor |
| Touch (bottom screen) | Depends on the tab (see [Bottom screen](#bottom-screen)). On MAP and INFO, a touchpad: drag moves the cursor (one drag across the pad crosses the frame); a short tap clicks where the cursor is |
| A | Left mouse button (held). In the game world it aims first: see [Auto-target](#auto-target-a-and-b) |
| B | Right mouse button (held): cast the right skill. In the game world it aims first, like A. With a panel open in the game: Escape, the panel's back or close; in a submenu of the game menu, its Previous; before the game, Escape (back one screen; nothing on the main menu) (see [Menu navigation](#menu-navigation)) |
| X | Inventory (I) |
| Y | Show items on the ground (Alt, held). With a shop open: sells the item under the cursor. With the inventory open (and no other panel on the left): drops it on the ground. If the cursor isn't on an item, nothing happens. |
| L | Stand still (Shift, held) |
| D-pad | Belt potions 1–4 (up, right, down, left) |
| ZL (New 3DS) | Weapon swap (W) |
| ZR (New 3DS) | A layer key (below); alone it does nothing |
| Start | Escape: game menu, closes panels |
| Select | Top screen: whole frame ↔ 1:1 zoom (on release, when no D-pad button was pressed with it) |

**R is a layer key.** While R is held:

| R + | Action |
| --- | --- |
| A | Character screen (C) |
| B | Skill tree (T) |
| X | Automap (Tab) |
| Y | Quests (Q) |
| D-pad | Skill hotkeys F1–F4 (also ZR + A / B / Y / X) |
| Start | Enter: confirm, e.g. OK after typing a character name |
| Select | On-screen keyboard (it also opens by itself when the game focuses a text field, and closes by itself when that field loses the focus; while it is open the sticks and A still work, the keyboard keeps B, Y, Start, Select and the touch screen) |
| L + Select | Screenshot into the Camera album (see below) |

**ZR is a layer key too** (New 3DS). While ZR is held:

| ZR + | Action |
| --- | --- |
| A | Skill 1 (F1) |
| B | Skill 2 (F2) |
| Y | Skill 3 (F3) |
| X | Skill 4 (F4) |
| D-pad (held) | Picks up items with the auto-target: up valuables (unique, set, runes), right gold, down everything; left is kept free. See [Auto-target](#auto-target-a-and-b) |

L and ZL do as they always do with ZR held.

**Select is a layer key for the bottom screen.** While Select is held (without R), the D-pad does not use the belt:

| Select + | Action |
| --- | --- |
| D-pad right / left | Next / previous tab along the bar (PANEL included while a panel is open), wrapping around |
| D-pad up | MAP (the map, also from HELP) |
| D-pad down | Switches the map's size (x1 ↔ x1/2), showing MAP |

## Auto-target (A and B)

In the game world (no panel open, or one side panel open with the cursor on the other half; nothing on the cursor), A and B aim before they click (`src/game/auto_target.cpp`; only mouse input, no game code is called):

- The cursor jumps onto the enemy in the circle pad's direction (within a cone, or along the last walking direction while the pad is centred), 30 pixels above its feet. The button goes down once the game shows that enemy under the cursor (its health bar), a frame later. If the game never shows it, another one is tried, else it is a plain click there.
- While the button is held, the cursor follows the target, even out of the cone, and the circle pad only aims: the hero doesn't walk off. The target never changes by itself; a **flick** of the circle pad (a quick push out of the centre; a slow tilt is not one) switches between enemies:
  - a flick the way the hero is fighting (within 30° of the line from the hero to the target), light or strong: the next enemy further away along that line; after the furthest, back to the nearest;
  - a light flick another way: the enemy nearest the target in that direction from it (within 30°), to step sideways;
  - a strong flick another way: the hero turns to the nearest enemy in the button's cone round that direction;
  - nobody there: the target stays. The pad has to come back to the centre between two flicks: one flick, one step. Flicks don't switch away from an NPC, an object or an item.
- Hover first: with the circle pad centred, a press on a unit the game already shows under the cursor (an enemy, an NPC, an object, an item or an item's name) that the button aims at clicks exactly that one. The cursor is not moved, and the rules below don't replace it while the button is held; if it moves out from under the cursor, the cursor follows it. While the pad walks, the cursor is the walk's point, so the rules choose.
- Otherwise the cone (zone I) comes first; with no enemy in it, the nearest enemy within `aim_near` subtiles (a melee hit's reach) all round the hero (zone II, the circle). A monster in either comes before any object, NPC or item.
- The target is kept when you let go and press again, or switch between A and B. It is let go when it dies, leaves the screen, when the hero gets no closer to it for 1.5 s, or 2.5 s after the last attack.
- Nothing in the cone: a plain click where the cursor is, as before.
- With no enemy in the cone or the circle, plain A takes, in this order: an object (shrines, then chests and other containers, waypoints and portals, doors, levers and level warps (cave entrances, stairs, passages to the next area), barrels; used ones are skipped, exploding barrels never), an NPC (Cain, Tyrael), the nearest item on the ground (gold first with `aim_gold_first=1`). Held, A goes from one to the next. B aims only at enemies and NPCs. An enemy coming into the cone takes over from an NPC, an object or an item.
- **With the items shown** (Y held, or Items latched on the KEYS page): the game takes a click on an item's name for that item, before any unit under it, and an item's own figure is not clickable then. So the auto-target reads the names the game drew (their rectangles, Game.exe's table at `0x7C54A0`, while the UI flag `0x0D` is set): an item is clicked on its own name (that item, no other), and the cursor on an enemy, an object, an NPC or a corpse goes to the first of its heights, then a step to either side, that no name covers. Alt stays down all the time, and while it is held the original game reports no monster under the cursor (no name or life bar, a click only walks or picks up): enemies can't be confirmed, so the auto-target does not attack them. Alt is for highlighting and collecting items; turn it off to fight.
- In town it aims at the NPCs (to talk to) first, then (A) objects and items. The mercenary, summons and other players are never aimed at.
- When the right button's skill is cast on a corpse (Raise Skeleton, Corpse Explosion, Revive, Find Item...), B aims at corpses by itself. The cursor goes just below the corpse's feet (where the game shows a corpse under the cursor); once the skill has been cast on a corpse (the hero's casting mode), it is skipped for 10 minutes and B goes on to the next one.
- **ZR + D-pad, held**, picks up the items within A's range all around the hero, one after another: up unique and set items, then runes; right gold; down everything (unique and set, runes, gold, the rest; gold first with `aim_gold_first=1`). An item the hero stands next to and still doesn't pick up (no room in the inventory) is skipped for 30 s. With nothing to pick up, nothing is clicked.
- L (stand still), R+A/B and ZR+A/B are as before: no aiming.
- With the cursor on the game's interface (the bottom bar with its skill buttons, belt and mini-panel, the globes, an open belt), A and B are a plain click there: no aiming, nothing is walked to. game.log says `aim: A on the game's interface at x,y: a plain click`.

Each button has its own mode: `enemies`, `corpses` (for corpse skills such as Raise Skeleton, Corpse Explosion, Find Item) or `off`. The settings are lines in `sdmc:/3ds/3ds-d2/settings.txt`, written there at the first start; a change made there applies at the next start. The bottom screen's AIM tab changes most of them at once (and saves them):

| Key | Default | Meaning |
| --- | --- | --- |
| `aim` | `1` | 0 turns the auto-target off: A and B click where the cursor is |
| `aim_a_mode`, `aim_b_mode` | `enemies` | `enemies`, `corpses` or `off` |
| `aim_a_cone`, `aim_b_cone` | `45` | degrees each side of the circle pad's direction |
| `aim_a_range`, `aim_b_range` | `20` | subtiles from the hero (a subtile is a third of a floor tile) |
| `aim_near` | `4` | the circle (zone II): with no enemy in the cone, the nearest enemy this close (subtiles), all round the hero |
| `aim_near_cone` | `90` | no longer used (the circle is all round); kept for older files |
| `aim_flick_cone` | `30` | degrees each side of a flick's direction |
| `aim_flick_ms` | `150` | a flick gets from the centre to `aim_flick_light` within this time |
| `aim_flick_light` | `40` | percent of the full tilt a flick reaches |
| `aim_flick_strong` | `80` | ...and a strong flick (the hero turns) |
| `aim_objects` | `1` | plain A uses objects with no enemy in the cone |
| `aim_loot` | `1` | plain A picks up the nearest item with nothing else in the cone |
| `aim_gold_first` | `0` | gold before other items (plain A and ZR + down) |
| `aim_lost_ms` | `1500` | the hero hasn't attacked or got closer for this long: the target is given up |
| `aim_forget_ms` | `2500` | this long after the last attack the target is forgotten |
| `aim_height` | `30` | pixels above the target's feet for the cursor |
| `aim_confirm` | `4` | frames to wait for the game to show the target under the cursor |
| `aim_panel_walk_ms` | `280` | with one side panel open, the circle pad held out this long walks the hero (0: never) |
| `aim_walk_stop` | `1` | the circle pad let go after a walk: a one-frame click on the next subtile ahead of the hero stops him there (AIM > SETTINGS "Walk stop"); 0: no click, he walks on to the walk's last point (up to a few subtiles further) |

An older file's `aim_turn` line is ignored and dropped when the file is written again; its other values stay, except an `aim_a_range=10` (the default then), which becomes the new default 20.

game.log's `aim:` lines say what it chose and why it let go, and every 30 s of use a `aim: stats:` line counts it all.

## Menu navigation

In the game with a panel or an NPC's menu open, a **flick** of the circle pad (the auto-target's flick: `aim_flick_ms`, `aim_flick_light`) moves the cursor one cell or option up, down, left or right (`src/game/menu_nav.cpp`). A slow tilt moves the cursor freely, as before. After a flick the cursor stays on its cell until the pad is back in the centre.

- One cell a flick, also over an item that covers several cells.
- The inventory's weapon swap tabs ("I" and "II" over each weapon) are one cell per weapon, over it: a flick up from a weapon reaches its tab, and A there swaps the weapons (either tab does, as W does).
- At a window's edge, a flick goes into the open window next to it, at the same relative place: the inventory and the stash, a shop or the Horadric Cube.
- With an item on the cursor, a flick past a window's edge puts the cursor in the middle of the free half of the screen (to drop it there with A). A flick back towards the window puts it back on the cell it left.
- Otherwise a flick past the edge comes round to the far side of the window, in the same row or column.
- An NPC's menu (and a menu under it, such as Talk's) only goes up and down; off the menu, a flick goes onto its nearest option.
- With **one** side panel open (the inventory or the skill tree on the right; the character screen, the quest log, the waypoints or the mercenary's screen on the left), the circle pad held out for `aim_panel_walk_ms` (280 ms; `0`: never) starts the same walk as with no panel (the auto-target's: it keeps off units and warps, and stops when the pad is let go). The cursor stays on the free half of the screen while walking, and the walk orbits the hero where the game draws him (the middle of the free half, read from the game's camera). With the cursor on the free half, A and B aim as with no panel, from the hero's real place, at targets on that half only. A shorter flick still moves between cells. With panels open on both sides (or the stash, a shop, a trade, the cube, an NPC's menu, the game menu), holding the pad only moves the cursor, as before. game.log's start says the value (`a panel on one side: the stick held N ms walks`), and `walk: the stick held N ms with a panel open on the left/right: walking` when it starts.
- A is the left button as before. **B is Escape**: it closes the panel, goes back from a menu under an NPC's menu, or closes the NPC's menu or a talk. R + B and ZR + B are as before.

The game menu (Start) and its submenus (Options, Sound, Video, Automap) use the game's own keyboard navigation: a push of the circle pad up or down (half way out; back to the centre between two) is the Up or Down arrow key, which moves the game's light past titles and disabled rows and round at the ends, and the cursor then goes onto the lit row, so A takes it. Left and right are the Left and Right keys on a choice or a slider (they do nothing on a button). The circle pad doesn't move the cursor there (the C-stick and touch still do). When a menu opens, the cursor goes onto its lit row. **B in a submenu is its Previous** (one menu back); in the top menu it is Escape (the menu closes). Configure Controls keeps a free cursor: the circle pad moves it, B is Escape (back to Options). Before the game, B is Escape too: back from the character select to the main menu, from the class screen to the character select, and so on; on the main menu itself it does nothing (Escape there would ask to quit).

Before the game (D2Win's screens: the main menu, the character and class screens, the difficulty choice, the multiplayer screens), a flick moves the cursor onto the next enabled button, text field, list, character slot or hero that way; disabled and hidden controls are skipped. The heroes of the class screen are where the game takes their clicks (their first frame, standing). It reads the screen's own control list (`src/game/d2_menus.cpp`).

The character screen with more heroes than fit (four rows of two): flicks never scroll the list. A flick right from the right column of heroes goes onto the scroll bar's up or down button, whichever is nearer in height, but only one that scrolls (up while there are rows above, down while there are rows below; neither for eight heroes or fewer, and a flick right is as before). On the bar, up and down go round its column, up button, down button, Delete, skipping a button that doesn't scroll; right from Delete is OK as before; left from a button goes back to the hero nearest in height. A on a button is a click: the list moves one row (two heroes) a press, holding doesn't repeat. When the list reaches its end the button stops scrolling, and once A is up the cursor goes back to the hero nearest in height. The buttons and the list's position are the scroll bar's own (its top 11 and bottom 9 pixels; its last top row at +0x48, its top row at +0x4C), measured on the lane.

The windows: the inventory with its equipment slots, a shop, the stash, the Horadric Cube, a trade and the mercenary's slots, from the game's `inventory.txt`; the belt's row of four under the screen (with the inventory, a shop, the stash, the cube or a trade), from the game's `belts.txt`, and the worn belt's upper rows too while the game shows them (a belt of more than one row, open while the cursor is on it: a flick up from the row of four goes up the belt, a flick off it closes it; open, its boxes take the place of the inventory cells they cover); an NPC's menu, from the game's memory (found in the frame should that not read); the game menu, the character screen (its four "+" buttons while there are stat points to spend, and its close button), the skill tree with its tabs, the quest log and the waypoints, at the places the game draws them at 640×480, moved at 800×600 as far as `inventory.txt` moves the panels (`src/game/d2_menus.cpp`). With none of them under the cursor and none that way, a flick leaves the cursor where it was. game.log's `menu:` lines say where each flick went.

## Bottom screen

`src/game/second_screen.cpp` draws the bottom screen and handles its touches. The tab bar is the top 18 pixels. Tap a tab to show it. Tap the tab that is already shown to use its second function. The chosen tab, the map size, the map's marker refresh and whether MAP shows HELP are saved in `sdmc:/3ds/3ds-d2/settings.txt`.

| Tab | Shows | Touch in the area |
| --- | --- | --- |
| MAP | The automap, centred on the player and scrolling with them, at 1:1 or half size. Three buttons under the map: how often the crosses and names of others are refreshed while you move (mark .25s, .5s, 1s, 2s, 5s), the size (x1, x1/2; tapping MAP again also switches it), and **?** in the corner. The game's own map on the top screen (Tab) is separate and stays off unless you turn it on. | Touchpad, except the buttons |
| HELP | Shown in the MAP tab by **?**: the controls, one line per button or combination. The tab reads HELP and the corner button MAP; tapping either goes back to the map. **?** works before the game too, under "The map shows here in the game." On the first start (no `settings.txt` yet) the tab opens on HELP. | Touchpad, except the MAP button |
| KEYS | 20 buttons: Skill 1–8 (F1–F8), Character, Inventory, Skills, Quests, Party, Merc, Map (Tab), Run/Walk, Swap (W), Items (Alt, latched: tap on, tap off), Chat (Enter), Close all (Space) | Presses the button: the key is held while you touch it |
| ZOOM | The frame at 1:1 around the cursor: readable text and item details | Points at the game (below) |
| INFO | The diagnostics: game time, fps, worst frame gap, draw time per frame, guest heap and VirtualAlloc use against their limits (with peaks), host heap, linear and application memory free, the sprite cache, the JIT's memory (in the AOT build, which has none: the compiled functions, how many differ from the image at the start's check, the natives bound, and the most a game thread has used of its host stack), files and bytes read, the last 10 s's hitches (frames over 100 and 250 ms, the longest; frames not drawn and sound starved; what held up the drawing thread: waiting, ready while another game thread ran, host work on its core, and the last heartbeat's length), then the latest runtime messages. **report** at the bottom (lit as **will report**) asks for a [problem report](../README.md#problem-reports) of this session when the game ends; tap it again to take that back. **profiler run** opens a checklist of what to measure: sprite trace, guest code profile (about 12% slower while on), file-read trace, SD card benchmark (a second's pause), frame timing, sound clipping (the mixer's peaks and the limiter, as game.log's `sondiag` lines). Its last row, **log writes**, steps through batched, every second and at once on each tap (how soon game.log and the traces reach the SD card: at once is for chasing a crash or a freeze), a run on or not ([hardware-test.md](hardware-test.md)). Tap the options, then **start the run**; the button stays lit (and the first line shows PROF) until **stop the run**. The ticks are remembered; they cannot change during a run. A session with a run is offered as a problem report when the game ends | Touchpad, except the buttons (and the checklist while it is open) |
| AIM | The auto-target, on two pages chosen by the buttons at the bottom. **SETTINGS**: a row per setting (Auto-target, A aims at, A cone, A range, B aims at, B cone, B range, Flick cone, Flick time, Light flick, Strong flick, Objects, Loot, Gold first, Walk stop) with its value between **-** and **+**; a tap changes it at once, and it is saved like the other settings. **FAQ**: how the auto-target works | SETTINGS: the **-** and **+** buttons. FAQ: dragging scrolls it |
| PANEL | The open panel's half of the frame at 1:1. At 640×480 D2's panels are 320 pixels wide, the width of the screen. It scrolls to follow the cursor, and a bar on the right shows where you are. With panels on both sides (stash and inventory, shop and inventory), it shows the side the cursor is on. Tap PANEL again to move the cursor to the same spot on the other side. | Points at the game (below) |

PANEL appears when a panel opens and is shown by itself. Choosing another tab hides it until another panel opens. When the panels close, the bottom screen goes back to your tab.

**Pointing** (ZOOM and PANEL): the cursor goes to the touched spot and follows the finger. A short tap is a left click there; it picks up or drops an item, buys, or walks. Holding still for half a second is a right click; it drinks or uses an item, or casts. Dragging near the edge of the area scrolls it.

The panels are found from the UI table at `0x7A27C0` (see [Telling menus from the game world](#telling-menus-from-the-game-world)): inventory and skill tree on the right; character sheet, shop, socketing, quest log, waypoints, party, trade, stash, cube and mercenary on the left; NPC dialogue in the centre. The game menu (Start) is not offered: it is readable on the top screen as it is.

**The automap** (`src/game/automap.cpp`): the bottom map is drawn in two parts.

- **Tiles** (floors, walls, objects, extras): drawn by 3ds-d2 itself from the game's map data, every time the player moves a pixel, at the player's exact position, so the map scrolls smoothly. The level's map layer (`0x7A5164`) holds a binary tree of cells per kind, ordered by y; each cell names a frame of the map's DC6 cell file (`0x7A5168`) and a position in map pixels (world pixels / 10). The frames are decoded once and kept as runs of pixels. Drawn the way the game draws them (bottom-left corner at the cell's position plus the frame's offsets, opaque), they matched every pixel of a capture of the game's own map in the lane. The game draws the ground as single dots with black between; at 1:1 that turned each pixel on and off as the map moved, so the black around each lone dot takes its colour and the ground shows solid, as at half size. Drawing takes about 2 ms in Azahar, where the game's own map took about 16 ms, 15 of them for the tiles.
- **Units** (the crosses and names of NPCs and other players, waypoint labels): captured from the game. Game.exe's UI pass draws the map with `0x45AD60` when `ui[0x0A]` (Tab) is set. That call site (`0x456F93`) now calls a small stub of guest code in the zero slack after `.text` (`0x6CB5B0`). When a capture is due, the stub runs only the game's map setup (`0x457520`) and units pass (`0x45AC90`, about 1 ms), with the map's offsets and unit bounds set so it draws around the player into a region at the frame's top-left corner, the size of the bottom map plus a margin. That part of the frame is copied aside and cleared to black first; every pixel drawn is kept, and the frame gets its copy back, so the top screen is unaffected. The player's own cross is left out (a filter in front of the per-unit function `0x45A860` skips the player while capturing) and drawn by 3ds-d2 at the centre, in the game's colour for it (`0x7A51B0`). Between captures the unit pixels move with the map. Captures come at the marker refresh set under the map while the player moves, every two seconds standing still (attacking in place included), and at once when MAP is shown, its size changes or the player nears the edge of the last capture. The stub calls the game's functions as ordinary guest code because the map's first draw in a level waits for the game's other threads, which a native `guest_call` cannot do.

On the Linux lane, `M4_BOTTOM=map|help|keys|zoom|info|aim` picks the starting tab. The script event `<ms> touch <x> <y> [ms]` touches the bottom screen, and each `shot` also saves the bottom screen as `b<ms>.png`.

## Screenshots

**L + R + Select** saves what the game is showing into the console's Camera album, the way L + R does in Animal Crossing: New Leaf. Hold L and R, then press Select. `src/app/ctr_screenshot.cpp` writes the file.

- **Where:** `sdmc:/DCIM/<NNN>NIN03/HNI_<NNNN>.JPG`, the folder and names the camera uses. The number is one past the highest in the newest folder. A folder holding 100 pictures moves on to the next one.
- **What:** the game's own frame, not the 400×240 screen. In game that is 640×480, the camera's own photo size. Larger frames, such as the 800×600 menus, are scaled to fit 640×480. Cinematics keep their aspect ratio. The zoom setting makes no difference.
- **File:** a baseline JPEG with an EXIF block, holding the date and a 160×120 thumbnail.
- **The bottom screen too:** each screenshot is a pair. The bottom screen, as shown, goes to `sdmc:/3ds/3ds-d2/screenshots/<date>_<time>_bot.jpg` (320×240), not the album. The game lists its newest 16 pairs in `screenshots/pairs.txt`. When a pair drops off the list, its bottom picture is deleted; the album's picture stays.
- **While saving:** the top screen holds the frame for a moment. The bottom screen then shows the path, or why saving failed.

Rosalina's screenshot (Luma3DS menu, L + Down + Select, then *Take screenshot*) still works too. It saves both screens as BMPs in `/luma/screenshots/`, and it works while the game is suspended in the HOME Menu.

Either kind can go with a [problem report](../README.md#problem-reports): **L** on the report's screen chooses up to 4 pairs.

## Problem reports

The top screen shows [problem reports](../README.md#problem-reports) before the game starts (when reports wait to be sent, or with Select held while 3ds-d2 starts) and after it ends (when a report was made for that session).

| Screen | Input | Action |
| --- | --- | --- |
| List | Up / Down, L / R | Choose a report; a page up or down |
| List | A | Open the report |
| List | X | Send every report waiting, oldest first; stops at the first that fails (no Wi-Fi), the rest wait |
| List | Start | Go on (to the game, or out of the app after a session) |
| Report | A | Send it; then its Sentry ID, and A to go on |
| Report | Y | Write or change the description (the system keyboard; the game's own if that does not start) |
| Report | X | Mark it as not to be sent |
| Report | Select, twice | Delete the report and its files |
| Report | B | Back to the list |

## Cinematics

A, B, Start (Escape), R + Start (Enter) or a tap anywhere on the bottom screen skips the video playing, as a key or click does on the PC. The press is held back until it is released, so it does not also act on the next screen. [cinematics.md](cinematics.md) explains the player.

While a video plays, the bottom screen shows its own panel instead of the tabs: the video's title, how far it has played, and how to skip it. The tab shown before comes back with the next screen.

## How input reaches the game

Each poll (every ≥ 16 ms, from the game's own message pump) becomes the window messages Windows would send:
- `WM_MOUSEMOVE`, with consecutive moves merged;
- `WM_LBUTTONDOWN`/`UP` and `WM_RBUTTONDOWN`/`UP`, with lParam = `(y << 16) | x` in game pixels;
- `WM_KEYDOWN`/`UP` with set-1 scan codes. Alt is sent as `WM_SYSKEY*`, and Enter, Escape and Backspace also produce `WM_CHAR`;
- `WM_CHAR` for typed text.

`GetKeyState` and `GetCursorPos` change when the event is queued. `DispatchMessageA` and `SendMessageA` tail-call the window procedure (`rt::tail_call`), so it runs as ordinary guest code that may wait on the game's other threads. When it ran as a nested call, menu clicks were silently lost.

Two timing rules the game needs:
- **Taps:** a tap holds the button for 120 ms. Down and up inside one 40 ms game frame do not count as a click.
- **Walking:** the cursor moves first and the button goes down on the next poll. The game resolves a click against what the cursor hovered in the previous frame.
- **Presses shorter than a frame** (issue #118): the game pumps messages about once a frame (40–80 ms in play, 200 ms at 5 fps), so a press and release between two pumps used to be lost. On a console with a second core, a thread reads the pad every 4 ms (`src/game/pad_history.h`). When a button or the touch screen changed twice between two pumps, the pump replays each change in order, each at its own time. Otherwise it applies the latest reading alone, as before. A mouse button that goes down and up within one pump's replay is released at the next pump, so the game sees a click. Without a second core, the pump reads the pad itself, as before. `scripts/docker.sh test pad` runs D2Vita's bench (`bc968fb`): presses of 80 and 150 ms at 25, 10 and 5 fps. 80 ms presses at 5 fps went from 4 of 12 kept to 12 of 12.

On the Linux lane, `M4_SCRIPT` drives the same `game::Controls` with scripted pad events (`tests/m4/game_main.cpp`). The scripted run below goes from the main menu, through a new Barbarian, into the Rogue Encampment:

`20000 tap 400 307; 26000 tap 400 290; 32000 text Tester; 34000 press R; 34100 press START; 34300 release START; 34400 release R`

The events are listed in `src/game/autoplay.h`. Besides taps, buttons and text, a script can wait for a state (`ingame`, `menus`, `textfield`, `npcmenu`, `menuclosed`, `act1` to `act5`). `tapunit <npc>` clicks an NPC where it stands, read from the game's units (`warriv`, or a monstats class number), and keeps clicking until its menu opens. `tapmenu <line>` then picks a line of that menu, and `units` logs the units near the player. Every tap puts the cursor on its spot 300 ms before it presses, so a slow frame rate still sees the spot hovered. `skipvideos` presses A as each cinematic starts, which skips it. `tests/m4/scripts/act2-east.txt` uses them to go from Act I to Act II.

## On-screen keyboard

When the game focuses a text field, such as the hero name box, a keyboard takes over the bottom screen, under the diagnostics. R + Select opens it at any time. `src/game/osk.cpp` holds the layout and behaviour.

- **Typing:** tap the keys, or click them with the mouse in Azahar. Characters go to the game as `WM_CHAR`.
- **Keys:** `<-` deletes, `Shift` capitalises the next letter, `OK` sends Enter and closes, `Close` just closes.
- **Buttons while it is open:** B deletes, Y is shift, Start is OK, Select closes. The button that closed the keyboard is held back from the game until it is released.
- **Field detection:** D2Win's focused-control pointer at `0x7D55C8`; type 1 at the control is an edit box. The field's text is never read: on Battle.net it can be a password.

## Telling menus from the game world

`src/game/d2_state.cpp` reads two locations in Game.exe 1.14d's memory on every input poll (addresses from D2Vita's reverse engineering, used as facts):
- **In game:** the player unit pointer at `0x7A6A70` is non-null.
- **Panel open:** one of the panel entries in the UI table at `0x7A27C0` is non-zero (inventory, character sheet, skill tree, NPC dialogue, game menu, shop, quest log, waypoint, party, stash, and others). Every non-zero entry below `0x28` is logged with each change, for example `game state: in game, panel open (ui 800208003)`.

Before trusting either, it checks that the dword at `0x45B450` is `0x7A5204` (an instruction's operand inside a function, not its first bytes, which a native or the compiled code's hook overwrites with a jump). On any other executable the state stays "menus", so the circle pad only moves the cursor. Transitions are logged, for example `game state: in game, panel open`.

## Azahar's default keyboard mapping

| 3DS | Key | | 3DS | Key |
| --- | --- | --- | --- | --- |
| A | A | | Circle pad | arrow keys |
| B | S | | C-stick | I J K L |
| X | Z | | D-pad | T F G H |
| Y | X | | Start | M |
| L | Q | | Select | N |
| R | W | | ZL / ZR | 1 / 2 |
| Touch | mouse on the bottom screen | | | |

