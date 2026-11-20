// The PAD window: its story (src/game/pad_story.h: names, talks revealed at
// the game's pace, kept, appended, cleared), the PAD tab in the bottom
// screen (src/game/second_screen.h: in and out, no PANEL switching, the
// close button, the scroll column, the touchpad elsewhere), its art's
// decoders (src/game/pad_art.h, src/game/mpq.h, against the player's own
// files when a game folder is given) and the SCREEN switches' defaults
// (src/game/screen_auto.h, both kinds of build). Under qemu's ARM11 MPCore
// model; run through `scripts/docker.sh test padwin`.
#include "game/mpq.h"
#include "game/pad_art.h"
#include "game/pad_story.h"
#include "game/screen_auto.h"
#include "game/second_screen.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The automap and the runtime are not under test (the keyboard, osk.cpp,
// types through the runtime).
namespace rt {
void logf(const char *, ...) {}
void input_char(unsigned int) {}
void input_key(unsigned char, bool) {}
}  // namespace rt
namespace game {
bool automap_draw(uint8_t *, int, int, int) { return false; }
uint64_t automap_state(int) { return 0; }
}  // namespace game

using namespace game;

static int g_failed = 0, g_checks = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        g_checks++;                                                               \
        if (!(c)) {                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);              \
            g_failed++;                                                           \
        }                                                                         \
    } while (0)

static std::u16string U(const char *s) { return pad_utf16(s); }

constexpr size_t kTestLine = 32;
// A story's lines as the game wraps them: three paragraphs.
static std::vector<std::u16string> talk(const char *who, int paragraphs = 3, int lines_each = 3)
{
    std::vector<std::u16string> out;
    for (int p = 0; p < paragraphs; p++) {
        if (p)
            out.push_back(u"");
        for (int l = 0; l < lines_each; l++) {
            // 32 characters, as wide as the test's window: wrapped again,
            // each stays a line of its own.
            std::u16string line = U(who) + u" says line " + (char16_t)(u'a' + p) + (char16_t)(u'0' + l % 10) + u" of the tale";
            line.resize(kTestLine, u'.');
            out.push_back(line);
        }
    }
    return out;
}

// One poll at `t` ms with the dialog open.
struct Game {
    PadStory story{nullptr, 6 * (int)kTestLine};   // 6 px a character (no measure given), a line of 32
    PadGame g;
    uint32_t next_id = 0x1000;
    Game()
    {
        g.in_game = true;
    }
    void open(const char *npc)
    {
        g.dialog = true;
        g.npc = U(npc);
    }
    void start_talk(uint64_t t, std::vector<std::u16string> lines)
    {
        g.talking = true;
        g.npc.clear();   // the menu box is gone while the talk shows
        g.talk_id = next_id += 0x40;
        g.has_lines = true;
        g.lines = std::move(lines);
        story.step(g, t);
        g.has_lines = false;
        g.lines.clear();
    }
    void step(uint64_t t) { story.step(g, t); }
};

static void test_names()
{
    Game G;
    G.step(0);
    CHECK(G.story.mode() == PadFrame::kIdle);
    // An NPC under the cursor: gold; an object: white; an enemy: red with its life.
    G.g.hover = PadGame::kNpc;
    G.g.hover_name = U("Kashya");
    G.step(10);
    PadFrame f;
    G.story.frame(f);
    CHECK(f.mode == PadFrame::kName && f.colour == PadFrame::kGold && f.name == U("Kashya"));
    G.g.hover = PadGame::kObject;
    G.g.hover_name = U("Gate");
    G.step(20);
    G.story.frame(f);
    CHECK(f.mode == PadFrame::kName && f.colour == PadFrame::kWhite && f.name == U("Gate"));
    G.g.hover = PadGame::kEnemy;
    G.g.hover_name = U("\u00ffc1Fallen");   // a colour code is not shown
    G.g.hp = 32768;
    G.g.hp_max = 32768;
    G.step(30);
    G.story.frame(f);
    CHECK(f.mode == PadFrame::kEnemy && f.colour == PadFrame::kRed && f.name == U("Fallen") && f.hp == 32768);
    G.g.hp = 12000;
    G.step(40);
    G.story.frame(f);
    CHECK(f.hp == 12000 && f.hp_max == 32768);
    // The cursor leaves: the name stays a moment, then idle.
    G.g.hover = PadGame::kNone;
    G.g.hover_name.clear();
    G.step(40 + PadStory::kHoverLingerMs - 1);
    CHECK(G.story.mode() == PadFrame::kEnemy);
    G.step(40 + PadStory::kHoverLingerMs + 1);
    CHECK(G.story.mode() == PadFrame::kIdle);
    // The dialog's NPC: gold, its name only until a talk.
    G.open("Kashya");
    G.step(2000);
    G.story.frame(f);
    CHECK(f.mode == PadFrame::kName && f.colour == PadFrame::kGold && f.name == U("Kashya"));
}

