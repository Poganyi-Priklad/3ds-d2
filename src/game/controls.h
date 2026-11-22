// 3ds-d2's control scheme for Diablo II: turns the 3DS's buttons, circle pad,
// C-stick and touch screen into the mouse and keyboard input the game
// expects (through rt::input_*). Platform-neutral: the 3DS app fills a Pad
// from HID each poll; the Linux lane can replay scripted pads.
//
// Default mapping (docs/controls.md):
//   Circle pad   in the game world: walk (the cursor orbits the character and
//                the left button is held; the point keeps off units, so a
//                walk never talks, opens, picks up or attacks; let go, the
//                hero stops where he is); in menus and open panels: move the
//                cursor; with a panel open on one side only, held out
//                (aim_panel_walk_ms) it walks on the free half;
//                a flick moves it one cell or option (menu_nav.h):
//                in the game with a panel open, and on the screens out of
//                the game (their buttons, fields, heroes...). In the game
//                menu (Escape) a tilt up or down is the arrow key and the
//                cursor follows the game's lit row; left or right changes a
//                choice or slider
//   C-stick      move the cursor (New 3DS)
//   Touch        a touchpad: drag moves the cursor, a short tap is a left click
//                at the cursor
//   A / B        left / right mouse button (held); B with a panel open in
//                the game (menu navigation on): Escape, the panel's back or
//                close; in a game menu's submenu, its "Previous" (one back).
//                In the game world, with
//                the auto-target on (auto_target.h): the cursor first goes
//                onto the enemy, NPC or corpse the circle pad points at, and
//                the button goes down once the game shows it under the cursor;
//                on the control panel (skills, belt, globes): a plain click
//   X            inventory (I)        Y   show items (Alt, held); with the
//                inventory open, sells (a shop open) or drops the item under
//                the cursor (issue #99)
//   L            stand still (Shift, held)
//   R            layer: R+A character (C), R+B skill tree (T), R+X automap (Tab),
//                R+Y quests (Q), R+D-pad skill hotkeys F1-F4, R+Start Enter,
//                R+Select on-screen keyboard (opens by itself on a text field)
//   L+R+Select   screenshot into the Camera album
//   D-pad        belt potions 1-4
//   ZL           weapon swap (W)                            (New 3DS)
//   ZR           layer: ZR+A/B/Y/X skill hotkeys F1-F4 (New 3DS); ZR+D-pad
//                held, with the auto-target: pick up items (Up valuables,
//                Right gold, Down everything)
//   Start        Escape (game menu)
//   Select       top screen: whole frame <-> 1:1 zoom around the cursor
#ifndef D2_GAME_CONTROLS_H
#define D2_GAME_CONTROLS_H

#include "game/auto_target.h"
#include "game/menu_nav.h"

#include <cstdint>

namespace game {

enum Button : uint32_t {
    kA = 1u << 0, kB = 1u << 1, kX = 1u << 2, kY = 1u << 3,
    kL = 1u << 4, kR = 1u << 5, kZL = 1u << 6, kZR = 1u << 7,
    kStart = 1u << 8, kSelect = 1u << 9,
    kUp = 1u << 10, kDown = 1u << 11, kLeft = 1u << 12, kRight = 1u << 13,
};

struct Pad {
    uint32_t held = 0;          // Button bits
    int circle_x = 0, circle_y = 0;   // -156..156, y up (libctru's circle pad range)
    int cstick_x = 0, cstick_y = 0;   // -156..156, y up
    bool touching = false;
    int touch_x = 0, touch_y = 0;     // touch-screen pixels (320x240)
    // A scripted tap (autoplay "tap"): the touchpad's, whatever the bottom
    // screen shows.
    bool touchpad = false;
    // From the bottom screen (second_screen.h), which takes the touches it
    // acts on out of `touching`: a spot the cursor goes to, in game pixels,
    // with the mouse buttons held there; and keys its buttons hold down.
    bool pointing = false;
    int point_x = 0, point_y = 0;
    bool point_left = false, point_right = false;
    uint8_t keys[4] = {};             // virtual keys, 0 = none
    // A script's tap (autoplay.h): A clicks where the script put the
    // cursor; the auto-target leaves it alone.
    bool aim_off = false;
};

// While the on-screen keyboard is open (osk.h), the pad the game gets: the
// sticks and every button the keyboard doesn't use (it has B backspace, Y
// shift, Start Enter, Select close), so the cursor, a flick's step and A
// still work; no touch (the touch screen is the keyboard's).
inline Pad keyboard_pass_through(const Pad &pad)
{
    Pad out;
    out.held = pad.held & ~(kB | kY | kStart | kSelect);
    out.circle_x = pad.circle_x;
    out.circle_y = pad.circle_y;
    out.cstick_x = pad.cstick_x;
    out.cstick_y = pad.cstick_y;
    return out;
}

// The panels open (GameState::ui): -1 on the left half only, 1 on the
// right half only, 0 on both or any other kind (controls.cpp).
int one_side_panel(uint64_t ui);

class Controls {
public:
    // Game frame size (cursor bounds, walk radius scale).
    void set_frame(int width, int height);
    // One poll. now_ms: the guest clock (tap timing).
    void update(const Pad &pad, uint64_t now_ms);
    // A message pump starts (issue #118: the app replays the pad's readings
    // between two pumps). A mouse button that went down during this pump's
    // updates and comes up in the same pump stays down until the next one:
    // the game reads down and up in one pump as no click. Never called (the
    // Linux lane): buttons follow the pad, as before.
    void begin_pump()
    {
        pump_++;
        pumps_ = true;
    }

