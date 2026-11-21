// Menu navigation (src/game/menu_nav.h): a flick moves the cursor one cell,
// round a window's edge, into the next window, to the free half with an item,
// and through D2Win's screens before the game; B is Escape there (nothing on
// the main menu). Made-up windows stand in for the game. Run through
// `scripts/docker.sh test menu`.
#include "game/controls.h"
#include "game/menu_nav.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace game;

static int g_fail = 0;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            g_fail++;                                                    \
        }                                                                \
    } while (0)

// ---- the runtime the controls call ----
static int g_cursor_x = 0, g_cursor_y = 0;
static bool g_left = false, g_right = false;
static bool g_keys[256] = {};
namespace rt {
void input_button(bool right, bool down) { (right ? g_right : g_left) = down; }
void input_key(uint8_t vk, bool down) { g_keys[vk] = down; }
void input_mouse_move(int x, int y)
{
    g_cursor_x = x;
    g_cursor_y = y;
}
void logf(const char *f, ...) { if (std::getenv("MENU_LOG")) { va_list a; va_start(a, f); std::vprintf(f, a); va_end(a); std::printf("\n"); } }
}  // namespace rt

// ---- made-up windows, with inventory.txt's numbers at 640x480 ----
static NavWindow inventory()
{
    NavWindow w;
    w.name = "inventory";
    w.area = NavRect{320, 0, 640, 441};
    add_grid(w, NavRect{339, 255, 626, 368}, 10, 4);
    const NavRect slots[] = {{340, 47, 395, 159},  {453, 77, 509, 159},  {571, 47, 626, 159}, {455, 8, 509, 59},
                             {529, 35, 552, 59},   {415, 180, 438, 204}, {529, 180, 552, 204}, {456, 179, 508, 204},
                             {572, 182, 626, 234}, {341, 181, 395, 234}};
    for (const NavRect &s : slots)
        w.cells.push_back(s);
    return w;
}

static NavWindow stash()
{
    NavWindow w;
    w.name = "stash";
    w.area = NavRect{0, 0, 321, 442};
    add_grid(w, NavRect{74, 82, 244, 313}, 6, 8);
    return w;
}

// The grid's cell centre, column col, row row (the inventory's 10x4).
static void inv_cell(int col, int row, int &x, int &y)
{
    x = 339 + (626 - 339) * col / 10;
    x = (x + 339 + (626 - 339) * (col + 1) / 10) / 2;
    y = 255 + (368 - 255) * row / 4;
    y = (y + 255 + (368 - 255) * (row + 1) / 4) / 2;
}

static void stash_cell(int col, int row, int &x, int &y)
{
    x = (74 + 170 * col / 6 + 74 + 170 * (col + 1) / 6) / 2;
    y = (82 + 231 * row / 8 + 82 + 231 * (row + 1) / 8) / 2;
}

static MenuNav::Step flick_at(MenuNav &nav, const std::vector<NavWindow> &w, int x, int y, NavDirection d,
                              bool holding = false)
{
    return nav.step(w, x, y, d, holding, 640, 480);
}

static void test_direction()
{
    CHECK(nav_direction(1, 0.2f) == NavDirection::kRight);
    CHECK(nav_direction(-0.9f, 0.5f) == NavDirection::kLeft);
    CHECK(nav_direction(0.3f, -0.9f) == NavDirection::kUp);
    CHECK(nav_direction(-0.3f, 0.9f) == NavDirection::kDown);
}

static void test_one_cell()
{
    MenuNav nav;
    std::vector<NavWindow> w = {inventory()};
    int x, y, ex, ey;
    inv_cell(3, 1, x, y);
    MenuNav::Step s = flick_at(nav, w, x, y, NavDirection::kRight);
    inv_cell(4, 1, ex, ey);
    CHECK(s.moved && s.x == ex && s.y == ey);
    s = flick_at(nav, w, x, y, NavDirection::kDown);
    inv_cell(3, 2, ex, ey);
    CHECK(s.moved && s.x == ex && s.y == ey);
    // From anywhere in a cell (a large item spans several; the cursor at
    // its edge): one cell on, from that cell.
    s = flick_at(nav, w, x + 10, y - 10, NavDirection::kLeft);
    inv_cell(2, 1, ex, ey);
    CHECK(s.moved && s.x == ex && s.y == ey);
    // The grid's top row, up: the equipment above it (gloves over column 0).
    inv_cell(0, 0, x, y);
    s = flick_at(nav, w, x, y, NavDirection::kUp);
    CHECK(s.moved && s.x == (341 + 395) / 2 && s.y == (181 + 234) / 2);
}

