// See menu_nav.h for the rules and the types.
//
// Menu navigation turns a flick of the circle pad (up, down, left or right)
// into "move the cursor to the next cell": an inventory square, a button, a
// list row, a character slot. Everything in this file is pure geometry on
// NavWindow and NavRect lists, so tests/menu drives it with made-up windows:
//   - next_cell, entry_cell: the nearest cell a flick's way, and the cell it
//     comes round to on the far side when there is none;
//   - MenuNav::step: one flick over all the open windows, with the rules
//     for held items and scroll columns;
//   - fixed_windows, game_menu_rows, d2win_cell, d2win_scroll: the cells of
//     the game's panels, the game menu and D2Win's out-of-game screens, from
//     what d2_menus.cpp read in the game's memory.
#include "game/menu_nav.h"

#include <climits>
#include <cmath>
#include <cstdlib>

namespace game {
namespace {

// Up and down move along y, left and right along x.
bool vertical(NavDirection dir) { return dir == NavDirection::kUp || dir == NavDirection::kDown; }

// How far `cell` is along the flick from x, y (negative: behind), and how
// far beside the line the flick goes along (0 when the line crosses it).
int along(const NavRect &cell, int x, int y, NavDirection dir)
{
    switch (dir) {
    case NavDirection::kUp: return y - cell.centre_y();
    case NavDirection::kDown: return cell.centre_y() - y;
    case NavDirection::kLeft: return x - cell.centre_x();
    case NavDirection::kRight: return cell.centre_x() - x;
    }
    return 0;
}

// How far `cell` is off to the side of the flick's line through x, y
// (0 when the line goes through it).
int beside(const NavRect &cell, int x, int y, NavDirection dir)
{
    if (vertical(dir))
        return x < cell.left ? cell.left - x : x >= cell.right ? x - cell.right + 1 : 0;
    return y < cell.top ? cell.top - y : y >= cell.bottom ? y - cell.bottom + 1 : 0;
}

// The nearest cell the flick's way from x, y (not `skip`, the cell it is
// on), or -1. A cell's middle must be past `skip`'s edge (a small slot
// beside a tall one is not "below" it); a cell straight ahead wins over one
// off to the side.
constexpr int kAhead = 4;   // pixels: with no cell, a cell's middle must be this far ahead
int next_cell(const std::vector<NavRect> &cells, int x, int y, NavDirection dir, const NavRect *skip)
{
    int ahead = kAhead;
    if (skip) {
        const int half = (vertical(dir) ? skip->bottom - skip->top : skip->right - skip->left) / 2;
        ahead = half > ahead ? half : ahead;
    }
    int best = -1, best_score = INT_MAX;
    for (size_t i = 0; i < cells.size(); i++) {
        const NavRect &c = cells[i];
        if (skip && c.left == skip->left && c.top == skip->top && c.right == skip->right && c.bottom == skip->bottom)
            continue;
        const int a = along(c, x, y, dir);
        if (a < ahead)
            continue;
        // Nor one across the cell's own middle (a wide button under a
        // small one is below it, not beside it).
        if (skip && (vertical(dir) ? c.top <= y && y < c.bottom : c.left <= x && x < c.right))
            continue;
        const int score = a + 2 * beside(c, x, y, dir);
        if (score < best_score) {
            best_score = score;
            best = (int)i;
        }
    }
    return best;
}

// The cell a flick going `dir` comes into the window at: in the row (or
// column) nearest `across` (a y for left and right, an x for up and down),
// the first cell on the side it comes in from.
int edge(const NavRect &c, NavDirection dir)   // smaller: nearer the side a flick `dir` comes in from
{
    switch (dir) {
    case NavDirection::kUp: return -c.bottom;
    case NavDirection::kDown: return c.top;
    case NavDirection::kLeft: return -c.right;
    case NavDirection::kRight: return c.left;
    }
    return 0;
}

// The cell a flick lands on when it enters a window from outside:
// nearest the side it comes in from, and nearest `across` (the cursor's
// place along that side).
int entry_cell(const std::vector<NavRect> &cells, int across, NavDirection dir)
{
    int nearest = INT_MAX;
    for (const NavRect &c : cells)
        nearest = edge(c, dir) < nearest ? edge(c, dir) : nearest;
    // Near that side and near the row (or column): a cell beside the line
    // counts twice its distance, one further in once.
    int best = -1, best_score = INT_MAX;
    for (size_t i = 0; i < cells.size(); i++) {
        const NavRect &c = cells[i];
        const int gap = vertical(dir) ? beside(c, across, 0, dir) : beside(c, 0, across, dir);
        const int score = 2 * gap + edge(c, dir) - nearest;
        if (score < best_score) {
            best = (int)i;
            best_score = score;
        }
    }
    return best;
}

constexpr int kTouching = 8;   // pixels: a window this close to the middle still covers that half

// Which window and cell x, y is on (-1: none).
void locate(const std::vector<NavWindow> &windows, int x, int y, int &window, int &cell)
{
    window = cell = -1;
    for (size_t w = 0; w < windows.size(); w++)
        for (size_t c = 0; c < windows[w].cells.size(); c++)
            if (windows[w].cells[c].contains(x, y)) {
                window = (int)w;
                cell = (int)c;
                return;
            }
    for (size_t w = 0; w < windows.size(); w++)
        if (windows[w].area.contains(x, y)) {
            window = (int)w;
            return;
        }
}

}  // namespace

NavDirection nav_direction(float dir_x, float dir_y)
{
    if (std::fabs(dir_x) > std::fabs(dir_y))
        return dir_x < 0 ? NavDirection::kLeft : NavDirection::kRight;
    return dir_y < 0 ? NavDirection::kUp : NavDirection::kDown;
}

const char *nav_direction_name(NavDirection dir)
{
    switch (dir) {
    case NavDirection::kUp: return "up";
    case NavDirection::kDown: return "down";
    case NavDirection::kLeft: return "left";
    case NavDirection::kRight: return "right";
    }
    return "?";
}

MenuNav::Step MenuNav::step(const std::vector<NavWindow> &windows, int x, int y, NavDirection dir, bool holding,
                            int width, int height)
{
    Step out;
    auto go = [&](const NavRect &cell, const char *why) {
        out.moved = true;
        out.x = cell.centre_x();
        out.y = cell.centre_y();
        out.why = why;
        return out;
    };
    for (const NavWindow &win : windows) {
        int tx = 0, ty = 0;
        const char *why = "";
        if (scroll_step(win.scroll, x, y, dir, tx, ty, why)) {
            out.moved = true;
            out.x = tx;
            out.y = ty;
            out.why = why;
            return out;
        }
    }
    int w = -1, c = -1;
    locate(windows, x, y, w, c);

    if (w < 0) {
        // Off every window: back onto the cell the cursor left for the free
        // half, if the flick goes towards it; else the nearest cell that way.
        if (left_cell_) {
            for (const NavWindow &win : windows)
                for (const NavRect &cell : win.cells)
                    if (cell.left == left_from_.left && cell.top == left_from_.top &&
                        cell.right == left_from_.right && cell.bottom == left_from_.bottom &&
                        along(cell, x, y, dir) >= kAhead) {
                        left_cell_ = false;
                        return go(cell, "back");
                    }
        }
        int best_w = -1, best_c = -1, best_score = INT_MAX;
        for (size_t i = 0; i < windows.size(); i++) {
            if (windows[i].up_down_only && !vertical(dir))
                continue;
            const int n = next_cell(windows[i].cells, x, y, dir, nullptr);
            if (n < 0)
                continue;
            const NavRect &cell = windows[i].cells[n];
            const int score = along(cell, x, y, dir) + 2 * beside(cell, x, y, dir);
            if (score < best_score) {
                best_score = score;
                best_w = (int)i;
                best_c = n;
            }
        }
        if (best_w >= 0)
            return go(windows[best_w].cells[best_c], "onto");
        return out;
    }

    const NavWindow &win = windows[w];
    if (win.up_down_only && !vertical(dir))
        return out;
    // From the cell's middle (or, on a panel between cells, the cursor):
    // the nearest cell that way in any open window. A list of options
    // (up_down_only) only takes up and down flicks, and keeps them to
    // itself.
    const int fx = c >= 0 ? win.cells[c].centre_x() : x, fy = c >= 0 ? win.cells[c].centre_y() : y;
    const NavRect *skip = c >= 0 ? &win.cells[c] : nullptr;
    auto takes = [&](const NavWindow &other) {
        if (win.up_down_only)
            return &other == &win;
        return !other.up_down_only || vertical(dir);
    };
    int best_w = -1, best_c = -1, best_score = INT_MAX;
    for (size_t i = 0; i < windows.size(); i++) {
        if (!takes(windows[i]))
            continue;
        const int n = next_cell(windows[i].cells, fx, fy, dir, skip);
        if (n < 0)
            continue;
        const NavRect &cell = windows[i].cells[n];
        const int score = along(cell, fx, fy, dir) + 2 * beside(cell, fx, fy, dir);
        if (score < best_score) {
            best_score = score;
            best_w = (int)i;
            best_c = n;
        }
    }
    if (best_w >= 0)
        return go(windows[best_w].cells[best_c], best_w == w ? "cell" : "window");

    if (holding && !win.up_down_only) {
        // The free half: the left or right half no open window covers.
        bool left_taken = false, right_taken = false;
        for (const NavWindow &other : windows) {
            left_taken |= other.area.left < width / 2 - kTouching;
            right_taken |= other.area.right > width / 2 + kTouching;
        }
        if (left_taken != right_taken) {
            left_cell_ = c >= 0;
            if (c >= 0)
                left_from_ = win.cells[c];
            out.moved = true;
            out.x = left_taken ? width * 3 / 4 : width / 4;
            out.y = height / 2;
            out.why = "free half";
            return out;
        }
    }
    // Nothing that way: round to the far side, over all the windows it may
    // go to, nearest the same row or column.
    std::vector<NavRect> all;
    for (const NavWindow &other : windows)
        if (takes(other))
            all.insert(all.end(), other.cells.begin(), other.cells.end());
    const int across = vertical(dir) ? fx : fy;
    const int e = entry_cell(all, across, dir);   // the side a flick this way comes in from is the far one
    if (e >= 0 && !(skip && all[e].left == skip->left && all[e].top == skip->top && all[e].right == skip->right &&
                     all[e].bottom == skip->bottom))
        return go(all[e], "round");
    return out;
}

NavRect weapon_tab(const NavRect &weapon_slot)
{
    return NavRect{weapon_slot.left, weapon_slot.top - 22, weapon_slot.right, weapon_slot.top - 3};
}

void add_grid(NavWindow &window, const NavRect &grid, int columns, int rows)
{
    if (columns <= 0 || rows <= 0 || grid.empty())
        return;
    for (int r = 0; r < rows; r++)
        for (int col = 0; col < columns; col++) {
            NavRect cell;
            cell.left = grid.left + (grid.right - grid.left) * col / columns;
            cell.right = grid.left + (grid.right - grid.left) * (col + 1) / columns;
            cell.top = grid.top + (grid.bottom - grid.top) * r / rows;
            cell.bottom = grid.top + (grid.bottom - grid.top) * (r + 1) / rows;
            window.cells.push_back(cell);
        }
}

namespace {

// A spot to click, as a small cell round it.
NavRect spot(int x, int y, int half = 8) { return NavRect{x - half, y - half, x + half, y + half}; }

constexpr uint64_t ui_bit(int id) { return 1ull << id; }

}  // namespace

PanelShift panel_shift(int width, int height)
{
    PanelShift s;
    s.left_x = s.right_x = s.centre_x = (width - 640) / 2;
    s.left_y = s.right_y = s.centre_y = (height - 480) / 2;
    return s;
}

int gold_field_amount(const uint16_t *text, int length)
{
    long long amount = 0;
    for (int i = 0; i < length && text[i] >= '0' && text[i] <= '9'; i++) {
        amount = amount * 10 + (text[i] - '0');
        if (amount > 0x7FFFFFFF)
            return 0x7FFFFFFF;
    }
    return (int)amount;
}

NavWindow gold_dialog_window(const GoldDialog &dialog)
{
    NavWindow w;
    w.name = "gold dialog";
    w.area = NavRect{dialog.ok.left - 40, dialog.ok.top - 120, dialog.cancel.right + 40, dialog.cancel.bottom + 12};
    for (const NavRect &r : {dialog.ok, dialog.cancel})
        if (!r.empty())
            w.cells.push_back(r);
    return w;
}

int GoldRepeat::update(int step, uint64_t now_ms)
{
    if (step != step_) {   // pressed (or another direction): one step now
        step_ = step;
        pressed_ms_ = now_ms;
        next_ms_ = now_ms + kDelayMs;
        interval_ms_ = kFirstMs;
        return step;
    }
    if (!step || now_ms < next_ms_)
        return 0;
    // Held: again, a little sooner each time, and tenfold every kTenfoldMs.
    next_ms_ = now_ms + (uint64_t)interval_ms_;
    interval_ms_ = interval_ms_ - interval_ms_ / 6 > kFastestMs ? interval_ms_ - interval_ms_ / 6 : kFastestMs;
    int tenfolds = (int)((now_ms - pressed_ms_) / kTenfoldMs);
    tenfolds = tenfolds < kMaxTenfolds ? tenfolds : kMaxTenfolds;
    int change = step;
    for (int i = 0; i < tenfolds; i++)
        change *= 10;
    return change;
}

NavRect stash_gold_button(const PanelShift &shift)
{
    return NavRect{72 + shift.left_x, 23 + shift.left_y, 97 + shift.left_x, 43 + shift.left_y};
}

NavRect inventory_gold_button(const PanelShift &shift)
{
    return NavRect{402 + shift.right_x, 392 + shift.right_y, 427 + shift.right_x, 412 + shift.right_y};
}

NavRect inventory_close_button(const PanelShift &shift)
{
    return NavRect{340 + shift.right_x, 388 + shift.right_y, 368 + shift.right_x, 416 + shift.right_y};
}

NavRect stash_close_button(const PanelShift &shift)
{
    return NavRect{273 + shift.left_x, 387 + shift.left_y, 301 + shift.left_x, 415 + shift.left_y};
}

void fixed_windows(uint64_t ui, const PanelShift &shift, bool stat_points, std::vector<NavWindow> &out)
{
    // A 640x480 rectangle or spot moved with the left half, the right half
    // or the centre.
    auto left = [&](NavRect r) {
        return NavRect{r.left + shift.left_x, r.top + shift.left_y, r.right + shift.left_x, r.bottom + shift.left_y};
    };
    auto right = [&](NavRect r) {
        return NavRect{r.left + shift.right_x, r.top + shift.right_y, r.right + shift.right_x,
                       r.bottom + shift.right_y};
    };
    auto centre = [&](NavRect r) {
        return NavRect{r.left + shift.centre_x, r.top + shift.centre_y, r.right + shift.centre_x,
                       r.bottom + shift.centre_y};
    };
    if (ui & ui_bit(0x09)) {
        // The game menu: its three options down the middle.
        NavWindow w;
        w.name = "game menu";
        w.area = centre(NavRect{120, 120, 520, 280});
        w.up_down_only = true;
        for (int y : {150, 199, 246})
            w.cells.push_back(centre(NavRect{170, y - 18, 470, y + 18}));
        out.push_back(w);
        return;   // drawn over every other panel
    }
    if (ui & ui_bit(0x02)) {
        // The character screen (the left half): the "+" button right of
        // each of the four stats while there are points to spend, and the
        // close button at the bottom.
        NavWindow w;
        w.name = "character";
        w.area = left(NavRect{0, 0, 320, 432});
        if (stat_points)
            for (int y : {92, 155, 241, 302})   // Strength, Dexterity, Vitality, Energy
                w.cells.push_back(left(NavRect{124, y - 15, 156, y + 15}));
        w.cells.push_back(left(spot(143, 404, 14)));
        out.push_back(w);
    }
    if (ui & ui_bit(0x04)) {
        // The skill tree (the right half): three columns of six skills, its
        // three tabs down the right, and the close button at the bottom.
        NavWindow w;
        w.name = "skill tree";
        w.area = right(NavRect{320, 0, 640, 432});
        for (int y : {38, 106, 174, 242, 310, 378})
            for (int x : {361, 430, 498})
                w.cells.push_back(right(spot(x, y, 14)));
        for (int y : {160, 270, 375})
            w.cells.push_back(right(spot(595, y, 14)));
        w.cells.push_back(right(spot(507, 401, 14)));
        out.push_back(w);
    }
    if (ui & ui_bit(0x0F)) {
        // The quest log (the left half): two rows of three quests, and the
        // close button at the bottom.
        NavWindow w;
        w.name = "quest log";
        w.area = left(NavRect{0, 0, 320, 432});
        for (int y : {85, 180})
            for (int x : {62, 158, 255})
                w.cells.push_back(left(spot(x, y, 20)));
        w.cells.push_back(left(spot(291, 404, 14)));
        out.push_back(w);
    }
    if (ui & ui_bit(0x14)) {
        // The waypoints (the left half): nine places down the left, each
        // a box 36 pixels apart, and the close button under them.
        NavWindow w;
        w.name = "waypoints";
        w.area = left(NavRect{0, 0, 320, 432});
        for (int i = 0; i < 9; i++)
            w.cells.push_back(left(NavRect{17, 63 + 36 * i, 300, 91 + 36 * i}));
        w.cells.push_back(left(spot(288, 401, 14)));
        out.push_back(w);
    }
}

namespace {

// A list of options: one cell a line, `first_y` the first option's middle.
bool option_list(const NavRect &box, int first_y, int options, NavWindow &out)
{
    if (options < 1 || options > 8 || box.right - box.left < 40)
        return false;
    out = NavWindow();
    out.name = "NPC menu";
    out.area = box;
    out.up_down_only = true;
    for (int i = 0; i < options; i++) {
        const int y = first_y + 15 * i;
        out.cells.push_back(NavRect{box.left + 2, y - 7, box.right - 2, y + 8});
    }
    return true;
}

}  // namespace

void game_menu_rows(int count, int step, const int8_t *kinds, int width, int height, GameMenu &out)
{
    out.rows.clear();
    if (count < 1 || count > 16 || step < 10 || step > 100)
        return;
    const PanelShift shift = panel_shift(width, height);
    for (int i = 0; i < count; i++) {
        const int y = 199 + shift.centre_y + (int)std::lround((i - (count - 1) / 2.0) * step);
        const int half = step / 2 - 4 < 18 ? step / 2 - 4 : 18;
        GameMenu::Row row;
        row.rect = NavRect{170 + shift.centre_x, y - half, 470 + shift.centre_x, y + half};
        row.kind = kinds ? kinds[i] : (int8_t)GameMenu::kButton;
        out.rows.push_back(row);
    }
}

bool control_panel_at(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0 || x < 0 || y < 0 || x >= width || y >= height)
        return false;
    if (y >= height - 48)
        return true;   // the bar
    if (y >= height - 102 && (x < 118 || x >= width - 118))
        return true;   // the globes
    // The mini-panel's buttons (over the bar's middle; counted shown or not).
    const NavRect mini = width >= 800 ? NavRect{195, height - 72, 349, height - 47}
                                      : NavRect{(width - 640) / 2 + 243, height - 72, (width - 640) / 2 + 397, height - 47};
    return mini.contains(x, y);
}

