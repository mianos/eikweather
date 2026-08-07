#pragma once
#include <cstdint>
#include <string>

#include "Monotonic.h"

struct Settings;

// Forecast from Open-Meteo (no API key required).
struct Weather {
  bool valid = false;    // has a successful fetch ever happened?
  int code = -1;         // WMO weather code for the current conditions
  // WHOLE degrees. The forecast line has room for "6/17" and nothing more, so a
  // tenth was never displayed — storing it as one only meant re-rounding at every
  // use. Rounded once, in the parser.
  int lo = 0;            // daily temperature_2m_min
  int hi = 0;            // daily temperature_2m_max
  int rainPct = -1;      // daily precipitation_probability_max, -1 if absent
  // MONOTONIC seconds since boot at the last SUCCESS (see Monotonic.h), not the
  // server's timestamp and not wall clock. This one was never actually broken by the
  // SNTP step — fetch() is already gated on a plausible clock, because mbedTLS cannot
  // validate the certificate without one — but it is an age like all the others, and
  // leaving it on a different time base to the readings made overdue() in main.cpp
  // compare two incompatible things.
  int64_t fetchedAt = 0;
};

// Fetches https://api.open-meteo.com/v1/forecast over TLS using the IDF root
// certificate bundle. Calls esp_http_client directly rather than going through
// mianesp's HttpClient, which has no crt_bundle_attach and so physically cannot
// do HTTPS.
class WeatherClient {
 public:
  explicit WeatherClient(const Settings& settings) : settings_(settings) {}

  // Blocking, ~1-3 s, hard 8 s timeout. One attempt, no internal retry.
  //
  // On failure `out` is left COMPLETELY UNTOUCHED so the caller keeps showing the
  // previous forecast. The indoor/outdoor readings come from MQTT and are
  // unaffected either way, so a weather outage never blanks the main display.
  bool fetch(Weather& out);

  uint32_t consecutiveFailures() const { return failures_; }
  int lastStatus() const { return lastStatus_; }
  // esp_err_to_name of the last perform(), or a parse-stage reason. A status of 0
  // with a non-empty error here means it never reached HTTP at all (DNS, TLS,
  // timeout), which is a completely different problem from a 4xx.
  const char* lastError() const { return lastError_; }

 private:
  const Settings& settings_;
  uint32_t failures_ = 0;
  int lastStatus_ = 0;
  const char* lastError_ = "";
};

// WMO weather_code -> a short display string. Empty for unknown codes.
// Widths are checked against the forecast line budget by tools/preview.
const char* wmoText(int code);