static void test_round_the_edges()
{
    MenuNav nav;
    std::vector<NavWindow> w = {inventory()};
    int x, y, ex, ey;
    // The right edge: round to the row's first cell.
    inv_cell(9, 2, x, y);
    MenuNav::Step s = flick_at(nav, w, x, y, NavDirection::kRight);
    inv_cell(0, 2, ex, ey);
    CHECK(s.moved && s.x == ex && s.y == ey && std::strcmp(s.why, "round") == 0);
    // And the left edge to the last.
    s = flick_at(nav, w, ex, ey, NavDirection::kLeft);
    CHECK(s.moved && s.x == x && s.y == y);
    // The bottom: up to the column's top, the shield over column 9.
    inv_cell(9, 3, x, y);
    s = flick_at(nav, w, x, y, NavDirection::kDown);
    CHECK(s.moved && s.x == (571 + 626) / 2 && s.y == (47 + 159) / 2);
    // The helmet, up: to the bottom row under it (column 4 or 5).
    s = flick_at(nav, w, (455 + 509) / 2, 30, NavDirection::kUp);
    inv_cell(4, 3, ex, ey);
    CHECK(s.moved && s.y == ey && s.x > 455 && s.x < 509);
}

static void test_next_window()
{
    MenuNav nav;
    std::vector<NavWindow> w = {inventory(), stash()};
    int x, y;
    inv_cell(0, 1, x, y);
    MenuNav::Step s = flick_at(nav, w, x, y, NavDirection::kLeft);
    CHECK(s.moved && std::strcmp(s.why, "window") == 0);
    CHECK(s.x == (74 + 170 * 5 / 6 + 244) / 2);   // the stash's right column
    CHECK(s.y > 82 + 231 / 2 && s.y < 313);       // the same share of the way down (lower half)
    // And back: the stash's right edge goes into the inventory's left side.
    int sx, sy;
    stash_cell(5, 0, sx, sy);
    s = flick_at(nav, w, sx, sy, NavDirection::kRight);
    CHECK(s.moved && std::strcmp(s.why, "window") == 0 && s.x < 400);
    // From the weapon slot (tall, its left edge over the grid's first
    // column), left: the stash, not the grid's cell below.
    s = flick_at(nav, w, (340 + 395) / 2, 103, NavDirection::kLeft);
    CHECK(s.moved && std::strcmp(s.why, "window") == 0 && s.x < 320);
    // An item on the cursor: into the next window all the same.
    s = flick_at(nav, w, x, y, NavDirection::kLeft, true);
    CHECK(s.moved && std::strcmp(s.why, "window") == 0);
    // Both halves taken, an item held, the inventory's right edge: round.
    inv_cell(9, 1, x, y);
    s = flick_at(nav, w, x, y, NavDirection::kRight, true);
    CHECK(s.moved && std::strcmp(s.why, "round") == 0);
}

static void test_free_half()
{
    MenuNav nav;
    std::vector<NavWindow> w = {inventory()};
    int x, y;
    inv_cell(0, 2, x, y);
    MenuNav::Step s = flick_at(nav, w, x, y, NavDirection::kLeft, true);
    CHECK(s.moved && s.x == 160 && s.y == 240 && std::strcmp(s.why, "free half") == 0);
    // A flick away from the window: nowhere.
    s = flick_at(nav, w, 160, 240, NavDirection::kLeft, true);
    CHECK(!s.moved);
    // Back towards it: the cell it left.
    s = flick_at(nav, w, 160, 240, NavDirection::kRight, true);
    CHECK(s.moved && s.x == x && s.y == y && std::strcmp(s.why, "back") == 0);
    // Without an item: round, not the free half.
    s = flick_at(nav, w, x, y, NavDirection::kLeft, false);
    CHECK(s.moved && std::strcmp(s.why, "round") == 0);
    // A window on the left only (the stash's place): the right half.
    std::vector<NavWindow> left = {stash()};
    int sx, sy;
    stash_cell(5, 3, sx, sy);
    s = flick_at(nav, left, sx, sy, NavDirection::kRight, true);
    CHECK(s.moved && s.x == 480 && s.y == 240);
    // Off every window with nothing to return to: the nearest cell that way.
    nav.forget();
    s = flick_at(nav, w, 100, 300, NavDirection::kRight);
    int ex, ey;
    inv_cell(0, 1, ex, ey);
    CHECK(s.moved && s.x == ex && s.y == ey && std::strcmp(s.why, "onto") == 0);
}

