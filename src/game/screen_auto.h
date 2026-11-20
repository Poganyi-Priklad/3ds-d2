// The bottom screen's automatic switches: what changes the bottom screen by
// itself, without a tap or a button. Each one is a box on the OPTIONS
// page's second page (SCREEN, second_screen.h), on by default (the way the
// port has always behaved; a personal build may start three of them off:
// D2_DEFAULT_NO_PANEL_AUTO below), and saved in settings.txt as "screenauto="
// (screen_auto_names), "-name" for one that is off.
#ifndef D2_GAME_SCREEN_AUTO_H
#define D2_GAME_SCREEN_AUTO_H

#include "game/port_options.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace game {

enum ScreenAuto : uint32_t {
    // A panel opening on either half (inventory, character, skills, stash,
    // shop, cube, trade, quests, waypoints...) shows PANEL by itself. Off:
    // PANEL is offered on the bar, and the tab shown stays.
    kAutoPanel = 1u << 0,
    // An NPC's menu or talk opening shows PANEL (centred) by itself.
    kAutoNpcMenu = 1u << 1,
    // With panels on both halves, PANEL shows the half the cursor is on.
    // Off: it stays on its half until PANEL is tapped again.
    kAutoPanelSide = 1u << 2,
    // A text field getting the focus (a hero's name, a game's name) opens
    // the on-screen keyboard. Off: R + Select opens it.
    kAutoKeyboard = 1u << 3,
    // A cinematic shows its own panel (title, time, how to skip) instead
    // of the tab. Off: the tab stays.
    kAutoVideo = 1u << 4,
};

struct ScreenAutoInfo {
    uint32_t bit;
    const char *name;    // in settings.txt
    const char *label;   // on the page
    const char *note;    // under it: what it does
};

// In the page's order.
inline constexpr ScreenAutoInfo kScreenAutos[] = {
    {kAutoPanel, "panels", "PANEL when a panel opens", "inventory, character, stash, shop..."},
    {kAutoNpcMenu, "npcmenu", "PANEL for an NPC's menu", "its menu or talk shown at 1:1"},
    {kAutoPanelSide, "panelside", "PANEL follows the cursor", "both halves open: the cursor's half"},
    {kAutoKeyboard, "keyboard", "keyboard for a text field", "off: R + Select opens it"},
    {kAutoVideo, "video", "cinematic panel", "title, time and how to skip"},
};
inline constexpr int kScreenAutoCount = sizeof kScreenAutos / sizeof kScreenAutos[0];

constexpr bool screen_autos_fit(int i = 0)
{
    return i == kScreenAutoCount ||
           (port_text_fits(kScreenAutos[i].label) && port_text_fits(kScreenAutos[i].note) && screen_autos_fit(i + 1));
}
static_assert(screen_autos_fit(), "a SCREEN label or note is longer than the 36 characters the page shows");

inline constexpr uint32_t kScreenAutoAll = kAutoPanel | kAutoNpcMenu | kAutoPanelSide | kAutoKeyboard | kAutoVideo;
// The switches on until settings.txt says otherwise: all of them, unless
// the build is made with D2_DEFAULT_NO_PANEL_AUTO=1 (a personal build:
// docker.sh package-cia passes it on), where the three that show PANEL
// by themselves (panels, npcmenu, panelside) start off and the keyboard and
// the cinematic panel stay on. A "screenauto=" line overrides either way.
#ifndef D2_DEFAULT_NO_PANEL_AUTO
#define D2_DEFAULT_NO_PANEL_AUTO 0
#endif
inline constexpr uint32_t kScreenAutoPanelBits = kAutoPanel | kAutoNpcMenu | kAutoPanelSide;
inline constexpr uint32_t screen_auto_default(bool no_panel_auto)
{
    return no_panel_auto ? kScreenAutoAll & ~kScreenAutoPanelBits : kScreenAutoAll;
}
inline constexpr uint32_t kScreenAutoDefault = screen_auto_default(D2_DEFAULT_NO_PANEL_AUTO != 0);
// The build's defaults in words, for the log at start-up (and to tell a
// personal build's ELF by its strings).
inline const char *screen_auto_build_note()
{
    return D2_DEFAULT_NO_PANEL_AUTO ? "screenauto defaults: panels, npcmenu, panelside off (D2_DEFAULT_NO_PANEL_AUTO=1)"
                                    : "screenauto defaults: all on";
}

// settings.txt's "screenauto=": the switches, "-name" for one that is off
// ("panels,-npcmenu"). One not named keeps its default (`defaults`: the
// build's); unknown names are left out.
inline uint32_t screen_auto_bits(const char *names, uint32_t defaults = kScreenAutoDefault)
{
    uint32_t bits = defaults;
    while (*names) {
        const size_t n = std::strcspn(names, ", \r\n");
        const bool off = n > 0 && *names == '-';
        const char *name = names + (off ? 1 : 0);
        const size_t len = n - (off ? 1 : 0);
        for (const ScreenAutoInfo &o : kScreenAutos)
            if (len == std::strlen(o.name) && std::strncmp(name, o.name, len) == 0)
                bits = off ? bits & ~o.bit : bits | o.bit;
        names += n;
        names += *names ? 1 : 0;
    }
    return bits;
}

inline std::string screen_auto_names(uint32_t bits)
{
    std::string s;
    for (const ScreenAutoInfo &o : kScreenAutos) {
        if (!s.empty())
            s += ',';
        if (!(bits & o.bit))
            s += '-';
        s += o.name;
    }
    return s;
}

}  // namespace game

#endif
