// See auto_target.h for the rules and the types.
//
// This file has three parts:
//   1. Pure helpers (classify_unit, object_kind, loot_class, the angles):
//      what a unit is, from the fields d2_units.cpp read for it.
//   2. The choices (choose_target, choose_loot, choose_flick): which unit
//      a press, a held ZR + D-pad or a flick of the circle pad goes to.
//      They only look at the lists they are given, so tests/aim drives
//      them with made-up units.
//   3. AutoTarget, the state machine Controls runs every poll: it picks a
//      target, puts the cursor on it, waits until the game itself shows the
//      unit under the cursor (the "confirm" step), and only then lets the
//      button go down. It never calls game code: its only output is the
//      cursor position and the two mouse buttons (AimResult).
#include "game/auto_target.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace game {

using namespace aim_bits;

// What a unit counts as for the auto-target (enemy, NPC, corpse, object,
// item or nothing), from the game's own flags. Checked in this order so
// that a pet, a dying monster or anything the game's hit test would never
// pick is never chosen.
AimKind classify_unit(const AimUnit &unit, bool in_town)
{
    if (unit.type == 4) {
        // An item: only one lying on the ground that the game lets you pick.
        return unit.mode == kItemOnTheGround && (unit.flags_b & kPickable) ? AimKind::kItem : AimKind::kNone;
    }
    if (unit.type == 5) {
        // A level's warp (a cave entrance, stairs, a passage to the next
        // area): used again and again, so any mode; only one the game's hit
        // test may pick (the lane showed flags_a 0x22, flags_b 0x02000080
        // on Lut Gholein's warps).
        return (unit.flags_a & kSelectable) && (unit.flags_b & kPickable) ? AimKind::kObject : AimKind::kNone;
    }
    if (unit.type == 2) {
        // An object: one of the kinds A uses, not used yet. The game keeps
        // its "can be picked" bit (flags_a bit 1) from objects.txt's
        // Selectable for the object's mode: an opened chest or a used shrine
        // loses it. A door, which stays clickable open (to close it), counts
        // only while closed; a waypoint or portal is used again and again.
        const AimObject kind = object_kind(unit);
        if (kind == AimObject::kNone || !(unit.flags_a & kSelectable) || !(unit.flags_b & kPickable))
            return AimKind::kNone;
        if (kind != AimObject::kWaypoint && kind != AimObject::kPortal && unit.mode != kObjectClosed)
            return AimKind::kNone;
        return AimKind::kObject;
    }
    // The mercenary and summons: in the client's owner list, or marked as owned.
    if (unit.owned || (unit.flags_b & kHasOwner))
        return AimKind::kNone;
    if (unit.mode == kModeDead) {
        // A corpse: only one the game lets a corpse skill use (the game's own
        // check also needs no "used up" state on it, which only the hover shows).
        if (in_town)
            return AimKind::kNone;
        return (unit.flags_a & kDead) && (unit.monstats2_flags & kMon2CorpseSelect) ? AimKind::kCorpse
                                                                                    : AimKind::kNone;
    }
    if (unit.mode == kModeDeath)
        return AimKind::kNone;   // dying
    if (!(unit.flags_b & kPickable) || (unit.flags_a & kNeverPick))
        return AimKind::kNone;   // the game's hit test never picks it
    if (unit.monstats_flags & kMonInteract)
        return AimKind::kNpc;    // someone to talk to (Akara, Cain, Tyrael...)
    if (in_town)
        return AimKind::kNone;   // in town nothing else is a target
    if (unit.monstats2_flags & (kMon2AllySelect | kMon2NoSelect))
        return AimKind::kNone;   // a guard, a statue, a critter: never hit
    if (!(unit.flags_a & kSelectable))
        return AimKind::kNone;
    return AimKind::kEnemy;
}

AimObject object_kind(const AimUnit &unit)
{
    if (unit.type == 5)
        return AimObject::kWarp;   // every warp the same: where it leads doesn't matter
    if (unit.type != 2)
        return AimObject::kNone;
    // objects.txt's OperateFn (The Phrozen Keep's list; the lane showed
    // 14 on a loose boulder, which drops an item like a chest).
    switch (unit.operate_fn) {
    case 2: return AimObject::kShrine;
    case 1:    // casket, sarcophagus
    case 3:    // urn
    case 4:    // chest
    case 14:   // crate, loose rock, corpse on a stake
    case 51:   // the jungle's containers
        return AimObject::kChest;
    case 23: return AimObject::kWaypoint;
    case 15:   // a town portal, a red portal
    case 27:   // the Arcane Sanctuary's teleport pads
    case 34:   // the Arcane Sanctuary's portal
        return AimObject::kPortal;
    case 8:    // a door
    case 18:   // a secret door
    case 29:   // the Maggot Lair's slime door
        return AimObject::kDoor;
    case 45: return AimObject::kLever;   // the Act III sewer's lever
    case 5: return AimObject::kBarrel;   // not 7: an exploding barrel hurts the hero next to it
    default: return AimObject::kNone;
    }
}

// Plain A's order among objects: lower comes first.
int object_rank(AimObject kind)
{
    switch (kind) {
    case AimObject::kShrine: return 0;
    case AimObject::kChest: return 1;
    case AimObject::kWaypoint:
    case AimObject::kPortal: return 2;
    case AimObject::kDoor:
    case AimObject::kLever:
    case AimObject::kWarp: return 3;   // a warp beside doors and levers (the player's choice)
    case AimObject::kBarrel: return 4;
    case AimObject::kNone: break;
    }
    return 99;
}

