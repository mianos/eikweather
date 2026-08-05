#pragma once
#include <cstdint>

// Which way a temperature has moved over the trend window.
//
// Its own header, deliberately tiny, because it sits on BOTH sides of the
// ScreenModel boundary: Sensors computes it from MQTT samples, Layout draws it.
// Putting it in ScreenModel.h would make Sensors.h depend on the display model;
// putting it in Sensors.h would drag MqttClient.h into the host preview build.
//
// Only Rising and Falling draw anything. Steady and Unknown are both blank on
// screen — the arrow answers "which way is it going", and a third symbol for
// "it isn't" would be one more thing to learn for the case where the number
// alone already says everything.
//
// They stay separate values regardless, because /healthz needs to tell them
// apart: Unknown means no basis for an opinion yet (less than trend_win_min of
// history, or a gap in the data), Steady means measured and flat. That is the
// difference between "just rebooted" and "sensor is not publishing".
enum class Trend : int8_t { Unknown, Steady, Rising, Falling };

inline const char* trendName(Trend t) {
  switch (t) {
    case Trend::Rising:  return "rising";
    case Trend::Falling: return "falling";
    case Trend::Steady:  return "steady";
    default:             return "unknown";
  }
}
