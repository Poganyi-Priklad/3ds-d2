// See pad_art.h.
#include "game/pad_art.h"

#include <algorithm>
#include <cstring>

namespace game {
namespace {

namespace L = pad_layout;
constexpr int kW = 320, kH = 240;

uint32_t le32(const std::vector<uint8_t> &d, size_t at)
{
    return at + 4 <= d.size() ? (uint32_t)d[at] | (uint32_t)d[at + 1] << 8 | (uint32_t)d[at + 2] << 16 | (uint32_t)d[at + 3] << 24
                              : 0;
}
uint16_t le16(const std::vector<uint8_t> &d, size_t at)
{
    return at + 2 <= d.size() ? (uint16_t)(d[at] | d[at + 1] << 8) : 0;
}

inline uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// An RGBA picture while composing, row-major.
struct Img {
    int w = 0, h = 0;
    std::vector<uint32_t> px;   // 0xAABBGGRR
    Img() = default;
    Img(int w_, int h_, uint32_t fill = 0) : w(w_), h(h_), px((size_t)w_ * h_, fill) {}
    uint32_t at(int x, int y) const { return px[(size_t)y * w + x]; }
};
constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }

Img frame_img(const Dc6Frame &f, const uint8_t pal[768])
{
    Img im(f.w, f.h);
    for (size_t i = 0; i < f.px.size(); i++)
        if (f.mask[i]) {
            const uint8_t *c = pal + f.px[i] * 3;   // pal.dat: blue, green, red
            im.px[i] = rgba(c[2], c[1], c[0], 255);
        }
    return im;
}

// A picture cut into frames, as the game stores the big ones: a row of
// frames ends with one narrower than 256 pixels.
Img assemble(const std::vector<Dc6Frame> &frames, const uint8_t pal[768])
{
    std::vector<std::vector<const Dc6Frame *>> rows(1);
    for (const Dc6Frame &f : frames) {
        rows.back().push_back(&f);
        if (f.w < 256)
            rows.emplace_back();
    }
    if (rows.back().empty())
        rows.pop_back();
    int w = 0, h = 0;
    for (const auto &r : rows) {
        int rw = 0, rh = 0;
        for (const Dc6Frame *f : r)
            rw += f->w, rh = std::max(rh, f->h);
        w = std::max(w, rw);
        h += rh;
    }
    Img out(w, h);
    int y = 0;
    for (const auto &r : rows) {
        int x = 0, rh = 0;
        for (const Dc6Frame *f : r) {
            const Img im = frame_img(*f, pal);
            for (int j = 0; j < im.h; j++)
                for (int i = 0; i < im.w; i++)
                    out.px[(size_t)(y + j) * w + x + i] = im.at(i, j);
            x += f->w;
            rh = std::max(rh, f->h);
        }
        y += rh;
    }
    return out;
}

Img crop(const Img &src, int x0, int y0, int x1, int y1)
{
    Img out(x1 - x0, y1 - y0);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (x >= 0 && y >= 0 && x < src.w && y < src.h)
                out.px[(size_t)(y - y0) * out.w + (x - x0)] = src.at(x, y);
    return out;
}

// Source over destination (straight alpha).
void composite(Img &dst, const Img &src, int dx, int dy)
{
    for (int y = 0; y < src.h; y++)
        for (int x = 0; x < src.w; x++) {
            const int tx = dx + x, ty = dy + y;
            if (tx < 0 || ty < 0 || tx >= dst.w || ty >= dst.h)
                continue;
            const uint32_t s = src.at(x, y), a = s >> 24;
            if (!a)
                continue;
            uint32_t &d = dst.px[(size_t)ty * dst.w + tx];
            if (a == 255) {
                d = s;
                continue;
            }
            auto mix = [a](uint32_t sc, uint32_t dc) { return (sc * a + dc * (255 - a) + 127) / 255; };
            d = rgba(mix(s & 255, d & 255), mix(s >> 8 & 255, d >> 8 & 255), mix(s >> 16 & 255, d >> 16 & 255), 255);
        }
}

Img tile_h(const Img &strip, int w)
{
    Img out(w, strip.h);
    for (int x = 0; x < w; x += strip.w)
        composite(out, crop(strip, 0, 0, std::min(strip.w, w - x), strip.h), x, 0);
    return out;
}
Img tile_v(const Img &strip, int h)
{
    Img out(strip.w, h);
    for (int y = 0; y < h; y += strip.h)
        composite(out, crop(strip, 0, 0, strip.w, std::min(strip.h, h - y)), 0, y);
    return out;
}

// A box drawn from textslid's own corners (0 top left, 3 top right, 1
// bottom left, 2 bottom right) and edges (4 top, 6 bottom, 5 left, 7
// right), 12x13 each.
void frame40(Img &m, const std::vector<Img> &ts, int x0, int y0, int x1, int y1)
{
    const int w = x1 - x0, h = y1 - y0;
    Img o(w, h);
    composite(o, tile_h(ts[4], w), 0, 0);
    composite(o, tile_h(ts[6], w), 0, h - 13);
    composite(o, tile_v(ts[5], h), 0, 0);
    composite(o, tile_v(ts[7], h), w - 12, 0);
    composite(o, ts[0], 0, 0);
    composite(o, ts[3], w - 12, 0);
    composite(o, ts[1], 0, h - 13);
    composite(o, ts[2], w - 12, h - 13);
    composite(m, o, x0, y0);
}

std::vector<uint16_t> to_fb(const Img &m)
{
    std::vector<uint16_t> fb((size_t)kW * kH);
    for (int x = 0; x < kW; x++)
        for (int y = 0; y < kH; y++) {
            const uint32_t p = m.at(x, y);
            fb[(size_t)x * kH + (kH - 1 - y)] = rgb565(p & 255, p >> 8 & 255, p >> 16 & 255);
        }
    return fb;
}

PadSprite to_sprite(const Img &im)
{
    PadSprite s;
    s.w = im.w;
    s.h = im.h;
    s.rgb.resize(im.px.size());
    s.a.resize(im.px.size());
    for (size_t i = 0; i < im.px.size(); i++) {
        const uint32_t p = im.px[i];
        s.rgb[i] = rgb565(p & 255, p >> 8 & 255, p >> 16 & 255);
        s.a[i] = (uint8_t)(p >> 24);
    }
    return s;
}

bool read_dc6(const PadFileReader &read, const std::string &name, std::vector<Dc6Frame> &frames, std::string &error)
{
    std::vector<uint8_t> file;
    if (!read(name, file)) {
        error = "missing " + name;
        return false;
    }
    if (!dc6_decode(file, frames) || frames.empty()) {
        error = "not a DC6: " + name;
        return false;
    }
    return true;
}

}  // namespace

