#include "Sensors.h"

#include <cinttypes>
#include <cstring>
#include <ctime>
#include <regex>

#include "esp_log.h"
#include "freertos/task.h"

namespace {
constexpr char TAG[] = "sensors";
constexpr char kGridDayKey[] = "grid_day";
}  // namespace

int32_t localDayKey() {
  const time_t now = time(nullptr);
  if (now < 1700000000) return 0;  // same threshold as main.cpp's kPlausibleTime
  struct tm t;
  localtime_r(&now, &t);
  return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
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

  // The only float -> fixed-point conversion in the data path. cJSON handed us a
  // double (it parses every JSON number as one); from here on it is all integers.
  b->dest->tenths = tenthsFromDegrees(v);
  b->dest->at = nowMonoS();
  b->dest->everSeen = true;
  const Settings& s = b->self->settings_;
  b->dest->updateTrend(s.trendWinMin, s.trendTenths);
  ++b->self->messages_;
  char shown[12];
  formatTenths(shown, sizeof shown, b->dest->tenths);
  // Logs the stored tenths, not the raw double: if fromDegrees ever clamps or
  // rounds surprisingly, the log should show what we actually kept.
  ESP_LOGI(TAG, "%s %s=%s (%s)", b->name, b->field->c_str(), shown,
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
      // They fall back to "--" on screen; don't claim otherwise here.
      ESP_LOGW(TAG, "%s topic not configured", s.name);
      continue;
    }
    *s.binding = Binding{this, s.dest, &s.field, s.name};

    subscribeExact(mqtt, s.topic, &Sensors::onMessage, s.binding);
    ESP_LOGI(TAG, "%s <- %s field '%s'", s.name, s.topic.c_str(),
             s.field.c_str());
  }

  if (settings_.gridTopic.empty()) {
    ESP_LOGW(TAG, "grid topic not configured");
    return;
  }
  std::string saved;
  if (nvs_.retrieve(kGridDayKey, saved)) {
    long key = 0;
    long long base = 0;
    if (sscanf(saved.c_str(), "%ld %lld", &key, &base) == 2) {
      grid_.restoreDay(static_cast<int32_t>(key), base);
      ESP_LOGI(TAG, "grid: restored day %ld baseline %lld Wh", key, base);
    }
  }
  subscribeExact(mqtt, settings_.gridTopic, &Sensors::onGridMessage, this);
  ESP_LOGI(TAG, "grid <- %s import '%s' export '%s'", settings_.gridTopic.c_str(),
           settings_.gridImportField.c_str(), settings_.gridExportField.c_str());
}

void Sensors::subscribeExact(MqttClient& mqtt, const std::string& topic,
                             HandlerFunc handler, void* context) {
  // MqttClient dispatches on a std::regex match against the topic. These are
  // exact topics, so escape any regex metacharacters ('+' and '$' are legal in
  // MQTT topic names and both mean something to std::regex).
  std::string pattern;
  for (char c : topic) {
    if (std::strchr(R"(\^$.|?*+()[]{})", c)) pattern += '\\';
    pattern += c;
  }
  mqtt.subscribe(topic);
  mqtt.registerHandler(topic, std::regex(pattern), handler, context);
}

esp_err_t Sensors::onGridMessage(MqttClient*, const std::string& topic,
                                 const JsonWrapper& json, void* context) {
  auto* self = static_cast<Sensors*>(context);
  const Settings& s = self->settings_;
  double imp = 0.0, exp = 0.0;
  if (!json.GetField(s.gridImportField, imp) ||
      !json.GetField(s.gridExportField, exp)) {
    ESP_LOGD(TAG, "%s: missing '%s'/'%s'", topic.c_str(),
             s.gridImportField.c_str(), s.gridExportField.c_str());
    return ESP_OK;
  }

  bool rebased;
  int64_t base;
  int32_t day;
  {
    std::lock_guard<std::mutex> lock(self->gridMutex_);
    rebased = self->grid_.add(nowMonoS(), GridEnergy::whFromKwh(imp),
                              GridEnergy::whFromKwh(exp), localDayKey());
    base = self->grid_.dayBaseNetWh;
    day = self->grid_.dayKey;
  }
  ++self->messages_;
  ESP_LOGI(TAG, "grid import=%.2f export=%.2f kWh", imp, exp);

  // Once a day (plus after a meter reset): one small NVS write, so a reboot
  // mid-afternoon does not restart "today" from zero.
  if (rebased) {
    char buf[40];
    snprintf(buf, sizeof buf, "%" PRId32 " %" PRId64, day, base);
    self->nvs_.store(kGridDayKey, buf);
    ESP_LOGI(TAG, "grid: new day %" PRId32 ", baseline %" PRId64 " Wh", day, base);
  }

  if (self->notify_) xTaskNotifyGive(self->notify_);
  return ESP_OK;
}
