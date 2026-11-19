// See auto_target.h.
#include "game/auto_target.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace game {

using namespace aim_bits;

AimKind classify_unit(const AimUnit &unit, bool in_town)
{
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
    const bool over_a_globe = x < view.width * 0.19f || x > view.width * 0.81f;
    return y <= (over_a_globe ? view.height * 0.78f : view.height * 0.86f);
}

// Whether a kind of unit is something a profile aims at.
bool kind_fits(AimKind kind, AimMode mode, bool in_town)
{
    if (mode == AimMode::kOff || kind == AimKind::kNone)
        return false;
    if (in_town)
        return kind == AimKind::kNpc;
    if (mode == AimMode::kCorpses)
        return kind == AimKind::kCorpse;
    return kind == AimKind::kEnemy || kind == AimKind::kNpc;
}

bool contains(const std::vector<uint32_t> &ids, uint32_t id)
{
    for (uint32_t x : ids)
        if (x == id)
            return true;
    return false;
}

}  // namespace

int choose_target(const std::vector<AimUnit> &units, const std::vector<AimKind> &kinds, const AimView &view,
                  const AimProfile &profile, const AimSettings &settings, float dir_x, float dir_y,
                  const std::vector<uint32_t> &skip, bool *near_rule)
{
    if (near_rule)
        *near_rule = false;
    if (profile.mode == AimMode::kOff)
        return -1;
    const float near_cone =
        (float)(settings.near_cone_degrees > profile.cone_degrees ? settings.near_cone_degrees : profile.cone_degrees);
    int nearest_near_enemy = -1, best_enemy = -1, nearest_npc = -1, nearest_corpse = -1;
    float best_enemy_score = 0;
    for (size_t i = 0; i < units.size() && i < kinds.size(); i++) {
        const AimUnit &u = units[i];
        const AimKind kind = kinds[i];
        if (!kind_fits(kind, profile.mode, view.in_town) || contains(skip, u.id))
            continue;
        if (!point_visible(view, u.feet_x, u.feet_y - settings.height_px))
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
        }
    }
    if (view.in_town)
        return nearest_npc;
    if (profile.mode == AimMode::kCorpses)
        return nearest_corpse;
    if (nearest_near_enemy >= 0) {
        if (near_rule)
            *near_rule = true;
        return nearest_near_enemy;
    }
    if (best_enemy >= 0)
        return best_enemy;
    return nearest_npc;   // outside town an NPC only when no enemy is in the cone
}

bool AutoTarget::attacking_mode(uint32_t hero_mode)
{
    // The player's unit modes 7 to 16 (attack 1 and 2, block, cast, throw,
    // kick, skills 1 to 4) and 18 (a sequence, such as Whirlwind).
    return (hero_mode >= 7 && hero_mode <= 16) || hero_mode == 18;
}

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

bool AutoTarget::skipped(uint32_t id, uint64_t now_ms)
{
    for (const Skip &s : skips_)
        if (s.id == id && now_ms < s.until_ms)
            return true;
    return false;
}

void AutoTarget::skip(uint32_t id, uint64_t until_ms)
{
    for (size_t i = 0; i < skips_.size();)
        if (skips_[i].id == id || skips_[i].until_ms + 60000 < until_ms)
            skips_.erase(skips_.begin() + (long)i);
        else
            i++;
    if (skips_.size() < 16)
        skips_.push_back({id, until_ms});
}

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

void AutoTarget::start_confirming(uint64_t now_ms)
{
    phase_ = Phase::kConfirming;
    confirm_frames_ = 0;
    cursor_moving_ = true;   // the cursor goes there this poll; the game looks from the next frame on
    missed_frames_ = 0;
    progress_ms_ = now_ms;
}

// A full read of the units, and the profile's choice in it (or -1).
bool AutoTarget::pick(const AimPoll &poll, const AimView &view, AimWorld &world, const AimProfile &profile,
                      bool pressed)
{
    units_.clear();
    world.units(units_);
    stats_.scans++;
    last_scan_ms_ = poll.now_ms;
    kinds_.resize(units_.size());
    for (size_t i = 0; i < units_.size(); i++)
        kinds_[i] = classify_unit(units_[i], view.in_town);
    skip_ids_.clear();
    for (const Skip &s : skips_)
        if (poll.now_ms < s.until_ms)
            skip_ids_.push_back(s.id);
    bool near_rule = false;
    const int i = choose_target(units_, kinds_, view, profile, settings_, poll.dir_x, poll.dir_y, skip_ids_, &near_rule);
    if (i < 0)
        return false;
    const AimUnit &u = units_[(size_t)i];
    const AimKind kind = kinds_[(size_t)i];
    if (target_id_) {
        // A target is kept unless an enemy beats it: one next to the hero
        // (the safety rule) or any enemy over an NPC.
        if (u.id == target_id_)
            return true;
        const bool enemy_over_npc = kind == AimKind::kEnemy && target_kind_ == AimKind::kNpc;
        AimUnit current;
        const bool current_near =
            world.unit(target_id_, current) && current.distance <= (float)settings_.near_subtiles;
        if (!enemy_over_npc && !(near_rule && !current_near))
            return true;
        say(poll.now_ms, "target %u replaced by %u (%s)", target_id_, u.id,
            enemy_over_npc ? "an enemy before an NPC" : "an enemy next to the hero");
    }
    target_id_ = u.id;
    target_kind_ = kind;
    chosen_dir_x_ = poll.dir_x;
    chosen_dir_y_ = poll.dir_y;
    height_index_ = 0;
    npc_clicked_ = false;
    best_distance_ = u.distance;
    stats_.targets++;
    if (near_rule)
        stats_.near_rule++;
    say(poll.now_ms, "%s %u (class %u) %.1f subtiles away%s, %s", kind == AimKind::kNpc ? "NPC" : kind == AimKind::kCorpse ? "corpse" : "enemy",
        u.id, u.cls, (double)u.distance, near_rule ? ", next to the hero" : "",
        pressed ? (active_profile_ ? "B pressed" : "A pressed") : "while held");
    start_confirming(poll.now_ms);
    return true;
}