// Sorts an item on the ground into the classes the loot commands use.
AimLoot loot_class(const AimUnit &item)
{
    if (item.quality == kQualityUnique || item.quality == kQualitySet)
        return AimLoot::kUniqueOrSet;
    if (item.code == kGoldCode)
        return AimLoot::kGold;
    // A rune's code is "r01" to "r33".
    const char c0 = (char)(item.code & 0xff), c1 = (char)((item.code >> 8) & 0xff), c2 = (char)((item.code >> 16) & 0xff);
    if (c0 == 'r' && c1 >= '0' && c1 <= '9' && c2 >= '0' && c2 <= '9')
        return AimLoot::kRune;
    return AimLoot::kOther;
}

// The order items are picked up in: lower comes first.
int loot_rank(AimLoot loot, bool gold_first)
{
    if (gold_first && loot == AimLoot::kGold)
        return -1;
    switch (loot) {
    case AimLoot::kUniqueOrSet: return 0;
    case AimLoot::kRune: return 1;
    case AimLoot::kGold: return 2;
    case AimLoot::kOther: return 3;
    }
    return 3;
}

// Which classes each ZR + D-pad command picks up.
bool loot_command_takes(AimLootCommand command, AimLoot loot)
{
    switch (command) {
    case AimLootCommand::kValuables: return loot == AimLoot::kUniqueOrSet || loot == AimLoot::kRune;
    case AimLootCommand::kGold: return loot == AimLoot::kGold;
    case AimLootCommand::kEverything: return true;
    case AimLootCommand::kNone: break;
    }
    return false;
}

// The angle between the stick's direction and the way to a unit: the
// "cone" test is angle <= cone_degrees.
float aim_angle_degrees(float dir_x, float dir_y, int from_x, int from_y, int to_x, int to_y)
{
    const float vx = (float)(to_x - from_x), vy = (float)(to_y - from_y);
    const float length = std::sqrt(vx * vx + vy * vy);
    const float dir_length = std::sqrt(dir_x * dir_x + dir_y * dir_y);
    if (length < 1.0f || dir_length < 1e-6f)
        return 0.0f;   // on the hero, or no direction: in every cone
    float cosine = (vx * dir_x + vy * dir_y) / (length * dir_length);
    cosine = cosine > 1.0f ? 1.0f : cosine < -1.0f ? -1.0f : cosine;
    return std::acos(cosine) * (180.0f / 3.14159265f);
}

namespace {

// Whether the cursor can go to x,y: inside the frame, and not on the
// control panel at the bottom (its middle is low, the two globes in the
// corners reach higher). Fractions of the frame, so any resolution works.
bool point_visible(const AimView &view, int x, int y)
{
    if (x < 8 || x > view.width - 8 || y < 8)
        return false;
    if (view.area_right > view.area_left && (x < view.area_left + 8 || x > view.area_right - 8))
        return false;   // under a panel open on one side
    const bool over_a_globe = x < view.width * 0.19f || x > view.width * 0.81f;
    return y <= (over_a_globe ? view.height * 0.78f : view.height * 0.86f);
}

// Whether a kind of unit is something a profile aims at (button_a: plain
// A's, which also takes objects and items).
bool kind_fits(AimKind kind, AimMode mode, bool in_town, bool button_a)
{
    if (mode == AimMode::kOff || kind == AimKind::kNone)
        return false;
    if (kind == AimKind::kObject || kind == AimKind::kItem)
        return button_a && mode == AimMode::kEnemies;
    if (in_town)
        return kind == AimKind::kNpc;
    if (mode == AimMode::kCorpses)
        return kind == AimKind::kCorpse;
    return kind == AimKind::kEnemy || kind == AimKind::kNpc;
}

// Whether a unit's key (aim_key) is in a skip list.
bool contains(const std::vector<uint64_t> &keys, uint64_t key)
{
    for (uint64_t x : keys)
        if (x == key)
            return true;
    return false;
}

// How far apart two points on the screen are on the floor: the isometric
// view draws a floor square twice as wide as it is high, so the height
// counts twice.
float floor_distance(int x0, int y0, int x1, int y1)
{
    const float dx = (float)(x1 - x0), dy = 2.0f * (float)(y1 - y0);
    return std::sqrt(dx * dx + dy * dy);
}

// The cursor's heights above a unit's feet to try, in turn: a monster's
// body, an object's (chests are low, shrines tall), an item's (flat on
// the floor; the lane picked gold up from 4 pixels above its spot) and a
// warp's (its spot is the warp's corner nearest the top of the screen: the
// stairs or the opening are drawn below it, so below it is tried first).
// A corpse lies on the floor: the game shows it under the cursor only from
// about 4 to 16 pixels below its feet point (a hover scan on the lane; above
// the feet it shows nothing, or what stands there), so it is aimed below.
void target_heights(uint32_t type, bool corpse, int height_px, int out[4])
{
    if (corpse) {
        const int body[4] = {-8, -12, -4, -16};
        for (int i = 0; i < 4; i++)
            out[i] = body[i];
    } else if (type == 5) {
        const int warp[4] = {-16, -28, -4, -40};
        for (int i = 0; i < 4; i++)
            out[i] = warp[i];
    } else if (type == 4) {
        const int item[4] = {4, 10, 1, 16};
        for (int i = 0; i < 4; i++)
            out[i] = item[i];
    } else if (type == 2) {
        const int object[4] = {16, 8, 28, 44};
        for (int i = 0; i < 4; i++)
            out[i] = object[i];
    } else {
        const int monster[4] = {height_px, height_px / 2, height_px * 3 / 2, height_px * 2};
        for (int i = 0; i < 4; i++)
            out[i] = monster[i];
    }
}

// How high above its feet a unit is seen by point_visible (its body; a
// corpse's below its feet, where the game shows it).
int visible_height(uint32_t type, int height_px, bool corpse = false)
{
    return corpse ? -8 : type == 1 ? height_px : type == 2 ? 16 : type == 5 ? -16 : 4;
}

}  // namespace

