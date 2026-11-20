// See pad_story.h.
//
// PadStory is the PAD window's state, kept apart from the game and from
// drawing so tests/padwin can drive it with made-up polls: which NPC the
// dialog is with, the talks ("stories") told in it so far, how many of
// their lines have been revealed (at the game's own scroll speed, so the
// window never runs ahead of the speech), the lines wrapped to the window's
// width, the scroll position, and the hovered unit's name and life.
#include "game/pad_story.h"

#include <algorithm>

namespace game {

// The game's text without its colour codes and control characters (line
// breaks and tabs become spaces): the window wraps and colours it itself.
std::u16string pad_plain_text(const std::u16string &s)
{
    std::u16string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        const char16_t c = s[i];
        if (c == 0x00FF && i + 1 < s.size() && s[i + 1] == u'c') {
            i += 2;   // a colour code: "\u00ffc" and the colour's character
            continue;
        }
        if (c == u'\r' || c == u'\n' || c == u'\t')
            out += u' ';
        else if (c >= 0x20)
            out += c;
    }
    return out;
}

std::string pad_utf8(const std::u16string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 63));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 63));
            out += (char)(0x80 | (c & 63));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 63));
            out += (char)(0x80 | ((c >> 6) & 63));
            out += (char)(0x80 | (c & 63));
        }
    }
    return out;
}

std::u16string pad_utf16(const std::string &s)
{
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        const uint8_t b = (uint8_t)s[i];
        uint32_t c = b;
        int n = 0;
        if (b >= 0xF0) c = b & 7, n = 3;
        else if (b >= 0xE0) c = b & 15, n = 2;
        else if (b >= 0xC0) c = b & 31, n = 1;
        i++;
        for (; n > 0 && i < s.size(); n--, i++)
            c = (c << 6) | ((uint8_t)s[i] & 63);
        if (c >= 0x10000) {
            c -= 0x10000;
            out += (char16_t)(0xD800 + (c >> 10));
            out += (char16_t)(0xDC00 + (c & 0x3FF));
        } else {
            out += (char16_t)c;
        }
    }
    return out;
}

PadStory::PadStory(std::function<int(const std::u16string &)> measure, int width)
    : measure_(std::move(measure)), width_(width)
{
}

// Out of the game: everything forgotten.
void PadStory::reset()
{
    clear_stories();
    prev_talking_ = false;
    prev_talk_id_ = 0;
    npc_.clear();
    last_npc_.clear();
    dialog_lost_at_ = 0;
    hover_ = PadGame::kNone;
    hover_name_.clear();
    hp_ = hp_max_ = 0;
    hover_until_ = 0;
    dialog_ = false;
}

// The dialog closed (or a shop or trade opened): its talks are dropped.
void PadStory::clear_stories()
{
    stories_.clear();
    wrapped_.clear();
    waiting_lines_ = false;
    top_ = 0;
    follow_ = true;
    rewrap_ = false;
}

int PadStory::max_top() const { return std::max(0, (int)wrapped_.size() - pad_layout::kRows); }

// Scrolling by the player. Scrolled to the bottom, the window follows new
// lines again; scrolled up, it stays where it was put.
void PadStory::scroll_by(int lines)
{
    top_ = std::max(0, std::min(max_top(), top_ + lines));
    follow_ = top_ == max_top();
}

void PadStory::scroll_to_fraction(float f)
{
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    top_ = (int)(f * (float)max_top() + 0.5f);
    top_ = std::max(0, std::min(max_top(), top_));
    follow_ = top_ == max_top();
}

