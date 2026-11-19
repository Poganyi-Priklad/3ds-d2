// The auto-target on A and B (src/game/auto_target.h): which unit the cone
// picks, the rules (an enemy next to the hero first, NPCs in town, the
// mercenary never), how the target sticks and when it is let go, the
// settings.txt keys (src/game/aim_settings.h), and the controls around it
// (src/game/controls.cpp: the stick aims instead of walking, the cursor
// goes back). Made-up units stand in for the game. Run through
// `scripts/docker.sh test aim`.
#include "game/aim_settings.h"
#include "game/auto_target.h"
#include "game/controls.h"
#include "game/settings_file.h"

#include <cmath>
#include <cstdio>
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

// ---- the runtime the controls call (the cursor and buttons recorded) ----
static int g_cursor_x = 0, g_cursor_y = 0;
static bool g_left = false, g_right = false;
namespace rt {
void input_button(bool right, bool down) { (right ? g_right : g_left) = down; }
void input_key(uint8_t, bool) {}
void input_mouse_move(int x, int y)
{
    g_cursor_x = x;
    g_cursor_y = y;
}
void logf(const char *, ...) {}
}  // namespace rt

// ---- a made-up game ----
// The hero's feet at 320,232 (640x480, as the lane showed). A unit placed
// by its offset in subtiles, projected as the game does: x - y across at 16
// pixels, x + y down at 8.
struct FakeWorld : AimWorld {
    AimView v;
    std::vector<AimUnit> list;
    int scans = 0;
    // Where the cursor was at the previous frame: what the game shows under
    // it now (each unit's body from 0 to 60 pixels above its feet, 24 wide).
    bool hover_follows_cursor = true;
    int last_cursor_x = -1000, last_cursor_y = -1000;
    FakeWorld()
    {
        v.hero_x = 320;
        v.hero_y = 232;
        v.width = 640;
        v.height = 480;
        v.hero_mode = 1;
    }
    bool view(AimView &out) override
    {
        out = v;
        if (hover_follows_cursor) {
            out.hover_on = false;
            for (const AimUnit &u : list)
                if (std::abs(last_cursor_x - u.feet_x) <= 12 && last_cursor_y <= u.feet_y &&
                    last_cursor_y >= u.feet_y - 60 && u.mode != 0) {
                    out.hover_on = true;
                    out.hover_type = 1;
                    out.hover_id = u.id;
                    break;
                }
        }
        return true;
    }
    void units(std::vector<AimUnit> &out) override
    {
        scans++;
        out.insert(out.end(), list.begin(), list.end());
    }
    bool unit(uint32_t id, AimUnit &out) override
    {
        for (const AimUnit &u : list)
            if (u.id == id) {
                out = u;
                return true;
            }
        return false;
    }
    AimUnit *find(uint32_t id)
    {
        for (AimUnit &u : list)
            if (u.id == id)
                return &u;
        return nullptr;
    }
    // A monster at dx, dy subtiles from the hero.
    AimUnit &add(uint32_t id, double dx, double dy, AimKind kind = AimKind::kEnemy)
    {
        AimUnit u;
        u.id = id;
        u.cls = 19;
        u.mode = 1;
        u.flags_a = 0x0000000F;
        u.flags_b = 0x80;
        u.monstats2_flags = aim_bits::kMon2CorpseSelect;
        if (kind == AimKind::kNpc) {
            u.cls = 155;
            u.monstats_flags = aim_bits::kMonNpc | aim_bits::kMonInteract;
            u.monstats2_flags = 0;
        } else if (kind == AimKind::kCorpse) {
            u.mode = 12;
            u.flags_a = 0x3001002D;
        }
        place(u, dx, dy);
        list.push_back(u);
        return list.back();
    }
    void place(AimUnit &u, double dx, double dy)
    {
        u.feet_x = v.hero_x + (int)std::lround((dx - dy) * 16);
        u.feet_y = v.hero_y + (int)std::lround((dx + dy) * 8);
        u.distance = (float)std::sqrt(dx * dx + dy * dy);
    }
};

// Screen directions (y down) as the stick gives them.
static const float kAimRight[2] = {1, 0}, kAimLeft[2] = {-1, 0}, kAimUp[2] = {0, -1}, kAimDown[2] = {0, 1};
// Subtile offsets that land straight right / left / up / down on the screen.
static void right_of_hero(double n, double &dx, double &dy) { dx = n / std::sqrt(2.0); dy = -dx; }