static void test_npc_menu()
{
    // Warriv's menu as the lane showed it: the box 206..307 by 31..97,
    // Talk at 62, Cancel at 77; the Talk menu under it 26..108, three options.
    NavWindow menu;
    CHECK(npc_menu_window(NavRect{206, 31, 308, 98}, menu));
    CHECK(menu.cells.size() == 2 && menu.cells[0].centre_y() == 62 && menu.cells[1].centre_y() == 77);
    NavWindow talk;
    CHECK(npc_menu_window(NavRect{206, 26, 308, 109}, talk));
    CHECK(talk.cells.size() == 3 && talk.cells[0].centre_y() == 57 && talk.cells[2].centre_y() == 87);
    CHECK(!npc_menu_window(NavRect{206, 27, 230, 108}, talk));
    MenuNav nav;
    std::vector<NavWindow> w = {menu};
    MenuNav::Step s = flick_at(nav, w, 256, 62, NavDirection::kDown);
    CHECK(s.moved && s.y == 77);
    s = flick_at(nav, w, 256, 77, NavDirection::kDown);   // round to the top
    CHECK(s.moved && s.y == 62);
    s = flick_at(nav, w, 256, 62, NavDirection::kUp);
    CHECK(s.moved && s.y == 77);
    s = flick_at(nav, w, 256, 62, NavDirection::kLeft);   // up and down only
    CHECK(!s.moved);
    s = flick_at(nav, w, 256, 62, NavDirection::kRight, true);
    CHECK(!s.moved);
    // The cursor off the menu (where the click on the NPC left it): onto it.
    s = flick_at(nav, w, 256, 200, NavDirection::kUp);
    CHECK(s.moved && s.y == 77);
    // The same menus as the game keeps them (0x007C0E54): box 173,48 102x66
    // with two options, Talk 78 and Cancel 93; the Talk menu 173,43 102x81
    // with three, 73 88 103 (the lane at 640x480); at 800x600 253,108: 138.
    NavWindow game;
    CHECK(npc_menu_window(NavRect{173, 48, 275, 114}, 2, game));
    CHECK(game.cells.size() == 2 && game.cells[0].centre_y() == 78 && game.cells[1].centre_y() == 93 &&
          game.up_down_only);
    CHECK(npc_menu_window(NavRect{173, 43, 275, 124}, 3, game));
    CHECK(game.cells.size() == 3 && game.cells[0].centre_y() == 73 && game.cells[2].centre_y() == 103);
    CHECK(npc_menu_window(NavRect{253, 108, 355, 174}, 2, game));
    CHECK(game.cells[0].centre_y() == 138 && game.cells[0].centre_x() == 304);
    CHECK(!npc_menu_window(NavRect{173, 48, 275, 114}, 0, game));
    CHECK(!npc_menu_window(NavRect{173, 48, 275, 114}, 9, game));
}

static void test_fixed_windows()
{
    const PanelShift small = panel_shift(640, 480);
    CHECK(small.left_x == 0 && small.right_y == 0 && small.centre_x == 0);
    std::vector<NavWindow> out;
    fixed_windows(1ull << 0x09 | 1ull << 0x04, small, false, out);
    CHECK(out.size() == 1 && out[0].cells.size() == 3 && out[0].up_down_only);   // the game menu over the rest
    out.clear();
    fixed_windows(1ull << 0x04 | 1ull << 0x0F, small, false, out);
    CHECK(out.size() == 2 && out[0].cells.size() == 21 && out[1].cells.size() == 6);
    out.clear();
    fixed_windows(1ull << 0x14, small, false, out);
    CHECK(out.size() == 1 && out[0].cells.size() == 9 && out[0].cells[0].centre_y() == 77 &&
          out[0].cells[8].centre_y() == 77 + 36 * 8);
    out.clear();
    // The game menu: down from Return to Game round to Options.
    fixed_windows(1ull << 0x09, small, false, out);
    MenuNav nav;
    MenuNav::Step s = flick_at(nav, out, 320, 246, NavDirection::kDown);
    CHECK(s.moved && s.x == 320 && s.y == 150);
    // 800x600 moves everything 80 right and 60 down (the lane's frames:
    // the menu's options at 210, 259, 306; the skill tree's first at 441,98).
    const PanelShift big = panel_shift(800, 600);
    CHECK(big.left_x == 80 && big.left_y == 60 && big.right_x == 80 && big.centre_y == 60);
    out.clear();
    fixed_windows(1ull << 0x09, big, false, out);
    CHECK(out.size() == 1 && out[0].cells[0].centre_y() == 210 && out[0].cells[0].centre_x() == 400);
    out.clear();
    fixed_windows(1ull << 0x04, big, false, out);
    CHECK(out.size() == 1 && out[0].cells[0].centre_x() == 441 && out[0].cells[0].centre_y() == 98 &&
          out[0].area.left == 400);
    // The character screen: the close button only, or the four "+" too.
    out.clear();
    fixed_windows(1ull << 0x02, small, false, out);
    CHECK(out.size() == 1 && out[0].cells.size() == 1 && out[0].cells[0].centre_x() == 143 &&
          out[0].cells[0].centre_y() == 404);
    out.clear();
    fixed_windows(1ull << 0x02, small, true, out);
    CHECK(out.size() == 1 && out[0].cells.size() == 5 && out[0].cells[0].centre_x() == 140 &&
          out[0].cells[0].centre_y() == 92 && out[0].cells[3].centre_y() == 302);
    // Down from Strength's "+" to Dexterity's, and from Energy's to the close.
    s = flick_at(nav, out, 140, 92, NavDirection::kDown);
    CHECK(s.moved && s.y == 155);
    s = flick_at(nav, out, 140, 302, NavDirection::kDown);
    CHECK(s.moved && s.y == 404);
    out.clear();
    fixed_windows(1ull << 0x02, big, true, out);
    CHECK(out[0].cells[0].centre_x() == 220 && out[0].cells[0].centre_y() == 152 &&
          out[0].cells[4].centre_y() == 464);
}

