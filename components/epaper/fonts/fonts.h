#pragma once
#include "Gfx.h"

// The three faces used by Layout.cpp. Tables are defined in fonts.cpp — ONE
// translation unit, so the data lands in .rodata (flash) exactly once.
//
// Regenerate with: ./tools/fontgen/genfonts.sh

namespace epd {

extern const GFXfont Font_Big;    // the two temperatures, bold
extern const GFXfont Font_Label;  // INSIDE / OUTSIDE, bold
extern const GFXfont Font_Cond;   // forecast line, regular

}  // namespace epd