static void test_story_reveal_and_keep()
{
    Game G;
    G.open("Warriv");
    G.step(0);
    const std::vector<std::u16string> lines = talk("Warriv");   // 11 lines: 9 text, 2 blank
    G.start_talk(1000, lines);
    CHECK(G.story.stories() == 1);
    CHECK(G.story.total() == 0);   // nothing scrolled into the game's box yet
    G.step(1000 + PadStory::kFirstMs);
    CHECK(G.story.total() == 1);
    G.step(1000 + PadStory::kFirstMs + 2 * PadStory::kLineMs);
    CHECK(G.story.total() == 3);
    G.step(1000 + PadStory::kFirstMs + 4 * PadStory::kLineMs);   // the blank line and the next paragraph's first
    CHECK(G.story.total() == 5 && G.story.wrapped()[3].empty());
    PadFrame f;
    G.story.frame(f);
    CHECK(f.mode == PadFrame::kStory && f.name == U("Warriv"));   // the name kept from the menu
    // The talk ends: everything shows, and stays while the menu comes back.
    G.g.talking = false;
    G.step(20000);
    CHECK(G.story.total() == 11);
    G.g.npc = U("Warriv");
    for (uint64_t t = 20000; t < 30000; t += 500)
        G.step(t);
    CHECK(G.story.total() == 11 && G.story.mode() == PadFrame::kStory);
    // The transition gap: the menu struct gone for a moment, the dialog bit on.
    G.g.npc.clear();
    G.step(30100);
    CHECK(G.story.total() == 11);
    // Closing the dialog clears it (once it has been gone kDialogGraceMs).
    G.g.dialog = false;
    G.step(31000);
    CHECK(G.story.total() == 11);
    G.step(31000 + PadStory::kDialogGraceMs);
    CHECK(G.story.total() == 0 && G.story.stories() == 0 && G.story.mode() == PadFrame::kIdle);
}

// An NPC's first words come before its menu (the menu bit off while they
// scroll), and the menu's title may hold the chosen line for a moment.
static void test_talk_before_menu()
{
    Game G;
    G.g.hover = PadGame::kNpc;
    G.g.hover_name = U("Kashya");
    G.step(0);
    G.g.hover = PadGame::kNone;
    G.g.hover_name.clear();
    G.start_talk(100, talk("W"));   // the menu bit still off
    G.step(100 + PadStory::kFirstMs + PadStory::kLineMs);
    PadFrame f;
    G.story.frame(f);
    CHECK(G.story.stories() == 1 && f.mode == PadFrame::kStory && f.total == 2 && f.name == U("Kashya"));
    // A short gap as the talk ends, then the menu.
    G.g.talking = false;
    G.step(30000);
    G.step(30000 + PadStory::kDialogGraceMs - 1);
    CHECK(G.story.stories() == 1);
    G.open("a line");   // its title not the NPC's yet
    G.step(30500);
    G.story.frame(f);
    CHECK(G.story.stories() == 1 && f.name == U("Kashya"));
    // The same talk again (its list at the same address): a second story.
    G.next_id -= 0x40;
    G.start_talk(31000, talk("W"));
    G.g.talking = false;
    G.step(60000);
    CHECK(G.story.stories() == 2);
    // A shop: cleared at once.
    G.g.trade = true;
    G.step(60010);
    CHECK(G.story.stories() == 0 && G.story.total() == 0);
}

