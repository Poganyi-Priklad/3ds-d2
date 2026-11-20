// See d2_units.h.
#include "game/d2_units.h"

#include "runtime/rt_host.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <strings.h>

namespace game {
namespace {

constexpr uint32_t kPlayerUnitVa = 0x007A6A70;
constexpr uint32_t kUnitTablesVa = 0x007A5E70;
constexpr uint32_t kBuckets = 128;
constexpr uint32_t kMonster = 1, kObject = 2;
constexpr uint32_t kListNext = 0xE4;
constexpr uint32_t kPath = 0x2C;
// The player's body on the screen (the view's centre, above the feet), and
// how far above its feet a unit's body is.
constexpr int kCentreX = 320, kCentreY = 197;
constexpr int kNear = 40;   // subtiles: the units logged

// Names for the NPCs a script talks to, as the lane showed them (a class
// not here is given by its number).
const struct { const char *name; uint32_t cls; } kNpcs[] = {
    {"warriv", 155},   // Act I, the Rogue Encampment
};

uint32_t rd32(uint32_t va)
{
    uint32_t v = 0;
    rt::guest_read(va, &v, 4);
    return v;
}

// A unit's place in subtiles, with the fraction. An object (types 2, 4, 5)
// has a static path: +0x0C x, +0x10 y (0x620b11).
bool place(uint32_t unit, double &x, double &y)
{
    const uint32_t path = rd32(unit + kPath);
    if (!path)
        return false;
    const uint32_t type = rd32(unit);
    if (type == 2 || type == 4 || type == 5) {
        x = (int32_t)rd32(path + 0x0C);
        y = (int32_t)rd32(path + 0x10);
        return true;
    }
    uint16_t p[4] = {};
    if (!rt::guest_read(path, p, sizeof p))
        return false;
    x = p[1] + p[0] / 65536.0;
    y = p[3] + p[2] / 65536.0;
    return true;
}

// Each unit of a type (1 monsters and NPCs, 2 objects) in the client's
// table, with its offset from the player.
template <class F> bool each_unit(uint32_t type, F f)
{
    const uint32_t player = rd32(kPlayerUnitVa);
    double px, py;
    if (!player || !place(player, px, py))
        return false;
    for (uint32_t b = 0; b < kBuckets; b++) {
        uint32_t u = rd32(kUnitTablesVa + (type * kBuckets + b) * 4);
        for (int n = 0; u && n < 64; n++, u = rd32(u + kListNext)) {
            double x, y;
            if (rd32(u) != type || !place(u, x, y))
                continue;
            UnitSpot s;
            s.unit = u;
            s.cls = rd32(u + 0x04);
            s.id = rd32(u + 0x0C);
            s.mode = rd32(u + 0x10);
            const double dx = x - px, dy = y - py;
            s.x = kCentreX + (int)std::lround((dx - dy) * 16);
            s.y = kCentreY + (int)std::lround((dx + dy) * 8);
            s.distance = (float)std::sqrt(dx * dx + dy * dy);
            f(s);
        }
    }
    return true;
}

}  // namespace

// The level's preset units, loaded or not: the player's room (its path's
// +0x1c) has its level's room (+0x10), whose level (+0x58) lists them all
// (+0x10, next +0x24); a room's place in tiles (+0x34, +0x38) and its
// presets (+0x5c, next +0xc: +4 class, +8 x, +0x14 type, +0x18 y, in
// subtiles from the room's corner). f(type, class, x, y), places in subtiles.
template <class F> bool each_preset(F f)
{
    const uint32_t player = rd32(kPlayerUnitVa);
    const uint32_t path = player ? rd32(player + kPath) : 0;
    const uint32_t room = path ? rd32(path + 0x1c) : 0;
    const uint32_t room2 = room ? rd32(room + 0x10) : 0;
    const uint32_t level = room2 ? rd32(room2 + 0x58) : 0;
    if (!level)
        return false;
    int rooms = 0;
    for (uint32_t r = rd32(level + 0x10); r && rooms < 4096; r = rd32(r + 0x24), rooms++) {
        if (!rooms)
            rt::logf("units: level %08x (no. %u), first room %08x", level, rd32(level + 0x1d0), r);
        const int32_t rx = (int32_t)rd32(r + 0x34) * 5, ry = (int32_t)rd32(r + 0x38) * 5;
        int n = 0;
        for (uint32_t p = rd32(r + 0x5c); p && n < 512; p = rd32(p + 0xc), n++)
            f(rd32(p + 0x14), rd32(p + 4), rx + (int32_t)rd32(p + 8), ry + (int32_t)rd32(p + 0x18));
    }
    rt::logf("units: %d rooms in the level", rooms);
    return true;
}

std::string object_name(uint32_t unit);   // below, by log_units

bool find_object_named(const char *name, UnitSpot &out)
{
    bool found = false;
    each_unit(kObject, [&](const UnitSpot &s) {
        if (strcasecmp(object_name(s.unit).c_str(), name) == 0 && (!found || s.distance < out.distance)) {
            out = s;
            found = true;
        }
    });
    return found;
}

bool frontier_room(const std::vector<uint32_t> &skip, uint32_t &room, double &dx, double &dy, double &distance)
{
    const uint32_t player = rd32(kPlayerUnitVa);
    double px, py;
    const uint32_t path = player ? rd32(player + kPath) : 0;
    const uint32_t r1 = path ? rd32(path + 0x1c) : 0;
    const uint32_t r2 = r1 ? rd32(r1 + 0x10) : 0;
    const uint32_t level = r2 ? rd32(r2 + 0x58) : 0;
    if (!level || !place(player, px, py))
        return false;
    bool found = false;
    int n = 0;
    for (uint32_t r = rd32(level + 0x10); r && n < 4096; r = rd32(r + 0x24), n++) {
        if (rd32(r + 0x30) || std::find(skip.begin(), skip.end(), r) != skip.end())
            continue;   // loaded (its room is there), or given up on
        const double cx = ((int32_t)rd32(r + 0x34) + (int32_t)rd32(r + 0x3c) / 2.0) * 5;
        const double cy = ((int32_t)rd32(r + 0x38) + (int32_t)rd32(r + 0x40) / 2.0) * 5;
        const double ex = cx - px, ey = cy - py, d = std::sqrt(ex * ex + ey * ey);
        if (!found || d < distance) {
            room = r;
            dx = ex;
            dy = ey;
            distance = d;
            found = true;
        }
    }
    return found;
}

bool find_preset(uint32_t cls, uint32_t type, double &dx, double &dy, double &distance)
{
    const uint32_t player = rd32(kPlayerUnitVa);
    double px, py;
    if (!player || !place(player, px, py))
        return false;
    bool found = false;
    each_preset([&](uint32_t t, uint32_t c, int32_t x, int32_t y) {
        if (t != type || c != cls)
            return;
        const double ex = x - px, ey = y - py, d = std::sqrt(ex * ex + ey * ey);
        if (!found || d < distance) {
            dx = ex;
            dy = ey;
            distance = d;
            found = true;
        }
    });
    return found;
}

bool find_unit(uint32_t cls, UnitSpot &out, uint32_t type)
{
    bool found = false;
    each_unit(type, [&](const UnitSpot &s) {
        if (s.cls == cls && (!found || s.distance < out.distance)) {
            out = s;
            found = true;
        }
    });
    return found;
}

bool holding_item()
{
    // The player's inventory (+0x60; its first dword the 0x01020304
    // signature) holds the item on the cursor at +0x20.
    const uint32_t player = rd32(kPlayerUnitVa);
    const uint32_t inv = player ? rd32(player + 0x60) : 0;
    return inv && rd32(inv) == 0x01020304u && rd32(inv + 0x20) != 0;
}

int player_level()
{
    const uint32_t player = rd32(kPlayerUnitVa);
    const uint32_t path = player ? rd32(player + kPath) : 0;
    const uint32_t room = path ? rd32(path + 0x1c) : 0;
    const uint32_t room2 = room ? rd32(room + 0x10) : 0;
    const uint32_t level = room2 ? rd32(room2 + 0x58) : 0;
    return level ? (int)rd32(level + 0x1d0) : 0;
}

int player_act()
{
    const uint32_t player = rd32(kPlayerUnitVa);
    return player ? (int)rd32(player + 0x18) + 1 : 0;
}

void npc_menu_line(const UnitSpot &npc, int line, int &x, int &y)
{
    x = npc.x - 12;
    y = npc.y - 138 + 15 * line;
}

uint32_t unit_class(const char *name)
{
    for (const auto &n : kNpcs)
        if (strcasecmp(name, n.name) == 0)
            return n.cls;
    char *end = nullptr;
    const unsigned long v = std::strtoul(name, &end, 10);
    return end != name && !*end ? (uint32_t)v : 0;
}

// An object's name, from its objects.txt record (the unit's data, +0x14,
// starts with a pointer to it; the record starts with the name), or "".
std::string object_name(uint32_t unit)
{
    char name[33] = {};
    const uint32_t data = rd32(unit + 0x14), txt = data ? rd32(data) : 0;
    if (!txt || !rt::guest_read(txt, name, sizeof name - 1))
        return "";
    for (char &ch : name)
        if (ch && (ch < 0x20 || ch > 0x7e))
            return "";
    return name;
}

void log_units(bool all)
{
    int n = 0;
    const bool ok = each_unit(kMonster, [&](const UnitSpot &s) {
        if (all || s.distance <= kNear) {
            rt::logf("units: class %u id %u mode %u, %.1f subtiles away, at %d,%d", s.cls, s.id, s.mode, s.distance,
                     s.x, s.y);
            n++;
        }
    });
    each_unit(kObject, [&](const UnitSpot &s) {
        if (all || s.distance <= kNear)
            rt::logf("units: object class %u id %u mode %u \"%s\", %.1f subtiles away, at %d,%d", s.cls, s.id, s.mode,
                     object_name(s.unit).c_str(), s.distance, s.x, s.y);
    });
    if (all) {   // and the level's preset objects (type 2), loaded or not
        double px = 0, py = 0;
        const uint32_t player = rd32(kPlayerUnitVa);
        if (player)
            place(player, px, py);
        int counts[8] = {};
        each_preset([&](uint32_t t, uint32_t c, int32_t x, int32_t y) {
            counts[t & 7]++;
            if (t == 2 || t == 1)
                rt::logf("units: preset %s class %u at %d,%d, %.1f subtiles away", t == 2 ? "object" : "monster", c,
                         x, y, std::sqrt((x - px) * (x - px) + (y - py) * (y - py)));
        });
        rt::logf("units: presets by type: %d %d %d %d %d %d", counts[0], counts[1], counts[2], counts[3], counts[4],
                 counts[5]);
    }
    rt::logf(ok ? "units: %d near the player" : "units: not in a game", n);
}

// ---- the auto-target's reads (d2_units.h, auto_target.h) ----
//
// GameAimWorld is the only place the auto-target touches the game: it reads
// guest memory (never writes it, never calls a game function) and turns what
// it finds into AimUnit and AimView records. Everything else in the
// auto-target works on those records, which is what lets tests/aim run the
// same rules on made-up units. The addresses below are Diablo II 1.14d's.

namespace {

constexpr uint32_t kCameraXVa = 0x007A520C, kCameraXShiftVa = 0x007A5214, kCameraYVa = 0x007A5208;
constexpr uint32_t kFrameWidthVa = 0x0071146C, kFrameHeightVa = 0x00711470;
constexpr uint32_t kHoverFlagVa = 0x007A6A94, kHoverTypeVa = 0x007A6A8C, kHoverIdVa = 0x007A6A78;
constexpr uint32_t kPetListVa = 0x007BB5BC;
constexpr uint32_t kDataTablesVa = 0x00744304;   // the loaded .txt tables
constexpr float kAimReadSubtiles = 40.0f;          // units further away are never targets
constexpr uint32_t kItem = 4;
constexpr uint32_t kWarp = 5;   // a level's warp ("tile" units): cave entrances, stairs, passages
// Items.txt (weapons, armour and misc in one table): the rows and their
// count, 0x1A8 bytes a row, the four-character code at +0x80.
constexpr uint32_t kItemRowsVa = 0x0096CA5C, kItemCountVa = 0x0096CA58;
constexpr uint32_t kItemRowSize = 0x1A8, kItemCode = 0x80;
// objects.txt's record (the object's data +0x14 points at a pointer to
// it): OperateFn at +0x1B3.
constexpr uint32_t kObjectOperateFn = 0x1B3;
// The player's skills (unit +0xA8): the right button's at +0x0C, a node
// whose +0x00 points at its skills.txt row; the row starts with the id, and
// its flags at +0x04 have TargetCorpse in bit 24.
constexpr uint32_t kSkillInfo = 0xA8, kRightSkill = 0x0C, kSkillFlags = 0x04;
constexpr uint32_t kSkillTargetCorpse = 1u << 24;
// One subtile on the screen: half a floor tile's diamond (32 by 16 pixels).
constexpr int kSubtileHalfWidthPx = 16, kSubtileHalfHeightPx = 8;

uint64_t (*g_aim_clock)() = rt::now_us;
AimReadCost g_aim_cost;

// The camera's top-left corner in world pixels: a unit's feet on the screen
// are its world pixel minus these.
int camera_x() { return (int)(rd32(kCameraXVa) - rd32(kCameraXShiftVa)); }
int camera_y() { return (int)rd32(kCameraYVa) - 8; }

// The five towns' level ids (Rogue Encampment, Lut Gholein, Kurast Docks,
// the Pandemonium Fortress, Harrogath): in town only NPCs, objects and
// items are targets.
bool is_town(int level) { return level == 1 || level == 40 || level == 75 || level == 103 || level == 109; }

// A monster's flags from MonStats.txt and MonStats2.txt, by its class,
// read once per class (the tables don't change during a game).
class MonsterFlags {
public:
    void get(uint32_t cls, uint32_t &monstats, uint32_t &monstats2)
    {
        const uint32_t tables = rd32(kDataTablesVa);
        if (tables != tables_) {
            tables_ = tables;
            known_.clear();
        }
        monstats = monstats2 = 0;
        if (!tables || cls >= 4096)
            return;
        if (known_.size() <= cls)
            known_.resize(cls + 1, Flags{});
        Flags &f = known_[cls];
        if (!f.read) {
            f.read = true;
            if (cls < rd32(tables + 0xA80)) {
                const uint32_t row = rd32(tables + 0xA78) + cls * 0x1A8;
                f.monstats = rd32(row + 0x0C);
                uint16_t row2 = 0;
                rt::guest_read(row + 0x18, &row2, 2);
                if ((int16_t)row2 >= 0 && row2 < rd32(tables + 0xA98))
                    f.monstats2 = rd32(rd32(tables + 0xA90) + row2 * 0x134u + 4);
            }
        }
        monstats = f.monstats;
        monstats2 = f.monstats2;
    }

private:
    struct Flags {
        bool read = false;
        uint32_t monstats = 0, monstats2 = 0;
    };
    uint32_t tables_ = 0;
    std::vector<Flags> known_;
};

// The AimWorld the console and the Linux lane use (tests/aim has its own,
// made-up one). Each list read walks the game's unit hash tables (128
// buckets per unit type), keeps only units within kAimReadSubtiles of the
// hero, and times itself so game.log's "aim: stats:" line can show what the
// reads cost.
class GameAimWorld : public AimWorld {
public:
    AimReadCost read_cost() const override { return g_aim_cost; }
    // The hero, the frame's size, the town flag, the hover (what the game
    // shows under the cursor now) and the right button's skill.
    bool view(AimView &out) override
    {
        const uint32_t player = rd32(kPlayerUnitVa);
        int feet_x, feet_y;
        if (!player || !feet(player, feet_x, feet_y) || !place(player, player_x_, player_y_))
            return false;
        out.hero_x = feet_x;
        out.hero_y = feet_y;
        const int width = (int)rd32(kFrameWidthVa), height = (int)rd32(kFrameHeightVa);
        out.width = width >= 320 && width <= 2048 ? width : 640;
        out.height = height >= 240 && height <= 2048 ? height : 480;
        out.in_town = is_town(player_level());
        out.hero_mode = rd32(player + 0x10);
        out.hover_on = rd32(kHoverFlagVa) != 0;
        out.hover_type = rd32(kHoverTypeVa);
        out.hover_id = rd32(kHoverIdVa);
        const uint32_t skills = rd32(player + kSkillInfo), right = skills ? rd32(skills + kRightSkill) : 0;
        const uint32_t row = right ? rd32(right) : 0;
        out.right_skill = row ? rd32(row) : 0;
        out.right_skill_on_corpses = row && (rd32(row + kSkillFlags) & kSkillTargetCorpse);
        return true;
    }