    // What the game is showing (from game::GameState): the circle pad walks
    // only in the game world with no panel open. ui: GameState::ui (the
    // panels open); holding: an item is on the cursor (d2_units.h).
    void set_context(bool in_game, bool panel_open, uint64_t ui = 0, bool holding = false);
    // The auto-target on plain A and B (auto_target.h): its settings, and
    // where it reads the game (d2_units.h's game_aim_world). No world (the
    // default, and the pad test): A and B click where the cursor is.
    void set_auto_target(const AimSettings &settings, AimWorld *world);
    const AutoTarget &auto_target() const { return aim_; }
    // Menu navigation (menu_nav.h): with a panel open in the game, a flick
    // of the circle pad moves the cursor a cell, and B is Escape. Its flicks
    // are the auto-target's (aim_flick_ms, aim_flick_light). No world (the
    // default, and the pad test): neither.
    void set_menu_nav(MenuWorld *world) { menu_world_ = world; }
    // Puts the cursor at x,y (scripted tests; a touchpad has no absolute position).
    void warp_cursor(int x, int y);
    int cursor_x() const { return cx_; }
    int cursor_y() const { return cy_; }
    // One-shot requests for the platform layer, cleared when read.
    bool take_zoom_toggle() { bool r = zoom_toggle_; zoom_toggle_ = false; return r; }
    bool take_keyboard_request() { bool r = keyboard_; keyboard_ = false; return r; }
    bool take_screenshot_request() { bool r = screenshot_; screenshot_ = false; return r; }
    // Select + D-pad, for the bottom screen: next or previous tab, MAP, or
    // the map's size. kNone when there is none.
    enum class Bottom : uint8_t { kNone, kNextTab, kPrevTab, kMap, kMapSize };
    Bottom take_bottom_request() { Bottom r = bottom_; bottom_ = Bottom::kNone; return r; }

private:
    void move_cursor(int x, int y);
    void key(uint8_t vk, bool down);
    void stick_cursor(int sx, int sy, float speed_800, float &acc_x, float &acc_y);
    void item_move_start(bool sell, uint64_t now_ms);
    bool item_move_step(uint64_t now_ms);   // the left button for this poll