bool d2win_cell(const D2WinControl &c, int width, int height, NavRect &out)
{
    if ((c.state & 5) != 5)
        return false;   // hidden or disabled
    bool takes = false;
    switch (c.type) {
    case 1: case 6: case 7: case 12: takes = true; break;   // text field, button, list, account list
    case 2: takes = c.on_click; break;                      // an image that is a button (the heroes)
    case 4: takes = c.on_click && c.lines >= 2; break;      // a character's slot
    default: takes = false; break;                          // scroll bars, timers, videos...
    }
    if (!takes)
        return false;
    NavRect r{c.x, c.y - c.height, c.x + c.width, c.y};
    if (c.type == 2 && c.frame)
        r = NavRect{c.x + c.frame_x, c.y + c.frame_y - c.frame_height, c.x + c.frame_x + c.frame_width,
                    c.y + c.frame_y};
    if (r.empty() || r.right - r.left > width || r.bottom - r.top > height)
        return false;
    // Partly off the frame: the part on it.
    r.left = r.left < 0 ? 0 : r.left;
    r.top = r.top < 0 ? 0 : r.top;
    r.right = r.right > width ? width : r.right;
    r.bottom = r.bottom > height ? height : r.bottom;
    if (r.empty())
        return false;
    out = r;
    return true;
}

