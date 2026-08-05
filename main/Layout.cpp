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
//  y=22 |  IN                            2 1 . 4 °          |  label Font_Label
//  y=34 |                              Font_Big, 32px       |  temp  BLACK
//  y=60 |  OUT                            7 . 8 °           |
//  y=72 |                              right-aligned x=232  |  temp  RED
//  y=76 |===================================================|  RED rule
//  y=94 |  Partly cloudy 6/17                               |  Font_Cond
//  y=116|  Wed 5 Aug                            rain 10%    |  Font_Cond
//       +---------------------------------------------------+
//
// The forecast and the date each get their OWN full-width line. Sharing one line
// does not work: measured, the widest date is 113px and the widest forecast 245px
// against a 246px line, so one of them would always truncate. Font_Big was reduced
// 52pt -> 44pt to buy the second line.
//
// Inside is BLACK and outside is RED. That is the one piece of colour that earns
// its place: which number is which is readable across a room without reading the
// labels at all.

namespace {

constexpr int kLabelX = 4;
constexpr int kInsideLabelBaseline = 22;
constexpr int kInsideTempBaseline = 34;
constexpr int kOutsideLabelBaseline = 60;
constexpr int kOutsideTempBaseline = 72;

constexpr int kTempRightX = 232;  // right edge of the digits; ring sits beyond
constexpr int kDegreeR = 4;
// 70, not 120: the label shares its row with the big digits, so every pixel it
// takes is stolen from the number. See the note in ScreenModel.h.
constexpr int kLabelMaxW = 70;

constexpr int kSepY = 76;
constexpr int kSepH = 3;
// Two lines between the rule (ends y=78) and the bottom edge (121): 43px for
// 2 x ~19px of Font_Cond. Baselines set by DESCENDER depth, not cap height.
constexpr int kFcBaseline = 94;
constexpr int kDateBaseline = 116;
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
                      tempBaseline - 25 + kDegreeR, kDegreeR, colour);
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
  // Rain chance is drawn first and right-aligned, and the date's budget shrinks
  // around its MEASURED width, so the date truncates before they can collide.
  int dateBudget = kFcMaxW;
  if (m.rain[0]) {
    epd::drawTextRight(c, epd::Font_Cond, c.width() - 4, kDateBaseline, m.rain,
                       epd::Color::Black);
    dateBudget -= epd::measureText(epd::Font_Cond, m.rain).advance + 8;
    if (dateBudget < 40) dateBudget = 40;
  }
  if (m.date[0]) {
    epd::drawTextClipped(c, epd::Font_Cond, kLabelX, kDateBaseline, dateBudget,
                         m.date, epd::Color::Black);
  }
}