int choose_target(const std::vector<AimUnit> &units, const std::vector<AimKind> &kinds, const AimView &view,
                  const AimProfile &profile, const AimSettings &settings, float dir_x, float dir_y,
                  const std::vector<uint64_t> &skip, bool *near_rule, bool button_a)
{
    if (near_rule)
        *near_rule = false;
    if (profile.mode == AimMode::kOff)
        return -1;
    const float near_cone =
        (float)(settings.near_cone_degrees > profile.cone_degrees ? settings.near_cone_degrees : profile.cone_degrees);
    int nearest_near_enemy = -1, best_enemy = -1, nearest_npc = -1, nearest_corpse = -1, best_object = -1,
        best_item = -1;
    float best_enemy_score = 0;
    for (size_t i = 0; i < units.size() && i < kinds.size(); i++) {
        const AimUnit &u = units[i];
        const AimKind kind = kinds[i];
        if (!kind_fits(kind, profile.mode, view.in_town, button_a) || contains(skip, aim_key(u.type, u.id)))
            continue;
        if (kind == AimKind::kObject && !settings.objects)
            continue;
        if (kind == AimKind::kItem && !settings.loot)
            continue;
        if (!point_visible(view, u.feet_x, u.feet_y - visible_height(u.type, settings.height_px, kind == AimKind::kCorpse)))
            continue;
        const float angle = aim_angle_degrees(dir_x, dir_y, view.hero_x, view.hero_y, u.feet_x, u.feet_y);
        const bool in_cone = angle <= (float)profile.cone_degrees && u.distance <= (float)profile.range_subtiles;
        if (kind == AimKind::kEnemy) {
            // The safety rule: an enemy next to the hero, anywhere in front.
            if (u.distance <= (float)settings.near_subtiles && angle <= near_cone &&
                (nearest_near_enemy < 0 || u.distance < units[nearest_near_enemy].distance))
                nearest_near_enemy = (int)i;
            // Otherwise the nearest in the cone, a little in favour of the
            // one the stick points at most exactly.
            if (in_cone) {
                const float score = u.distance * (1.0f + 0.5f * (1.0f - std::cos(angle * 3.14159265f / 180.0f)));
                if (best_enemy < 0 || score < best_enemy_score) {
                    best_enemy = (int)i;
                    best_enemy_score = score;
                }
            }
        } else if (kind == AimKind::kNpc) {
            if (in_cone && (nearest_npc < 0 || u.distance < units[nearest_npc].distance))
                nearest_npc = (int)i;
        } else if (kind == AimKind::kCorpse) {
            if (in_cone && (nearest_corpse < 0 || u.distance < units[nearest_corpse].distance))
                nearest_corpse = (int)i;
        } else if (kind == AimKind::kObject) {
            // The best kind first (a shrine before a chest...), then the nearest.
            if (in_cone) {
                const int rank = object_rank(object_kind(u));
                const int best_rank = best_object < 0 ? 0 : object_rank(object_kind(units[best_object]));
                if (best_object < 0 || rank < best_rank ||
                    (rank == best_rank && u.distance < units[best_object].distance))
                    best_object = (int)i;
            }
        } else if (kind == AimKind::kItem) {
            // The nearest; with "gold first", the nearest gold before any other.
            if (in_cone) {
                const bool gold = settings.gold_first && loot_class(u) == AimLoot::kGold;
                const bool best_gold = best_item >= 0 && settings.gold_first && loot_class(units[best_item]) == AimLoot::kGold;
                if (best_item < 0 || (gold && !best_gold) ||
                    (gold == best_gold && u.distance < units[best_item].distance))
                    best_item = (int)i;
            }
        }
    }
    if (profile.mode == AimMode::kCorpses && !view.in_town)
        return nearest_corpse;
    if (view.in_town) {
        // No enemies in town: NPCs to talk to first, then objects (the
        // waypoint, a portal), then items.
        if (nearest_npc >= 0)
            return nearest_npc;
        return best_object >= 0 ? best_object : best_item;
    }
    if (nearest_near_enemy >= 0) {
        if (near_rule)
            *near_rule = true;
        return nearest_near_enemy;
    }
    if (best_enemy >= 0)
        return best_enemy;
    // No enemy in the cone: plain A's order is objects, NPCs, then items.
    if (best_object >= 0)
        return best_object;
    if (nearest_npc >= 0)
        return nearest_npc;
    return best_item;
}