// A talk revealed by the game's own scroll (its pace follows the speech,
// slower for some NPCs than the clock's pace).
static void test_reveal_by_scroll()
{
    Game G;
    G.open("Akara");
    G.step(0);
    G.g.scroll = 0;
    G.start_talk(100, talk("A"));
    G.step(100 + 10 * PadStory::kLineMs);    // the clock alone would show 10
    CHECK(G.story.total() == 1);
    G.g.scroll = PadStory::kFirstScroll + PadStory::kLineScroll - 1;
    G.step(30000);
    CHECK(G.story.total() == 1);
    G.g.scroll = PadStory::kFirstScroll + 2 * PadStory::kLineScroll;
    G.step(31000);
    CHECK(G.story.total() == 3);
    // The talk ends: all of it.
    G.g.talking = false;
    G.g.scroll = -1;
    G.step(32000);
    CHECK(G.story.total() == (int)talk("A").size());
}

static void test_many_stories()
{
    Game G;
    G.open("Warriv");
    G.step(0);
    uint64_t t = 1000;
    for (int k = 1; k <= 6; k++) {
        char who[16];
        std::snprintf(who, sizeof who, "S%d", k);
        G.start_talk(t, talk(who, 2, 2));   // 5 lines: 4 text, 1 blank
        G.g.talking = false;
        G.step(t + 500);                     // ended early (skipped): all of it shows
        G.g.npc = U("Warriv");
        G.step(t + 600);
        CHECK(G.story.stories() == (size_t)k);
        // Each story: 5 lines; between two: 2 blank lines.
        CHECK(G.story.total() == k * 5 + (k - 1) * 2);
        t += 10000;
    }
    // Every story is there in order, two blank lines between each.
    const auto &w = G.story.wrapped();
    for (int k = 1; k <= 6; k++) {
        const int first = (k - 1) * 7;
        char who[16];
        std::snprintf(who, sizeof who, "S%d", k);
        CHECK(w[first].rfind(U(who), 0) == 0);
        if (k > 1)
            CHECK(w[first - 1].empty() && w[first - 2].empty() && !w[first - 3].empty());
    }
    // A shop opening clears them (and they don't come back after it).
    G.g.trade = true;
    G.step(t);
    CHECK(G.story.stories() == 0 && G.story.total() == 0);
    G.g.trade = false;
    G.step(t + 100);
    CHECK(G.story.total() == 0 && G.story.mode() == PadFrame::kName);
}

static void test_memory_cap()
{
    Game G;
    G.open("Cain");
    G.step(0);
    // Stories of 101 lines each: past kMaxGameLines the oldest go, the newest stays.
    const int each = 101, n = (int)(PadStory::kMaxGameLines / each) + 3;
    uint64_t t = 1000;
    for (int k = 0; k < n; k++) {
        G.start_talk(t, talk("C", 1, each));
        G.g.talking = false;
        G.step(t + 100);
        t += 1000;
    }
    size_t lines = 0;
    CHECK(G.story.stories() < (size_t)n && G.story.stories() >= 2);
    lines = G.story.stories() * each;
    CHECK(lines <= PadStory::kMaxGameLines);
    // A single story longer than the cap is kept whole.
    Game H;
    H.open("Cain");
    H.step(0);
    H.start_talk(10, talk("D", 1, (int)PadStory::kMaxGameLines + 50));
    H.g.talking = false;
    H.step(20);
    CHECK(H.story.stories() == 1 && H.story.total() == (int)PadStory::kMaxGameLines + 50);
}