// ---- the controls ----
struct FakeMenus : MenuWorld {
    std::vector<NavWindow> open;
    int asked = 0;
    bool menu_on = false;   // game_menu() reads
    GameMenu menu;
    bool on_main_menu = false;   // main_menu() says
    bool main_menu() override { return on_main_menu; }
    void windows(uint64_t, int, int, std::vector<NavWindow> &out) override
    {
        asked++;
        out = open;
    }
    bool game_menu(uint64_t ui, int, int, GameMenu &out) override
    {
        if (!menu_on || !(ui & (1ull << 0x09)))
            return false;
        out = menu;
        return true;
    }
};

static Pad pad_with(uint32_t held, int circle_x = 0, int circle_y = 0)
{
    Pad p;
    p.held = held;
    p.circle_x = circle_x;
    p.circle_y = circle_y;
    return p;
}

constexpr uint8_t VK_ESCAPE = 0x1B, VK_MENU = 0x12, VK_F2 = 0x71, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27,
                  VK_DOWN = 0x28;

// ---- D2Win's controls (out of the game), with the lane's numbers ----
static void test_d2win_cells()
{
    NavRect r;
    // The main menu's Single Player button (state 5: shown and enabled).
    D2WinControl b;
    b.type = 6, b.state = 5, b.x = 264, b.y = 324, b.width = 272, b.height = 35;
    CHECK(d2win_cell(b, 800, 600, r) && r.left == 264 && r.top == 289 && r.right == 536 && r.bottom == 324);
    // Disabled (4) or hidden (1, 0): not a stop.
    b.state = 4;
    CHECK(!d2win_cell(b, 800, 600, r));
    b.state = 1;
    CHECK(!d2win_cell(b, 800, 600, r));
    b.state = 0;
    CHECK(!d2win_cell(b, 800, 600, r));
    // The hero name box (a text field, no click handler).
    D2WinControl e;
    e.type = 1, e.state = 5, e.x = 318, e.y = 510, e.width = 157, e.height = 16;
    CHECK(d2win_cell(e, 800, 600, r) && r.left == 318 && r.top == 494 && r.bottom == 510);
    // An image: the background (no click handler) isn't; a hero is, where
    // its first frame is drawn (the Amazon standing: 25,219..143,417).
    D2WinControl bg;
    bg.type = 2, bg.state = 5, bg.x = 0, bg.y = 600, bg.width = 800, bg.height = 600;
    CHECK(!d2win_cell(bg, 800, 600, r));
    D2WinControl hero;
    hero.type = 2, hero.state = 5, hero.x = 40, hero.y = 400, hero.width = 90, hero.height = 160;
    hero.on_click = true;
    hero.frame = true, hero.frame_width = 118, hero.frame_height = 198, hero.frame_x = -15, hero.frame_y = 17;
    CHECK(d2win_cell(hero, 800, 600, r) && r.left == 25 && r.top == 219 && r.right == 143 && r.bottom == 417);
    hero.frame = false;   // the frame didn't read: the control's own box
    CHECK(d2win_cell(hero, 800, 600, r) && r.left == 40 && r.top == 240);
    // A character's slot (a text box with a click handler and lines) is;
    // an empty one, a label and a scroll bar aren't.
    D2WinControl slot;
    slot.type = 4, slot.state = 5, slot.x = 37, slot.y = 270, slot.width = 200, slot.height = 92;
    slot.on_click = true, slot.lines = 4;
    CHECK(d2win_cell(slot, 800, 600, r) && r.top == 178);
    slot.lines = 0;
    CHECK(!d2win_cell(slot, 800, 600, r));
    slot.lines = 4, slot.on_click = false;
    CHECK(!d2win_cell(slot, 800, 600, r));
    D2WinControl bar;
    bar.type = 5, bar.state = 5, bar.x = 585, bar.y = 820, bar.width = 10, bar.height = 363;
    CHECK(!d2win_cell(bar, 800, 600, r));
    // Nonsense sizes: not a stop.
    b.state = 5, b.width = 0;
    CHECK(!d2win_cell(b, 800, 600, r));
}