int choose_loot(const std::vector<AimUnit> &units, const std::vector<AimKind> &kinds, const AimView &view,
                AimLootCommand command, const AimSettings &settings, int range_subtiles,
                const std::vector<uint64_t> &skip)
{
    int best = -1, best_rank = 0;
    for (size_t i = 0; i < units.size() && i < kinds.size(); i++) {
        const AimUnit &u = units[i];
        if (kinds[i] != AimKind::kItem || u.distance > (float)range_subtiles || contains(skip, aim_key(u.type, u.id)))
            continue;
        const AimLoot loot = loot_class(u);
        if (!loot_command_takes(command, loot) || !point_visible(view, u.feet_x, u.feet_y - 4))
            continue;
        const int rank = loot_rank(loot, settings.gold_first);
        if (best < 0 || rank < best_rank || (rank == best_rank && u.distance < units[best].distance)) {
            best = (int)i;
            best_rank = rank;
        }
    }
    return best;
}

int choose_flick(const std::vector<AimUnit> &units, const std::vector<AimKind> &kinds, const AimView &view,
                 const AimProfile &profile, const AimSettings &settings, const AimUnit &current, float flick_x,
                 float flick_y, bool strong, const std::vector<uint64_t> &skip)
{
    const float cone = (float)settings.flick_cone_degrees;
    // The way the hero is fighting: from the hero to the target.
    const float fight_x = (float)(current.feet_x - view.hero_x), fight_y = (float)(current.feet_y - view.hero_y);
    const bool same_way = aim_angle_degrees(fight_x, fight_y, 0, 0, (int)(flick_x * 1000), (int)(flick_y * 1000)) <= cone;
    int next_further = -1, nearest_ahead = -1, step = -1, turn = -1;
    float step_distance = 0;
    for (size_t i = 0; i < units.size() && i < kinds.size(); i++) {
        const AimUnit &u = units[i];
        if (kinds[i] != AimKind::kEnemy || u.distance > (float)profile.range_subtiles ||
            contains(skip, aim_key(u.type, u.id)) ||
            !point_visible(view, u.feet_x, u.feet_y - settings.height_px))
            continue;
        if (same_way) {
            // Along the fighting line, from the hero.
            if (aim_angle_degrees(fight_x, fight_y, view.hero_x, view.hero_y, u.feet_x, u.feet_y) > cone)
                continue;
            if (u.id != current.id && u.distance > current.distance &&
                (next_further < 0 || u.distance < units[next_further].distance))
                next_further = (int)i;
            if (nearest_ahead < 0 || u.distance < units[nearest_ahead].distance)
                nearest_ahead = (int)i;
        } else if (!strong) {
            // A step sideways: in the flick's direction from the target.
            if (u.id == current.id ||
                aim_angle_degrees(flick_x, flick_y, current.feet_x, current.feet_y, u.feet_x, u.feet_y) > cone)
                continue;
            const float d = floor_distance(current.feet_x, current.feet_y, u.feet_x, u.feet_y);
            if (step < 0 || d < step_distance) {
                step = (int)i;
                step_distance = d;
            }
        } else {
            // The hero turns: in the profile's cone round the flick, from the hero.
            if (aim_angle_degrees(flick_x, flick_y, view.hero_x, view.hero_y, u.feet_x, u.feet_y) >
                (float)profile.cone_degrees)
                continue;
            if (turn < 0 || u.distance < units[turn].distance)
                turn = (int)i;
        }
    }
    int chosen = same_way ? (next_further >= 0 ? next_further : nearest_ahead) : strong ? turn : step;
    if (chosen >= 0 && units[(size_t)chosen].id == current.id)
        chosen = -1;   // round again to itself: the target stays
    return chosen;
}

AimFlicks::Flick AimFlicks::update(float x, float y, uint64_t now_ms, const AimSettings &settings)
{
    Flick f;
    const float tilt = std::sqrt(x * x + y * y);
    const float light = settings.flick_light_percent / 100.0f, strong = settings.flick_strong_percent / 100.0f;
    if (tilt < kCentre) {
        // Back in the centre: a flick being read ends here (a quick one,
        // out and back), and the next may come.
        if (rising_) {
            f.happened = true;
            f.strong = peak_ >= strong;
            f.dir_x = peak_x_ / peak_;
            f.dir_y = peak_y_ / peak_;
        }
        armed_ = true;
        rising_ = false;
        centre_ms_ = now_ms;
        return f;
    }
    if (!armed_)
        return f;
    if (!rising_) {
        if (now_ms - centre_ms_ > (uint64_t)settings.flick_ms) {
            armed_ = false;   // too slow out of the centre: a tilt, not a flick
            return f;
        }
        if (tilt < light)
            return f;
        rising_ = true;
        rise_ms_ = now_ms;
        peak_ = 0;
    }
    if (tilt > peak_) {
        peak_ = tilt;
        peak_x_ = x;
        peak_y_ = y;
    }
    // Read it once it is as far as it gets: strong already, coming back, or
    // a moment after it got past the light threshold.
    if (peak_ >= strong || tilt < peak_ - 0.05f || now_ms - rise_ms_ >= (uint64_t)kSettleMs) {
        f.happened = true;
        f.strong = peak_ >= strong;
        f.dir_x = peak_x_ / peak_;
        f.dir_y = peak_y_ / peak_;
        armed_ = rising_ = false;
    }
    return f;
}