bool dc6_decode(const std::vector<uint8_t> &d, std::vector<Dc6Frame> &frames)
{
    frames.clear();
    if (d.size() < 24 || le32(d, 0) != 6)
        return false;
    const uint32_t dirs = le32(d, 16), per = le32(d, 20);
    if (!dirs || !per || dirs * per > 4096 || 24 + 4 * (size_t)dirs * per > d.size())
        return false;
    for (uint32_t k = 0; k < dirs * per; k++) {
        const size_t p = le32(d, 24 + 4 * (size_t)k);
        if (p + 32 > d.size())
            return false;
        const bool flip = le32(d, p) != 0;
        const int32_t w = (int32_t)le32(d, p + 4), h = (int32_t)le32(d, p + 8);
        const uint32_t len = le32(d, p + 28);
        if (w < 0 || h < 0 || w > 4096 || h > 4096 || p + 32 + len > d.size())
            return false;
        Dc6Frame f;
        f.w = w;
        f.h = h;
        f.px.assign((size_t)w * h, 0);
        f.mask.assign((size_t)w * h, 0);
        // Runs: 0x80 ends a row; a byte with the high bit skips that many
        // (low 7 bits) pixels; any other is that many pixels that follow.
        // Rows go up from the bottom unless the frame is flipped.
        int x = 0, y = flip ? 0 : h - 1;
        for (size_t i = p + 32, end = p + 32 + len; i < end;) {
            const uint8_t b = d[i++];
            if (b == 0x80) {
                x = 0;
                y += flip ? 1 : -1;
            } else if (b & 0x80) {
                x += b & 0x7F;
            } else {
                for (int n = 0; n < b && i < end; n++, i++, x++)
                    if (x >= 0 && x < w && y >= 0 && y < h) {
                        f.px[(size_t)y * w + x] = d[i];
                        f.mask[(size_t)y * w + x] = 1;
                    }
            }
        }
        frames.push_back(std::move(f));
    }
    return true;
}

const PadFont::Glyph *PadFont::find(char16_t code) const
{
    auto it = std::lower_bound(glyphs.begin(), glyphs.end(), code,
                               [](const Glyph &g, char16_t c) { return g.code < c; });
    return it != glyphs.end() && it->code == code ? &*it : nullptr;
}