// The main menu as the lane read it: a flick down from Single Player goes
// to Battle.net, round from Exit to the top.
static void test_d2win_screen()
{
    const D2WinControl buttons[] = {
        {6, 5, 264, 324, 272, 35, true, 0, false, 0, 0, 0, 0},   // Single Player
        {6, 5, 264, 366, 272, 35, true, 0, false, 0, 0, 0, 0},   // Battle.net
        {6, 5, 264, 391, 272, 25, true, 0, false, 0, 0, 0, 0},   // Gateway
        {6, 5, 264, 433, 272, 35, true, 0, false, 0, 0, 0, 0},   // Other Multiplayer
        {6, 5, 264, 528, 135, 25, true, 0, false, 0, 0, 0, 0},   // Credits
        {6, 5, 402, 528, 135, 25, true, 0, false, 0, 0, 0, 0},   // Cinematics
        {6, 5, 264, 568, 272, 35, true, 0, false, 0, 0, 0, 0},   // Exit
        {6, 4, 264, 600, 272, 25, true, 0, false, 0, 0, 0, 0},   // a disabled one
    };
    NavWindow screen;
    screen.area = NavRect{0, 0, 800, 600};
    for (const D2WinControl &c : buttons) {
        NavRect r;
        if (d2win_cell(c, 800, 600, r))
            screen.cells.push_back(r);
    }
    CHECK(screen.cells.size() == 7);
    std::vector<NavWindow> ws = {screen};
    MenuNav nav;
    MenuNav::Step s = nav.step(ws, 400, 306, NavDirection::kDown, false, 800, 600);
    CHECK(s.moved && s.y == 348);   // Battle.net
    s = nav.step(ws, 400, 550, NavDirection::kDown, false, 800, 600);
    CHECK(s.moved && s.y == 306);   // Exit: round to Single Player
    s = nav.step(ws, 331, 515, NavDirection::kRight, false, 800, 600);
    CHECK(s.moved && s.x == 469 && s.y == 515);   // Credits to Cinematics
    s = nav.step(ws, 100, 100, NavDirection::kDown, false, 800, 600);
    CHECK(s.moved && s.y == 306);   // off every control: the nearest that way
}

// ---- the character screen's scroll column (D2Win, 800x600) ----
//
// As the lane read it: the slots two a row, four rows (a text box with a
// click handler, its lines 4 for a hero and fewer for an empty slot), the
// scroll bar right of them, the buttons under them; `heroes` on the page
// shown, `last` the scroll bar's last top row, `top` its top row now.
static std::vector<D2WinControl> char_screen(int heroes, int last, int top)
{
    std::vector<D2WinControl> cs;
    auto button = [&](int x, int y, int w, int h, uint32_t state) {
        D2WinControl c;
        c.type = 6, c.state = state, c.x = x, c.y = y, c.width = w, c.height = h, c.on_click = true;
        cs.push_back(c);
    };
    button(627, 572, 128, 35, 5);   // OK
    button(33, 572, 128, 35, 5);    // Exit
    button(33, 528, 168, 60, 5);    // Create
    button(233, 528, 168, 60, 4);   // Convert (disabled)
    button(433, 528, 168, 60, 5);   // Delete
    D2WinControl bar;
    bar.type = 5, bar.state = 5, bar.x = 585, bar.y = 457, bar.width = 10, bar.height = 363;
    bar.scroll_last = last, bar.scroll_top = top;
    cs.push_back(bar);
    D2WinControl hit;   // the bar's hit box: a text box without lines
    hit.type = 4, hit.state = 5, hit.x = 564, hit.y = 457, hit.width = 34, hit.height = 371, hit.on_click = true;
    cs.push_back(hit);
    for (int i = 0; i < 8; i++) {
        D2WinControl slot;
        slot.type = 4, slot.state = 5, slot.x = i % 2 ? 309 : 37, slot.y = 178 + 93 * (i / 2), slot.width = 200;
        slot.height = 92, slot.on_click = true, slot.lines = i < heroes ? 4 : 0;
        cs.push_back(slot);
    }
    return cs;
}

