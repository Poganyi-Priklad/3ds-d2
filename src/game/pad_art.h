// The PAD window's art (second_screen.h's PAD tab), decoded once from the
// player's own game files when the window first shows, and kept: nothing
// of Blizzard's is in this repository. From the MPQs (patch_d2, then
// d2exp, then d2data, as the game looks them up):
//   data\global\ui\BIGMENU\blankbckg.dc6       the background ("Diablo II")
//   data\global\ui\FrontEnd\PopUp_340x224.dc6  the frame's thin gold line
//   data\global\ui\PANEL\buysellbtn.DC6        the close button (frames 10, 11)
//   data\global\ui\MENU\textslid.DC6           the box's frame and its scroll column
//   data\local\font\<language>\font16, fontformal10 (.dc6 and .tbl)
//   data\global\palette\ACT1\pal.dat
// The language's folder is the game's own (CYR, LATIN...), so the fonts have
// every letter the game's text uses.
//
// The static parts are composed into three whole-screen backgrounds (idle;
// with the story's box; with the life bar's frame), already in the 3DS
// framebuffer's layout, so a redraw starts with one copy. The rest (the
// button, the arrows and thumb, the text) are small blits.
#ifndef D2_GAME_PAD_ART_H
#define D2_GAME_PAD_ART_H

#include "game/pad_story.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace game {

// A DC6 frame: palette indices and which pixels are drawn, row-major from
// the top.
struct Dc6Frame {
    int w = 0, h = 0;
    std::vector<uint8_t> px, mask;
};
// All of a DC6 file's frames; false if it is not one.
bool dc6_decode(const std::vector<uint8_t> &file, std::vector<Dc6Frame> &frames);

// A font: its glyphs by UTF-16 code (the .tbl), their pixels' brightness
// (the .dc6 through the palette), tinted when drawn.
struct PadFont {
    struct Glyph {
        uint16_t code = 0;
        uint8_t advance = 0, w = 0, h = 0;
        uint32_t off = 0;   // its w * h brightness values in lum (0: not drawn)
    };
    int cell_h = 0;                 // the tallest glyph: a line's height
    std::vector<Glyph> glyphs;      // by code
    std::vector<uint8_t> lum;
    const Glyph *find(char16_t code) const;
    int width(const std::u16string &s) const;
};
// The .tbl's glyphs over the .dc6's frames; false if either is not one.
bool pad_font_decode(const std::vector<uint8_t> &dc6, const std::vector<uint8_t> &tbl, const uint8_t pal[768],
                     PadFont &out);

// A sprite: RGB565 and which pixels are drawn, row-major.
struct PadSprite {
    int w = 0, h = 0;
    std::vector<uint16_t> rgb;
    std::vector<uint8_t> a;
};

struct PadArt {
    bool ok = false;
    std::string error;   // why not
    // 320x240 RGB565, column-major as the framebuffer (pixel (x, y) at
    // x * 240 + 239 - y).
    std::vector<uint16_t> bg_idle, bg_story, bg_enemy;
    PadSprite close[2];         // up, pressed
    PadSprite slider[17];       // textslid.DC6's frames
    PadFont name_font, text_font;
    size_t bytes() const;       // what it holds
};

// Reads one of the game's files whole ("data\\global\\..."); false if none.
using PadFileReader = std::function<bool(const std::string &name, std::vector<uint8_t> &out)>;
// Decodes and composes everything; `language` is the font folder ("CYR",
// "LATIN"...). False (art.error says why) if a file is missing or is not
// what it should be.
bool pad_art_build(PadArt &art, const PadFileReader &read, const std::string &language);

// Draws the window into a 320x240 RGB565 framebuffer (the 3DS layout).
void pad_draw(uint16_t *fb, const PadArt &art, const PadFrame &f);

}  // namespace game

#endif
