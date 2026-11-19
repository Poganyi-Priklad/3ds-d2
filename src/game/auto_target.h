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
//   - In town only NPCs are targets (to talk to). Outside town an NPC is a
//     target only when there is no enemy in the cone.
//   - The mercenary, summons and other players are never targets.
//   - The target is kept while A and B are swapped and across release and
//     press again. It is dropped when the stick turns away, when it dies,
//     leaves the screen, or the hero can't get to it for a while, and
//     forgotten a few seconds after the last attack.
//   - Nothing in the cone: the button clicks where the cursor is, as before.
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
enum class AimKind : uint8_t { kNone, kEnemy, kNpc, kCorpse };

struct AimProfile {
    AimMode mode = AimMode::kEnemies;
    int cone_degrees = 45;     // each side of the stick's direction
    int range_subtiles = 10;   // how far from the hero a new target may be
};

// Every number the player can change in settings.txt (aim_settings.h names
// the keys). Times in milliseconds, distances in subtiles (a subtile is the
// game's small floor square, about a third of a tile), heights in pixels.
struct AimSettings {
    bool enabled = true;                                 // aim=
    AimProfile a{AimMode::kEnemies, 45, 10};             // A, the left mouse button
    AimProfile b{AimMode::kEnemies, 45, 20};             // B, the right one: longer, for ranged skills
    int near_subtiles = 4;      // an enemy this close always comes first
    int near_cone_degrees = 90; // ...when it is within this angle of the stick (90: anywhere in front)
    int turn_degrees = 60;      // the stick turned this far away from the target: a new target
    int lost_ms = 1500;         // the hero hasn't attacked or got closer for this long: give up the target
    int forget_ms = 2500;       // no button held for this long: the target is forgotten
    int height_px = 30;         // the cursor goes this far above the target's feet
    int confirm_polls = 4;      // frames to wait for the game to show the target under the cursor
};

// One monster or NPC (a type 1 unit) as the game's memory has it.
struct AimUnit {
    uint32_t id = 0, cls = 0, mode = 0;
    uint32_t flags_a = 0;        // unit +0xC4
    uint32_t flags_b = 0;        // unit +0xC8
    uint32_t monstats_flags = 0; // MonStats.txt's flags (npc, interact...)
    uint32_t monstats2_flags = 0;// MonStats2.txt's flags (noSel, alSel, corpseSel...)
    bool owned = false;          // a pet: the mercenary, a summon (owner list or owner bit)
    int feet_x = 0, feet_y = 0;  // on the screen, game pixels
    float distance = 0;          // from the hero, subtiles
};

// The hero, the screen and the unit under the cursor, this frame.
struct AimView {
    int hero_x = 0, hero_y = 0;  // the hero's feet on the screen
    int width = 640, height = 480;
    bool in_town = false;
    uint32_t hero_mode = 0;      // the player's unit mode (7 to 16: attacking or casting)
    bool hover_on = false;       // the game shows a unit under the cursor
    uint32_t hover_type = 0, hover_id = 0;
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
    // One unit by its id; false if it is gone.
    virtual bool unit(uint32_t id, AimUnit &out) = 0;
};

// What a unit counts as. Pure: from the fields alone.
AimKind classify_unit(const AimUnit &unit, bool in_town);

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
}  // namespace aim_bits

// The angle in degrees (0 to 180) between a direction and the way from the
// hero to a point, both on the screen (y down). A point on the hero: 0.
float aim_angle_degrees(float dir_x, float dir_y, int from_x, int from_y, int to_x, int to_y);

// The best unit for a profile, or -1. units and kinds side by side; skip:
// ids not to choose (tried and failed a moment ago). near_rule is set when
// the "an enemy next to the hero comes first" rule chose it.
int choose_target(const std::vector<AimUnit> &units, const std::vector<AimKind> &kinds, const AimView &view,
                  const AimProfile &profile, const AimSettings &settings, float dir_x, float dir_y,
                  const std::vector<uint32_t> &skip, bool *near_rule = nullptr);

// One poll's buttons and stick, as the controls see them.
struct AimPoll {
    uint64_t now_ms = 0;
    bool allowed = false;        // the game world, no panel, nothing on the cursor, no L, no script tap
    bool a_held = false, b_held = false;      // plain A and B (no R or ZR layer)
    bool a_pressed = false, b_pressed = false;
    bool stick_engaged = false;  // the circle pad is tilted (walking)
    float dir_x = 0, dir_y = 1;  // the aim direction on the screen (y down), unit length
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
    uint32_t dropped_dead = 0, dropped_gone = 0, dropped_turn = 0, dropped_lost = 0, dropped_screen = 0,
             forgotten = 0;
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

private:
    enum class Phase : uint8_t { kIdle, kConfirming, kConfirmed, kFallback };
    void drop(const char *why, uint32_t &counter, uint64_t now_ms);
    bool pick(const AimPoll &poll, const AimView &view, AimWorld &world, const AimProfile &profile, bool pressed);
    void start_confirming(uint64_t now_ms);
    bool skipped(uint32_t id, uint64_t now_ms);
    void skip(uint32_t id, uint64_t until_ms);
    void say(uint64_t now_ms, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
    static bool attacking_mode(uint32_t hero_mode);

    AimSettings settings_;
    AimStats stats_;
    void (*log_)(const char *) = nullptr;
    // The target, kept between presses.
    uint32_t target_id_ = 0;
    AimKind target_kind_ = AimKind::kNone;
    float chosen_dir_x_ = 0, chosen_dir_y_ = 1;   // the aim direction when it was chosen
    uint64_t last_attack_ms_ = 0;                 // a button was held on it
    // Waiting for the game to show it under the cursor.
    Phase phase_ = Phase::kIdle;
    int confirm_frames_ = 0;       // frames looked at since the cursor went there
    bool cursor_moving_ = false;   // the cursor goes to the target this poll: nothing to look at yet
    int height_index_ = 0;         // which height is being tried
    int missed_frames_ = 0;        // confirmed, but not under the cursor for this many frames
    // An NPC's click: when it went down (it stays down kNpcClickMs, and
    // comes again after kNpcClickAgainMs if no menu opened).
    // The hero's figure on the screen round its feet (game pixels), and how
    // far above a unit's feet the cursor goes when that figure is in the way.
    static constexpr int kHeroHalfWidthPx = 20, kHeroHeightPx = 80, kHeroFeetPx = 8, kFeetLiftPx = 6;
    static constexpr int kNpcClickMs = 120, kNpcClickAgainMs = 1000;
    uint64_t npc_click_ms_ = 0;
    bool npc_clicked_ = false;
    // Getting to it.
    float best_distance_ = 0;
    uint64_t progress_ms_ = 0;
    // Buttons.
    int active_profile_ = 0;       // 0 A, 1 B: the one pressed last
    bool holding_ = false;         // a button was held at the last poll
    bool paused_ = false;          // the target was lost under a held button: no plain click until a new press
    bool pressed_now_ = false;     // A or B went down this poll
    uint64_t last_scan_ms_ = 0;
    bool scan_now_ = false;        // the target was just dropped: look for the next one now
    // Units tried a moment ago that the game never showed under the cursor.
    struct Skip { uint32_t id; uint64_t until_ms; };
    std::vector<Skip> skips_;
    // Scratch for scans (kept to avoid allocating each time).
    std::vector<AimUnit> units_;
    std::vector<AimKind> kinds_;
    std::vector<uint32_t> skip_ids_;
    // The log's rate limit: a few lines a second.
    uint64_t log_window_ms_ = 0;
    int log_lines_ = 0, log_skipped_ = 0;
};

}  // namespace game

#endif
