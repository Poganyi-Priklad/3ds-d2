// 3ds-d2's control scheme for Diablo II: turns the 3DS's buttons, circle pad,
// C-stick and touch screen into the mouse and keyboard input the game
// expects (through rt::input_*). Platform-neutral: the 3DS app fills a Pad
// from HID each poll; the Linux lane can replay scripted pads.
//
// Default mapping (docs/controls.md):
//   Circle pad   in the game world: walk (the cursor orbits the character and
//                the left button is held; the point keeps off units, so a
//                walk never talks, opens, picks up or attacks; let go, the
//                hero stops where he is); in menus and open panels: move the cursor
//   C-stick      move the cursor (New 3DS)
//   Touch        a touchpad: drag moves the cursor, a short tap is a left click
//                at the cursor
//   A / B        left / right mouse button (held). In the game world, with
//                the auto-target on (auto_target.h): the cursor first goes
//                onto the enemy, NPC or corpse the circle pad points at, and
//                the button goes down once the game shows it under the cursor
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

#include <cstdint>
#include <vector>

namespace game {

// A rectangle in game pixels; right and bottom are outside it.
struct WalkBox {
    int left = 0, top = 0, right = 0, bottom = 0;
    bool contains(int x, int y) const { return x >= left && x < right && y >= top && y < bottom; }
};

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
        WalkBox box;
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
    WalkBox walk_hover_box_;      // a spot the game showed a unit at, kept off until walk_hover_until_ms_
    uint64_t walk_hover_until_ms_ = 0;
    int walk_x_min_ = 0, walk_x_max_ = 1 << 20;   // the game's frame until a panel walk clips it
    uint64_t walk_now_ms_ = 0;
    bool walking_ = false;
    int walk_stop_ = 0;           // let go: 1 the cursor just ahead of the hero, 2 a click there, until walk_stop_ms_
    float walk_nx_ = 0, walk_ny_ = 1;   // the walk's last direction (y up)
    uint64_t walk_stop_ms_ = 0;
    uint32_t walk_stop_pump_ = 0;
    uint32_t walk_dodges_ = 0, walk_hover_releases_ = 0, walk_blocked_polls_ = 0;
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
    int aim_saved_x_ = 0, aim_saved_y_ = 0;   // where the cursor was before; it goes back there
    uint32_t aim_pump_seen_ = 0;
    uint32_t loot_button_ = 0;    // ZR + this D-pad button: a loot command held (0: none)
    bool loot_pressed_ = false;   // ...pressed this poll
    uint64_t aim_stats_ms_ = 0;
    uint32_t aim_stats_presses_ = 0;
    uint8_t keys_[256] = {};      // which virtual keys this layer holds down
    uint8_t pad_keys_[4] = {};    // Pad::keys at the previous poll
};

}  // namespace game

#endif
