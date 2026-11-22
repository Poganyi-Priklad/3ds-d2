// The auto-target's settings in settings.txt (sdmc:/3ds/3ds-d2/settings.txt),
// one "key=value" line each. The port writes them all (so they can be
// edited by hand) and reads them at the start:
//
//   aim=1                 auto-target on A and B (0: off, A and B click as before)
//   aim_a_mode=enemies    what A (the left mouse button) aims at: enemies, corpses or off
//   aim_a_cone=45         degrees each side of the circle pad's direction
//   aim_a_range=20        subtiles from the hero (a subtile: a third of a floor tile)
//   aim_b_mode=enemies    the same for B (the right mouse button)
//   aim_b_cone=45
//   aim_b_range=20
//   aim_near=4            the circle: with no enemy in the cone, the nearest this close, all round (subtiles)
//   aim_near_cone=90      no longer used (the circle is all round); kept for older files
//   aim_flick_cone=30     degrees each side of a circle pad flick's direction
//   aim_flick_ms=150      a flick gets from the centre to aim_flick_light within this time
//   aim_flick_light=40    percent of the full tilt a flick reaches
//   aim_flick_strong=80   ...and a strong flick (the hero turns)
//   aim_objects=1         plain A uses shrines, chests, waypoints, doors, levers,
//                         level warps (cave entrances, stairs), barrels
//   aim_loot=1            plain A picks up items with nothing else around
//   aim_gold_first=0      gold before other items (plain A and ZR + Down)
//   aim_lost_ms=1500      the hero hasn't got closer or attacked for this long: give it up
//   aim_forget_ms=2500    no attack for this long: the target is forgotten
//   aim_height=30         pixels above the target's feet for the cursor
//   aim_confirm=4         frames to wait for the game to show the target under the cursor
//   aim_walk_stop=1       the circle pad let go after a walk: a click just ahead of the
//                         hero stops him there (0: no click, he finishes the walk's last step)
//
// Out-of-range numbers are clamped to sensible limits. aim_turn (an older
// file's: the stick turned away from the target dropped it) is read and
// ignored; an older file's aim_a_range=10 (its default then) becomes 20. Header only, no
// platform: the app and the host test use it alike.
#ifndef D2_GAME_AIM_SETTINGS_H
#define D2_GAME_AIM_SETTINGS_H

#include "game/auto_target.h"
#include "game/settings_file.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace game {

inline const char *aim_mode_name(AimMode mode)
{
    return mode == AimMode::kOff ? "off" : mode == AimMode::kCorpses ? "corpses" : "enemies";
}

inline bool aim_mode_from_name(const std::string &name, AimMode &out)
{
    if (name == "enemies")
        out = AimMode::kEnemies;
    else if (name == "corpses")
        out = AimMode::kCorpses;
    else if (name == "off")
        out = AimMode::kOff;
    else
        return false;
    return true;
}