static void test_scroll()
{
    Game G;
    G.open("Akara");
    G.step(0);
    G.start_talk(10, talk("A", 3, 6));   // 20 lines
    G.g.talking = false;
    G.step(20);
    CHECK(G.story.total() == 20);
    CHECK(G.story.top() == 20 - pad_layout::kRows);   // follows the newest
    G.story.scroll_by(-100);
    CHECK(G.story.top() == 0);
    G.story.scroll_by(-1);
    CHECK(G.story.top() == 0);
    G.story.scroll_by(3);
    CHECK(G.story.top() == 3);
    G.story.scroll_by(100);
    CHECK(G.story.top() == G.story.max_top() && G.story.max_top() == 11);
    G.story.scroll_to_fraction(0.5f);
    CHECK(G.story.top() == 6);
    G.story.scroll_to_fraction(-3.0f);
    CHECK(G.story.top() == 0);
    G.story.scroll_to_fraction(7.0f);
    CHECK(G.story.top() == 11);
    // Scrolled up: a new story doesn't pull the view down...
    G.story.scroll_by(-5);
    G.start_talk(100, talk("B", 1, 2));
    G.g.talking = false;
    G.step(200);
    CHECK(G.story.top() == 6);
    // ...at the bottom, it does.
    G.story.scroll_by(1000);
    G.start_talk(300, talk("C", 1, 2));
    G.g.talking = false;
    G.step(400);
    CHECK(G.story.top() == G.story.max_top());
    // Fewer lines than rows: nothing to scroll.
    Game H;
    H.open("Gheed");
    H.step(0);
    H.start_talk(10, talk("G", 1, 3));
    H.g.talking = false;
    H.step(20);
    H.story.scroll_by(5);
    CHECK(H.story.top() == 0 && H.story.max_top() == 0);
    // The thumb stays on the track.
    for (int top = -2; top < 40; top++) {
        int y = 0, h = 0;
        pad_layout::thumb(top, 30, y, h);
        CHECK(y >= pad_layout::kTrackY0 && y + h <= pad_layout::kTrackY1 && h >= 13);
    }
}

static void test_combat_and_priority()
{
    Game G;
    G.open("Kashya");
    G.step(0);
    G.start_talk(10, talk("K"));
    // While talking an enemy can't take the window (the dialog is open).
    G.g.hover = PadGame::kEnemy;
    G.g.hover_name = U("Fallen");
    G.g.hp = G.g.hp_max = 32768;
    G.step(5000);
    CHECK(G.story.mode() == PadFrame::kStory);
    // Attacking closes the dialog: the story goes, the enemy shows.
    G.g.dialog = false;
    G.g.talking = false;
    G.step(5100);
    G.step(5100 + PadStory::kDialogGraceMs);
    CHECK(G.story.mode() == PadFrame::kEnemy && G.story.stories() == 0);
    // Leaving the game resets everything.
    G.g.in_game = false;
    G.step(6000);
    CHECK(G.story.mode() == PadFrame::kIdle);
}

static void test_wrap_and_text()
{
    CHECK(pad_plain_text(u"\u00ffc4Gold\u00ffc0 text\n") == u"Gold text ");
    CHECK(pad_utf8(U("Ærøn")) == "Ærøn");
    CHECK(pad_utf16("Ąą €") == u"Ąą €");
    // Lines are wrapped again to the window's width, words kept whole.
    PadStory s(nullptr, 6 * 10);   // ten characters
    PadGame g;
    g.in_game = g.dialog = true;
    s.step(g, 0);
    g.talking = true;
    g.talk_id = 1;
    g.has_lines = true;
    g.lines = {u"one two three four", u"five", u"", u"abcdefghijklmnopqrstuvwxyz"};
    s.step(g, 0);
    g.talking = false;
    g.has_lines = false;
    s.step(g, 10);
    const auto &w = s.wrapped();
    CHECK(w.size() == 7);
    if (w.size() == 7) {
        CHECK(w[0] == u"one two" && w[1] == u"three four" && w[2] == u"five" && w[3].empty());
        CHECK(w[4] == u"abcdefghij" && w[5] == u"klmnopqrst" && w[6] == u"uvwxyz");
    }
}