// One poll of the game (read_pad_game): starts, extends or closes the
// dialog's stories, follows the hovered unit, and reveals lines.
void PadStory::step(const PadGame &g, uint64_t now_ms)
{
    if (!g.in_game) {
        reset();
        return;
    }
    // The NPC's dialog: its menu, or a talk (an NPC's first words come
    // before its menu does).
    bool dialog = (g.dialog || g.talking) && !g.trade;
    if (dialog) {
        dialog_lost_at_ = 0;
    } else if (dialog_ && !g.trade) {
        // Gone for a moment (a talk ending, its menu coming back)? Kept
        // until kDialogGraceMs.
        if (!dialog_lost_at_)
            dialog_lost_at_ = now_ms ? now_ms : 1;
        if (now_ms - dialog_lost_at_ < kDialogGraceMs)
            dialog = true;
    }
    if (!dialog) {
        // The dialog closed (Cancel, B, walking away, a fight), or a shop
        // or trade took its place: its stories go.
        if (!stories_.empty() || waiting_lines_)
            clear_stories();
        npc_.clear();
        dialog_lost_at_ = 0;
    } else {
        // Its NPC: the one under the cursor when it opened, else its menu's
        // title (read once the menu shows; while a talk runs it may hold
        // the chosen line).
        if (!dialog_)
            npc_ = last_npc_;
        if (npc_.empty() && !g.npc.empty() && g.dialog && !g.talking)
            npc_ = pad_plain_text(g.npc);
        const bool started = g.talking && (!prev_talking_ || (g.talk_id && g.talk_id != prev_talk_id_));
        if (started) {
            // A new talk: the one before (if any) is told whole.
            for (Story &s : stories_)
                s.done = true;
            waiting_lines_ = true;
            talk_t0_ = now_ms;
            rewrap_ = true;
        }
        if (waiting_lines_ && g.has_lines && !g.lines.empty()) {
            Story s;
            for (const std::u16string &l : g.lines)
                s.lines.push_back(pad_plain_text(l));
            s.t0 = talk_t0_;
            s.scroll = g.talking ? g.scroll : -1;
            s.done = !g.talking;
            stories_.push_back(std::move(s));
            waiting_lines_ = false;
            // The oldest go once there are too many lines (never the newest).
            size_t n = 0;
            for (const Story &st : stories_)
                n += st.lines.size();
            while (stories_.size() > 1 && n > kMaxGameLines) {
                n -= stories_.front().lines.size();
                stories_.erase(stories_.begin());
            }
            rewrap_ = true;
        }
        if (g.talking && !stories_.empty() && !stories_.back().done && g.scroll >= 0 &&
            stories_.back().scroll >= 0)
            stories_.back().scroll = g.scroll;
        if (!g.talking && prev_talking_) {
            // The talk ended (its menu comes back): all of it shows, and stays.
            for (Story &s : stories_)
                s.done = true;
            waiting_lines_ = false;
            rewrap_ = true;
        }
    }
    dialog_ = dialog;
    prev_talking_ = g.talking;
    prev_talk_id_ = g.talk_id;

    // The unit under the cursor, outside a dialog: its name (and an
    // enemy's life), kept a moment after the cursor leaves it.
    if (dialog) {
        hover_ = PadGame::kNone;
        hover_until_ = 0;
    } else if (g.hover != PadGame::kNone && !g.hover_name.empty()) {
        if (g.hover == PadGame::kNpc)
            last_npc_ = pad_plain_text(g.hover_name);
        hover_ = g.hover;
        hover_name_ = pad_plain_text(g.hover_name);
        hp_ = g.hp;
        hp_max_ = g.hp_max;
        hover_until_ = now_ms + kHoverLingerMs;
    } else if (now_ms >= hover_until_) {
        hover_ = PadGame::kNone;
    }
    reveal(now_ms);
    if (rewrap_)
        rewrap();
}

// How many lines of each story are shown: as far as the game's own talk
// has scrolled, or by time when the game's scroll can't be read.
void PadStory::reveal(uint64_t now_ms)
{
    for (Story &s : stories_) {
        size_t n = s.lines.size();
        if (!s.done && s.scroll >= 0) {
            n = s.scroll < kFirstScroll ? 0 : (size_t)(1 + (s.scroll - kFirstScroll) / kLineScroll);
            n = std::min(n, s.lines.size());
        } else if (!s.done) {
            const uint64_t first = s.t0 + kFirstMs;
            n = now_ms < first ? 0 : (size_t)(1 + (now_ms - first) / kLineMs);
            n = std::min(n, s.lines.size());
        }
        if (n != s.shown) {
            s.shown = n;
            rewrap_ = true;
        }
    }
}

