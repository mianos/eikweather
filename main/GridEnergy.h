#pragma once
#include <cmath>
#include <cstdint>

// Net grid energy from the meter's two CUMULATIVE counters (import and export,
// kWh), turned into the two numbers on the screen:
//
//   - average kW over the last window (min_interval_min), and
//   - net kWh since local midnight.
//
// Counters rather than the instantaneous power reading because the screen only
// repaints every ~15 minutes: one power sample from up to 15 minutes ago is close
// to noise on a cloudy day, whereas a counter difference is the EXACT energy over
// the interval, with no sampling error at all.
//
// "Net" is export minus import throughout, so positive means net exporting.
//
// Pure: no esp_* and no clock reads — every timestamp and the day key are passed
// in — so tools/preview compiles it and asserts the arithmetic on the host.
// Persisting the midnight baseline (which must survive a reboot) is the caller's
// job; add() says when it needs saving.
struct GridEnergy {
  struct Sample {
    int64_t at;     // monotonic seconds
    int64_t netWh;  // export - import, cumulative
  };
  // One sample a minute arrives, so 32 covers a window of up to ~30 minutes.
  static constexpr int kRing = 32;
  // Shortest history the average is shown over. Counters step in 0.01 kWh, so
  // across two adjacent one-minute samples one step is 0.6 kW of pure noise; at
  // five minutes it is 0.12 kW. Only matters for the first minutes after a boot.
  static constexpr int kMinAvgS = 5 * 60;

  Sample ring[kRing] = {};
  int count = 0;
  int head = 0;  // index of the NEXT write

  int64_t importWh = 0;
  int64_t exportWh = 0;
  int64_t at = 0;
  bool everSeen = false;

  // Net counter value at the start of the local day `dayKey` (YYYYMMDD).
  int32_t dayKey = 0;
  int64_t dayBaseNetWh = 0;

  static int64_t whFromKwh(double kwh) { return std::llround(kwh * 1000.0); }

  // Returns true when the midnight baseline changed and should be persisted.
  // today == 0 means the wall clock is not trustworthy yet: the sample still feeds
  // the average, but the day baseline is left alone rather than keyed to 1970.
  bool add(int64_t now, int64_t impWh, int64_t expWh, int32_t today) {
    // A counter that goes BACKWARDS is a meter reset or replacement. History across
    // it is meaningless, so drop the ring and re-base the day on the new counters.
    const bool reset = everSeen && (impWh < importWh || expWh < exportWh);
    if (reset) count = 0;

    importWh = impWh;
    exportWh = expWh;
    at = now;
    everSeen = true;

    const int64_t net = expWh - impWh;
    ring[head] = {now, net};
    head = (head + 1) % kRing;
    if (count < kRing) ++count;

    if (today != 0 && (today != dayKey || reset)) {
      dayKey = today;
      dayBaseNetWh = net;
      return true;
    }
    return false;
  }

  void restoreDay(int32_t key, int64_t baseNetWh) {
    dayKey = key;
    dayBaseNetWh = baseNetWh;
  }

  // Average net power in TENTHS of a kW over the newest windowS seconds of samples.
  // Uses less history if that is all there is, but never less than minS: an average
  // over two adjacent 0.01 kWh-resolution samples would be quantisation noise.
  bool avgTenthsKw(int64_t now, int windowS, int minS, int staleS,
                   int* out) const {
    if (count < 2 || (staleS > 0 && now - at > staleS)) return false;
    const Sample& newest = ring[(head - 1 + kRing) % kRing];
    // Walk back from the newest; keep the newest sample that is at least windowS
    // old, or failing that the oldest one held.
    const Sample* base = nullptr;
    for (int i = 1; i < count; ++i) {
      const Sample& s = ring[(head - 1 - i + 2 * kRing) % kRing];
      base = &s;
      if (newest.at - s.at >= windowS) break;
    }
    const int64_t dt = newest.at - base->at;
    if (dt < minS || dt <= 0) return false;
    // Wh over dt seconds -> W is Wh * 3600 / dt; tenths of a kW is W / 100.
    const double tenths = static_cast<double>(newest.netWh - base->netWh) * 36.0 /
                          static_cast<double>(dt);
    *out = clampTenths(std::lround(tenths));
    return true;
  }

  // Net energy since local midnight in TENTHS of a kWh. False until a baseline for
  // `today` exists, so a value is never shown against yesterday's baseline.
  bool todayTenthsKwh(int32_t today, int64_t now, int staleS, int* out) const {
    if (!everSeen || today == 0 || dayKey != today) return false;
    if (staleS > 0 && now - at > staleS) return false;
    const int64_t wh = (exportWh - importWh) - dayBaseNetWh;
    // Round half away from zero, in integers.
    const int64_t t = wh >= 0 ? (wh + 50) / 100 : -((-wh + 50) / 100);
    *out = clampTenths(t);
    return true;
  }

 private:
  static int clampTenths(int64_t t) {
    return static_cast<int>(t < -9999 ? -9999 : t > 9999 ? 9999 : t);
  }
};