// ---- the bottom screen -----------------------------------------------------------

static GameState in_game(uint64_t ui = 0)
{
    GameState gs;
    gs.known = gs.in_game = true;
    gs.ui = ui;
    return gs;
}

// One poll; a touch at (x, y) if x >= 0.
static Pad poll(SecondScreen &ss, const GameState &gs, uint64_t t, int x = -1, int y = -1)
{
    Pad p;
    if (x >= 0) {
        p.touching = true;
        p.touch_x = x;
        p.touch_y = y;
    }
    ss.update(p, gs, 320, 240, t);
    return p;
}

static void tap(SecondScreen &ss, const GameState &gs, uint64_t &t, int x, int y)
{
    poll(ss, gs, t += 20, x, y);
    poll(ss, gs, t += 20, x, y);
    poll(ss, gs, t += 20);
}

static void test_pad_tab()
{
    SecondScreen ss;
    ss.set_screen_auto(kScreenAutoAll);   // as the repository builds it, whichever this is
    BottomView v;
    uint64_t t = 0;
    GameState gs = in_game();
    poll(ss, gs, t);
    ss.stage(v, t);
    CHECK(v.kind == BottomView::kMap && !v.panel_tab);
    // Six tabs (seven with PANEL), each wide enough for its name in the 8-pixel font.
    CHECK(320 / 7 >= 8 * 5 + 2);
    // PAD is the sixth: x 267..319 of six.
    tap(ss, gs, t, 5 * 320 / 6 + 10, 5);
    ss.stage(v, t);
    CHECK(v.kind == BottomView::kPad && ss.pad_shown());
    // The whole screen: a touch at the top is the touchpad's, not the bar's.
    Pad p = poll(ss, gs, t += 20, 30, 5);
    CHECK(p.touching);
    p = poll(ss, gs, t += 20, 160, 120);
    CHECK(p.touching);   // a drag: still the touchpad
    poll(ss, gs, t += 20);
    ss.stage(v, t);
    CHECK(v.kind == BottomView::kPad);
    // The inventory and an NPC's menu don't switch to PANEL.
    gs = in_game(1ull << 0x01);
    poll(ss, gs, t += 20);
    ss.stage(v, t);
    CHECK(v.kind == BottomView::kPad);
    gs = in_game(1ull << 0x08);
    poll(ss, gs, t += 20);
    ss.stage(v, t);
    CHECK(v.kind == BottomView::kPad);
    gs = in_game(1ull << 0x01 | 1ull << 0x02);
    poll(ss, gs, t += 20);
    ss.stage(v, t);
    CHECK(v.kind == BottomView::kPad);
    // The close button: pressed while touched, closed when let go on it.
    p = poll(ss, gs, t += 20, pad_layout::kCloseX + 10, pad_layout::kCloseY + 10);
    CHECK(!p.touching);   // not the touchpad's
    ss.stage(v, t);
    CHECK(v.kind == BottomView::kPad && v.pad.close_pressed);
    poll(ss, gs, t += 20);
    ss.stage(v, t);
    CHECK(v.kind == BottomView::kMap && v.panel_tab);   // back to MAP, PANEL offered, not shown
    CHECK(ss.tab() == BottomTab::Map);
    // Stays off: the panels opening again (closed first) switch as usual.
    gs = in_game();
    poll(ss, gs, t += 20);
    gs = in_game(1ull << 0x01);
    poll(ss, gs, t += 20);
    ss.stage(v, t);
    CHECK(v.kind == BottomView::kViewport && v.tab == BottomTab::Panel);
    // From PANEL (7 tabs), PAD is the sixth again; back to the tab under PANEL.
    tap(ss, gs, t, 5 * 320 / 7 + 10, 5);
    ss.stage(v, t);
    CHECK(v.kind == BottomView::kPad);
    // Sliding off the close button before letting go: not closed.
    poll(ss, gs, t += 20, pad_layout::kCloseX + 5, pad_layout::kCloseY + 5);
    poll(ss, gs, t += 20, 100, 100);
    ss.stage(v, t);
    CHECK(!v.pad.close_pressed);
    poll(ss, gs, t += 20);
    ss.stage(v, t);
    CHECK(v.kind == BottomView::kPad);
    // Select + D-pad steps out of it and back.
    ss.step_tab(1);
    ss.stage(v, t);
    CHECK(v.kind == BottomView::kViewport && v.tab == BottomTab::Panel);
    ss.step_tab(-1);
    ss.stage(v, t);
    CHECK(v.kind == BottomView::kPad);
    // The settings name.
    BottomTab bt = BottomTab::Map;
    CHECK(bottom_tab_from_name("pad", bt) && bt == BottomTab::Pad && std::strcmp(bottom_tab_name(bt), "pad") == 0);
    CHECK(!bottom_tab_from_name("panel", bt));
}