int PadFont::width(const std::u16string &s) const
{
    int w = 0;
    for (char16_t c : s) {
        const Glyph *g = find(c);
        w += g ? g->advance : cell_h / 2;
    }
    return w;
}

bool pad_font_decode(const std::vector<uint8_t> &dc6, const std::vector<uint8_t> &tbl, const uint8_t pal[768],
                     PadFont &out)
{
    out = PadFont();
    std::vector<Dc6Frame> frames;
    if (!dc6_decode(dc6, frames) || tbl.size() < 12 || std::memcmp(tbl.data(), "Woo!", 4) != 0)
        return false;
    for (const Dc6Frame &f : frames)
        out.cell_h = std::max(out.cell_h, f.h);
    std::vector<uint32_t> frame_off(frames.size(), UINT32_MAX);
    // Entries of 14 bytes: the code, a byte, the advance, the height, a
    // byte, a word, the frame, a dword.
    for (size_t at = 12; at + 14 <= tbl.size(); at += 14) {
        PadFont::Glyph g;
        g.code = le16(tbl, at);
        g.advance = tbl[at + 3];
        const uint16_t frame = le16(tbl, at + 8);
        if (frame >= frames.size())
            continue;
        const Dc6Frame &f = frames[frame];
        g.w = (uint8_t)std::min(f.w, 255);
        g.h = (uint8_t)std::min(f.h, 255);
        if (frame_off[frame] != UINT32_MAX) {   // codes sharing a glyph share its pixels
            g.off = frame_off[frame];
            out.glyphs.push_back(g);
            continue;
        }
        g.off = frame_off[frame] = (uint32_t)out.lum.size();
        for (int y = 0; y < g.h; y++)
            for (int x = 0; x < g.w; x++) {
                const size_t i = (size_t)y * f.w + x;
                const uint8_t *c = pal + f.px[i] * 3;
                const uint8_t l = std::max(c[0], std::max(c[1], c[2]));
                out.lum.push_back(f.mask[i] ? (uint8_t)std::max<int>(1, l) : 0);
            }
        out.glyphs.push_back(g);
    }
    std::stable_sort(out.glyphs.begin(), out.glyphs.end(),
                     [](const PadFont::Glyph &a, const PadFont::Glyph &b) { return a.code < b.code; });
    // The first entry for a code wins, as the game's lookup finds it.
    out.glyphs.erase(std::unique(out.glyphs.begin(), out.glyphs.end(),
                                 [](const PadFont::Glyph &a, const PadFont::Glyph &b) { return a.code == b.code; }),
                     out.glyphs.end());
    return !out.glyphs.empty();
}

size_t PadArt::bytes() const
{
    size_t n = (bg_idle.size() + bg_story.size() + bg_enemy.size()) * 2;
    for (const PadSprite &s : close)
        n += s.rgb.size() * 3;
    for (const PadSprite &s : slider)
        n += s.rgb.size() * 3;
    n += name_font.lum.size() + text_font.lum.size();
    n += (name_font.glyphs.size() + text_font.glyphs.size()) * sizeof(PadFont::Glyph);
    return n;
}

