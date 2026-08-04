#pragma once

#include "Canvas.h"

// The POD boundary between "what to show" (decided in main.cpp, needs the clock,
// Wi-Fi and weather) and "how to draw it" (Layout.cpp, pure pixels).
//
// This split is what makes the layout testable on the host: Layout.cpp includes
// only Canvas.h / Gfx.h / fonts.h and this header — no esp_* anywhere — so
// tools/preview compiles it with clang and renders to a PBM in 0.2 s instead of
// 30 s on glass.

struct ScreenModel {
  // "07:35". Empty => leave the clock area blank. NEVER put a placeholder time
  // here: a wrong time burned into e-paper for 5 minutes is worse than nothing.
  char clock[8] = {};

  // "Wed 24 Sep". The weekday is ALWAYS abbreviated: measured at Font_Date,
  // "Wednesday 24 Sep" is 217px against a 164px column, and only "Friday" and
  // "Sunday" fit unabbreviated. A date format that silently changes shape
  // depending on the day of the week reads as a bug, so it is abbreviated
  // consistently rather than opportunistically.
  char date[24] = {};

  char cond[24] = {};  // "Partly cloudy", may be empty
  char temp[8] = {};   // "21" | "-5" | "--"

  bool haveTemp = false;  // false => draw no temperature and no degree ring
  bool stale = false;     // => dashed separator instead of solid

  // Non-null => draw the boot / provisioning screen instead of the clock.
  const char* banner = nullptr;
  const char* banner2 = nullptr;
};

void renderScreen(epd::Canvas& c, const ScreenModel& m);
