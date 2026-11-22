// Menu navigation (src/game/menu_nav.h): a flick moves the cursor one cell,
// round a window's edge in the same row or column, into the window next to
// it, to the free half with an item on the cursor and back, an NPC's menu
// up and down only; the 640x480 windows and the NPC menu's options; and the
// controls around it (src/game/controls.cpp): a slow tilt still moves the
// cursor freely, B is Escape with a panel open, the other buttons as
// before, and with one side panel open a held stick walks (the auto-target's
// walk, on the free half). Made-up windows with the game's own numbers (inventory.txt's
// rows as the lane read them) stand in for the game. Run through
// `scripts/docker.sh test menu`.
#include "game/controls.h"
#include "game/menu_nav.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
static std::string g_typed;          // the text typed: digits, and '<' for a Backspace
static bool g_ever[256] = {};        // keys that went down since the last clear
namespace rt {
void input_button(bool right, bool down) { (right ? g_right : g_left) = down; }
void input_key(uint8_t vk, bool down)
{
    g_keys[vk] = down;
    if (down)
        g_ever[vk] = true;
    if (down && vk == 0x08)
        g_typed += '<';
}
void input_char(uint32_t ch) { g_typed += (char)ch; }
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
    CHECK(out.size() == 2 && out[0].cells.size() == 22 && out[1].cells.size() == 7);
    // Their close buttons, last.
    CHECK(out[0].cells[21].centre_x() == 507 && out[0].cells[21].centre_y() == 401);
    CHECK(out[1].cells[6].centre_x() == 291 && out[1].cells[6].centre_y() == 404);
    out.clear();
    fixed_windows(1ull << 0x14, small, false, out);
    CHECK(out.size() == 1 && out[0].cells.size() == 10 && out[0].cells[0].centre_y() == 77 &&
          out[0].cells[8].centre_y() == 77 + 36 * 8 && !out[0].up_down_only);
    {
        // Down from the last waypoint: its close button; down again: round to the first.
        MenuNav wp;
        MenuNav::Step d = flick_at(wp, out, 158, 77 + 36 * 8, NavDirection::kDown);
        CHECK(d.moved && d.x == 288 && d.y == 401);
        d = flick_at(wp, out, 288, 401, NavDirection::kDown);
        CHECK(d.moved && d.y == 77);
    }
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
    bool gold_on = false;   // gold_dialog() reads
    GoldDialog gold;
    bool gold_dialog(uint64_t, int, int, GoldDialog &out) override
    {
        if (gold_on)
            out = gold;
        return gold_on;
    }
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
static void test_game_menu_rows()
{
    GameMenu m;
    game_menu_rows(3, 50, nullptr, 640, 480, m);
    CHECK(m.rows.size() == 3 && m.rows[0].rect.centre_y() == 149 && m.rows[1].rect.centre_y() == 199 &&
          m.rows[2].rect.centre_y() == 249 && m.rows[1].rect.centre_x() == 320);
    game_menu_rows(3, 50, nullptr, 800, 600, m);
    CHECK(m.rows[0].rect.centre_y() == 209 && m.rows[0].rect.centre_x() == 400);
    // Sound options: eight rows 45 apart, a title first.
    const int8_t kinds[8] = {GameMenu::kTitle, GameMenu::kSlider, GameMenu::kSlider, GameMenu::kChoice,
                             GameMenu::kChoice, GameMenu::kChoice, GameMenu::kChoice, GameMenu::kButton};
    game_menu_rows(8, 45, kinds, 640, 480, m);
    CHECK(m.rows.size() == 8 && m.rows[0].kind == GameMenu::kTitle && m.rows[1].kind == GameMenu::kSlider);
    CHECK(std::abs(m.rows[0].rect.centre_y() - 41) <= 1 && std::abs(m.rows[7].rect.centre_y() - 356) <= 1);
    CHECK(m.rows[0].rect.bottom <= m.rows[1].rect.top);   // rows don't overlap
    game_menu_rows(0, 50, nullptr, 640, 480, m);
    CHECK(m.rows.empty());
}

// The controls in the game menu: a tilt is the arrow key, the cursor
// follows the game's lit row; Left/Right on a choice; B is "Previous" in a
// submenu, Escape at the top; Configure Controls a free cursor.
static void test_controls_game_menu()
{
    FakeMenus menus;
    menus.menu_on = true;
    const int8_t kinds[5] = {GameMenu::kButton, GameMenu::kChoice, GameMenu::kSlider, GameMenu::kButton,
                             GameMenu::kButton};
    game_menu_rows(5, 50, kinds, 640, 480, menus.menu);
    menus.menu.lit = 4;
    menus.menu.previous = 4;
    Controls c;
    c.set_frame(640, 480);
    c.set_menu_nav(&menus);
    const uint64_t ui = 1ull | 1ull << 0x09;
    c.set_context(true, true, ui, false);
    c.warp_cursor(600, 420);
    uint64_t t = 1000;
    // Opened: the cursor onto the lit row (the last).
    c.update(pad_with(0), t);
    CHECK(c.cursor_x() == 320 && c.cursor_y() == menus.menu.rows[4].rect.centre_y());
    // A tilt down: Down, and the stick doesn't move the cursor.
    c.update(pad_with(0, 0, -156), t += 16);
    CHECK(g_keys[VK_DOWN] && c.cursor_y() == menus.menu.rows[4].rect.centre_y());
    for (int i = 0; i < 6; i++)
        c.update(pad_with(0, 0, -156), t += 16);   // held out: one key only, up after 80 ms
    CHECK(!g_keys[VK_DOWN]);
    menus.menu.lit = 0;   // the game moved its light, round to the top
    c.update(pad_with(0), t += 16);
    CHECK(c.cursor_x() == 320 && c.cursor_y() == menus.menu.rows[0].rect.centre_y());
    // Left on a button: nothing to change.
    c.update(pad_with(0, -156, 0), t += 16);
    CHECK(!g_keys[VK_LEFT]);
    c.update(pad_with(0), t += 16);
    // Down again, the game lights the choice; Right changes it.
    c.update(pad_with(0, 0, -156), t += 16);
    CHECK(g_keys[VK_DOWN]);
    c.update(pad_with(0), t += 100);
    menus.menu.lit = 1;
    c.update(pad_with(0), t += 16);
    CHECK(c.cursor_y() == menus.menu.rows[1].rect.centre_y() && !g_keys[VK_DOWN]);
    c.update(pad_with(0, 156, 0), t += 16);
    CHECK(g_keys[VK_RIGHT] && c.cursor_y() == menus.menu.rows[1].rect.centre_y());
    c.update(pad_with(0), t += 100);
    CHECK(!g_keys[VK_RIGHT]);
    // Up with a light the game didn't move: the cursor goes onto it anyway
    // after a moment.
    c.update(pad_with(0, 0, 156), t += 16);
    CHECK(g_keys[VK_UP]);
    c.update(pad_with(0), t += 400);
    c.update(pad_with(0), t += 16);
    CHECK(c.cursor_y() == menus.menu.rows[1].rect.centre_y());
    // B: a click on Previous, not Escape: the cursor there, the button a
    // frame later, up again after a moment.
    c.update(pad_with(kB), t += 16);
    CHECK(!g_keys[VK_ESCAPE] && !g_right && !g_left && c.cursor_y() == menus.menu.rows[4].rect.centre_y());
    c.update(pad_with(kB), t += 50);
    CHECK(g_left && !g_right);
    c.update(pad_with(0), t += 50);
    CHECK(g_left);
    c.update(pad_with(0), t += 100);
    CHECK(!g_left && !g_keys[VK_ESCAPE]);
    // The top menu (no Previous): B is Escape.
    game_menu_rows(3, 50, nullptr, 640, 480, menus.menu);
    menus.menu.previous = -1;
    menus.menu.lit = 2;
    c.update(pad_with(0), t += 16);
    CHECK(c.cursor_y() == 249);   // another menu: onto its lit row
    c.update(pad_with(kB), t += 16);
    CHECK(g_keys[VK_ESCAPE] && !g_left);
    c.update(pad_with(0), t += 16);
    CHECK(!g_keys[VK_ESCAPE]);
    // Configure Controls (0x0B, 0x09 off): the stick moves the cursor
    // freely, no keys, no steps; B is Escape (back to Options).
    c.set_context(true, true, 1ull | 1ull << 0x0B, false);
    const int asked = menus.asked, x0 = c.cursor_x();
    for (int i = 0; i < 20; i++)
        c.update(pad_with(0, 156, 0), t += 16);
    c.update(pad_with(0), t += 16);
    CHECK(c.cursor_x() > x0 + 40 && menus.asked == asked && !g_keys[VK_RIGHT]);
    c.update(pad_with(kB), t += 16);
    CHECK(g_keys[VK_ESCAPE]);
    c.update(pad_with(0), t += 16);
    // The game menu's rows don't read: the fixed windows' flicks, as before.
    menus.menu_on = false;
    c.set_context(true, true, ui, false);
    c.update(pad_with(0, 0, -156), t += 16);
    c.update(pad_with(0), t += 16);
    CHECK(!g_keys[VK_DOWN] && menus.asked == asked + 1);
}

// ---- one-panel walk and related controls (FakeAim is the made-up world) ----
struct FakeAim : AimWorld {
    std::vector<AimUnit> monsters, ground;
    AimView v;
    bool view(AimView &out) override
    {
        out = v;
        return true;
    }
    void units(std::vector<AimUnit> &out) override { out.insert(out.end(), monsters.begin(), monsters.end()); }
    bool unit(uint32_t id, AimUnit &out) override
    {
        for (const AimUnit &u : monsters)
            if (u.id == id) {
                out = u;
                return true;
            }
        return false;
    }
    void ground_units(std::vector<AimUnit> &out) override { out.insert(out.end(), ground.begin(), ground.end()); }
    // Warp classes with a known click rectangle (LvlWarp.txt's): class 7 only.
    bool warp_area(uint32_t cls, int &left, int &top, int &width, int &height) override
    {
        if (cls != 7)
            return false;
        left = -60, top = -60, width = 40, height = 60;
        return true;
    }
};

static AimUnit npc_at(uint32_t id, int x, int y)
{
    AimUnit u;
    u.type = 1, u.id = id, u.mode = 1, u.feet_x = x, u.feet_y = y;
    return u;
}

// ---- a panel on one side: a flick steps, the stick held walks ----
static void test_panel_hold_walks()
{
    CHECK(one_side_panel(1ull | 1ull << 0x01) == 1 && one_side_panel(1ull << 0x04) == 1);   // inventory, skills
    CHECK(one_side_panel(1ull << 0x02) == -1 && one_side_panel(1ull << 0x0F) == -1);        // character, quests
    CHECK(one_side_panel(1ull << 0x02 | 1ull << 0x01) == 0);   // both halves
    CHECK(one_side_panel(1ull << 0x01 | 1ull << 0x19) == 0 && one_side_panel(1ull << 0x01 | 1ull << 0x0C) == 0);
    CHECK(one_side_panel(1ull << 0x09) == 0 && one_side_panel(1ull << 0x08) == 0 && one_side_panel(0) == 0);
    FakeMenus menus;
    menus.open = {inventory()};
    FakeAim world;
    world.v.width = 640, world.v.height = 480, world.v.hero_x = 320, world.v.hero_y = 240, world.v.hero_mode = 1;
    Controls c;
    c.set_frame(640, 480);
    c.set_menu_nav(&menus);
    c.set_auto_target(AimSettings(), &world);   // aim_panel_walk_ms 280
    c.set_context(true, true, 1ull | 1ull << 0x01, false);   // the inventory (the right half)
    int x, y;
    inv_cell(3, 1, x, y);
    c.warp_cursor(x, y);
    uint64_t t = 1000;
    c.update(pad_with(0), t);
    // A short flick: one cell, no walk.
    c.update(pad_with(0, 156, 0), t += 16);
    c.update(pad_with(0, 156, 0), t += 16);
    c.update(pad_with(0), t += 16);
    int ex, ey;
    inv_cell(4, 1, ex, ey);
    CHECK(c.cursor_x() == ex && c.cursor_y() == ey && !g_left);
    // Held out left (towards the free half) past 280 ms: walking, the left
    // button held, the point on the free (left) half.
    for (int i = 0; i < 12; i++)   // 192 ms: not yet
        c.update(pad_with(0, -156, 0), t += 16);
    CHECK(!g_left);
    for (int i = 0; i < 8; i++)
        c.update(pad_with(0, -156, 0), t += 16);
    CHECK(g_left && c.cursor_x() < 320 && c.cursor_x() == 320 - 88 && c.cursor_y() == 225);
    // Towards the panel (right): the point stays off its half.
    for (int i = 0; i < 4; i++)
        c.update(pad_with(0, 156, 0), t += 16);
    CHECK(g_left && c.cursor_x() <= 320 - 5);
    // Let go: the stop click (ahead of his middle the way he walked, off
    // the panel's half), then up; and a flick steps cells again.
    c.update(pad_with(0), t += 16);
    CHECK(g_left && c.cursor_x() == 315 && c.cursor_y() == 232);
    c.update(pad_with(0), t += 150);
    c.update(pad_with(0), t += 16);
    CHECK(!g_left);
    c.warp_cursor(ex, ey);
    c.update(pad_with(0), t += 16);
    c.update(pad_with(0, 156, 0), t += 16);
    c.update(pad_with(0), t += 16);
    inv_cell(5, 1, x, y);
    CHECK(c.cursor_x() == x && c.cursor_y() == y && !g_left);
    // A left panel (the character sheet): the point stays on the right half.
    c.set_context(true, true, 1ull | 1ull << 0x02, false);
    for (int i = 0; i < 25; i++)
        c.update(pad_with(0, -156, 0), t += 16);
    CHECK(g_left && c.cursor_x() >= 324);
    c.update(pad_with(0), t += 16);
    c.update(pad_with(0), t += 40);   // the stop click
    c.update(pad_with(0), t += 200);
    CHECK(!g_left);
    // Both halves open (the character sheet and the inventory): held, no walk.
    c.set_context(true, true, 1ull | 1ull << 0x02 | 1ull << 0x01, false);
    for (int i = 0; i < 40; i++) {
        c.update(pad_with(0, -156, 0), t += 16);
        CHECK(!g_left);
    }
    c.update(pad_with(0), t += 16);
    // An item on the cursor: no walk (a click would drop it).
    c.set_context(true, true, 1ull | 1ull << 0x01, true);
    for (int i = 0; i < 40; i++) {
        c.update(pad_with(0, -156, 0), t += 16);
        CHECK(!g_left);
    }
    c.update(pad_with(0), t += 16);
    // aim_panel_walk_ms=0: never.
    AimSettings never;
    never.panel_walk_ms = 0;
    c.set_auto_target(never, &world);
    c.set_context(true, true, 1ull | 1ull << 0x01, false);
    for (int i = 0; i < 40; i++) {
        c.update(pad_with(0, -156, 0), t += 16);
        CHECK(!g_left);
    }
    c.update(pad_with(0), t += 16);
    // Another threshold: 500 ms.
    AimSettings slow;
    slow.panel_walk_ms = 500;
    c.set_auto_target(slow, &world);
    for (int i = 0; i < 25; i++)   // 400 ms
        c.update(pad_with(0, -156, 0), t += 16);
    CHECK(!g_left);
    for (int i = 0; i < 10; i++)
        c.update(pad_with(0, -156, 0), t += 16);
    CHECK(g_left);
    c.update(pad_with(0), t += 16);
    c.update(pad_with(0), t += 200);
}

// ---- A on the control panel: a plain click, no auto-target ----
static void test_control_panel_plain_click()
{
    CHECK(control_panel_at(182, 461, 640, 480));    // run / walk
    CHECK(control_panel_at(320, 470, 640, 480));    // the bar
    CHECK(control_panel_at(60, 400, 640, 480));     // the life globe
    CHECK(control_panel_at(600, 400, 640, 480));    // the mana globe
    CHECK(control_panel_at(300, 420, 640, 480));    // the mini-panel
    CHECK(!control_panel_at(320, 240, 640, 480));   // the world
    CHECK(!control_panel_at(320, 400, 640, 480));
    CHECK(control_panel_at(400, 580, 800, 600) && control_panel_at(30, 520, 800, 600) &&
          control_panel_at(250, 540, 800, 600) && !control_panel_at(400, 500, 800, 600));
    FakeAim world;
    world.v.width = 640, world.v.height = 480, world.v.hero_x = 320, world.v.hero_y = 240;
    AimUnit enemy = npc_at(21, 320, 300);   // a monster, as tests/aim makes one
    enemy.cls = 19, enemy.flags_a = 0x0000000F, enemy.flags_b = 0x80;
    enemy.monstats2_flags = aim_bits::kMon2CorpseSelect;
    enemy.distance = 3;
    world.monsters = {enemy};
    world.v.hero_mode = 1;   // standing
    FakeMenus menus;
    Controls c;
    c.set_frame(640, 480);
    c.set_auto_target(AimSettings(), &world);
    c.set_menu_nav(&menus);
    c.set_context(true, false, 1, false);
    uint64_t t = 1000;
    // In the world the auto-target has A: the cursor goes onto the enemy.
    c.warp_cursor(320, 200);
    c.update(pad_with(0), t);
    for (int i = 0; i < 3; i++)
        c.update(pad_with(kA), t += 16);
    CHECK(c.cursor_x() != 320 || c.cursor_y() != 200);
    c.update(pad_with(0), t += 16);
    t += 5000;
    c.warp_cursor(182, 461);
    c.update(pad_with(0), t);
    for (int i = 0; i < 5; i++) {
        c.update(pad_with(kA), t += 16);
        CHECK(g_left && c.cursor_x() == 182 && c.cursor_y() == 461);
    }
    c.update(pad_with(0), t += 16);
    CHECK(!g_left);
    // B too.
    c.update(pad_with(kB), t += 16);
    CHECK(g_right && c.cursor_x() == 182 && c.cursor_y() == 461);
    c.update(pad_with(0), t += 16);
}

// ---- the on-screen keyboard open: the game still gets the sticks and A ----
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

static void test_controls()
{
    FakeMenus menus;
    menus.open = {inventory()};
    Controls c;
    c.set_frame(640, 480);
    c.set_menu_nav(&menus);
    c.set_context(true, true, 1ull << 0x01, false);
    int x, y, ex, ey;
    inv_cell(3, 1, x, y);
    c.warp_cursor(x, y);
    uint64_t t = 1000;
    c.update(pad_with(0), t);
    // A flick right (out at full tilt, back): one cell right.
    c.update(pad_with(0, 156, 0), t += 16);
    c.update(pad_with(0, 156, 0), t += 16);
    c.update(pad_with(0), t += 16);
    inv_cell(4, 1, ex, ey);
    CHECK(c.cursor_x() == ex && c.cursor_y() == ey);
    // A flick held out: one cell, and the cursor stays there until the
    // stick is back in the centre.
    c.update(pad_with(0, 156, 0), t += 16);
    for (int i = 0; i < 10; i++)
        c.update(pad_with(0, 156, 0), t += 16);
    c.update(pad_with(0), t += 16);
    inv_cell(5, 1, ex, ey);
    CHECK(c.cursor_x() == ex && c.cursor_y() == ey);
    inv_cell(4, 1, ex, ey);
    c.update(pad_with(0, -156, 0), t += 16);
    c.update(pad_with(0), t += 16);
    CHECK(c.cursor_x() == ex && c.cursor_y() == ey);
    // A flick up (the pad's y up): one cell up.
    c.update(pad_with(0, 0, 156), t += 16);
    c.update(pad_with(0), t += 16);
    inv_cell(4, 0, x, y);
    CHECK(c.cursor_x() == x && c.cursor_y() == y);
    // A slow tilt with the panel walk off (aim_panel_walk_ms=0): the cursor
    // moves freely, no cell steps.
    AimSettings no_walk;
    no_walk.panel_walk_ms = 0;
    c.set_auto_target(no_walk, nullptr);
    const int asked = menus.asked;
    for (int i = 1; i <= 70; i++)   // from 20% of the tilt to 40% in 190 ms: slower than a flick (150)
        c.update(pad_with(0, 156 * (i < 60 ? i : 60) / 60, 0), t += 16);
    c.update(pad_with(0), t += 16);
    CHECK(menus.asked == asked && c.cursor_x() > x + 40 && c.cursor_y() == y && !g_left);
    // B: Escape, not the right button.
    c.update(pad_with(kB), t += 16);
    CHECK(g_keys[VK_ESCAPE] && !g_right);
    c.update(pad_with(0), t += 16);
    CHECK(!g_keys[VK_ESCAPE]);
    // A: the left button, where the cursor is.
    const int cx = c.cursor_x();
    c.update(pad_with(kA), t += 16);
    CHECK(g_left && c.cursor_x() == cx);
    c.update(pad_with(0), t += 16);
    CHECK(!g_left);
    // ZR + B: still F2; R + B: still the skill tree (T).
    c.update(pad_with(kZR | kB), t += 16);
    CHECK(g_keys[VK_F2] && !g_keys[VK_ESCAPE]);
    c.update(pad_with(0), t += 16);
    c.update(pad_with(kR | kB), t += 16);
    CHECK(g_keys['T'] && !g_keys[VK_ESCAPE]);
    c.update(pad_with(0), t += 16);
    // X: still the inventory (I).
    c.update(pad_with(kX), t += 16);
    CHECK(g_keys['I']);
    c.update(pad_with(0), t += 16);
    // Y with the inventory open: still sells or drops (the item picked up
    // first: the left button), not Alt.
    c.update(pad_with(kY), t += 16);
    c.update(pad_with(kY), t += 100);
    CHECK(!g_keys[VK_MENU]);
    c.update(pad_with(0), t += 100);
    for (int i = 0; i < 20; i++)
        c.update(pad_with(0), t += 100);
    // No panel: B is the right button again, and the stick walks.
    c.set_context(true, false, 0, false);
    c.update(pad_with(kB), t += 16);
    CHECK(g_right && !g_keys[VK_ESCAPE]);
    c.update(pad_with(0), t += 16);
    CHECK(!g_right);
    // Out of the game (D2Win's screens): B is Escape (one screen back,
    // test_b_out_of_game); a flick steps over the screen's controls.
    c.set_context(false, true, 0, false);
    c.update(pad_with(kB), t += 16);
    CHECK(!g_right && g_keys[VK_ESCAPE]);
    c.update(pad_with(0), t += 16);
    const int before = menus.asked;
    c.update(pad_with(0, 156, 0), t += 16);
    c.update(pad_with(0), t += 16);
    CHECK(menus.asked == before + 1);
    // No menu world (the default): B is the right button with a panel open.
    Controls plain;
    plain.set_frame(640, 480);
    plain.set_context(true, true, 1ull << 0x01, false);
    plain.update(pad_with(kB), 0);
    CHECK(g_right && !g_keys[VK_ESCAPE]);
    plain.update(pad_with(0), 16);
}

static void test_controls_holding()
{
    FakeMenus menus;
    menus.open = {inventory()};
    Controls c;
    c.set_frame(640, 480);
    c.set_menu_nav(&menus);
    c.set_context(true, true, 1ull << 0x01, true);
    int x, y;
    inv_cell(0, 2, x, y);
    c.warp_cursor(x, y);
    uint64_t t = 1000;
    c.update(pad_with(0), t);
    c.update(pad_with(0, -156, 0), t += 16);
    c.update(pad_with(0), t += 16);
    CHECK(c.cursor_x() == 160 && c.cursor_y() == 240);
    c.update(pad_with(0, 156, 0), t += 16);
    c.update(pad_with(0), t += 16);
    CHECK(c.cursor_x() == x && c.cursor_y() == y);
    // Nowhere to go (no windows): the cursor stays where the flick began.
    menus.open.clear();
    c.update(pad_with(0, 156, 0), t += 16);
    c.update(pad_with(0, 156, 0), t += 16);
    c.update(pad_with(0), t += 16);
    CHECK(c.cursor_x() == x && c.cursor_y() == y);
}

// ---- B out of the game: Escape (one screen back), not on the main menu ----
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
static void test_panel_side_hero_place()
{
    FakeMenus menus;
    menus.open = {inventory()};
    FakeAim world;
    // The inventory open on the right: the game draws the hero at 160 across.
    world.v.width = 640, world.v.height = 480, world.v.hero_x = 160, world.v.hero_y = 240, world.v.hero_mode = 1;
    Controls c;
    c.set_frame(640, 480);
    c.set_menu_nav(&menus);
    c.set_auto_target(AimSettings(), &world);
    c.set_context(true, true, 1ull | 1ull << 0x01, false);
    int x, y;
    inv_cell(3, 1, x, y);
    c.warp_cursor(x, y);
    uint64_t t = 1000;
    c.update(pad_with(0), t);
    // Held right (towards the panel) past 280 ms: the hero walks right, the
    // point right of him, still off the panel's half.
    for (int i = 0; i < 25; i++)
        c.update(pad_with(0, 156, 0), t += 16);
    CHECK(g_left && c.cursor_x() == 160 + 88 && c.cursor_x() < 320);
    // Held left: left of him.
    for (int i = 0; i < 4; i++)
        c.update(pad_with(0, -156, 0), t += 16);
    CHECK(g_left && c.cursor_x() == 160 - 88);
    c.update(pad_with(0), t += 16);
    CHECK(g_left && c.cursor_x() == 140 && c.cursor_y() == 232);   // the stop click ahead of him (left)
    c.update(pad_with(0), t += 200);
    c.update(pad_with(0), t += 16);
    CHECK(!g_left);
    // No view of the hero: the free half's middle all the same.
    Controls blind;
    blind.set_frame(640, 480);
    blind.set_menu_nav(&menus);
    blind.set_context(true, true, 1ull | 1ull << 0x02, false);   // the character sheet (the left half)
    struct NoView : FakeAim {
        bool view(AimView &) override { return false; }
    } none;
    blind.set_auto_target(AimSettings(), &none);
    for (int i = 0; i < 25; i++)
        blind.update(pad_with(0, -156, 0), t += 16);
    CHECK(g_left && blind.cursor_x() == (324 + 640) / 2 - 88);
    blind.update(pad_with(0), t += 16);
    blind.update(pad_with(0), t += 300);
    // The auto-target with the inventory open: an enemy on the free half,
    // left of the hero (the stick's last direction).
    AimUnit enemy;
    enemy.type = 1, enemy.id = 5, enemy.mode = 1, enemy.flags_a = 0x0F, enemy.flags_b = 0x80;
    enemy.feet_x = 80, enemy.feet_y = 240, enemy.distance = 5;
    world.monsters = {enemy};
    AimUnit hidden = enemy;   // under the panel: never taken
    hidden.id = 6, hidden.feet_x = 420, hidden.distance = 4;
    world.monsters.push_back(hidden);
    // A with the cursor on a panel cell: a plain click there.
    c.warp_cursor(x, y);
    c.update(pad_with(0), t += 16);
    c.update(pad_with(kA), t += 16);
    CHECK(g_left && c.cursor_x() == x && c.cursor_y() == y);
    c.update(pad_with(0), t += 16);
    c.update(pad_with(0), t += 200);
    // A with the cursor on the free half: the cursor onto the enemy there.
    c.warp_cursor(200, 240);
    c.update(pad_with(0), t += 16);
    c.update(pad_with(kA), t += 16);
    CHECK(c.cursor_x() == 80 && c.cursor_y() == 240 - 30);
    c.update(pad_with(0), t += 16);
}

// ---- the inventory's weapon swap tabs: one cell over each weapon slot ----
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

// ---- the gold dialog: its most, its window, the D-pad typing its amount ----
static void test_gold_field_amount()
{
    const uint16_t full[] = {'1', '0', '0', '0', '0'}, empty[] = {0}, odd[] = {'9', '7', 'x', '5'};
    const uint16_t huge[] = {'9', '9', '9', '9', '9', '9', '9', '9', '9', '9'};
    CHECK(gold_field_amount(full, 5) == 10000);
    CHECK(gold_field_amount(full, 3) == 100);
    CHECK(gold_field_amount(empty, 0) == 0);
    CHECK(gold_field_amount(odd, 4) == 97);
    CHECK(gold_field_amount(huge, 10) == 0x7FFFFFFF);
}

static void test_gold_repeat()
{
    GoldRepeat r;
    uint64_t t = 1000;
    CHECK(r.update(0, t) == 0);
    CHECK(r.update(1, t) == 1);   // pressed: one step at once
    int sum = 0;
    for (uint64_t ms = 16; ms < (uint64_t)GoldRepeat::kDelayMs; ms += 16)
        sum += r.update(1, t + ms);
    CHECK(sum == 0);              // nothing more until kDelayMs
    CHECK(r.update(1, t + GoldRepeat::kDelayMs) == 1);
    // Then sooner each time: the gaps shrink to kFastestMs.
    uint64_t last = t + GoldRepeat::kDelayMs, now = last;
    int prev_gap = 1 << 30, repeats = 0;
    bool shrank = true;
    while (now < t + GoldRepeat::kTenfoldMs - 1) {
        now += 1;
        if (r.update(1, now)) {
            const int gap = (int)(now - last);
            shrank = shrank && gap <= prev_gap && gap >= GoldRepeat::kFastestMs;
            prev_gap = gap;
            last = now;
            repeats++;
        }
    }
    CHECK(shrank && repeats >= 10 && prev_gap == GoldRepeat::kFastestMs);
    // Held past kTenfoldMs: tenfold steps; past three: a thousandfold at most.
    GoldRepeat r2;
    r2.update(-100, 0);
    int seen = 0;
    for (uint64_t ms = 1; ms < 10000; ms++) {
        const int c = r2.update(-100, ms);
        if (c && ms >= (uint64_t)GoldRepeat::kTenfoldMs * 3 + 100)
            seen = c;
        if (c && ms < (uint64_t)GoldRepeat::kTenfoldMs)
            CHECK(c == -100);
        if (c && ms >= (uint64_t)GoldRepeat::kTenfoldMs + 50 && ms < (uint64_t)GoldRepeat::kTenfoldMs * 2)
            CHECK(c == -1000);
    }
    CHECK(seen == -100000);
    // Let go: nothing; another press: one step at once again.
    CHECK(r2.update(0, 10000) == 0);
    CHECK(r2.update(0, 20000) == 0);
    CHECK(r2.update(100, 20001) == 100);
    // Another direction while held counts as a new press.
    CHECK(r2.update(-1, 20017) == -1);
}

static GoldDialog deposit_dialog(int amount, int max)
{
    GoldDialog d;
    d.mode = GoldDialog::kDeposit;
    d.amount = amount;
    d.max = max;
    d.ok = NavRect{250, 255, 282, 287};       // as the lane read them at 640x480
    d.cancel = NavRect{355, 255, 387, 287};
    return d;
}

static void test_gold_dialog_window()
{
    const NavWindow w = gold_dialog_window(deposit_dialog(0, 10));
    CHECK(w.cells.size() == 2 && w.cells[0].centre_x() == 266 && w.cells[0].centre_y() == 271 &&
          w.cells[1].centre_x() == 371 && w.cells[1].centre_y() == 271);
    MenuNav nav;
    const std::vector<NavWindow> ws = {w};
    MenuNav::Step s = nav.step(ws, 266, 271, NavDirection::kRight, false, 640, 480);
    CHECK(s.moved && s.x == 371 && s.y == 271);
    s = nav.step(ws, 371, 271, NavDirection::kLeft, false, 640, 480);
    CHECK(s.moved && s.x == 266 && s.y == 271);
}

// The gold buttons at 640x480 (the lane's pixels) and moved with the halves.
static void test_gold_buttons()
{
    const PanelShift none = panel_shift(640, 480), big = panel_shift(800, 600);
    const NavRect stash640 = stash_gold_button(none), inv640 = inventory_gold_button(none);
    CHECK(stash640.contains(84, 32) && inv640.contains(414, 401));
    const NavRect stash800 = stash_gold_button(big), inv800 = inventory_gold_button(big);
    CHECK(stash800.contains(164, 92) && inv800.contains(494, 461));
    // The close buttons under the inventory and under the stash or a shop.
    CHECK(inventory_close_button(none).centre_x() == 354 && inventory_close_button(none).centre_y() == 402);
    CHECK(stash_close_button(none).centre_x() == 287 && stash_close_button(none).centre_y() == 401);
    CHECK(inventory_close_button(big).contains(434, 462) && stash_close_button(big).contains(367, 461));
}

static void test_controls_gold_dialog()
{
    FakeMenus menus;
    menus.open = {gold_dialog_window(deposit_dialog(50000, 50000))};
    menus.gold = deposit_dialog(50000, 50000);
    menus.gold_on = true;
    Controls c;
    c.set_frame(640, 480);
    c.set_menu_nav(&menus);
    c.set_context(true, true, 1ull << 0x19 | 1ull << 0x01, false);
    c.warp_cursor(266, 271);
    uint64_t t = 5000;
    std::memset(g_ever, 0, sizeof g_ever);
    g_typed.clear();
    c.update(pad_with(0), t);
    // Up at the most: nothing typed, no belt key.
    c.update(pad_with(kUp), t += 16);
    c.update(pad_with(0), t += 16);
    CHECK(g_typed.empty() && !g_ever['1']);
    // Down: 49999, typed over the field (Backspaces, then the digits).
    c.update(pad_with(kDown), t += 16);
    c.update(pad_with(0), t += 16);
    CHECK(g_typed == std::string(10, '<') + "49999" && !g_ever['3']);
    // Left (-100) while the field still shows 50000: from the typed 49999.
    g_typed.clear();
    c.update(pad_with(kLeft), t += 16);
    c.update(pad_with(0), t += 16);
    CHECK(g_typed == std::string(10, '<') + "49899" && !g_ever['4']);
    // Right (+100) once the field shows something else (1200): from it.
    menus.gold.amount = 1200;
    g_typed.clear();
    c.update(pad_with(kRight), t += 16);
    c.update(pad_with(0), t += 16);
    CHECK(g_typed == std::string(10, '<') + "1300" && !g_ever['2']);
    // Down held: one at once, then repeats after the delay.
    menus.gold.amount = 10;
    t += 1000;
    g_typed.clear();
    c.update(pad_with(kDown), t += 16);
    const size_t first = g_typed.size();
    for (int ms = 16; ms < GoldRepeat::kDelayMs - 16; ms += 16)
        c.update(pad_with(kDown), t += 16);
    CHECK(first == 11 && g_typed.size() == first);   // "<<<<<<<<<<9"
    for (int ms = 0; ms < 1000; ms += 16)
        c.update(pad_with(kDown), t += 16);
    c.update(pad_with(0), t += 16);
    // Down to 0: the field emptied, never below.
    CHECK(g_typed.size() > first && g_typed.substr(g_typed.size() - 10) == std::string(10, '<'));
    CHECK(!g_ever['1'] && !g_ever['2'] && !g_ever['3'] && !g_ever['4']);
    // R + Up: still F1 (a skill), not the amount.
    g_typed.clear();
    c.update(pad_with(kR), t += 16);
    c.update(pad_with(kR | kUp), t += 16);
    CHECK(g_keys[0x70] && g_typed.empty());
    c.update(pad_with(0), t += 16);
    // A flick goes between OK and Cancel.
    c.update(pad_with(0, 156, 0), t += 16);
    c.update(pad_with(0), t += 16);
    CHECK(c.cursor_x() == 371 && c.cursor_y() == 271);
    // Closed: the D-pad is the belt again.
    menus.gold_on = false;
    menus.open = {inventory()};
    c.update(pad_with(0), t += 16);
    c.update(pad_with(kUp), t += 16);
    CHECK(g_keys['1']);
    c.update(pad_with(0), t += 16);
    CHECK(!g_keys['1']);
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
    test_controls();
    test_controls_holding();
    test_d2win_cells();
    test_d2win_screen();
    test_char_screen_scroll();
    test_controls_scroll_button();
    test_game_menu_rows();
    test_controls_game_menu();
    test_b_out_of_game();
    test_weapon_tabs();
    test_keyboard_pass_through();
    test_control_panel_plain_click();
    test_panel_hold_walks();
    test_panel_side_hero_place();
    test_gold_field_amount();
    test_gold_repeat();
    test_gold_dialog_window();
    test_gold_buttons();
    test_controls_gold_dialog();
    if (g_fail) {
        std::printf("%d checks FAILED\n", g_fail);
        return 1;
    }
    std::printf("menu: all tests passed\n");
    return 0;
}
