#pragma once
#include <string>

#include "SettingsBase.h"

// einkweather settings schema. The persistence / reset / log / onChange machinery
// lives in mianesp's settingsbase (everything is stored as ONE JSON blob under a
// single NVS key). Member initialisers are the compiled-in defaults, which
// resetToDefaults() restores.
//
// SettingsBase supports ONLY std::string& and int&. No float, no bool, no
// unsigned. Booleans are modelled as 0/1 ints.
struct Settings : SettingsBase {
  // --- identity / time --------------------------------------------------
  std::string sensorName = "einkweather";
  std::string tz = "AEST-10AEDT,M10.1.0,M4.1.0/3";  // POSIX TZ, Sydney
  std::string ntpServer = "pool.ntp.org";

  // --- location ---------------------------------------------------------
  // Stored as TEXT, not scaled ints. Their one and only consumer is the
  // Open-Meteo query string, so text goes straight in with no conversion: no
  // scale factor to get wrong, no precision lost on an int round trip, and
  // "-33.8688" is what a human pastes from a map. A malformed value just makes
  // Open-Meteo return 400, which the fetch-failure path already handles.
  // Sanity-checked with strtod at startup (warn only).
  std::string latitude = "-33.8688";   // Sydney
  std::string longitude = "151.2093";
  std::string tempUnit = "celsius";    // "celsius" | "fahrenheit", passed through

  // --- MQTT (local readings) --------------------------------------------
  std::string mqttServer = "mqtt2.mianos.com";
  int mqttPort = 1883;

  // Topics and field names, so the display can be re-pointed with POST /config
  // and no reflash. An empty topic means "not used" and shows "--".
  // Payloads are flat JSON objects with a named numeric field, which is the
  // convention on this broker (tele/ldr/lux publishes {"lux":71.2682,...}).
  std::string insideTopic = "";
  std::string insideField = "temperature";
  std::string insideLabel = "IN";
  std::string outsideTopic = "";
  std::string outsideField = "temperature";
  std::string outsideLabel = "OUT";

  // A reading older than this shows "--" rather than leaving a plausible but
  // hours-old number on a display that repaints only every few minutes.
  // 0 disables the staleness check.
  int sensorStaleMin = 30;

  // --- rise / fall arrow ------------------------------------------------
  // The arrow compares the current reading against the one from trendWinMin ago,
  // and only claims a direction once the move exceeds trendTenths tenths of a
  // degree. Both are deliberately coarse: this is a "is it warming up or cooling
  // down" glance, and every change of the arrow costs a full ~20 s repaint, so a
  // twitchy indicator would be worse than none.
  //
  // Consequence of the 30 min window: no arrow for the first half hour after a
  // reboot. That is intentional — see Trend::Unknown.
  int trendWinMin = 30;
  int trendTenths = 3;  // 0.3 degC

  // --- refresh cadence --------------------------------------------------
  // This is a WEATHER display, not a clock: it repaints only when the drawn
  // content actually CHANGES, rather than on a timer. The comparison is done on
  // the formatted strings, so a 0.01 degC wobble that does not alter a displayed
  // digit costs nothing — which is the whole point, since a full refresh is
  // 19-25 s of flashing on this panel and there is no partial refresh.
  //
  // minIntervalMin rate-limits repaints. Good Display advise >=180 s between
  // tri-colour refreshes, so this is clamped to >=3 at use (see minIntervalSec).
  int minIntervalMin = 10;

  // How often to re-fetch the Open-Meteo forecast. A fetch only causes a repaint
  // if the condition text or the hi/lo actually changed.
  int weatherPollMin = 15;

  int bootScreen = 1;  // 0 => skip the boot/status paint

  // --- panel geometry & init tunables -----------------------------------
  // Runtime rather than compile-time ON PURPOSE: every one of these is something
  // that could be wrong on first flash, and each is a POST /config + POST /refresh
  // away from being tested instead of a build-flash cycle. The framebuffer is
  // statically sized for the 128x296 worst case, so panelW/panelH are free to move.
  //
  // border/srcMode/updateMode come from GxEPD2's SSD1680 sequences, NOT from a
  // datasheet — they are the likeliest values to be wrong. See README bring-up.
  int panelW = 122;   // 122 (GDEM0213C90) or 128 (vendor's GxEPD2_290_C90c)
  int panelH = 250;   // 250 or 296
  int rotation = 3;   // epd::Rotation; 1 or 3 are the landscape options
  int invertRed = 0;  // 1 => XOR the 0x26 plane on the way out
  int border = 0x05;      // 0x3C border waveform
  int srcMode = 0x80;     // 0x21 byte B (source output range)
  int updateMode = 0xF7;  // 0x22 update sequence

  // --- services ---------------------------------------------------------
  // The mianesp WebServer CONSTRUCTOR unconditionally spawns 5 async worker tasks
  // x 4096 B (~21 KB heap) that nothing in this app uses. On a 320 KB WROOM that
  // deserves an escape hatch: 0 skips constructing it entirely, reclaiming ~28 KB,
  // at the cost of losing the only way to change lat/lon without a reflash.
  int enableWeb = 1;

  explicit Settings(NvsStorageManager& nvs) : SettingsBase(nvs) {
    field("sensor_name", sensorName);
    field("tz", tz);
    field("ntp_server", ntpServer);
    field("latitude", latitude);
    field("longitude", longitude);
    field("temp_unit", tempUnit);
    field("mqtt_server", mqttServer);
    field("mqtt_port", mqttPort);
    field("inside_topic", insideTopic);
    field("inside_field", insideField);
    field("inside_label", insideLabel);
    field("outside_topic", outsideTopic);
    field("outside_field", outsideField);
    field("outside_label", outsideLabel);
    field("sensor_stale_min", sensorStaleMin);
    field("trend_win_min", trendWinMin);
    field("trend_tenths", trendTenths);
    field("min_interval_min", minIntervalMin);
    field("weather_poll_min", weatherPollMin);
    field("boot_screen", bootScreen);
    field("panel_w", panelW);
    field("panel_h", panelH);
    field("rotation", rotation);
    field("invert_red", invertRed);
    field("border", border);
    field("src_mode", srcMode);
    field("update_mode", updateMode);
    field("enable_web", enableWeb);
    load();  // MUST be last, after every field() registration
  }

  // Never let a bad setting drive the panel harder than the vendor allows:
  // Good Display advise >=180 s between tri-colour refreshes.
  int minIntervalSec() const {
    const int m = minIntervalMin < 3 ? 3 : minIntervalMin;
    return m * 60;
  }
};
