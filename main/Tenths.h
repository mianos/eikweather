#pragma once
#include <cmath>
#include <cstdio>

// Fixed-point temperatures: integer TENTHS of a degree, so 17.3 degC is 173.
//
// Tenths because that is exactly the resolution the screen displays, so every
// question downstream — "did the number change?", "which way did it move?", "what
// do I print?" — is answered with exact integer arithmetic. The float version had
// a real bug in it: 17.3f - 17.2f is 0.100000381f, so a `>= 0.1f` trend test
// missed genuine one-digit changes depending on which values it landed on.
//
// This does NOT remove floating point from the firmware and is not a size or speed
// win — measured, the binary grew ~500 bytes. cJSON parses every JSON number into
// a double and prints them back with sprintf("%1.15g"), so double support is linked
// regardless, and the ESP32 has hardware single-precision FP anyway. What it buys
// is that the conversion happens in exactly ONE place per input instead of being
// re-derived with lroundf at each use, and that no comparison needs an epsilon.
//
// Its own header, free of esp_* and of MqttClient, so tools/preview can compile it
// and assert the edge cases on the host — which matters most for the sign trap in
// formatTenths, whose failure case only appears below freezing.

// The one float -> fixed-point conversion. Clamped so the arithmetic stays far
// inside int and a garbage payload cannot render as nonsense: -99.9 to 999.9 degC
// covers every real sensor.
inline int tenthsFromDegrees(double deg) {
  double t = deg * 10.0;
  if (t < -999.0) t = -999.0;
  if (t > 9999.0) t = 9999.0;
  return static_cast<int>(std::lround(t));
}

// Tenths -> "17.3" / "-3.7" / "-0.5".
//
// The sign MUST be built separately. For tenths = -5, tenths / 10 is 0 and
// tenths % 10 is -5, so a naive "%d.%d" prints "0.5" and silently drops the minus
// sign for every value between -0.9 and -0.1.
//
// The re-clamp is not redundant with tenthsFromDegrees: it is what lets the
// compiler bound the conversion to 3 digits so -Wformat-truncation can prove the
// result fits the caller's buffer.
inline void formatTenths(char* out, size_t n, int tenths) {
  int a = tenths < 0 ? -tenths : tenths;
  if (a > 9999) a = 9999;
  snprintf(out, n, "%s%d.%d", tenths < 0 ? "-" : "", a / 10, a % 10);
}

// Tenths -> whole degrees, rounded half away from zero, no float involved.
inline int wholeDegrees(int tenths) {
  return tenths >= 0 ? (tenths + 5) / 10 : -((-tenths + 5) / 10);
}
