#pragma once
#include <string>

#include "SettingsBase.h"

// einkclock settings schema. The persistence / reset / log / onChange machinery
// lives in mianesp's settingsbase (everything is stored as ONE JSON blob under a
// single NVS key). Member initialisers are the compiled-in defaults, which
// resetToDefaults() restores.
//
// SettingsBase supports ONLY std::string& and int&. No float, no bool, no
// unsigned. Booleans are modelled as 0/1 ints.
struct Settings : SettingsBase {
  // --- identity / time --------------------------------------------------
  std::string sensorName = "einkclock";
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

  // --- refresh cadence --------------------------------------------------
  // Must divide 60 so local minute-of-hour alignment is well defined; clamped at
  // use. The tri-colour panel has NO partial refresh and takes ~27-30 s for a full
  // one, and Good Display advise >=180 s between refreshes — so anything under 5
  // is for bench testing only.
  int refreshMin = 5;
  // Start the fetch+render this many seconds BEFORE the boundary so the burn
  // COMPLETES at :00/:05/... rather than starting there.
  //
  // MEASURED on this panel, and it is NOT a constant: 18.7 s at room temperature,
  // 24.6 s at 7.8 degC. E-paper waveform duration rises as the panel gets colder
  // (the controller picks a waveform per temperature range from OTP), so budget
  // for the cold end, not the warm one.
  // Budget: ~2 s typical fetch + up to ~25 s render = ~27 s.
  // 30 biases EARLY at both ends (finishing ~3 s before the boundary when cold,
  // ~8 s early when warm), which is the better error: showing the upcoming minute
  // a moment early beats still showing the previous one after the boundary passed.
  int renderLeadS = 30;
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
    field("refresh_min", refreshMin);
    field("render_lead_s", renderLeadS);
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

  // Clamp refreshMin to a divisor of 60 so the wall-clock alignment in
  // main.cpp is always well defined.
  int refreshPeriodMin() const {
    static const int kAllowed[] = {1, 2, 3, 4, 5, 6, 10, 12, 15, 20, 30, 60};
    for (int a : kAllowed)
      if (refreshMin == a) return a;
    return 5;
  }
};