// One poll of the auto-target alone; the world's hover then follows its cursor.
static AimResult poll(AutoTarget &aim, FakeWorld &w, uint64_t t, bool a, bool b, bool a_pressed, bool b_pressed,
                      const float *dir, bool stick = true, bool new_frame = true)
{
    AimPoll p;
    p.now_ms = t;
    p.allowed = true;
    p.a_held = a;
    p.b_held = b;
    p.a_pressed = a_pressed;
    p.b_pressed = b_pressed;
    p.stick_engaged = stick;
    p.dir_x = dir[0];
    p.dir_y = dir[1];
    p.new_frame = new_frame;
    AimResult r = aim.update(p, w);
    if (r.aiming) {
        w.last_cursor_x = r.cursor_x;
        w.last_cursor_y = r.cursor_y;
    }
    return r;
}

// ---- classify_unit ----
static void test_classify()
{
    FakeWorld w;
    AimUnit enemy = w.add(1, 2, 0);
    CHECK(classify_unit(enemy, false) == AimKind::kEnemy);
    CHECK(classify_unit(enemy, true) == AimKind::kNone);   // in town nothing but NPCs
    AimUnit npc = w.add(2, 2, 0, AimKind::kNpc);
    CHECK(classify_unit(npc, true) == AimKind::kNpc);
    CHECK(classify_unit(npc, false) == AimKind::kNpc);
    AimUnit corpse = w.add(3, 2, 0, AimKind::kCorpse);
    CHECK(classify_unit(corpse, false) == AimKind::kCorpse);
    CHECK(classify_unit(corpse, true) == AimKind::kNone);
    AimUnit used = corpse;
    used.monstats2_flags = 0;   // a monster whose corpse no skill can use
    CHECK(classify_unit(used, false) == AimKind::kNone);
    AimUnit dying = enemy;
    dying.mode = 0;
    CHECK(classify_unit(dying, false) == AimKind::kNone);
    AimUnit pet = enemy;
    pet.owned = true;   // in the pet list: the mercenary, a summon
    CHECK(classify_unit(pet, false) == AimKind::kNone);
    AimUnit owned = enemy;
    owned.flags_b |= aim_bits::kHasOwner;
    CHECK(classify_unit(owned, false) == AimKind::kNone);
    AimUnit guard = enemy;
    guard.monstats2_flags |= aim_bits::kMon2AllySelect;   // a guard, a statue
    CHECK(classify_unit(guard, false) == AimKind::kNone);
    AimUnit critter = enemy;
    critter.monstats2_flags |= aim_bits::kMon2NoSelect;
    CHECK(classify_unit(critter, false) == AimKind::kNone);
    AimUnit unpickable = enemy;
    unpickable.flags_b = 0;   // just spawned: the game's hit test skips it
    CHECK(classify_unit(unpickable, false) == AimKind::kNone);
    AimUnit never = enemy;
    never.flags_a |= aim_bits::kNeverPick;
    CHECK(classify_unit(never, false) == AimKind::kNone);
    AimUnit unselectable = enemy;
    unselectable.flags_a &= ~aim_bits::kSelectable;
    CHECK(classify_unit(unselectable, false) == AimKind::kNone);
}

static int choose(FakeWorld &w, const AimProfile &p, const float *dir, bool *near_rule = nullptr,
                  const AimSettings &s = AimSettings(), std::vector<uint32_t> skip = {})
{
    std::vector<AimKind> kinds;
    for (const AimUnit &u : w.list)
        kinds.push_back(classify_unit(u, w.v.in_town));
    const int i = choose_target(w.list, kinds, w.v, p, s, dir[0], dir[1], skip, near_rule);
    return i < 0 ? 0 : (int)w.list[(size_t)i].id;
}