bool pad_art_build(PadArt &art, const PadFileReader &read, const std::string &language)
{
    art = PadArt();
    std::vector<uint8_t> pal;
    if (!read("data\\global\\palette\\ACT1\\pal.dat", pal) || pal.size() < 768) {
        art.error = "missing the ACT1 palette";
        return false;
    }
    std::vector<Dc6Frame> bg, popup, button, slider;
    if (!read_dc6(read, "data\\global\\ui\\BIGMENU\\blankbckg.dc6", bg, art.error) ||
        !read_dc6(read, "data\\global\\ui\\FrontEnd\\PopUp_340x224.dc6", popup, art.error) ||
        !read_dc6(read, "data\\global\\ui\\PANEL\\buysellbtn.DC6", button, art.error) ||
        !read_dc6(read, "data\\global\\ui\\MENU\\textslid.DC6", slider, art.error))
        return false;
    if (button.size() < 12 || slider.size() < 17) {
        art.error = "buysellbtn or textslid has too few frames";
        return false;
    }
    // The fonts: the game's language first, LATIN if that has none.
    auto font = [&](const char *name, PadFont &out) {
        for (const std::string &folder : {language, std::string("LATIN")}) {
            if (folder.empty())
                continue;
            std::vector<uint8_t> dc6, tbl;
            const std::string base = "data\\local\\font\\" + folder + "\\" + name;
            if (read(base + ".dc6", dc6) && read(base + ".tbl", tbl) && pad_font_decode(dc6, tbl, pal.data(), out))
                return true;
        }
        art.error = std::string("missing the font ") + name;
        return false;
    };
    if (!font("font16", art.name_font) || !font("fontformal10", art.text_font))
        return false;

    std::vector<Img> ts;
    for (const Dc6Frame &f : slider)
        ts.push_back(frame_img(f, pal.data()));
    for (int i = 0; i < 17; i++)
        art.slider[i] = to_sprite(ts[i]);
    art.close[0] = to_sprite(frame_img(button[10], pal.data()));
    art.close[1] = to_sprite(frame_img(button[11], pal.data()));

    // The background: blankbckg's "Diablo II" centred (its middle at 183.5,
    // 185 of the 373x373 picture), at 1:1.
    Img m(kW, kH, rgba(0, 0, 0, 255));
    const Img big = assemble(bg, pal.data());
    composite(m, crop(big, 23, 65, 23 + kW, 65 + kH), 0, 0);
    // A dark band inside the frame's line, six pixels wide.
    for (int i = 0; i < 6; i++) {
        const int x0 = 3 + i, y0 = 3 + i, x1 = kW - 4 - i, y1 = kH - 4 - i;
        for (int x = x0; x <= x1; x++)
            m.px[(size_t)y0 * kW + x] = m.px[(size_t)y1 * kW + x] = rgba(0, 0, 0, 255);
        for (int y = y0; y <= y1; y++)
            m.px[(size_t)y * kW + x0] = m.px[(size_t)y * kW + x1] = rgba(0, 0, 0, 255);
    }
    // The frame: PopUp_340x224's thin gold line, its real corners (4x4) and
    // 3-pixel edges (cut from their middles), nine-sliced to the screen.
    {
        const Img p = assemble(popup, pal.data());
        const int pw = p.w, ph = p.h, c = 4, e = 3;
        Img o(kW, kH);
        composite(o, tile_h(crop(p, 100, 0, 180, e), kW), 0, 0);
        composite(o, tile_h(crop(p, 100, ph - e, 180, ph), kW), 0, kH - e);
        composite(o, tile_v(crop(p, 0, 80, e, 160), kH), 0, 0);
        composite(o, tile_v(crop(p, pw - e, 80, pw, 160), kH), kW - e, 0);
        composite(o, crop(p, 0, 0, c, c), 0, 0);
        composite(o, crop(p, pw - c, 0, pw, c), kW - c, 0);
        composite(o, crop(p, 0, ph - c, c, ph), 0, kH - c);
        composite(o, crop(p, pw - c, ph - c, pw, ph), kW - c, kH - c);
        composite(m, o, 0, 0);
    }
    art.bg_idle = to_fb(m);

    // The story's: a veil over the box, the box's #40 frame, and the scroll
    // column's track (the arrows and the thumb are drawn over it).
    {
        Img s = m;
        Img veil(L::kBoxX1 - L::kBoxX0, L::kBoxY1 - L::kBoxY0, rgba(0, 0, 0, 110));
        composite(s, veil, L::kBoxX0, L::kBoxY0);
        frame40(s, ts, L::kBoxX0, L::kBoxY0, L::kBoxX1, L::kBoxY1);
        for (int y = L::kTrackY0; y + L::kArrowH <= L::kTrackY1; y += L::kArrowH)
            composite(s, ts[13], L::kScrollX, y);
        art.bg_story = to_fb(s);
    }
    // The enemy's: the life bar's #40 frame.
    {
        Img s = m;
        frame40(s, ts, L::kHpFrameX0, L::kHpFrameY0, L::kHpFrameX1, L::kHpFrameY1);
        art.bg_enemy = to_fb(s);
    }
    art.ok = true;
    return true;
}

// ---- drawing ----------------------------------------------------------------------

