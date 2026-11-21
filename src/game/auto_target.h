// Auto-target for A and B (the left and right mouse buttons): when A or B is
// pressed in the game world, the cursor jumps onto the enemy (or NPC, or
// corpse) the circle pad points at, and the button is pressed only once the
// game itself shows that unit under the cursor. The game then does what a
// click on that unit does (walk up and hit it, shoot it, cast at it, talk to
// it). Only mouse input is generated; no game function is called.
//
// The rules (agreed with the player while it was designed):
//   - Two profiles, A and B, each with a mode (enemies, corpses or off), a
//     cone (degrees each side of the stick's direction) and a range.
//   - The direction is the circle pad's, or the last walking direction
//     while the pad is centred.
//   - An enemy within the "near" radius always comes first, even for a
//     long-range profile, so a ranged attack never ignores the enemy next
//     to the hero.
//   - Plain A with no enemy in the cone takes, in this order: an object
//     (shrine, chest, waypoint or portal, door or lever or a level's warp -
//     a cave entrance, stairs, a passage to the next area -, barrel; used
//     ones skipped), an NPC, the nearest item on the ground (gold first if the
//     player asks for it). B only aims at enemies and NPCs (or corpses).
//   - In town NPCs come first (to talk to), then objects and items; there
//     are no enemies there.
//   - B aims at corpses by itself while the right-button skill is one that
//     is cast on a corpse (Raise Skeleton, Corpse Explosion, Find Item...).
//   - The mercenary, summons and other players are never targets.
//   - The target is kept while A and B are swapped and across release and
//     press again, and never switches to "the nearest" by itself: only a
//     flick of the circle pad switches between enemies (choose_flick). It
//     is dropped when it dies, leaves the screen, or the hero can't get to
//     it for a while, and forgotten a few seconds after the last attack.
//   - ZR + D-pad, held: pick up items one after another (valuables, gold,
//     or everything), all around the hero. An item the hero stands by but
//     can't pick up (no room) is skipped for a while and the next is taken.
//   - Nothing to aim at: the button clicks where the cursor is, as before.
//
// This file is the pure logic: it reads the game through the AimWorld
// interface (d2_units.cpp reads Diablo II's memory; the host test gives it
// made-up units), so every rule can be tested without the game.
#ifndef D2_GAME_AUTO_TARGET_H
#define D2_GAME_AUTO_TARGET_H

#include <cstdint>
#include <string>
#include <vector>