static NavWindow char_window(int heroes, int last, int top)
{
    const std::vector<D2WinControl> cs = char_screen(heroes, last, top);
    NavWindow w;
    w.name = "screen";
    w.area = NavRect{0, 0, 800, 600};
    for (const D2WinControl &c : cs) {
        NavRect r;
        if (d2win_cell(c, 800, 600, r))
            w.cells.push_back(r);
    }
    d2win_scroll(cs, w.cells, 800, 600, w.scroll);
    return w;
}

static bool at(const MenuNav::Step &s, int x, int y) { return s.moved && s.x == x && s.y == y; }

static void test_char_screen_scroll()
{
    // 18 heroes (9 rows), the list at its top.
    NavWindow w = char_window(8, 5, 0);
    CHECK(w.cells.size() == 12 && w.scroll.present && w.scroll.slots.size() == 8);
    CHECK(w.scroll.up.left == 585 && w.scroll.up.top == 94 && w.scroll.up.right == 595 && w.scroll.up.bottom == 105);
    CHECK(w.scroll.down.left == 585 && w.scroll.down.top == 448 && w.scroll.down.right == 595 &&
          w.scroll.down.bottom == 457);
    CHECK(!w.scroll.up_on && w.scroll.down_on);
    CHECK(w.scroll.below.left == 433 && w.scroll.below.top == 468);   // Delete
    const int up_x = 590, up_y = 99, dn_x = 590, dn_y = 452, del_x = 517, del_y = 498;
    std::vector<NavWindow> ws = {w};
    MenuNav nav;
    // Right from the right column: the active button (only down at the top), whatever the row.
    CHECK(at(nav.step(ws, 409, 132, NavDirection::kRight, false, 800, 600), dn_x, dn_y));
    CHECK(at(nav.step(ws, 409, 411, NavDirection::kRight, false, 800, 600), dn_x, dn_y));
    // From the left column: the slot beside it, as before.
    CHECK(at(nav.step(ws, 137, 225, NavDirection::kRight, false, 800, 600), 409, 225));
    // Up and down in the grid: as before (the bottom row down to Delete, the top row round).
    CHECK(at(nav.step(ws, 409, 411, NavDirection::kDown, false, 800, 600), del_x, del_y));
    MenuNav::Step s = nav.step(ws, 409, 132, NavDirection::kUp, false, 800, 600);
    CHECK(s.moved && !(s.x == up_x && s.y == up_y));
    // The column round with only down active: down -> Delete -> down.
    CHECK(at(nav.step(ws, dn_x, dn_y, NavDirection::kDown, false, 800, 600), del_x, del_y));
    CHECK(at(nav.step(ws, del_x, del_y, NavDirection::kDown, false, 800, 600), dn_x, dn_y));
    CHECK(at(nav.step(ws, dn_x, dn_y, NavDirection::kUp, false, 800, 600), del_x, del_y));
    CHECK(at(nav.step(ws, del_x, del_y, NavDirection::kUp, false, 800, 600), dn_x, dn_y));
    // OK stays right of Delete.
    CHECK(at(nav.step(ws, del_x, del_y, NavDirection::kRight, false, 800, 600), 691, 554));
    // Left from a button: the slot nearest in height (the right one).
    CHECK(at(nav.step(ws, dn_x, dn_y, NavDirection::kLeft, false, 800, 600), 409, 411));

    // In the middle of the list: both active; the nearer one in height.
    ws = {char_window(8, 5, 2)};
    CHECK(ws[0].scroll.up_on && ws[0].scroll.down_on);
    CHECK(at(nav.step(ws, 409, 132, NavDirection::kRight, false, 800, 600), up_x, up_y));
    CHECK(at(nav.step(ws, 409, 225, NavDirection::kRight, false, 800, 600), up_x, up_y));
    CHECK(at(nav.step(ws, 409, 318, NavDirection::kRight, false, 800, 600), dn_x, dn_y));
    CHECK(at(nav.step(ws, 409, 411, NavDirection::kRight, false, 800, 600), dn_x, dn_y));
    // Round the column: up -> down -> Delete -> up; and back the other way.
    CHECK(at(nav.step(ws, up_x, up_y, NavDirection::kDown, false, 800, 600), dn_x, dn_y));
    CHECK(at(nav.step(ws, dn_x, dn_y, NavDirection::kDown, false, 800, 600), del_x, del_y));
    CHECK(at(nav.step(ws, del_x, del_y, NavDirection::kDown, false, 800, 600), up_x, up_y));
    CHECK(at(nav.step(ws, up_x, up_y, NavDirection::kUp, false, 800, 600), del_x, del_y));
    CHECK(at(nav.step(ws, del_x, del_y, NavDirection::kUp, false, 800, 600), dn_x, dn_y));
    CHECK(at(nav.step(ws, dn_x, dn_y, NavDirection::kUp, false, 800, 600), up_x, up_y));
    CHECK(at(nav.step(ws, up_x, up_y, NavDirection::kLeft, false, 800, 600), 409, 132));

    // At the end of 17 heroes: the last row has one (the left slot); only up active.
    ws = {char_window(7, 5, 5)};
    CHECK(ws[0].scroll.up_on && !ws[0].scroll.down_on && ws[0].scroll.slots.size() == 7);
    CHECK(at(nav.step(ws, 409, 318, NavDirection::kRight, false, 800, 600), up_x, up_y));
    CHECK(at(nav.step(ws, up_x, up_y, NavDirection::kLeft, false, 800, 600), 409, 132));
    CHECK(at(nav.step(ws, up_x, up_y, NavDirection::kDown, false, 800, 600), del_x, del_y));
    CHECK(at(nav.step(ws, del_x, del_y, NavDirection::kDown, false, 800, 600), up_x, up_y));
    // The left slot alone in its row isn't the right column: as before.
    const MenuNav::Step lone = nav.step(ws, 137, 411, NavDirection::kRight, false, 800, 600);
    CHECK(!(lone.moved && lone.x == up_x && lone.y == up_y));

    // A list that fits (2, 3, 5, 8 heroes): no button active, every flick as without the column.
    for (int heroes : {2, 3, 5, 8}) {
        NavWindow with = char_window(heroes, 0, 0), without = with;
        without.scroll = NavScroll();
        CHECK(with.scroll.present && !with.scroll.any_on() && (int)with.scroll.slots.size() == heroes);
        const std::vector<NavWindow> a = {with}, b = {without};
        for (const NavRect &c : with.cells)
            for (int d = 0; d < 4; d++) {
                MenuNav n1, n2;
                const MenuNav::Step s1 = n1.step(a, c.centre_x(), c.centre_y(), (NavDirection)d, false, 800, 600);
                const MenuNav::Step s2 = n2.step(b, c.centre_x(), c.centre_y(), (NavDirection)d, false, 800, 600);
                CHECK(s1.moved == s2.moved && s1.x == s2.x && s1.y == s2.y);
                // ...and never onto an empty slot.
                if (s1.moved)
                    for (int i = heroes; i < 8; i++)
                        CHECK(!(s1.x == (i % 2 ? 409 : 137) && s1.y == 132 + 93 * (i / 2)));
            }
    }
    // No scroll bar: no column.
    std::vector<D2WinControl> cs = char_screen(8, 5, 0);
    cs.erase(cs.begin() + 5);
    NavScroll none;
    CHECK(!d2win_scroll(cs, {}, 800, 600, none) && !none.present);
}

