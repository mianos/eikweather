// Host-side layout preview. Compiles Layout.cpp / Gfx.cpp / fonts.cpp with clang
// and renders to PPM, so the entire layout can be settled without touching the
// hardware. A panel refresh costs ~25 s; this costs 0.2 s.
//
//   make run && open out-*.ppm
//
// It also asserts every width and vertical budget, which is the whole point: the
// layout constants in Layout.cpp are checked against MEASURED font metrics rather
// than estimates. This is what caught a descender being clipped off the bottom
// edge in the previous layout.

#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

#include "Canvas.h"
#include "Gfx.h"
#include "ScreenModel.h"
#include "Tenths.h"
#include "fonts.h"

namespace {

constexpr int kW = 250;
constexpr int kH = 122;

// These must track the constants in Layout.cpp. Duplicated deliberately rather
// than exported: they are layout policy, and the preview's job is to prove the
// chosen numbers are safe. Change one here, change it there.
constexpr int kTempRightX = 232;
constexpr int kLabelMaxW = 70;
constexpr int kFcMaxW = 246;
constexpr int kInsideLabelBaseline = 22;
constexpr int kInsideTempBaseline = 34;
constexpr int kOutsideLabelBaseline = 60;
constexpr int kOutsideTempBaseline = 72;
constexpr int kFcBaseline = 94;
constexpr int kDateBaseline = 116;
constexpr int kArrowW = 23;
constexpr int kArrowH = kArrowW / 2 + 1;  // 45-degree edges fix the height
constexpr int kArrowGap = 10;
constexpr int kArrowLift = 10;
constexpr int kAlertX = 241;
constexpr int kAlertBaseline = 19;

class MemCanvas final : public epd::Canvas {
 public:
  MemCanvas() { clear(); }
  int width() const override { return kW; }
  int height() const override { return kH; }
  void setPixel(int x, int y, epd::Color c) override {
    if (x < 0 || y < 0 || x >= kW || y >= kH) return;  // clip, like Panel
    px_[static_cast<size_t>(y) * kW + x] = static_cast<uint8_t>(c);
  }
  void clear() { memset(px_, static_cast<int>(epd::Color::White), sizeof px_); }

  // Off-panel reads as White so callers can probe a neighbourhood at the edges
  // without special-casing. Used by the alert-mark isolation check.
  epd::Color at(int x, int y) const {
    if (x < 0 || y < 0 || x >= kW || y >= kH) return epd::Color::White;
    return static_cast<epd::Color>(px_[static_cast<size_t>(y) * kW + x]);
  }

  void writePpm(const char* path) const {
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    constexpr int S = 3;  // 250x122 is tiny on a Retina display; problems hide at 1x
    fprintf(f, "P6\n%d %d\n255\n", kW * S, kH * S);
    for (int y = 0; y < kH; ++y)
      for (int s = 0; s < S; ++s)
        for (int x = 0; x < kW; ++x) {
          uint8_t r, g, b;
          switch (static_cast<epd::Color>(px_[static_cast<size_t>(y) * kW + x])) {
            case epd::Color::Black: r = g = b = 0x18; break;
            case epd::Color::Red:   r = 0xC8; g = 0x1E; b = 0x1E; break;
            default:                r = g = b = 0xF2; break;  // e-paper white
          }
          for (int t = 0; t < S; ++t) { fputc(r, f); fputc(g, f); fputc(b, f); }
        }
    fclose(f);
  }

  // Ink extent in a row band, so collisions are detected numerically rather than
  // by squinting at the render.
  void inkBounds(int y0, int y1, int* minX, int* maxX) const {
    *minX = kW; *maxX = -1;
    for (int y = y0; y <= y1 && y < kH; ++y)
      for (int x = 0; x < kW; ++x)
        if (px_[static_cast<size_t>(y) * kW + x]) {
          if (x < *minX) *minX = x;
          if (x > *maxX) *maxX = x;
        }
  }