    // Every object, item and warp near the hero (read only when plain A or
    // a loot command may use them).
    void ground_units(std::vector<AimUnit> &out) override
    {
        const uint64_t start = g_aim_clock();
        const uint32_t player = rd32(kPlayerUnitVa);
        if (!player || !place(player, player_x_, player_y_) || !feet(player, hero_x_, hero_y_))
            return;
        uint32_t read = 0;
        for (uint32_t type : {kObject, kItem, kWarp}) {
            uint32_t heads[kBuckets] = {};
            rt::guest_read(kUnitTablesVa + type * kBuckets * 4, heads, sizeof heads);
            for (uint32_t b = 0; b < kBuckets; b++) {
                uint32_t u = heads[b];
                for (int n = 0; u && n < 64; n++) {
                    AimUnit unit;
                    uint32_t next = 0;
                    read++;
                    if (read_ground_unit(u, type, unit, next) && unit.distance <= kAimReadSubtiles)
                        out.push_back(unit);
                    u = next;
                }
            }
        }
        const uint64_t took = g_aim_clock() - start;
        g_aim_cost.units += read;
        g_aim_cost.total_us += took;
        g_aim_cost.longest_us = took > g_aim_cost.longest_us ? took : g_aim_cost.longest_us;
    }

    // One object, item or warp again, by id (its bucket only).
    bool ground_unit(uint32_t type, uint32_t id, AimUnit &out) override
    {
        const uint32_t player = rd32(kPlayerUnitVa);
        if ((type != kObject && type != kItem && type != kWarp) || !player || !place(player, player_x_, player_y_) ||
            !feet(player, hero_x_, hero_y_))
            return false;
        uint32_t u = rd32(kUnitTablesVa + (type * kBuckets + (id & (kBuckets - 1))) * 4);
        for (int n = 0; u && n < 64; n++) {
            uint32_t next = 0;
            if (rd32(u + 0x0C) == id)
                return read_ground_unit(u, type, out, next);
            u = rd32(u + kListNext);
        }
        return false;
    }

