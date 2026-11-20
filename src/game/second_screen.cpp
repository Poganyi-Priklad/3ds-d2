// See second_screen.h.
#include "game/second_screen.h"

#include "game/aim_faq.h"
#include "game/aim_settings.h"
#include "game/automap.h"
#include "game/profiler.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace game {
namespace {

constexpr int kW = SecondScreen::kWidth, kBarH = SecondScreen::kBarH, kAreaH = SecondScreen::kAreaH;

constexpr uint8_t VK_TAB = 0x09, VK_RETURN = 0x0D, VK_MENU = 0x12, VK_SPACE = 0x20, VK_F1 = 0x70;

// Touch: a tap is short and nearly still; a long press is still for longer.
constexpr int kTapMs = 300, kTapSlop = 6, kLongMs = 500;
constexpr int kClickHoldMs = 120;   // a click holds the button this long (one 40 ms game frame is not enough)
constexpr int kSettleMs = 60;       // the cursor rests this long before a click: the game resolves a
                                    // click against what the cursor hovered in the previous frame
constexpr int kEdge = 16, kEdgeStep = 6;   // dragging this close to the area's edge scrolls this much per poll

// KEYS: four columns of five rows.
struct KeyButton {
    const char *label, *hint;
    uint8_t vk;
    bool latch;   // a tap turns it on, the next one off (Alt: items stay shown)
};
const KeyButton kKeys[SecondScreen::kKeyCount] = {
    {"Skill 1", "F1", VK_F1, false},          {"Skill 2", "F2", VK_F1 + 1, false},
    {"Skill 3", "F3", VK_F1 + 2, false},      {"Skill 4", "F4", VK_F1 + 3, false},
    {"Skill 5", "F5", VK_F1 + 4, false},      {"Skill 6", "F6", VK_F1 + 5, false},
    {"Skill 7", "F7", VK_F1 + 6, false},      {"Skill 8", "F8", VK_F1 + 7, false},
    {"Character", "C", 'C', false},           {"Inventory", "I", 'I', false},
    {"Skills", "T", 'T', false},              {"Quests", "Q", 'Q', false},
    {"Party", "P", 'P', false},               {"Merc", "O", 'O', false},
    {"Map", "Tab", VK_TAB, false},            {"Run/Walk", "R", 'R', false},
    {"Swap", "W", 'W', false},                {"Items", "Alt", VK_MENU, true},
    {"Chat", "Enter", VK_RETURN, false},      {"Close all", "Space", VK_SPACE, false},
};
constexpr int kKeyCols = 4, kKeyW = kW / kKeyCols, kKeyH = 44;

// HELP: the controls (docs/controls.md), a button and what it does. The
// font is code page 437: 0x18-0x1B are the arrows up, down, right, left.
struct HelpRow {
    const char *keys, *action;
};
const HelpRow kHelp[] = {
    {"Circle pad", "walk; cursor in menus"},
    {"C-stick", "move the cursor"},
    {"A / B", "left / right mouse button"},
    {"X / Y", "inv. / items; sell or drop"},
    {"L", "stand still (held)"},
    {"D-pad", "belt potions 1-4"},
    {"ZL", "weapon swap"},
    {"Start", "game menu, close panels"},
    {"Select", "top: whole frame / 1:1 zoom"},
    {"R+A / R+B", "character / skill tree"},
    {"R+X / R+Y", "automap / quests"},
    {"ZR+A/B/Y/X", "skills F1-F4, or R+D-pad"},
    {"R+Start", "Enter (confirm)"},
    {"R+Select", "on-screen keyboard"},
    {"L+R+Select", "screenshot to Camera album"},
    {"Sel+\x1b/\x1a", "previous / next tab"},
    {"Sel+\x18/\x19", "map / map size"},
    {"Touch", "touchpad on MAP and INFO"},
    {"Tap/hold", "L/R click in ZOOM and PANEL"},
    {"A,B,Start", "skip a cinematic"},
};
constexpr int kHelpRowH = 10;

// Panels: the UI table entries (d2_state.h) by the half of the frame they
// cover at 640x480. Entries not listed draw nothing over the world.
// Inventory 0x01, quest log 0x0F and game menu 0x09 were seen on the lane;
// the others are the community's names for the 1.13/1.14 UI table.
constexpr uint64_t bit(int i) { return 1ull << i; }
constexpr uint64_t kLeftPanels = bit(0x02)     // character sheet
                               | bit(0x0C)     // shop (with the inventory on the right)
                               | bit(0x0E)     // socketing (anvil)
                               | bit(0x0F)     // quest log
                               | bit(0x14)     // waypoints
                               | bit(0x16)     // party
                               | bit(0x17)     // trade
                               | bit(0x19)     // stash
                               | bit(0x1A)     // Horadric Cube
                               | bit(0x24);    // mercenary
constexpr uint64_t kRightPanels = bit(0x01)    // inventory
                                | bit(0x04);   // skill tree
// The game menu (Escape, 0x09) is left out: it fills the top screen,
// readable as it is.
constexpr uint64_t kCentrePanels = bit(0x08);  // NPC dialogue menu

inline uint16_t rgb(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

}  // namespace

const char *bottom_tab_name(BottomTab t)
{
    switch (t) {
    case BottomTab::Map: return "map";
    case BottomTab::Keys: return "keys";
    case BottomTab::Zoom: return "zoom";
    case BottomTab::Info: return "info";
    case BottomTab::Aim: return "aim";
    case BottomTab::Pad: return "pad";
    case BottomTab::Panel: return "panel";
    }
    return "map";
}

bool bottom_tab_from_name(const char *s, BottomTab &t)
{
    for (BottomTab c : {BottomTab::Map, BottomTab::Keys, BottomTab::Zoom, BottomTab::Info, BottomTab::Aim, BottomTab::Pad})
        if (std::strcmp(s, bottom_tab_name(c)) == 0) {
            t = c;
            return true;
        }
    return false;
}

void SecondScreen::set_tab(BottomTab t)
{
    if (t == BottomTab::Panel)
        return;   // not a choice to keep: it comes with the panels
    if (t != tab_)
        settings_changed_ = true;
    tab_ = t;
    dirty_ = true;
}

void SecondScreen::set_frame(int width, int height)
{
    if (width > 0 && height > 0) {
        fw_ = width;
        fh_ = height;
    }
}

BottomTab SecondScreen::shown_tab() const
{
    if (tab_ == BottomTab::Pad)
        return BottomTab::Pad;   // no panel takes the screen from PAD
    if (panel_sides_ && !panel_dismissed_)
        return BottomTab::Panel;
    return tab_;
}

BottomView::Kind SecondScreen::shown_kind() const
{
    if (osk_ && osk_->is_open())
        return BottomView::kKeyboard;
    if (video_ && (screen_auto_ & kAutoVideo))
        return BottomView::kVideo;
    switch (shown_tab()) {
    case BottomTab::Map: return help_ ? BottomView::kHelp : BottomView::kMap;
    case BottomTab::Keys: return BottomView::kKeys;
    case BottomTab::Zoom:
    case BottomTab::Panel: return BottomView::kViewport;
    case BottomTab::Info: return BottomView::kInfo;
    case BottomTab::Aim: return BottomView::kAim;
    case BottomTab::Pad: return BottomView::kPad;
    }
    return BottomView::kInfo;
}

bool SecondScreen::wants_automap() const { return gs_.in_game && shown_kind() == BottomView::kMap; }

void SecondScreen::step_tab(int dir)
{
    if (shown_kind() == BottomView::kKeyboard)
        return;
    const int n = panel_sides_ ? 7 : 6;
    const BottomTab from = shown_tab(), to = (BottomTab)(((int)from + dir + n) % n);
    if (from == BottomTab::Pad)
        story_.reset();
    if (to == BottomTab::Pad && from != BottomTab::Pad)
        pad_back_ = from == BottomTab::Panel ? tab_ : from;
    if (to == BottomTab::Panel) {
        if (tab_ == BottomTab::Pad)
            set_tab(pad_back_);
        panel_dismissed_ = false;
    } else {
        panel_dismissed_ = panel_sides_ != 0;
        set_tab(to);
    }
    dirty_ = true;
}

void SecondScreen::show_map()
{
    if (shown_kind() == BottomView::kKeyboard)
        return;
    if (tab_ == BottomTab::Pad)
        story_.reset();
    panel_dismissed_ = panel_sides_ != 0;
    set_tab(BottomTab::Map);
    set_map_help(false);
    dirty_ = true;
}

void SecondScreen::set_map_help(bool on)
{
    if (on != help_)
        settings_changed_ = true;
    help_ = on;
    dirty_ = true;
}

void SecondScreen::set_profile_choice(uint32_t bits)
{
    bits &= kProfileAll;
    if (bits != profile_)
        settings_changed_ = true;
    profile_ = bits;
    dirty_ = true;
}

void SecondScreen::set_profiling(bool on)
{
    profiling_ = on && profile_ != 0;
    dirty_ = true;
}

void SecondScreen::set_port_options(uint32_t bits)
{
    bits &= kPortAll;
    if (bits != port_)
        settings_changed_ = true;
    port_ = bits;
    dirty_ = true;
}

void SecondScreen::set_screen_auto(uint32_t bits)
{
    bits &= kScreenAutoAll;
    if (bits != screen_auto_)
        settings_changed_ = true;
    screen_auto_ = bits;
    dirty_ = true;
}

void SecondScreen::set_trace_writes(TraceWrites m)
{
    if (m != trace_writes_)
        settings_changed_ = true;
    trace_writes_ = m;
    dirty_ = true;
}

void SecondScreen::request(Controls::Bottom r)
{
    switch (r) {
    case Controls::Bottom::kNextTab: step_tab(1); break;
    case Controls::Bottom::kPrevTab: step_tab(-1); break;
    case Controls::Bottom::kMap: show_map(); break;
    case Controls::Bottom::kMapSize: switch_map_size(); break;
    case Controls::Bottom::kNone: break;
    }
}

void SecondScreen::switch_map_size()
{
    if (shown_kind() == BottomView::kKeyboard)
        return;
    if (shown_tab() != BottomTab::Map || help_)
        show_map();
    set_map_scale(map_scale_ == 1 ? 2 : 1);
    settings_changed_ = true;
}

void SecondScreen::set_map_ms(uint32_t ms)
{
    // The nearest of the choices.
    uint32_t best = kMapMs[0];
    for (uint32_t c : kMapMs)
        if ((c > ms ? c - ms : ms - c) < (best > ms ? best - ms : ms - best))
            best = c;
    map_ms_ = best;
    dirty_ = true;
}

// MAP's button under (x, y): 0 size, 1 refresh, 2 HELP, -1 none. Size and
// refresh are there only over the map in the game.
static int map_button(int x, int y, bool map)
{
    if (y < SecondScreen::kMapBtnY)
        return -1;
    if (x >= SecondScreen::kHelpBtnX && x < SecondScreen::kHelpBtnX + SecondScreen::kHelpBtnW)
        return 2;
    if (!map)
        return -1;
    if (x >= SecondScreen::kScaleBtnX && x < SecondScreen::kScaleBtnX + SecondScreen::kScaleBtnW)
        return 0;
    if (x >= SecondScreen::kMsBtnX && x < SecondScreen::kMsBtnX + SecondScreen::kMsBtnW)
        return 1;
    return -1;
}

// Which halves hold a panel, and which one to show.
void SecondScreen::panel_geometry()
{
    const uint64_t ui = gs_.in_game ? gs_.ui : 0;
    uint32_t sides = 0;
    if (ui & kLeftPanels)
        sides |= 1;
    if (ui & kRightPanels)
        sides |= 2;
    if (!sides && (ui & kCentrePanels))
        sides |= 4;
    const uint64_t panels = ui & (kLeftPanels | kRightPanels | kCentrePanels);
    if (panels != panel_ui_) {
        // A panel opened or closed: show the panels again (unless all closed),
        // as far as the OPTIONS page's SCREEN switches allow; one they don't
        // leaves the tab shown as it was (PANEL offered on the bar).
        const uint64_t opened = panels & ~panel_ui_;
        const bool side_opened = (opened & (kLeftPanels | kRightPanels)) != 0;
        const bool menu_opened = (opened & kCentrePanels) != 0;
        if ((side_opened && (screen_auto_ & kAutoPanel)) || (menu_opened && (screen_auto_ & kAutoNpcMenu)))
            panel_dismissed_ = false;
        else if (opened && !panel_ui_)
            panel_dismissed_ = true;
        panel_ui_ = panels;
        dirty_ = true;
    }
    if (sides != panel_sides_) {
        panel_sides_ = sides;
        dirty_ = true;
    }
    if (sides & 4)
        side_ = 2;
    else if (sides == 1)
        side_ = 0;
    else if (sides == 2)
        side_ = 1;
    else if (sides == 3 && grip_ != Grip::Point && ((screen_auto_ & kAutoPanelSide) || side_ == 2))
        side_ = cx_ < fw_ / 2 ? 0 : 1;   // both: the cursor's half (or kept, the SCREEN switch off)
}

// The viewport, while nothing holds it: ZOOM centres the cursor; PANEL keeps
// its half and scrolls only to keep the cursor in view.
void SecondScreen::follow_cursor()
{
    if (shown_tab() == BottomTab::Panel) {
        const int panel_h = fh_ - 48;   // the panels end at the control panel
        scroll_h_ = panel_h;
        vx_ = side_ == 0 ? 0 : side_ == 1 ? fw_ - kW : (fw_ - kW) / 2;
        if (grip_ != Grip::Point) {
            if (cy_ < vy_ + 24)
                vy_ = cy_ - 24;
            else if (cy_ > vy_ + kAreaH - 24)
                vy_ = cy_ - kAreaH + 24;
        }
        vy_ = clampi(vy_, 0, panel_h > kAreaH ? panel_h - kAreaH : 0);
    } else {
        scroll_h_ = 0;
        if (grip_ != Grip::Point) {
            vx_ = cx_ - kW / 2;
            vy_ = cy_ - kAreaH / 2;
        }
        vx_ = clampi(vx_, 0, fw_ > kW ? fw_ - kW : 0);
        vy_ = clampi(vy_, 0, fh_ > kAreaH ? fh_ - kAreaH : 0);
    }
}

void SecondScreen::tab_tapped(int x, Pad &pad)
{
    const int n = panel_sides_ ? 7 : 6;
    const int i = clampi(x * n / kW, 0, n - 1);
    const BottomTab hit = (BottomTab)i;
    if (hit == shown_tab()) {
        // The shown tab again: its second function.
        if (hit == BottomTab::Map && help_) {
            set_map_help(false);   // HELP: back to the map
        } else if (hit == BottomTab::Map) {
            set_map_scale(map_scale_ == 1 ? 2 : 1);
            settings_changed_ = true;
        } else if (hit == BottomTab::Panel && panel_sides_ == 3) {
            // Panels on both sides: the cursor crosses to the same spot on the other.
            pad.pointing = true;
            pad.point_x = clampi(side_ == 0 ? cx_ + fw_ / 2 : cx_ - fw_ / 2, 0, fw_ - 1);
            pad.point_y = cy_;
            side_ ^= 1;
        }
        dirty_ = true;
        return;
    }
    if (hit == BottomTab::Pad) {
        pad_back_ = shown_tab() == BottomTab::Panel ? tab_ : shown_tab();
        set_tab(BottomTab::Pad);
    } else if (hit == BottomTab::Panel) {
        panel_dismissed_ = false;
    } else {
        panel_dismissed_ = panel_sides_ != 0;
        set_tab(hit);
    }
    dirty_ = true;
}

void SecondScreen::update(Pad &pad, const GameState &gs, int cursor_x, int cursor_y, uint64_t now_ms)
{
    if (gs.in_game != gs_.in_game)
        dirty_ = true;   // into or out of a game: the map or its note again
    gs_ = gs;
    cx_ = cursor_x;
    cy_ = cursor_y;
    panel_geometry();
    const BottomView::Kind kind = shown_kind();
    const bool down = pad.touching && !prev_touch_, up = !pad.touching && prev_touch_;
    prev_touch_ = pad.touching;

    if (kind == BottomView::kKeyboard || kind == BottomView::kVideo || pad.touchpad) {
        // The keyboard has the touch screen (the platform gives it the
        // touch); a scripted tap is the touchpad's, and so is any touch
        // during a video (a tap skips it).
        if (grip_ == Grip::Key || grip_ == Grip::Point)
            grip_ = Grip::None;
    } else {
        if (down) {
            tx0_ = pad.touch_x;
            ty0_ = pad.touch_y;
            t0_ = moved_at_ = now_ms;
            moved_ = 0;
            long_fired_ = scrolled_ = false;
            if (kind == BottomView::kPad) {
                pad_touch(pad, true, now_ms);
            } else if (pad.touch_y < kBarH) {
                grip_ = Grip::Bar;
                tab_tapped(pad.touch_x, pad);
            } else if (kind == BottomView::kKeys) {
                const int col = clampi(pad.touch_x / kKeyW, 0, kKeyCols - 1);
                const int row = clampi((pad.touch_y - kBarH) / kKeyH, 0, kKeyCount / kKeyCols - 1);
                grip_ = Grip::Key;
                key_ = row * kKeyCols + col;
                if (kKeys[key_].latch)
                    alt_latched_ = !alt_latched_;
                dirty_ = true;
            } else if (kind == BottomView::kViewport && gs_.in_game) {
                grip_ = Grip::Point;
            } else if ((kind == BottomView::kMap || kind == BottomView::kHelp) &&
                       map_button(pad.touch_x, pad.touch_y, kind == BottomView::kMap && gs_.in_game) >= 0) {
                grip_ = Grip::MapButton;
                key_ = map_button(pad.touch_x, pad.touch_y, kind == BottomView::kMap && gs_.in_game);
            } else if (kind == BottomView::kAim && pad.touch_y >= kMapBtnY) {
                grip_ = Grip::MapButton;   // AIM's pages: SETTINGS, FAQ
                key_ = pad.touch_x < kAimFaqBtnX ? 300 : 301;
            } else if (kind == BottomView::kAim && aim_faq_) {
                grip_ = Grip::Scroll;      // the FAQ follows the finger
                aim_faq_grab_top_ = aim_faq_top_;
            } else if (kind == BottomView::kAim) {
                // A SETTINGS row's "-" or "+" (key 200 + row * 2, + 1 for "+").
                const int row = (pad.touch_y - kAimRowY) / kAimRowH;
                const bool on_row = pad.touch_y >= kAimRowY && row < kAimSettingRows;
                grip_ = Grip::MapButton;
                key_ = !on_row                                                   ? kKeyAimNothing
                       : pad.touch_x >= kAimMinusX && pad.touch_x < kAimValueX ? 200 + row * 2
                       : pad.touch_x >= kAimPlusX                               ? 201 + row * 2
                                                                                 : kKeyAimNothing;
            } else if (kind == BottomView::kInfo && pad.touch_y >= kMapBtnY && !checklist_ && !options_) {
                // INFO's own buttons: the profiler run, the OPTIONS page, the report.
                grip_ = Grip::MapButton;
                // OPTIONS is 99: 5 and up are the checklist's rows, and its third
                // (5 + 2) used to open OPTIONS instead of ticking the file-read trace.
                key_ = pad.touch_x < kInfoBtnX1 ? 4 : pad.touch_x < kInfoBtnX2 ? kKeyOptions : 3;
            } else if (kind == BottomView::kInfo && pad.touch_y >= kMapBtnY && pad.touch_x >= kReportBtnX) {
                grip_ = Grip::MapButton;
                key_ = 3;   // the checklist's and the OPTIONS page's way back
            } else if (kind == BottomView::kInfo && pad.touch_y >= kMapBtnY && pad.touch_x >= kProfBtnX &&
                       pad.touch_x < kProfBtnX + kProfBtnW) {
                grip_ = Grip::MapButton;
                key_ = 4;   // the checklist's start/stop
            } else if (kind == BottomView::kInfo && checklist_ && pad.touch_y >= kProfRowY &&
                       pad.touch_y < kProfRowY + kProfRows * kProfRowH) {
                grip_ = Grip::MapButton;
                key_ = 5 + (pad.touch_y - kProfRowY) / kProfRowH;   // a row of the checklist
            } else if (kind == BottomView::kInfo && options_ && pad.touch_y >= kOptRowY &&
                       pad.touch_y < kOptRowY + (options_screen_ ? kScreenAutoCount : kPortOptionCount) * kOptRowH) {
                grip_ = Grip::MapButton;
                key_ = 100 + (pad.touch_y - kOptRowY) / kOptRowH;   // a row of the OPTIONS page (or SCREEN)
            } else {
                grip_ = Grip::Pad;
            }
        }
        if (pad.touching && grip_ != Grip::None) {
            const int d = std::abs(pad.touch_x - tx0_) + std::abs(pad.touch_y - ty0_);
            if (d > moved_)
                moved_ = d;
        }
        if (kind == BottomView::kPad && !down)
            pad_touch(pad, false, now_ms);
        if (grip_ == Grip::Scroll && pad.touching) {
            const int top = clampi(aim_faq_grab_top_ - (pad.touch_y - ty0_) / kAimFaqRowH, 0,
                                   std::max(0, kAimFaqLines - kAimFaqRows));
            if (top != aim_faq_top_) {
                aim_faq_top_ = top;
                dirty_ = true;
            }
        }
        if (grip_ == Grip::Point && pad.touching) {
            // Resting at the edge scrolls (the spot under the finger moves on),
            // once the touch is too long for a tap: a quick tap there clicks.
            const int ay = pad.touch_y - kBarH, vx0 = vx_, vy0 = vy_;
            const bool hold = now_ms - t0_ >= (uint64_t)kTapMs;
            if (hold && ay < kEdge)
                vy_ -= kEdgeStep;
            else if (hold && ay >= kAreaH - kEdge)
                vy_ += kEdgeStep;
            if (hold && shown_tab() == BottomTab::Zoom) {
                if (pad.touch_x < kEdge)
                    vx_ -= kEdgeStep;
                else if (pad.touch_x >= kW - kEdge)
                    vx_ += kEdgeStep;
            }
            follow_cursor();   // clamps
            scrolled_ |= vx_ != vx0 || vy_ != vy0;   // holding at the edge to scroll is not a long press
            const int x = clampi(vx_ + pad.touch_x, 0, fw_ - 1), y = clampi(vy_ + ay, 0, fh_ - 1);
            if (x != px_ || y != py_)
                moved_at_ = now_ms;
            px_ = x;
            py_ = y;
            if (!long_fired_ && !scrolled_ && moved_ <= kTapSlop && now_ms - t0_ >= (uint64_t)kLongMs) {
                long_fired_ = true;   // a long press: right click
                right_until_ = now_ms + kClickHoldMs;
            }
        }
        if (up && grip_ == Grip::Point && !long_fired_ && !scrolled_ && moved_ <= kTapSlop && now_ms - t0_ < (uint64_t)kTapMs) {
            left_from_ = moved_at_ + kSettleMs > now_ms ? moved_at_ + kSettleMs : now_ms;
            left_until_ = left_from_ + kClickHoldMs;
        }
        if (up && grip_ == Grip::Key)
            dirty_ = true;
        if (up && grip_ == Grip::MapButton && moved_ <= kTapSlop) {
            // A tap: the size switches; the refresh steps through its
            // choices; the corner one switches between the map and HELP.
            // INFO's ask for a report, or take the request back; its
            // profiler run's checklist, where the ticks change only between
            // runs, and a run starts only with something ticked.
            if (key_ == 300 || key_ == 301) {
                aim_faq_ = key_ == 301;
                dirty_ = true;
            } else if (key_ == kKeyAimNothing) {
                // a SETTINGS row's label or value: nothing
            } else if (key_ >= 200) {
                // AIM's "-" or "+": applied at once, saved like the other settings.
                if (aim_setting_step(aim_, (key_ - 200) / 2, key_ & 1 ? 1 : -1)) {
                    aim_changed_ = true;
                    settings_changed_ = true;
                    dirty_ = true;
                }
            } else if (key_ == 3 && (checklist_ || options_)) {
                checklist_ = options_ = options_screen_ = false;
                dirty_ = true;
            } else if (key_ == kKeyOptions) {
                options_ = true;
                dirty_ = true;
            } else if (key_ >= 100 && options_screen_) {
                set_screen_auto(screen_auto_ ^ kScreenAutos[key_ - 100].bit);   // at once
            } else if (key_ >= 100) {
                set_port_options(port_option_tap(port_, kPortOptions[key_ - 100]));   // at once
            } else if (key_ == 4 && options_) {
                options_screen_ = !options_screen_;   // the left-hand button: OPTIONS <-> SCREEN
                dirty_ = true;
            } else if (key_ == 3) {
                report_ = !report_;
                dirty_ = true;
            } else if (key_ == 4 && !checklist_) {
                checklist_ = true;
                dirty_ = true;
            } else if (key_ == 4) {
                profiling_ = !profiling_ && profile_ != 0;
                dirty_ = true;
            } else if (key_ == 5 + kProfileOptionCount) {
                set_trace_writes(next_trace_writes(trace_writes_));   // a run on or not
            } else if (key_ >= 5) {
                if (!profiling_)
                    set_profile_choice(profile_ ^ kProfileOptions[key_ - 5].bit);
            } else if (key_ == 2) {
                set_map_help(!help_);
            } else if (key_ == 0) {
                set_map_scale(map_scale_ == 1 ? 2 : 1);
            } else {
                size_t i = 0;
                while (i < sizeof kMapMs / sizeof kMapMs[0] && kMapMs[i] != map_ms_)
                    i++;
                set_map_ms(kMapMs[(i + 1) % (sizeof kMapMs / sizeof kMapMs[0])]);
            }
            settings_changed_ |= key_ < 3;
        }
        if (up)
            grip_ = Grip::None;
    }

    // What the touch does instead of the touchpad.
    const bool clicking = now_ms < left_until_ || now_ms < right_until_;
    if (grip_ == Grip::Point || clicking) {
        pad.pointing = true;
        pad.point_x = px_;
        pad.point_y = py_;
        pad.point_left = now_ms >= left_from_ && now_ms < left_until_;
        pad.point_right = now_ms < right_until_;
    }
    int nk = 0;
    if (grip_ == Grip::Key && pad.touching && !kKeys[key_].latch)
        pad.keys[nk++] = kKeys[key_].vk;
    if (alt_latched_ && gs_.in_game && kind != BottomView::kKeyboard && kind != BottomView::kVideo)
        pad.keys[nk++] = VK_MENU;
    if (grip_ == Grip::Bar || grip_ == Grip::Key || grip_ == Grip::Point || grip_ == Grip::MapButton ||
        grip_ == Grip::Scroll || grip_ == Grip::PadClose || grip_ == Grip::PadUp || grip_ == Grip::PadDown ||
        grip_ == Grip::PadThumb)
        pad.touching = false;
    if (grip_ != Grip::Point)
        follow_cursor();
}

void SecondScreen::close_pad()
{
    if (tab_ != BottomTab::Pad)
        return;
    story_.reset();
    // Back to the tab shown before, with any open panel offered on the bar
    // (not switched to: it was opened while PAD showed).
    set_tab(pad_back_);
    panel_dismissed_ = panel_sides_ != 0;
    dirty_ = true;
}

void SecondScreen::set_pad_game(const PadGame &g, uint64_t now_ms)
{
    story_.step(g, now_ms);
}

void SecondScreen::set_pad_art(const PadArt *art)
{
    pad_art_ = art;
    if (art)
        story_.set_measure([art](const std::u16string &s) { return art->text_font.width(s); });
    else
        story_.set_measure([](const std::u16string &s) { return 8 * (int)s.size(); });
    dirty_ = true;
}

// PAD's touch: its close button and the story's scroll column; anything
// else is the touchpad's (Grip::Pad). `down`: the finger just touched.
void SecondScreen::pad_touch(Pad &pad, bool down, uint64_t now_ms)
{
    namespace L = pad_layout;
    const int x = pad.touch_x, y = pad.touch_y;
    const bool story = story_.mode() == PadFrame::kStory;
    auto in_close = [](int tx, int ty) {
        return tx >= L::kCloseX && tx < L::kCloseX + L::kCloseSize && ty >= L::kCloseY && ty < L::kCloseY + L::kCloseSize;
    };
    // The scroll column's touch area: the drawn 12-pixel column and the empty
    // strips beside it, from the story box's right edge to the screen's, so
    // a finger a little off the narrow bar still takes it.
    const bool in_column = x >= L::kBoxX1 && x < kWidth && y >= L::kBoxY0 && y < L::kBoxY1;
    constexpr uint64_t kFirstRepeatMs = 350, kRepeatMs = 110;
    if (down) {
        int ty = 0, th = 0;
        L::thumb(story_.top(), story_.total(), ty, th);
        if (in_close(x, y)) {
            grip_ = Grip::PadClose;
            pad_close_inside_ = true;
        } else if (story && in_column && y < L::kTrackY0) {
            grip_ = Grip::PadUp;
            story_.scroll_by(-1);
            pad_repeat_at_ = now_ms + kFirstRepeatMs;
        } else if (story && in_column && y >= L::kTrackY1) {
            grip_ = Grip::PadDown;
            story_.scroll_by(1);
            pad_repeat_at_ = now_ms + kFirstRepeatMs;
        } else if (story && in_column && y >= ty && y < ty + th) {
            grip_ = Grip::PadThumb;   // the thumb follows the finger, held where it was taken
            pad_grab_dy_ = y - ty;
        } else if (story && in_column) {
            // The track: the thumb jumps there, centred on the finger, and
            // is then held by its middle, so the same touch drags on.
            grip_ = Grip::PadThumb;
            pad_grab_dy_ = th / 2;
            const int span = (L::kTrackY1 - L::kTrackY0) - th;
            if (span > 0)
                story_.scroll_to_fraction((float)(y - pad_grab_dy_ - L::kTrackY0) / (float)span);
        } else {
            grip_ = Grip::Pad;
        }
        dirty_ = true;
        return;
    }
    if (!pad.touching) {
        if (grip_ == Grip::PadClose && pad_close_inside_)
            close_pad();   // let go on the button: closed (slid off it first: nothing)
        if (grip_ >= Grip::PadClose)
            dirty_ = true;
        return;
    }
    if (grip_ == Grip::PadClose) {
        pad_close_inside_ = in_close(x, y);
        dirty_ = true;
    } else if ((grip_ == Grip::PadUp || grip_ == Grip::PadDown) && now_ms >= pad_repeat_at_) {
        story_.scroll_by(grip_ == Grip::PadUp ? -1 : 1);
        pad_repeat_at_ = now_ms + kRepeatMs;
    } else if (grip_ == Grip::PadThumb) {
        int ty = 0, th = 0;
        L::thumb(story_.top(), story_.total(), ty, th);
        const int span = (L::kTrackY1 - L::kTrackY0) - th;
        if (span > 0)
            story_.scroll_to_fraction((float)(y - pad_grab_dy_ - L::kTrackY0) / (float)span);
    }
}

void SecondScreen::set_stats(std::vector<BottomView::Line> lines)
{
    stats_ = std::move(lines);
    if (shown_kind() == BottomView::kInfo || shown_kind() == BottomView::kKeyboard)
        dirty_ = true;
}

void SecondScreen::set_video(const char *file, uint32_t at_ms, uint32_t length_ms)
{
    if (!video_ || video_file_ != (file ? file : "")) {
        video_file_ = file ? file : "";
        video_title_ = video_title(video_file_);
        dirty_ = true;
    }
    video_ = true;
    video_s_ = at_ms / 1000;
    video_length_s_ = length_ms / 1000;
}

void SecondScreen::end_video()
{
    if (!video_)
        return;
    video_ = false;
    last_video_s_ = UINT32_MAX;
    dirty_ = true;   // the tab as it was, with the game's next frame
}

std::string video_title(const std::string &file)
{
    // The name without its folders, extension and size ("640x292").
    std::string s = file.substr(file.find_last_of("\\/") + 1);
    if (const size_t dot = s.rfind('.'); dot != std::string::npos)
        s.erase(dot);
    size_t size = s.find_last_not_of("0123456789x") + 1;
    while (size < s.size() && s[size] == 'x')
        size++;
    if (s.find('x', size) != std::string::npos)
        s.erase(size);
    while (!s.empty() && s.back() == '_')
        s.pop_back();
    std::string key = s;
    for (char &c : key)
        c = (char)std::tolower((unsigned char)c);
    static const struct { const char *key, *title; } kTitles[] = {
        {"new_bliz", "Blizzard Entertainment"},
        {"bliznorth", "Blizzard North"},
        {"d2intro", "Diablo II: the intro"},
        {"act02start", "Act II"},
        {"act03start", "Act III"},
        {"act04start", "Act IV"},
        {"act04end", "Diablo II: the ending"},
        {"d2x_intro", "Lord of Destruction: the intro"},
        {"d2x_out", "Lord of Destruction: the ending"},
    };
    for (const auto &t : kTitles)
        if (key == t.key)
            return t.title;
    return s.empty() ? std::string("A cinematic") : s;
}

void SecondScreen::log(const char *s)
{
    // A ring of 40-column lines; longer messages are cut, as the console did.
    char *line = log_[log_head_ % kLogLines];
    std::snprintf(line, 41, "%s", s);
    log_head_++;
}

bool SecondScreen::stage(BottomView &v, uint64_t now_ms)
{
    presents_++;
    const BottomView::Kind kind = shown_kind();
    const BottomTab tab = shown_tab();
    bool redraw = dirty_ || kind != last_kind_ || tab != last_tab_ || (panel_sides_ != 0) != last_panel_tab_;
    v.kind = kind;
    v.tab = tab;
    v.panel_tab = panel_sides_ != 0;
    v.note.clear();
    v.lines.clear();
    v.osk_keys.clear();
    switch (kind) {
    case BottomView::kMap: {
        v.map_scale = map_scale_;
        v.map_ms = map_ms_;
        const uint64_t state = gs_.in_game ? automap_state(map_scale_) : 0;
        if (!gs_.in_game) {
            v.map.clear();
            v.note = "The map shows here in the game.";
        } else if (state && (state != last_map_state_ || redraw || map_scale_ != last_scale_ || v.map.empty())) {
            // The player moved a pixel (at this size), new units, or the
            // screen was taken away: the map again, around the player.
            v.map.assign((size_t)kW * kAreaH, 0);
            if (automap_draw(v.map.data(), kW, kAreaH, map_scale_)) {
                last_map_state_ = state;
                last_scale_ = map_scale_;
                redraw = true;
            } else {
                v.map.clear();
            }
        }
        if (gs_.in_game && v.map.empty())
            v.note = "Drawing the map...";
        break;
    }
    case BottomView::kHelp:
        break;   // nothing changes: drawn when shown
    case BottomView::kKeys: {
        uint32_t lit = 0;
        if (grip_ == Grip::Key && key_ >= 0)
            lit |= 1u << key_;
        for (int i = 0; i < kKeyCount; i++)
            if (kKeys[i].latch && alt_latched_)
                lit |= 1u << i;
        redraw |= lit != last_keys_;
        v.keys_lit = last_keys_ = lit;
        break;
    }
    case BottomView::kViewport:
        v.vx = vx_;
        v.vy = vy_;
        v.scroll_h = scroll_h_;
        // The frame changes all the time: every other present is enough.
        redraw |= vx_ != last_vx_ || vy_ != last_vy_ || (presents_ & 1) == 0;
        last_vx_ = vx_;
        last_vy_ = vy_;
        break;
    case BottomView::kInfo:
    case BottomView::kKeyboard: {
        v.lines = stats_;
        if (kind == BottomView::kInfo) {
            // The newest messages under the statistics, as many as fit
            // above the report button.
            v.report = report_;
            v.profiling = profiling_;
            v.checklist = checklist_;
            v.profile = profile_;
            v.trace_writes = trace_writes_;
            v.options = options_;
            v.port = port_;
            v.options_screen = options_screen_;
            v.screen_auto = screen_auto_;
            const int rows = (kAreaH - kMapBtnH) / 8 - (int)stats_.size();
            const uint32_t n = log_head_ < (uint32_t)kLogLines ? log_head_ : (uint32_t)kLogLines;
            const uint32_t show = rows > 0 ? ((uint32_t)rows < n ? (uint32_t)rows : n) : 0;
            for (uint32_t i = log_head_ - show; i < log_head_; i++)
                v.lines.push_back({log_[i % kLogLines], rgb(200, 200, 200)});
            // New messages: at most four redraws a second.
            if (log_head_ != last_log_head_ && now_ms - log_drawn_ms_ >= 250) {
                redraw = true;
                last_log_head_ = log_head_;
                log_drawn_ms_ = now_ms;
            }
        } else {
            v.osk_title = osk_->title();
            v.osk_keys = osk_->keys();
            redraw |= osk_->dirty();
        }
        break;
    }
    case BottomView::kAim:
        v.aim = aim_;
        v.aim_faq = aim_faq_;
        v.aim_faq_top = aim_faq_top_;
        redraw |= aim_faq_top_ != last_aim_faq_top_;
        last_aim_faq_top_ = aim_faq_top_;
        break;
    case BottomView::kVideo:
        // Redrawn as the time shown changes: once a second.
        v.video_title = video_title_;
        v.video_s = video_s_;
        v.video_length_s = video_length_s_;
        redraw |= video_s_ != last_video_s_;
        last_video_s_ = video_s_;
        break;
    case BottomView::kPad: {
        story_.frame(v.pad);
        const bool touching = prev_touch_;
        v.pad.close_pressed = grip_ == Grip::PadClose && touching && pad_close_inside_;
        v.pad.up_pressed = grip_ == Grip::PadUp && touching;
        v.pad.down_pressed = grip_ == Grip::PadDown && touching;
        v.pad_art = pad_art_;
        redraw |= v.pad != last_pad_;
        last_pad_ = v.pad;
        break;
    }
    case BottomView::kNone:
        break;
    }
    last_kind_ = kind;
    last_tab_ = tab;
    last_panel_tab_ = panel_sides_ != 0;
    dirty_ = false;
    return redraw;
}

// ---- drawing ----------------------------------------------------------------------

namespace {

struct Canvas {
    uint16_t *fb;
    const uint8_t *font;
    void px(int x, int y, uint16_t c) const
    {
        if (x >= 0 && x < kW && y >= 0 && y < SecondScreen::kHeight)
            fb[x * 240 + (239 - y)] = c;
    }
    void fill(int x, int y, int w, int h, uint16_t c) const
    {
        const int x0 = clampi(x, 0, kW), x1 = clampi(x + w, 0, kW);
        const int y0 = clampi(y, 0, SecondScreen::kHeight), y1 = clampi(y + h, 0, SecondScreen::kHeight);
        for (int i = x0; i < x1; i++) {
            uint16_t *col = fb + i * 240 + (239 - y0);
            for (int j = y0; j < y1; j++)
                *col-- = c;
        }
    }
    void text(int x, int y, const char *s, uint16_t c) const
    {
        for (; *s; s++, x += 8) {
            const uint8_t *g = font + (uint8_t)*s * 8;
            for (int r = 0; r < 8; r++)
                for (int b = 0; b < 8; b++)
                    if (g[r] & (0x80 >> b))
                        px(x + b, y + r, c);
        }
    }
    void centred(int x, int y, int w, const char *s, uint16_t c) const
    {
        text(x + (w - 8 * (int)std::strlen(s)) / 2, y, s, c);
    }
};

const uint16_t kBg = 0, kBarBg = rgb(40, 36, 30), kBarText = rgb(170, 160, 140), kGold = rgb(199, 179, 119);
const uint16_t kKeyBg = rgb(58, 52, 44), kKeyLit = rgb(199, 179, 119), kHint = rgb(130, 120, 100);

void draw_bar(const Canvas &cv, const BottomView &v)
{
    static const char *const kNames[] = {"MAP", "KEYS", "ZOOM", "INFO", "AIM", "PAD", "PANEL"};
    const int n = v.panel_tab ? 7 : 6, w = kW / n;
    cv.fill(0, 0, kW, kBarH, kBarBg);
    for (int i = 0; i < n; i++) {
        const bool on = (int)v.tab == i;
        const int x = i * w;
        if (on)
            cv.fill(x + 1, 1, w - 2, kBarH - 3, kGold);
        const char *name = i == 0 && v.kind == BottomView::kHelp ? "HELP" : kNames[i];
        cv.centred(x, 5, w, name, on ? kBg : kBarText);
        if (i)
            cv.fill(x, 3, 1, kBarH - 7, kHint);
    }
    cv.fill(0, kBarH - 1, kW, 1, kHint);
}

// A button in the strip under the map.
void draw_map_button(const Canvas &cv, int x, int w, const char *label)
{
    const int y = SecondScreen::kMapBtnY, h = SecondScreen::kMapBtnH;
    cv.fill(x + 1, y + 1, w - 2, h - 2, kBarBg);
    cv.fill(x + 1, y, w - 2, 1, kHint);
    cv.centred(x, y + 4, w, label, kBarText);
}

}  // namespace

void draw_bottom(uint16_t *fb, const BottomView &v, const uint8_t *frame, int fw, int fh, const uint16_t *pal565,
                 const uint8_t *font)
{
    const Canvas cv{fb, font};
    if (v.kind == BottomView::kNone)
        return;
    if (v.kind == BottomView::kKeyboard) {
        cv.fill(0, 0, kW, SecondScreen::kHeight, kBg);
        for (size_t i = 0; i < v.lines.size() && i < 12; i++)
            cv.text(0, (int)i * 8, v.lines[i].text.c_str(), v.lines[i].color);
        const int y0 = SecondScreen::kOskY;
        const uint16_t key = rgb(200, 200, 200), down = rgb(80, 80, 80);
        cv.text(0, y0 + 2, v.osk_title.c_str(), rgb(255, 255, 255));
        for (const Osk::KeyView &k : v.osk_keys) {
            cv.fill(k.x, y0 + k.y, k.w, k.h, k.pressed ? down : key);
            cv.text(k.x + (k.w - 8 * (int)k.label.size()) / 2, y0 + k.y + (k.h - 8) / 2, k.label.c_str(),
                    k.pressed ? rgb(255, 255, 255) : 0);
        }
        return;
    }
    if (v.kind == BottomView::kVideo) {
        // The whole screen: no tabs to tap while the touch screen skips.
        const int h = 112, y = (SecondScreen::kHeight - h) / 2, bx = 40, bw = kW - 2 * bx;
        cv.fill(0, 0, kW, SecondScreen::kHeight, kBg);
        cv.fill(0, y, kW, h, kBarBg);
        cv.fill(0, y, kW, 1, kHint);
        cv.fill(0, y + h - 1, kW, 1, kHint);
        cv.centred(0, y + 14, kW, "CINEMATIC", kHint);
        cv.centred(0, y + 30, kW, v.video_title.c_str(), kGold);
        const uint32_t len = v.video_length_s, at = std::min(v.video_s, len);
        cv.fill(bx, y + 52, bw, 6, kBg);
        if (len)
            cv.fill(bx, y + 52, (int)(bw * at / len), 6, kGold);
        char time[32];
        std::snprintf(time, sizeof time, "%u:%02u / %u:%02u", (unsigned)(at / 60), (unsigned)(at % 60),
                      (unsigned)(len / 60), (unsigned)(len % 60));
        cv.centred(0, y + 66, kW, time, kBarText);
        cv.centred(0, y + 90, kW, "A, B, Start or a tap: skip", kHint);
        return;
    }
    if (v.kind == BottomView::kPad) {
        // The whole screen, no tabs: the game's art, or plainly without it.
        if (v.pad_art) {
            pad_draw(fb, *v.pad_art, v.pad);
            return;
        }
        namespace L = pad_layout;
        cv.fill(0, 0, kW, SecondScreen::kHeight, kBg);
        cv.fill(0, 0, kW, 2, kGold);
        cv.fill(0, SecondScreen::kHeight - 2, kW, 2, kGold);
        cv.fill(0, 0, 2, SecondScreen::kHeight, kGold);
        cv.fill(kW - 2, 0, 2, SecondScreen::kHeight, kGold);
        // Latin letters only in this font: others show as '?'.
        auto ascii = [](const std::u16string &s) {
            std::string o;
            for (char16_t c : s)
                o += c >= 0x20 && c < 0x7F ? (char)c : '?';
            return o;
        };
        const uint16_t name_c = v.pad.colour == PadFrame::kRed ? rgb(232, 80, 64)
                                : v.pad.colour == PadFrame::kWhite ? rgb(232, 228, 220) : kGold;
        cv.centred(0, L::kNameY + 4, kW, v.pad.mode == PadFrame::kIdle ? "PAD" : ascii(v.pad.name).c_str(),
                   v.pad.mode == PadFrame::kIdle ? kHint : name_c);
        for (size_t i = 0; i < v.pad.lines.size(); i++)
            cv.text(L::kBoxX0 + 4, L::kBoxY0 + 4 + (int)i * L::kLineH, ascii(v.pad.lines[i]).c_str(), rgb(232, 228, 220));
        if (v.pad.mode == PadFrame::kEnemy && v.pad.hp_max > 0) {
            const int w = (int)((int64_t)(L::kHpX1 - L::kHpX0) * std::max<int32_t>(0, std::min<int32_t>(v.pad.hp, v.pad.hp_max)) / v.pad.hp_max);
            cv.fill(L::kHpX0, L::kHpY0, L::kHpX1 - L::kHpX0, L::kHpY1 - L::kHpY0 + 1, rgb(48, 4, 4));
            cv.fill(L::kHpX0, L::kHpY0, w, L::kHpY1 - L::kHpY0 + 1, rgb(196, 16, 12));
        }
        cv.fill(L::kCloseX, L::kCloseY, L::kCloseSize, L::kCloseSize, v.pad.close_pressed ? kGold : kKeyBg);
        cv.centred(L::kCloseX, L::kCloseY + 12, L::kCloseSize, "X", v.pad.close_pressed ? kBg : kBarText);
        return;
    }
    draw_bar(cv, v);
    switch (v.kind) {
    case BottomView::kMap:
        if (v.map.empty()) {
            cv.fill(0, kBarH, kW, kAreaH, kBg);
            cv.centred(0, kBarH + kAreaH / 2 - 4, kW, v.note.c_str(), kBarText);
            draw_map_button(cv, SecondScreen::kHelpBtnX, SecondScreen::kHelpBtnW, "?");
            break;
        }
        for (int x = 0; x < kW; x++) {
            uint16_t *col = fb + x * 240 + (239 - kBarH);
            const uint8_t *src = v.map.data() + x;
            for (int y = 0; y < kAreaH; y++, src += kW)
                *col-- = *src ? pal565[*src] : kBg;
        }
        {
            // The buttons: size and the units' refresh.
            char ms[16];
            if (v.map_ms < 1000)
                std::snprintf(ms, sizeof ms, "mark .%us", (unsigned)(v.map_ms % 100 ? v.map_ms / 10 : v.map_ms / 100));
            else
                std::snprintf(ms, sizeof ms, "mark %us", (unsigned)(v.map_ms / 1000));
            draw_map_button(cv, SecondScreen::kMsBtnX, SecondScreen::kMsBtnW, ms);
            draw_map_button(cv, SecondScreen::kScaleBtnX, SecondScreen::kScaleBtnW, v.map_scale == 1 ? "x1" : "x1/2");
            draw_map_button(cv, SecondScreen::kHelpBtnX, SecondScreen::kHelpBtnW, "?");
        }
        break;
    case BottomView::kHelp: {
        cv.fill(0, kBarH, kW, kAreaH, kBg);
        const int n = (int)(sizeof kHelp / sizeof kHelp[0]);
        for (int i = 0; i < n; i++) {
            const int y = kBarH + 4 + i * kHelpRowH;
            cv.text(4, y, kHelp[i].keys, kGold);
            cv.text(4 + 12 * 8, y, kHelp[i].action, rgb(230, 220, 200));
        }
        draw_map_button(cv, SecondScreen::kHelpBtnX, SecondScreen::kHelpBtnW, "MAP");
        break;
    }
    case BottomView::kKeys:
        cv.fill(0, kBarH, kW, kAreaH, kBg);
        for (int i = 0; i < SecondScreen::kKeyCount; i++) {
            const int x = (i % kKeyCols) * kKeyW, y = kBarH + 1 + (i / kKeyCols) * kKeyH;
            const bool lit = (v.keys_lit >> i) & 1;
            cv.fill(x + 2, y + 2, kKeyW - 4, kKeyH - 4, lit ? kKeyLit : kKeyBg);
            cv.centred(x, y + 11, kKeyW, kKeys[i].label, lit ? kBg : rgb(230, 220, 200));
            cv.centred(x, y + 25, kKeyW, kKeys[i].hint, lit ? kBg : kHint);
        }
        break;
    case BottomView::kViewport: {
        if (!frame || fw < kW || fh < kAreaH) {
            cv.fill(0, kBarH, kW, kAreaH, kBg);
            break;
        }
        const int vx = clampi(v.vx, 0, fw - kW), vy = clampi(v.vy, 0, fh - kAreaH);
        for (int x = 0; x < kW; x++) {
            uint16_t *col = fb + x * 240 + (239 - kBarH);
            const uint8_t *src = frame + (size_t)vy * fw + vx + x;
            for (int y = 0; y < kAreaH; y++, src += fw)
                *col-- = pal565[*src];
        }
        if (v.scroll_h > kAreaH) {
            // Where the view is in the panel: a thin bar on the right.
            const int h = kAreaH * kAreaH / v.scroll_h, y = kBarH + vy * kAreaH / v.scroll_h;
            cv.fill(kW - 3, kBarH, 3, kAreaH, kBarBg);
            cv.fill(kW - 3, y, 3, h, kGold);
        }
        break;
    }
    case BottomView::kAim: {
        cv.fill(0, kBarH, kW, kAreaH, kBg);
        const uint16_t ink = rgb(230, 220, 200);
        if (v.aim_faq) {
            // The FAQ: headings (no small letters) in gold; a thin bar on the
            // right shows where the page is.
            for (int i = 0; i < SecondScreen::kAimFaqRows && v.aim_faq_top + i < kAimFaqLines; i++) {
                const char *line = kAimFaq[v.aim_faq_top + i];
                bool heading = *line != 0;
                for (const char *c = line; *c; c++)
                    heading &= !(*c >= 'a' && *c <= 'z');
                cv.text(4, SecondScreen::kAimFaqY + i * SecondScreen::kAimFaqRowH, line, heading ? kGold : ink);
            }
            if (kAimFaqLines > SecondScreen::kAimFaqRows) {
                const int area = SecondScreen::kMapBtnY - kBarH;
                const int h = area * SecondScreen::kAimFaqRows / kAimFaqLines;
                const int y = kBarH + area * v.aim_faq_top / kAimFaqLines;
                cv.fill(kW - 3, kBarH, 3, area, kBarBg);
                cv.fill(kW - 3, y, 3, h, kGold);
            }
        } else {
            // SETTINGS: a row each, "label   - value +".
            for (int row = 0; row < kAimSettingRows; row++) {
                const int y = SecondScreen::kAimRowY + row * SecondScreen::kAimRowH;
                cv.text(4, y + 3, aim_setting_label(row), ink);
                cv.fill(SecondScreen::kAimMinusX + 2, y + 1, SecondScreen::kAimButtonW - 4,
                        SecondScreen::kAimRowH - 2, kKeyBg);
                cv.centred(SecondScreen::kAimMinusX, y + 3, SecondScreen::kAimButtonW, "-", ink);
                cv.centred(SecondScreen::kAimValueX, y + 3, SecondScreen::kAimPlusX - SecondScreen::kAimValueX,
                           aim_setting_value(v.aim, row).c_str(), kGold);
                cv.fill(SecondScreen::kAimPlusX + 2, y + 1, SecondScreen::kAimButtonW - 4,
                        SecondScreen::kAimRowH - 2, kKeyBg);
                cv.centred(SecondScreen::kAimPlusX, y + 3, SecondScreen::kAimButtonW, "+", ink);
            }
        }
        // The pages' buttons: the one shown is lit.
        const int y = SecondScreen::kMapBtnY, w = SecondScreen::kAimBtnW;
        const struct { int x; const char *label; bool on; } pages[] = {
            {SecondScreen::kAimSettingsBtnX, "SETTINGS", !v.aim_faq}, {SecondScreen::kAimFaqBtnX, "FAQ", v.aim_faq}};
        for (const auto &page : pages) {
            if (page.on) {
                cv.fill(page.x + 1, y + 1, w - 2, SecondScreen::kMapBtnH - 2, kGold);
                cv.centred(page.x, y + 4, w, page.label, kBg);
            } else {
                draw_map_button(cv, page.x, w, page.label);
            }
        }
        break;
    }
    case BottomView::kInfo:
        cv.fill(0, kBarH, kW, kAreaH, kBg);
        if (v.checklist) {
            // The profiler run's options: a box, its name, and under it what
            // it writes and costs; greyed while the run is on.
            cv.text(4, kBarH + 6, v.profiling ? "PROFILER RUN: on, until stopped" : "PROFILER RUN: tap what to measure",
                    kGold);
            for (int i = 0; i < kProfileOptionCount; i++) {
                const ProfileOptionInfo &o = kProfileOptions[i];
                const int y = SecondScreen::kProfRowY + i * SecondScreen::kProfRowH + 3;
                const bool ticked = (v.profile & o.bit) != 0;
                const uint16_t ink = v.profiling ? kHint : rgb(230, 220, 200);
                cv.fill(8, y, 14, 14, ink);
                cv.fill(10, y + 2, 10, 10, ticked ? (v.profiling ? kHint : kGold) : kBg);
                cv.text(32, y + 3, o.label, ink);
                cv.text(32, y + 15, o.note, kHint);
            }
            {
                // How the traces are written: no box, a tap steps it on;
                // never greyed, it changes during a run too.
                const TraceWritesInfo &w = trace_writes_info(v.trace_writes);
                const int y = SecondScreen::kProfRowY + kProfileOptionCount * SecondScreen::kProfRowH + 3;
                cv.text(32, y + 3, w.label, w.mode == TraceWrites::Buffered ? rgb(230, 220, 200) : kGold);
                cv.text(32, y + 15, w.note, kHint);
            }
        } else if (v.options && v.options_screen) {
            // The bottom screen's automatic switches: a box each, the same rows.
            cv.text(4, kBarH + 6, "SCREEN: what switches by itself", kGold);
            for (int i = 0; i < kScreenAutoCount; i++) {
                const ScreenAutoInfo &o = kScreenAutos[i];
                const int y = SecondScreen::kOptRowY + i * SecondScreen::kOptRowH + 3;
                const uint16_t ink = rgb(230, 220, 200);
                cv.fill(8, y, 14, 14, ink);
                cv.fill(10, y + 2, 10, 10, (v.screen_auto & o.bit) ? kGold : kBg);
                cv.text(32, y + 3, o.label, ink);
                cv.text(32, y + 15, o.note, kHint);
            }
        } else if (v.options) {
            // The port's options: a box, its name, and under it what it
            // does; never greyed (a profiler run can compare them).
            cv.text(4, kBarH + 6, "OPTIONS: tap one to switch it, at once", kGold);
            for (int i = 0; i < kPortOptionCount; i++) {
                const PortOptionInfo &o = kPortOptions[i];
                const int y = SecondScreen::kOptRowY + i * SecondScreen::kOptRowH + 3;
                const uint16_t ink = rgb(230, 220, 200);
                cv.fill(8, y, 14, 14, ink);
                const bool alt = (v.port & o.alt) != 0;   // the row's second state: a box of another colour
                cv.fill(10, y + 2, 10, 10, (v.port & o.bit) ? kGold : alt ? rgb(120, 170, 230) : kBg);
                cv.text(32, y + 3, o.label, ink);
                cv.text(32, y + 15, alt ? o.alt_note : o.note, kHint);
            }
        } else {
            for (size_t i = 0; i < v.lines.size() && (int)i < (kAreaH - SecondScreen::kMapBtnH) / 8; i++)
                cv.text(0, kBarH + 1 + (int)i * 8, v.lines[i].text.c_str(), v.lines[i].color);
        }
        if (v.checklist || v.options) {
            draw_map_button(cv, SecondScreen::kReportBtnX, SecondScreen::kReportBtnW, "back to INFO");
            if (v.options)
                draw_map_button(cv, SecondScreen::kProfBtnX, SecondScreen::kProfBtnW,
                                v.options_screen ? "engine options" : "screen switches");
            if (v.checklist && v.profiling) {
                const int x = SecondScreen::kProfBtnX, w = SecondScreen::kProfBtnW, y = SecondScreen::kMapBtnY;
                cv.fill(x + 1, y + 1, w - 2, SecondScreen::kMapBtnH - 2, kGold);
                cv.centred(x, y + 4, w, "stop the run", kBg);
            } else if (v.checklist) {
                draw_map_button(cv, SecondScreen::kProfBtnX, SecondScreen::kProfBtnW,
                                v.profile ? "start the run" : "nothing ticked");
            }
        } else {
            // INFO's own page: the profiler run, OPTIONS, the report; a lit
            // button is one that is on.
            const int w = SecondScreen::kInfoBtnW, y = SecondScreen::kMapBtnY;
            auto lit = [&](int x, const char *label) {
                cv.fill(x + 1, y + 1, w - 2, SecondScreen::kMapBtnH - 2, kGold);
                cv.centred(x, y + 4, w, label, kBg);
            };
            if (v.profiling)
                lit(SecondScreen::kInfoBtnX0, "profiling");
            else
                draw_map_button(cv, SecondScreen::kInfoBtnX0, w, "profiler run");
            draw_map_button(cv, SecondScreen::kInfoBtnX1, w, "options");
            if (v.report)
                lit(SecondScreen::kInfoBtnX2, "will report");
            else
                draw_map_button(cv, SecondScreen::kInfoBtnX2, w, "report");
        }
        break;
    default:
        break;
    }
}

void draw_suspending(uint16_t *fb, const uint8_t *font)
{
    const Canvas cv{fb, font};
    const int h = 64, y = (SecondScreen::kHeight - h) / 2;
    cv.fill(0, 0, kW, SecondScreen::kHeight, kBg);
    cv.fill(0, y, kW, h, kBarBg);
    cv.fill(0, y, kW, 1, kHint);
    cv.fill(0, y + h - 1, kW, 1, kHint);
    cv.centred(0, y + 18, kW, "Suspending...", kGold);
    cv.centred(0, y + 38, kW, "The HOME menu opens in a few seconds.", kBarText);
}

}  // namespace game
