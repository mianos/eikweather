#pragma once
#include <ctime>
#include <string>

#include "MqttClient.h"
#include "Settings.h"
#include "Trend.h"

// Indoor / outdoor temperatures taken from local MQTT (published by Node-RED).
//
// Topics and field names are runtime SETTINGS, not compile-time constants, so the
// display can be pointed at different sensors with a POST /config and no reflash.
//
// Payloads are assumed to be flat JSON objects with a named numeric field, which
// is the convention across this broker — e.g. tele/ldr/lux publishes
// {"lux":71.2682,...}. mianesp's MqttClient hands handlers a parsed JsonWrapper,
// so that convention is also the only one it can express.
struct Reading {
  float value = 0.0f;
  time_t at = 0;      // our own time() when the message arrived
  bool everSeen = false;

  // A reading is only shown if it arrived recently. A sensor that has died should
  // blank to "--" rather than leave a plausible but hours-old number on a display
  // that only repaints every few minutes.
  bool fresh(int staleMin) const {
    if (!everSeen || staleMin <= 0) return everSeen;
    return (time(nullptr) - at) <= static_cast<time_t>(staleMin) * 60;
  }

  // --- rise / fall indicator ---------------------------------------------
  // A single anchor, re-set once per window, rather than a rolling history.
  //
  // The trend is evaluated only when the anchor is a full window old, and the
  // anchor then moves to NOW, so consecutive windows never overlap and the
  // arrow can change at most once per window. That bound is the point, not a
  // simplification: this panel has no partial refresh, so every arrow change
  // costs ~20 s of full-screen flashing. A rolling comparison would flap
  // between Steady and Rising around the threshold and burn a refresh each time.
  //
  // Costs 8 bytes per reading and needs no history buffer.
  float refValue = 0.0f;
  time_t refAt = 0;
  Trend trend = Trend::Unknown;

  // Call AFTER value/at have been updated from an arriving sample.
  void updateTrend(int winMin, int deltaTenths) {
    const time_t win = static_cast<time_t>(winMin < 1 ? 1 : winMin) * 60;
    if (refAt == 0) {  // first sample: nothing to compare against yet
      refValue = value;
      refAt = at;
      return;
    }
    const time_t age = at - refAt;
    if (age < win) return;

    if (age > 3 * win) {
      // The sensor was away far longer than the window (a dead publisher, or
      // SNTP stepping the clock forward at boot). The difference across that
      // gap is not a trend — say so instead of inventing one.
      trend = Trend::Unknown;
    } else {
      const float thr = (deltaTenths < 1 ? 1 : deltaTenths) / 10.0f;
      const float d = value - refValue;
      trend = d >= thr    ? Trend::Rising
              : d <= -thr ? Trend::Falling
                          : Trend::Steady;
    }
    refValue = value;
    refAt = at;
  }
};

class Sensors {
 public:
  Sensors(const Settings& settings, TaskHandle_t notify)
      : settings_(settings), notify_(notify) {}

  // The display task is created after Sensors, so its handle arrives late.
  void setNotify(TaskHandle_t t) { notify_ = t; }

  // Subscribes and registers handlers for whichever of the two topics is
  // configured (an empty topic simply means "not used").
  void attach(MqttClient& mqtt);

  const Reading& inside() const { return inside_; }
  const Reading& outside() const { return outside_; }
  uint32_t messages() const { return messages_; }

 private:
  // Shared by both topics; `which` selects the destination.
  static esp_err_t onMessage(MqttClient* client, const std::string& topic,
                            const JsonWrapper& json, void* context);

  struct Binding {
    Sensors* self;
    Reading* dest;
    const std::string* field;
    const char* name;
  };

  const Settings& settings_;
  TaskHandle_t notify_;
  Reading inside_;
  Reading outside_;
  uint32_t messages_ = 0;

  Binding insideBinding_{};
  Binding outsideBinding_{};
};