// The controls on the character screen: a flick right onto the down
// button; at the list's end (the button inactive) the cursor goes back to
// the slot nearest in height, but not while A is down.
static void test_controls_scroll_button()
{
    FakeMenus menus;
    menus.open = {char_window(8, 5, 4)};
    Controls c;
    c.set_frame(800, 600);
    c.set_menu_nav(&menus);
    c.set_context(false, true, 0, false);
    c.warp_cursor(409, 411);
    uint64_t t = 1000;
    c.update(pad_with(0), t += 16);
    c.update(pad_with(0, 156, 0), t += 16);   // a flick right
    c.update(pad_with(0), t += 16);
    CHECK(c.cursor_x() == 590 && c.cursor_y() == 452);
    // A clicks it; the list reaches its end while A is down: the cursor stays.
    c.update(pad_with(kA), t += 16);
    CHECK(g_left);
    menus.open = {char_window(8, 5, 5)};
    c.update(pad_with(kA), t += 16);
    CHECK(c.cursor_x() == 590 && c.cursor_y() == 452);
    c.update(pad_with(0), t += 16);
    c.update(pad_with(0), t += 16);
    c.update(pad_with(0), t += 16);
    CHECK(!g_left && c.cursor_x() == 409 && c.cursor_y() == 411);
    // An active button the cursor is on stays.
    menus.open = {char_window(8, 5, 3)};
    c.warp_cursor(590, 99);
    for (int i = 0; i < 4; i++)
        c.update(pad_with(0), t += 16);
    CHECK(c.cursor_x() == 590 && c.cursor_y() == 99);
}

