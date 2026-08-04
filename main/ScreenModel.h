#pragma once

#include "Canvas.h"

// The POD boundary between "what to show" (decided in main.cpp, needs MQTT and
// the weather fetch) and "how to draw it" (Layout.cpp, pure pixels).
//
// This split is what makes the layout testable on the host: Layout.cpp includes
// only Canvas.h / Gfx.h / fonts.h and this header — no esp_* anywhere — so
// tools/preview compiles it with clang and renders to a PBM in 0.2 s instead of
// ~25 s on glass.

struct ScreenModel {
  // The two big readings, pre-formatted (e.g. "21.4", "-3.0", or "--" when the
  // MQTT topic has never been seen or has gone stale).
  //
  // Labels are SHORT by default ("IN" / "OUT"). They share the row with the big
  // digits, so a long label directly steals width from the number: measured at
  // Font_Label, "OUTSIDE" is 107px, which left only 108px for the temperature and
  // made "-12.4" (118px) and "100.0" (130px) overflow. Short labels give the
  // number a 158px budget, which fits every plausible reading.
  char insideLabel[12] = {};   // "IN" / "LOUNGE" if you prefer, and accept clipping
  char insideTemp[10] = {};
  bool insideValid = false;    // false => draw no degree ring

  char outsideLabel[12] = {};  // "OUT"
  char outsideTemp[10] = {};
  bool outsideValid = false;

  // ONE forecast line, small type under the rule. Not two: the band between the
  // rule and the bottom edge is 25px, and two lines of Font_Cond need ~36px.
  char forecast[48] = {};  // "Clear  6/17  rain 10%"

  // Non-null => draw the boot / provisioning screen instead of the readings.
  const char* banner = nullptr;
  const char* banner2 = nullptr;
};

void renderScreen(epd::Canvas& c, const ScreenModel& m);
