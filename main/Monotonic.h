#pragma once
#include <cstdint>

#include "esp_timer.h"

// Monotonic seconds since boot: the time base for every AGE in this firmware.
//
// Deliberately NOT time(nullptr). Every question these stamps answer is a DURATION —
// "how old is this reading", "how long since the trend anchor", "has this source gone
// quiet", "when was the forecast fetched" — and a duration must not be measured
// against a clock that can step. This one steps hard: MQTT is started as soon as
// there is an IP, before SNTP has synced, so readings used to be stamped near the
// epoch, and the instant SNTP jumped to the present they appeared to be decades old.
//
// Two observed consequences, both on the real device:
//   - Reading::fresh() blanked seconds-old readings to "--" until they republished,
//     which for the outside sensor is up to ~12 minutes.
//   - Reading::updateTrend() hit its `age > 3 * win` dead-publisher guard, discarded
//     the anchor and restarted the window, so no rise/fall mark for another
//     trend_win_min on top of the one already owed.
//
// It was a race, not a certainty, which is what made it easy to miss: the retained
// MQTT messages land at T+3 s and SNTP lands within a second or two of them. One
// instrumented boot showed valid stamps at T+3 s; an earlier boot showed the outside
// anchor set at T+649 s while still reporting Unknown, which is only reachable
// through that guard.
//
// esp_timer cannot step, is unaffected by SNTP, and survives light sleep. It resets
// on reboot, which is correct — a fresh boot genuinely has no history to compare
// against, and everSeen/haveRef already model "nothing yet".
//
// Wall-clock time is still used, but only where an ABSOLUTE instant is genuinely
// meant: the date line, /healthz's local_time, and gating the first weather fetch on
// a plausible clock so mbedTLS can validate the certificate's validity window.
inline int64_t nowMonoS() { return esp_timer_get_time() / 1000000; }
