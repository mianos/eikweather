#pragma once

#include "Canvas.h"
#include "Trend.h"

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
  //
  // The trend arrow is drawn in the gap the short labels leave between the label
  // and the right-aligned digits — space that was previously dead. Only Rising
  // and Falling mark the screen; Steady and Unknown leave it blank.
  char insideLabel[12] = {};   // "IN" / "LOUNGE" if you prefer, and accept clipping
  char insideTemp[10] = {};
  bool insideValid = false;    // false => draw no degree ring
  Trend insideTrend = Trend::Unknown;

  char outsideLabel[12] = {};  // "OUT"
  char outsideTemp[10] = {};
  bool outsideValid = false;
  Trend outsideTrend = Trend::Unknown;

  // ONE line under the rule, shared: forecast on the left, date on the right.
  // Not two lines — the band between the rule and the bottom edge is 25px, and two
  // lines of Font_Cond need ~36px.
  //
  // The date earns its place on a display that refuses to be a clock: it changes
  // once a day, so it costs one refresh per day. A time would cost one per minute.
  char forecast[48] = {};      // "Clear 6/16" — condition and today's low/high
  bool forecastValid = false;  // false => the fetch has never succeeded
  // Third reading (heat pump hot water tank), right-aligned on the FORECAST line
  // with a small degree ring, directly above the rain chance.
  //
  // UNLABELLED by default — just "48" plus the ring. It shares the tightest line
  // on the screen: measured, the condition plus today's lo/hi is up to 188px of a
  // 246px line, so the bare number's 39px worst case is all that fits. A label is
  // available via water_label for anyone who wants one, at the cost of the
  // forecast text truncating.
  //
  // Empty => drawn as nothing and the forecast keeps the full width, which covers
  // both "not configured" and "gone stale".
  char water[16] = {};
  // 24, not 16: the compiler bounds %a and %b at 7 bytes each, so a 16-byte buffer
  // trips -Werror=format-truncation even though real output is ~10 chars.
  char date[24] = {};      // "Wed 5 Aug", empty until SNTP has synced
  // Drawn right-aligned on the date line and LABELLED. A bare "0%" tacked onto the
  // forecast was unreadable — nothing said what it was a percentage of.
  char rain[16] = {};      // "rain 0%", empty when the API returns no probability

  // Some source that WAS working has gone quiet for alert_age_min — draw a red "!"
  // in the top-right corner. See the long note in Settings.h for exactly what
  // counts, and what this deliberately cannot detect.
  //
  // One bool rather than which-source-failed: the corner is 7px wide, which is
  // enough to say "look at /healthz" and nothing more. /healthz carries every
  // individual age, so the mark's job is only to make you go and look.
  bool alert = false;

  // Non-null => draw the boot / provisioning screen instead of the readings.
  const char* banner = nullptr;
  const char* banner2 = nullptr;

  // Compared to decide whether a repaint is worth ~25 s of flashing. DEFAULTED
  // rather than hand-written, and that is the whole point: the previous version was
  // a 12-field strcmp chain, so adding a field to this struct silently left it out
  // of the comparison. No compile error, no warning — just a field that never
  // updates on the glass. Adding `alert` nearly shipped exactly that bug, and the
  // symptom would have been "the alert feature doesn't work" with nothing pointing
  // at the comparison. Now a new field joins the comparison automatically.
  //
  // Two things make this safe, both of which a naive defaulted == would get wrong:
  //
  // 1. Defaulted == compares char arrays ELEMENT-WISE, including bytes past the
  //    NUL, and snprintf does not zero the tail. That would be a bug — "Overcast"
  //    overwritten by "Clear" leaves stale bytes — except that every member here has
  //    a `= {}` initialiser and buildModel() always fills a FRESH `ScreenModel`
  //    declared inside the display loop. So each pass starts fully zeroed, fields
  //    that go unwritten stay zeroed, and identical content is identical bytes.
  //    Keep it that way: reusing one instance across passes would break this.
  //
  // 2. banner/banner2 are pointers, so they compare by ADDRESS, not by string. That
  //    is a widening — the old comparison ignored them entirely — and it is harmless
  //    because the banner path is the boot/provisioning screen, which never competes
  //    with the readings: in the normal loop both are nullptr in both operands.
  bool operator==(const ScreenModel&) const = default;
};

void renderScreen(epd::Canvas& c, const ScreenModel& m);