// ---- the cone ----
static void test_cone()
{
    FakeWorld w;
    double dx, dy;
    right_of_hero(6, dx, dy);
    w.add(1, dx, dy);         // right, 6 subtiles
    right_of_hero(-5, dx, dy);
    w.add(2, dx, dy);         // left, 5 (nearer)
    AimSettings s;
    s.near_subtiles = 0;      // no near rule here
    CHECK(choose(w, s.a, kAimRight, nullptr, s) == 1);
    CHECK(choose(w, s.a, kAimLeft, nullptr, s) == 2);
    CHECK(choose(w, s.a, kAimUp, nullptr, s) == 0);   // nothing up: a plain click
    // Range: 6 subtiles is beyond a range of 5.
    AimProfile shorter = s.a;
    shorter.range_subtiles = 5;
    CHECK(choose(w, shorter, kAimRight, nullptr, s) == 0);
    // The nearer of two in the cone; a skipped one is passed over.
    right_of_hero(3, dx, dy);
    w.add(3, dx, dy + 0.5);
    CHECK(choose(w, s.a, kAimRight, nullptr, s) == 3);
    CHECK(choose(w, s.a, kAimRight, nullptr, s, {3}) == 1);
    // Off the screen or behind the control panel: not chosen.
    FakeWorld low;
    low.add(4, 12, 12);   // 192 pixels straight down: y 424, the panel
    CHECK(choose(low, s.a, kAimDown, nullptr, s) == 0);
    // Angles.
    CHECK(std::fabs(aim_angle_degrees(1, 0, 0, 0, 10, 0)) < 0.01f);
    CHECK(std::fabs(aim_angle_degrees(1, 0, 0, 0, 0, 10) - 90) < 0.01f);
    CHECK(std::fabs(aim_angle_degrees(1, 0, 0, 0, -10, 0) - 180) < 0.01f);
    CHECK(aim_angle_degrees(1, 0, 5, 5, 5, 5) == 0);   // on the hero
}

// ---- the rules ----
static void test_near_rule()
{
    // B (long range): a far enemy straight ahead, a near one 70 degrees
    // off. The near one comes first.
    FakeWorld w;
    double dx, dy;
    right_of_hero(12, dx, dy);
    w.add(1, dx, dy);
    w.add(2, 1.5, 1.5);   // 2.1 subtiles, straight down the screen: 90 degrees from the stick
    AimSettings s;
    bool near_rule = false;
    CHECK(choose(w, s.b, kAimRight, &near_rule, s) == 2 && near_rule);
    // Behind the hero (beyond the near cone): not chosen, the far one is.
    w.list[1].feet_x = w.v.hero_x - 30;
    w.list[1].feet_y = w.v.hero_y;
    CHECK(choose(w, s.b, kAimRight, &near_rule, s) == 1 && !near_rule);
    // The radius is a setting.
    w.list[1].feet_x = w.v.hero_x + 10;
    w.list[1].feet_y = w.v.hero_y + 20;
    w.list[1].distance = 3.5f;
    s.near_subtiles = 3;
    CHECK(choose(w, s.b, kAimRight, &near_rule, s) == 1);
    s.near_subtiles = 4;
    CHECK(choose(w, s.b, kAimRight, &near_rule, s) == 2);
    // Corpse mode keeps its corpses (a Necromancer's B).
    w.add(3, 4, -3, AimKind::kCorpse);
    AimProfile corpses = s.b;
    corpses.mode = AimMode::kCorpses;
    CHECK(choose(w, corpses, kAimRight, nullptr, s) == 3);
    // Off: nothing.
    AimProfile off = s.a;
    off.mode = AimMode::kOff;
    CHECK(choose(w, off, kAimRight, nullptr, s) == 0);
}

static void test_npcs()
{
    double dx, dy;
    // In town: the NPC in the cone, never anything else.
    FakeWorld town;
    town.v.in_town = true;
    right_of_hero(5, dx, dy);
    town.add(1, dx, dy, AimKind::kNpc);
    right_of_hero(2, dx, dy);
    town.add(2, dx, dy);   // a monster-type unit in town (a hireling standing about)
    AimSettings s;
    CHECK(choose(town, s.a, kAimRight, nullptr, s) == 1);
    CHECK(choose(town, s.b, kAimRight, nullptr, s) == 1);
    CHECK(choose(town, s.a, kAimLeft, nullptr, s) == 0);
    // Outside town: an NPC only when no enemy is in the cone.
    FakeWorld field;
    right_of_hero(3, dx, dy);
    field.add(1, dx, dy, AimKind::kNpc);   // Cain, near
    right_of_hero(8, dx, dy);
    field.add(2, dx, dy);                  // an enemy, further
    s.near_subtiles = 0;
    CHECK(choose(field, s.a, kAimRight, nullptr, s) == 2);
    field.list[1].mode = 0;                // the enemy dies
    CHECK(choose(field, s.a, kAimRight, nullptr, s) == 1);
    // The mercenary is never a target.
    FakeWorld merc;
    merc.add(1, 2, -2).owned = true;
    CHECK(choose(merc, s.a, kAimRight, nullptr, s) == 0);
}

