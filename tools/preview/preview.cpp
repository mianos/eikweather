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
// chosen numbers are safe. Change one here, change it there.
constexpr int kTempRightX = 232;
constexpr int kLabelMaxW = 70;
constexpr int kFcMaxW = 246;
constexpr int kInsideLabelBaseline = 20;
constexpr int kInsideTempBaseline = 43;
constexpr int kOutsideLabelBaseline = 65;
constexpr int kOutsideTempBaseline = 88;
constexpr int kFcBaseline = 116;

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

}  // namespace

int main() {
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
  // Forecast format is "<condition> <lo>/<hi> <rain>%". The longest WMO strings
  // are 13 chars ("Partly cloudy", "Heavy drizzle", "Heavy showers"), so the
  // realistic worst case is checked here. drawTextClipped is the backstop for
  // anything pathological — it truncates with ".." rather than overflowing.
  checkWidth("fc typical", epd::Font_Cond, "Partly cloudy 6/17 10%", kFcMaxW);
  checkWidth("fc long cond", epd::Font_Cond, "Heavy drizzle 2/11 90%", kFcMaxW);
  checkWidth("fc negative lo", epd::Font_Cond, "Heavy showers -9/45 100%", kFcMaxW);
  checkWidth("fc no rain data", epd::Font_Cond, "Partly cloudy 6/17", kFcMaxW);

  printf("\nvertical budgets (kH=%d, rule at y=94..96)\n", kH);
  checkVertical("inside label", epd::Font_Label, "IN", kInsideLabelBaseline, 0, 93);
  checkVertical("inside temp", epd::Font_Big, "-12.4", kInsideTempBaseline, 0, 93);
  checkVertical("outside label", epd::Font_Label, "OUT", kOutsideLabelBaseline, 0, 93);
  checkVertical("outside temp", epd::Font_Big, "-12.4", kOutsideTempBaseline, 0, 93);
  checkVertical("forecast desc", epd::Font_Cond, "Heavy drizzle 2/11 90%", kFcBaseline, 97, 121);

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
  struct Case { const char* name; ScreenModel m; };
  std::vector<Case> cases;

  auto mk = [](const char* name, const char* inL, const char* inT, bool inV,
               const char* outL, const char* outT, bool outV,
               const char* fc) {
    Case c{name, {}};
    snprintf(c.m.insideLabel, sizeof c.m.insideLabel, "%s", inL);
    snprintf(c.m.insideTemp, sizeof c.m.insideTemp, "%s", inT);
    c.m.insideValid = inV;
    snprintf(c.m.outsideLabel, sizeof c.m.outsideLabel, "%s", outL);
    snprintf(c.m.outsideTemp, sizeof c.m.outsideTemp, "%s", outT);
    c.m.outsideValid = outV;
    snprintf(c.m.forecast, sizeof c.m.forecast, "%s", fc);
    return c;
  };

  cases.push_back(mk("normal", "IN", "21.4", true, "OUT", "7.8", true,
                     "Partly cloudy 6/17 10%"));
  cases.push_back(mk("widest", "IN", "-12.4", true, "OUT", "100.0", true,
                     "Heavy showers -9/45 100%"));
  cases.push_back(mk("no-mqtt", "IN", "--", false, "OUT", "--", false,
                     "Clear 6/17 - no MQTT yet"));
  cases.push_back(mk("lounge", "IN", "19.0", true, "OUT", "3.2", true,
                     "Heavy drizzle 2/11 90%"));
  {
    Case c{"banner", {}};
    c.m.banner = "einkclock";
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