static void test_pad_scroll_column()
{
    namespace L = pad_layout;
    SecondScreen ss;
    BottomView v;
    uint64_t t = 0;
    GameState gs = in_game(1ull << 0x08);
    poll(ss, gs, t);
    tap(ss, gs, t, 5 * 320 / 7 + 10, 5);   // seven tabs: the NPC's menu offers PANEL
    ss.stage(v, t);
    CHECK(v.kind == BottomView::kPad);
    PadGame g;
    g.in_game = g.dialog = true;
    g.npc = u"Warriv";
    ss.set_pad_game(g, t);
    g.talking = true;
    g.talk_id = 7;
    g.has_lines = true;
    g.lines = talk("W", 4, 8);   // 35 lines
    ss.set_pad_game(g, t);
    g.has_lines = false;
    g.talking = false;
    ss.set_pad_game(g, t += 10);
    ss.stage(v, t);
    CHECK(v.pad.mode == PadFrame::kStory && v.pad.total > L::kRows && v.pad.lines.size() == (size_t)L::kRows);
    const int bottom = ss.pad_story().max_top();
    CHECK(ss.pad_story().top() == bottom);
    // The up arrow: a line, shown pressed; held, it repeats.
    Pad p = poll(ss, gs, t += 20, L::kScrollX + 5, L::kBoxY0 + 4);
    CHECK(!p.touching);
    ss.stage(v, t);
    CHECK(v.pad.up_pressed && ss.pad_story().top() == bottom - 1);
    for (int i = 0; i < 30; i++)
        poll(ss, gs, t += 20, L::kScrollX + 5, L::kBoxY0 + 4);
    CHECK(ss.pad_story().top() < bottom - 2);
    poll(ss, gs, t += 20);
    ss.stage(v, t);
    CHECK(!v.pad.up_pressed);
    // The down arrow, held to the end: clamped.
    for (int i = 0; i < 200; i++)
        poll(ss, gs, t += 20, L::kScrollX + 5, L::kBoxY1 - 3);
    ss.stage(v, t);
    CHECK(v.pad.down_pressed && ss.pad_story().top() == bottom);
    poll(ss, gs, t += 20);
    // The thumb dragged to the top of the track: the first line.
    int ty = 0, th = 0;
    L::thumb(ss.pad_story().top(), ss.pad_story().total(), ty, th);
    poll(ss, gs, t += 20, L::kScrollX + 5, ty + 2);
    poll(ss, gs, t += 20, L::kScrollX + 5, L::kTrackY0 - 30);
    CHECK(ss.pad_story().top() == 0);
    poll(ss, gs, t += 20, L::kScrollX + 5, L::kTrackY1 + 30);
    CHECK(ss.pad_story().top() == bottom);
    poll(ss, gs, t += 20);
    // The track above the thumb: a page up.
    L::thumb(ss.pad_story().top(), ss.pad_story().total(), ty, th);
    poll(ss, gs, t += 20, L::kScrollX + 5, L::kTrackY0 + 2);
    CHECK(ss.pad_story().top() == bottom - L::kRows);
    poll(ss, gs, t += 20);
    // Outside the column and the button: the touchpad.
    p = poll(ss, gs, t += 20, 150, 100);
    CHECK(p.touching);
    poll(ss, gs, t += 20);
    // Drawn plainly without the art (no game files): nothing crashes.
    static uint16_t fb[320 * 240];
    static uint8_t font[256 * 8];
    ss.stage(v, t);
    draw_bottom(fb, v, nullptr, 0, 0, nullptr, font);
    CHECK(v.kind == BottomView::kPad);
}