// The revealed lines wrapped to the window's width with the game's font.
void PadStory::rewrap()
{
    rewrap_ = false;
    const bool follow = follow_ || top_ >= max_top();
    auto width = [this](const std::u16string &s) { return measure_ ? measure_(s) : (int)s.size() * 6; };
    std::vector<std::u16string> out;
    for (Story &s : stories_) {
        if (s.wrapped_for == s.shown) {
            if (!s.wrapped.empty()) {
                if (!out.empty()) {
                    out.push_back(u"");   // two blank lines between stories
                    out.push_back(u"");
                }
                out.insert(out.end(), s.wrapped.begin(), s.wrapped.end());
            }
            continue;
        }
        // The story's revealed lines as paragraphs (the game's blank lines
        // part them), wrapped again to the window's width.
        std::vector<std::u16string> story;
        std::u16string para;
        auto flush = [&]() {
            if (para.empty())
                return;
            std::u16string line;
            size_t i = 0;
            while (i < para.size()) {
                while (i < para.size() && para[i] == u' ')
                    i++;
                size_t j = i;
                while (j < para.size() && para[j] != u' ')
                    j++;
                if (i == j)
                    break;
                const std::u16string word = para.substr(i, j - i);
                const std::u16string tried = line.empty() ? word : line + u' ' + word;
                if (line.empty() || width(tried) <= width_) {
                    line = tried;
                } else {
                    story.push_back(line);
                    line = word;
                }
                // A word wider than the window alone: cut where it no longer fits.
                while (width(line) > width_ && line.size() > 1) {
                    size_t k = line.size() - 1;
                    while (k > 1 && width(line.substr(0, k)) > width_)
                        k--;
                    story.push_back(line.substr(0, k));
                    line = line.substr(k);
                }
                i = j;
            }
            if (!line.empty())
                story.push_back(line);
            para.clear();
        };
        for (size_t i = 0; i < s.shown; i++) {
            const std::u16string &l = s.lines[i];
            if (l.find_first_not_of(u' ') == std::u16string::npos) {
                flush();
                if (!story.empty() && !story.back().empty())
                    story.push_back(u"");
            } else {
                if (!para.empty())
                    para += u' ';
                para += l;
            }
        }
        flush();
        while (!story.empty() && story.back().empty())
            story.pop_back();
        s.wrapped = story;
        s.wrapped_for = s.shown;
        if (story.empty())
            continue;
        if (!out.empty()) {
            out.push_back(u"");   // two blank lines between stories
            out.push_back(u"");
        }
        out.insert(out.end(), story.begin(), story.end());
    }
    wrapped_ = std::move(out);
    top_ = follow ? max_top() : std::min(top_, max_top());
    follow_ = top_ == max_top();
}

// Which of the window's layouts shows: a story while a dialog has lines,
// a name for an NPC or object, a life bar for an enemy, or the idle art.
PadFrame::Mode PadStory::mode() const
{
    if (dialog_)
        return !wrapped_.empty() ? PadFrame::kStory : !npc_.empty() ? PadFrame::kName : PadFrame::kIdle;
    switch (hover_) {
    case PadGame::kEnemy: return PadFrame::kEnemy;
    case PadGame::kNpc:
    case PadGame::kObject: return PadFrame::kName;
    case PadGame::kNone: break;
    }
    return PadFrame::kIdle;
}

void PadStory::frame(PadFrame &f) const
{
    f.mode = mode();
    f.lines.clear();
    f.name.clear();
    f.top = f.total = 0;
    f.hp = f.hp_max = 0;
    f.colour = PadFrame::kGold;
    switch (f.mode) {
    case PadFrame::kStory:
        f.top = top_;
        f.total = (int)wrapped_.size();
        for (int i = top_; i < (int)wrapped_.size() && i < top_ + pad_layout::kRows; i++)
            f.lines.push_back(wrapped_[i]);
        f.name = npc_;
        break;
    case PadFrame::kName:
        f.name = dialog_ ? npc_ : hover_name_;
        f.colour = !dialog_ && hover_ == PadGame::kObject ? PadFrame::kWhite : PadFrame::kGold;
        break;
    case PadFrame::kEnemy:
        f.name = hover_name_;
        f.colour = PadFrame::kRed;
        f.hp = hp_;
        f.hp_max = hp_max_;
        break;
    case PadFrame::kIdle:
        break;
    }
}

}  // namespace game