    // Every monster and NPC near the hero, with the pet list read first so
    // the mercenary and summons are marked as owned.
    void units(std::vector<AimUnit> &out) override
    {
        const uint64_t start = g_aim_clock();
        const uint32_t player = rd32(kPlayerUnitVa);
        if (!player || !place(player, player_x_, player_y_))
            return;
        read_pets();
        uint32_t heads[kBuckets] = {};
        rt::guest_read(kUnitTablesVa + kMonster * kBuckets * 4, heads, sizeof heads);
        uint32_t read = 0;
        for (uint32_t b = 0; b < kBuckets; b++) {
            uint32_t u = heads[b];
            for (int n = 0; u && n < 64; n++) {
                AimUnit unit;
                uint32_t next = 0;
                read++;
                if (read_unit(u, unit, next) && unit.distance <= kAimReadSubtiles)
                    out.push_back(unit);
                u = next;
            }
        }
        const uint64_t took = g_aim_clock() - start;
        g_aim_cost.scans++;
        g_aim_cost.units += read;
        g_aim_cost.total_us += took;
        g_aim_cost.longest_us = took > g_aim_cost.longest_us ? took : g_aim_cost.longest_us;
    }

    // One monster or NPC again, by id: the current target, every poll
    // while a button is held.
    bool unit(uint32_t id, AimUnit &out) override
    {
        const uint32_t player = rd32(kPlayerUnitVa);
        if (!player || !place(player, player_x_, player_y_))
            return false;
        uint32_t u = rd32(kUnitTablesVa + (kMonster * kBuckets + (id & (kBuckets - 1))) * 4);
        for (int n = 0; u && n < 64; n++) {
            uint32_t next = 0;
            if (rd32(u + 0x0C) == id) {
                read_pets();
                return read_unit(u, out, next);
            }
            u = rd32(u + kListNext);
        }
        return false;
    }

private:
    // A unit's feet on the screen: its world pixel (path +0x08, +0x0C) minus the camera.
    bool feet(uint32_t unit, int &x, int &y)
    {
        const uint32_t path = rd32(unit + kPath);
        int32_t pixel[2] = {};
        if (!path || !rt::guest_read(path + 0x08, pixel, sizeof pixel))
            return false;
        x = pixel[0] - camera_x();
        y = pixel[1] - camera_y();
        return true;
    }