// Whether the hero is casting: a skill went off (a corpse it was aimed at
// is used up).
bool AutoTarget::casting_mode(uint32_t hero_mode)
{
    // 10 a cast (SC), 13 to 16 the skill animations (S1-S4); not 7-9, 11, 12
    // (attacks, block, throw, kick).
    return hero_mode == 10 || (hero_mode >= 13 && hero_mode <= 16);
}

// Whether the hero is in the middle of an attack or a cast: used to tell
// "still fighting" from "stuck" when deciding to give a target up.
bool AutoTarget::attacking_mode(uint32_t hero_mode)
{
    // The player's unit modes 7 to 16 (attack 1 and 2, block, cast, throw,
    // kick, skills 1 to 4) and 18 (a sequence, such as Whirlwind).
    return (hero_mode >= 7 && hero_mode <= 16) || hero_mode == 18;
}

// One "aim:" line for game.log, at most six a second (a held button polls
// every frame; the log must not flood the SD card).
void AutoTarget::say(uint64_t now_ms, const char *fmt, ...)
{
    if (!log_)
        return;
    if (now_ms - log_window_ms_ >= 1000) {
        if (log_skipped_ > 0) {
            char note[64];
            std::snprintf(note, sizeof note, "aim: (%d lines not logged)", log_skipped_);
            log_(note);
        }
        log_window_ms_ = now_ms;
        log_lines_ = 0;
        log_skipped_ = 0;
    }
    if (log_lines_ >= 6) {
        log_skipped_++;
        return;
    }
    log_lines_++;
    char line[160] = "aim: ";
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line + 5, sizeof line - 5, fmt, args);
    va_end(args);
    log_(line);
}

// Remembers a unit not to choose again until until_ms (it was tried and
// the game never showed it, or the hero could not reach it). The list is
// short and old entries are pruned as new ones come.
void AutoTarget::skip(uint64_t key, uint64_t until_ms)
{
    for (size_t i = 0; i < skips_.size();)
        if (skips_[i].key == key || skips_[i].until_ms + 60000 < until_ms)
            skips_.erase(skips_.begin() + (long)i);
        else
            i++;
    if (skips_.size() < 32)
        skips_.push_back({key, until_ms});
}

// Lets the current target go (why: for the log; counter: the stat to count).
void AutoTarget::drop(const char *why, uint32_t &counter, uint64_t now_ms)
{
    say(now_ms, "target %u dropped: %s", target_id_, why);
    counter++;
    target_id_ = 0;
    target_kind_ = AimKind::kNone;
    phase_ = Phase::kIdle;
    paused_ = holding_ && !pressed_now_;   // under a held button: no plain click onto where it was
    scan_now_ = true;                      // the next one is looked for at once
}

// The cursor has just been put on a new target: from the next frame on,
// wait for the game to report that unit under the cursor.
void AutoTarget::start_confirming(uint64_t now_ms)
{
    phase_ = Phase::kConfirming;
    confirm_frames_ = 0;
    cursor_moving_ = true;   // the cursor goes there this poll; the game looks from the next frame on
    missed_frames_ = 0;
    progress_ms_ = now_ms;
}

// Reads one unit again by its type and id: monsters and NPCs from the
// monster table, objects and items from the ground tables.
bool AutoTarget::look_up(AimWorld &world, uint32_t type, uint32_t id, AimUnit &out)
{
    return type == 1 ? world.unit(id, out) : world.ground_unit(type, id, out);
}

// Whether a unit of this kind is something the active button aims at.
bool AutoTarget::fits(AimKind kind, const AimUnit &unit, const AimProfile &profile, bool in_town) const
{
    if (active_profile_ == 2)
        return kind == AimKind::kItem && loot_command_takes(loot_, loot_class(unit));
    if (kind == AimKind::kObject && !settings_.objects)
        return false;
    if (kind == AimKind::kItem && !settings_.loot)
        return false;
    return kind_fits(kind, profile.mode, in_town, active_profile_ == 0);
}

// A full read of the units: monsters and NPCs, and (ground) the objects
// and items; their kinds, and the keys not to choose now.
void AutoTarget::read_units(const AimPoll &poll, const AimView &view, AimWorld &world, bool ground)
{
    units_.clear();
    world.units(units_);
    if (ground)
        world.ground_units(units_);
    stats_.scans++;
    last_scan_ms_ = poll.now_ms;
    kinds_.resize(units_.size());
    for (size_t i = 0; i < units_.size(); i++)
        kinds_[i] = classify_unit(units_[i], view.in_town);
    skip_keys_.clear();
    for (const Skip &s : skips_)
        if (poll.now_ms < s.until_ms)
            skip_keys_.push_back(s.key);
}

void AutoTarget::take(const AimUnit &u, AimKind kind, const AimPoll &poll, const char *why)
{
    target_id_ = u.id;
    target_type_ = u.type;
    target_kind_ = kind;
    height_index_ = 0;
    npc_clicked_ = false;
    reached_ms_ = 0;
    best_distance_ = u.distance;
    stats_.targets++;
    if (kind == AimKind::kObject)
        stats_.objects++;
    else if (kind == AimKind::kItem)
        stats_.items++;
    const char *name = kind == AimKind::kNpc      ? "NPC"
                       : kind == AimKind::kCorpse ? "corpse"
                       : kind == AimKind::kObject ? (u.type == 5 ? "warp" : "object")
                       : kind == AimKind::kItem   ? "item"
                                                  : "enemy";
    say(poll.now_ms, "%s %u (class %u) %.1f subtiles away, %s", name, u.id, u.cls, (double)u.distance, why);
    start_confirming(poll.now_ms);
}

