// The auto-target on A and B (src/game/auto_target.h): which unit the cone
// picks, the rules (an enemy next to the hero first, NPCs in town, the
// mercenary never; plain A's objects, NPCs and items after the enemies),
// how the target sticks and when it is let go, the circle pad's flicks,
// picking up items (ZR + D-pad, the order, the ones with no room), the
// settings.txt keys and the AIM tab's rows (src/game/aim_settings.h), and
// the controls around it (src/game/controls.cpp: the stick aims instead of
// walking, the cursor goes back). Made-up units stand in for the game. Run through
// `scripts/docker.sh test aim`.
#include "game/aim_settings.h"
#include "game/auto_target.h"
#include "game/controls.h"
#include "game/settings_file.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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
    // it now (each unit's body from 0 to 60 pixels above its feet, 24 wide;
    // a corpse from 4 to 16 below them and a warp from 8 to 40 below, as on the lane).
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
                if (std::abs(last_cursor_x - u.feet_x) <= 12 &&
                    (u.type == 5   ? last_cursor_y >= u.feet_y + 8 && last_cursor_y <= u.feet_y + 40
                     : u.mode == 12 ? last_cursor_y >= u.feet_y + 4 && last_cursor_y <= u.feet_y + 16   // a corpse
                                    : last_cursor_y <= u.feet_y && last_cursor_y >= u.feet_y - 60) &&
                    (u.type != 1 || u.mode != 0)) {
                    out.hover_on = true;
                    out.hover_type = u.type;
                    out.hover_id = u.id;
                    break;
                }
        }
        return true;
    }
    void units(std::vector<AimUnit> &out) override
    {
        scans++;
        for (const AimUnit &u : list)
            if (u.type == 1)
                out.push_back(u);
    }
    bool unit(uint32_t id, AimUnit &out) override { return ground_unit(1, id, out); }
    void ground_units(std::vector<AimUnit> &out) override
    {
        for (const AimUnit &u : list)
            if (u.type != 1)
                out.push_back(u);
    }
    bool ground_unit(uint32_t type, uint32_t id, AimUnit &out) override
    {
        for (const AimUnit &u : list)
            if (u.id == id && u.type == type) {
                out = u;
                return true;
            }
        return false;
    }
    void remove(uint32_t id)
    {
        for (size_t i = 0; i < list.size(); i++)
            if (list[i].id == id) {
                list.erase(list.begin() + (long)i);
                return;
            }
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
    // An object (OperateFn: 2 shrine, 4 chest, 8 door...) at dx, dy; mode 0
    // closed, not used yet.
    AimUnit &add_object(uint32_t id, double dx, double dy, uint8_t operate_fn, uint32_t mode = 0)
    {
        AimUnit u;
        u.type = 2;
        u.id = id;
        u.cls = 5;
        u.mode = mode;
        u.flags_a = 0x30000002;
        u.flags_b = 0x02000080;
        u.operate_fn = operate_fn;
        place(u, dx, dy);
        list.push_back(u);
        return list.back();
    }
    // A level's warp (unit type 5: a cave entrance, stairs) at dx, dy, with
    // the flags the lane showed on Lut Gholein's.
    AimUnit &add_warp(uint32_t id, double dx, double dy)
    {
        AimUnit u;
        u.type = 5;
        u.id = id;
        u.cls = 19;
        u.mode = 0;
        u.flags_a = 0x22;
        u.flags_b = 0x02000080;
        place(u, dx, dy);
        list.push_back(u);
        return list.back();
    }
    // An item on the ground ("gld ", "r08", "hax "...), quality 2 normal, 5 set, 7 unique.
    AimUnit &add_item(uint32_t id, double dx, double dy, const char *code, uint32_t quality = 2)
    {
        AimUnit u;
        u.type = 4;
        u.id = id;
        u.cls = 523;
        u.mode = 3;
        u.flags_a = 0x30000023;
        u.flags_b = 0x02000080;
        u.quality = quality;
        char c[4] = {' ', ' ', ' ', ' '};
        for (int i = 0; i < 4 && code[i]; i++)
            c[i] = code[i];
        u.code = (uint32_t)(uint8_t)c[0] | (uint32_t)(uint8_t)c[1] << 8 | (uint32_t)(uint8_t)c[2] << 16 |
                 (uint32_t)(uint8_t)c[3] << 24;
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
static AimResult run(AutoTarget &aim, FakeWorld &w, const AimPoll &p)
{
    AimResult r = aim.update(p, w);
    if (r.aiming) {
        w.last_cursor_x = r.cursor_x;
        w.last_cursor_y = r.cursor_y;
    }
    return r;
}

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

static std::vector<AimKind> kinds_of(const FakeWorld &w)
{
    std::vector<AimKind> kinds;
    for (const AimUnit &u : w.list)
        kinds.push_back(classify_unit(u, w.v.in_town));
    return kinds;
}

static std::vector<uint64_t> keys_of(const FakeWorld &w, const std::vector<uint32_t> &ids)
{
    std::vector<uint64_t> keys;
    for (uint32_t id : ids)
        for (const AimUnit &u : w.list)
            if (u.id == id)
                keys.push_back(aim_key(u.type, u.id));
    return keys;
}

static int choose(FakeWorld &w, const AimProfile &p, const float *dir, bool *near_rule = nullptr,
                  const AimSettings &s = AimSettings(), std::vector<uint32_t> skip = {}, bool button_a = true)
{
    const int i = choose_target(w.list, kinds_of(w), w.v, p, s, dir[0], dir[1], keys_of(w, skip), near_rule, button_a);
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
    low.add(4, 14, 14);   // 224 pixels straight down: y 456, the panel
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

static void test_turning_keeps_and_death()
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
    // The stick turned the other way while held (slowly: no flick): the
    // target stays; only a flick switches.
    for (uint64_t t = 80; t < 600; t += 40)
        poll(aim, w, t, true, false, false, false, kAimLeft);
    CHECK(aim.target_id() == 7);
    AimResult r = poll(aim, w, 640, true, false, false, false, kAimLeft);
    CHECK(r.left);
    // It dies under the held button: the button comes up, no click onto
    // its corpse; the next one is looked for at once (here, 8 to the left).
    w.find(7)->mode = 0;
    r = poll(aim, w, 680, true, false, false, false, kAimLeft, false);
    CHECK(aim.target_id() == 8 && aim.stats().dropped_dead == 1);
    w.find(8)->mode = 0;
    r = poll(aim, w, 720, true, false, false, false, kAimLeft, false);
    CHECK(!r.aiming && r.hold_plain_buttons && aim.target_id() == 0 && aim.stats().dropped_dead == 2);
    // A new enemy comes into the cone while held: taken within a rescan.
    right_of_hero(-4, dx, dy);
    w.add(9, dx, dy);
    r = poll(aim, w, 900, true, false, false, false, kAimLeft, false);
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

static void test_held_target_not_replaced_by_nearer()
{
    FakeWorld w;
    double dx, dy;
    right_of_hero(12, dx, dy);
    w.add(7, dx, dy);
    AutoTarget aim;
    poll(aim, w, 0, false, true, false, true, kAimRight);
    poll(aim, w, 40, false, true, false, false, kAimRight);
    CHECK(aim.target_id() == 7);
    // An enemy walks up next to the hero while B is held: no switch by
    // itself (a flick switches).
    w.add(8, 1, 1);
    for (uint64_t t = 80; t < 1000; t += 40)
        poll(aim, w, t, false, true, false, false, kAimRight, false);
    CHECK(aim.target_id() == 7 && aim.stats().near_rule == 0);
    // A fresh press: the one next to the hero comes first.
    poll(aim, w, 1040, false, false, false, false, kAimRight, false);
    poll(aim, w, 1080, false, true, false, true, kAimRight, false);
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
    CHECK(text == "bottom=map\naim=1\naim_a_mode=enemies\naim_b_mode=enemies\naim_a_cone=45\naim_a_range=20\n"
                  "aim_b_cone=45\naim_b_range=20\naim_near=4\naim_near_cone=90\naim_flick_cone=30\n"
                  "aim_flick_ms=150\naim_flick_light=40\naim_flick_strong=80\naim_lost_ms=1500\n"
                  "aim_forget_ms=2500\naim_height=30\naim_confirm=4\naim_objects=1\naim_loot=1\n"
                  "aim_gold_first=0\n");
    // An older file (Stage 2: aim_turn, A's range of 10, its default then):
    // read, its A range moved to the new default 20, and its aim_turn line
    // dropped when the file is written again.
    AimSettings old;
    CHECK(parse_aim_setting("aim_turn=60", old) && parse_aim_setting("aim_a_range=10", old));
    CHECK(old.a.range_subtiles == 10 && old.flick_cone_degrees == 30 && old.loot && !old.gold_first);
    migrate_older_aim_settings(old);
    CHECK(old.a.range_subtiles == 20 && old.b.range_subtiles == 20);
    lines.clear();
    aim_settings_lines(old, lines);
    text = merge_settings_text("aim=1\naim_turn=60\naim_a_range=10\nbottom=map\n", lines, {"aim_turn"});
    CHECK(text.find("aim_turn") == std::string::npos && text.find("aim_a_range=20\n") != std::string::npos &&
          text.find("aim_flick_cone=30\n") != std::string::npos && text.find("bottom=map\n") != std::string::npos);
    // Another A range in an older file is kept.
    AimSettings other;
    CHECK(parse_aim_setting("aim_a_range=14", other));
    migrate_older_aim_settings(other);
    CHECK(other.a.range_subtiles == 14);
    // The new keys.
    AimSettings n;
    CHECK(parse_aim_setting("aim_flick_cone=200", n) && n.flick_cone_degrees == 90);
    CHECK(parse_aim_setting("aim_flick_ms=10", n) && n.flick_ms == 50);
    CHECK(parse_aim_setting("aim_flick_light=50", n) && n.flick_light_percent == 50);
    CHECK(parse_aim_setting("aim_flick_strong=90", n) && n.flick_strong_percent == 90);
    CHECK(parse_aim_setting("aim_objects=0", n) && !n.objects);
    CHECK(parse_aim_setting("aim_loot=0", n) && !n.loot);
    CHECK(parse_aim_setting("aim_gold_first=1", n) && n.gold_first);
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

// ---- the AIM tab's SETTINGS rows ----
static void test_setting_rows()
{
    AimSettings s;
    CHECK(std::string(aim_setting_label(0)) == "Auto-target" && aim_setting_value(s, 0) == "on");
    CHECK(aim_setting_value(s, 3) == "20" && aim_setting_value(s, 6) == "20");
    CHECK(aim_setting_value(s, 7) == "30 deg" && aim_setting_value(s, 9) == "40%");
    // "+" and "-" within the limits.
    CHECK(aim_setting_step(s, 3, 1) && s.a.range_subtiles == 21);
    CHECK(aim_setting_step(s, 3, -1) && s.a.range_subtiles == 20);
    s.a.range_subtiles = 60;
    CHECK(!aim_setting_step(s, 3, 1) && s.a.range_subtiles == 60);
    CHECK(aim_setting_step(s, 2, -1) && s.a.cone_degrees == 40);
    // The modes go round: enemies, corpses, off.
    CHECK(aim_setting_step(s, 4, 1) && s.b.mode == AimMode::kCorpses);
    CHECK(aim_setting_step(s, 4, 1) && s.b.mode == AimMode::kOff);
    CHECK(aim_setting_step(s, 4, 1) && s.b.mode == AimMode::kEnemies);
    CHECK(aim_setting_step(s, 4, -1) && s.b.mode == AimMode::kOff);
    // The light flick stays below the strong one.
    s.flick_light_percent = 75;
    s.flick_strong_percent = 80;
    CHECK(!aim_setting_step(s, 9, 1) && s.flick_light_percent == 75);
    CHECK(!aim_setting_step(s, 10, -1) && s.flick_strong_percent == 80);
    // On/off rows.
    CHECK(aim_setting_step(s, 13, 1) && s.gold_first && aim_setting_value(s, 13) == "on");
    CHECK(aim_setting_step(s, 12, -1) && !s.loot);
    CHECK(aim_setting_step(s, 0, 1) && !s.enabled);
    CHECK(!aim_setting_step(s, kAimSettingRows, 1));
    // Every row has a label and a value that fit the bottom screen's
    // 40 columns ("label  - value +").
    for (int row = 0; row < kAimSettingRows; row++)
        CHECK(std::strlen(aim_setting_label(row)) > 0 && std::strlen(aim_setting_label(row)) <= 14 &&
              !aim_setting_value(s, row).empty() && aim_setting_value(s, row).size() <= 8);
}

// ---- flicks of the circle pad ----
// One poll with the stick at x, y (fractions of the full tilt, screen y
// down) and A held.
static AimResult poll_stick(AutoTarget &aim, FakeWorld &w, uint64_t t, float x, float y, bool a_pressed = false)
{
    AimPoll p;
    p.now_ms = t;
    p.allowed = true;
    p.a_held = true;
    p.a_pressed = a_pressed;
    p.stick_x = x;
    p.stick_y = y;
    const float m = std::sqrt(x * x + y * y);
    p.stick_engaged = m > 0.2f;
    p.dir_x = p.stick_engaged ? x / m : 1;
    p.dir_y = p.stick_engaged ? y / m : 0;
    p.new_frame = true;
    return run(aim, w, p);
}

// A flick: out of the centre to (x, y) in two polls 20 ms apart, held there
// for hold_ms, back to the centre. t moves on.
static void flick(AutoTarget &aim, FakeWorld &w, uint64_t &t, float x, float y, uint64_t hold_ms = 40)
{
    poll_stick(aim, w, t, 0, 0);
    t += 20;
    poll_stick(aim, w, t, x * 0.6f, y * 0.6f);
    t += 20;
    for (uint64_t end = t + hold_ms; t <= end; t += 20)
        poll_stick(aim, w, t, x, y);
    poll_stick(aim, w, t, 0, 0);
    t += 40;
    poll_stick(aim, w, t, 0, 0);
    t += 40;
}

static void test_flick_detector()
{
    AimSettings s;
    // Quick and full: a strong flick, at once, to the right.
    AimFlicks f;
    CHECK(!f.update(0, 0, 0, s).happened);
    CHECK(!f.update(0.5f, 0, 20, s).happened);
    AimFlicks::Flick got = f.update(0.95f, 0, 40, s);
    CHECK(got.happened && got.strong && got.dir_x > 0.99f);
    // Held there, or pushed again without coming back: no second one.
    for (uint64_t t = 60; t < 600; t += 20)
        CHECK(!f.update(t % 40 ? 0.95f : 0.85f, 0, t, s).happened);
    // Back to the centre: the next one counts. A light one (half the
    // tilt), read when the stick starts coming back.
    CHECK(!f.update(0, 0, 600, s).happened);
    CHECK(!f.update(0, -0.45f, 620, s).happened);
    CHECK(!f.update(0, -0.55f, 640, s).happened);
    got = f.update(0, -0.4f, 660, s);
    CHECK(got.happened && !got.strong && got.dir_y < -0.99f);
    // A light one held still: read after the settle time.
    f.update(0, 0, 700, s);
    f.update(-0.5f, 0, 720, s);
    CHECK(!f.update(-0.5f, 0, 740, s).happened);
    got = f.update(-0.5f, 0, 720 + AimFlicks::kSettleMs, s);
    CHECK(got.happened && !got.strong && got.dir_x < -0.99f);
    // A slow tilt (walking): never a flick, however far it goes.
    f.update(0, 0, 1000, s);
    bool any = false;
    for (uint64_t t = 1040; t < 1600; t += 40)
        any |= f.update(0.25f + (float)(t - 1040) / 800.0f, 0, t, s).happened;
    CHECK(!any);
    // A small wobble (below the light threshold): nothing.
    f.update(0, 0, 2000, s);
    CHECK(!f.update(0.3f, 0, 2020, s).happened && !f.update(0.35f, 0, 2040, s).happened &&
          !f.update(0.3f, 0, 2200, s).happened);
}

// Enemies straight right of the hero at 3, 6 and 9 subtiles (1, 2, 3); 4
// 60 pixels above 1 on the screen (outside the fighting line's 30
// degrees); 5 45 pixels above the hero. The hero is attacking.
static void flick_world(FakeWorld &w)
{
    double dx, dy;
    right_of_hero(3, dx, dy);
    w.add(1, dx, dy);
    w.add(4, dx - 3.75, dy - 3.75);
    right_of_hero(6, dx, dy);
    w.add(2, dx, dy);
    right_of_hero(9, dx, dy);
    w.add(3, dx, dy);
    w.add(5, -2.8, -2.8);
    w.v.hero_mode = 7;
}

// A on the nearest to the right (1), confirmed.
static void start_fight(AutoTarget &aim, FakeWorld &w, uint64_t &t)
{
    AimSettings s;
    s.near_subtiles = 0;
    aim.set_settings(s);
    poll(aim, w, t, true, false, true, false, kAimRight, false);
    t += 40;
    poll(aim, w, t, true, false, false, false, kAimRight, false);
    t += 40;
    CHECK(aim.target_id() == 1);
}

static void test_flick_same_direction()
{
    // The way the hero fights (right), light or strong: the next one
    // further away; from the furthest, round to the nearest.
    FakeWorld w;
    flick_world(w);
    AutoTarget aim;
    uint64_t t = 0;
    start_fight(aim, w, t);
    flick(aim, w, t, 0.5f, 0);   // light
    CHECK(aim.target_id() == 2);
    flick(aim, w, t, 1, 0);      // strong
    CHECK(aim.target_id() == 3);
    flick(aim, w, t, 0.5f, 0);
    CHECK(aim.target_id() == 1);   // round again
    // Within the flick cone of the fighting line (20 degrees off): the same.
    flick(aim, w, t, 0.94f * 0.5f, -0.34f * 0.5f);
    CHECK(aim.target_id() == 2);
    CHECK(aim.stats().flicks == 4 && aim.stats().flick_switches == 4);
    // The new target is attacked once the game shows it.
    AimResult r = poll(aim, w, t, true, false, false, false, kAimRight, false);
    r = poll(aim, w, t + 40, true, false, false, false, kAimRight, false);
    CHECK(r.left && r.cursor_x == w.find(2)->feet_x);
}

static void test_flick_light_refines()
{
    // A light flick another way: the enemy in that direction from the
    // current target (4, above 1), not from the hero (5).
    FakeWorld w;
    flick_world(w);
    AutoTarget aim;
    uint64_t t = 0;
    start_fight(aim, w, t);
    flick(aim, w, t, 0, -0.5f);
    CHECK(aim.target_id() == 4);
    // And back down from 4: 1 is below it.
    flick(aim, w, t, 0, 0.5f);
    CHECK(aim.target_id() == 1);
    // The cone is a setting: 4 is straight above 1; a flick 40 degrees off
    // "up" (to the right) misses it with 30 degrees, finds it with 45.
    flick(aim, w, t, 0.32f, -0.38f);
    CHECK(aim.target_id() == 1);
    AimSettings s = aim.settings();
    s.flick_cone_degrees = 45;
    aim.set_settings(s);
    flick(aim, w, t, 0.32f, -0.38f);
    CHECK(aim.target_id() == 4);
}

static void test_flick_strong_turns()
{
    // A strong flick another way: the hero turns; the nearest to the hero
    // in the profile's cone round that direction (5, above the hero; not 4,
    // above the target 1, 49 degrees off "up" from the hero).
    FakeWorld w;
    flick_world(w);
    AutoTarget aim;
    uint64_t t = 0;
    start_fight(aim, w, t);
    flick(aim, w, t, 0.5f, 0);
    CHECK(aim.target_id() == 2);
    flick(aim, w, t, 0, -1);
    CHECK(aim.target_id() == 5);
    // Strong back to the right: the nearest there (1).
    flick(aim, w, t, -1, 0);   // left: nobody
    CHECK(aim.target_id() == 5);
    flick(aim, w, t, 1, 0);
    CHECK(aim.target_id() == 1);
}

static void test_flick_nobody_keeps()
{
    FakeWorld w;
    flick_world(w);
    AutoTarget aim;
    uint64_t t = 0;
    start_fight(aim, w, t);
    // Nobody down from the target, or left of the hero: the target stays.
    flick(aim, w, t, 0, 0.5f);
    CHECK(aim.target_id() == 1);
    flick(aim, w, t, -1, 0);
    CHECK(aim.target_id() == 1);
    CHECK(aim.stats().flicks == 2 && aim.stats().flick_switches == 0);
    // Alone in the fighting line: a same-way flick keeps it too.
    FakeWorld one;
    double dx, dy;
    right_of_hero(3, dx, dy);
    one.add(1, dx, dy);
    AutoTarget aim2;
    t = 0;
    start_fight(aim2, one, t);
    flick(aim2, one, t, 1, 0);
    CHECK(aim2.target_id() == 1);
    // A slow tilt (walking) is not a flick: no switch.
    FakeWorld w2;
    flick_world(w2);
    AutoTarget aim3;
    t = 0;
    start_fight(aim3, w2, t);
    for (int i = 0; i <= 20; i++, t += 40)
        poll_stick(aim3, w2, t, 0.25f + 0.035f * (float)i, 0);
    CHECK(aim3.target_id() == 1 && aim3.stats().flicks == 0);
}

static void test_flick_one_step()
{
    // One flick, one step: the stick held out long after it does nothing
    // more; it has to come back to the centre first.
    FakeWorld w;
    flick_world(w);
    AutoTarget aim;
    uint64_t t = 0;
    start_fight(aim, w, t);
    flick(aim, w, t, 1, 0, 1500);
    CHECK(aim.target_id() == 2 && aim.stats().flicks == 1);
    // Out again, held, eased back a little and pushed again without coming
    // back to the centre: one more step only.
    poll_stick(aim, w, t, 0.6f, 0);
    poll_stick(aim, w, t + 20, 1, 0);
    for (uint64_t u = t + 40; u < t + 400; u += 20)
        poll_stick(aim, w, u, u % 80 ? 0.5f : 1.0f, 0);
    CHECK(aim.target_id() == 3 && aim.stats().flicks == 2);
    t += 400;
    // While no button is held, a flick does nothing (and is not counted).
    FakeWorld w2;
    flick_world(w2);
    AutoTarget aim2;
    t = 0;
    start_fight(aim2, w2, t);
    AimPoll p;
    p.allowed = true;
    for (int i = 0; i < 4; i++) {
        p.now_ms = t + (uint64_t)i * 20;
        p.stick_x = i == 0 || i == 3 ? 0 : 1;
        run(aim2, w2, p);
    }
    CHECK(aim2.target_id() == 1 && aim2.stats().flicks == 0);
}

static void test_flicks_enemies_only()
{
    // On an NPC, an object or an item, a flick doesn't switch.
    FakeWorld w;
    double dx, dy;
    right_of_hero(4, dx, dy);
    w.add_object(20, dx, dy, 4);   // a chest
    right_of_hero(8, dx, dy);
    w.add_object(21, dx, dy, 4);
    AutoTarget aim;
    uint64_t t = 0;
    poll(aim, w, t, true, false, true, false, kAimRight, false);
    CHECK(aim.target_id() == 20 && aim.target_type() == 2);
    t += 40;
    flick(aim, w, t, 1, 0);
    CHECK(aim.target_id() == 20 && aim.stats().flicks == 0);
}

// ---- plain A: objects, NPCs and items after the enemies ----
static void test_objects_and_items_order()
{
    double dx, dy;
    AimSettings s;
    s.near_subtiles = 0;
    FakeWorld w;
    right_of_hero(5, dx, dy);
    w.add_item(30, dx, dy + 0.2, "hax ");
    right_of_hero(6, dx, dy);
    w.add(10, dx, dy - 0.3, AimKind::kNpc);
    right_of_hero(7, dx, dy);
    w.add_object(20, dx, dy, 4);   // a chest
    right_of_hero(9, dx, dy);
    w.add(1, dx, dy);               // an enemy, furthest
    // Enemies first, then objects, NPCs, items.
    CHECK(choose(w, s.a, kAimRight, nullptr, s) == 1);
    w.find(1)->mode = 0;
    CHECK(choose(w, s.a, kAimRight, nullptr, s) == 20);
    w.find(20)->mode = 1;   // opened: used, skipped
    CHECK(classify_unit(*w.find(20), false) == AimKind::kNone);
    CHECK(choose(w, s.a, kAimRight, nullptr, s) == 10);
    w.find(10)->mode = 0;
    w.find(10)->flags_a &= ~aim_bits::kSelectable;
    CHECK(choose(w, s.a, kAimRight, nullptr, s) == 30);
    // B never takes objects or items.
    CHECK(choose(w, s.b, kAimRight, nullptr, s, {}, false) == 0);
    // aim_loot off: no item; aim_objects off: no object.
    AimSettings no_loot = s;
    no_loot.loot = false;
    CHECK(choose(w, s.a, kAimRight, nullptr, no_loot) == 0);
    FakeWorld objects;
    right_of_hero(3, dx, dy);
    objects.add_object(1, dx, dy, 5);           // a barrel, nearest
    right_of_hero(5, dx, dy);
    objects.add_object(2, dx, dy - 0.2, 8);     // a door
    right_of_hero(6, dx, dy);
    objects.add_object(3, dx, dy + 0.2, 23, 2); // a waypoint (active: mode 2, still used)
    right_of_hero(7, dx, dy);
    objects.add_object(4, dx, dy - 0.3, 4);     // a chest
    right_of_hero(8, dx, dy);
    objects.add_object(5, dx, dy + 0.3, 2);     // a shrine, furthest
    right_of_hero(4, dx, dy);
    objects.add_object(6, dx, dy, 7);           // an exploding barrel: never
    objects.add_object(7, dx, dy + 0.1, 32);    // the stash: never
    CHECK(choose(objects, s.a, kAimRight, nullptr, s) == 5);
    objects.find(5)->flags_a &= ~aim_bits::kSelectable;   // used shrine
    CHECK(choose(objects, s.a, kAimRight, nullptr, s) == 4);
    objects.find(4)->mode = 1;
    CHECK(choose(objects, s.a, kAimRight, nullptr, s) == 3);
    objects.find(3)->flags_a &= ~aim_bits::kSelectable;
    CHECK(choose(objects, s.a, kAimRight, nullptr, s) == 2);
    objects.find(2)->mode = 1;   // opened door
    CHECK(choose(objects, s.a, kAimRight, nullptr, s) == 1);
    objects.find(1)->mode = 1;
    CHECK(choose(objects, s.a, kAimRight, nullptr, s) == 0);
    AimSettings no_objects = s;
    no_objects.objects = false;
    objects.find(1)->mode = 0;
    CHECK(choose(objects, s.a, kAimRight, nullptr, no_objects) == 0);
    // Gold first: the nearest gold before a nearer item.
    FakeWorld items;
    right_of_hero(3, dx, dy);
    items.add_item(1, dx, dy, "hax ");
    right_of_hero(6, dx, dy);
    items.add_item(2, dx, dy, "gld ");
    CHECK(choose(items, s.a, kAimRight, nullptr, s) == 1);
    AimSettings gold = s;
    gold.gold_first = true;
    CHECK(choose(items, s.a, kAimRight, nullptr, gold) == 2);
    // An item still falling (mode 5) is not taken yet.
    items.find(1)->mode = 5;
    CHECK(choose(items, s.a, kAimRight, nullptr, s) == 2);
    // In town: NPCs first, then objects, then items.
    FakeWorld town;
    town.v.in_town = true;
    right_of_hero(6, dx, dy);
    town.add(10, dx, dy, AimKind::kNpc);
    right_of_hero(4, dx, dy);
    town.add_object(20, dx, dy, 23, 2);
    right_of_hero(3, dx, dy);
    town.add_item(30, dx, dy, "gld ");
    CHECK(choose(town, s.a, kAimRight, nullptr, s) == 10);
    town.remove(10);
    CHECK(choose(town, s.a, kAimRight, nullptr, s) == 20);
    town.remove(20);
    CHECK(choose(town, s.a, kAimRight, nullptr, s) == 30);
}

static void test_loot_order()
{
    FakeWorld w;
    double dx, dy;
    right_of_hero(2, dx, dy);
    w.add_item(1, dx, dy, "hax ");          // a normal axe, nearest
    right_of_hero(-3, dx, dy);
    w.add_item(2, dx, dy, "gld ");          // gold, behind the hero
    w.add_item(3, 2, 2, "r08");             // a rune (Ral)
    w.add_item(4, -2, -2.5, "rin ", 7);     // a unique ring
    w.add_item(5, 3, -1, "amu ", 5);        // a set amulet
    CHECK(loot_class(w.list[0]) == AimLoot::kOther && loot_class(w.list[1]) == AimLoot::kGold &&
          loot_class(w.list[2]) == AimLoot::kRune && loot_class(w.list[3]) == AimLoot::kUniqueOrSet &&
          loot_class(w.list[4]) == AimLoot::kUniqueOrSet);
    AimSettings s;
    auto pick = [&](AimLootCommand c, const AimSettings &st, std::vector<uint32_t> skip) {
        const int i = choose_loot(w.list, kinds_of(w), w.v, c, st, 20, keys_of(w, skip));
        return i < 0 ? 0 : (int)w.list[(size_t)i].id;
    };
    // Everything: unique or set (the nearer of the two), runes, gold, the rest.
    CHECK(pick(AimLootCommand::kEverything, s, {}) == 5);
    CHECK(pick(AimLootCommand::kEverything, s, {5}) == 4);
    CHECK(pick(AimLootCommand::kEverything, s, {5, 4}) == 3);
    CHECK(pick(AimLootCommand::kEverything, s, {5, 4, 3}) == 2);
    CHECK(pick(AimLootCommand::kEverything, s, {5, 4, 3, 2}) == 1);
    // Gold first moves gold to the front.
    AimSettings gold = s;
    gold.gold_first = true;
    CHECK(pick(AimLootCommand::kEverything, gold, {}) == 2);
    // Valuables: unique, set and runes only; gold: gold only.
    CHECK(pick(AimLootCommand::kValuables, s, {5, 4}) == 3);
    CHECK(pick(AimLootCommand::kValuables, s, {5, 4, 3}) == 0);
    CHECK(pick(AimLootCommand::kGold, s, {}) == 2);
    CHECK(pick(AimLootCommand::kGold, s, {2}) == 0);
    // Beyond the range: not taken.
    w.place(*w.find(2), -5, -20);   // 20.6 subtiles
    CHECK(choose_loot(w.list, kinds_of(w), w.v, AimLootCommand::kGold, s, 20, {}) < 0);
    CHECK(choose_loot(w.list, kinds_of(w), w.v, AimLootCommand::kGold, s, 21, {}) >= 0);
    CHECK(loot_rank(AimLoot::kGold, true) < loot_rank(AimLoot::kUniqueOrSet, true));
}

static AimResult poll_loot(AutoTarget &aim, FakeWorld &w, uint64_t t, AimLootCommand c, bool pressed)
{
    AimPoll p;
    p.now_ms = t;
    p.allowed = true;
    p.loot = c;
    p.loot_pressed = pressed;
    p.new_frame = true;
    return run(aim, w, p);
}

static void test_loot_command()
{
    // ZR + Right held: the gold, one after another, picked up with the left
    // button (a click each); all around the hero, no cone.
    FakeWorld w;
    double dx, dy;
    right_of_hero(-3, dx, dy);
    w.add_item(1, dx, dy, "gld ");
    w.add_item(2, 2, 2, "gld ");
    w.add_item(3, 1, 1, "hax ");
    AutoTarget aim;
    AimResult r = poll_loot(aim, w, 0, AimLootCommand::kGold, true);
    CHECK(aim.target_id() == 2 && aim.target_type() == 4 && r.aiming && !r.left);
    CHECK(r.cursor_x == w.find(2)->feet_x && r.cursor_y == w.find(2)->feet_y - 4);
    r = poll_loot(aim, w, 40, AimLootCommand::kGold, false);
    CHECK(r.left && !r.right);
    // Picked up: the next gold, not the axe.
    w.remove(2);
    poll_loot(aim, w, 80, AimLootCommand::kGold, false);
    CHECK(aim.target_id() == 1);
    w.remove(1);
    r = poll_loot(aim, w, 120, AimLootCommand::kGold, false);
    CHECK(aim.target_id() == 0 && !r.aiming);
    // Let go: nothing; ZR + Down then takes the axe.
    run(aim, w, AimPoll());
    poll_loot(aim, w, 200, AimLootCommand::kEverything, true);
    CHECK(aim.target_id() == 3);
    // An enemy around doesn't matter to a loot command.
    w.add(9, 1.5, -1.5);
    poll_loot(aim, w, 240, AimLootCommand::kEverything, false);
    CHECK(aim.target_id() == 3);
}

static void test_item_no_room()
{
    // A held A on an item the hero stands by but never picks up (no room):
    // skipped after lost_ms, the next one taken; it isn't tried again for
    // a while.
    FakeWorld w;
    w.add_item(1, 1, 0.5, "hax ");
    w.add_item(2, 2, 2.5, "hax ");
    AutoTarget aim;
    uint64_t t = 0;
    poll(aim, w, t, true, false, true, false, kAimDown, false);
    CHECK(aim.target_id() == 1);
    for (t = 40; t < 1500; t += 40)
        poll(aim, w, t, true, false, false, false, kAimDown, false);
    CHECK(aim.target_id() == 1);
    for (; t < 1800; t += 40)
        poll(aim, w, t, true, false, false, false, kAimDown, false);
    CHECK(aim.stats().no_room == 1);
    for (int i = 0; i < 10; i++, t += 40)
        poll(aim, w, t, true, false, false, false, kAimDown, false);
    CHECK(aim.target_id() == 2);
    // A new press a few seconds later: still not the first.
    poll(aim, w, t, false, false, false, false, kAimDown, false);
    poll(aim, w, t + 5000, true, false, true, false, kAimDown, false);
    CHECK(aim.target_id() == 2);
    // The same with ZR + Down.
    FakeWorld w2;
    w2.add_item(1, 1, 0.5, "gld ");
    AutoTarget aim2;
    for (t = 0; t < 2000; t += 40)
        poll_loot(aim2, w2, t, AimLootCommand::kEverything, t == 0);
    CHECK(aim2.stats().no_room == 1 && aim2.target_id() == 0);
    // Clicks while it waits: one every second.
    FakeWorld w3;
    w3.add_item(1, 1, 0.5, "gld ");
    AutoTarget aim3;
    int downs = 0;
    bool was = false;
    for (t = 0; t < 1400; t += 40) {
        AimResult r = poll_loot(aim3, w3, t, AimLootCommand::kEverything, t == 0);
        downs += r.left && !was;
        was = r.left;
    }
    CHECK(downs == 2);
}

static void test_b_on_corpse_skill()
{
    // The right button's skill is cast on corpses (Raise Skeleton): B aims
    // at corpses by itself; A still at enemies.
    FakeWorld w;
    double dx, dy;
    right_of_hero(4, dx, dy);
    w.add(1, dx, dy);
    right_of_hero(6, dx, dy);
    w.add(2, dx, dy + 0.3, AimKind::kCorpse);
    w.v.right_skill = 70;
    w.v.right_skill_on_corpses = true;
    AutoTarget aim;
    poll(aim, w, 0, false, true, false, true, kAimRight);
    CHECK(aim.target_id() == 2);
    poll(aim, w, 40, false, false, false, false, kAimRight);
    poll(aim, w, 80, true, false, true, false, kAimRight);
    CHECK(aim.target_id() == 1);
    // Another skill: B on enemies again.
    w.v.right_skill_on_corpses = false;
    AutoTarget aim2;
    poll(aim2, w, 0, false, true, false, true, kAimRight);
    CHECK(aim2.target_id() == 1);
    // B set to off stays off.
    w.v.right_skill_on_corpses = true;
    AimSettings s;
    s.b.mode = AimMode::kOff;
    AutoTarget aim3;
    aim3.set_settings(s);
    AimResult r = poll(aim3, w, 0, false, true, false, true, kAimRight);
    CHECK(!r.aiming && aim3.target_id() == 0);
}

// A corpse skill (Find Item) with B held: the cursor goes below the
// corpse's feet, where the game shows it, and B is held there; once the hero
// casts, that corpse is used up: dropped, the next one taken, and not chosen
// again. A cast already going on when a corpse is chosen doesn't count.
static void test_corpse_skill_cast()
{
    FakeWorld w;
    double dx, dy;
    right_of_hero(5, dx, dy);
    w.add(2, dx, dy, AimKind::kCorpse);
    right_of_hero(8, dx, dy);
    w.add(3, dx, dy + 0.4, AimKind::kCorpse);
    w.v.right_skill = 142;
    w.v.right_skill_on_corpses = true;
    w.v.hero_mode = 10;   // still casting at another corpse
    AutoTarget aim;
    AimResult r = poll(aim, w, 0, false, true, false, true, kAimRight);
    CHECK(aim.target_id() == 2);
    AimUnit c;
    CHECK(w.ground_unit(1, 2, c) && r.cursor_y > c.feet_y);   // below its feet
    uint64_t t = 0;
    for (int i = 0; i < 6; i++)
        r = poll(aim, w, t += 40, false, true, false, false, kAimRight);
    CHECK(aim.target_id() == 2 && r.right);   // the old cast is not this corpse's
    w.v.hero_mode = 2;                        // walking to it
    r = poll(aim, w, t += 40, false, true, false, false, kAimRight);
    CHECK(aim.target_id() == 2 && r.right);
    w.v.hero_mode = 10;                       // the skill goes off on it
    r = poll(aim, w, t += 40, false, true, false, false, kAimRight);
    CHECK(aim.target_id() == 0 && !r.right);
    w.v.hero_mode = 1;
    for (int i = 0; i < 4; i++)
        r = poll(aim, w, t += 40, false, true, false, false, kAimRight);
    CHECK(aim.target_id() == 3);
    // A new press later: the used corpse is still left alone.
    poll(aim, w, t += 40, false, false, false, false, kAimRight);
    poll(aim, w, t += 3000, false, true, false, true, kAimRight);
    CHECK(aim.target_id() == 3);
}

// ---- a level's warps (cave entrances, stairs) on plain A ----
static void test_warps()
{
    double dx, dy;
    AimSettings s;
    s.near_subtiles = 0;
    // What a warp counts as: an object (any mode), beside doors and levers.
    FakeWorld w;
    right_of_hero(6, dx, dy);
    AimUnit &warp = w.add_warp(40, dx, dy);
    CHECK(classify_unit(warp, false) == AimKind::kObject);
    CHECK(classify_unit(warp, true) == AimKind::kObject);   // in town too (Lut Gholein's sewer)
    CHECK(object_kind(warp) == AimObject::kWarp);
    CHECK(object_rank(AimObject::kWarp) == object_rank(AimObject::kDoor));
    CHECK(object_rank(AimObject::kWarp) == object_rank(AimObject::kLever));
    warp.mode = 1;
    CHECK(classify_unit(warp, false) == AimKind::kObject);
    warp.flags_b = 0x02000000;   // the game's hit test doesn't pick it (another level's)
    CHECK(classify_unit(warp, false) == AimKind::kNone);
    warp.flags_b = 0x02000080;
    warp.flags_a = 0;
    CHECK(classify_unit(warp, false) == AimKind::kNone);
    warp.flags_a = 0x22;
    CHECK(choose(w, s.a, kAimRight, nullptr, s) == 40);
    CHECK(choose(w, s.a, kAimLeft, nullptr, s) == 0);           // the cone holds
    CHECK(choose(w, s.b, kAimRight, nullptr, s, {}, false) == 0);   // B never takes one
    AimSettings no_objects = s;
    no_objects.objects = false;
    CHECK(choose(w, s.a, kAimRight, nullptr, no_objects) == 0);   // aim_objects=0: none
    // The order: shrine, chest, waypoint, then a door, lever or warp (the
    // nearest of those), then a barrel; an enemy before all of them.
    FakeWorld order;
    right_of_hero(3, dx, dy);
    order.add_object(1, dx, dy, 5);            // a barrel, nearest
    right_of_hero(5, dx, dy);
    order.add_warp(2, dx, dy - 0.2);           // a warp
    right_of_hero(7, dx, dy);
    order.add_object(3, dx, dy + 0.2, 8);      // a door, further
    right_of_hero(8, dx, dy);
    order.add_object(4, dx, dy - 0.3, 4);      // a chest, furthest
    CHECK(choose(order, s.a, kAimRight, nullptr, s) == 4);
    order.find(4)->mode = 1;   // opened
    CHECK(choose(order, s.a, kAimRight, nullptr, s) == 2);   // the warp: nearer than the door
    order.place(*order.find(2), dx + 2, dy - 2);   // the warp now further than the door
    CHECK(choose(order, s.a, kAimRight, nullptr, s) == 3);
    order.find(3)->mode = 1;   // the door opened
    CHECK(choose(order, s.a, kAimRight, nullptr, s) == 2);
    right_of_hero(9, dx, dy);
    order.add(9, dx, dy);   // an enemy comes first
    CHECK(choose(order, s.a, kAimRight, nullptr, s) == 9);
    // Pressed: the cursor goes below the warp's spot (where the stairs are
    // drawn), and A clicks once the game shows the warp there.
    FakeWorld press;
    right_of_hero(5, dx, dy);
    press.add_warp(7, dx, dy);
    AutoTarget aim;
    aim.set_settings(s);
    AimResult r = poll(aim, press, 0, true, false, true, false, kAimRight);
    CHECK(r.aiming && !r.left && aim.target_type() == 5 && aim.target_id() == 7);
    CHECK(r.cursor_x == press.list[0].feet_x && r.cursor_y == press.list[0].feet_y + 16);
    r = poll(aim, press, 40, true, false, false, false, kAimRight);
    CHECK(r.aiming && r.left);
    CHECK(aim.stats().confirmed == 1 && aim.stats().objects == 1);
    // Taken (the level changes): it is gone, A clicks plainly again.
    press.remove(7);
    for (int t = 80; t < 400; t += 40)
        r = poll(aim, press, (uint64_t)t, true, false, false, false, kAimRight);
    CHECK(!aim.has_target());
    // Still held, out of the warp beside the one leading back: that one is
    // not taken while A is held, only on the next press.
    right_of_hero(3, dx, dy);
    press.add_warp(8, dx, dy);
    for (int t = 400; t < 800; t += 40)
        r = poll(aim, press, (uint64_t)t, true, false, false, false, kAimRight);
    CHECK(!aim.has_target());
    r = poll(aim, press, 800, false, false, false, false, kAimRight);
    r = poll(aim, press, 840, true, false, true, false, kAimRight);
    CHECK(aim.has_target() && aim.target_id() == 8 && aim.target_type() == 5);
}

// ---- a panel open on one side: targets on the free half only ----
static void test_panel_area()
{
    double dx, dy;
    AimSettings s;
    s.near_subtiles = 0;
    FakeWorld w;
    // The inventory open on the right: the game moves the hero to the left
    // half's middle (160 across at 640x480).
    w.v.hero_x = 160;
    right_of_hero(6, dx, dy);
    w.add(1, dx, dy);    // right of the hero, under the panel (256 across)... still on the free half
    right_of_hero(12, dx, dy);
    w.add(2, dx, dy);    // further right: 352 across, under the panel
    right_of_hero(-4, dx, dy);
    w.add(3, dx, dy);    // left of the hero
    AutoTarget aim;
    aim.set_settings(s);
    AimPoll p;
    p.allowed = true;
    p.a_held = p.a_pressed = true;
    p.stick_engaged = true;
    p.dir_x = 1;
    p.dir_y = 0;
    p.area_left = 0;
    p.area_right = 320;
    AimResult r = run(aim, w, p);
    CHECK(r.aiming && aim.target_id() == 1);   // the angle from the hero's real place, not the screen's middle
    // The nearer one gone: the one under the panel is never taken.
    w.remove(1);
    AutoTarget aim2;
    aim2.set_settings(s);
    r = run(aim2, w, p);
    CHECK(!aim2.has_target());
    p.dir_x = -1;
    r = run(aim2, w, p);
    CHECK(r.aiming && aim2.target_id() == 3);
    // No area (no panel): the whole screen again.
    AutoTarget aim3;
    aim3.set_settings(s);
    p.dir_x = 1;
    p.area_right = 0;
    r = run(aim3, w, p);
    CHECK(r.aiming && aim3.target_id() == 2);
}

static void test_controls_loot()
{
    // ZR + Right through the controls: the gold, with the left button.
    FakeWorld w;
    w.add_item(1, 2, 2, "gld ");
    Controls c;
    c.set_frame(640, 480);
    c.set_context(true, false, 0, false);
    c.set_auto_target(AimSettings(), &w);
    c.warp_cursor(100, 100);
    auto step = [&](const Pad &p, uint64_t t) {
        c.update(p, t);
        w.last_cursor_x = c.cursor_x();
        w.last_cursor_y = c.cursor_y();
    };
    Pad p;
    p.held = kZR | kRight;
    step(p, 0);
    CHECK(c.cursor_x() == w.list[0].feet_x && !g_left);
    step(p, 40);
    CHECK(g_left);
    // Let go: the cursor back.
    step(Pad(), 200);
    step(Pad(), 240);
    CHECK(!g_left && c.cursor_x() == 100);
    // ZR + Left: kept free, nothing.
    p.held = kZR | kLeft;
    step(p, 300);
    step(p, 340);
    CHECK(!g_left && c.cursor_x() == 100);
    step(Pad(), 400);
    // ZR + Up with only gold about: nothing to pick up, so no click and
    // the cursor stays.
    p.held = kZR | kUp;
    for (uint64_t t = 500; t <= 900; t += 40) {
        step(p, t);
        CHECK(!g_left && !g_right && c.cursor_x() == 100 && c.cursor_y() == 100);
    }
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
    test_turning_keeps_and_death();
    test_lost_and_screen();
    test_held_target_not_replaced_by_nearer();
    test_town_npc_released_after_talk();
    test_disabled_reads_nothing();
    test_controls();
    test_settings();
    test_setting_rows();
    test_flick_detector();
    test_flick_same_direction();
    test_flick_light_refines();
    test_flick_strong_turns();
    test_flick_nobody_keeps();
    test_flick_one_step();
    test_flicks_enemies_only();
    test_objects_and_items_order();
    test_loot_order();
    test_loot_command();
    test_item_no_room();
    test_b_on_corpse_skill();
    test_corpse_skill_cast();
    test_controls_loot();
    test_warps();
    test_panel_area();
    if (g_fail) {
        std::printf("aim: %d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("aim: all checks passed (32 tests)\n");
    return 0;
}