static void test_defaults()
{
    // The repository's build: all on. A personal build's: the PANEL three off.
    CHECK(screen_auto_default(false) == (kAutoPanel | kAutoNpcMenu | kAutoPanelSide | kAutoKeyboard | kAutoVideo));
    CHECK(screen_auto_default(true) == (kAutoKeyboard | kAutoVideo));
#if D2_DEFAULT_NO_PANEL_AUTO
    CHECK(kScreenAutoDefault == (kAutoKeyboard | kAutoVideo));
    CHECK(std::strstr(screen_auto_build_note(), "off") != nullptr);
#else
    CHECK(kScreenAutoDefault == kScreenAutoAll);
    CHECK(std::strstr(screen_auto_build_note(), "all on") != nullptr);
#endif
    // No screenauto= line: the build's defaults (SecondScreen starts with them).
    SecondScreen ss;
    CHECK(ss.screen_auto() == kScreenAutoDefault);
    CHECK(screen_auto_bits("") == kScreenAutoDefault);
    // A line overrides either way.
    for (bool personal : {false, true}) {
        const uint32_t d = screen_auto_default(personal);
        CHECK(screen_auto_bits("", d) == d);
        CHECK(screen_auto_bits("panels,npcmenu,panelside", d) == kScreenAutoAll);
        CHECK(screen_auto_bits("-panels,-npcmenu,-panelside,-keyboard,-video", d) == 0);
        CHECK(screen_auto_bits(screen_auto_names(kScreenAutoAll).c_str(), d) == kScreenAutoAll);
        CHECK(screen_auto_bits(screen_auto_names(kAutoKeyboard | kAutoVideo).c_str(), d) == (kAutoKeyboard | kAutoVideo));
        CHECK(screen_auto_bits("panels", d) == (d | kAutoPanel));
    }
    // Personal: an NPC menu with the defaults doesn't switch to PANEL.
    SecondScreen p;
    p.set_screen_auto(screen_auto_default(true));
    BottomView v;
    uint64_t t = 0;
    poll(p, in_game(), t);
    poll(p, in_game(1ull << 0x08), t += 20);
    p.stage(v, t);
    CHECK(v.kind == BottomView::kMap && v.panel_tab);
    poll(p, in_game(1ull << 0x01), t += 20);
    p.stage(v, t);
    CHECK(v.kind == BottomView::kMap && v.panel_tab);
    // Repo: they do.
    SecondScreen r;
    r.set_screen_auto(screen_auto_default(false));
    poll(r, in_game(), t);
    poll(r, in_game(1ull << 0x08), t += 20);
    r.stage(v, t);
    CHECK(v.kind == BottomView::kViewport && v.tab == BottomTab::Panel);
}

// ---- the art's decoders ------------------------------------------------------------

static void put32(std::vector<uint8_t> &d, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        d.push_back((uint8_t)(v >> (8 * i)));
}