    // One unit's fields in one read (its first 0xE8 bytes), and the next in its bucket.
    bool read_unit(uint32_t u, AimUnit &out, uint32_t &next)
    {
        uint32_t fields[0xE8 / 4] = {};
        if (!rt::guest_read(u, fields, sizeof fields))
            return false;
        next = fields[kListNext / 4];
        if (fields[0] != kMonster)
            return false;
        out.cls = fields[0x04 / 4];
        out.id = fields[0x0C / 4];
        out.mode = fields[0x10 / 4];
        out.flags_a = fields[0xC4 / 4];
        out.flags_b = fields[0xC8 / 4];
        const uint32_t path = fields[kPath / 4];
        uint16_t place16[4] = {};
        int32_t pixel[2] = {};
        if (!path || !rt::guest_read(path, place16, sizeof place16) || !rt::guest_read(path + 0x08, pixel, sizeof pixel))
            return false;
        const double dx = place16[1] + place16[0] / 65536.0 - player_x_;
        const double dy = place16[3] + place16[2] / 65536.0 - player_y_;
        out.distance = (float)std::sqrt(dx * dx + dy * dy);
        out.feet_x = pixel[0] - camera_x_;
        out.feet_y = pixel[1] - camera_y_;
        flags_.get(out.cls, out.monstats_flags, out.monstats2_flags);
        out.owned = std::find(pets_.begin(), pets_.end(), out.id) != pets_.end();
        return true;
    }