namespace aim_settings_detail {

// The numeric keys, where each lives in AimSettings, and its limits.
struct NumberKey {
    const char *key;
    int AimSettings::*field;   // a top-level field, or nullptr for a profile's
    int AimProfile::*profile_field;
    int profile;               // 0 A, 1 B (profile fields only)
    int lowest, highest;
};

inline const NumberKey *number_keys(int &count)
{
    static const NumberKey keys[] = {
        {"aim_a_cone", nullptr, &AimProfile::cone_degrees, 0, 5, 180},
        {"aim_a_range", nullptr, &AimProfile::range_subtiles, 0, 1, 60},
        {"aim_b_cone", nullptr, &AimProfile::cone_degrees, 1, 5, 180},
        {"aim_b_range", nullptr, &AimProfile::range_subtiles, 1, 1, 60},
        {"aim_near", &AimSettings::near_subtiles, nullptr, 0, 0, 20},
        {"aim_near_cone", &AimSettings::near_cone_degrees, nullptr, 0, 0, 180},
        {"aim_flick_cone", &AimSettings::flick_cone_degrees, nullptr, 0, 5, 90},
        {"aim_flick_ms", &AimSettings::flick_ms, nullptr, 0, 50, 500},
        {"aim_flick_light", &AimSettings::flick_light_percent, nullptr, 0, 25, 95},
        {"aim_flick_strong", &AimSettings::flick_strong_percent, nullptr, 0, 30, 100},
        {"aim_lost_ms", &AimSettings::lost_ms, nullptr, 0, 300, 10000},
        {"aim_forget_ms", &AimSettings::forget_ms, nullptr, 0, 0, 30000},
        {"aim_height", &AimSettings::height_px, nullptr, 0, 0, 120},
        {"aim_confirm", &AimSettings::confirm_polls, nullptr, 0, 1, 12},
    };
    count = (int)(sizeof keys / sizeof keys[0]);
    return keys;
}

// The on/off keys.
struct SwitchKey {
    const char *key;
    bool AimSettings::*field;
};

inline const SwitchKey *switch_keys(int &count)
{
    static const SwitchKey keys[] = {
        {"aim_objects", &AimSettings::objects},
        {"aim_loot", &AimSettings::loot},
        {"aim_gold_first", &AimSettings::gold_first},
        {"aim_walk_stop", &AimSettings::walk_stop},
    };
    count = (int)(sizeof keys / sizeof keys[0]);
    return keys;
}

inline int &number(AimSettings &s, const NumberKey &k)
{
    if (k.field)
        return s.*(k.field);
    AimProfile &profile = k.profile ? s.b : s.a;
    return profile.*(k.profile_field);
}

}  // namespace aim_settings_detail

// One settings.txt line into the settings; false if it isn't an aim key
// (or its value can't be read, which leaves the default).
inline bool parse_aim_setting(const char *line, AimSettings &s)
{
    using namespace aim_settings_detail;
    const std::string text(line);
    const std::string key = settings_line_key(text);
    if (key.empty())
        return false;
    std::string value = text.substr(key.size() + 1);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' '))
        value.pop_back();
    if (key == "aim") {
        s.enabled = std::atoi(value.c_str()) != 0;
        return true;
    }
    if (key == "aim_turn")
        return true;   // an older file's: no longer used
    if (key == "aim_a_mode" || key == "aim_b_mode") {
        AimMode mode;
        if (aim_mode_from_name(value, mode))
            (key == "aim_a_mode" ? s.a : s.b).mode = mode;
        return true;
    }
    int count = 0;
    const NumberKey *keys = number_keys(count);
    for (int i = 0; i < count; i++) {
        if (key != keys[i].key)
            continue;
        char *end = nullptr;
        const long v = std::strtol(value.c_str(), &end, 10);
        if (end != value.c_str()) {
            const long clamped = v < keys[i].lowest ? keys[i].lowest : v > keys[i].highest ? keys[i].highest : v;
            number(s, keys[i]) = (int)clamped;
        }
        return true;
    }
    const SwitchKey *switches = switch_keys(count);
    for (int i = 0; i < count; i++) {
        if (key != switches[i].key)
            continue;
        if (!value.empty())
            s.*(switches[i].field) = std::atoi(value.c_str()) != 0;
        return true;
    }
    return false;
}

// The settings' lines, in the order the file lists them.
// An older file's A range: before the flicks (a file without aim_gold_first),
// A's default range was 10 and B's 20. Such a file's aim_a_range=10 becomes
// the new default, 20, once: the file is written again with the new keys,
// so a 10 chosen later is kept.
constexpr int kOldDefaultARange = 10;

inline void migrate_older_aim_settings(AimSettings &s)
{
    if (s.a.range_subtiles == kOldDefaultARange)
        s.a.range_subtiles = AimSettings().a.range_subtiles;
}

