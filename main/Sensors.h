#pragma once
#include <cstdint>
#include <mutex>
#include <string>

#include "GridEnergy.h"
#include "Monotonic.h"
#include "MqttClient.h"
#include "NvsStorageManager.h"
#include "Settings.h"
#include "Tenths.h"
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
  // Integer TENTHS of a degree — 17.3 degC is 173. See Tenths.h for why.
  int tenths = 0;
  // MONOTONIC seconds since boot, not time(nullptr) — see Monotonic.h. Using the
  // wall clock here meant a reading that arrived before SNTP synced looked decades
  // old the moment the clock stepped.
  int64_t at = 0;
  bool everSeen = false;

  // A reading is only shown if it arrived recently. A sensor that has died should
  // blank to "--" rather than leave a plausible but hours-old number on a display
  // that only repaints every few minutes.
  bool fresh(int staleMin) const {
    if (!everSeen || staleMin <= 0) return everSeen;
    return (nowMonoS() - at) <= static_cast<int64_t>(staleMin) * 60;
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
  int refTenths = 0;
  int64_t refAt = 0;
  // Explicit flag rather than treating refAt == 0 as "no anchor yet". On a monotonic
  // clock 0 is a legitimate stamp — the first second after boot — and the retained
  // MQTT messages arrive about three seconds in, close enough that relying on the
  // sentinel was asking for trouble.
  bool haveRef = false;
  Trend trend = Trend::Unknown;


  // Call AFTER value/at have been updated from an arriving sample.
  void updateTrend(int winMin, int deltaTenths) {
    const int64_t win = static_cast<int64_t>(winMin < 1 ? 1 : winMin) * 60;
    if (!haveRef) {  // first sample: nothing to compare against yet
      refTenths = tenths;
      refAt = at;
      haveRef = true;
      return;
    }
    const int64_t age = at - refAt;
    if (age < win) return;

    if (age > 3 * win) {
      // The sensor was away far longer than the window: a dead publisher that has
      // come back. The difference across that gap is not a trend — say so instead of
      // inventing one.
      //
      // This used to also fire from SNTP stepping the clock forward at boot, which
      // silently cost a whole extra trend window on any boot where a retained
      // message beat the time sync. Monotonic stamps make that impossible, so this
      // branch now means only what it says.
      trend = Trend::Unknown;
    } else {
      // Plain integer subtraction — the whole point of storing tenths. Asks
      // exactly the question the repaint logic asks: "did the number you can see
      // change, and which way?"
      const int d = tenths - refTenths;
      const int thr = deltaTenths < 1 ? 1 : deltaTenths;
      trend = d >= thr    ? Trend::Rising
              : d <= -thr ? Trend::Falling
                          : Trend::Steady;
    }
    refTenths = tenths;
    refAt = at;
  }
};

// Local date as YYYYMMDD, or 0 while the wall clock is still at its 1970 boot
// value. Keys the grid's "today" figure; shared so that the reading side and the
// display side can never disagree about what day it is.
int32_t localDayKey();

class Sensors {
 public:
  Sensors(const Settings& settings, NvsStorageManager& nvs, TaskHandle_t notify)
      : settings_(settings), nvs_(nvs), notify_(notify) {}

  // The display task is created after Sensors, so its handle arrives late.
  void setNotify(TaskHandle_t t) { notify_ = t; }

  // Subscribes and registers handlers for whichever of the two topics is
  // configured (an empty topic simply means "not used").
  void attach(MqttClient& mqtt);

  const Reading& inside() const { return inside_; }
  const Reading& outside() const { return outside_; }
  // A COPY, taken under the lock: the ring is several hundred bytes written by the
  // MQTT task, and reading it in place from the display task could tear.
  GridEnergy grid() const {
    std::lock_guard<std::mutex> lock(gridMutex_);
    return grid_;
  }
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

  static esp_err_t onGridMessage(MqttClient* client, const std::string& topic,
                                const JsonWrapper& json, void* context);
  void subscribeExact(MqttClient& mqtt, const std::string& topic,
                      HandlerFunc handler, void* context);

  const Settings& settings_;
  NvsStorageManager& nvs_;
  TaskHandle_t notify_;
  Reading inside_;
  Reading outside_;
  mutable std::mutex gridMutex_;
  GridEnergy grid_;
  uint32_t messages_ = 0;

  // One per topic, and they must OUTLIVE attach(): MqttClient keeps the void*
  // context pointer, so these cannot be locals.
  Binding insideBinding_{};
  Binding outsideBinding_{};
};