 private:
  uint8_t px_[static_cast<size_t>(kW) * kH];
};

int fails = 0;

void checkWidth(const char* label, const epd::GFXfont& f, const char* s,
                int budget) {
  const int w = epd::measureText(f, s).advance;
  const bool ok = w <= budget;
  if (!ok) ++fails;
  printf("  %-20s %-22s %4d px / %4d %s\n", label, s, w, budget,
         ok ? "ok" : "*** OVERFLOWS ***");
}

void checkVertical(const char* label, const epd::GFXfont& f, const char* s,
                   int baseline, int topLimit, int bottomLimit) {
  const epd::TextMetrics m = epd::measureText(f, s);
  const int top = baseline + m.inkTop;     // inkTop is negative
  const int bot = baseline + m.inkBottom;  // positive for descenders
  const bool ok = top >= topLimit && bot <= bottomLimit;
  if (!ok) ++fails;
  printf("  %-20s %-18s baseline=%3d ink y=[%3d..%3d] allowed [%d..%d] %s\n",
         label, s, baseline, top, bot, topLimit, bottomLimit,
         ok ? "ok" : "*** CLIPS ***");
}

// Fixed-point conversions. Asserted on the host because the interesting cases are
// the ones you cannot reach by looking at a screen in August: the sign of
// sub-degree negatives, and rounding exactly on .5.
void checkTenths() {
  printf("\nfixed point (Tenths.h)\n");
  struct FmtCase { int tenths; const char* want; };
  const FmtCase fmt[] = {
      {173, "17.3"}, {0, "0.0"}, {-37, "-3.7"},
      // THE trap: -0.5 has tenths/10 == 0, so the sign has to come from the
      // sign of `tenths`, not from the integer division.
      {-5, "-0.5"},  {-1, "-0.1"}, {999, "99.9"},
      {-999, "-99.9"}, {1000, "100.0"},
  };
  for (const FmtCase& c : fmt) {
    char got[12];
    formatTenths(got, sizeof got, c.tenths);
    const bool ok = strcmp(got, c.want) == 0;
    if (!ok) ++fails;
    printf("  formatTenths(%5d) -> %-8s want %-8s %s\n", c.tenths, got, c.want,
           ok ? "ok" : "*** WRONG ***");
  }

  struct RoundCase { double deg; int tenths; int whole; };
  const RoundCase rc[] = {
      {17.34, 173, 17}, {17.35, 174, 17}, {-3.65, -37, -4},
      {0.05, 1, 0},     {-0.05, -1, 0},   {2.5, 25, 3},
      {-2.5, -25, -3},  // half away from zero in BOTH directions
      {1e9, 9999, 1000}, {-1e9, -999, -100},  // clamped, not overflowed
  };
  for (const RoundCase& c : rc) {
    const int t = tenthsFromDegrees(c.deg);
    const int w = wholeDegrees(t);
    const bool ok = t == c.tenths && w == c.whole;
    if (!ok) ++fails;
    printf("  %12.4g -> %5d tenths, %5d whole  want %5d / %5d  %s\n", c.deg, t,
           w, c.tenths, c.whole, ok ? "ok" : "*** WRONG ***");
  }
}

}  // namespace