    // An object's, an item's or a warp's fields, and the next in its bucket.
    // Its place is a whole subtile (the static path); its feet on the screen
    // are the hero's plus the isometric offset. A warp has no data record
    // (+0x14 is 0): only its place and flags are used.
    bool read_ground_unit(uint32_t u, uint32_t type, AimUnit &out, uint32_t &next)
    {
        uint32_t fields[0xE8 / 4] = {};
        if (!rt::guest_read(u, fields, sizeof fields))
            return false;
        next = fields[kListNext / 4];
        const uint32_t path = fields[kPath / 4];
        if (fields[0] != type || !path)
            return false;
        out.type = type;
        out.cls = fields[0x04 / 4];
        out.id = fields[0x0C / 4];
        out.mode = fields[0x10 / 4];
        out.flags_a = fields[0xC4 / 4];
        out.flags_b = fields[0xC8 / 4];
        int32_t spot[2] = {};
        if (!rt::guest_read(path + 0x0C, spot, sizeof spot))
            return false;
        const double dx = spot[0] - player_x_, dy = spot[1] - player_y_;
        out.distance = (float)std::sqrt(dx * dx + dy * dy);
        out.feet_x = hero_x_ + (int)std::lround((dx - dy) * kSubtileHalfWidthPx);
        out.feet_y = hero_y_ + (int)std::lround((dx + dy) * kSubtileHalfHeightPx);
        const uint32_t data = fields[0x14 / 4];
        if (type == kObject) {
            const uint32_t txt = data ? rd32(data) : 0;
            uint8_t operate_fn = 0;
            if (txt)
                rt::guest_read(txt + kObjectOperateFn, &operate_fn, 1);
            out.operate_fn = operate_fn;
        } else if (type == kItem) {
            out.quality = data ? rd32(data) : 0;
            const uint32_t rows = rd32(kItemRowsVa);
            out.code = rows && out.cls < rd32(kItemCountVa) ? rd32(rows + out.cls * kItemRowSize + kItemCode) : 0;
        }
        return true;
    }

    // The client's pet list (the mercenary, summons): their unit ids. Read
    // once per poll at most.
    void read_pets()
    {
        camera_x_ = camera_x();
        camera_y_ = camera_y();
        pets_.clear();
        for (uint32_t node = rd32(kPetListVa), n = 0; node && n < 256; node = rd32(node + 0x30), n++)
            pets_.push_back(rd32(node + 0x08));
    }

    double player_x_ = 0, player_y_ = 0;
    int hero_x_ = 0, hero_y_ = 0;   // the hero's feet on the screen (ground units)
    int camera_x_ = 0, camera_y_ = 0;
    std::vector<uint32_t> pets_;
    MonsterFlags flags_;
};

}  // namespace

// The one GameAimWorld (Controls keeps a pointer to it).
AimWorld &game_aim_world()
{
    static GameAimWorld world;
    return world;
}

void set_aim_clock(uint64_t (*clock_us)()) { g_aim_clock = clock_us ? clock_us : rt::now_us; }

}  // namespace game
