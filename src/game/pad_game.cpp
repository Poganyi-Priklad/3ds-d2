// See pad_game.h.
#include "game/pad_game.h"

#include "game/d2_units.h"
#include "game/mpq.h"
#include "game/second_screen.h"
#include "runtime/rt_host.h"

#include <memory>

namespace game {
namespace {

constexpr uint32_t kTalkingVa = 0x007BF1C0, kTalkListVa = 0x007BF250, kNpcMenuVa = 0x007C0E54;
constexpr uint32_t kMenuTitle = 0x68;
constexpr uint32_t kTalkTextVa = 0x007BF0A4;
constexpr uint32_t kFontFolderVa = 0x00841D8C;
constexpr int kMaxTalkLines = 400, kMaxChars = 256;
constexpr uint64_t kUiNpcMenu = 1ull << 0x08, kUiShop = 1ull << 0x0C, kUiTrade = 1ull << 0x17;

uint32_t rd32(uint32_t va)
{
    uint32_t v = 0;
    rt::guest_read(va, &v, 4);
    return v;
}

// A NUL-ended UTF-16 string, at most `max` characters.
std::u16string rd_wstr(uint32_t va, int max = kMaxChars)
{
    std::u16string s;
    if (!va)
        return s;
    char16_t buf[32];
    for (int done = 0; done < max; done += 32) {
        if (!rt::guest_read(va + (uint32_t)done * 2, buf, sizeof buf))
            break;
        for (char16_t c : buf) {
            if (!c || (int)s.size() >= max)
                return s;
            s += c;
        }
    }
    return s;
}

struct TalkReader {
    uint32_t id = 0;
    size_t count = 0;
    bool talking = false;
} g_talk;

}  // namespace

void read_pad_game(const GameState &gs, PadGame &g)
{
    g = PadGame();
    g.in_game = gs.known && gs.in_game;
    if (!g.in_game) {
        g_talk = TalkReader();
        return;
    }
    g.dialog = (gs.ui & kUiNpcMenu) != 0;
    g.trade = (gs.ui & (kUiShop | kUiTrade)) != 0;
    g.talking = rd32(kTalkingVa) != 0;
    g.talk_id = rd32(kTalkListVa);
    if (const uint32_t menu = rd32(kNpcMenuVa))
        g.npc = rd_wstr(menu + kMenuTitle, 40);
    // A new talk's lines: read once (again while its list is still empty).
    // A talk told again may reuse its list's address: a new talk is also
    // one that starts.
    const bool started = g.talking && !g_talk.talking;
    g_talk.talking = g.talking;
    if (g.talking && g.talk_id && (started || g.talk_id != g_talk.id || g_talk.count == 0)) {
        // The talk's text: 0x007BF0A4 -> {first line's node, line count,
        // scroll};
        // each node {text, next}. Its nodes come from the list at
        // 0x007BF250, whose first slot holds the first line only the first
        // time (a talk told again starts at another slot).
        uint32_t node = 0, count = kMaxTalkLines;
        if (const uint32_t text = rd32(kTalkTextVa)) {
            node = rd32(text);
            const uint32_t n = rd32(text + 4);
            if (n && n < (uint32_t)kMaxTalkLines)
                count = n;
        }
        if (!node) {
            const uint32_t header = rd32(g.talk_id + 8);
            node = header ? header + 0x10 : 0;
        }
        for (int i = 0; node && i < (int)count; i++) {
            uint32_t n[2] = {};
            if (!rt::guest_read(node, n, sizeof n))
                break;
            g.lines.push_back(rd_wstr(n[0]));
            node = n[1];
        }
        g_talk.id = g.talk_id;
        g_talk.count = g.lines.size();
        g.has_lines = !g.lines.empty();
    }
    // How far the talk has scrolled (its text object's +8).
    if (g.talking)
        if (const uint32_t text = rd32(kTalkTextVa)) {
            const uint32_t v = rd32(text + 8);
            g.scroll = v < 0x40000000u ? (int32_t)v : -1;
        }
    // The unit under the cursor.
    HoveredUnit h;
    if (hovered_unit(h)) {
        g.hover_key = h.type << 24 ^ h.id;
        g.hover_name = h.name;
        g.hp = h.hp;
        g.hp_max = h.hp_max;
        g.hover = h.kind == HoveredUnit::kEnemy ? PadGame::kEnemy
                  : h.kind == HoveredUnit::kNpc ? PadGame::kNpc
                  : h.kind == HoveredUnit::kObject ? PadGame::kObject
                                                   : PadGame::kNone;
    }
}

void pad_poll(SecondScreen &screen, const GameState &gs, uint64_t now_ms)
{
    if (!screen.pad_shown())
        return;
    static bool art_given = false;
    if (!art_given) {
        art_given = true;
        screen.set_pad_art(pad_art());
    }
    static PadGame g;
    read_pad_game(gs, g);
    if (g.has_lines)
        rt::logf("pad: a talk of %u lines (list %08x)", (unsigned)g.lines.size(), g.talk_id);
    screen.set_pad_game(g, now_ms);
    // What changed, for the log.
    static PadFrame last;
    static size_t last_stories = 0;
    static uint64_t hp_logged_at = 0;
    PadFrame f;
    const PadStory &story = screen.pad_story();
    story.frame(f);
    static const char *const kModes[] = {"idle", "name", "story", "enemy"};
    if (f.mode != last.mode || f.name != last.name || story.stories() != last_stories ||
        (f.mode == PadFrame::kStory && !story.wrapped().empty() && f.total != last.total && !g.talking)) {
        rt::logf("pad: %s '%s' (%s), %u stories, %d lines, top %d", kModes[f.mode], pad_utf8(f.name).c_str(),
                 f.colour == PadFrame::kRed ? "red" : f.colour == PadFrame::kWhite ? "white" : "gold",
                 (unsigned)story.stories(), f.total, f.top);
        last_stories = story.stories();
    }
    if (f.mode == PadFrame::kEnemy && (f.hp != last.hp || f.name != last.name) && now_ms - hp_logged_at >= 500) {
        rt::logf("pad: enemy life %d / %d", f.hp, f.hp_max);
        hp_logged_at = now_ms;
    } else if (f.mode == PadFrame::kEnemy && f.hp != last.hp) {
        f.hp = last.hp;   // logged at the next change after the pause
    }
    last = f;
}

const PadArt *pad_art()
{
    static std::unique_ptr<PadArt> art;
    static bool tried = false;
    if (tried)
        return art && art->ok ? art.get() : nullptr;
    tried = true;
    // The game's font folder ("CYR\"): a short string, in place or behind a pointer.
    auto folder_at = [](uint32_t va) {
        char s[16] = {};
        if (!rt::guest_read(va, s, sizeof s - 1))
            return std::string();
        std::string out;
        for (char c : s) {
            if (c == '\\' || c == '/' || !c)
                break;
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
                return std::string();
            out += c;
        }
        return out;
    };
    std::string language = folder_at(kFontFolderVa);
    if (language.empty())
        language = folder_at(rd32(kFontFolderVa));
    std::vector<std::string> archives;
    for (const char *name : {"patch_d2.mpq", "d2exp.mpq", "d2data.mpq"}) {
        std::string host;
        if (rt::host_path(name, false, host))
            archives.push_back(host);
    }
    const PadFileReader read = [&archives](const std::string &name, std::vector<uint8_t> &out) {
        for (const std::string &a : archives)
            if (mpq_read_file(a, name, out))
                return true;
        return false;
    };
    const uint64_t t0 = rt::now_ms();
    art.reset(new PadArt());
    if (pad_art_build(*art, read, language)) {
        rt::logf("pad: art from the game's files (fonts %s), %u KB, %u ms", language.empty() ? "LATIN" : language.c_str(),
                 (unsigned)(art->bytes() / 1024), (unsigned)(rt::now_ms() - t0));
        return art.get();
    }
    rt::logf("pad: no art (%s): the window is drawn plain", art->error.c_str());
    art.reset();
    return nullptr;
}

}  // namespace game
