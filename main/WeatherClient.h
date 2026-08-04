#pragma once
#include <ctime>
#include <string>

struct Settings;

// Current conditions from Open-Meteo (no API key required).
struct Weather {
  bool valid = false;    // has a successful fetch ever happened?
  float temp = 0.0f;
  int code = -1;         // WMO weather code
  time_t fetchedAt = 0;  // our own time() at the last SUCCESS, not the server's
};

// Fetches https://api.open-meteo.com/v1/forecast over TLS using the IDF root
// certificate bundle. Calls esp_http_client directly rather than going through
// mianesp's HttpClient, which has no crt_bundle_attach and so physically cannot
// do HTTPS.
class WeatherClient {
 public:
  explicit WeatherClient(const Settings& settings) : settings_(settings) {}

  // Blocking, ~1-3 s, hard 8 s timeout. One attempt, no internal retry: the next
  // cycle is only a few minutes away and we have last-known-good data.
  //
  // On failure `out` is left COMPLETELY UNTOUCHED so the caller keeps showing the
  // previous reading, and the render proceeds regardless — a failed weather fetch
  // must never cost the user a correct clock.
  bool fetch(Weather& out);

  uint32_t consecutiveFailures() const { return failures_; }
  int lastStatus() const { return lastStatus_; }

 private:
  const Settings& settings_;
  uint32_t failures_ = 0;
  int lastStatus_ = 0;
};

// WMO weather_code -> a short display string. Empty for unknown codes.
// Widths are checked against the 164px column by tools/preview.
const char* wmoText(int code);
