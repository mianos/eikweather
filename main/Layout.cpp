#include "ScreenModel.h"

#include "Gfx.h"
#include "fonts.h"

// ScreenModel -> Canvas. Deliberately includes NO esp_* headers so that
// tools/preview can compile this exact file on the host. Keep it that way: it is
// the difference between a 0.2 s and a ~25 s layout iteration.
//
// Geometry (250 x 122 landscape). Labels sit left, the big readings are
// right-aligned so the digits line up between the two rows regardless of width
// ("7.8" and "-12.4" end at the same x):
//
//      x=0    74                                     232  246
//       +------+---------------------------------------+----+
//  y=20 |  IN                            2 1 . 4 °          |  label Font_Label
//  y=43 |                              Font_Big, 38px       |  temp  BLACK
//  y=65 |  OUT                            7 . 8 °           |
//  y=88 |                              right-aligned x=232  |  temp  RED
//  y=94 |===================================================|  RED rule
//  y=116|  Partly cloudy   6 / 17   rain 10%                |  Font_Cond
//       +---------------------------------------------------+
//
// Inside is BLACK and outside is RED. That is the one piece of colour that earns
// its place: which number is which is readable across a room without reading the
// labels at all.

namespace {

constexpr int kLabelX = 4;
constexpr int kInsideLabelBaseline = 20;
constexpr int kInsideTempBaseline = 43;
constexpr int kOutsideLabelBaseline = 65;
constexpr int kOutsideTempBaseline = 88;

constexpr int kTempRightX = 232;  // right edge of the digits; ring sits beyond
constexpr int kDegreeR = 4;
// 70, not 120: the label shares its row with the big digits, so every pixel it
// takes is stolen from the number. See the note in ScreenModel.h.
constexpr int kLabelMaxW = 70;

constexpr int kSepY = 94;
constexpr int kSepH = 3;
// ONE line, baseline 116 -> ink y=[101..120], inside the 97..121 band between the
// rule and the bottom edge. A baseline of 108 puts the ascenders into the rule,
// and there is no room for a second line.
constexpr int kFcBaseline = 116;
constexpr int kFcMaxW = 246;  // x=4..249, the full remaining width

// One reading: label on the left, big right-aligned number, degree ring beyond
// it. The ring is only drawn for a real value — "--" gets no degree sign.
void drawReading(epd::Canvas& c, const char* label, int labelBaseline,
                 const char* temp, int tempBaseline, bool valid,
                 epd::Color colour) {
  if (label && *label) {
    epd::drawTextClipped(c, epd::Font_Label, kLabelX, labelBaseline, kLabelMaxW,
                         label, epd::Color::Black);
  }
  if (temp && *temp) {
    epd::drawTextRight(c, epd::Font_Big, kTempRightX, tempBaseline, temp, colour);
    if (valid) {
      epd::drawDegree(c, kTempRightX + 2 + kDegreeR,
                      tempBaseline - 30 + kDegreeR, kDegreeR, colour);
    }
  }
}

void renderBanner(epd::Canvas& c, const ScreenModel& m) {
  // Boot / provisioning screen. Deliberately plain: this is what you stare at
  // when nothing else works, so it must not depend on MQTT, weather or layout
  // subtleties.
  epd::fillRect(c, 0, 0, c.width(), 6, epd::Color::Red);
  epd::drawTextClipped(c, epd::Font_Label, kLabelX, 40, c.width() - 8, m.banner,
                       epd::Color::Black);
  if (m.banner2 && *m.banner2) {
    epd::drawTextClipped(c, epd::Font_Cond, kLabelX, 70, c.width() - 8,
                         m.banner2, epd::Color::Black);
  }
}

}  // namespace

void renderScreen(epd::Canvas& c, const ScreenModel& m) {
  if (m.banner && *m.banner) {
    renderBanner(c, m);
    return;
  }

  drawReading(c, m.insideLabel, kInsideLabelBaseline, m.insideTemp,
              kInsideTempBaseline, m.insideValid, epd::Color::Black);
  drawReading(c, m.outsideLabel, kOutsideLabelBaseline, m.outsideTemp,
              kOutsideTempBaseline, m.outsideValid, epd::Color::Red);

  epd::fillRect(c, 0, kSepY, c.width(), kSepH, epd::Color::Red);

  if (m.forecast[0]) {
    epd::drawTextClipped(c, epd::Font_Cond, kLabelX, kFcBaseline, kFcMaxW,
                         m.forecast, epd::Color::Black);
  }
}