AimResult AutoTarget::update(const AimPoll &poll, AimWorld &world)
{
    AimResult result;
    const uint64_t now = poll.now_ms;
    const bool any_held = poll.a_held || poll.b_held;

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

    // Which profile: the button pressed last (or the only one held).
    if (poll.a_pressed)
        active_profile_ = 0;
    else if (poll.b_pressed)
        active_profile_ = 1;
    if (poll.a_held && !poll.b_held)
        active_profile_ = 0;
    else if (poll.b_held && !poll.a_held)
        active_profile_ = 1;
    const bool pressed = poll.a_pressed || poll.b_pressed;
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

    const AimProfile &profile = active_profile_ ? settings_.b : settings_.a;
    if (profile.mode == AimMode::kOff)
        return result;   // this button clicks as it always did
    AimView view;
    if (!world.view(view))
        return result;

    // The kept target: still there, still what it was, still on the screen,
    // and the stick not turned away from it.
    AimUnit current;
    if (target_id_) {
        if (!world.unit(target_id_, current)) {
            drop("gone", stats_.dropped_gone, now);
        } else {
            const AimKind kind = classify_unit(current, view.in_town);
            if (kind != target_kind_) {
                drop(current.mode == kModeDeath || current.mode == kModeDead ? "dead" : "no longer a target",
                     stats_.dropped_dead, now);
            } else if (!kind_fits(kind, profile.mode, view.in_town)) {
                target_id_ = 0;   // not what this button aims at (A on enemies, B on corpses): a new choice
                phase_ = Phase::kIdle;
            } else if (!point_visible(view, current.feet_x, current.feet_y - settings_.height_px)) {
                drop("off the screen", stats_.dropped_screen, now);
            } else if (poll.stick_engaged &&
                       aim_angle_degrees(poll.dir_x, poll.dir_y, 0, 0, (int)(chosen_dir_x_ * 1000),
                                         (int)(chosen_dir_y_ * 1000)) > (float)settings_.turn_degrees &&
                       aim_angle_degrees(poll.dir_x, poll.dir_y, view.hero_x, view.hero_y, current.feet_x,
                                         current.feet_y) > (float)settings_.turn_degrees) {
                drop("the stick turned away", stats_.dropped_turn, now);
            }
        }
    }

    // A new target: on a press, or now and then while held (a new enemy in
    // the cone, an enemy coming next to the hero).
    const uint64_t rescan_ms = target_id_ ? 250 : 150;
    const bool may_be_beaten = target_id_ && !view.in_town && profile.mode == AimMode::kEnemies &&
                               (target_kind_ == AimKind::kNpc || current.distance > (float)settings_.near_subtiles);
    if ((!target_id_ || may_be_beaten) && (pressed || scan_now_ || now - last_scan_ms_ >= rescan_ms)) {
        scan_now_ = false;
        if (pick(poll, view, world, profile, pressed) && target_id_ != current.id)
            world.unit(target_id_, current);
    }
    if (!target_id_) {
        if (pressed) {
            stats_.no_target++;
            say(now, "%s: nothing in the cone, a plain click", active_profile_ ? "B" : "A");
        }
        result.hold_plain_buttons = paused_;
        return result;
    }
    if (phase_ == Phase::kIdle)
        start_confirming(now);   // a remembered target: the cursor back on it, then the press

    const int heights[4] = {settings_.height_px, settings_.height_px / 2, settings_.height_px * 3 / 2,
                            settings_.height_px * 2};
    const bool on_target = view.hover_on && view.hover_type == 1 && view.hover_id == target_id_;
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
            } else if (view.hover_on && view.hover_type == 1 && view.hover_id != target_id_ &&
                       target_kind_ == AimKind::kEnemy && world.unit(view.hover_id, other) &&
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
                const uint32_t failed = target_id_;
                skip(failed, now + 2000);
                target_id_ = 0;
                if (pick(poll, view, world, profile, false) && world.unit(target_id_, current)) {
                    say(now, "target %u never under the cursor, trying %u", failed, target_id_);
                } else {
                    target_id_ = failed;
                    world.unit(target_id_, current);
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
    } else if (phase_ == Phase::kConfirmed && target_kind_ == AimKind::kNpc) {
        // Once the game has shown the NPC there, the cursor stays at that
        // height and the clicks go on: the game shows nothing under the
        // cursor for a while when the hero gets to an NPC (seen on the lane).
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
    if (phase_ == Phase::kConfirmed || phase_ == Phase::kFallback) {
        if (attacking_mode(view.hero_mode) || current.distance < best_distance_ - 0.75f ||
            (target_kind_ == AimKind::kNpc && npc_clicked_ && now - npc_click_ms_ < (uint64_t)kNpcClickAgainMs)) {
            best_distance_ = current.distance;
            progress_ms_ = now;
        } else if (now - progress_ms_ > (uint64_t)settings_.lost_ms) {
            skip(target_id_, now + 3000);
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

    // An NPC is clicked, not held: the game opens its menu when the hero
    // gets there only once the button is up (the lane showed a held button
    // keeping the menu shut). One short click, and another every second
    // while no menu has opened (it walked off).
    if (target_kind_ == AimKind::kNpc && press) {
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
    result.left = press && poll.a_held;
    result.right = press && poll.b_held;
    return result;
}

}  // namespace game
