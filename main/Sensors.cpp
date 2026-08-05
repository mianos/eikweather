#include "Sensors.h"

#include <cstring>
#include <regex>

#include "esp_log.h"
#include "freertos/task.h"

namespace {
constexpr char TAG[] = "sensors";
}

esp_err_t Sensors::onMessage(MqttClient*, const std::string& topic,
                             const JsonWrapper& json, void* context) {
  auto* b = static_cast<Binding*>(context);
  if (!b || !b->self || !b->dest || !b->field) return ESP_OK;

  double v = 0.0;
  if (!json.GetField(*b->field, v)) {
    // Not an error worth shouting about on every message: a device may publish
    // several payload shapes to the same topic (Tasmota does), and only some
    // carry the field we want.
    ESP_LOGD(TAG, "%s: no '%s' in payload", topic.c_str(), b->field->c_str());
    return ESP_OK;
  }

  b->dest->value = static_cast<float>(v);
  b->dest->at = time(nullptr);
  b->dest->everSeen = true;
  const Settings& s = b->self->settings_;
  b->dest->updateTrend(s.trendWinMin, s.trendTenths);
  ++b->self->messages_;
  ESP_LOGI(TAG, "%s %s=%.1f (%s)", b->name, b->field->c_str(), v,
           trendName(b->dest->trend));

  // Wake the display task so it can decide whether this actually changed what is
  // drawn. It compares the FORMATTED strings, so a 0.01 degC wobble that does not
  // alter the display costs nothing.
  if (b->self->notify_) xTaskNotifyGive(b->self->notify_);
  return ESP_OK;
}

void Sensors::attach(MqttClient& mqtt) {
  struct Spec {
    const std::string& topic;
    const std::string& field;
    Reading* dest;
    Binding* binding;
    const char* name;
  };
  const Spec specs[] = {
      {settings_.insideTopic, settings_.insideField, &inside_, &insideBinding_,
       "inside"},
      {settings_.outsideTopic, settings_.outsideField, &outside_,
       &outsideBinding_, "outside"},
  };

  for (const Spec& s : specs) {
    if (s.topic.empty()) {
      ESP_LOGW(TAG, "%s topic not configured — will show \"--\"", s.name);
      continue;
    }
    *s.binding = Binding{this, s.dest, &s.field, s.name};

    // MqttClient dispatches on a std::regex match against the topic. These are
    // exact topics, so escape any regex metacharacters ('+' and '$' are legal in
    // MQTT topic names and both mean something to std::regex).
    std::string pattern;
    for (char c : s.topic) {
      if (std::strchr(R"(\^$.|?*+()[]{})", c)) pattern += '\\';
      pattern += c;
    }

    mqtt.subscribe(s.topic);
    mqtt.registerHandler(s.topic, std::regex(pattern), &Sensors::onMessage,
                         s.binding);
    ESP_LOGI(TAG, "%s <- %s field '%s'", s.name, s.topic.c_str(),
             s.field.c_str());
  }
}
