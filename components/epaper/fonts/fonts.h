#pragma once
#include "Gfx.h"

// The four faces used by Layout.cpp. Tables are defined in fonts.cpp — ONE
// translation unit, so the data lands in .rodata (flash) exactly once.
//
// Regenerate with: ./tools/fontgen/genfonts.sh

namespace epd {

extern const GFXfont Font_Clock;  // big HH:MM digits, condensed bold
extern const GFXfont Font_Temp;   // temperature, bold
extern const GFXfont Font_Date;   // weekday + date, bold
extern const GFXfont Font_Cond;   // weather condition, regular

}  // namespace epd
