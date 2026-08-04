#include "ScreenModel.h"

#include "Gfx.h"
#include "fonts.h"

// ScreenModel -> Canvas. Deliberately includes NO esp_* headers so that
// tools/preview can compile this exact file on the host. Keep it that way: it is
// the difference between a 0.2 s and a 30 s layout iteration.
//
// Geometry (250 x 122 landscape, measured metrics from tools/fontgen):
//
//      x=0                                 168     177        249
//       +-----------------------------------+-------+-----------+
//  y=5  |                                                       |
//       |          2 3 : 5 9      Font_Clock 92pt, 67px figures  |  BLACK
//  y=70 |          centred on cx=125, baseline y=70              |  ':' in RED
//  y=76 |=======================================================|  RED rule
//  y=79 |                                   |                   |
//  y=97 |  Wed 24 Sep                       |      2 1 °        |  date BLACK
//       |  left column, maxW=164            |    temp RED       |
//  y=119|  Partly cloudy                    |   baseline 114    |  cond BLACK
//  y=121+-----------------------------------+-------------------+
//
// The right column is reserved for the temperature across BOTH lower rows, so the
// left column is capped rather than given the full width — otherwise the date
// collides with the temperature.
//
// kLeftMaxW is 164, not 174, because of a measured case: "-15" at Font_Temp is
// 55px, which right-aligned at x=232 starts at x=177. A 174px left column reaches
// x=178 and would overlap by 2px. 164 leaves an 8px gutter.

namespace {

constexpr int kSepY = 76;      // red rule, 3px tall (76..78)
constexpr int kSepH = 3;
constexpr int kLeftX = 4;
constexpr int kLeftMaxW = 164;  // see the note above — do not raise without
                                // re-running tools/preview
// Baselines are set by DESCENDER depth, not cap height. Measured at these values
// (tools/preview "vertical budgets"): date ink y=[80..101] including the 'p' of
// "Sep", condition ink y=[104..121] including the 'y' of "Partly cloudy". A
// condition baseline of 119 — the obvious choice from cap height alone — pushes
// that 'y' to y=123 and clips it off the bottom of the 122px panel.
constexpr int kDateBaseline = 96;
constexpr int kCondBaseline = 117;

constexpr int kTempRightX = 232;   // right edge of the digits (ring sits beyond)
constexpr int kTempBaseline = 114;
constexpr int kDegreeR = 4;

void renderBanner(epd::Canvas& c, const ScreenModel& m) {
  // Boot / provisioning screen. Deliberately plain: this is what you stare at
  // when nothing else works, so it must not depend on weather, time or layout
  // subtleties.
  epd::fillRect(c, 0, 0, c.width(), 6, epd::Color::Red);
  epd::drawTextClipped(c, epd::Font_Date, kLeftX, 40, c.width() - 8, m.banner,
                       epd::Color::Black);
  if (m.banner2 && *m.banner2) {
    epd::drawTextClipped(c, epd::Font_Cond, kLeftX, 70, c.width() - 8, m.banner2,
                         epd::Color::Black);
  }
}

}  // namespace

void renderScreen(epd::Canvas& c, const ScreenModel& m) {
  if (m.banner && *m.banner) {
    renderBanner(c, m);
    return;
  }

  // --- the clock ---------------------------------------------------------
  // Drawn in two passes so the ':' can be red: the whole string in black
  // first, then the colon repainted at its measured offset. Red pixels also set
  // the BW plane, so the second pass cleanly overwrites the black colon.
  if (m.clock[0]) {
    const int advance = epd::measureText(epd::Font_Clock, m.clock).advance;
    const int x = c.width() / 2 - advance / 2;
    epd::drawText(c, epd::Font_Clock, x, 70, m.clock, epd::Color::Black);

    const int colonDx = epd::glyphOffsetOf(epd::Font_Clock, m.clock, ':');
    if (colonDx >= 0) {
      epd::drawText(c, epd::Font_Clock, x + colonDx, 70, ":", epd::Color::Red);
    }
  }

  // --- separator --------------------------------------------------------
  // Dashed => the weather data is stale. This costs zero layout space, which is
  // why there is no status footer: it is unmistakable once you know it and
  // invisible noise if you don't.
  if (m.stale) {
    for (int i = 0; i < kSepH; ++i)
      epd::drawDashedHLine(c, 0, kSepY + i, c.width(), 6, 6, epd::Color::Red);
  } else {
    epd::fillRect(c, 0, kSepY, c.width(), kSepH, epd::Color::Red);
  }

  // --- date -------------------------------------------------------------
  if (m.date[0]) {
    epd::drawTextClipped(c, epd::Font_Date, kLeftX, kDateBaseline, kLeftMaxW,
                         m.date, epd::Color::Black);
  }

  // --- condition --------------------------------------------------------
  if (m.cond[0]) {
    epd::drawTextClipped(c, epd::Font_Cond, kLeftX, kCondBaseline, kLeftMaxW,
                         m.cond, epd::Color::Black);
  }

  // --- temperature ------------------------------------------------------
  if (m.temp[0]) {
    epd::drawTextRight(c, epd::Font_Temp, kTempRightX, kTempBaseline, m.temp,
                       epd::Color::Red);
    // Only ring a real reading. "--" (never fetched) gets no degree sign.
    if (m.haveTemp) {
      epd::drawDegree(c, kTempRightX + 2 + kDegreeR,
                      kTempBaseline - 22 + kDegreeR, kDegreeR, epd::Color::Red);
    }
  }
}