// ---- pressing, confirming, holding ----
static void test_press_waits_for_hover()
{
    FakeWorld w;
    double dx, dy;
    right_of_hero(5, dx, dy);
    w.add(7, dx, dy);
    AutoTarget aim;
    // Press A: the cursor goes onto the enemy (30 pixels above its feet), no click yet.
    AimResult r = poll(aim, w, 0, true, false, true, false, kAimRight);
    CHECK(r.aiming && !r.left && !r.right);
    CHECK(r.cursor_x == w.list[0].feet_x && r.cursor_y == w.list[0].feet_y - 30);
    // Another reading in the same pump: still no click (the game hasn't drawn a frame).
    r = poll(aim, w, 5, true, false, false, false, kAimRight, true, false);
    CHECK(r.aiming && !r.left);
    // The next frame: the game shows it under the cursor, the button goes down.
    r = poll(aim, w, 40, true, false, false, false, kAimRight);
    CHECK(r.aiming && r.left && !r.right);
    CHECK(aim.stats().confirmed == 1);
    // It walks off: the cursor follows it, outside the cone too.
    w.place(w.list[0], -4, 4);   // left of the hero now
    r = poll(aim, w, 80, true, false, false, false, kAimRight, false);
    CHECK(r.aiming && r.cursor_x == w.list[0].feet_x);
    CHECK(!r.left);   // the game showed the old spot: the button waits a frame
    r = poll(aim, w, 120, true, false, false, false, kAimRight, false);
    CHECK(r.left);
}

static void test_never_shown()
{
    // The game never shows it (an ally, a used corpse): after the frames
    // allowed, another unit; with none, a plain click at it.
    FakeWorld w;
    w.hover_follows_cursor = false;
    double dx, dy;
    right_of_hero(5, dx, dy);
    w.add(7, dx, dy);
    AutoTarget aim;
    AimResult r = poll(aim, w, 0, true, false, true, false, kAimRight);
    int cursor_heights = 0, last_y = r.cursor_y;
    for (int f = 1; f <= 3; f++) {
        r = poll(aim, w, (uint64_t)f * 40, true, false, false, false, kAimRight);
        CHECK(r.aiming && !r.left);
        cursor_heights += r.cursor_y != last_y;   // another height each frame
        last_y = r.cursor_y;
    }
    CHECK(cursor_heights == 3);
    r = poll(aim, w, 160, true, false, false, false, kAimRight);
    CHECK(r.aiming && r.left);   // the plain click at it
    CHECK(aim.stats().fallbacks == 1);

    // With a second enemy, that one is tried instead.
    FakeWorld two;
    two.hover_follows_cursor = false;
    right_of_hero(4, dx, dy);
    two.add(7, dx, dy);
    right_of_hero(6, dx, dy);
    two.add(8, dx, dy + 0.3);
    AutoTarget aim2;
    poll(aim2, two, 0, true, false, true, false, kAimRight);
    CHECK(aim2.target_id() == 7);
    for (int f = 1; f <= 4; f++)
        poll(aim2, two, (uint64_t)f * 40, true, false, false, false, kAimRight);
    CHECK(aim2.target_id() == 8);
}

static void test_adopt_hovered_enemy()
{
    // Another enemy stands in front of ours: the game shows it; it is taken.
    FakeWorld w;
    w.hover_follows_cursor = false;
    double dx, dy;
    right_of_hero(5, dx, dy);
    w.add(7, dx, dy);
    w.add(9, dx + 0.2, dy + 0.2);
    AutoTarget aim;
    poll(aim, w, 0, true, false, true, false, kAimRight);
    CHECK(aim.target_id() == 7);
    w.v.hover_on = true;
    w.v.hover_type = 1;
    w.v.hover_id = 9;
    AimResult r = poll(aim, w, 40, true, false, false, false, kAimRight);
    CHECK(aim.target_id() == 9 && r.left && aim.stats().adopted == 1);
}

