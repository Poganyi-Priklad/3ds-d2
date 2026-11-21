// See controls.h.
#include "game/controls.h"

#include "runtime/rt_host.h"

#include <cmath>

namespace game {
namespace {

constexpr uint8_t VK_TAB = 0x09, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_MENU = 0x12, VK_ESCAPE = 0x1B,
                  VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_F1 = 0x70;
constexpr uint8_t kMouseLeft = 0xFE, kMouseRight = 0xFF;   // bindings that are mouse buttons, not keys
constexpr uint8_t kAsNormal = 0xFD;                         // with_zr: the button's normal binding

constexpr float kStickMax = 156.0f;
constexpr float kWalkEngage = 0.20f, kWalkRelease = 0.15f;   // hysteresis, fraction of full tilt
constexpr int kTapMs = 250, kTapSlop = 6;                     // a tap: short and nearly still (touch pixels)
constexpr int kTapHoldMs = 120;                               // how long a tap holds the button
constexpr float kTouchGain = 3.0f;                            // touchpad: frame pixels per touch pixel (800 wide)
constexpr float kCstickSpeed = 18.0f;                         // pixels per poll at full tilt (800 wide)
constexpr float kCircleCursorSpeed = 12.0f;                   // circle pad as a cursor (menus)
constexpr float kMenuStepTilt = 0.5f;     // the game menu: a tilt this far out is one arrow key
constexpr int kMenuKeyMs = 80;            // ...held this long (two game frames)
constexpr int kMenuSnapMs = 300;          // the cursor follows the lit row once it moved, or after this long
constexpr int kMenuClickWaitMs = 40;      // B: the cursor on "Previous" a frame before its button goes down
// Let go of a walk: a click just ahead of the hero stops him there. The
// numbers were measured on the lane (walks in every direction, with no
// panel and with one on either side):
// - the game moves the hero from 8 pixels above the feet point the view
//   gives (the middle of his subtile on the screen), at any frame size;
// - it ignores a click on the subtile he is on (he walks on to the walk's
//   last point, 4 to 8 subtiles), which a click 1 to 8 pixels ahead
//   mostly is; from 12 pixels on it takes it, and 20 lands on the next
//   subtile in every direction (a subtile is 32 x 16 pixels);
// - held longer than a frame, the click becomes a walk after the cursor
//   (the camera keeps the hero in the middle), so it is one frame.
// Drift after letting go, 26 walks: 3.2 subtiles on average (up to 6.8,
// one in three turning back) at the feet, 1.4 (up to 2.1, none turning
// back) this way.
constexpr int kStopCentreDy = -8;         // the hero's middle: his feet point's y plus this
constexpr float kStopAheadPx = 20.0f;     // the click this far ahead of it, the way he walked
constexpr int kStopWaitMs = 0;            // the cursor there first; the click on the next frame
constexpr int kStopHoldMs = 0;            // ...held for one frame
constexpr int kHoverKeepOutMs = 1000;     // a spot the game showed a unit at is kept off this long
constexpr uint64_t kUiGameMenu = 1ull << 0x09, kUiConfigure = 1ull << 0x0B;

struct Binding {
    Button button;
    uint8_t normal, with_r, with_zr;   // virtual key (or kMouse*, kAsNormal); 0 = nothing
};

// Buttons that act on press and release. Select and Start are handled apart.
// R and ZR are modifiers (issue #95): ZR alone does nothing; ZR + A/B/Y/X are
// the skill hotkeys F1-F4 (R + D-pad still are too); ZR + D-pad, held, picks
// up items with the auto-target (Up valuables, Right gold, Down everything,
// Left kept free); L and ZL do as they always do.
const Binding kBindings[] = {
    {kA, kMouseLeft, 'C', VK_F1},
    {kB, kMouseRight, 'T', VK_F1 + 1},
    {kX, 'I', VK_TAB, VK_F1 + 3},
    {kY, VK_MENU, 'Q', VK_F1 + 2},
    {kL, VK_SHIFT, VK_SHIFT, kAsNormal},
    {kUp, '1', VK_F1, 0},
    {kRight, '2', VK_F1 + 1, 0},
    {kDown, '3', VK_F1 + 2, 0},
    {kLeft, '4', VK_F1 + 3, 0},
    {kZL, 'W', 'W', kAsNormal},
};

}  // namespace

void Controls::set_frame(int width, int height)
{
    if (width > 0 && height > 0) {
        w_ = width;
        h_ = height;
    }
}

void Controls::warp_cursor(int x, int y) { move_cursor(x, y); }

namespace {
// The auto-target's "aim:" lines go to game.log like the port's others.
void aim_log(const char *line) { rt::logf("%s", line); }
}  // namespace

void Controls::set_auto_target(const AimSettings &settings, AimWorld *world)
{
    aim_.set_settings(settings);
    aim_.set_log(aim_log);
    aim_world_ = world;
}

void Controls::set_context(bool in_game, bool panel_open, uint64_t ui, bool holding)
{
    in_game_ = in_game;
    panel_open_ = panel_open;
    ui_ = ui;
    holding_ = holding;
}

// ---- Y with the inventory open: sell or drop the item under the cursor ----
//
// The UI table's entries (d2_state.h): the inventory 0x01 (the frame's right
// half), a shop 0x0C (the left half, the inventory beside it without 0x01's
// entry, as the lane showed); the other left-half panels, over which a drop
// would land on the panel: character sheet, socketing, quest log, waypoints,
// party, trade, stash, cube, mercenary.
constexpr uint64_t kUiInventory = 1ull << 0x01, kUiShop = 1ull << 0x0C;
constexpr uint64_t kUiLeftPanels = 1ull << 0x02 | 1ull << 0x0C | 1ull << 0x0E | 1ull << 0x0F | 1ull << 0x14 |
                                   1ull << 0x16 | 1ull << 0x17 | 1ull << 0x19 | 1ull << 0x1A | 1ull << 0x24;
constexpr uint64_t kItemStepMs = 80;   // two game frames

void Controls::item_move_start(bool sell, uint64_t now_ms)
{
    item_sell_ = sell;
    item_x_ = cx_;
    item_y_ = cy_;
    item_step_ = 1;
    item_at_ms_ = now_ms;
    rt::logf("controls: %s the item at %d,%d", sell ? "selling" : "dropping", cx_, cy_);
}

// Steps: 1 press (picks the item up) - 2 release - 3 the item on the cursor?
// else stop (nothing was under the cursor: a click on a shop's goods would
// buy, one on the ground walk) - 4 the cursor to the left half's middle (the
// shop's panel, or the ground left of the character) - 5 press - 6 release
// - 7 the cursor back; still holding (the shop refused it)? 8 press, 9
// release: the item back where it was.
bool Controls::item_move_step(uint64_t now_ms)
{
    if (!item_step_)
        return false;
    if (now_ms < item_at_ms_)
        return item_left_;   // the button as the last step left it
    item_at_ms_ = now_ms + kItemStepMs;
    item_left_ = false;
    switch (item_step_++) {
    case 1:
        item_left_ = true;
        break;
    case 3:
        if (!holding_) {
            rt::logf("controls: no item under the cursor");
            item_step_ = 0;
        }
        break;
    case 4:
        move_cursor(w_ / 4, h_ / 2);
        break;
    case 5:
        item_left_ = true;
        break;
    case 7:
        move_cursor(item_x_, item_y_);
        if (!holding_) {
            item_step_ = 0;
            rt::logf("controls: item %s", item_sell_ ? "sold" : "dropped");
        }
        break;
    case 8:
        rt::logf("controls: the item came back; put where it was");
        item_left_ = true;
        break;
    case 9:
        item_step_ = 0;
        break;
    default:   // 2, 6: let go
        break;
    }
    return item_left_;
}

// Relative cursor from a stick: quadratic response, so small tilts aim finely.
void Controls::stick_cursor(int sx, int sy, float speed_800, float &acc_x, float &acc_y)
{
    const float vx = sx / kStickMax, vy = sy / kStickMax;
    const float speed = speed_800 * w_ / 800.0f;
    if (std::fabs(vx) > 0.15f)
        acc_x += vx * std::fabs(vx) * speed;
    if (std::fabs(vy) > 0.15f)
        acc_y -= vy * std::fabs(vy) * speed;   // stick up = screen up
    const int dx = (int)acc_x, dy = (int)acc_y;
    if (dx || dy) {
        acc_x -= dx;
        acc_y -= dy;
        move_cursor(cx_ + dx, cy_ + dy);
    }
}

// ---- menu navigation (menu_nav.h) ----
//
// A slow tilt moves the cursor as before (stick_cursor); a flick, once read,
// puts it a cell on from where it was when the stick left the centre, so
// the bit the flick itself moved it doesn't count.
void Controls::menu_flick(const Pad &pad)
{
    // In the game with a panel open; out of it, D2Win's screens.
    const bool open = menu_world_ && (in_game_ ? panel_open_ : true);
    if (!open) {
        if (menu_was_open_)
            menu_nav_.forget();
        menu_was_open_ = flick_held_ = false;
        return;
    }
    menu_was_open_ = true;
    if ((in_game_ && (ui_ & kUiConfigure)) || game_menu_on_) {
        // The flicks' own state kept current (the stick's last centring).
        menu_flicks_.update(pad.circle_x / kStickMax, -pad.circle_y / kStickMax, menu_now_ms_, aim_.settings());
        if (game_menu_on_)
            game_menu_flick(pad);
        else
            flick_held_ = false;   // Configure Controls: a free cursor
        return;
    }
    game_menu_armed_ = true;
    const float x = pad.circle_x / kStickMax, y = -pad.circle_y / kStickMax;   // screen y down
    if (std::sqrt(x * x + y * y) < AimFlicks::kCentre) {
        flick_from_x_ = cx_;
        flick_from_y_ = cy_;
        flick_held_ = false;
    }
    const AimFlicks::Flick flick = menu_flicks_.update(x, y, menu_now_ms_, aim_.settings());
    if (!flick.happened)
        return;
    flick_held_ = std::sqrt(x * x + y * y) >= AimFlicks::kCentre;
    const NavDirection dir = nav_direction(flick.dir_x, flick.dir_y);
    menu_world_->windows(ui_, w_, h_, menu_windows_);
    menu_scroll_ = NavScroll();
    for (const NavWindow &win : menu_windows_)
        if (win.scroll.present)
            menu_scroll_ = win.scroll;
    const MenuNav::Step step =
        menu_nav_.step(menu_windows_, flick_from_x_, flick_from_y_, dir, holding_, w_, h_);
    if (step.moved) {
        rt::logf("menu: flick %s from %d,%d: %s, to %d,%d", nav_direction_name(dir), flick_from_x_, flick_from_y_,
                 step.why, step.x, step.y);
        move_cursor(step.x, step.y);
        circle_acc_x_ = circle_acc_y_ = 0;
    } else {
        rt::logf("menu: flick %s from %d,%d: nowhere to go (%u windows)", nav_direction_name(dir), flick_from_x_,
                 flick_from_y_, (unsigned)menu_windows_.size());
        move_cursor(flick_from_x_, flick_from_y_);
    }
    flick_from_x_ = cx_;
    flick_from_y_ = cy_;
}

// A scroll button the cursor is on (a flick put it there; A clicks it, a
// row a press) stops scrolling at the list's end: the cursor goes back to
// the slot nearest in height (the list's first or last row), so A doesn't
// click a button that does nothing. Read again each frame while the cursor
// is on one, not while A is down.
void Controls::scroll_button_check(uint32_t held)
{
    if (in_game_ || !menu_world_ || !menu_scroll_.present || (held & kA) || button_on_[0])
        return;
    const bool on_up = menu_scroll_.up.contains(cx_, cy_), on_down = menu_scroll_.down.contains(cx_, cy_);
    if (!on_up && !on_down)
        return;
    menu_world_->windows(ui_, w_, h_, menu_windows_);
    NavScroll now;
    for (const NavWindow &win : menu_windows_)
        if (win.scroll.present)
            now = win.scroll;
    menu_scroll_ = now;
    if (!now.present || (on_up ? now.up_on : now.down_on))
        return;
    int x = cx_, y = cy_;
    const char *why = "";
    NavScroll back = now;
    back.up_on = back.down_on = true;   // scroll_step's left rule, whatever is active
    if (!scroll_step(back, cx_, cy_, NavDirection::kLeft, x, y, why))
        return;
    rt::logf("menu: the scroll button at %d,%d scrolls no further: to the slot at %d,%d", cx_, cy_, x, y);
    move_cursor(x, y);
    flick_from_x_ = cx_;
    flick_from_y_ = cy_;
}

// ---- the game menu (Escape) and its submenus ----
//
// The game moves its own lit row with the arrow keys (past titles and
// disabled rows, round at the ends) and re-picks it from the cursor only
// when the mouse moves. So a tilt up or down is Up or Down, and once the
// game has moved the light the cursor goes onto that row, for A; left and
// right are Left and Right on a choice or a slider. The stick doesn't move
// the cursor here: a move in the same frame as the key would re-pick the
// row under the cursor over the key's.
void Controls::game_menu_flick(const Pad &pad)
{
    const int n = (int)game_menu_.rows.size();
    const int lit = game_menu_.lit;
    if (menu_snap_ && !menu_click_ && lit >= 0 && lit < n &&
        (lit != menu_snap_from_ || menu_now_ms_ >= menu_snap_until_ms_)) {
        const NavRect &r = game_menu_.rows[lit].rect;
        move_cursor(r.centre_x(), r.centre_y());
        menu_snap_ = false;
    }
    const float x = pad.circle_x / kStickMax, y = -pad.circle_y / kStickMax;   // screen y down
    const float tilt = std::sqrt(x * x + y * y);
    if (tilt < AimFlicks::kCentre) {
        game_menu_armed_ = true;
        flick_held_ = false;
        return;
    }
    if (!game_menu_armed_ || tilt < kMenuStepTilt)
        return;
    game_menu_armed_ = false;
    flick_held_ = true;
    const NavDirection dir = nav_direction(x, y);
    uint8_t vk = 0;
    if (dir == NavDirection::kUp || dir == NavDirection::kDown) {
        vk = dir == NavDirection::kUp ? VK_UP : VK_DOWN;
        menu_snap_ = true;
        menu_snap_from_ = lit;
        menu_snap_until_ms_ = menu_now_ms_ + kMenuSnapMs;
    } else if (lit >= 0 && lit < n &&
               (game_menu_.rows[lit].kind == GameMenu::kChoice || game_menu_.rows[lit].kind == GameMenu::kSlider)) {
        vk = dir == NavDirection::kLeft ? VK_LEFT : VK_RIGHT;
    }
    if (!vk) {
        rt::logf("menu: game menu, %s on row %d: nothing to change", nav_direction_name(dir), lit);
        return;
    }
    if (menu_key_)
        key(menu_key_, false);
    key(vk, true);
    menu_key_ = vk;
    menu_key_up_ms_ = menu_now_ms_ + kMenuKeyMs;
    rt::logf("menu: game menu, %s from row %d of %d", nav_direction_name(dir), lit, n);
}

// B in a submenu: a click on its "Previous" row, the cursor there a frame
// before the button goes down (the game clicks what it showed under the
// cursor in the frame before).
void Controls::game_menu_click(int row, uint64_t now_ms)
{
    if (row < 0 || row >= (int)game_menu_.rows.size())
        return;
    const NavRect &r = game_menu_.rows[row].rect;
    move_cursor(r.centre_x(), r.centre_y());
    menu_click_ = 1;
    menu_click_ms_ = now_ms + kMenuClickWaitMs;
    menu_click_pump_ = pump_;
    menu_snap_ = false;
    rt::logf("menu: game menu, B: row %d (Previous) at %d,%d", row, cx_, cy_);
}

bool Controls::game_menu_buttons(uint64_t now_ms)
{
    if (menu_click_ == 1) {
        if (now_ms < menu_click_ms_ || (pumps_ && pump_ == menu_click_pump_))
            return false;
        menu_click_ = 2;
        menu_click_ms_ = now_ms + kTapHoldMs;
        return true;
    }
    if (menu_click_ == 2) {
        if (now_ms < menu_click_ms_)
            return true;
        menu_click_ = 0;
    }
    return false;
}


// ---- walking: the point keeps off units ----
//
// A click (or the held button) on a unit is the game's action on it: a talk,
// a chest or door opened, an item picked up, an attack. The walk's point
// keeps off the places units take clicks at (a box over each one's feet,
// from the auto-target's world); should the game still show a unit under
// it (a box too small, a warp), the button goes up at once and that spot is
// kept off for a moment. Picking up and talking stay on A.
namespace {

// Where a unit takes clicks, round its feet on the screen (game pixels).
NavRect unit_box(const AimUnit &u)
{
    switch (u.type) {
    case 2: return NavRect{u.feet_x - 36, u.feet_y - 80, u.feet_x + 36, u.feet_y + 20};   // an object
    case 4: return NavRect{u.feet_x - 22, u.feet_y - 18, u.feet_x + 22, u.feet_y + 12};   // an item
    case 5: return NavRect{u.feet_x - 38, u.feet_y - 128, u.feet_x + 98, u.feet_y + 54};  // a warp (warp_box)
    default: return NavRect{u.feet_x - 28, u.feet_y - 95, u.feet_x + 28, u.feet_y + 12};  // a monster, an NPC
    }
}

// A level warp (a cave entrance, stairs, a passage): where the game takes
// its clicks (LvlWarp.txt's select rectangle, read by the world), with 8
// pixels round it and 24 more below, where a hover scan on the lane still
// found it (the Act I cave exit: its rectangle says 30 below the spot, the
// game showed it to about 50). Unknown, a box like that cave exit's (unit_box).
NavRect warp_box(const AimUnit &u, AimWorld *world)
{
    int left = 0, top = 0, width = 0, height = 0;
    if (!world || !world->warp_area(u.cls, left, top, width, height) || width <= 0 || height <= 0)
        return unit_box(u);
    return NavRect{u.feet_x + left - 8, u.feet_y + top - 8, u.feet_x + left + width + 8, u.feet_y + top + height + 24};
}

// A monster's modes: 0 dying, 12 dead (a corpse takes no plain click).
bool alive_other(const AimUnit &u) { return !u.owned && u.mode != 0 && u.mode != 12; }

}  // namespace

void Controls::walk_read_units(int hero_x, int hero_y)
{
    walk_hero_x_ = hero_x;
    walk_hero_y_ = hero_y;
    if (walk_units_read_ && pumps_ && walk_units_pump_ == pump_)
        return;   // once a frame
    walk_units_read_ = true;
    walk_units_pump_ = pump_;
    walk_units_.clear();
    walk_on_warp_ = false;
    if (!aim_world_)
        return;
    static std::vector<AimUnit> units, ground;
    units.clear();
    ground.clear();
    aim_world_->units(units);
    aim_world_->ground_units(ground);
    for (const AimUnit &u : units)
        if (u.type == 1 && alive_other(u))
            walk_units_.push_back(KeepOut{unit_box(u)});
    for (const AimUnit &u : ground) {
        if (u.type == 2 && object_kind(u) == AimObject::kNone && !u.operate_fn)
            continue;   // scenery: no click does anything to it
        if (u.type == 2 || u.type == 4)
            walk_units_.push_back(KeepOut{unit_box(u)});
        else if (u.type == 5) {   // a warp: the stick never takes the hero to another level
            KeepOut k{warp_box(u, aim_world_)};
            k.spot_x = u.feet_x, k.spot_y = u.feet_y;
            k.warp_round_hero = k.box.contains(hero_x, hero_y);
            walk_on_warp_ = walk_on_warp_ || k.warp_round_hero;
            walk_units_.push_back(k);
        }
    }
}

bool Controls::walk_blocked(int x, int y) const
{
    auto d2 = [](int ax, int ay, int bx, int by) { return (ax - bx) * (ax - bx) + (ay - by) * (ay - by); };
    for (const KeepOut &k : walk_units_)
        if (k.box.contains(x, y) &&
            (!k.warp_round_hero || d2(x, y, k.spot_x, k.spot_y) < d2(walk_hero_x_, walk_hero_y_, k.spot_x, k.spot_y)))
            return true;
    return menu_now_ms_ < walk_hover_until_ms_ && walk_hover_box_.contains(x, y);
}

// Where the game draws the hero across: the auto-target's view (his feet,
// the camera moved by a panel open on one side), else the middle of the
// free half (walk_x_min_ / walk_x_max_ while a panel is open), else the
// frame's middle.
int Controls::hero_screen_x(const AimView *v) const
{
    if (v && v->hero_x > 0 && v->hero_x < w_)
        return v->hero_x;
    if (walk_x_min_ > 0)
        return (walk_x_min_ + w_) / 2;
    if (walk_x_max_ < w_ - 1)
        return walk_x_max_ / 2;
    return w_ / 2;
}

void Controls::walk(float vx, float vy, float m, bool aiming, uint64_t now_ms)
{
    if (!walking_ && m > kWalkEngage) {
        walking_ = true;
        walk_press_pending_ = true;
        walk_units_read_ = false;
    } else if (walking_ && m < kWalkRelease) {
        walking_ = false;
        walk_press_pending_ = false;
        // Let go: the hero stops where he stands, not where the point last
        // was. The button goes up, the cursor just ahead of him (kStopAheadPx
        // from his middle, the way he walked), and a frame later a one-frame
        // click there: the walk's new goal is the next subtile.
        bool stop = walk_left_ && !aiming && aim_.settings().walk_stop;   // aim_walk_stop=0: no click
        int hx = hero_screen_x(nullptr), hy = (int)(h_ * 0.47f);   // no view: the walk's own middle
        AimView v;
        if (stop && aim_world_ && aim_world_->view(v)) {
            hx = v.hero_x;
            hy = v.hero_y + kStopCentreDy;
        }
        int fx = hx + (int)std::lround(walk_nx_ * kStopAheadPx), fy = hy - (int)std::lround(walk_ny_ * kStopAheadPx);
        fx = fx < walk_x_min_ ? walk_x_min_ : fx > walk_x_max_ ? walk_x_max_ : fx;
        if (stop) {
            walk_read_units(hx, hy - kStopCentreDy);
            stop = !walk_blocked(fx, fy) && !walk_on_warp_;   // no click on a unit, nor on a warp he stands by
        }
        walk_left_ = false;
        if (stop) {
            move_cursor(fx, fy);
            walk_stop_ = 1;
            walk_stop_ms_ = now_ms + kStopWaitMs;
            walk_stop_pump_ = pump_;
        }
        if (walk_dodges_ || walk_hover_releases_ || walk_blocked_polls_)
            rt::logf("walk: kept off units: %u polls on another point, %u button releases for the game's hover, %u "
                     "polls with every point taken",
                     walk_dodges_, walk_hover_releases_, walk_blocked_polls_);
        walk_dodges_ = walk_hover_releases_ = walk_blocked_polls_ = 0;
    }
    if (walk_stop_ && (walking_ || aiming)) {
        if (!walking_)
            walk_left_ = false;
        walk_stop_ = 0;   // the stick again, or A: no stop click
    } else if (walk_stop_ == 1 && now_ms >= walk_stop_ms_ && (!pumps_ || pump_ != walk_stop_pump_)) {
        walk_stop_ = 2;   // the click ahead of him
        walk_left_ = true;
        walk_stop_ms_ = now_ms + kStopHoldMs;
    } else if (walk_stop_ == 2 && now_ms >= walk_stop_ms_) {
        walk_stop_ = 0;
        walk_left_ = false;
    }
    if (walking_ && aiming) {
        // The auto-target has the cursor: the stick only aims, the hero
        // doesn't walk. Once it lets go, the walk moves the cursor first
        // and presses on the next poll, as at a new tilt.
        walk_left_ = false;
        walk_press_pending_ = true;
        return;
    }
    if (!walking_)
        return;
    // The click point orbits the character (drawn a little above the
    // frame's centre); a fuller tilt reaches further. Across, it orbits the
    // hero where the game draws him: a panel open on one side moves him to
    // the middle of the other half (the game's camera, read by the
    // auto-target's world), else the frame's middle.
    if (m > 1.0f)
        m = 1.0f;
    const float scale = h_ / 600.0f;
    const float radius = (50.0f + 60.0f * m) * scale;
    const float len = std::sqrt(vx * vx + vy * vy);
    const float nx = len > 0 ? vx / len : 0.0f, ny = len > 0 ? vy / len : 1.0f;
    walk_nx_ = nx, walk_ny_ = ny;   // the stop click's direction
    AimView v;
    const bool seen = aim_world_ && aim_world_->view(v);
    const int ox = hero_screen_x(seen ? &v : nullptr), oy = (int)(h_ * 0.47f);

    // The game shows a unit under the cursor (where the walk put it a frame
    // ago): the button up before the game acts on it, and that spot kept off.
    bool hovered = false;
    if (seen && v.hover_on && v.hover_type != 0) {
        hovered = true;
        AimUnit u;
        if (v.hover_type == 1 && aim_world_->unit(v.hover_id, u) && !alive_other(u))
            hovered = false;   // a pet or a corpse: a click there walks
    }
    if (hovered) {
        if (walk_left_)
            walk_hover_releases_++;
        walk_left_ = false;
        walk_press_pending_ = true;
        walk_hover_box_ = NavRect{cx_ - 30, cy_ - 30, cx_ + 30, cy_ + 30};
        walk_hover_until_ms_ = now_ms + kHoverKeepOutMs;
    }
    walk_read_units(seen ? v.hero_x : ox, seen ? v.hero_y : oy);
    // The point the stick asks for, else the nearest free one round it.
    static const struct {
        float turn_deg, extra;
    } kPoints[] = {{0, 0}, {0, 45}, {20, 0}, {-20, 0}, {20, 45}, {-20, 45}, {0, -25}, {40, 0}, {-40, 0}, {40, 45}, {-40, 45}};
    for (size_t k = 0; k < sizeof kPoints / sizeof kPoints[0]; k++) {
        const float a = kPoints[k].turn_deg * 3.14159265f / 180.0f;
        const float rx = nx * std::cos(a) - ny * std::sin(a), ry = nx * std::sin(a) + ny * std::cos(a);
        float r = radius + kPoints[k].extra * scale;
        r = r < 35.0f * scale ? 35.0f * scale : r;
        int x = ox + (int)(rx * r);
        const int y = oy - (int)(ry * r);
        x = x < walk_x_min_ ? walk_x_min_ : x > walk_x_max_ ? walk_x_max_ : x;   // a panel's half: off it
        if (walk_blocked(x, y) || x < 0 || y < 0 || x >= w_ || y >= h_)
            continue;
        if (k)
            walk_dodges_++;
        move_cursor(x, y);
        // Move first, press on the next poll: the game resolves a click
        // against what the cursor hovered in the previous frame.
        if (walk_press_pending_) {
            if (!hovered)
                walk_press_pending_ = false;
        } else {
            walk_left_ = true;
        }
        return;
    }
    // Every point is on a unit: the button up, the hero stops by it.
    walk_blocked_polls_++;
    walk_left_ = false;
    walk_press_pending_ = true;
}

void Controls::move_cursor(int x, int y)
{
    x = x < 0 ? 0 : x >= w_ ? w_ - 1 : x;
    y = y < 0 ? 0 : y >= h_ ? h_ - 1 : y;
    if (x == cx_ && y == cy_)
        return;
    cx_ = x;
    cy_ = y;
    rt::input_mouse_move(x, y);
}

void Controls::key(uint8_t vk, bool down)
{
    if (!vk || (keys_[vk] != 0) == down)
        return;
    keys_[vk] = down;
    rt::input_key(vk, down);
}

void Controls::update(const Pad &pad, uint64_t now_ms)
{
    const uint32_t held = pad.held, pressed = held & ~prev_, released = prev_ & ~held;
    const bool r_layer = (held & kR) != 0;
    const bool zr_layer = (held & kZR) != 0;

    // ---- the game menu's rows, while it is open (menu_nav.h) ----
    menu_now_ms_ = now_ms;
    if (menu_key_ && now_ms >= menu_key_up_ms_) {
        key(menu_key_, false);
        menu_key_ = 0;
    }
    const GameMenu was = game_menu_;
    const bool was_on = game_menu_on_;
    game_menu_on_ = menu_world_ && in_game_ && (ui_ & kUiGameMenu) && !(ui_ & kUiConfigure) &&
                    menu_world_->game_menu(ui_, w_, h_, game_menu_) && !game_menu_.rows.empty() &&
                    !game_menu_.free_cursor;
    if (game_menu_on_ &&
        (!was_on || was.rows.size() != game_menu_.rows.size() || was.previous != game_menu_.previous ||
         was.rows[0].rect.top != game_menu_.rows[0].rect.top)) {
        // A menu opened (or another in its place): the cursor onto its lit
        // row (once a click for B is over).
        menu_snap_ = true;
        menu_snap_from_ = -2;
    }
    if (!game_menu_on_)
        menu_snap_ = false;

    // ---- Select / Start: zoom, keyboard, Escape, Enter ----
    if (pressed & kSelect) {
        select_used_ = r_layer;
        if (r_layer && (held & kL))
            screenshot_ = true;   // L+R+Select: screenshot to the Camera album
        else if (r_layer)
            keyboard_ = true;   // R+Select: on-screen keyboard
    }
    if (pressed & kStart) {
        start_vk_ = r_layer ? VK_RETURN : VK_ESCAPE;
        key(start_vk_, true);
    }
    if (released & kStart) {
        key(start_vk_, false);
        start_vk_ = 0;
    }
    if ((released & kSelect) && !select_used_)
        zoom_toggle_ = true;

    // ---- Select + D-pad: the bottom screen's tabs (not the belt) ----
    const bool select_layer = (held & kSelect) && !r_layer;
    if (select_layer) {
        static const struct { uint32_t button; Bottom request; } kSelectPad[] = {
            {kRight, Bottom::kNextTab}, {kLeft, Bottom::kPrevTab}, {kUp, Bottom::kMap}, {kDown, Bottom::kMapSize}};
        for (const auto &s : kSelectPad)
            if (pressed & s.button) {
                bottom_ = s.request;
                select_used_ = true;   // no zoom toggle when Select is let go
            }
    }

    // ---- bound buttons: the layer chosen at press time is the one released ----
    bool a_left = false, b_right = false;
    for (size_t i = 0; i < sizeof kBindings / sizeof kBindings[0]; i++) {
        const Binding &b = kBindings[i];
        if ((pressed & b.button) && select_layer && (b.button & (kUp | kDown | kLeft | kRight))) {
            bound_[i] = 0;   // taken by the Select layer above
        } else if ((pressed & b.button) && b.button == kY && !r_layer && !zr_layer && in_game_ && !item_step_ &&
                   ((ui_ & kUiShop) || ((ui_ & kUiInventory) && !(ui_ & kUiLeftPanels)))) {
            bound_[i] = 0;   // Y with the inventory open: sell or drop, not Alt (issue #99)
            item_move_start((ui_ & kUiShop) != 0, now_ms);
        } else if ((pressed & b.button) && zr_layer && (b.button & (kUp | kDown | kRight))) {
            bound_[i] = 0;   // ZR + D-pad: a loot command for the auto-target (below)
            loot_button_ = b.button;
            loot_pressed_ = true;
        } else if ((pressed & b.button) && b.button == kB && !r_layer && !zr_layer && menu_world_ && in_game_ &&
                   panel_open_ && game_menu_on_ && game_menu_.previous >= 0) {
            bound_[i] = 0;   // a game menu's submenu: B is its "Previous", one menu back
            game_menu_click(game_menu_.previous, now_ms);
        } else if ((pressed & b.button) && b.button == kB && !r_layer && !zr_layer && menu_world_ && in_game_ &&
                   panel_open_) {
            bound_[i] = VK_ESCAPE;   // menu navigation: B is the panel's back or close
            key(bound_[i], true);
        } else if ((pressed & b.button) && b.button == kB && !r_layer && !zr_layer && menu_world_ && !in_game_) {
            // Out of the game (character select, the class screen, a
            // difficulty or a message box): B is Escape, the screen's own
            // Exit or Cancel, one screen back. Not on the main menu, whose
            // Escape quits the game: there B does nothing.
            bound_[i] = menu_world_->main_menu() ? 0 : VK_ESCAPE;
            key(bound_[i], true);
        } else if (pressed & b.button) {
            bound_[i] = zr_layer ? (b.with_zr == kAsNormal ? b.normal : b.with_zr) : r_layer ? b.with_r : b.normal;
            if (bound_[i] != kMouseLeft && bound_[i] != kMouseRight)
                key(bound_[i], true);
        }
        if ((held & b.button) && bound_[i] == kMouseLeft)
            a_left = true;
        if ((held & b.button) && bound_[i] == kMouseRight)
            b_right = true;
        if (released & b.button) {
            if (bound_[i] != kMouseLeft && bound_[i] != kMouseRight)
                key(bound_[i], false);
            bound_[i] = 0;
        }
    }

    // ---- keys held by the bottom screen's buttons ----
    for (uint8_t old : pad_keys_) {
        bool still = false;
        for (uint8_t k : pad.keys)
            still |= k == old;
        if (old && !still)
            key(old, false);
    }
    for (int i = 0; i < 4; i++) {
        key(pad.keys[i], true);
        pad_keys_[i] = pad.keys[i];
    }

    // ---- the bottom screen pointing straight at the game ----
    if (pad.pointing)
        move_cursor(pad.point_x, pad.point_y);

    // ---- C-stick: relative cursor, quadratic response ----
    stick_cursor(pad.cstick_x, pad.cstick_y, kCstickSpeed, cstick_acc_x_, cstick_acc_y_);

    // ---- touch: a touchpad. Dragging moves the cursor (relative, scaled so
    // one drag across the pad crosses the frame); a short tap clicks where the
    // cursor already is. ----
    // A tap's button stays down for kTapHoldMs: down and up inside one game
    // frame (40 ms) read as no click at all.
    bool tap_left = now_ms < tap_hold_until_ms_;
    if (pad.touching) {
        if (!touch_prev_) {
            touch_start_ms_ = now_ms;
            touch_x0_ = touch_lx_ = pad.touch_x;
            touch_y0_ = touch_ly_ = pad.touch_y;
            touch_moved_ = 0;
        }
        const int d = std::abs(pad.touch_x - touch_x0_) + std::abs(pad.touch_y - touch_y0_);
        touch_moved_ = d > touch_moved_ ? d : touch_moved_;
        touch_acc_x_ += (pad.touch_x - touch_lx_) * kTouchGain * w_ / 800.0f;
        touch_acc_y_ += (pad.touch_y - touch_ly_) * kTouchGain * h_ / 600.0f;
        touch_lx_ = pad.touch_x;
        touch_ly_ = pad.touch_y;
        const int dx = (int)touch_acc_x_, dy = (int)touch_acc_y_;
        if (dx || dy) {
            touch_acc_x_ -= dx;
            touch_acc_y_ -= dy;
            move_cursor(cx_ + dx, cy_ + dy);
        }
    } else if (touch_prev_ && now_ms - touch_start_ms_ < (uint64_t)kTapMs && touch_moved_ <= kTapSlop) {
        tap_left = true;
        tap_hold_until_ms_ = now_ms + kTapHoldMs;
    }
    if (!pad.touching)
        touch_acc_x_ = touch_acc_y_ = 0;
    touch_prev_ = pad.touching;

    // ---- the auto-target on plain A and B (auto_target.h) ----
    // A and B with no R or ZR layer (bound to the mouse buttons at press
    // time), in the game world with no panel open, nothing on the cursor,
    // L (stand still) not held and no script tap. It reads the game only
    // while A or B is held.
    AimResult aim;
    if (aim_world_) {
        const float vx = pad.circle_x / kStickMax, vy = pad.circle_y / kStickMax;
        const float m = std::sqrt(vx * vx + vy * vy);
        const bool stick_engaged = m > kWalkEngage || (walking_ && m >= kWalkRelease);
        if (stick_engaged) {   // the screen's y is down, the pad's up
            walk_dir_x_ = vx / m;
            walk_dir_y_ = -vy / m;
        }
        // A or B pressed with the cursor on the game's own interface (the
        // control panel, the open belt): a plain click there for the whole
        // press, no auto-target.
        if ((pressed & (kA | kB)) && !aiming_) {
            aim_on_interface_ = in_game_ && !panel_open_ &&
                                (menu_world_ ? menu_world_->on_interface(ui_, cx_, cy_, w_, h_)
                                             : control_panel_at(cx_, cy_, w_, h_));
            if (aim_on_interface_)
                rt::logf("aim: %s on the game's interface at %d,%d: a plain click", (pressed & kA) ? "A" : "B", cx_,
                         cy_);
        }
        if (!(held & (kA | kB)))
            aim_on_interface_ = false;
        AimPoll poll;
        poll.now_ms = now_ms;
        poll.allowed = in_game_ && !panel_open_ && !holding_ && !item_step_ && !(held & kL) && !pad.aim_off &&
                       !pad.pointing && !select_layer && !aim_on_interface_;
        poll.a_held = (held & kA) && bound_[0] == kMouseLeft;
        poll.b_held = (held & kB) && bound_[1] == kMouseRight;
        poll.a_pressed = poll.a_held && (pressed & kA);
        poll.b_pressed = poll.b_held && (pressed & kB);
        poll.stick_engaged = stick_engaged;
        poll.dir_x = walk_dir_x_;
        poll.dir_y = walk_dir_y_;
        poll.stick_x = vx;
        poll.stick_y = -vy;
        poll.cursor_x = cx_;   // hover first: an item's name under the cursor
        poll.cursor_y = cy_;
        // ZR + D-pad: held until the D-pad button is let go.
        if (loot_button_ && !(held & loot_button_))
            loot_button_ = 0;
        poll.loot = loot_button_ == kUp      ? AimLootCommand::kValuables
                    : loot_button_ == kRight ? AimLootCommand::kGold
                    : loot_button_ == kDown  ? AimLootCommand::kEverything
                                             : AimLootCommand::kNone;
        poll.loot_pressed = loot_pressed_ && poll.loot != AimLootCommand::kNone;
        loot_pressed_ = false;
        poll.new_frame = !pumps_ || pump_ != aim_pump_seen_;
        aim_pump_seen_ = pump_;
        aim = aim_.update(poll, *aim_world_);
        if (aim.aiming && !aiming_) {
            aim_saved_x_ = cx_;
            aim_saved_y_ = cy_;
        }
        if (!aim.aiming && aiming_ && !poll.a_held && !poll.b_held && poll.loot == AimLootCommand::kNone &&
            !stick_engaged)
            move_cursor(aim_saved_x_, aim_saved_y_);   // let go: the cursor back where it was
        aiming_ = aim.aiming;
        // Every 30 s of use, one summary line in game.log: how often each
        // rule fired and what the memory reads cost (for tuning and reports).
        const AimStats &st = aim_.stats();
        if (st.presses != aim_stats_presses_ && now_ms - aim_stats_ms_ >= 30000) {
            const AimReadCost cost = aim_world_->read_cost();
            rt::logf("aim: stats: %u presses, %u targets (%u next to the hero), %u confirmed, %u taken from the "
                     "hover, %u plain clicks at a target, %u with nothing in the cone; held %u frames, %u on the "
                     "target; dropped %u dead, %u gone, %u unreachable, %u off the screen, %u forgotten; %u flicks "
                     "(%u switched); %u objects, %u items (%u with no room); "
                     "%u scans of %u units, %llu us on average, %llu at most",
                     st.presses, st.targets, st.near_rule, st.confirmed, st.adopted, st.fallbacks, st.no_target,
                     st.held_polls, st.held_on_target, st.dropped_dead, st.dropped_gone, st.dropped_lost,
                     st.dropped_screen, st.forgotten, st.flicks, st.flick_switches, st.objects, st.items, st.no_room,
                     cost.scans, cost.units,
                     (unsigned long long)(cost.scans ? cost.total_us / cost.scans : 0),
                     (unsigned long long)cost.longest_us);
            aim_stats_ms_ = now_ms;
            aim_stats_presses_ = st.presses;
        }
    }

    // ---- circle pad: walk in the game world; elsewhere (menus, the screens
    // before the game) it moves the cursor like the C-stick, and a flick
    // steps one cell.
    const bool walk_mode = in_game_ && !panel_open_;
    const float cvx = pad.circle_x / kStickMax, cvy = pad.circle_y / kStickMax;
    const float cm = std::sqrt(cvx * cvx + cvy * cvy);
    if (walk_mode) {
        walk(cvx, cvy, cm, aim.aiming, now_ms);
    } else {
        if (walking_ || walk_stop_) {   // the context changed under a held stick: stop walking
            walking_ = walk_press_pending_ = walk_left_ = false;
            walk_stop_ = 0;
        }
        // A flick's cell step: the stick moves the cursor again once
        // centred. The game menu: never (game_menu_flick).
        if (!flick_held_ && !game_menu_on_)
            stick_cursor(pad.circle_x, pad.circle_y, kCircleCursorSpeed, circle_acc_x_, circle_acc_y_);
        menu_flick(pad);
        scroll_button_check(held);
    }

    // ---- the mouse buttons, from all their sources ----
    const bool item_left = item_move_step(now_ms);
    if (aim.aiming) {
        move_cursor(aim.cursor_x, aim.cursor_y);
        a_left = aim.left;
        b_right = aim.right;
    } else if (aim.hold_plain_buttons) {
        a_left = b_right = false;   // the target is gone under a held button: no click onto its corpse
    }
    const bool menu_left = game_menu_buttons(now_ms);
    button(false, a_left || walk_left_ || tap_left || pad.point_left || item_left || menu_left);
    button(true, b_right || pad.point_right);
    prev_ = held;
}

void Controls::button(bool right, bool want)
{
    const int k = right ? 1 : 0;
    if (!want && button_on_[k] && pumps_ && down_pump_[k] == pump_)
        want = true;   // down and up in one pump: up at the next one (begin_pump)
    if (want && !button_on_[k])
        down_pump_[k] = pump_;
    button_on_[k] = want;
    rt::input_button(right, want);
}

}  // namespace game