namespace game {

// What a profile aims at.
enum class AimMode : uint8_t { kOff, kEnemies, kCorpses };

// What a unit is, for the auto-target.
enum class AimKind : uint8_t { kNone, kEnemy, kNpc, kCorpse, kObject, kItem };

// The objects (unit type 2) plain A uses, from objects.txt's OperateFn
// (object_kind below), and a level's warps (unit type 5: a cave entrance,
// stairs, a passage to the next area; a click on one takes the hero
// there). Other objects (quest objects, braziers, the stash...) are never
// targets.
enum class AimObject : uint8_t { kNone, kShrine, kChest, kWaypoint, kPortal, kDoor, kLever, kBarrel, kWarp };

// What an item on the ground is, for the order it is picked up in.
enum class AimLoot : uint8_t { kOther, kGold, kRune, kUniqueOrSet };

// A ZR + D-pad command: which items to pick up while it is held.
enum class AimLootCommand : uint8_t { kNone, kValuables, kGold, kEverything };

struct AimProfile {
    AimMode mode = AimMode::kEnemies;
    int cone_degrees = 45;     // each side of the stick's direction
    int range_subtiles = 20;   // how far from the hero a new target may be
};

// Every number the player can change in settings.txt (aim_settings.h names
// the keys). Times in milliseconds, distances in subtiles (a subtile is the
// game's small floor square, about a third of a tile), heights in pixels.
struct AimSettings {
    bool enabled = true;                                 // aim=
    AimProfile a{AimMode::kEnemies, 45, 20};             // A, the left mouse button
    AimProfile b{AimMode::kEnemies, 45, 20};             // B, the right one
    int near_subtiles = 4;      // an enemy this close always comes first
    int near_cone_degrees = 90; // ...when it is within this angle of the stick (90: anywhere in front)
    // Flicks of the circle pad (choose_flick, AimFlicks): a flick is a quick
    // move out of the centre; a slow tilt is not one.
    int flick_cone_degrees = 30;    // each side of a flick's direction
    int flick_ms = 150;             // from the centre to a flick's tilt within this time
    int flick_light_percent = 40;   // a flick reaches at least this much of the full tilt
    int flick_strong_percent = 80;  // ...and a strong one this much
    bool objects = true;        // plain A uses shrines, chests, doors... with no enemy in the cone
    bool loot = true;           // plain A picks up the nearest item with nothing else in the cone
    bool gold_first = false;    // gold before other items (plain A and ZR + Down)
    int lost_ms = 1500;         // the hero hasn't attacked or got closer for this long: give up the target
    int forget_ms = 2500;       // no button held for this long: the target is forgotten
    int height_px = 30;         // the cursor goes this far above the target's feet
    int confirm_polls = 4;      // frames to wait for the game to show the target under the cursor
    // The circle pad let go after a walk: a click just ahead of the hero so
    // he stops where he stands rather than walking on to the walk's last point.
    // Off: no click; the hero finishes that last step (up to a few subtiles).
    bool walk_stop = true;
};

// One unit as the game's memory has it: a monster or NPC (type 1), an
// object (type 2), an item on the ground (type 4) or a level's warp (type 5).
struct AimUnit {
    uint32_t type = 1;
    uint32_t id = 0, cls = 0, mode = 0;
    uint32_t flags_a = 0;        // unit +0xC4
    uint32_t flags_b = 0;        // unit +0xC8
    uint32_t monstats_flags = 0; // MonStats.txt's flags (npc, interact...)
    uint32_t monstats2_flags = 0;// MonStats2.txt's flags (noSel, alSel, corpseSel...)
    bool owned = false;          // a pet: the mercenary, a summon (owner list or owner bit)
    uint8_t operate_fn = 0;      // an object: objects.txt's OperateFn (what a click on it does)
    uint32_t quality = 0;        // an item: 5 set, 7 unique (the item's data +0x00)
    uint32_t code = 0;           // an item: items.txt's code, four characters ("gld ")
    int feet_x = 0, feet_y = 0;  // on the screen, game pixels
    float distance = 0;          // from the hero, subtiles
};

// The hero, the screen and the unit under the cursor, this frame.
struct AimView {
    int hero_x = 0, hero_y = 0;  // the hero's feet on the screen
    int width = 640, height = 480;
    bool in_town = false;
    uint32_t hero_mode = 0;      // the player's unit mode (7 to 16: attacking or casting)
    uint32_t right_skill = 0;    // the skill on the right mouse button (skills.txt's id)
    bool right_skill_on_corpses = false;   // ...is cast on a corpse (skills.txt's TargetCorpse)
    bool hover_on = false;       // the game shows a unit under the cursor
    uint32_t hover_type = 0, hover_id = 0;
    // The part of the screen a target may be in, across (AimPoll's, copied
    // in by AutoTarget::update); 0 and 0: all of it.
    int area_left = 0, area_right = 0;
};

// What reading the units costs: full reads, units read, their total and
// longest time in microseconds.
struct AimReadCost {
    uint32_t scans = 0, units = 0;
    uint64_t total_us = 0, longest_us = 0;
};

// Where the auto-target reads the game from.
class AimWorld {
public:
    virtual AimReadCost read_cost() const { return {}; }
    virtual ~AimWorld() = default;
    // False when there is no game to read (menus, another Game.exe).
    virtual bool view(AimView &out) = 0;
    // Every monster and NPC the game has loaded near the hero.
    virtual void units(std::vector<AimUnit> &out) = 0;
    // One monster or NPC by its id; false if it is gone.
    virtual bool unit(uint32_t id, AimUnit &out) = 0;
    // The objects (type 2), the items on the ground (type 4) and the
    // level's warps (type 5) near the hero, and one of them by its type and
    // id. None by default.
    virtual void ground_units(std::vector<AimUnit> &out) { (void)out; }
    virtual bool ground_unit(uint32_t type, uint32_t id, AimUnit &out)
    {
        (void)type, (void)id, (void)out;
        return false;
    }
    // Where the game takes clicks on a level warp of class `cls` (its
    // LvlWarp.txt row's SelectX, SelectY, SelectDX, SelectDY): a rectangle
    // in screen pixels from the warp's feet point. False if not known.
    virtual bool warp_area(uint32_t cls, int &left, int &top, int &width, int &height)
    {
        (void)cls, (void)left, (void)top, (void)width, (void)height;
        return false;
    }
};

// What a unit counts as. Pure: from the fields alone.
AimKind classify_unit(const AimUnit &unit, bool in_town);

// The kind of object an object or warp unit is (kNone: not one A uses),
// and its place in plain A's order: 0 shrine, 1 chest, 2 waypoint or
// portal, 3 door, lever or warp, 4 barrel.
AimObject object_kind(const AimUnit &unit);
int object_rank(AimObject kind);

// What an item is worth, and its place in the order items are picked up
// in: unique or set, then runes, then gold, then the rest; gold first when
// gold_first is on.
AimLoot loot_class(const AimUnit &item);
int loot_rank(AimLoot loot, bool gold_first);
// Whether a ZR + D-pad command picks up this item.
bool loot_command_takes(AimLootCommand command, AimLoot loot);

// A unit's key in the lists of ids: its type and id (an item and a monster
// may have the same id).
inline uint64_t aim_key(uint32_t type, uint32_t id) { return (uint64_t)type << 32 | id; }

// The bits classify_unit reads, named (Diablo II 1.14d).
namespace aim_bits {
constexpr uint32_t kSelectable = 1u << 1;        // flags_a: a live unit the game lets you pick
constexpr uint32_t kDead = 1u << 16;             // flags_a: a corpse
constexpr uint32_t kNeverPick = 1u << 21;        // flags_a: never picked (d2_native.cpp's hit test)
constexpr uint32_t kPickable = 1u << 7;          // flags_b: the hit test needs it
constexpr uint32_t kHasOwner = 1u << 10;         // flags_b: owned by a player (a summon)
constexpr uint32_t kMonNpc = 1u << 8;            // monstats_flags: npc
constexpr uint32_t kMonInteract = 1u << 9;       // monstats_flags: interact (talks)
constexpr uint32_t kMon2AllySelect = 1u << 4;    // monstats2_flags: alSel (Cain, guards: picked, never hit)
constexpr uint32_t kMon2NoSelect = 1u << 5;      // monstats2_flags: noSel (critters)
constexpr uint32_t kMon2CorpseSelect = 1u << 7;  // monstats2_flags: corpseSel (its corpse can be used)
constexpr uint32_t kModeDeath = 0, kModeDead = 12;
constexpr uint32_t kItemOnTheGround = 3;          // an item's mode on the ground
constexpr uint32_t kObjectClosed = 0;             // an object's mode before it is used (opened, operated)
constexpr uint32_t kQualitySet = 5, kQualityUnique = 7;
constexpr uint32_t kGoldCode = 0x20646c67;        // "gld ", little-endian
}  // namespace aim_bits

// The angle in degrees (0 to 180) between a direction and the way from the
// hero to a point, both on the screen (y down). A point on the hero: 0.
float aim_angle_degrees(float dir_x, float dir_y, int from_x, int from_y, int to_x, int to_y);

// The best unit for a profile, or -1. units and kinds side by side; skip:
// keys (aim_key) not to choose (tried and failed a moment ago). button_a:
// the profile is plain A's, which also takes objects and items. near_rule
// is set when the "an enemy next to the hero comes first" rule chose it.
int choose_target(const std::vector<AimUnit> &units, const std::vector<AimKind> &kinds, const AimView &view,
                  const AimProfile &profile, const AimSettings &settings, float dir_x, float dir_y,
                  const std::vector<uint64_t> &skip, bool *near_rule = nullptr, bool button_a = true);

// The item a ZR + D-pad command picks up next, or -1: within range_subtiles
// all around the hero, the best in loot_rank's order, the nearest of those.
int choose_loot(const std::vector<AimUnit> &units, const std::vector<AimKind> &kinds, const AimView &view,
                AimLootCommand command, const AimSettings &settings, int range_subtiles,
                const std::vector<uint64_t> &skip);

// A flick of the circle pad while fighting `current` (an enemy): the enemy
// it switches to, or -1 (nobody there: the target stays). Directions on the
// screen, y down. The rules (plan section 10 B):
//   - a flick the way the hero is fighting (within flick_cone_degrees of the
//     line from the hero to the target), light or strong: the next enemy
//     further away along that line; from the furthest, round again to the
//     nearest;
//   - a light flick another way: the enemy nearest the current target in
//     that direction from it (within flick_cone_degrees), to step sideways;
//   - a strong flick another way: the hero turns: the enemy nearest the hero
//     in the profile's cone round that direction.
// Only enemies within the profile's range and on the screen.
int choose_flick(const std::vector<AimUnit> &units, const std::vector<AimKind> &kinds, const AimView &view,
                 const AimProfile &profile, const AimSettings &settings, const AimUnit &current, float flick_x,
                 float flick_y, bool strong, const std::vector<uint64_t> &skip);

// Tells flicks of the circle pad from slow tilts, one poll at a time. A
// flick: the stick leaves the centre and reaches flick_light_percent of its
// full tilt within flick_ms; its direction and strength are read at the
// furthest point it gets to. One flick each time the stick leaves the
// centre: it has to come back before the next one counts.
class AimFlicks {
public:
    struct Flick {
        bool happened = false;
        bool strong = false;
        float dir_x = 0, dir_y = 0;   // unit length, screen (y down)
    };
    // x, y: the tilt as a fraction of the full one (-1 to 1, screen y down).
    Flick update(float x, float y, uint64_t now_ms, const AimSettings &settings);

