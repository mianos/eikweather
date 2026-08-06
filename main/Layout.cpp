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
//  y=22 |  IN    /\                     2 1 . 4 °           |  label Font_Label
//  y=34 |                             Font_Big, 32px        |  temp  BLACK
//  y=60 |  OUT   \/                        7 . 8 °          |
//  y=72 |                             right-aligned x=232   |  temp  RED
//            solid triangle: up if rising, down if falling,
//            nothing at all if steady or not yet known
//  y=76 |===================================================|  RED rule
//  y=94 |  Partly cloudy 6/17                          48°  |  Font_Cond
//  y=116|  Wed 5 Aug                            rain 10%    |  Font_Cond
//       +---------------------------------------------------+
//
// The forecast and the date each get their OWN full-width line. Sharing one line
// does not work: measured, the widest date is 113px and the widest forecast 245px
// against a 246px line, so one of them would always truncate. Font_Big was reduced
// 52pt -> 44pt to buy the second line.
//
// Both bottom lines carry a right-aligned passenger (the water temperature and
// the rain chance). In each case the passenger is drawn FIRST and the left item's
// budget shrinks around its measured width, so the left item truncates with ".."
// before they can ever collide. There is no vertical room for a third line: the
// band below the rule is 43px and Font_Cond needs ~19px a line.
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
// A bare 23x12 triangle. Odd width, and the height is not a free choice — it is
// w/2+1, which is what holds the edges at 45 degrees (see drawTrendArrow).
constexpr int kArrowW = 23;
constexpr int kArrowH = kArrowW / 2 + 1;
constexpr int kArrowGap = 10;  // clearance on each side
// The triangle is CENTRED on the digits' cap height, not stood on their baseline:
// Font_Big's caps run 31px above the baseline, so a 12px mark sitting on the
// baseline looks like it has slipped down. baseline - 10 puts its middle within a
// pixel of the caps' middle.
constexpr int kArrowLift = 10;

constexpr int kSepY = 76;
constexpr int kSepH = 3;
// Two lines between the rule (ends y=78) and the bottom edge (121): 43px for
// 2 x ~19px of Font_Cond. Baselines set by DESCENDER depth, not cap height.
constexpr int kFcBaseline = 94;
constexpr int kDateBaseline = 116;
constexpr int kFcMaxW = 246;  // x=4..249, the full remaining width
// Degree ring for the small water reading. r=2, not the big rows' r=4: it has to
// read as a degree sign against 20pt text, not 44pt.
constexpr int kSmallDegreeR = 2;
constexpr int kRightMargin = 4;
constexpr int kPassengerGap = 8;  // clearance between a line's two items

// Stale-data alert: a red "!" in the top-right corner.
//
// That corner is the only space on this screen that is free by CONSTRUCTION rather
// than by luck. The big readings right-align at kTempRightX (232) and their degree
// ring ends at 242, so nothing above the rule can ever reach x=243..249 — 7px wide
// by 76px tall. Everything else that looks empty is only the width that a
// particular date, forecast or label happened to leave: taking the union of inked
// pixels across every tools/preview case, the next-largest guaranteed-free box is
// 12px wide and it narrows further as labels lengthen. This is why the alert is a
// corner mark and not a badge on the date line.
//
// Font_Label's '!' rather than hand-drawn rectangles: Font_Label covers all of
// 0x20..0x7E, so a properly tapered exclamation mark already exists. Measured, its
// ink is 4px wide and 17px tall, sitting at +2..+5 from the text origin — so an
// origin of 241 puts the ink at 243..246, inside the column with 3px to spare.
//
// The baseline puts the top of the '!' at y=3, which is exactly the cap top of the
// inside digits, so it reads as aligned to the row rather than floating in space.
constexpr int kAlertX = 241;        // ink lands at x=243..246
constexpr int kAlertBaseline = 19;  // ink spans y=3..19

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
  // the triangle answers "is this going up or down", and an explicit "steady"
  // glyph is a third symbol to learn for the one case where the number alone
  // already says everything.
  const int dir = trend == Trend::Rising ? 1 : trend == Trend::Falling ? -1 : 0;
  if (dir == 0) return;

  // The number always wins the space. A long label plus a wide reading
  // ("LOUNGE" + "-12.4") can leave no room, and dropping the arrow is the right
  // trade — it is the decoration, the temperature is the point.
  const int tempLeft =
      kTempRightX - epd::measureText(epd::Font_Big, temp).advance;
  if (arrowX + kArrowW + kArrowGap > tempLeft) return;

  epd::drawTrendArrow(c, arrowX, tempBaseline - kArrowLift, kArrowW, dir, colour);
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

  // Red, making three red things on the screen (outside temperature, rule, alert)
  // — the stated budget before the tri-colour stops reading as an accent. An alert
  // is exactly what the third one should be spent on.
  if (m.alert) {
    epd::drawText(c, epd::Font_Label, kAlertX, kAlertBaseline, "!",
                  epd::Color::Red);
  }

  epd::fillRect(c, 0, kSepY, c.width(), kSepH, epd::Color::Red);

  // Both bottom lines: right-aligned passenger FIRST, then the left item with a
  // budget shrunk around the passenger's measured width.
  int fcBudget = kFcMaxW;
  if (m.water[0]) {
    // The ring sits beyond the digits, so the text right-aligns short of the
    // margin by the ring's full width.
    const int ringW = 2 * kSmallDegreeR + 1;
    const int ringCx = c.width() - kRightMargin - kSmallDegreeR;
    epd::drawTextRight(c, epd::Font_Cond, ringCx - kSmallDegreeR - 1, kFcBaseline,
                       m.water, epd::Color::Black);
    // -12: Font_Cond's cap height, so the ring aligns with the tops of the
    // digits rather than floating above them.
    epd::drawDegree(c, ringCx, kFcBaseline - 12, kSmallDegreeR, epd::Color::Black);
    fcBudget -= epd::measureText(epd::Font_Cond, m.water).advance + ringW + 1 +
                kPassengerGap;
    if (fcBudget < 40) fcBudget = 40;
  }
  if (m.forecast[0]) {
    epd::drawTextClipped(c, epd::Font_Cond, kLabelX, kFcBaseline, fcBudget,
                         m.forecast, epd::Color::Black);
  }

  int dateBudget = kFcMaxW;
  if (m.rain[0]) {
    epd::drawTextRight(c, epd::Font_Cond, c.width() - kRightMargin,
                       kDateBaseline, m.rain, epd::Color::Black);
    dateBudget -=
        epd::measureText(epd::Font_Cond, m.rain).advance + kPassengerGap;
    if (dateBudget < 40) dateBudget = 40;
  }
  if (m.date[0]) {
    epd::drawTextClipped(c, epd::Font_Cond, kLabelX, kDateBaseline, dateBudget,
                         m.date, epd::Color::Black);
  }
}