    int w_ = 800, h_ = 600;
    int cx_ = 400, cy_ = 300;     // cursor, game pixels
    uint32_t prev_ = 0;           // buttons held at the previous poll
    // Walking with the circle pad.
    int hero_screen_x(const AimView *v) const;   // where the game draws the hero across
    void walk(float vx, float vy, float m, bool aiming, uint64_t now_ms);
    bool walk_blocked(int x, int y) const;   // a unit a click there would act on
    void walk_read_units(int hero_x, int hero_y);
    struct KeepOut {
        NavRect box;
        // A warp whose box the hero stands in (he arrived by it): only the
        // points nearer its spot than he is are kept off, so he can walk away.
        bool warp_round_hero = false;
        int spot_x = 0, spot_y = 0;
    };
    int walk_hero_x_ = 0, walk_hero_y_ = 0;   // the hero's feet when the units were read
    bool walk_on_warp_ = false;               // ...inside a warp's box: no stop click there
    std::vector<KeepOut> walk_units_;   // where units take clicks, this frame
    uint32_t walk_units_pump_ = 0;
    bool walk_units_read_ = false;
    NavRect walk_hover_box_;      // a spot the game showed a unit at, kept off until walk_hover_until_ms_
    uint64_t walk_hover_until_ms_ = 0;
    int walk_x_min_ = 0, walk_x_max_ = 1 << 20;   // a panel's half: the walk's points stay off it
    bool panel_hold_ = false;     // a panel on one side: the stick out since panel_hold_ms_
    uint64_t panel_hold_ms_ = 0;
    bool panel_walking_ = false;  // ...held long enough (aim_panel_walk_ms): walking
    int walk_stop_ = 0;           // let go: 1 the cursor just ahead of the hero, 2 a click there, until walk_stop_ms_
    float walk_nx_ = 0, walk_ny_ = 1;   // the walk's last direction (y up)
    uint64_t walk_stop_ms_ = 0;
    uint32_t walk_stop_pump_ = 0;
    uint32_t walk_dodges_ = 0, walk_hover_releases_ = 0, walk_blocked_polls_ = 0;
    bool walking_ = false;
    bool walk_press_pending_ = false;   // cursor moved first; press on the next poll
    bool walk_left_ = false;      // walking holds the left button (A and taps are other sources)
    bool select_used_ = false;    // Select was used with R or the D-pad: no zoom toggle on release
    uint8_t start_vk_ = 0;
    uint8_t bound_[16] = {};      // the key each binding pressed (chosen by the R or ZR layer at press time)
    // Touch.
    bool touch_prev_ = false;
    uint64_t touch_start_ms_ = 0;
    int touch_x0_ = 0, touch_y0_ = 0, touch_moved_ = 0, touch_lx_ = 0, touch_ly_ = 0;
    float touch_acc_x_ = 0, touch_acc_y_ = 0;
    uint64_t tap_hold_until_ms_ = 0;
    // The mouse buttons as last sent, and the pump each went down in (begin_pump).
    uint32_t pump_ = 0, down_pump_[2] = {};
    bool pumps_ = false, button_on_[2] = {};
    void button(bool right, bool want);
    float cstick_acc_x_ = 0, cstick_acc_y_ = 0, circle_acc_x_ = 0, circle_acc_y_ = 0;
    bool in_game_ = false, panel_open_ = false, holding_ = false;
    uint64_t ui_ = 0;
    // Y with the inventory open (issue #99): the item under the cursor
    // picked up, put on the shop's panel (sell) or the ground (drop), the
    // cursor back. A step a poll at most, kItemStepMs apart: the game sees
    // each move and each button state for a frame.
    int item_step_ = 0;           // 0: none
    bool item_sell_ = false;
    int item_x_ = 0, item_y_ = 0;  // where the cursor was
    uint64_t item_at_ms_ = 0;     // the next step's time
    bool item_left_ = false;      // the left button as the last step left it
    bool zoom_toggle_ = false, keyboard_ = false, screenshot_ = false;
    Bottom bottom_ = Bottom::kNone;
    // The auto-target.
    AutoTarget aim_;
    AimWorld *aim_world_ = nullptr;
    float walk_dir_x_ = 0, walk_dir_y_ = 1;   // the last walking direction on the screen (y down)
    bool aiming_ = false;         // the auto-target had the cursor at the last poll
    bool aim_on_interface_ = false;   // A or B went down on the control panel: a plain click
    int aim_saved_x_ = 0, aim_saved_y_ = 0;   // where the cursor was before; it goes back there
    uint32_t aim_pump_seen_ = 0;
    uint32_t loot_button_ = 0;    // ZR + this D-pad button: a loot command held (0: none)
    bool loot_pressed_ = false;   // ...pressed this poll
    uint64_t aim_stats_ms_ = 0;
    uint32_t aim_stats_presses_ = 0;
    // Menu navigation.
    void menu_flick(const Pad &pad);
    void game_menu_flick(const Pad &pad);
    void scroll_button_check(uint32_t held);   // the cursor off a scroll button that no longer scrolls
    void game_menu_click(int row, uint64_t now_ms);
    bool game_menu_buttons(uint64_t now_ms);   // the left button for a row clicked for B
    GameMenu game_menu_;
    bool game_menu_on_ = false;   // the game menu's rows read this poll
    bool game_menu_armed_ = true; // the stick was centred since its last step
    uint8_t menu_key_ = 0;        // an arrow key down for the game menu
    uint64_t menu_key_up_ms_ = 0;
    bool menu_snap_ = false;      // the cursor goes onto the lit row once the game moved it
    int menu_snap_from_ = -1;
    uint64_t menu_snap_until_ms_ = 0;
    int menu_click_ = 0;          // B in a submenu: 1 the cursor on "Previous", 2 its button down
    uint64_t menu_click_ms_ = 0;
    uint32_t menu_click_pump_ = 0;
    MenuWorld *menu_world_ = nullptr;
    MenuNav menu_nav_;
    AimFlicks menu_flicks_;
    std::vector<NavWindow> menu_windows_;
    NavScroll menu_scroll_;       // the screen's scroll column at the last flick (D2Win's character screen)
    int flick_from_x_ = 0, flick_from_y_ = 0;   // the cursor when the stick last left the centre
    bool menu_was_open_ = false;
    bool flick_held_ = false;     // a flick moved the cursor a cell; the stick isn't back in the centre yet
    uint64_t menu_now_ms_ = 0;
    // The gold dialog (menu_nav.h's GoldDialog): the D-pad types its amount.
    void gold_step(uint32_t held, bool layer, uint64_t now_ms);
    void gold_type(int amount);
    GoldDialog gold_;
    bool gold_on_ = false;        // open, read this poll
    GoldRepeat gold_repeat_;
    int gold_typed_ = -1;         // the amount last typed in this dialog (-1: none yet)
    int gold_typed_over_ = -1;    // what the field showed when it was typed
    uint8_t keys_[256] = {};      // which virtual keys this layer holds down
    uint8_t pad_keys_[4] = {};    // Pad::keys at the previous poll
};

}  // namespace game

#endif
