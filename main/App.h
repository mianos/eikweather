#pragma once
#include "Epaper.h"
#include "Settings.h"
#include "WeatherClient.h"

class WiFiManager;

// Everything the display task and the web handlers both need. Single-owner by
// design: the display task is the ONLY writer of `weather`, so there is no mutex
// and no possibility of a torn read. The web server only reads it.
struct App {
  Settings* settings;
  epd::Panel* panel;
  WeatherClient* weather;
  WiFiManager* wifi;

  Weather current{};

  // Diagnostics surfaced on GET /healthz.
  uint32_t renderCount = 0;
  int32_t lastStackHighWater = 0;
  char lastClock[8] = {};

  // Set by the web server to request a one-off test pattern on the next wake.
  // 0 = none, otherwise a TestPattern value.
  volatile int pendingTest = 0;
};

enum TestPattern {
  kTestNone = 0,
  kTestBlack,
  kTestWhite,
  kTestCalib,   // native-coordinate geometry pattern (bring-up stage 4)
  kTestColors,  // black vs red bars (bring-up stage 6)
  kTestRot,     // rotation probe (bring-up stage 5)
};