static void test_sticky_and_forgotten()
{
    FakeWorld w;
    double dx, dy;
    right_of_hero(6, dx, dy);
    w.add(7, dx, dy);
    AutoTarget aim;
    poll(aim, w, 0, true, false, true, false, kAimRight);
    poll(aim, w, 40, true, false, false, false, kAimRight);
    CHECK(aim.target_id() == 7);
    // A nearer enemy comes into the cone (not next to the hero).
    right_of_hero(5, dx, dy);
    w.add(8, dx, dy + 0.4);
    // Let go of A, the stick centred, then B: still 7.
    AimResult r = poll(aim, w, 100, false, false, false, false, kAimRight, false);
    CHECK(!r.aiming);
    r = poll(aim, w, 600, false, true, false, true, kAimRight, false);
    CHECK(aim.target_id() == 7 && r.aiming && !r.right);
    r = poll(aim, w, 640, false, true, false, false, kAimRight, false);
    CHECK(r.right && !r.left);
    // A and B swapped while held: still 7.
    r = poll(aim, w, 680, true, true, true, false, kAimRight, false);
    CHECK(aim.target_id() == 7 && r.left && r.right);
    // Let go; after forget_ms without an attack, a new press chooses again.
    poll(aim, w, 700, false, false, false, false, kAimRight, false);
    poll(aim, w, 700 + 2400, false, false, false, false, kAimRight, false);
    CHECK(aim.target_id() == 7);   // 2.4 s: still remembered
    poll(aim, w, 700 + 2600, false, false, false, false, kAimRight, false);
    CHECK(aim.target_id() == 0 && aim.stats().forgotten == 1);
    poll(aim, w, 4000, true, false, true, false, kAimRight);
    CHECK(aim.target_id() == 8);
}

static void test_turn_and_death()
{
    FakeWorld w;
    double dx, dy;
    right_of_hero(6, dx, dy);
    w.add(7, dx, dy);
    right_of_hero(-6, dx, dy);
    w.add(8, dx, dy);
    AutoTarget aim;
    poll(aim, w, 0, true, false, true, false, kAimRight);
    poll(aim, w, 40, true, false, false, false, kAimRight);
    CHECK(aim.target_id() == 7);
    // A small turn (30 degrees) keeps it.
    const float slightly[2] = {0.866f, -0.5f};
    poll(aim, w, 80, true, false, false, false, slightly);
    CHECK(aim.target_id() == 7);
    // The stick turned the other way while held: the enemy there.
    poll(aim, w, 120, true, false, false, false, kAimLeft);
    CHECK(aim.target_id() == 8 && aim.stats().dropped_turn == 1);
    AimResult r = poll(aim, w, 160, true, false, false, false, kAimLeft);
    CHECK(r.left);
    // It dies under the held button: the button comes up, no click onto
    // its corpse; nothing else in the cone.
    w.find(8)->mode = 0;
    r = poll(aim, w, 200, true, false, false, false, kAimLeft, false);
    CHECK(!r.aiming && r.hold_plain_buttons && aim.target_id() == 0 && aim.stats().dropped_dead == 1);
    // A new enemy comes into the cone while held: taken within a rescan.
    right_of_hero(-4, dx, dy);
    w.add(9, dx, dy);
    r = poll(aim, w, 400, true, false, false, false, kAimLeft, false);
    CHECK(aim.target_id() == 9 && r.aiming);
    // A fresh press with nothing in the cone is a plain click.
    FakeWorld empty;
    AutoTarget aim2;
    r = poll(aim2, empty, 0, true, false, true, false, kAimRight);
    CHECK(!r.aiming && !r.hold_plain_buttons && aim2.stats().no_target == 1);
}

static void test_lost_and_screen()
{
    FakeWorld w;
    double dx, dy;
    right_of_hero(8, dx, dy);
    w.add(7, dx, dy);
    AutoTarget aim;
    poll(aim, w, 0, true, false, true, false, kAimRight);
    poll(aim, w, 40, true, false, false, false, kAimRight);
    // The hero walks (mode 2) but gets no closer: given up after lost_ms.
    w.v.hero_mode = 2;
    uint64_t t = 40;
    for (; t < 1500; t += 40) {
        poll(aim, w, t, true, false, false, false, kAimRight, false);
        CHECK(aim.target_id() == 7);
    }
    for (; t < 1700; t += 40)
        poll(aim, w, t, true, false, false, false, kAimRight, false);
    CHECK(aim.target_id() == 0 && aim.stats().dropped_lost == 1);
    // Attacking counts as getting there: never given up while hitting.
    FakeWorld fight;
    right_of_hero(2, dx, dy);
    fight.add(7, dx, dy);
    AutoTarget aim2;
    AimSettings s;
    s.near_subtiles = 0;
    aim2.set_settings(s);
    poll(aim2, fight, 0, true, false, true, false, kAimRight);
    fight.v.hero_mode = 7;
    for (t = 40; t < 5000; t += 40)
        poll(aim2, fight, t, true, false, false, false, kAimRight, false);
    CHECK(aim2.target_id() == 7);
    // Off the screen: dropped.
    fight.find(7)->feet_x = 700;
    poll(aim2, fight, t, true, false, false, false, kAimRight, false);
    CHECK(aim2.target_id() == 0 && aim2.stats().dropped_screen == 1);
}