// ScreenModel::operator== decides whether to spend ~25 s of flashing on a repaint,
// and it is defaulted, so nothing in the compiler checks that it does the right
// thing. Two properties are asserted here.
//
// The second one is the reason this exists: the old hand-written strcmp chain could
// silently omit a field, and the only symptom would have been that field never
// updating on the glass. Mutating each field in turn proves every one participates —
// across all three member kinds (char arrays, bools, and the Trend enum).
void checkModelEquality() {
  printf("\nScreenModel equality\n");
  // Written the way buildModel does it: a FRESH, fully-zeroed model each time, so
  // bytes past each NUL are deterministic and element-wise array comparison is safe.
  auto make = [](const char* forecast = "Partly cloudy 6/17") {
    ScreenModel m;
    snprintf(m.forecast, sizeof m.forecast, "%s", forecast);
    snprintf(m.insideLabel, sizeof m.insideLabel, "IN");
    snprintf(m.insideTemp, sizeof m.insideTemp, "21.4");
    m.insideValid = true;
    m.insideTrend = Trend::Rising;
    snprintf(m.outsideLabel, sizeof m.outsideLabel, "OUT");
    snprintf(m.outsideTemp, sizeof m.outsideTemp, "7.8");
    m.outsideValid = true;
    m.outsideTrend = Trend::Falling;
    m.forecastValid = true;
    snprintf(m.water, sizeof m.water, "48");
    snprintf(m.date, sizeof m.date, "Wed 5 Aug");
    snprintf(m.rain, sizeof m.rain, "rain 10%%");
    return m;
  };

  const bool identical = make() == make();
  if (!identical) ++fails;
  printf("  %-22s %s\n", "identical content",
         identical ? "equal, ok" : "*** UNEQUAL — SPURIOUS REPAINTS ***");

  // THE constraint behind the defaulted operator==, pinned so it cannot rot.
  //
  // Arrays compare element-wise, including bytes past the NUL, and snprintf does not
  // zero the tail. So a buffer that once held a LONGER string and was overwritten by
  // a shorter one differs, byte for byte, from a fresh buffer holding only the
  // shorter string — even though both render identically. That would cost a spurious
  // ~25 s repaint.
  //
  // It is safe today only because buildModel() fills a brand-new ScreenModel every
  // pass. This asserts the hazard is REAL, so that "just reuse one instance" is never
  // mistaken for a harmless optimisation. If it ever starts reporting equal, someone
  // has begun zeroing tails and the freshness rule could be relaxed.
  {
    ScreenModel reused = make();                    // held "Partly cloudy 6/17"
    snprintf(reused.forecast, sizeof reused.forecast, "Clear 6/17");
    const ScreenModel fresh = make("Clear 6/17");    // only ever held the short one
    const bool differs = !(reused == fresh);
    if (!differs) ++fails;
    printf("  %-22s %s\n", "stale tail bytes",
           differs ? "detected, ok — buildModel must use a fresh model per pass"
                   : "*** NOT DETECTED — the freshness comment is now wrong ***");
  }

  struct Mut { const char* name; void (*apply)(ScreenModel&); };
  const Mut muts[] = {
      {"insideLabel", [](ScreenModel& m) { snprintf(m.insideLabel, sizeof m.insideLabel, "LOUNGE"); }},
      {"insideTemp",  [](ScreenModel& m) { snprintf(m.insideTemp, sizeof m.insideTemp, "21.5"); }},
      {"insideValid", [](ScreenModel& m) { m.insideValid = false; }},
      {"insideTrend", [](ScreenModel& m) { m.insideTrend = Trend::Steady; }},
      {"outsideLabel",[](ScreenModel& m) { snprintf(m.outsideLabel, sizeof m.outsideLabel, "EXT"); }},
      {"outsideTemp", [](ScreenModel& m) { snprintf(m.outsideTemp, sizeof m.outsideTemp, "7.9"); }},
      {"outsideValid",[](ScreenModel& m) { m.outsideValid = false; }},
      {"outsideTrend",[](ScreenModel& m) { m.outsideTrend = Trend::Steady; }},
      {"forecast",    [](ScreenModel& m) { snprintf(m.forecast, sizeof m.forecast, "Clear 6/17"); }},
      {"forecastValid",[](ScreenModel& m) { m.forecastValid = false; }},
      {"water",       [](ScreenModel& m) { snprintf(m.water, sizeof m.water, "49"); }},
      {"date",        [](ScreenModel& m) { snprintf(m.date, sizeof m.date, "Thu 6 Aug"); }},
      {"rain",        [](ScreenModel& m) { snprintf(m.rain, sizeof m.rain, "rain 20%%"); }},
      {"alert",       [](ScreenModel& m) { m.alert = true; }},
  };
  int missed = 0;
  for (const Mut& mut : muts) {
    ScreenModel m = make();
    mut.apply(m);
    if (m == make()) {
      ++missed;
      printf("  %-22s *** NOT COMPARED — changes to it will never repaint ***\n",
             mut.name);
    }
  }
  if (missed) ++fails;
  printf("  %-22s %d of %d fields affect equality  %s\n", "field coverage",
         static_cast<int>(std::size(muts)) - missed, static_cast<int>(std::size(muts)),
         missed ? "*** FAILED ***" : "ok");
}

