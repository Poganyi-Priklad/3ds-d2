// Menu navigation: with a panel or an NPC's menu open, a flick of the
// circle pad (the auto-target's flick, auto_target.h's AimFlicks) moves the
// cursor one cell or one option up, down, left or right; a slow tilt still
// moves it freely (controls.cpp). Platform-neutral: the game's side
// (d2_menus.h) says which windows are open and where their cells are.
//
// The rules, for one flick from where the cursor was when the flick began:
//   - on a cell (or a panel): the nearest cell that way in any open window
//     (a cell beside the line counts twice its distance), one cell a flick
//     however large the item lying there;
//   - else, with an item on the cursor: to the middle of the screen's free
//     half (nothing open there), to drop it; a flick back towards the window
//     puts the cursor back on the cell it left;
//   - else round to the far side, over all the open windows, nearest the
//     same row or column;
//   - a list of options (an NPC's menu) only goes up and down, and keeps
//     to itself;
//   - off every window: the nearest cell that way.
#ifndef D2_GAME_MENU_NAV_H
#define D2_GAME_MENU_NAV_H

#include <cstdint>
#include <vector>

namespace game {

// A rectangle in game pixels; right and bottom are outside it.
struct NavRect {
    int left = 0, top = 0, right = 0, bottom = 0;
    bool contains(int x, int y) const { return x >= left && x < right && y >= top && y < bottom; }
    int centre_x() const { return (left + right) / 2; }
    int centre_y() const { return (top + bottom) / 2; }
    bool empty() const { return right <= left || bottom <= top; }
};

// A list's scroll buttons beside its grid of slots (D2Win's character
// screen: the heroes on the left, the scroll bar's column on the right).
// Flicks never scroll the list; they put the cursor on a button that
// would scroll it (an active one), and A clicks it, a row a press.
struct NavScroll {
    bool present = false;
    std::vector<NavRect> slots;   // the grid's cells (also in the window's cells)
    NavRect up, down;             // the scroll bar's buttons
    bool up_on = false;           // the list has rows above the top one shown
    bool down_on = false;         // ...and below the bottom one
    NavRect below;                // the cell under the column (Delete); empty: none
    bool any_on() const { return up_on || down_on; }
};

// One open window: the panel it covers and the places a flick stops at.
struct NavWindow {
    const char *name = "";
    NavRect area;
    std::vector<NavRect> cells;
    bool up_down_only = false;   // a list of options: left and right do nothing
    NavScroll scroll;            // a scrolling list's buttons (D2Win's character screen)
};

enum class NavDirection : uint8_t { kUp, kDown, kLeft, kRight };

// A flick's direction (screen y down) as one of the four.
NavDirection nav_direction(float dir_x, float dir_y);
const char *nav_direction_name(NavDirection dir);

// The game menu (Escape) or one of its submenus (Options, Sound, Video,
// Automap) as the game keeps it: a row a line, which one the game has lit,
// and the row that goes one menu back. The arrow keys move the game's own
// light (skipping titles and disabled rows), so a flick sends Up or Down
// and the cursor follows the light; Left and Right change a choice or a
// slider.
struct GameMenu {
    enum Kind : int8_t { kTitle = -1, kButton = 0, kChoice = 1, kSlider = 2 };
    struct Row {
        NavRect rect;
        int8_t kind = kButton;
    };
    std::vector<Row> rows;
    int lit = -1;               // the row the game has lit; -1: none
    int previous = -1;          // "Previous": one menu back; -1 in the top menu
    bool free_cursor = false;   // Configure Controls: no rows, the cursor moves freely
};

// The rows' places for a menu of `count` rows `step` pixels apart (its
// header's two numbers), the frame's middle shifted to (width, height):
// row i's middle is 199 + (i - (count - 1) / 2) * step pixels down at
// 640x480, 320 across; each row 300 pixels wide. kinds: a GameMenu::Kind a row.
void game_menu_rows(int count, int step, const int8_t *kinds, int width, int height, GameMenu &out);

// One of D2Win's controls (the screens out of the game) as d2_menus.h
// reads it: its type (1 a text field, 2 an image, 4 a text box, 5 a scroll
// bar, 6 a button, 7 a list, 12 an account list), its state (bit 0
// enabled, bit 2 shown), its box (y its bottom edge), whether it has a
// click handler, a text box's lines, and for an animated image (the class
// screen's heroes) its first frame's size and offset, which is where it
// takes clicks.
struct D2WinControl {
    uint32_t type = 0, state = 0;
    int x = 0, y = 0, width = 0, height = 0;
    bool on_click = false;
    int lines = 0;
    bool frame = false;
    int frame_width = 0, frame_height = 0, frame_x = 0, frame_y = 0;
    int scroll_last = 0, scroll_top = 0;   // a scroll bar's last top row and its top row now
};

// Where a flick may stop for this control: shown and enabled text fields,
// buttons and lists; images and text boxes with a click handler (a text
// box only with something in it: two lines or more). False for the rest
// (scroll bars, disabled or hidden controls, the background).
bool d2win_cell(const D2WinControl &control, int width, int height, NavRect &out);

// A screen's stops (d2win_cell for each control, in the list's order). A
// popup over the screen (the difficulty choice, a confirmation such as
// Delete Character's) adds its controls with state bit 8 set (0xD: shown,
// enabled, the popup's; the lane at 800x600); while one is up, only its
// controls are stops, the screen under it takes no clicks. popup: one is up.
void d2win_cells(const std::vector<D2WinControl> &controls, int width, int height, std::vector<NavRect> &out,
                 bool &popup);

// A scroll bar's buttons (measured on the lane at 800x600 by clicking each
// pixel): the up button the top 11 pixels of its box, the down button the
// bottom 9, both its width; a click on either moves the list one row
// (clamped at the ends, no repeat while held), a click between them jumps
// to the end.
NavRect scroll_up_button(const D2WinControl &bar);
NavRect scroll_down_button(const D2WinControl &bar);

// The scroll column of a screen's controls: its first shown scroll bar
// (type 5) with text-box slots (d2win_cell's type 4 cells) left of it; the
// buttons active while the list has rows that way (top row > 0, top row <
// last row). below: the nearest cell under the down button (its column
// first). False without a scroll bar or slots.
bool d2win_scroll(const std::vector<D2WinControl> &controls, const std::vector<NavRect> &cells, int width,
                  int height, NavScroll &out);

// A flick by a scroll column's rules, before the usual ones (MenuNav::step
// calls it): right from the grid's right column to the active button
// nearest in height; up and down round the column (up, down, the cell
// below), active buttons only; left from a button to the slot nearest in
// height. False: the usual rules (no active button, or not one of these).
bool scroll_step(const NavScroll &scroll, int x, int y, NavDirection dir, int &to_x, int &to_y, const char *&why);

// Whether a screen's controls are D2Win's main menu (Single Player, Exit...):
// its Single Player and Exit buttons where the lane read them (264 across,
// their bottom edges at 324 and 568, 272 wide; out of the game the screen
// is always 800x600). There Escape is the Exit button and quits the game,
// so B (Escape on the other screens) does nothing.
bool d2win_main_menu(const std::vector<D2WinControl> &controls);

// The game's control panel under the world, in game pixels (measured on
// the lane at 640x480 and 800x600): the bar across the bottom (48 pixels),
// the life and mana globes over its ends, the mini-panel's buttons over
// its middle. A click there is for the panel (a skill, the belt, run or
// walk), never for the auto-target.
bool control_panel_at(int x, int y, int width, int height);

// How far a resolution moves the 640x480 layouts, in game pixels: the
// left half's panels, the right half's, and what is centred on the screen
// (the game menu, the gold dialog).
struct PanelShift {
    int left_x = 0, left_y = 0, right_x = 0, right_y = 0, centre_x = 0, centre_y = 0;
};

// The gold dialog ("How much gold do you want to drop?", and its deposit,
// withdraw and trade forms): opened by the inventory's and the stash's gold
// buttons. The game's side (d2_menus.h) reads it; the amount is changed by
// typing into its field (the D-pad, controls.cpp), OK and Cancel are cells.
struct GoldDialog {
    enum Mode : int { kDrop = 1, kTrade = 2, kDeposit = 3, kWithdraw = 4 };
    int mode = 0;     // a Mode
    int amount = 0;   // what its field holds now
    int max = 0;      // the most it can take (the game's own, which it clamps typing to)
    NavRect ok, cancel;
};

// The amount a gold dialog's field shows, from its characters (UTF-16, as
// the game keeps them): the digits up to the first other character, 0 when
// it is empty. Saturates rather than overflows.
int gold_field_amount(const uint16_t *text, int length);

// The gold dialog as a window: its OK and Cancel buttons.
NavWindow gold_dialog_window(const GoldDialog &dialog);

// The D-pad in the gold dialog: Up and Down +1 and -1, Right and Left +100
// and -100 a press; held, the step repeats after kDelayMs, faster each time
// (kFirstMs, shrinking by a sixth a repeat down to kFastestMs), and grows
// tenfold after each kTenfoldMs of holding (x10, x100, x1000), so the
// largest amounts are a few seconds away.
class GoldRepeat {
public:
    static constexpr int kDelayMs = 400, kFirstMs = 150, kFastestMs = 40, kTenfoldMs = 2000, kMaxTenfolds = 3;
    // step: the held direction's step (0: none held); returns the change to
    // apply now (0: none).
    int update(int step, uint64_t now_ms);

private:
    int step_ = 0;
    uint64_t pressed_ms_ = 0, next_ms_ = 0;
    int interval_ms_ = kFirstMs;
};

// The gold buttons the frame draws at fixed spots of the panels, measured
// at 640x480 on the Linux lane and moved with the panel's half (shift): the
// stash's (left half, withdraw) and the inventory's (right half: deposit
// with the stash open, else drop or offer in a trade), which open the gold
// dialog.
NavRect stash_gold_button(const PanelShift &shift);
NavRect inventory_gold_button(const PanelShift &shift);

// The close buttons ("X" in a circle) under the inventory (right half) and
// under the stash or a shop (left half), measured the same way. The
// character screen's, the skill tree's, the quest log's and the waypoints'
// are in fixed_windows.
NavRect inventory_close_button(const PanelShift &shift);
NavRect stash_close_button(const PanelShift &shift);
// The Horadric Cube's and the mercenary's close buttons (the left half), where
// Game.exe draws them (the cube's at 275, the mercenary's at 272 from the
// panel's left, 65 and 63 above its bottom; a button 32 by 32): not
// measured on the lane (the test hero has neither).
NavRect cube_close_button(const PanelShift &shift);
NavRect mercenary_close_button(const PanelShift &shift);

// Where the open windows are: the game's side (d2_menus.h's
// game_menu_world), or made-up ones in the tests. ui: GameState::ui.
// Out of the game (D2Win's screens: the main menu, the character and class
// screens and the rest) the game's side gives one window, the screen, whose
// cells are its enabled controls.
class MenuWorld {
public:
    virtual ~MenuWorld() = default;
    virtual void windows(uint64_t ui, int width, int height, std::vector<NavWindow> &out) = 0;
    // The game menu while the UI table has it open (0x09); false when it
    // doesn't read (the fixed rows of windows() stand in).
    // The cursor at x, y is on the game's own interface over the world
    // (the control panel, the belt's open rows): A and B click there plainly.
    virtual bool on_interface(uint64_t ui, int x, int y, int width, int height)
    {
        (void)ui;
        return control_panel_at(x, y, width, height);
    }
    virtual bool game_menu(uint64_t ui, int width, int height, GameMenu &out)
    {
        (void)ui, (void)width, (void)height, (void)out;
        return false;
    }
    // Out of the game: the screen shown is the main menu (d2win_main_menu).
    virtual bool main_menu() { return false; }
    // The gold dialog while it is open (false: not open, or it doesn't read).
    virtual bool gold_dialog(uint64_t ui, int width, int height, GoldDialog &out)
    {
        (void)ui, (void)width, (void)height, (void)out;
        return false;
    }
};

class MenuNav {
public:
    struct Step {
        bool moved = false;
        int x = 0, y = 0;            // where the cursor goes
        const char *why = "";        // "cell", "window", "free half", "back", "round", "onto": the rule used
    };
    // One flick. windows: the open ones; x, y: the cursor at the flick's
    // start; holding: an item on the cursor; width, height: the frame.
    Step step(const std::vector<NavWindow> &windows, int x, int y, NavDirection dir, bool holding, int width,
              int height);
    // The windows closed: the cell a flick back would return to is forgotten.
    void forget() { left_cell_ = false; }

private:
    bool left_cell_ = false;   // the cursor went to the free half from this cell
    NavRect left_from_;
};

// The centred guess of PanelShift for all three ((width - 640) / 2,
// (height - 480) / 2); the game's side (d2_menus.h) reads the halves' from
// inventory.txt's rows.
PanelShift panel_shift(int width, int height);

// The windows whose places Diablo II 1.14d draws at fixed spots, measured
// at 640x480 on the Linux lane and moved
// by `shift` (checked at 800x600 too), by the UI table's entries
// (d2_state.h): the game menu (0x09), the character screen (0x02: its four
// "+" buttons while stat_points, and its close button), the skill tree
// (0x04), the quest log (0x0F), the waypoints (0x14).
// The quest log and the waypoints also get their act tabs ("I" to "V" along
// the top) for the acts reached, `acts` of them (1 to 5): the game draws a
// tab only for an act the hero has reached.
void fixed_windows(uint64_t ui, const PanelShift &shift, bool stat_points, std::vector<NavWindow> &out,
                   int acts = 1);

// The act tabs along the top of the quest log and the waypoints (the left
// half, 640x480): Game.exe draws them at x 0 (the waypoints' first at 5),
// 67, 129, 191 and 253 from the panel's left, 62 wide; the first measured
// at 3..65 by 3..28 on the lane.
NavRect act_tab(int act, const PanelShift &shift);

// A grid of columns x rows boxes filling `grid` (inventory.txt's grid).
void add_grid(NavWindow &window, const NavRect &grid, int columns, int rows);

// The weapon swap tabs of the hero's inventory: drawn as "I" and "II" over
// each weapon slot, but each strip is one button (a click anywhere on it
// swaps the weapons, as W does; checked on the lane at 640x480). One cell
// over each weapon slot (inventory.txt's right and left arm boxes): as wide
// as the slot, from 22 to 3 pixels above its top.
NavRect weapon_tab(const NavRect &weapon_slot);

// An NPC's menu as the frame shows it (d2_menus.h finds its box): the title
// 16 pixels under the box's top, then an option every 15 pixels; the box
// ends 20 pixels under the last. Its options are the cells (not the title).
bool npc_menu_window(const NavRect &box, NavWindow &out);

// An NPC's menu as the game keeps it (d2_menus.h reads it): its box (the
// border's inside) and its number of options, Cancel too. The first
// option's middle is 30 pixels under the box's top, then one every 15.
bool npc_menu_window(const NavRect &box, int options, NavWindow &out);

}  // namespace game

#endif