// ---- the game menu's rows ----
static void test_keyboard_pass_through()
{
    Pad p;
    p.held = kA | kB | kY | kStart | kSelect | kX | kUp;
    p.circle_x = 100, p.circle_y = -50, p.cstick_x = 20, p.cstick_y = 30;
    p.touching = true, p.touch_x = 10, p.touch_y = 20, p.pointing = true, p.point_left = true;
    const Pad g = keyboard_pass_through(p);
    CHECK(g.held == (kA | kX | kUp));
    CHECK(g.circle_x == 100 && g.circle_y == -50 && g.cstick_x == 20 && g.cstick_y == 30);
    CHECK(!g.touching && !g.pointing && !g.point_left && !g.touchpad);
}

static void test_b_out_of_game()
{
    const D2WinControl main_menu[] = {
        {6, 5, 264, 324, 272, 35, true, 0, false, 0, 0, 0, 0},   // Single Player
        {6, 5, 264, 433, 272, 35, true, 0, false, 0, 0, 0, 0},   // Other Multiplayer
        {6, 5, 264, 568, 272, 35, true, 0, false, 0, 0, 0, 0},   // Exit
    };
    CHECK(d2win_main_menu(std::vector<D2WinControl>(main_menu, main_menu + 3)));
    CHECK(!d2win_main_menu(std::vector<D2WinControl>(main_menu, main_menu + 2)));   // no Exit there
    CHECK(!d2win_main_menu(char_screen(3, 0, 0)));   // character select
    CHECK(!d2win_main_menu({}));
    FakeMenus menus;
    Controls c;
    c.set_frame(800, 600);
    c.set_menu_nav(&menus);
    c.set_context(false, false, 0, false);
    uint64_t t = 1000;
    // Character select (or the class screen): B is Escape while held, never the right button.
    c.update(pad_with(kB), t += 16);
    CHECK(g_keys[VK_ESCAPE] && !g_right);
    c.update(pad_with(0), t += 16);
    CHECK(!g_keys[VK_ESCAPE]);
    // The main menu: nothing (its Escape would quit the game).
    menus.on_main_menu = true;
    c.update(pad_with(kB), t += 16);
    CHECK(!g_keys[VK_ESCAPE] && !g_right);
    c.update(pad_with(0), t += 16);
    // R + B is still R's: no Escape.
    menus.on_main_menu = false;
    c.update(pad_with(kR | kB), t += 16);
    CHECK(!g_keys[VK_ESCAPE]);
    c.update(pad_with(0), t += 16);
    // In the game with no panel: B is the right button as before.
    c.set_context(true, false, 0, false);
    c.update(pad_with(kB), t += 16);
    CHECK(!g_keys[VK_ESCAPE] && g_right);
    c.update(pad_with(0), t += 16);
}

// ---- a panel on one side: the walk and the auto-target round the hero
// where the game draws him (the free half's middle), not the frame's ----
static void test_weapon_tabs()
{
    NavWindow w = inventory();
    const NavRect right_arm = w.cells[40], left_arm = w.cells[42];   // the weapon boxes drawn left and right
    const NavRect tab_l = weapon_tab(right_arm), tab_r = weapon_tab(left_arm);
    CHECK(tab_l.left == 340 && tab_l.right == 395 && tab_l.top == 25 && tab_l.bottom == 44);
    CHECK(tab_r.left == 571 && tab_r.right == 626);
    w.cells.push_back(tab_l);
    w.cells.push_back(tab_r);
    std::vector<NavWindow> ws = {w};
    MenuNav nav;
    // Up from the left weapon: onto its tab; down from the tab: back.
    MenuNav::Step s = flick_at(nav, ws, right_arm.centre_x(), right_arm.centre_y(), NavDirection::kUp);
    CHECK(at(s, tab_l.centre_x(), tab_l.centre_y()));
    s = flick_at(nav, ws, tab_l.centre_x(), tab_l.centre_y(), NavDirection::kDown);
    CHECK(at(s, right_arm.centre_x(), right_arm.centre_y()));
    // The right weapon's tab, the same way.
    s = flick_at(nav, ws, left_arm.centre_x(), left_arm.centre_y(), NavDirection::kUp);
    CHECK(at(s, tab_r.centre_x(), tab_r.centre_y()));
    s = flick_at(nav, ws, tab_r.centre_x(), tab_r.centre_y(), NavDirection::kDown);
    CHECK(at(s, left_arm.centre_x(), left_arm.centre_y()));
}

int main()
{
    test_direction();
    test_one_cell();
    test_round_the_edges();
    test_next_window();
    test_free_half();
    test_npc_menu();
    test_fixed_windows();
    test_d2win_cells();
    test_d2win_screen();
    test_char_screen_scroll();
    test_controls_scroll_button();
    test_b_out_of_game();
    test_weapon_tabs();
    test_keyboard_pass_through();
    if (g_fail) {
        std::printf("%d checks FAILED\n", g_fail);
        return 1;
    }
    std::printf("menu: all tests passed\n");
    return 0;
}