NavRect scroll_up_button(const D2WinControl &bar)
{
    const int top = bar.y - bar.height;
    return NavRect{bar.x, top, bar.x + bar.width, top + 11};
}

NavRect scroll_down_button(const D2WinControl &bar)
{
    return NavRect{bar.x, bar.y - 9, bar.x + bar.width, bar.y};
}

bool d2win_main_menu(const std::vector<D2WinControl> &controls)
{
    bool single_player = false, exit = false;
    for (const D2WinControl &c : controls) {
        if (c.type != 6 || c.x != 264 || c.width != 272)
            continue;
        single_player |= c.y == 324;
        exit |= c.y == 568;
    }
    return single_player && exit;
}

bool d2win_scroll(const std::vector<D2WinControl> &controls, const std::vector<NavRect> &cells, int width,
                  int height, NavScroll &out)
{
    out = NavScroll();
    const D2WinControl *bar = nullptr;
    for (const D2WinControl &c : controls)
        if (c.type == 5 && (c.state & 4) && c.width > 0 && c.height > 20) {
            bar = &c;
            break;
        }
    if (!bar)
        return false;
    for (const D2WinControl &c : controls) {
        NavRect cell;
        if (c.type == 4 && d2win_cell(c, width, height, cell) && cell.right <= bar->x)
            out.slots.push_back(cell);
    }
    if (out.slots.empty())
        return false;
    out.present = true;
    out.up = scroll_up_button(*bar);
    out.down = scroll_down_button(*bar);
    const int last = bar->scroll_last > 0 && bar->scroll_last < 10000 ? bar->scroll_last : 0;
    out.up_on = last > 0 && bar->scroll_top > 0;
    out.down_on = last > 0 && bar->scroll_top < last;
    // The cell under the column: its own first (over the down button's
    // middle), else the nearest below.
    int best = INT_MAX;
    for (const NavRect &c : cells) {
        if (c.top < out.down.bottom)
            continue;
        const bool over = c.left <= out.down.centre_x() && out.down.centre_x() < c.right;
        const int score = (c.top - out.down.bottom) + (over ? 0 : 100000) +
                          std::abs(c.centre_x() - out.down.centre_x());
        if (score < best) {
            best = score;
            out.below = c;
        }
    }
    return true;
}

