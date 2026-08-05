#include "ScreenModel.h"

#include <initializer_list>

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
//  y=22 |  IN    ^                       2 1 . 4 °          |  label Font_Label
//  y=34 |        |                     Font_Big, 32px       |  temp  BLACK
//  y=60 |  OUT   |                        7 . 8 °           |
//  y=72 |        v                     right-aligned x=232  |  temp  RED
//            up if rising, down if falling, nothing otherwise
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

// The rise/fall arrow sits in the band between the label and the digits, which
// with the default "IN"/"OUT" labels was ~60px of dead space. Both rows share ONE
// arrow column, derived from the wider of the two labels, so the two arrows line
// up vertically — anchoring each to its own label or its own digits would leave
// them a few pixels out and read as a mistake.
//
// Bottom-aligned to the TEMPERATURE baseline, not the label's: the arrow modifies
// the number, so it should sit on the same line as the number.
constexpr int kArrowW = 21;    // odd: the stem lands on the exact centre column
constexpr int kArrowH = 30;
constexpr int kArrowGap = 10;  // clearance on each side

constexpr int kSepY = 76;
constexpr int kSepH = 3;
// Two lines between the rule (ends y=78) and the bottom edge (121): 43px for
// 2 x ~19px of Font_Cond. Baselines set by DESCENDER depth, not cap height.
constexpr int kFcBaseline = 94;
constexpr int kDateBaseline = 116;
constexpr int kFcMaxW = 246;  // x=4..249, the full remaining width

// x of the shared arrow column: past the WIDER of the two labels, so it clears
// both. Clamped to kLabelMaxW because that is where drawTextClipped truncates.
int arrowColumn(const ScreenModel& m) {
  int w = 0;
  for (const char* s : {m.insideLabel, m.outsideLabel}) {
    if (!s || !*s) continue;
    const int a = epd::measureText(epd::Font_Label, s).advance;
    if (a > w) w = a;
  }
  if (w > kLabelMaxW) w = kLabelMaxW;
  return kLabelX + w + kArrowGap;
}

// One reading: label on the left, trend arrow, big right-aligned number, degree
// ring beyond it. The ring is only drawn for a real value — "--" gets no degree
// sign, and no arrow either.
void drawReading(epd::Canvas& c, const char* label, int labelBaseline,
                 const char* temp, int tempBaseline, bool valid, Trend trend,
                 int arrowX, epd::Color colour) {
  if (label && *label) {
    epd::drawTextClipped(c, epd::Font_Label, kLabelX, labelBaseline, kLabelMaxW,
                         label, epd::Color::Black);
  }
  if (!temp || !*temp) return;

  epd::drawTextRight(c, epd::Font_Big, kTempRightX, tempBaseline, temp, colour);
  if (!valid) return;
  epd::drawDegree(c, kTempRightX + 2 + kDegreeR, tempBaseline - 25 + kDegreeR,
                  kDegreeR, colour);

  // Only a MOVING temperature gets a mark. Steady and Unknown both draw nothing:
  // the arrow answers "is this going up or down", and an explicit "steady" glyph
  // is a third symbol to learn for the one case where the number alone already
  // says everything.
  const int dir = trend == Trend::Rising ? 1 : trend == Trend::Falling ? -1 : 0;
  if (dir == 0) return;

  // The number always wins the space. A long label plus a wide reading
  // ("LOUNGE" + "-12.4") can leave no room, and dropping the arrow is the right
  // trade — it is the decoration, the temperature is the point.
  const int tempLeft =
      kTempRightX - epd::measureText(epd::Font_Big, temp).advance;
  if (arrowX + kArrowW + kArrowGap > tempLeft) return;

  epd::drawTrendArrow(c, arrowX, tempBaseline, kArrowW, kArrowH, dir, colour);
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

  const int arrowX = arrowColumn(m);
  drawReading(c, m.insideLabel, kInsideLabelBaseline, m.insideTemp,
              kInsideTempBaseline, m.insideValid, m.insideTrend, arrowX,
              epd::Color::Black);
  drawReading(c, m.outsideLabel, kOutsideLabelBaseline, m.outsideTemp,
              kOutsideTempBaseline, m.outsideValid, m.outsideTrend, arrowX,
              epd::Color::Red);

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