static void test_near_beats_held_far_target()
{
    FakeWorld w;
    double dx, dy;
    right_of_hero(12, dx, dy);
    w.add(7, dx, dy);
    AutoTarget aim;
    poll(aim, w, 0, false, true, false, true, kAimRight);
    poll(aim, w, 40, false, true, false, false, kAimRight);
    CHECK(aim.target_id() == 7);
    w.add(8, 1, 1);   // an enemy walks up next to the hero
    poll(aim, w, 300, false, true, false, false, kAimRight, false);
    CHECK(aim.target_id() == 8 && aim.stats().near_rule == 1);
}

static void test_town_npc_released_after_talk()
{
    FakeWorld w;
    w.v.in_town = true;
    double dx, dy;
    right_of_hero(5, dx, dy);
    w.add(3, dx, dy, AimKind::kNpc);
    AutoTarget aim;
    poll(aim, w, 0, true, false, true, false, kAimRight);
    AimResult r = poll(aim, w, 40, true, false, false, false, kAimRight);
    CHECK(aim.target_id() == 3 && r.left);
    // A click, not a hold (a held button keeps its menu shut): up after
    // a moment, down again a second later while no menu has opened.
    int downs = 0;
    bool was_down = true;
    for (uint64_t t = 80; t <= 2500; t += 40) {
        r = poll(aim, w, t, true, false, false, false, kAimRight, false);
        CHECK(r.aiming);
        downs += r.left && !was_down;
        was_down = r.left;
    }
    CHECK(downs == 2 && aim.target_id() == 3);
    // Its menu opens (a panel: not allowed): the NPC is let go.
    AimPoll p;
    p.now_ms = 2600;
    p.allowed = false;
    p.a_held = true;
    aim.update(p, w);
    CHECK(aim.target_id() == 0);
}

static void test_disabled_reads_nothing()
{
    FakeWorld w;
    w.add(7, 3, -3);
    AutoTarget aim;
    AimSettings s;
    s.enabled = false;
    aim.set_settings(s);
    AimResult r = poll(aim, w, 0, true, false, true, false, kAimRight);
    CHECK(!r.aiming && w.scans == 0);
    // On, but no button held: no reads either.
    AutoTarget idle;
    for (uint64_t t = 0; t < 1000; t += 40)
        poll(idle, w, t, false, false, false, false, kAimRight);
    CHECK(w.scans == 0);
}

// ---- the controls around it ----
static Pad pad_with(uint32_t held, int circle_x = 0, int circle_y = 0)
{
    Pad p;
    p.held = held;
    p.circle_x = circle_x;
    p.circle_y = circle_y;
    return p;
}

