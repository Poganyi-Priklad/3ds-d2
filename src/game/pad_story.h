// The PAD tab's window (second_screen.h): what it shows, worked out from
// what the game shows, one poll at a time. Pure: the game's state comes in
// as a PadGame (read by pad_game.h), the time as milliseconds, and the
// font's measure as a function, so it runs in the unit tests as it does on
// the console.
//
//   idle    the window alone (its background, frame and close button)
//   name    a name in the game's font16, frameless: gold for an NPC (the
//           one whose dialog is open, or the one under the cursor), white
//           for an object under the cursor
//   story   the NPC's name and what it tells: the talk's lines, revealed as
//           the game scrolls them (about one every 1.75 s, all of them once
//           the talk ends), kept when its menu comes back, and every later
//           story of the same dialog appended after two blank lines; cleared
//           when the dialog closes, a shop or trade opens, or a fight starts
//   enemy   a monster under the cursor: its name in red, and its life
//           (stats 6 and 7) as a bar under it
#ifndef D2_GAME_PAD_STORY_H
#define D2_GAME_PAD_STORY_H

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace game {

// The window's layout on the 320x240 screen (the mockups' Set B).
namespace pad_layout {
constexpr int kEdge = 5;                                // the frame's width (#8 PopUp_340x224's gold line and shadow)
constexpr int kCloseX = 280, kCloseY = 200, kCloseSize = 32;   // #17's close button, 3 px inside the frame
constexpr int kNameY = 9;                               // the name's top (font16, centred)
constexpr int kBoxX0 = 8, kBoxY0 = 30, kBoxX1 = 296, kBoxY1 = 186;   // the story's box (#40 frame, veil)
constexpr int kTextPadX = 10, kTextPadY = 7, kLineH = 15;
constexpr int kTextW = kBoxX1 - kBoxX0 - 2 * kTextPadX;            // 268: the lines are wrapped to it
constexpr int kRows = (kBoxY1 - kBoxY0 - 2 * kTextPadY) / kLineH;   // 9 lines shown
constexpr int kScrollX = 300, kScrollW = 12, kArrowH = 13;          // #40's own column, beside the box
constexpr int kTrackY0 = kBoxY0 + kArrowH, kTrackY1 = kBoxY1 - kArrowH;
constexpr int kHpX0 = 70, kHpX1 = 250, kHpY0 = 40, kHpY1 = 47;      // the life bar (inclusive y)
constexpr int kHpFrameX0 = 60, kHpFrameY0 = 30, kHpFrameX1 = 260, kHpFrameY1 = 58;
static_assert(kRows == 9, "the story box shows nine lines");

// The scroll thumb's top and height on the track, for `total` lines from
// `top` on: as tall as the share of the lines shown (13 pixels at least).
inline void thumb(int top, int total, int &y, int &h)
{
    const int track = kTrackY1 - kTrackY0, max_top = total > kRows ? total - kRows : 0;
    h = total > kRows ? track * kRows / total : track;
    h = h < 13 ? 13 : h;
    y = kTrackY0 + (max_top ? (track - h) * (top < 0 ? 0 : top > max_top ? max_top : top) / max_top : 0);
}
}  // namespace pad_layout

// One poll of the game, as the window needs it.
struct PadGame {
    bool in_game = false;
    bool dialog = false;        // an NPC's dialog is open: its menu, or its talk (UI entry 0x08)
    bool trade = false;         // a shop or a trade is open (UI entries 0x0C, 0x17)
    bool talking = false;       // the talk's text is scrolling (0x007BF1C0)
    uint32_t talk_id = 0;       // the talk's line list (changes with each talk)
    // The talk's lines, as the game wrapped them (blank lines between its
    // paragraphs); filled only when has_lines (a new talk was read).
    bool has_lines = false;
    std::vector<std::u16string> lines;
    std::u16string npc;         // the dialog's menu's title (another line while a talk shows)
    enum Hover : uint8_t { kNone, kNpc, kEnemy, kObject };
    Hover hover = kNone;
    uint32_t hover_key = 0;     // its type << 24 ^ id: a new unit
    std::u16string hover_name;
    int32_t hp = 0, hp_max = 0; // kEnemy: stats 6 and 7
    // How far the talk's text has scrolled, in 1/1024 pixels at 640x480
    // (its speed follows the talk's speech); -1 unknown (the clock then).
    int32_t scroll = -1;
};

// What the window draws now.
struct PadFrame {
    enum Mode : uint8_t { kIdle, kName, kStory, kEnemy };
    enum Colour : uint8_t { kGold, kRed, kWhite };
    Mode mode = kIdle;
    Colour colour = kGold;
    std::u16string name;
    std::vector<std::u16string> lines;   // the shown lines (at most pad_layout::kRows)
    int top = 0, total = 0;              // the first shown, of all revealed so far
    int32_t hp = 0, hp_max = 0;
    bool close_pressed = false, up_pressed = false, down_pressed = false;
    bool operator==(const PadFrame &o) const
    {
        return mode == o.mode && colour == o.colour && name == o.name && lines == o.lines && top == o.top &&
               total == o.total && hp == o.hp && hp_max == o.hp_max && close_pressed == o.close_pressed &&
               up_pressed == o.up_pressed && down_pressed == o.down_pressed;
    }
    bool operator!=(const PadFrame &o) const { return !(*this == o); }
};

class PadStory {
public:
    // The pace of the game's talk: the first line shows this long after the
    // talk starts, then one every kLineMs (measured on the lane at 640x480
    // and 800x600: about 10.7 pixels a second, a line every 18.5 pixels).
    static constexpr uint32_t kFirstMs = 1000, kLineMs = 1750;
    // The same by the game's own scroll (PadGame::scroll), as the lane's
    // screenshots line up with the top screen at 640x480 and 800x600: the
    // first line as the talk starts, then one every kLineScroll (18 pixels).
    static constexpr int32_t kFirstScroll = 0, kLineScroll = 18404;
    // Stories kept: once their lines (the game's) pass this many, the oldest
    // go (never the one being told).
    static constexpr size_t kMaxGameLines = 600;
    // A name stays this long after the cursor leaves its unit.
    static constexpr uint32_t kHoverLingerMs = 600;
    // A dialog gone this long is closed (shorter gaps, as a talk ends and
    // its menu comes back, keep its stories).
    static constexpr uint32_t kDialogGraceMs = 400;

    // `measure`: a line's width in pixels in the story's font; lines are
    // wrapped to `width`.
    explicit PadStory(std::function<int(const std::u16string &)> measure = nullptr, int width = pad_layout::kTextW);
    void set_measure(std::function<int(const std::u16string &)> measure)
    {
        measure_ = std::move(measure);
        for (Story &s : stories_)
            s.wrapped_for = SIZE_MAX;
        rewrap_ = true;
    }

    void step(const PadGame &g, uint64_t now_ms);
    // Clears everything (PAD left, the game ended).
    void reset();

    // Scrolling the revealed lines: by lines, or to a fraction of the way
    // (0 the first line at the top, 1 the last at the bottom).
    void scroll_by(int lines);
    void scroll_to_fraction(float f);
    int top() const { return top_; }
    int max_top() const;
    int total() const { return (int)wrapped_.size(); }
    size_t stories() const { return stories_.size(); }
    const std::vector<std::u16string> &wrapped() const { return wrapped_; }

    // The frame to draw (without the pressed buttons, which the touch sets).
    void frame(PadFrame &f) const;
    PadFrame::Mode mode() const;

private:
    struct Story {
        std::vector<std::u16string> lines;   // the game's lines
        uint64_t t0 = 0;                     // when the talk started
        int32_t scroll = -1;                 // the game's scroll, while told (-1: by the clock)
        bool done = false;                   // the talk ended: all revealed
        size_t shown = 0;                    // revealed so far
        std::vector<std::u16string> wrapped; // its revealed lines wrapped to the window
        size_t wrapped_for = SIZE_MAX;       // ...when `shown` was this
    };
    void clear_stories();
    void reveal(uint64_t now_ms);
    void rewrap();

    std::function<int(const std::u16string &)> measure_;
    int width_;
    std::vector<Story> stories_;
    std::vector<std::u16string> wrapped_;
    bool rewrap_ = false;
    int top_ = 0;
    bool follow_ = true;          // keeps the newest line in view (until scrolled up)
    bool prev_talking_ = false;
    uint32_t prev_talk_id_ = 0;
    bool waiting_lines_ = false;  // a talk started; its lines not read yet
    uint64_t talk_t0_ = 0;
    std::u16string npc_;
    std::u16string last_npc_;     // the last NPC under the cursor
    uint64_t dialog_lost_at_ = 0; // when the dialog went (0: it has not)
    // The unit under the cursor (outside a dialog), kept a moment.
    PadGame::Hover hover_ = PadGame::kNone;
    std::u16string hover_name_;
    int32_t hp_ = 0, hp_max_ = 0;
    uint64_t hover_until_ = 0;
    bool dialog_ = false;
};

// The game's text as it is shown: its colour codes ("\u00ffc" and a
// character) taken out.
std::u16string pad_plain_text(const std::u16string &s);
// UTF-16 to UTF-8 and back (the lane's logs, the tests).
std::string pad_utf8(const std::u16string &s);
std::u16string pad_utf16(const std::string &s);

}  // namespace game

#endif
