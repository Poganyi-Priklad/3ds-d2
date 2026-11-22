// See d2_menus.h for the addresses and what they hold.
//
// This file is menu navigation's only contact with the game: it reads (never
// writes) Diablo II 1.14d's memory to find which windows are open and where
// their cells are (inventory.txt and belts.txt rows, the NPC menu, the game
// menu, D2Win's control list before the game), and hands them to
// menu_nav.cpp as NavWindow lists. When the game's memory says nothing
// useful (an NPC menu drawn without its record), menus_frame finds the box
// in the frame's pixels instead.
#include "game/d2_menus.h"

#include "game/d2_units.h"
#include "runtime/rt_host.h"

#include <algorithm>
#include <cstring>


namespace game {
namespace {

constexpr uint32_t kInventoryTxtVa = 0x0096D4F4;
constexpr uint32_t kInventoryRowSize = 0xF0;
constexpr uint32_t kBeltsTxtVa = 0x0096D4F8;
constexpr uint32_t kBeltsRowSize = 0x108;
constexpr int kDefaultBeltRow = 2;      // no belt worn: the one row of four
constexpr int kBeltRows800 = 7;         // belts.txt's rows at 800x600 start here
constexpr int kBeltRowsPerSize = 7;     // belts.txt's rows at one size (0-6)
constexpr uint32_t kWornBeltVa = 0x00722354;   // the worn belt's belts.txt row (0-6), 2 with none
constexpr uint32_t kBeltOpenVa = 0x007BEFA0;   // non-zero while the belt's upper rows are shown
constexpr uint32_t kNpcMenuVa = 0x007C0E54;
constexpr uint32_t kUiTableVa = 0x007A27C0;
constexpr int kUiNpcMenu = 0x08;
constexpr int kStatPoints = 4;          // itemstatcost.txt: the stat points left
constexpr uint32_t kGoldModeVa = 0x007A279C;
constexpr uint32_t kGoldPartsVa = 0x007A27A0;   // five pointers
constexpr uint32_t kGoldOpenVa = 0x007A27B4;
constexpr uint32_t kGoldFieldVtable = 0x006DA7CC, kGoldButtonVtable = 0x006DA72C;

// One 32-bit word of guest memory (0 when it can't be read).
uint32_t rd32(uint32_t va)
{
    uint32_t v = 0;
    rt::guest_read(va, &v, 4);
    return v;
}

constexpr uint64_t ui_bit(int id) { return 1ull << id; }

// inventory.txt's rows, by what they are (at 640x480; +16 at 800x600).
enum Row : int { kShopRow = 5, kTheirTradeRow = 6, kMyTradeRow = 7, kCubeRow = 9, kStashRow = 12,
                 kMercenaryRow = 13, kDruidRow = 14, kAssassinRow = 15 };

// A left, right, top, bottom rectangle as the table keeps it.
bool read_rect(uint32_t va, NavRect &out)
{
    int32_t r[4] = {};
    if (!rt::guest_read(va, r, sizeof r))
        return false;
    out = NavRect{r[0], r[2], r[1], r[3]};
    return !out.empty() && out.left >= 0 && out.top >= 0 && out.right <= 2048 && out.bottom <= 2048;
}

struct InventoryRow {
    NavRect panel, grid;
    int columns = 0, rows = 0;
    NavRect slots[10];
    bool slot_used[10] = {};
};

// A row of inventory.txt as it is numbered (0-31).
bool read_table_row(int row, InventoryRow &out)
{
    const uint32_t rows = rd32(kInventoryTxtVa);
    if (!rows || row < 0 || row > 31)
        return false;
    const uint32_t at = rows + (uint32_t)row * kInventoryRowSize;
    if (!read_rect(at, out.panel))
        return false;
    uint8_t size[2] = {};
    rt::guest_read(at + 0x10, size, 2);
    out.columns = size[0];
    out.rows = size[1];
    if (out.columns > 16 || out.rows > 16 || !read_rect(at + 0x14, out.grid))
        out.columns = out.rows = 0;
    for (int i = 0; i < 10; i++)
        out.slot_used[i] = read_rect(at + 0x28 + 0x14 * i, out.slots[i]);
    return true;
}

// A row by what it is, for the frame's width (rows 16-31 at 800x600).
bool read_row(int row, int width, InventoryRow &out)
{
    return read_table_row(row + (width >= 800 ? 16 : 0), out);
}

int inventory_row(int cls) { return cls <= 4 ? cls : cls == 5 ? kDruidRow : kAssassinRow; }

// How far this width moves the left and right halves' panels from where
// they are at 640x480: a shop's row and the class's row against the same
// rows at 640x480. What inventory.txt can't tell keeps the centred guess.
PanelShift read_shift(int cls, int width, int height)
{
    PanelShift s = panel_shift(width, height);
    InventoryRow small, big;
    if (read_table_row(kShopRow, small) && read_row(kShopRow, width, big)) {
        s.left_x = big.panel.left - small.panel.left;
        s.left_y = big.panel.top - small.panel.top;
    }
    if (read_table_row(inventory_row(cls), small) && read_row(inventory_row(cls), width, big)) {
        s.right_x = big.panel.left - small.panel.left;
        s.right_y = big.panel.top - small.panel.top;
    }
    return s;
}

// The belt's boxes under the screen, from belts.txt (rows of 0x108 bytes:
// +0x04 the number of boxes, +0x08 sixteen boxes of left, right, top,
// bottom; rows 0-6 at 640x480, 7-13 at 800x600). Its first four boxes are
// the row always shown; the rest, the worn belt's upper rows, only while the
// game shows them (kBeltOpenVa, which the game sets each frame from the belt
// being open - the cursor on it, or it clicked - and the worn belt having
// more than one row; kWornBeltVa, the worn belt's row, the game keeps from
// the item). So a flick up from the row of four opens the rest, and one off
// the belt closes it again.
void add_belt(int width, std::vector<NavWindow> &out)
{
    const uint32_t rows = rd32(kBeltsTxtVa);
    if (!rows)
        return;
    uint32_t worn = rd32(kWornBeltVa);
    if (worn >= (uint32_t)kBeltRowsPerSize)
        worn = kDefaultBeltRow;
    const uint32_t at = rows + (worn + (uint32_t)(width >= 800 ? kBeltRows800 : 0)) * kBeltsRowSize;
    const uint32_t boxes = rd32(at + 0x04);
    if (boxes < 4 || boxes > 16)
        return;
    const uint32_t shown = rd32(kBeltOpenVa) ? boxes : 4;
    NavWindow w;
    w.name = "belt";
    for (uint32_t i = 0; i < shown; i++) {
        NavRect box;
        if (!read_rect(at + 0x08 + 0x10 * i, box))
            return;
        w.cells.push_back(box);
        w.area = i ? NavRect{std::min(w.area.left, box.left), std::min(w.area.top, box.top),
                             std::max(w.area.right, box.right), std::max(w.area.bottom, box.bottom)}
                   : box;
    }
    if (shown > 4) {
        // Open, the belt's upper rows are drawn over the inventory's bottom
        // rows: the cells under them are hidden, and the belt's boxes join
        // the panel they sit on, so a flick goes between the two as between
        // any two neighbouring cells.
        const NavRect &top = w.cells[shown - 1];
        for (NavWindow &other : out)
            other.cells.erase(std::remove_if(other.cells.begin(), other.cells.end(),
                                             [&](const NavRect &cell) {
                                                 return w.area.contains(cell.centre_x(), cell.centre_y());
                                             }),
                              other.cells.end());
        for (NavWindow &other : out)
            if (!other.up_down_only && other.area.contains(top.centre_x(), top.centre_y())) {
                other.cells.insert(other.cells.end(), w.cells.begin(), w.cells.end());
                return;
            }
    }
    out.push_back(w);
}

// An NPC's menu as the game keeps it: a pointer to it while its box is
// shown (0 during a talk's text and once closed); +0x2C its box's left,
// +0x30 top, +0x34 width, +0x38 height, +0x4C its options, Cancel too.
bool read_npc_menu(NavRect &box, int &options)
{
    const uint32_t menu = rd32(kNpcMenuVa);
    if (!menu)
        return false;
    int32_t r[4] = {};
    if (!rt::guest_read(menu + 0x2C, r, sizeof r))
        return false;
    options = (int)rd32(menu + 0x4C);
    box = NavRect{r[0], r[1], r[0] + r[2], r[1] + r[3]};
    return r[0] >= 0 && r[1] >= 0 && r[2] >= 40 && r[2] <= 400 && r[3] > 0 && r[3] <= 200 && options >= 1 &&
           options <= 8;
}

// inventory.txt's equipment boxes, in its order: the right arm (the weapon
// drawn on the left), the torso, the left arm (the weapon or shield drawn
// on the right), the head, the neck, two rings, the belt, the feet, the hands.
constexpr int kRightArmSlot = 0, kLeftArmSlot = 2;

// One open panel as a NavWindow, from its inventory.txt row: its grid's
// cells (grid), its equipment slots in use (slots), or both; the hero's
// own inventory also its two weapon swap tabs (tabs). A panel with no
// cells is left out (false; true: it is out.back()).
bool add_window(const char *name, int row, int width, bool grid, bool slots, std::vector<NavWindow> &out,
                bool tabs = false)
{
    InventoryRow r;
    if (!read_row(row, width, r))
        return false;
    NavWindow w;
    w.name = name;
    w.area = r.panel;
    if (grid)
        add_grid(w, r.grid, r.columns, r.rows);
    if (slots)
        for (int i = 0; i < 10; i++)
            if (r.slot_used[i])
                w.cells.push_back(r.slots[i]);
    if (tabs)
        for (int i : {kRightArmSlot, kLeftArmSlot})
            if (r.slot_used[i])
                w.cells.push_back(weapon_tab(r.slots[i]));
    if (w.cells.empty())
        return false;
    out.push_back(w);
    return true;
}

// ---- the NPC menu's box in the frame ----

NavRect g_box;
bool g_box_valid = false;
NavRect g_said;   // the game's menu box last logged

// Whether the frame's pixel at x, y is bright enough to be the NPC menu
// box's border against the darker scene (find_box follows it to measure
// the box).
bool border(const uint8_t *pixels, const uint32_t *palette, int32_t stride, int x, int y)
{
    const uint32_t c = palette[pixels[(ptrdiff_t)y * stride + x]];
    return ((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF) > 120;
}

// ---- D2Win's screens (out of the game) ----

constexpr uint32_t kD2WinFirstVa = 0x007D55BC;   // the screen's first control; +0x3C the next
constexpr uint32_t kAnimatedDrawFn = 0x005005B0;  // an animated image's draw function (+0x1C)
constexpr int kMaxControls = 160;

// One control: +0x00 type, +0x08 state, +0x0C x, +0x10 y (the bottom
// edge), +0x14 width, +0x18 height, +0x1C its draw function, +0x34 its
// click handler, +0x3C the next; a text box's lines at +0x54; a scroll
// bar's last top row at +0x48 and its top row at +0x4C; an animated
// image's animations at +0x40 (16 bytes each, the first the still one):
// a pointer to a pointer to its DC6 file (+0x00 6, +0x18 its first frame:
// +0x04 width, +0x08 height, +0x0C x offset, +0x10 y offset).
bool read_control(uint32_t va, D2WinControl &c, uint32_t &next)
{
    uint32_t f[0x58 / 4] = {};
    if (!rt::guest_read(va, f, sizeof f))
        return false;
    c = D2WinControl();
    c.type = f[0];
    c.state = f[2];
    c.x = (int32_t)f[3];
    c.y = (int32_t)f[4];
    c.width = (int32_t)f[5];
    c.height = (int32_t)f[6];
    c.on_click = f[0x34 / 4] != 0;
    c.lines = (int32_t)f[0x54 / 4];
    next = f[0x3C / 4];
    if (c.type == 5) {   // a scroll bar: +0x48 its last top row, +0x4C its top row now
        c.scroll_last = (int32_t)f[0x48 / 4];
        c.scroll_top = (int32_t)f[0x4C / 4];
    }
    if (c.type == 2 && f[0x1C / 4] == kAnimatedDrawFn && f[0x40 / 4]) {
        const uint32_t file = rd32(rd32(f[0x40 / 4]));
        const uint32_t frame = file && rd32(file) == 6 ? rd32(file + 0x18) : 0;
        int32_t fr[4] = {};
        if (frame && rt::guest_read(frame + 4, fr, sizeof fr) && fr[0] > 0 && fr[0] <= 800 && fr[1] > 0 &&
            fr[1] <= 600) {
            c.frame = true;
            c.frame_width = fr[0];
            c.frame_height = fr[1];
            c.frame_x = fr[2];
            c.frame_y = fr[3];
        }
    }
    return true;
}

// Out of the game (main menu, character select, class screen...): one
// window, "screen", with a cell for every D2Win control the player can
// use, walked from the screen's control list; and the character list's
// scroll column, when it has one.
// The current screen's controls, walked from its list (out of the game only).
void d2win_controls(std::vector<D2WinControl> &controls)
{
    controls.clear();
    uint32_t va = rd32(kD2WinFirstVa);
    for (int n = 0; va && n < kMaxControls; n++) {
        D2WinControl c;
        uint32_t next = 0;
        if (!read_control(va, c, next))
            break;
        controls.push_back(c);
        va = next;
    }
}

void d2win_windows(int width, int height, std::vector<NavWindow> &out)
{
    NavWindow w;
    w.name = "screen";
    w.area = NavRect{0, 0, width, height};
    uint32_t va = rd32(kD2WinFirstVa);
    std::vector<D2WinControl> controls;
    for (int n = 0; va && n < kMaxControls; n++) {
        D2WinControl c;
        uint32_t next = 0;
        if (!read_control(va, c, next))
            break;
        NavRect cell;
        if (d2win_cell(c, width, height, cell))
            w.cells.push_back(cell);
        controls.push_back(c);
        va = next;
    }
    d2win_scroll(controls, w.cells, width, height, w.scroll);
    if (!w.cells.empty())
        out.push_back(w);
}

// ---- the game menu (Escape) and its submenus ----

constexpr uint32_t kGameMenuLitVa = 0x007BC938;    // the lit row
constexpr uint32_t kGameMenuHeadVa = 0x007BC93C;   // the open menu's header: its rows, their step
constexpr uint32_t kGameMenuRowsVa = 0x007BC940;   // its rows, 0x550 bytes each
constexpr uint32_t kGameMenuRowSize = 0x550;

// The open game menu (Escape) or one of its submenus: its rows, which one
// is lit, and which row goes back ("Previous"). False when no menu is open
// or the record doesn't look right (it is stale once the menu closes).
bool read_game_menu(int width, int height, GameMenu &out)
{
    out = GameMenu();
    const uint32_t head = rd32(kGameMenuHeadVa), rows = rd32(kGameMenuRowsVa);
    if (head < 0x00400000 || head >= 0x01000000 || rows < 0x00400000 || rows >= 0x01000000)
        return false;
    const int count = (int)rd32(head), step = (int)rd32(head + 4);
    if (count < 1 || count > 16)
        return false;
    int8_t kinds[16] = {};
    for (int i = 0; i < count; i++) {
        const uint32_t row = rows + (uint32_t)i * kGameMenuRowSize;
        const int32_t kind = (int32_t)rd32(row);
        if (kind < -1 || kind > 2)
            return false;
        kinds[i] = (int8_t)kind;
        // Its name (+0x0C): "Previous", "SPrevious"... go one menu back.
        char id[24] = {};
        rt::guest_read(row + 0x0C, id, sizeof id - 1);
        const size_t len = std::strlen(id);
        if (len >= 8 && std::strcmp(id + len - 8, "Previous") == 0)
            out.previous = i;
    }
    game_menu_rows(count, step, kinds, width, height, out);
    if (out.rows.empty())
        return false;
    const int lit = (int)rd32(kGameMenuLitVa);
    out.lit = lit >= 0 && lit < count ? lit : -1;
    return true;
}

// ---- the gold dialog ----

// One of the gold dialog's buttons (OK, Cancel): its box, y its bottom edge.
bool read_gold_button(uint32_t part, NavRect &out)
{
    int32_t f[14] = {};
    if (!part || !rt::guest_read(part, f, sizeof f) || (uint32_t)f[0] != kGoldButtonVtable)
        return false;
    const int x = f[0x20 / 4], bottom = f[0x24 / 4], w = f[0x30 / 4], h = f[0x34 / 4];
    if (x < 0 || bottom <= 0 || w <= 0 || w > 64 || h <= 0 || h > 64 || x + w > 2048 || bottom > 2048)
        return false;
    out = NavRect{x, bottom - h, x + w, bottom};
    return true;
}

// The open gold dialog: its form, amount and buttons (d2_menus.h); false
// when none is open or its parts don't look right.
bool read_gold_dialog(GoldDialog &out)
{
    out = GoldDialog();
    if (rd32(kGoldOpenVa) != 1)
        return false;
    uint32_t parts[5] = {};
    if (!rt::guest_read(kGoldPartsVa, parts, sizeof parts) || rd32(parts[2]) != kGoldFieldVtable)
        return false;
    out.mode = (int)rd32(kGoldModeVa);
    if (out.mode < GoldDialog::kDrop || out.mode > GoldDialog::kWithdraw)
        return false;
    // The field: +0x40 its characters, +0x48 the most the game lets it
    // hold, +0x50 its length (high half) and its longest (low half).
    const uint32_t text = rd32(parts[2] + 0x40), length = rd32(parts[2] + 0x50) >> 16;
    uint16_t chars[16] = {};
    if (length > 16 || (length && !rt::guest_read(text, chars, length * 2)))
        return false;
    out.amount = gold_field_amount(chars, (int)length);
    out.max = (int)rd32(parts[2] + 0x48);
    if (out.max < 0)
        return false;
    if (!read_gold_button(parts[3], out.ok) || !read_gold_button(parts[4], out.cancel))
        return false;
    return true;
}

// The MenuWorld the console and the Linux lane use (tests/menu has its own).
class GameMenuWorld : public MenuWorld {
public:
    bool on_interface(uint64_t ui, int x, int y, int width, int height) override
    {
        (void)ui;
        if (control_panel_at(x, y, width, height))
            return true;
        if (!rd32(kBeltOpenVa))
            return false;
        std::vector<NavWindow> belt;   // the belt's open rows, over the world
        add_belt(width, belt);
        for (const NavWindow &w : belt)
            for (const NavRect &cell : w.cells)
                if (cell.contains(x, y))
                    return true;
        return false;
    }

    bool main_menu() override
    {
        if (player_class() >= 0)
            return false;   // in a game: D2Win's list is stale
        std::vector<D2WinControl> controls;
        d2win_controls(controls);
        return d2win_main_menu(controls);
    }

    bool gold_dialog(uint64_t ui, int width, int height, GoldDialog &out) override
    {
        (void)ui, (void)width, (void)height;
        return player_class() >= 0 && read_gold_dialog(out);
    }

    bool game_menu(uint64_t ui, int width, int height, GameMenu &out) override
    {
        if (!(ui & ui_bit(0x09)) || player_class() < 0)
            return false;   // its pointers are stale once it is closed
        return read_game_menu(width, height, out);
    }

    void windows(uint64_t ui, int width, int height, std::vector<NavWindow> &out) override
    {
        out.clear();
        const int cls = player_class();
        if (cls < 0) {   // out of the game: D2Win's screen
            d2win_windows(width, height, out);
            return;
        }
        const PanelShift shift = read_shift(cls, width, height);
        if (ui & ui_bit(0x09)) {   // the game menu, over everything
            fixed_windows(ui, shift, false, out);
            return;
        }
        GoldDialog gold;
        if (read_gold_dialog(gold)) {   // over the panels, and the only thing to click
            out.push_back(gold_dialog_window(gold));
            return;
        }
        // The right half: the inventory, also beside a shop, the stash, the
        // cube and a trade.
        const bool inventory = ui & (ui_bit(0x01) | ui_bit(0x0C) | ui_bit(0x17) | ui_bit(0x19) | ui_bit(0x1A));
        if (inventory && add_window("inventory", inventory_row(cls), width, true, true, out, true)) {
            out.back().cells.push_back(inventory_gold_button(shift));
            out.back().cells.push_back(inventory_close_button(shift));
        }
        if ((ui & ui_bit(0x0C)) && add_window("shop", kShopRow, width, true, false, out))
            out.back().cells.push_back(stash_close_button(shift));
        if ((ui & ui_bit(0x19)) && add_window("stash", kStashRow, width, true, false, out)) {
            out.back().cells.push_back(stash_gold_button(shift));
            out.back().cells.push_back(stash_close_button(shift));
        }
        if (ui & ui_bit(0x1A))
            add_window("cube", kCubeRow, width, true, false, out);
        if (ui & ui_bit(0x17)) {
            add_window("their trade", kTheirTradeRow, width, true, false, out);
            add_window("my trade", kMyTradeRow, width, true, false, out);
        }
        if (ui & ui_bit(0x24))
            add_window("mercenary", kMercenaryRow, width, false, true, out);
        if (inventory)   // the belt under it, to move potions
            add_belt(width, out);
        fixed_windows(ui, shift, ui & ui_bit(0x02) ? player_stat(kStatPoints) > 0 : false, out);
        NavWindow menu;
        NavRect box;
        int options = 0;
        if (ui & ui_bit(kUiNpcMenu)) {
            if (read_npc_menu(box, options) ? npc_menu_window(box, options, menu)
                                            : g_box_valid && npc_menu_window(g_box, menu))
                out.push_back(menu);
        }
    }
};

}  // namespace

namespace {
void find_box(const uint8_t *pixels, const uint32_t *palette, uint32_t width, uint32_t height, int32_t stride);
}

void menus_frame(const uint8_t *pixels, const uint32_t *palette, uint32_t width, uint32_t height, int32_t stride)
{
    const bool was = g_box_valid;
    const NavRect before = g_box;
    NavRect box;
    int options = 0;
    if (read_npc_menu(box, options)) {
        // The game's own menu: the frame isn't needed.
        g_box_valid = false;
        if (box.left != g_said.left || box.top != g_said.top || box.right != g_said.right ||
            box.bottom != g_said.bottom)
            rt::logf("menu: an NPC's menu of %d options at %d..%d by %d..%d (the game's)", options, box.left,
                     box.right - 1, box.top, box.bottom - 1);
        g_said = box;
        return;
    }
    g_said = NavRect();
    find_box(pixels, palette, width, height, stride);
    if (g_box_valid && (!was || before.left != g_box.left || before.top != g_box.top ||
                        before.right != g_box.right || before.bottom != g_box.bottom))
        rt::logf("menu: an NPC's menu at %d..%d by %d..%d", g_box.left, g_box.right - 1, g_box.top, g_box.bottom - 1);
}

namespace {
void find_box(const uint8_t *pixels, const uint32_t *palette, uint32_t width, uint32_t height, int32_t stride)
{
    g_box_valid = false;
    if (!pixels || !palette || width < 320 || height < 240 || !rd32(kUiTableVa + kUiNpcMenu * 4))
        return;
    // A shop, the inventory or another panel over it (the menu is gone).
    for (int id : {0x01, 0x0C, 0x17, 0x19, 0x1A})
        if (rd32(kUiTableVa + id * 4))
            return;
    const int w = (int)width, h = (int)height;
    auto lit = [&](int x, int y) { return border(pixels, palette, stride, x, y); };
    // The border is two pixels wide: either of two rows (or columns).
    auto across = [&](int x, int y) { return lit(x, y) || lit(x, y + 1); };
    auto down = [&](int x, int y, int inward) { return lit(x, y) || lit(x + inward, y); };
    // The box's top: a row of border pixels, not lit above it, whose two
    // ends go down as far as each other with the inside dark under the top.
    for (int y = 1; y < h * 2 / 3; y++) {
        for (int x = 8; x < w - 8; x += 8) {
            if (!lit(x, y) || lit(x, y - 1))
                continue;
            int left = x, right = x;
            while (left > 0 && across(left - 1, y))
                left--;
            while (right < w - 1 && across(right + 1, y))
                right++;
            x = right;   // the next look after this run
            // (A talk's text box across the top is wider: 328 pixels.)
            if (right - left < 50 || right - left > 300)
                continue;
            int bottom = y, bottom_right = y;
            while (bottom < h - 1 && down(left, bottom + 1, 1))
                bottom++;
            while (bottom_right < h - 1 && down(right, bottom_right + 1, -1))
                bottom_right++;
            // (Something bright under a corner may make one side look longer.)
            bottom = bottom_right < bottom ? bottom_right : bottom;
            if (bottom - y < 40 || bottom - y > 200)
                continue;
            int closed = 0;   // the bottom edge across
            for (int i = left; i <= right; i++)
                closed += lit(i, bottom) || lit(i, bottom - 1) || lit(i, bottom - 2);
            if (closed * 10 < (right - left + 1) * 9)
                continue;
            int dark = 0;
            for (int i = left + 4; i < right - 4; i++)
                dark += !lit(i, y + 5);
            if (dark * 4 < (right - left - 8) * 3)
                continue;
            g_box = NavRect{left, y, right + 1, bottom + 1};
            g_box_valid = true;
            return;
        }
    }
}
}  // namespace

bool npc_menu_box(NavRect &out)
{
    int options = 0;
    if (read_npc_menu(out, options))
        return true;
    if (g_box_valid)
        out = g_box;
    return g_box_valid;
}

MenuWorld &game_menu_world()
{
    static GameMenuWorld world;
    return world;
}

}  // namespace game
