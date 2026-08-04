// Host-side layout preview. Compiles Layout.cpp / Gfx.cpp / fonts.cpp with clang
// and renders to PBM + PNG-ish PPM, so the entire layout can be settled without
// touching the hardware. A panel refresh costs ~30 s; this costs 0.2 s.
//
//   make && ./preview && open out-*.ppm
//
// It also prints the width budget for every string it draws, which is the whole
// point: the layout constants in Layout.cpp are checked against MEASURED font
// metrics rather than estimates.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Canvas.h"
#include "Gfx.h"
#include "ScreenModel.h"
#include "fonts.h"

namespace {

constexpr int kW = 250;
constexpr int kH = 122;

// These must track the constants in Layout.cpp. Duplicated deliberately rather
// than exported: they are layout policy, and the preview's job is to prove the
// chosen numbers are safe. If you change one here, change it there.
constexpr int kLeftMaxW = 164;
constexpr int kClockBaseline = 70;
constexpr int kDateBaseline = 96;
constexpr int kCondBaseline = 117;
constexpr int kTempBaseline = 114;

// Byte-per-pixel Canvas, matching the panel's logical landscape orientation.
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

  void writePpm(const char* path) const {
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    // 3x nearest-neighbour upscale — 250x122 is uncomfortably small on a Retina
    // display and stroke problems hide at 1x.
    constexpr int S = 3;
    fprintf(f, "P6\n%d %d\n255\n", kW * S, kH * S);
    for (int y = 0; y < kH; ++y) {
      for (int s = 0; s < S; ++s) {
        for (int x = 0; x < kW; ++x) {
          uint8_t r, g, b;
          switch (static_cast<epd::Color>(px_[static_cast<size_t>(y) * kW + x])) {
            case epd::Color::Black: r = g = b = 0x18; break;
            case epd::Color::Red:   r = 0xC8; g = 0x1E; b = 0x1E; break;
            default:                r = g = b = 0xF2; break;  // e-paper white
          }
          for (int t = 0; t < S; ++t) { fputc(r, f); fputc(g, f); fputc(b, f); }
        }
      }
    }
    fclose(f);
  }

  void writePbm(const char* path) const {
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P1\n%d %d\n", kW, kH);
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x)
        fprintf(f, "%d ", px_[static_cast<size_t>(y) * kW + x] ? 1 : 0);
      fputc('\n', f);
    }
    fclose(f);
  }

  // Ink extent, so overlap between the left column and the temperature block can
  // be detected numerically rather than by squinting.
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

void checkWidth(const char* label, const epd::GFXfont& f, const char* s,
                int budget) {
  const int w = epd::measureText(f, s).advance;
  printf("  %-22s %-24s %4d px / %4d %s\n", label, s, w, budget,
         w <= budget ? "ok" : "*** OVERFLOWS ***");
}

// Vertical extent is the thing that actually bit us: a baseline chosen from cap
// height alone clips descenders ('y' in "Partly cloudy", 'p' in "Sep") straight
// off the bottom edge of a 122px panel. Check it numerically.
void checkVertical(const char* label, const epd::GFXfont& f, const char* s,
                   int baseline, int topLimit, int bottomLimit) {
  const epd::TextMetrics m = epd::measureText(f, s);
  const int top = baseline + m.inkTop;      // inkTop is negative
  const int bot = baseline + m.inkBottom;   // positive for descenders
  const bool ok = top >= topLimit && bot <= bottomLimit;
  printf("  %-22s %-20s baseline=%3d  ink y=[%3d..%3d]  allowed [%d..%d] %s\n",
         label, s, baseline, top, bot, topLimit, bottomLimit,
         ok ? "ok" : "*** CLIPS ***");
}

}  // namespace