int main() {
  checkTenths();
  checkModelEquality();

  printf("font metrics\n");
  printf("  Font_Big yAdvance=%d  Font_Label yAdvance=%d  Font_Cond yAdvance=%d\n",
         epd::Font_Big.yAdvance, epd::Font_Label.yAdvance, epd::Font_Cond.yAdvance);

  // The temperature column is right-aligned at kTempRightX, so the budget is the
  // gap between the widest label and that edge.
  const int tempBudget = kTempRightX - 4 - kLabelMaxW;
  printf("\nwidth budgets (measured)\n");
  checkWidth("temp typical", epd::Font_Big, "21.4", tempBudget);
  checkWidth("temp 1 digit", epd::Font_Big, "7.8", tempBudget);
  checkWidth("temp negative", epd::Font_Big, "-12.4", tempBudget);
  checkWidth("temp 3 digit", epd::Font_Big, "100.0", tempBudget);
  checkWidth("temp unknown", epd::Font_Big, "--", tempBudget);
  checkWidth("label IN", epd::Font_Label, "IN", kLabelMaxW);
  checkWidth("label OUT", epd::Font_Label, "OUT", kLabelMaxW);
  // Forecast format is "<condition> <lo>/<hi>". The longest WMO strings are 13
  // chars ("Partly cloudy", "Heavy drizzle", "Heavy showers"), so the realistic
  // worst case is checked here. drawTextClipped is the backstop for anything
  // pathological — it truncates with ".." rather than overflowing.
  checkWidth("fc typical", epd::Font_Cond, "Clear 6/16", kFcMaxW);
  checkWidth("fc worst", epd::Font_Cond, "Heavy showers -9/45", kFcMaxW);
  // The date line is shared: date left, labelled rain chance right-aligned.
  const int rainW = epd::measureText(epd::Font_Cond, "rain 100%").advance;
  printf("  %-20s %-22s %4d px (date budget becomes %d)\n", "rain widest",
         "rain 100%", rainW, kFcMaxW - rainW - 8);
  checkWidth("date widest", epd::Font_Cond, "Wed 28 May", kFcMaxW - rainW - 8);

  // The tank temperature rides on the forecast line, UNLABELLED, so the forecast's
  // budget shrinks by its width. This is the tightest line on the screen and the
  // reason the label is off by default: "100" costs 39px and still leaves room for
  // the pathological forecast, whereas "HWS 100" costs 91px and does not.
  const int waterW = epd::measureText(epd::Font_Cond, "100").advance + 5 + 1;
  const int fcBudget = kFcMaxW - waterW - 8;
  printf("  %-20s %-22s %4d px (forecast budget becomes %d)\n", "water + ring",
         "100 deg (3 digit)", waterW, fcBudget);
  checkWidth("fc typical +water", epd::Font_Cond, "Clear 6/16", fcBudget);
  checkWidth("fc 13ch +water", epd::Font_Cond, "Partly cloudy 6/17", fcBudget);
  checkWidth("fc worst +water", epd::Font_Cond, "Heavy showers -9/45", fcBudget);
  checkWidth("fc failed +water", epd::Font_Cond, "forecast unavailable", fcBudget);
  // What a label would cost, reported rather than asserted: water_label is a
  // supported setting, it just spends the forecast's width.
  const int labelledW = epd::measureText(epd::Font_Cond, "HWS 100").advance + 6;
  printf("  %-20s %-22s %4d px (would leave %d — 13-char conditions truncate)\n",
         "water WITH label", "HWS 100 deg", labelledW,
         kFcMaxW - labelledW - 8);

  printf("\nvertical budgets (kH=%d, rule at y=76..78)\n", kH);
  checkVertical("inside label", epd::Font_Label, "IN", kInsideLabelBaseline, 0, 75);
  checkVertical("inside temp", epd::Font_Big, "-12.4", kInsideTempBaseline, 0, 75);
  checkVertical("outside label", epd::Font_Label, "OUT", kOutsideLabelBaseline, 0, 75);
  checkVertical("outside temp", epd::Font_Big, "-12.4", kOutsideTempBaseline, 0, 75);
  checkVertical("forecast desc", epd::Font_Cond, "Heavy drizzle 2/11", kFcBaseline, 79, 121);
  checkVertical("rain desc", epd::Font_Cond, "rain 100%", kDateBaseline, 79, 121);
  checkVertical("date desc", epd::Font_Cond, "Wed 28 Sep", kDateBaseline, 79, 121);

  // Row separation: the inside block must not touch the outside block, and the
  // two forecast lines must not touch each other.
  {
    const epd::TextMetrics a = epd::measureText(epd::Font_Big, "-12.4");
    const int insideBot = kInsideTempBaseline + a.inkBottom;
    const int outsideTop = kOutsideLabelBaseline + epd::measureText(epd::Font_Label, "OUT").inkTop;
    const bool ok = outsideTop > insideBot;
    if (!ok) ++fails;
    printf("  %-20s inside bottom=%d  outside label top=%d  %s\n", "row separation",
           insideBot, outsideTop, ok ? "ok" : "*** ROWS COLLIDE ***");
  }
  {
    const int fcBot = kFcBaseline + epd::measureText(epd::Font_Cond, "Heavy drizzle 2/11").inkBottom;
    const int dTop = kDateBaseline + epd::measureText(epd::Font_Cond, "Wed 28 Sep").inkTop;
    const bool ok = dTop > fcBot;
    if (!ok) ++fails;
    printf("  %-20s forecast bottom=%d  date top=%d  %s\n", "small-line sep",
           fcBot, dTop, ok ? "ok" : "*** LINES COLLIDE ***");
  }
  // The arrow column must fit between the wider of the two labels and the LEFT
  // edge of the widest reading, or the arrow is silently dropped. Checked here
  // rather than eyeballed, because dropping it is a legitimate fallback for long
  // custom labels and would otherwise pass unnoticed with the defaults.
  printf("\ntrend arrow column\n");
  {
    const int labelW = epd::measureText(epd::Font_Label, "OUT").advance;
    const int arrowX = 4 + labelW + kArrowGap;
    const int tempLeft = kTempRightX - epd::measureText(epd::Font_Big, "-12.4").advance;
    const bool ok = arrowX + kArrowW + kArrowGap <= tempLeft;
    if (!ok) ++fails;
    printf("  %-20s x=%d..%d  widest number starts at %d  %s\n", "IN/OUT labels",
           arrowX, arrowX + kArrowW, tempLeft,
           ok ? "ok" : "*** ARROW WILL BE DROPPED ***");
    // Vertical: the triangle is centred on the cap height, so its bottom sits
    // kArrowLift above the baseline and it reaches kArrowH-1 further up. It must
    // stay inside the panel and must not climb into the row above.
    const int top = kInsideTempBaseline - kArrowLift - kArrowH + 1;
    const bool vok = top >= 0 && kOutsideTempBaseline - kArrowLift - kArrowH + 1 >
                                     kInsideTempBaseline;
    if (!vok) ++fails;
    printf("  %-20s inside y=[%d..%d] outside y=[%d..%d] %s\n", "vertical",
           top, kInsideTempBaseline - kArrowLift,
           kOutsideTempBaseline - kArrowLift - kArrowH + 1,
           kOutsideTempBaseline - kArrowLift, vok ? "ok" : "*** ARROWS COLLIDE ***");
  }

  // The stale-data "!" lives in the ONLY region of this screen that is free by
  // construction rather than by luck: right of the degree ring, above the rule.
  // Both halves of that claim are asserted, because the mark is drawn
  // unconditionally with no fallback — unlike the trend arrow, which measures and
  // drops itself. If it ever overlaps, it overlaps in silence on the glass.
  printf("\nstale-data alert mark\n");
  {
    const epd::TextMetrics bang = epd::measureText(epd::Font_Label, "!");
    // Ink, not advance: the glyph is inset from its origin, which is what makes
    // 4px of ink fit a 7px column.
    const int inkL = kAlertX + bang.inkLeft;
    const int inkR = kAlertX + bang.inkRight;
    // The widest possible thing to its left is a big reading's degree ring, whose
    // right edge is fixed by kTempRightX regardless of how wide the number is.
    const int ringRight = kTempRightX + 2 + 2 * 4;  // kDegreeR = 4
    const bool hok = inkL > ringRight && inkR <= kW - 1;
    if (!hok) ++fails;
    printf("  %-20s ink x=%d..%d  ring ends at %d  panel edge %d  %s\n",
           "horizontal", inkL, inkR, ringRight, kW - 1,
           hok ? "ok" : "*** ALERT MARK COLLIDES ***");
    const int inkT = kAlertBaseline + bang.inkTop;
    const int inkB = kAlertBaseline + bang.inkBottom;
    // Must clear the rule, and must not run off the top edge. Aligning its top with
    // the inside digits' cap top is a deliberate choice, so that is checked too
    // rather than left to drift if a font is regenerated.
    const int capTop = kInsideTempBaseline + epd::measureText(epd::Font_Big, "21.4").inkTop;
    const bool vok = inkT >= 0 && inkB < 76;
    if (!vok) ++fails;
    printf("  %-20s ink y=%d..%d  rule at 76  digit cap top=%d%s  %s\n", "vertical",
           inkT, inkB, capTop, inkT == capTop ? " (aligned)" : " (NOT aligned)",
           vok ? "ok" : "*** ALERT MARK OUT OF BAND ***");
  }
  struct Case { const char* name; ScreenModel m; };
  std::vector<Case> cases;

  auto mk = [](const char* name, const char* inL, const char* inT, bool inV,
               Trend inTr, const char* outL, const char* outT, bool outV,
               Trend outTr, const char* fc, const char* date, const char* rain,
               const char* water) {
    Case c{name, {}};
    snprintf(c.m.insideLabel, sizeof c.m.insideLabel, "%s", inL);
    snprintf(c.m.insideTemp, sizeof c.m.insideTemp, "%s", inT);
    c.m.insideValid = inV;
    c.m.insideTrend = inTr;
    snprintf(c.m.outsideLabel, sizeof c.m.outsideLabel, "%s", outL);
    snprintf(c.m.outsideTemp, sizeof c.m.outsideTemp, "%s", outT);
    c.m.outsideValid = outV;
    c.m.outsideTrend = outTr;
    snprintf(c.m.forecast, sizeof c.m.forecast, "%s", fc);
    snprintf(c.m.date, sizeof c.m.date, "%s", date);
    snprintf(c.m.rain, sizeof c.m.rain, "%s", rain);
    snprintf(c.m.water, sizeof c.m.water, "%s", water);
    return c;
  };

  cases.push_back(mk("normal", "IN", "21.4", true, Trend::Rising, "OUT", "7.8",
                     true, Trend::Falling, "Partly cloudy 6/17", "Wed 5 Aug",
                     "rain 10%", "48"));
  cases.push_back(mk("widest", "IN", "-12.4", true, Trend::Falling, "OUT",
                     "100.0", true, Trend::Rising, "Heavy showers -9/45",
                     "Wed 28 May", "rain 100%", "100"));
  cases.push_back(mk("no-mqtt", "IN", "--", false, Trend::Unknown, "OUT", "--",
                     false, Trend::Unknown, "Clear 6/17", "", "", ""));
  // Steady (measured flat) and Unknown (no history yet) are both blank, so this
  // case must render with an empty arrow column on both rows. Also the case where
  // the water reading is absent — the forecast should get the full width back.
  cases.push_back(mk("steady", "IN", "19.0", true, Trend::Steady, "OUT", "3.2",
                     true, Trend::Unknown, "Heavy drizzle 2/11", "Sat 12 Jul",
                     "rain 90%", ""));
  // Long labels squeeze the arrow column; the number must still be intact.
  cases.push_back(mk("longlabel", "LOUNGE", "19.0", true, Trend::Rising,
                     "OUTSIDE", "-12.4", true, Trend::Falling, "Clear 2/11",
                     "Sat 12 Jul", "rain 90%", "HWS 48"));
  // The alert on the WIDEST content, not on typical content: the mark has no
  // fallback (unlike the trend arrow, which measures and drops itself), so the case
  // that matters is the one where everything else is at maximum extent and the
  // corner is under the most pressure.
  {
    Case c = mk("alert", "IN", "-12.4", true, Trend::Falling, "OUT", "100.0", true,
                Trend::Rising, "Heavy showers -9/45", "Wed 28 May", "rain 100%",
                "100");
    c.m.alert = true;
    cases.push_back(c);

    // The bounding-box checks above can pass while the glyphs still visually
    // collide: the degree ring's right edge reaches x=242 and the mark's ink starts
    // at 243, so a box check only proves they do not OVERLAP, not that there is any
    // daylight between them. Assert the real invariant — render with and without
    // the mark, and require every pixel it adds to be at least 2px from any
    // pre-existing ink, diagonals included.
    ScreenModel without = c.m;
    without.alert = false;
    MemCanvas a, b;
    renderScreen(a, without);
    renderScreen(b, c.m);
    int added = 0, minGap = 99;
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x) {
        if (a.at(x, y) == b.at(x, y)) continue;
        ++added;
        for (int d = 1; d < minGap; ++d) {
          bool hit = false;
          for (int dy = -d; dy <= d && !hit; ++dy) {
            for (int dx = -d; dx <= d && !hit; ++dx) {
              // Ring of radius d only — inner rings were tested on earlier passes.
              if (dx != d && dx != -d && dy != d && dy != -d) continue;
              if (a.at(x + dx, y + dy) != epd::Color::White) hit = true;
            }
          }
          if (hit) { minGap = d; break; }
        }
      }
    }
    // minGap == 1 means directly adjacent to existing ink, which reads as touching.
    const bool ok = added > 0 && minGap >= 2;
    if (!ok) ++fails;
    printf("  %-20s %d px added, nearest other ink %d px  %s\n", "isolation", added,
           minGap, ok ? "ok" : "*** ALERT MARK TOUCHES OTHER INK ***");
  }
  {
    Case c{"banner", {}};
    c.m.banner = "einkweather";
    c.m.banner2 = "Run ESP-Touch v2 to set up Wi-Fi";
    cases.push_back(c);
  }

  printf("\nrendered cases\n");
  for (const auto& c : cases) {
    MemCanvas cv;
    renderScreen(cv, c.m);
    char ppm[128];
    snprintf(ppm, sizeof ppm, "out-%s.ppm", c.name);
    cv.writePpm(ppm);
    int lo, hi;
    cv.inkBounds(0, kH - 1, &lo, &hi);
    printf("  %-10s -> %-20s ink x=[%d..%d]%s\n", c.name, ppm, lo, hi,
           hi > 249 ? "  *** CLIPPED ***" : "");
  }

  printf("\n%s (%d budget failure%s)\n", fails ? "FAILED" : "all budgets ok",
         fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
