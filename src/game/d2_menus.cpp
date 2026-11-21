// See d2_menus.h for the addresses and what they hold.
//
// This file is menu navigation's only contact with the game: it reads (never
// writes) Diablo II 1.14d's memory to find which windows are open and where
// their cells are, and hands them to menu_nav.cpp as NavWindow lists.
// This commit covers D2Win's screens before the game (main menu, character
// select, class screen, difficulty, multiplayer). Later commits add the
// game menu, in-game panels, and the NPC menu box found in the frame.
#include "game/d2_menus.h"

#include "game/d2_units.h"
#include "runtime/rt_host.h"

#include <algorithm>
#include <cstring>

namespace game {
namespace {

uint32_t rd32(uint32_t va)
{
    uint32_t v = 0;
    rt::guest_read(va, &v, 4);
    return v;
}

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

// The MenuWorld the console and the Linux lane use (tests/menu has its own).
class GameMenuWorld : public MenuWorld {
public:
    bool main_menu() override
    {
        if (player_class() >= 0)
            return false;   // in a game: D2Win's list is stale
        std::vector<D2WinControl> controls;
        d2win_controls(controls);
        return d2win_main_menu(controls);
    }

    void windows(uint64_t ui, int width, int height, std::vector<NavWindow> &out) override
    {
        (void)ui;
        out.clear();
        if (player_class() < 0)   // out of the game: D2Win's screen
            d2win_windows(width, height, out);
    }
};

}  // namespace

void menus_frame(const uint8_t *pixels, const uint32_t *palette, uint32_t width, uint32_t height, int32_t stride)
{
    (void)pixels, (void)palette, (void)width, (void)height, (void)stride;
}

MenuWorld &game_menu_world()
{
    static GameMenuWorld world;
    return world;
}

}  // namespace game