bool scroll_step(const NavScroll &scroll, int x, int y, NavDirection dir, int &to_x, int &to_y, const char *&why)
{
    if (!scroll.present || !scroll.any_on())
        return false;
    const bool on_up = scroll.up.contains(x, y), on_down = scroll.down.contains(x, y);
    const bool on_below = !scroll.below.empty() && scroll.below.contains(x, y);
    auto go = [&](const NavRect &r, const char *rule) {
        to_x = r.centre_x();
        to_y = r.centre_y();
        why = rule;
        return true;
    };
    if ((on_up || on_down || on_below) && (dir == NavDirection::kUp || dir == NavDirection::kDown)) {
        // Round the column: up, down, the cell below; inactive buttons left out.
        const NavRect *stops[3] = {scroll.up_on ? &scroll.up : nullptr, scroll.down_on ? &scroll.down : nullptr,
                                   scroll.below.empty() ? nullptr : &scroll.below};
        const int at = on_up ? 0 : on_down ? 1 : 2;
        const int step = dir == NavDirection::kDown ? 1 : 2;   // +1 or -1 round three
        for (int i = 1; i <= 3; i++) {
            const int k = (at + i * step) % 3;
            if (stops[k] && k != at)
                return go(*stops[k], k == 2 ? "scroll column, below" : "scroll column");
        }
        return false;
    }
    if ((on_up || on_down) && dir == NavDirection::kLeft) {
        // Back to the slot nearest in height (the right one of a row).
        const NavRect *best = nullptr;
        for (const NavRect &s : scroll.slots) {
            if (!best) {
                best = &s;
                continue;
            }
            const int d = std::abs(s.centre_y() - y), bd = std::abs(best->centre_y() - y);
            if (d < bd || (d == bd && s.centre_x() > best->centre_x()))
                best = &s;
        }
        return best && go(*best, "slots");
    }
    if (dir == NavDirection::kRight) {
        int right = INT_MIN;
        for (const NavRect &s : scroll.slots)
            right = std::max(right, s.right);
        for (const NavRect &s : scroll.slots) {
            if (!s.contains(x, y) || s.right != right)
                continue;
            // The right column: onto the active button nearest in height.
            const NavRect *b = nullptr;
            if (scroll.up_on && scroll.down_on)
                b = std::abs(scroll.up.centre_y() - y) <= std::abs(scroll.down.centre_y() - y) ? &scroll.up
                                                                                                : &scroll.down;
            else
                b = scroll.up_on ? &scroll.up : &scroll.down;
            return go(*b, "scroll button");
        }
    }
    return false;
}

bool npc_menu_window(const NavRect &box, NavWindow &out)
{
    const int options = (int)std::lround((box.bottom - 1 - box.top - 16 - 20) / 15.0);
    return option_list(box, box.top + 16 + 15, options, out);
}

bool npc_menu_window(const NavRect &box, int options, NavWindow &out)
{
    return option_list(box, box.top + 30, options, out);
}

}  // namespace game
