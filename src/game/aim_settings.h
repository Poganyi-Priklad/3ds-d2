// The auto-target's settings in settings.txt (sdmc:/3ds/3ds-d2/settings.txt),
// one "key=value" line each. The port writes them all (so they can be
// edited by hand) and reads them at the start:
//
//   aim=1                 auto-target on A and B (0: off, A and B click as before)
//   aim_a_mode=enemies    what A (the left mouse button) aims at: enemies, corpses or off
//   aim_a_cone=45         degrees each side of the circle pad's direction
//   aim_a_range=10        subtiles from the hero (a subtile: a third of a floor tile)
//   aim_b_mode=enemies    the same for B (the right mouse button)
//   aim_b_cone=45
//   aim_b_range=20        longer, for ranged attacks and spells
//   aim_near=4            an enemy this close always comes first (subtiles)
//   aim_near_cone=90      ...when within this many degrees of the stick
//   aim_turn=60           the stick turned this far away from the target: a new one
//   aim_lost_ms=1500      the hero hasn't got closer or attacked for this long: give it up
//   aim_forget_ms=2500    no attack for this long: the target is forgotten
//   aim_height=30         pixels above the target's feet for the cursor
//   aim_confirm=4         frames to wait for the game to show the target under the cursor
//
// Out-of-range numbers are clamped to sensible limits. Header only, no
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
        {"aim_turn", &AimSettings::turn_degrees, nullptr, 0, 10, 180},
        {"aim_lost_ms", &AimSettings::lost_ms, nullptr, 0, 300, 10000},
        {"aim_forget_ms", &AimSettings::forget_ms, nullptr, 0, 0, 30000},
        {"aim_height", &AimSettings::height_px, nullptr, 0, 0, 120},
        {"aim_confirm", &AimSettings::confirm_polls, nullptr, 0, 1, 12},
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
    return false;
}

// The settings' lines, in the order the file lists them.
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
}

}  // namespace game

#endif