    // The stick counts as centred below this much of the full tilt (the
    // walk's own threshold, controls.cpp).
    static constexpr float kCentre = 0.20f;
    // A flick is read once it stops getting further, or this long after it
    // passed the light threshold.
    static constexpr int kSettleMs = 60;

private:
    bool armed_ = true;       // the stick was centred since the last flick
    bool rising_ = false;     // past the light threshold in time: a flick being read
    uint64_t centre_ms_ = 0;  // the last poll the stick was centred
    uint64_t rise_ms_ = 0;
    float peak_ = 0, peak_x_ = 0, peak_y_ = 0;
};

// One poll's buttons and stick, as the controls see them.
struct AimPoll {
    uint64_t now_ms = 0;
    bool allowed = false;        // the game world, no panel, nothing on the cursor, no L, no script tap
    bool a_held = false, b_held = false;      // plain A and B (no R or ZR layer)
    bool a_pressed = false, b_pressed = false;
    bool stick_engaged = false;  // the circle pad is tilted (walking)
    float dir_x = 0, dir_y = 1;  // the aim direction on the screen (y down), unit length
    float stick_x = 0, stick_y = 0;   // the circle pad's tilt, -1 to 1 (screen y down): flicks
    AimLootCommand loot = AimLootCommand::kNone;   // a ZR + D-pad command held
    bool loot_pressed = false;   // ...pressed this poll
    // With a panel open on one side, the free half of the screen, across
    // (game pixels): targets only there, never under the panel. 0 and 0:
    // the whole screen.
    int area_left = 0, area_right = 0;
    // A new game frame since the last poll: the game has looked at what is
    // under the cursor again. The 3DS app replays several pad readings in one
    // message pump; only the first of each pump is a new frame.
    bool new_frame = true;
};

// What the controls do with the cursor and buttons this poll.
struct AimResult {
    bool aiming = false;         // the cursor is ours: put it at cursor_x, cursor_y
    int cursor_x = 0, cursor_y = 0;
    bool left = false, right = false;   // the buttons while aiming
    // A or B is held but must not click where the cursor is (the target
    // just died: no walking onto its corpse). The circle pad may still walk.
    bool hold_plain_buttons = false;
};

// Counters for game.log and the lane's report.
struct AimStats {
    uint32_t presses = 0;          // A or B pressed with the auto-target on
    uint32_t targets = 0;          // targets chosen
    uint32_t near_rule = 0;        // ...of them by the near rule
    uint32_t confirmed = 0;        // the game showed the target under the cursor
    uint32_t adopted = 0;          // the game showed another enemy there: taken instead
    uint32_t fallbacks = 0;        // never shown: a plain click at the target
    uint32_t no_target = 0;        // pressed with nothing in the cone
    uint32_t held_polls = 0;       // polls with the button held on a target
    uint32_t held_on_target = 0;   // ...with the game showing the target under the cursor
    uint32_t dropped_dead = 0, dropped_gone = 0, dropped_lost = 0, dropped_screen = 0, forgotten = 0;
    uint32_t flicks = 0;           // flicks of the circle pad while fighting
    uint32_t flick_switches = 0;   // ...that switched to another enemy
    uint32_t objects = 0, items = 0;   // objects and items chosen
    uint32_t no_room = 0;          // items the hero stood by and couldn't pick up: skipped
    uint32_t scans = 0;            // full reads of the units
};

class AutoTarget {
public:
    void set_settings(const AimSettings &settings) { settings_ = settings; }
    const AimSettings &settings() const { return settings_; }
    // One poll. Reads the world only while A or B is pressed or held.
    AimResult update(const AimPoll &poll, AimWorld &world);
    // Lines for game.log (target choices and drops). Called with each line;
    // at most a few a second (the rest are counted and reported).
    void set_log(void (*log)(const char *line)) { log_ = log; }
    const AimStats &stats() const { return stats_; }
    bool has_target() const { return target_id_ != 0; }
    uint32_t target_id() const { return target_id_; }
    uint32_t target_type() const { return target_type_; }

private:
    enum class Phase : uint8_t { kIdle, kConfirming, kConfirmed, kFallback };
    void drop(const char *why, uint32_t &counter, uint64_t now_ms);
    bool pick(const AimPoll &poll, const AimView &view, AimWorld &world, const AimProfile &profile, bool pressed);
    void read_units(const AimPoll &poll, const AimView &view, AimWorld &world, bool ground);
    void take(const AimUnit &u, AimKind kind, const AimPoll &poll, const char *why);
    bool look_up(AimWorld &world, uint32_t type, uint32_t id, AimUnit &out);
    bool fits(AimKind kind, const AimUnit &unit, const AimProfile &profile, bool in_town) const;
    void start_confirming(uint64_t now_ms);
    void skip(uint64_t key, uint64_t until_ms);
    void say(uint64_t now_ms, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
    static bool attacking_mode(uint32_t hero_mode);
    static bool casting_mode(uint32_t hero_mode);   // a cast or a skill's animation

    AimSettings settings_;
    AimStats stats_;
    void (*log_)(const char *) = nullptr;
    // The target, kept between presses.
    uint32_t target_id_ = 0;
    uint32_t target_type_ = 1;     // 1 a monster or NPC, 2 an object, 4 an item, 5 a warp
    AimKind target_kind_ = AimKind::kNone;
    uint64_t last_attack_ms_ = 0;                 // a button was held on it
    // Waiting for the game to show it under the cursor.
    Phase phase_ = Phase::kIdle;
    int confirm_frames_ = 0;       // frames looked at since the cursor went there
    bool cursor_moving_ = false;   // the cursor goes to the target this poll: nothing to look at yet
    int height_index_ = 0;         // which height is being tried
    int missed_frames_ = 0;        // confirmed, but not under the cursor for this many frames
    // An NPC's, an object's or an item's click: when it went down (it stays
    // down kNpcClickMs, and comes again after kNpcClickAgainMs if nothing
    // happened).
    // The hero's figure on the screen round its feet (game pixels), and how
    // far above a unit's feet the cursor goes when that figure is in the way.
    static constexpr int kHeroHalfWidthPx = 20, kHeroHeightPx = 80, kHeroFeetPx = 8, kFeetLiftPx = 6;
    static constexpr int kNpcClickMs = 120, kNpcClickAgainMs = 1000;
    uint64_t npc_click_ms_ = 0;
    bool npc_clicked_ = false;
    // Getting to it.
    float best_distance_ = 0;
    uint64_t progress_ms_ = 0;
    uint64_t reached_ms_ = 0;      // an item: when the hero got next to it (0: not yet)
    // Buttons.
    int active_profile_ = 0;       // 0 A, 1 B: the one pressed last; 2 a ZR + D-pad loot command
    AimLootCommand loot_ = AimLootCommand::kNone;   // the loot command held
    AimFlicks flicks_;
    bool holding_ = false;         // a button was held at the last poll
    bool paused_ = false;          // the target was lost under a held button: no plain click until a new press
    bool pressed_now_ = false;     // A or B went down this poll
    uint64_t last_scan_ms_ = 0;
    bool scan_now_ = false;        // the target was just dropped: look for the next one now
    // Units not to choose for a while: tried a moment ago and never shown
    // under the cursor, out of reach, or an item that didn't fit.
    struct Skip { uint64_t key; uint64_t until_ms; };
    std::vector<Skip> skips_;
    // An item stood by but not picked up for this long: no room for it.
    static constexpr float kItemReachSubtiles = 2.5f;
    static constexpr int kNoRoomSkipMs = 30000;
    // A corpse a skill was cast on: used up, left alone this long.
    static constexpr int kUsedCorpseSkipMs = 600000;
    uint32_t corpse_ready_id_ = 0;   // the corpse target the hero was seen not casting at
    // Scratch for scans (kept to avoid allocating each time).
    std::vector<AimUnit> units_;
    std::vector<AimKind> kinds_;
    std::vector<uint64_t> skip_keys_;
    // The log's rate limit: a few lines a second.
    uint64_t log_window_ms_ = 0;
    int log_lines_ = 0, log_skipped_ = 0;
};

}  // namespace game

#endif