namespace {

struct Fb {
    uint16_t *fb;
    void put(int x, int y, uint16_t c) const
    {
        if (x >= 0 && x < kW && y >= 0 && y < kH)
            fb[x * kH + (kH - 1 - y)] = c;
    }
    // A sprite's rows sy0 to sy1 (exclusive) with its top-left at (x, y).
    void sprite(const PadSprite &s, int x, int y, int sy0 = 0, int sy1 = -1) const
    {
        if (sy1 < 0 || sy1 > s.h)
            sy1 = s.h;
        for (int j = sy0; j < sy1; j++)
            for (int i = 0; i < s.w; i++) {
                const size_t k = (size_t)j * s.w + i;
                if (s.a[k])
                    put(x + i, y + (j - sy0), s.rgb[k]);
            }
    }
    void fill(int x0, int y0, int x1, int y1, uint16_t c) const   // inclusive
    {
        for (int x = std::max(0, x0); x <= std::min(kW - 1, x1); x++)
            for (int y = std::max(0, y0); y <= std::min(kH - 1, y1); y++)
                fb[x * kH + (kH - 1 - y)] = c;
    }
    // Text in a game font, tinted: each pixel the colour times its brightness.
    void text(const PadFont &f, int x, int y, const std::u16string &s, uint32_t r, uint32_t g, uint32_t b) const
    {
        for (char16_t ch : s) {
            const PadFont::Glyph *gl = f.find(ch);
            if (!gl) {
                x += f.cell_h / 2;
                continue;
            }
            const int gy = y + (gl->h <= f.cell_h ? f.cell_h - gl->h : 0);
            const uint8_t *lum = f.lum.data() + gl->off;
            for (int j = 0; j < gl->h; j++)
                for (int i = 0; i < gl->w; i++) {
                    const uint32_t l = lum[j * gl->w + i];
                    if (l)
                        put(x + i, gy + j, rgb565(r * l / 255, g * l / 255, b * l / 255));
                }
            x += gl->advance;
        }
    }
};

}  // namespace

void pad_draw(uint16_t *fb, const PadArt &art, const PadFrame &f)
{
    const std::vector<uint16_t> &bg = f.mode == PadFrame::kStory ? art.bg_story
                                      : f.mode == PadFrame::kEnemy ? art.bg_enemy
                                                                   : art.bg_idle;
    std::memcpy(fb, bg.data(), (size_t)kW * kH * 2);
    const Fb d{fb};
    d.sprite(art.close[f.close_pressed ? 1 : 0], L::kCloseX, L::kCloseY);
    if (f.mode == PadFrame::kIdle)
        return;
    // The name: font16, centred (its line and a 4-pixel margin), frameless.
    static const uint8_t kColours[3][3] = {{214, 190, 120}, {232, 80, 64}, {232, 228, 220}};
    const uint8_t *c = kColours[f.colour];
    const int nw = art.name_font.width(f.name);
    d.text(art.name_font, kW / 2 - (nw + 4) / 2, L::kNameY, f.name, c[0], c[1], c[2]);
    if (f.mode == PadFrame::kEnemy) {
        // The life bar: red over dark red, a lighter line along its top.
        d.fill(L::kHpX0, L::kHpY0, L::kHpX1 - 1, L::kHpY1, rgb565(48, 4, 4));
        const int64_t max = f.hp_max > 0 ? f.hp_max : 1, hp = std::max<int64_t>(0, std::min<int64_t>(f.hp, max));
        const int w = (int)((L::kHpX1 - L::kHpX0) * hp / max);
        if (w > 0) {
            d.fill(L::kHpX0, L::kHpY0, L::kHpX0 + w - 1, L::kHpY1, rgb565(196, 16, 12));
            d.fill(L::kHpX0, L::kHpY0, L::kHpX0 + w - 1, L::kHpY0, rgb565(240, 80, 60));
        }
        return;
    }
    if (f.mode != PadFrame::kStory)
        return;
    // The lines, fontformal10 at 1:1.
    for (size_t i = 0; i < f.lines.size(); i++)
        if (!f.lines[i].empty())
            d.text(art.text_font, L::kBoxX0 + L::kTextPadX, L::kBoxY0 + L::kTextPadY + (int)i * L::kLineH, f.lines[i],
                   232, 228, 220);
    // The scroll column: the arrows (9 up, 8 down; 11 and 10 pressed) and
    // the thumb (14 its top, 15's middle row stretched, 16's bottom).
    d.sprite(art.slider[f.up_pressed ? 11 : 9], L::kScrollX, L::kBoxY0);
    d.sprite(art.slider[f.down_pressed ? 10 : 8], L::kScrollX, L::kBoxY1 - L::kArrowH);
    int ty = 0, th = 0;
    L::thumb(f.top, f.total, ty, th);
    d.sprite(art.slider[14], L::kScrollX, ty);
    for (int y = ty + 4; y < ty + th - 4; y++)
        d.sprite(art.slider[15], L::kScrollX, y, 6, 7);
    d.sprite(art.slider[16], L::kScrollX, ty + th - 4, 9, 13);
    d.sprite(art.slider[14], L::kScrollX, ty, 0, 4);
}

}  // namespace game