// A full read of the units, and the active button's choice in it.
bool AutoTarget::pick(const AimPoll &poll, const AimView &view, AimWorld &world, const AimProfile &profile,
                      bool pressed)
{
    read_units(poll, view, world, active_profile_ != 1);
    // A warp only on a press of A, never while it is held: held, the hero
    // would come out of a warp next to the one leading back and take that
    // at once (the lane showed him going in and out of a cave that way).
    if (!pressed)
        for (const AimUnit &u : units_)
            if (u.type == 5)
                skip_keys_.push_back(aim_key(u.type, u.id));
    int i;
    bool near_rule = false;
    if (active_profile_ == 2)
        i = choose_loot(units_, kinds_, view, loot_, settings_, settings_.a.range_subtiles, skip_keys_);
    else
        i = choose_target(units_, kinds_, view, profile, settings_, poll.dir_x, poll.dir_y, skip_keys_, &near_rule,
                          active_profile_ == 0);
    if (i < 0)
        return false;
    const AimUnit &u = units_[(size_t)i];
    const AimKind kind = kinds_[(size_t)i];
    if (target_id_) {
        // A target is kept: no switching to "the nearest" by itself (a
        // flick switches). Only an enemy beats an NPC, an object or an item
        // (the hero is in danger), and on a fresh press an enemy next to the
        // hero beats one further away.
        if (u.id == target_id_ && u.type == target_type_)
            return true;
        const bool enemy_over_other = kind == AimKind::kEnemy && target_kind_ != AimKind::kEnemy;
        AimUnit current;
        const bool current_near = look_up(world, target_type_, target_id_, current) &&
                                  current.distance <= (float)settings_.near_subtiles;
        const bool near_on_press = pressed && near_rule && !current_near;
        if (!enemy_over_other && !near_on_press)
            return true;
        say(poll.now_ms, "target %u replaced by %u (%s)", target_id_, u.id,
            enemy_over_other ? "an enemy comes first" : "an enemy next to the hero");
    }
    if (near_rule)
        stats_.near_rule++;
    take(u, kind, poll,
         active_profile_ == 2 ? "picking up"
         : near_rule          ? (pressed ? "next to the hero, pressed" : "next to the hero, while held")
         : pressed            ? (active_profile_ ? "B pressed" : "A pressed")
                              : "while held");
    return true;
}