int main() {
  printf("font metrics\n");
  printf("  Font_Clock yAdvance=%d  Font_Temp yAdvance=%d  "
         "Font_Date yAdvance=%d  Font_Cond yAdvance=%d\n",
         epd::Font_Clock.yAdvance, epd::Font_Temp.yAdvance,
         epd::Font_Date.yAdvance, epd::Font_Cond.yAdvance);

  printf("\nwidth budgets (measured, not estimated)\n");
  checkWidth("clock widest", epd::Font_Clock, "23:59", 244);
  checkWidth("clock 00:00", epd::Font_Clock, "00:00", 244);
  checkWidth("clock 11:11", epd::Font_Clock, "11:11", 244);
  checkWidth("date worst", epd::Font_Date, "Wed 24 Sep", kLeftMaxW);
  checkWidth("cond worst", epd::Font_Cond, "Heavy showers", kLeftMaxW);
  checkWidth("cond worst2", epd::Font_Cond, "Partly cloudy", kLeftMaxW);
  checkWidth("cond worst3", epd::Font_Cond, "Freezing fog", kLeftMaxW);
  checkWidth("temp widest", epd::Font_Temp, "-15", 56);

  // Exhaustive sweep of every weekday x month combination against the column
  // budget, so the worst case is known rather than assumed.
  printf("\ndate worst case over all weekday/month combinations (budget %d)\n",
         kLeftMaxW);
  const char* days[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
  const char* mons[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int worst = 0;
  char worstBuf[64] = {};
  for (const char* d : days) {
    for (const char* mo : mons) {
      char buf[64];
      snprintf(buf, sizeof buf, "%s 28 %s", d, mo);
      const int w = epd::measureText(epd::Font_Date, buf).advance;
      if (w > worst) { worst = w; snprintf(worstBuf, sizeof worstBuf, "%s", buf); }
    }
  }
  printf("  %-24s %4d px / %4d %s\n", worstBuf, worst, kLeftMaxW,
         worst <= kLeftMaxW ? "ok" : "*** OVERFLOWS ***");

  // Every WMO condition string against its column, since the table in
  // WeatherClient.cpp is hand-written and easy to extend past the budget.
  printf("\nWMO condition strings (budget %d)\n", kLeftMaxW);
  const char* conds[] = {
      "Clear", "Mainly clear", "Partly cloudy", "Overcast", "Fog",
      "Freezing fog", "Light drizzle", "Drizzle", "Heavy drizzle",
      "Icy drizzle", "Light rain", "Rain", "Heavy rain", "Icy rain",
      "Light snow", "Snow", "Heavy snow", "Snow grains", "Light showers",
      "Showers", "Heavy showers", "Snow showers", "Thunderstorm",
      "Storm, hail", "Severe storm"};
  int condWorst = 0;
  char condWorstBuf[64] = {};
  for (const char* s : conds) {
    const int w = epd::measureText(epd::Font_Cond, s).advance;
    if (w > condWorst) {
      condWorst = w;
      snprintf(condWorstBuf, sizeof condWorstBuf, "%s", s);
    }
  }
  printf("  %-24s %4d px / %4d %s\n", condWorstBuf, condWorst, kLeftMaxW,
         condWorst <= kLeftMaxW ? "ok" : "*** OVERFLOWS ***");

  // --- vertical budgets --------------------------------------------------
  // The clock must clear the top edge and stay above the y=76 rule; the two lower
  // rows must fit between the rule and the bottom edge without colliding.
  printf("\nvertical budgets (kH=%d, rule at y=76..78)\n", kH);
  checkVertical("clock", epd::Font_Clock, "23:59", kClockBaseline, 0, 75);
  checkVertical("date ascender", epd::Font_Date, "Wed 24 Sep", kDateBaseline, 79, 121);
  checkVertical("date descender", epd::Font_Date, "Sep", kDateBaseline, 79, 121);
  checkVertical("cond descender", epd::Font_Cond, "Partly cloudy", kCondBaseline, 79, 121);
  checkVertical("cond descender2", epd::Font_Cond, "Light drizzle", kCondBaseline, 79, 121);
  checkVertical("temp", epd::Font_Temp, "-15", kTempBaseline, 79, 121);

  // Date and condition must not overlap vertically.
  {
    const epd::TextMetrics d = epd::measureText(epd::Font_Date, "Sep");
    const epd::TextMetrics c = epd::measureText(epd::Font_Cond, "Partly cloudy");
    const int dBot = kDateBaseline + d.inkBottom;
    const int cTop = kCondBaseline + c.inkTop;
    printf("  %-22s date bottom=%d  cond top=%d  %s\n", "row separation", dBot,
           cTop, cTop > dBot ? "ok" : "*** ROWS COLLIDE ***");
  }

  struct Case {
    const char* name;
    ScreenModel m;
  };
  std::vector<Case> cases;

  {   // the normal, everyday screen
    Case c{"normal", {}};
    snprintf(c.m.clock, sizeof c.m.clock, "%s", "14:35");
    snprintf(c.m.date, sizeof c.m.date, "%s", "Wed 24 Sep");
    snprintf(c.m.cond, sizeof c.m.cond, "%s", "Partly cloudy");
    snprintf(c.m.temp, sizeof c.m.temp, "%s", "21");
    c.m.haveTemp = true;
    cases.push_back(c);
  }
  {   // widest possible everything
    Case c{"widest", {}};
    snprintf(c.m.clock, sizeof c.m.clock, "%s", "23:59");
    snprintf(c.m.date, sizeof c.m.date, "%s", "Wed 28 Sep");
    snprintf(c.m.cond, sizeof c.m.cond, "%s", "Heavy showers");
    snprintf(c.m.temp, sizeof c.m.temp, "%s", "-15");
    c.m.haveTemp = true;
    cases.push_back(c);
  }
  {   // weather never fetched
    Case c{"no-weather", {}};
    snprintf(c.m.clock, sizeof c.m.clock, "%s", "06:05");
    snprintf(c.m.date, sizeof c.m.date, "%s", "Mon 1 Jan");
    snprintf(c.m.temp, sizeof c.m.temp, "%s", "--");
    c.m.haveTemp = false;
    cases.push_back(c);
  }
  {   // stale weather -> dashed rule
    Case c{"stale", {}};
    snprintf(c.m.clock, sizeof c.m.clock, "%s", "09:20");
    snprintf(c.m.date, sizeof c.m.date, "%s", "Sat 12 Jul");
    snprintf(c.m.cond, sizeof c.m.cond, "%s", "Overcast");
    snprintf(c.m.temp, sizeof c.m.temp, "%s", "8");
    c.m.haveTemp = true;
    c.m.stale = true;
    cases.push_back(c);
  }
  {   // provisioning banner
    Case c{"banner", {}};
    c.m.banner = "einkclock";
    c.m.banner2 = "Run ESP-Touch v2 to set up Wi-Fi";
    cases.push_back(c);
  }

  printf("\nrendered cases\n");
  for (const auto& c : cases) {
    MemCanvas cv;
    renderScreen(cv, c.m);

    char ppm[128], pbm[128];
    snprintf(ppm, sizeof ppm, "out-%s.ppm", c.name);
    snprintf(pbm, sizeof pbm, "out-%s.pbm", c.name);
    cv.writePpm(ppm);
    cv.writePbm(pbm);

    // Collision check: the left column (date/cond) must not reach into the
    // temperature block. Measured, not eyeballed.
    int lo, hi;
    cv.inkBounds(79, 121, &lo, &hi);
    printf("  %-12s -> %-20s lower-band ink x=[%d..%d]%s\n", c.name, ppm, lo, hi,
           hi > 249 ? "  *** CLIPPED ***" : "");
  }

  printf("\nopen them with:  open tools/preview/out-*.ppm\n");
  return 0;
}
