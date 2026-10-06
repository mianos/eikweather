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
  // Placeholder on purpose — point it at your own broker with
  //   curl -X POST -d '{"mqtt_server":"broker.example.lan"}' http://<host>/config
  // followed by POST /reboot, since MQTT subscriptions are established once at
  // startup (see the onChange warning in main.cpp).
  //
  // Note this default is only ever consulted on a board that has never been
  // configured: SettingsBase persists the whole schema as one JSON blob in NVS, so
  // once a device has stored a broker, that stored value wins over any compiled
  // default forever. Changing this line does not move a running device.
  std::string mqttServer = "mqtt.local";
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

  // A third, OPTIONAL reading: net grid power, shown small and right-aligned on
  // the forecast line rather than as a third big row — there is no vertical room
  // for another row. One SIGNED number covers both directions, because at any
  // instant the meter is either importing or exporting, never both: "+3.4" is
  // exporting 3.4 kW (net positive), "-1.2" is importing 1.2 kW.
  //
  // Empty topic => drawn as nothing at all, and the forecast gets the full width
  // back. Unlike the two big readings it does NOT fall back to "--" when stale:
  // that row is dedicated, so a blank would look broken, whereas this shares the
  // most width-constrained line on the screen and the forecast can use the space.
  std::string gridTopic = "";
  std::string gridField = "power";
  // The payload value is divided by this to get kW: magnitude 1000 for a meter
  // that publishes watts, 1 for one that publishes kW. The SIGN sets the display
  // convention: negative for a meter that reports import as positive (the iMeter
  // does), positive for one that already reports export as positive. Needed, not
  // cosmetic: readings are stored as tenths clamped to -99.9..999.9, so raw watts
  // would pin at the clamp.
  int gridDiv = -1000;

  // A reading older than this shows "--" rather than leaving a plausible but
  // hours-old number on a display that repaints only every few minutes.
  // 0 disables the staleness check.
  int sensorStaleMin = 30;

  // --- rise / fall arrow ------------------------------------------------
  // The arrow compares the current reading against the one from trendWinMin ago,
  // and claims a direction once the move exceeds trendTenths tenths of a degree.
  //
  // trendTenths is ONE tenth: the smallest change the screen can show, since the
  // big readings are drawn at %.1f. So the rule is simply "if the number you can
  // see moved, the arrow says which way", and only a completely unchanged digit
  // reads as steady (which draws nothing).
  //
  // This started at 3 tenths over 30 min and that was wrong. Measured indoors,
  // 0.3 degC per 30 min is a rare event, so both rows sat blank all afternoon while
  // the visible digits changed — indistinguishable from the feature being broken.
  //
  // The reasoning that produced those numbers — that a twitchy arrow would cost
  // refreshes — was also wrong. The readings are drawn to a tenth, so "16.4" ->
  // "16.5" already forces a repaint on its own; the arrow rides along on repaints
  // that were happening anyway. Hence the window matching minIntervalMin: there is
  // no reason for the arrow to move slower than the screen does.
  //
  // Consequence of the window: no arrow for the first trendWinMin after a reboot
  // (see Trend::Unknown). Every OTA therefore blanks the arrows for 10 minutes —
  // do not read anything into a blank column right after a flash.
  int trendWinMin = 10;
  int trendTenths = 1;  // 0.1 degC — one displayed digit

  // --- stale-data alert -------------------------------------------------
  // A red "!" in the top-right corner once any source that WAS working has gone
  // quiet for this long. 0 disables it.
  //
  // An hour rather than sensor_stale_min's 30 because the two answer different
  // questions. sensor_stale_min blanks ONE reading to "--" the moment its own
  // number stops being trustworthy. This is the screen-level "nothing here is
  // current any more" mark — and it is the ONLY age indication for the forecast,
  // which otherwise renders a 12-hour-old condition identically to a fresh one
  // (app.current keeps last-known-good indefinitely, by design, so that a failed
  // fetch never costs you the rest of the screen).
  //
  // A source that is unconfigured, or has never been seen at all, does not count.
  // An empty topic is a choice, and a configured-but-silent one already shows "--"
  // or "forecast unavailable", which says more than a 4px mark in a corner can.
  // The mark therefore means specifically "this was working and has stopped".
  //
  // What it CANNOT tell you is that the board is dead: e-paper holds its last
  // image with no power at all, and a wedged device cannot draw a warning about
  // itself. This covers MQTT, Wi-Fi and the weather API failing underneath a
  // display task that is still running, which is the common case but is not a
  // heartbeat.
  int alertAgeMin = 60;

  // --- refresh cadence --------------------------------------------------
  // This is a WEATHER display, not a clock: it repaints only when the drawn
  // content actually CHANGES, rather than on a timer. The comparison is done on
  // the formatted strings, so a 0.01 degC wobble that does not alter a displayed
  // digit costs nothing — which is the whole point, since a full refresh is
  // 19-25 s of flashing on this panel and there is no partial refresh.
  //
  // minIntervalMin rate-limits repaints. Good Display advise >=180 s between
  // tri-colour refreshes, so this is clamped to >=3 at use (see minIntervalSec).
  int minIntervalMin = 15;

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

  // --- power management -------------------------------------------------
  // 1 => dynamic frequency scaling (160 MHz busy / 40 MHz idle) plus automatic
  // light sleep. 0 => no power management at all, pinned at 160 MHz.
  //
  // This device is mains powered, so the goal is LESS HEAT behind the panel, not
  // battery life. The panel refreshes for ~25 s once every ~10 min — a 3.9% duty
  // cycle — so almost all of the energy went into idling with the radio
  // associated. See sdkconfig.defaults and the README for the measurements.
  //
  // Runtime rather than compile-time because light sleep is the one change here
  // that can plausibly degrade the network: sleep timing comes from the internal
  // RC oscillator (no 32 k crystal on this board), so if MQTT latency or the web
  // API get unreliable, POST /config {"light_sleep":0} is the escape hatch and it
  // takes effect immediately — no reflash, and no reboot needed either.
  int lightSleep = 1;

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
    field("grid_topic", gridTopic);
    field("grid_field", gridField);
    field("grid_div", gridDiv);
    field("sensor_stale_min", sensorStaleMin);
    field("trend_win_min", trendWinMin);
    field("trend_tenths", trendTenths);
    field("alert_age_min", alertAgeMin);
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
    field("light_sleep", lightSleep);
    load();  // MUST be last, after every field() registration
  }

  // Never let a bad setting drive the panel harder than the vendor allows:
  // Good Display advise >=180 s between tri-colour refreshes.
  int minIntervalSec() const {
    const int m = minIntervalMin < 3 ? 3 : minIntervalMin;
    return m * 60;
  }
};