AimResult AutoTarget::update(const AimPoll &poll, AimWorld &world)
{
    AimResult result;
    const uint64_t now = poll.now_ms;
    // Flicks are followed every poll (the stick has to come back to the
    // centre between two), and used only while fighting.
    const AimFlicks::Flick flick = flicks_.update(poll.stick_x, poll.stick_y, now, settings_);
    const bool looting = poll.loot != AimLootCommand::kNone;
    const bool any_held = poll.a_held || poll.b_held || looting;

    if (!settings_.enabled || !poll.allowed) {
        // A panel opened (an NPC's menu: the talk happened), an item on the
        // cursor, L held...: hands off. An NPC that was talked to is let go.
        if (holding_ && target_id_ && target_kind_ == AimKind::kNpc && settings_.enabled) {
            say(now, "NPC %u reached", target_id_);
            target_id_ = 0;
        }
        holding_ = false;
        paused_ = false;
        if (phase_ != Phase::kIdle)
            phase_ = Phase::kIdle;
        return result;
    }

    // Which button: a loot command while it is held, else the button
    // pressed last (or the only one held).
    if (looting) {
        if (active_profile_ != 2 || loot_ != poll.loot)
            scan_now_ = true;
        active_profile_ = 2;
        loot_ = poll.loot;
    } else {
        if (active_profile_ == 2)
            active_profile_ = 0;
        if (poll.a_pressed)
            active_profile_ = 0;
        else if (poll.b_pressed)
            active_profile_ = 1;
        if (poll.a_held && !poll.b_held)
            active_profile_ = 0;
        else if (poll.b_held && !poll.a_held)
            active_profile_ = 1;
    }
    const bool pressed = looting ? poll.loot_pressed : poll.a_pressed || poll.b_pressed;
    pressed_now_ = pressed;
    if (pressed) {
        stats_.presses++;
        paused_ = false;
    }

    if (!any_held) {
        // Released: the target is remembered (the next press goes back to
        // it), until forget_ms after the last attack.
        holding_ = false;
        paused_ = false;
        phase_ = Phase::kIdle;
        if (target_id_ && now - last_attack_ms_ > (uint64_t)settings_.forget_ms) {
            say(now, "target %u forgotten", target_id_);
            stats_.forgotten++;
            target_id_ = 0;
        }
        return result;
    }
    if (pressed && !holding_ && target_id_ && now - last_attack_ms_ > (uint64_t)settings_.forget_ms) {
        stats_.forgotten++;   // pressed again after a long pause (the release poll may not have seen it)
        target_id_ = 0;
    }
    holding_ = true;

    AimView view;
    AimProfile profile = active_profile_ == 1 ? settings_.b : settings_.a;
    if (active_profile_ != 2 && profile.mode == AimMode::kOff)
        return result;   // this button clicks as it always did
    if (!world.view(view))
        return result;
    view.area_left = poll.area_left;   // a panel on one side: its half is left out
    view.area_right = poll.area_right;
    // B with a skill cast on corpses (Raise Skeleton, Corpse Explosion...):
    // B aims at corpses by itself.
    if (active_profile_ == 1 && profile.mode == AimMode::kEnemies && view.right_skill_on_corpses)
        profile.mode = AimMode::kCorpses;

    // The kept target: still there, still what it was, still on the screen.
    AimUnit current;
    if (target_id_) {
        if (!look_up(world, target_type_, target_id_, current)) {
            drop(target_kind_ == AimKind::kItem ? "picked up" : "gone", stats_.dropped_gone, now);
        } else {
            const AimKind kind = classify_unit(current, view.in_town);
            if (kind != target_kind_) {
                drop(current.mode == kModeDeath || current.mode == kModeDead ? "dead"
                     : target_kind_ == AimKind::kObject                     ? "used"
                                                                             : "no longer a target",
                     stats_.dropped_dead, now);
            } else if (!fits(kind, current, profile, view.in_town)) {
                target_id_ = 0;   // not what this button aims at (A on enemies, B on corpses): a new choice
                phase_ = Phase::kIdle;
            } else if (!point_visible(view, current.feet_x,
                                      current.feet_y - visible_height(target_type_, settings_.height_px,
                                                                      target_kind_ == AimKind::kCorpse))) {
                drop("off the screen", stats_.dropped_screen, now);
            }
        }
    }

    // A flick of the circle pad while fighting an enemy: another enemy.
    if (flick.happened && target_id_ && target_kind_ == AimKind::kEnemy && active_profile_ != 2 &&
        profile.mode == AimMode::kEnemies) {
        stats_.flicks++;
        read_units(poll, view, world, false);
        const int i = choose_flick(units_, kinds_, view, profile, settings_, current, flick.dir_x, flick.dir_y,
                                   flick.strong, skip_keys_);
        if (i >= 0) {
            stats_.flick_switches++;
            current = units_[(size_t)i];
            take(current, kinds_[(size_t)i], poll, flick.strong ? "a strong flick" : "a light flick");
        } else {
            say(now, "%s flick: nobody there, target %u kept", flick.strong ? "strong" : "light", target_id_);
        }
    }

    // A new target: on a press, or now and then while held (a new enemy in
    // the cone while the hero goes to an NPC, an object or an item; the
    // next item to pick up). An enemy is never replaced while held.
    const uint64_t rescan_ms = target_id_ ? 250 : 150;
    const bool may_be_beaten = target_id_ && !view.in_town && active_profile_ != 2 &&
                               profile.mode == AimMode::kEnemies && target_kind_ != AimKind::kEnemy;
    if ((!target_id_ || may_be_beaten || (pressed && target_kind_ == AimKind::kEnemy &&
                                          current.distance > (float)settings_.near_subtiles)) &&
        (pressed || scan_now_ || now - last_scan_ms_ >= rescan_ms)) {
        scan_now_ = false;
        if (pick(poll, view, world, profile, pressed) && target_id_ != current.id)
            look_up(world, target_type_, target_id_, current);
    }
    if (!target_id_) {
        // A or B: a plain click where the cursor is. ZR + D-pad: nothing at
        // all (it has no mouse button of its own).
        if (pressed) {
            stats_.no_target++;
            if (active_profile_ == 2)
                say(now, "loot: nothing to pick up, nothing done");
            else
                say(now, "%s: nothing to aim at, a plain click", active_profile_ ? "B" : "A");
        }
        result.hold_plain_buttons = paused_;
        return result;
    }
    if (phase_ == Phase::kIdle)
        start_confirming(now);   // a remembered target: the cursor back on it, then the press

    // A skill cast on a corpse uses it up (Find Item, Raise Skeleton,
    // Corpse Explosion...): once the hero goes into a cast while B is on a
    // corpse (and was not already casting when it was chosen), that corpse
    // is left alone from then on and the next one taken. The game keeps no
    // sign of it in the unit that could be read instead: on the lane only
    // its random seed changed, and the hover stopped showing it.
    if (target_kind_ == AimKind::kCorpse) {
        if (!casting_mode(view.hero_mode)) {
            corpse_ready_id_ = target_id_;
        } else if (corpse_ready_id_ == target_id_) {
            skip(aim_key(target_type_, target_id_), now + kUsedCorpseSkipMs);
            drop("a skill was cast on it", stats_.dropped_dead, now);
            result.hold_plain_buttons = true;
            return result;
        }
    }

    int heights[4];
    target_heights(target_type_, target_kind_ == AimKind::kCorpse, settings_.height_px, heights);
    const bool on_target = view.hover_on && view.hover_type == target_type_ && view.hover_id == target_id_;
    // An NPC, an object and an item are clicked; an enemy is held.
    const bool clicked = target_kind_ == AimKind::kNpc || target_kind_ == AimKind::kObject ||
                         target_kind_ == AimKind::kItem;
    bool press = false;

    if (phase_ == Phase::kConfirming) {
        const bool look = poll.new_frame && !cursor_moving_;
        cursor_moving_ = false;
        if (look)
            confirm_frames_++;
        if (look) {
            AimUnit other;
            if (on_target) {
                phase_ = Phase::kConfirmed;
                stats_.confirmed++;
            } else if (view.hover_on && view.hover_type == 1 && target_kind_ == AimKind::kEnemy &&
                       view.hover_id != target_id_ && world.unit(view.hover_id, other) &&
                       classify_unit(other, view.in_town) == AimKind::kEnemy) {
                // The game shows another enemy there (one in front of ours):
                // that one is hit, so it becomes the target.
                say(now, "target %u: the game shows enemy %u there, taken instead", target_id_, other.id);
                target_id_ = other.id;
                current = other;
                phase_ = Phase::kConfirmed;
                stats_.adopted++;
            } else if (confirm_frames_ >= settings_.confirm_polls) {
                // Never shown: try another unit, else a plain click at it.
                const uint32_t failed = target_id_, failed_type = target_type_;
                const AimKind failed_kind = target_kind_;
                skip(aim_key(failed_type, failed), now + 2000);
                target_id_ = 0;
                if (pick(poll, view, world, profile, false) && look_up(world, target_type_, target_id_, current)) {
                    say(now, "target %u never under the cursor, trying %u", failed, target_id_);
                } else {
                    target_id_ = failed;
                    target_type_ = failed_type;
                    target_kind_ = failed_kind;
                    look_up(world, target_type_, target_id_, current);
                    phase_ = Phase::kFallback;
                    stats_.fallbacks++;
                    say(now, "target %u never under the cursor: a plain click at it", failed);
                }
            } else {
                height_index_ = (height_index_ + 1) % 4;   // the next height
            }
            if (phase_ == Phase::kConfirmed || phase_ == Phase::kFallback) {
                best_distance_ = current.distance;
                progress_ms_ = now;
                missed_frames_ = 0;
            }
        }
        press = phase_ == Phase::kConfirmed || phase_ == Phase::kFallback;
    } else if (phase_ == Phase::kConfirmed && clicked) {
        // Once the game has shown it there, the cursor stays at that height
        // and the clicks go on: the game shows nothing under the cursor for
        // a while when the hero gets to an NPC (seen on the lane).
        press = true;
    } else if (phase_ == Phase::kConfirmed) {
        if (poll.new_frame)
            missed_frames_ = on_target ? 0 : missed_frames_ + 1;
        if (missed_frames_ >= settings_.confirm_polls)
            start_confirming(now);   // lost under the cursor for a while: look for it again
        else
            press = missed_frames_ == 0;   // hold only while the game shows it (else a click is a walk)
    } else if (phase_ == Phase::kFallback) {
        press = true;
        if (on_target && poll.new_frame) {
            phase_ = Phase::kConfirmed;
            missed_frames_ = 0;
        }
    }

    // Getting there: the hero attacking, or closer than before. Else, after
    // lost_ms, the target is given up (no endless chase round an obstacle).
    // An item the hero stands next to and still doesn't pick up has no room
    // in the inventory: it is skipped for a while, and the next one taken.
    if (phase_ == Phase::kConfirmed || phase_ == Phase::kFallback) {
        if (target_kind_ == AimKind::kItem && current.distance <= kItemReachSubtiles) {
            if (!reached_ms_)
                reached_ms_ = now;
            if (now - reached_ms_ > (uint64_t)settings_.lost_ms) {
                skip(aim_key(target_type_, target_id_), now + kNoRoomSkipMs);
                drop("no room for it", stats_.no_room, now);
                result.hold_plain_buttons = true;
                return result;
            }
        }
        if (attacking_mode(view.hero_mode) || current.distance < best_distance_ - 0.75f ||
            (target_kind_ == AimKind::kNpc && npc_clicked_ && now - npc_click_ms_ < (uint64_t)kNpcClickAgainMs) ||
            reached_ms_) {
            best_distance_ = current.distance;
            progress_ms_ = now;
        } else if (now - progress_ms_ > (uint64_t)settings_.lost_ms) {
            skip(aim_key(target_type_, target_id_), now + 3000);
            drop("the hero can't get to it", stats_.dropped_lost, now);
            result.hold_plain_buttons = true;
            return result;
        }
        if (poll.new_frame) {
            stats_.held_polls++;
            if (on_target)
                stats_.held_on_target++;
        }
    }

    // An NPC, an object or an item is clicked, not held: the game opens an
    // NPC's menu when the hero gets there only once the button is up (the
    // lane showed a held button keeping the menu shut). One short click, and
    // another every second while nothing happened (it walked off).
    if (clicked && press) {
        if (!npc_clicked_ || now - npc_click_ms_ >= (uint64_t)kNpcClickAgainMs) {
            npc_click_ms_ = now;
            npc_clicked_ = true;
        }
        press = now - npc_click_ms_ < (uint64_t)kNpcClickMs;
    }

    last_attack_ms_ = now;
    result.aiming = true;
    result.cursor_x = current.feet_x;
    result.cursor_y = current.feet_y - heights[height_index_];
    // A point on the hero's own figure is a click on nothing: for a unit
    // standing just below the hero, its lowest part (by its feet) is used.
    const bool on_hero = std::abs(result.cursor_x - view.hero_x) < kHeroHalfWidthPx &&
                         result.cursor_y > view.hero_y - kHeroHeightPx && result.cursor_y < view.hero_y + kHeroFeetPx;
    if (on_hero && current.feet_y - kFeetLiftPx >= view.hero_y + kHeroFeetPx)
        result.cursor_y = current.feet_y - kFeetLiftPx;
    // A loot command and plain A pick up with the left button.
    result.left = press && (poll.a_held || active_profile_ == 2);
    result.right = press && poll.b_held && active_profile_ != 2;
    return result;
}

}  // namespace game