static void test_controls()
{
    FakeWorld w;
    double dx, dy;
    right_of_hero(5, dx, dy);
    w.add(7, dx, dy);
    Controls c;
    c.set_frame(640, 480);
    c.set_context(true, false, 0, false);
    c.set_auto_target(AimSettings(), &w);
    c.warp_cursor(300, 200);
    auto step = [&](const Pad &p, uint64_t t) {
        c.update(p, t);
        w.last_cursor_x = c.cursor_x();
        w.last_cursor_y = c.cursor_y();
    };
    // The stick right (walking), then A: the cursor onto the enemy, the hero
    // stops walking (no left button) until the game shows it.
    step(pad_with(0, 140, 0), 0);
    step(pad_with(0, 140, 0), 40);
    CHECK(g_left);   // walking holds the left button
    step(pad_with(kA, 140, 0), 80);
    CHECK(c.cursor_x() == w.list[0].feet_x && !g_left);
    step(pad_with(kA, 140, 0), 120);
    CHECK(g_left);   // shown under the cursor: the attack
    // Let go, stick centred: the cursor goes back where it was.
    step(pad_with(0), 160);
    CHECK(!g_left && c.cursor_x() != w.list[0].feet_x);
    // L held (stand still, Shift): A is the plain button, where the cursor is.
    const int x = c.cursor_x(), y = c.cursor_y();
    step(pad_with(kL | kA), 200);
    CHECK(g_left && c.cursor_x() == x && c.cursor_y() == y);
    step(pad_with(0), 240);
    // R+A (the character sheet) and ZR+A (F1): no aim, no mouse button.
    step(pad_with(kR | kA), 280);
    CHECK(!g_left && c.cursor_x() == x);
    step(pad_with(0), 320);
    step(pad_with(kZR | kA), 360);
    CHECK(!g_left && c.cursor_x() == x);
    step(pad_with(0), 400);
    // A script's tap: clicks where the script put the cursor.
    Pad tap = pad_with(kA);
    tap.aim_off = true;
    step(tap, 440);
    CHECK(g_left && c.cursor_x() == x);
    step(pad_with(0), 480);
    // A panel open: A is the plain button.
    c.set_context(true, true, 0, false);
    step(pad_with(kA), 520);
    CHECK(g_left && c.cursor_x() == x);
    step(pad_with(0), 560);
    // An item on the cursor: the same.
    c.set_context(true, false, 0, true);
    step(pad_with(kA), 600);
    CHECK(g_left && c.cursor_x() == x);
    step(pad_with(0), 640);
    // No world (the default): A as always.
    Controls plain;
    plain.set_frame(640, 480);
    plain.set_context(true, false, 0, false);
    plain.warp_cursor(100, 100);
    plain.update(pad_with(kA), 0);
    CHECK(g_left && plain.cursor_x() == 100);
}

// ---- settings.txt ----
static void test_settings()
{
    AimSettings s;
    std::vector<SettingLine> lines;
    aim_settings_lines(s, lines);
    std::string text = merge_settings_text("bottom=map\n", lines);
    CHECK(text == "bottom=map\naim=1\naim_a_mode=enemies\naim_b_mode=enemies\naim_a_cone=45\naim_a_range=10\n"
                  "aim_b_cone=45\naim_b_range=20\naim_near=4\naim_near_cone=90\naim_turn=60\naim_lost_ms=1500\n"
                  "aim_forget_ms=2500\naim_height=30\naim_confirm=4\n");
    AimSettings r;
    CHECK(parse_aim_setting("aim=0\n", r) && !r.enabled);
    CHECK(parse_aim_setting("aim_b_mode=corpses\r\n", r) && r.b.mode == AimMode::kCorpses);
    CHECK(parse_aim_setting("aim_a_mode=off", r) && r.a.mode == AimMode::kOff);
    CHECK(parse_aim_setting("aim_a_mode=nonsense", r) && r.a.mode == AimMode::kOff);   // unknown: kept
    CHECK(parse_aim_setting("aim_near=6", r) && r.near_subtiles == 6);
    CHECK(parse_aim_setting("aim_b_range=500", r) && r.b.range_subtiles == 60);   // clamped
    CHECK(parse_aim_setting("aim_lost_ms=5", r) && r.lost_ms == 300);
    CHECK(parse_aim_setting("aim_a_cone=", r) && r.a.cone_degrees == 45);         // no number: kept
    CHECK(!parse_aim_setting("aimless=1", r));
    CHECK(!parse_aim_setting("callret=1", r));
    // Written and read back: the same.
    lines.clear();
    aim_settings_lines(r, lines);
    AimSettings back;
    for (const SettingLine &l : lines)
        parse_aim_setting((l.key + "=" + l.value).c_str(), back);
    CHECK(!back.enabled && back.a.mode == AimMode::kOff && back.b.mode == AimMode::kCorpses &&
          back.near_subtiles == 6 && back.b.range_subtiles == 60 && back.lost_ms == 300);
}

int main()
{
    test_classify();
    test_cone();
    test_near_rule();
    test_npcs();
    test_press_waits_for_hover();
    test_never_shown();
    test_adopt_hovered_enemy();
    test_sticky_and_forgotten();
    test_turn_and_death();
    test_lost_and_screen();
    test_near_beats_held_far_target();
    test_town_npc_released_after_talk();
    test_disabled_reads_nothing();
    test_controls();
    test_settings();
    if (g_fail) {
        std::printf("aim: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("aim: all checks passed (15 tests)\n");
    return 0;
}