inline void aim_settings_lines(const AimSettings &s, std::vector<SettingLine> &out)
{
    using namespace aim_settings_detail;
    out.push_back({"aim", s.enabled ? "1" : "0"});
    out.push_back({"aim_a_mode", aim_mode_name(s.a.mode)});
    out.push_back({"aim_b_mode", aim_mode_name(s.b.mode)});
    int count = 0;
    const NumberKey *keys = number_keys(count);
    AimSettings copy = s;
    for (int i = 0; i < count; i++)
        out.push_back({keys[i].key, std::to_string(number(copy, keys[i]))});
    const SwitchKey *switches = switch_keys(count);
    for (int i = 0; i < count; i++)
        out.push_back({switches[i].key, s.*(switches[i].field) ? "1" : "0"});
}

// The rows of the bottom screen's AIM > SETTINGS page: a label, the value
// as text, and "-" and "+" to change it (aim_setting_step).
constexpr int kAimSettingRows = 15;

inline const char *aim_setting_label(int row)
{
    static const char *const labels[kAimSettingRows] = {
        "Auto-target", "A aims at", "A cone", "A range", "B aims at", "B cone", "B range",
        "Flick cone", "Flick time", "Light flick", "Strong flick", "Objects", "Loot", "Gold first",
        "Walk stop",
    };
    return row >= 0 && row < kAimSettingRows ? labels[row] : "";
}

inline std::string aim_setting_value(const AimSettings &s, int row)
{
    auto on_off = [](bool on) { return std::string(on ? "on" : "off"); };
    switch (row) {
    case 0: return on_off(s.enabled);
    case 1: return aim_mode_name(s.a.mode);
    case 2: return std::to_string(s.a.cone_degrees) + " deg";
    case 3: return std::to_string(s.a.range_subtiles);
    case 4: return aim_mode_name(s.b.mode);
    case 5: return std::to_string(s.b.cone_degrees) + " deg";
    case 6: return std::to_string(s.b.range_subtiles);
    case 7: return std::to_string(s.flick_cone_degrees) + " deg";
    case 8: return std::to_string(s.flick_ms) + " ms";
    case 9: return std::to_string(s.flick_light_percent) + "%";
    case 10: return std::to_string(s.flick_strong_percent) + "%";
    case 11: return on_off(s.objects);
    case 12: return on_off(s.loot);
    case 13: return on_off(s.gold_first);
    case 14: return on_off(s.walk_stop);
    default: return "";
    }
}

// One "-" (direction -1) or "+" (1) on a row, within the same limits as
// settings.txt's; the light flick stays below the strong one. False if the
// value didn't change.
inline bool aim_setting_step(AimSettings &s, int row, int direction)
{
    auto step = [direction](int &value, int by, int lowest, int highest) {
        const int was = value;
        value += direction * by;
        value = value < lowest ? lowest : value > highest ? highest : value;
        return value != was;
    };
    auto cycle = [direction](AimMode &mode) {
        static const AimMode order[3] = {AimMode::kEnemies, AimMode::kCorpses, AimMode::kOff};
        int i = mode == AimMode::kCorpses ? 1 : mode == AimMode::kOff ? 2 : 0;
        mode = order[(i + 3 + direction) % 3];
        return true;
    };
    switch (row) {
    case 0: s.enabled = !s.enabled; return true;
    case 1: return cycle(s.a.mode);
    case 2: return step(s.a.cone_degrees, 5, 5, 180);
    case 3: return step(s.a.range_subtiles, 1, 1, 60);
    case 4: return cycle(s.b.mode);
    case 5: return step(s.b.cone_degrees, 5, 5, 180);
    case 6: return step(s.b.range_subtiles, 1, 1, 60);
    case 7: return step(s.flick_cone_degrees, 5, 5, 90);
    case 8: return step(s.flick_ms, 10, 50, 500);
    case 9: return step(s.flick_light_percent, 5, 25, s.flick_strong_percent - 5);
    case 10: return step(s.flick_strong_percent, 5, s.flick_light_percent + 5, 100);
    case 11: s.objects = !s.objects; return true;
    case 12: s.loot = !s.loot; return true;
    case 13: s.gold_first = !s.gold_first; return true;
    case 14: s.walk_stop = !s.walk_stop; return true;
    default: return false;
    }
}

}  // namespace game

#endif
