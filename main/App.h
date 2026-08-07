#pragma once
#include "Epaper.h"
#include "Settings.h"
#include "Sensors.h"
#include "WeatherClient.h"

class WiFiManager;
class MqttClient;

// Everything the display task and the web handlers both need. Single-owner by
// design: the display task is the ONLY writer of `weather`, so there is no mutex
// and no possibility of a torn read. The web server only reads it.
struct App {
  Settings* settings;
  epd::Panel* panel;
  WeatherClient* weather;
  Sensors* sensors;
  WiFiManager* wifi;
  // Started by the display task once there is an IP, not by app_main — see the
  // comment at that call site.
  MqttClient* mqtt;

  Weather current{};

  // Diagnostics surfaced on GET /healthz.
  uint32_t renderCount = 0;
  int32_t lastStackHighWater = 0;
  // Last computed stale-data alert state. Written by the display task on every
  // pass (whether or not it repaints) and only read by the web server, so it keeps
  // App's single-writer contract. Surfaced so that "why is there a bang on my
  // screen" is answerable from /healthz, next to the per-source ages that caused it.
  bool alertActive = false;

  // Why this boot happened, and how many boots there have been. Both surfaced on
  // /healthz, and the pair is the point: esp_reset_reason() alone tells you nothing
  // if you were not watching at the time, while a counter you can poll makes a
  // restart you MISSED visible after the fact. Without these, a manual reset and a
  // panic looked identical from outside, and an hour was spent suspecting light sleep
  // on no evidence at all.
  //
  // bootCount is read-modify-written in NVS once per boot, so it survives power
  // cycles. See the note at the call site about the flash-wear arithmetic.
  const char* resetReason = "?";
  int bootCount = 0;

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