static void test_dc6()
{
    // One 3x2 frame, bottom-up: row 1 "skip 1, pixel 5", row 0 "pixels 1 2 3".
    std::vector<uint8_t> d;
    for (uint32_t v : {6u, 1u, 0u, 0xEEEEEEEEu, 1u, 1u})
        put32(d, v);
    put32(d, 28);   // the frame's place
    const std::vector<uint8_t> rle = {0x81, 1, 5, 0x80, 3, 1, 2, 3, 0x80};
    for (uint32_t v : {0u, 3u, 2u, 0u, 0u, 0u, 0u, (uint32_t)rle.size()})
        put32(d, v);
    d.insert(d.end(), rle.begin(), rle.end());
    std::vector<Dc6Frame> f;
    CHECK(dc6_decode(d, f) && f.size() == 1);
    if (f.size() == 1) {
        CHECK(f[0].w == 3 && f[0].h == 2);
        CHECK(f[0].mask[3] == 0 && f[0].mask[4] == 1 && f[0].px[4] == 5);   // bottom row: (1, 1)
        CHECK(f[0].px[0] == 1 && f[0].px[1] == 2 && f[0].px[2] == 3);
    }
    // Damaged files are refused, not read past their end.
    std::vector<uint8_t> cut(d.begin(), d.begin() + 40);
    CHECK(!dc6_decode(cut, f));
    std::vector<uint8_t> junk(100, 0xFF);
    CHECK(!dc6_decode(junk, f));
}

// The player's own files (PADWIN_GAME=<game folder>): the MPQ reader and
// the whole art, as the console builds it. Nothing is written anywhere.
static void test_game_files()
{
    const char *dir = std::getenv("PADWIN_GAME");
    if (!dir || !*dir) {
        std::printf("padwin: PADWIN_GAME not set, the game-file checks skipped\n");
        return;
    }
    const std::string base = std::string(dir) + "/";
    std::vector<uint8_t> pal;
    CHECK(mpq_read_file(base + "d2data.mpq", "data\\global\\palette\\ACT1\\pal.dat", pal) && pal.size() == 768);
    std::vector<uint8_t> use;
    CHECK(!mpq_read_file(base + "d2data.mpq", "data\\no\\such\\file.dc6", use));
    PadFileReader read = [&base](const std::string &name, std::vector<uint8_t> &out) {
        for (const char *a : {"patch_d2.mpq", "d2exp.mpq", "d2data.mpq"})
            if (mpq_read_file(base + a, name, out))
                return true;
        return false;
    };
    for (const char *lang : {"CYR", "LATIN"}) {
        PadArt art;
        const bool ok = pad_art_build(art, read, lang);
        CHECK(ok);
        if (!ok) {
            std::printf("padwin: art (%s): %s\n", lang, art.error.c_str());
            continue;
        }
        CHECK(art.bg_idle.size() == 320 * 240 && art.bg_story.size() == 320 * 240 && art.close[1].w == 32);
        CHECK(art.text_font.find(u'A') && art.name_font.cell_h > art.text_font.cell_h - 4);
        std::printf("padwin: art (%s) %u KB, %u + %u glyphs\n", lang, (unsigned)(art.bytes() / 1024),
                    (unsigned)art.name_font.glyphs.size(), (unsigned)art.text_font.glyphs.size());
        static uint16_t fb[320 * 240];
        PadFrame f;
        f.mode = PadFrame::kStory;
        f.name = u"Warriv";
        f.lines = {u"First line", u"", u"Third"};
        f.total = 30;
        f.top = 10;
        pad_draw(fb, art, f);
        CHECK(std::memcmp(fb, art.bg_story.data(), sizeof fb) != 0);
    }
    PadArt missing;
    PadFileReader none = [](const std::string &, std::vector<uint8_t> &) { return false; };
    CHECK(!pad_art_build(missing, none, "CYR") && !missing.error.empty());
}

int main()
{
    test_names();
    test_story_reveal_and_keep();
    test_talk_before_menu();
    test_reveal_by_scroll();
    test_many_stories();
    test_memory_cap();
    test_scroll();
    test_combat_and_priority();
    test_wrap_and_text();
    test_pad_tab();
    test_pad_scroll_column();
    test_defaults();
    test_dc6();
    test_game_files();
    if (g_failed) {
        std::printf("padwin: %d of %d checks failed\n", g_failed, g_checks);
        return 1;
    }
    std::printf("padwin%s: all %d checks passed\n", D2_DEFAULT_NO_PANEL_AUTO ? " (personal defaults)" : "", g_checks);
    return 0;
}
