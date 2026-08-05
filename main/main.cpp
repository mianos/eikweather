// einkclock — Lonely Binary ESP32 e-ink board: 2.13" tri-colour SSD1680 showing
// indoor and outdoor temperature from local MQTT, plus an Open-Meteo forecast.
//
// THE CONSTRAINT THAT SHAPES EVERYTHING: this tri-colour panel has no partial
// refresh and a full one costs 19-25 s of visible flashing (measured; it rises as
// the panel gets colder). Partial refresh was tested and is not available on this
// panel — see README.
//
// So this deliberately is NOT a clock. It repaints only when the drawn content
// actually changes, compared on the FORMATTED strings, rate-limited by
// min_interval_min. Indoor temperature that wobbles by 0.01 degC costs nothing;
// a genuine change of a displayed digit costs one refresh.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "App.h"
#include "ClockWebServer.h"
#include "Epaper.h"
#include "Gfx.h"
#include "MqttClient.h"
#include "NvsStorageManager.h"
#include "ScreenModel.h"
#include "Sensors.h"
#include "Settings.h"
#include "WeatherClient.h"
#include "WifiManager.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "fonts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

constexpr char TAG[] = "einkclock";

constexpr gpio_num_t kLedPin = GPIO_NUM_2;      // lit while the panel refreshes
constexpr gpio_num_t kButtonPin = GPIO_NUM_34;  // probed only; no pull-up on this board

// Below this, time() is the epoch rather than a real clock. SNTP matters even
// though nothing shows the time: mbedTLS needs a plausible clock to validate the
// Open-Meteo certificate.
constexpr time_t kPlausibleTime = 1700000000;

SemaphoreHandle_t s_gotIp = nullptr;
TaskHandle_t s_displayTask = nullptr;

void onGotIp(void*, esp_event_base_t, int32_t, void*) {
  if (s_gotIp) xSemaphoreGive(s_gotIp);
}

void onSntpSync(struct timeval* tv) {
  ESP_LOGI(TAG, "sntp: time synced (epoch %lld)",
           static_cast<long long>(tv->tv_sec));
}

void validateLatLon(const Settings& s) {
  char* end = nullptr;
  const double lat = strtod(s.latitude.c_str(), &end);
  const bool latOk = end && *end == '\0' && std::isfinite(lat) && lat >= -90.0 &&
                     lat <= 90.0;
  end = nullptr;
  const double lon = strtod(s.longitude.c_str(), &end);
  const bool lonOk = end && *end == '\0' && std::isfinite(lon) &&
                     lon >= -180.0 && lon <= 180.0;
  if (!latOk || !lonOk) {
    ESP_LOGW(TAG, "suspicious location lat='%s' lon='%s' — expect HTTP 400",
             s.latitude.c_str(), s.longitude.c_str());
  }
}

// Required because CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y.
void otaVerifyTask(void*) {
  esp_ota_img_states_t state;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    if (s_gotIp && xSemaphoreTake(s_gotIp, pdMS_TO_TICKS(120000)) == pdTRUE) {
      ESP_LOGI(TAG, "OTA image verified (got IP), cancelling rollback");
      esp_ota_mark_app_valid_cancel_rollback();
      xSemaphoreGive(s_gotIp);
    } else {
      ESP_LOGE(TAG, "no IP within 120 s — rolling back");
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
  }
  vTaskDelete(nullptr);
}

// --- test patterns (bring-up stages 3-6) ---------------------------------

void drawCalibration(epd::Panel& p) {
  const int w = p.nativeW(), h = p.nativeH();
  for (int x = 0; x < w; ++x) {
    p.setPixelNative(x, 0, epd::Color::Black);
    p.setPixelNative(x, h - 1, epd::Color::Black);
  }
  for (int y = 0; y < h; ++y) {
    p.setPixelNative(0, y, epd::Color::Black);
    p.setPixelNative(w - 1, y, epd::Color::Black);
  }
  for (int i = 0; i < (w < h ? w : h); ++i)
    p.setPixelNative(i, i, epd::Color::Red);
  for (int x = 0; x < w; x += 20)
    for (int t = 0; t < 10; ++t) p.setPixelNative(x, t, epd::Color::Black);
  for (int y = 0; y < h; y += 20)
    for (int t = 0; t < 10; ++t) p.setPixelNative(t, y, epd::Color::Black);
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) p.setPixelNative(x, y, epd::Color::Red);
}

void drawColorBars(epd::Panel& p) {
  const int w = p.width(), h = p.height();
  epd::fillRect(p, 4, 4, w / 2 - 8, h / 2 - 8, epd::Color::Black);
  epd::fillRect(p, w / 2 + 4, 4, w / 2 - 8, h / 2 - 8, epd::Color::Red);
  epd::drawText(p, epd::Font_Label, 6, h - 8, "BLACK", epd::Color::Black);
  epd::drawText(p, epd::Font_Label, w / 2 + 6, h - 8, "RED", epd::Color::Red);
}

void drawRotationProbe(epd::Panel& p) {
  epd::drawRect(p, 0, 0, p.width(), p.height(), epd::Color::Black);
  epd::drawText(p, epd::Font_Label, 4, 24, "TOP-LEFT", epd::Color::Black);
  epd::fillRect(p, 0, 0, 12, 12, epd::Color::Red);
  epd::drawTextRight(p, epd::Font_Label, p.width() - 4, p.height() - 6,
                     "bottom-right", epd::Color::Black);
}

// --- the display task -----------------------------------------------------

// Format one reading. Stale or never-seen becomes "--", which is honest: this
// display repaints rarely, so a plausible but hours-old number is worse than
// visibly having no number.
void formatReading(const Reading& r, int staleMin, char* out, size_t n,
                   bool* valid) {
  if (r.fresh(staleMin)) {
    snprintf(out, n, "%.1f", r.value);
    *valid = true;
  } else {
    snprintf(out, n, "%s", "--");
    *valid = false;
  }
}

void buildModel(const App& app, ScreenModel& m) {
  const Settings& s = *app.settings;

  snprintf(m.insideLabel, sizeof m.insideLabel, "%s", s.insideLabel.c_str());
  snprintf(m.outsideLabel, sizeof m.outsideLabel, "%s", s.outsideLabel.c_str());
  formatReading(app.sensors->inside(), s.sensorStaleMin, m.insideTemp,
                sizeof m.insideTemp, &m.insideValid);
  formatReading(app.sensors->outside(), s.sensorStaleMin, m.outsideTemp,
                sizeof m.outsideTemp, &m.outsideValid);

  // "<condition> <lo>/<hi> <rain>%" — tools/preview asserts this fits 246px.
  if (app.current.valid) {
    const char* cond = wmoText(app.current.code);
    if (app.current.rainPct >= 0) {
      snprintf(m.forecast, sizeof m.forecast, "%s %d/%d %d%%", cond,
               static_cast<int>(lroundf(app.current.lo)),
               static_cast<int>(lroundf(app.current.hi)), app.current.rainPct);
    } else {
      snprintf(m.forecast, sizeof m.forecast, "%s %d/%d", cond,
               static_cast<int>(lroundf(app.current.lo)),
               static_cast<int>(lroundf(app.current.hi)));
    }
  } else {
    snprintf(m.forecast, sizeof m.forecast, "%s", "forecast unavailable");
  }
}

// Is the screen showing everything it is meant to? Used to let the very first
// complete picture through immediately: at boot the MQTT readings arrive within
// a second (the topics are retained) but the forecast needs DNS, TLS and often a
// retry, so without this exception the panel sits on "forecast unavailable" for a
// full min_interval_min. One extra refresh per boot is nothing against panel life.
bool complete(const ScreenModel& m) {
  return m.insideValid && m.outsideValid && strstr(m.forecast, "unavailable") == nullptr;
}

// Everything that is actually drawn. Comparing this is what decides whether a
// repaint is worth 20+ seconds of flashing — far more robust than a temperature
// delta threshold, because it is exactly the question "would the screen differ?".
bool sameAsDrawn(const ScreenModel& a, const ScreenModel& b) {
  return strcmp(a.insideLabel, b.insideLabel) == 0 &&
         strcmp(a.insideTemp, b.insideTemp) == 0 &&
         a.insideValid == b.insideValid &&
         strcmp(a.outsideLabel, b.outsideLabel) == 0 &&
         strcmp(a.outsideTemp, b.outsideTemp) == 0 &&
         a.outsideValid == b.outsideValid &&
         strcmp(a.forecast, b.forecast) == 0;
}

void paint(App& app, const ScreenModel& m) {
  epd::Panel& p = *app.panel;
  p.clear(epd::Color::White);
  renderScreen(p, m);

  gpio_set_level(kLedPin, 1);
  const esp_err_t err = p.refresh();
  gpio_set_level(kLedPin, 0);

  if (err == ESP_OK) ++app.renderCount;
  p.hibernate();
}

void displayTask(void* arg) {
  App& app = *static_cast<App*>(arg);
  Settings& s = *app.settings;

  wifi_config_t wc = {};
  const bool provisioned =
      esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0] != 0;
  if (!provisioned) {
    ESP_LOGW(TAG, "no Wi-Fi credentials — showing the ESP-Touch setup screen");
    ScreenModel m;
    m.banner = s.sensorName.c_str();
    m.banner2 = "Run ESP-Touch v2 to set up WiFi";
    paint(app, m);
  }

  // Wait for a plausible clock before the first weather fetch: mbedTLS cannot
  // validate the certificate without one.
  for (int i = 0; i < 60 && time(nullptr) < kPlausibleTime; ++i) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  ScreenModel drawn;      // what is physically on the glass
  bool everPainted = false;
  bool everComplete = false;  // has a full picture ever been drawn?
  int64_t lastPaintUs = 0;
  int64_t lastWeatherUs = 0;
  bool weatherOk = false;

  // A FAILED fetch must be retried far sooner than the success interval. The
  // first attempt often loses a race with DNS coming up, and waiting the full
  // weather_poll_min after that leaves the forecast line blank for 15 minutes.
  constexpr int64_t kWeatherRetryUs = 60LL * 1000000;

  for (;;) {
    const int64_t nowUs = esp_timer_get_time();

    // Poll the forecast on its own schedule. Failure leaves app.current alone.
    const int64_t weatherPeriodUs =
        static_cast<int64_t>(s.weatherPollMin < 1 ? 1 : s.weatherPollMin) * 60 *
        1000000;
    const int64_t weatherDueUs = weatherOk ? weatherPeriodUs : kWeatherRetryUs;
    if (lastWeatherUs == 0 || nowUs - lastWeatherUs >= weatherDueUs) {
      // Gate on a plausible clock: mbedTLS cannot validate the certificate
      // without one, so attempting earlier just burns a guaranteed failure.
      if (time(nullptr) >= kPlausibleTime) {
        weatherOk = app.weather->fetch(app.current);
        lastWeatherUs = nowUs;
      }
    }

    // A queued test pattern pre-empts everything for one cycle.
    const int test = app.pendingTest;
    if (test != kTestNone) {
      app.pendingTest = kTestNone;
      epd::Panel& p = *app.panel;
      p.clear(test == kTestBlack ? epd::Color::Black : epd::Color::White);
      switch (test) {
        case kTestCalib:  drawCalibration(p); break;
        case kTestColors: drawColorBars(p); break;
        case kTestRot:    drawRotationProbe(p); break;
        default: break;
      }
      gpio_set_level(kLedPin, 1);
      p.refresh();
      gpio_set_level(kLedPin, 0);
      p.hibernate();
      everPainted = false;  // the glass no longer matches `drawn`
      ESP_LOGI(TAG, "test pattern %d done in %lld ms", test,
               p.lastRefreshUs() / 1000);
      continue;
    }

    ScreenModel want;
    buildModel(app, want);

    const bool changed = !everPainted || !sameAsDrawn(want, drawn);
    const int64_t sinceUs = nowUs - lastPaintUs;
    const int64_t minUs = static_cast<int64_t>(s.minIntervalSec()) * 1000000;
    // The rate limit is bypassed exactly twice per boot: for the first paint of
    // any kind, and for the first paint that has all the data.
    const bool firstComplete = !everComplete && complete(want);
    const bool allowed = !everPainted || firstComplete || sinceUs >= minUs;

    if (changed && allowed) {
      paint(app, want);
      drawn = want;
      everPainted = true;
      if (complete(want)) everComplete = true;
      lastPaintUs = esp_timer_get_time();
      app.lastStackHighWater =
          static_cast<int32_t>(uxTaskGetStackHighWaterMark(nullptr));
      ESP_LOGI(TAG, "painted %s %s | %s %s | %s | heap %u min %u | stack %d",
               want.insideLabel, want.insideTemp, want.outsideLabel,
               want.outsideTemp, want.forecast,
               static_cast<unsigned>(esp_get_free_heap_size()),
               static_cast<unsigned>(esp_get_minimum_free_heap_size()),
               static_cast<int>(app.lastStackHighWater));
    } else if (changed) {
      ESP_LOGD(TAG, "content changed but rate-limited (%lld s of %d s)",
               sinceUs / 1000000, s.minIntervalSec());
    }

    // Sleep until either something arrives (an MQTT reading, POST /refresh, a
    // settings change) or the next thing we owe: the rate-limit expiry if we are
    // holding a pending change, otherwise the next weather poll. Capped at 30 s so
    // a missed notification can never strand the display.
    int64_t waitUs;
    if (changed && !allowed) {
      waitUs = minUs - sinceUs;  // a change is pending; wake when allowed
    } else {
      const int64_t due = weatherOk ? weatherPeriodUs : kWeatherRetryUs;
      waitUs = due - (esp_timer_get_time() - lastWeatherUs);
    }
    if (waitUs < 1000000) waitUs = 1000000;
    if (waitUs > 30LL * 1000000) waitUs = 30LL * 1000000;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitUs / 1000));
  }
}

}  // namespace

extern "C" void app_main(void) {
  static NvsStorageManager nvs;
  static Settings settings(nvs);
  settings.log();
  validateLatLon(settings);

  static epd::Panel panel;
  ESP_ERROR_CHECK(panel.init());
  panel.configure(settings.panelW, settings.panelH,
                  static_cast<epd::Rotation>(settings.rotation),
                  settings.invertRed != 0,
                  static_cast<uint8_t>(settings.border),
                  static_cast<uint8_t>(settings.srcMode),
                  static_cast<uint8_t>(settings.updateMode));

  gpio_reset_pin(kLedPin);
  gpio_set_direction(kLedPin, GPIO_MODE_OUTPUT);
  gpio_set_level(kLedPin, 0);

  gpio_reset_pin(kButtonPin);
  gpio_set_direction(kButtonPin, GPIO_MODE_INPUT);
  ESP_LOGI(TAG, "GPIO34 idle level = %d (1 => external pull-up present)",
           gpio_get_level(kButtonPin));

  s_gotIp = xSemaphoreCreateBinary();

  // WiFiManager's CONSTRUCTOR does all the bring-up (esp_netif_init, the default
  // event loop, the STA netif, esp_wifi_init/start, and ESP-Touch V2 if
  // unprovisioned). The app must NOT call esp_netif_init or
  // esp_event_loop_create_default itself.
  static WiFiManager wifi(nvs, onGotIp, nullptr);
  std::string host = settings.sensorName;
  wifi.configSetHostName(host);

  // TZ + SNTP after WiFiManager, since esp_netif_sntp_init needs the STA netif.
  // Nothing displays the time, but mbedTLS needs a plausible clock to validate
  // the Open-Meteo certificate.
  setenv("TZ", settings.tz.c_str(), 1);
  tzset();
  esp_sntp_config_t sntpCfg =
      ESP_NETIF_SNTP_DEFAULT_CONFIG(settings.ntpServer.c_str());
  sntpCfg.sync_cb = onSntpSync;
  ESP_ERROR_CHECK(esp_netif_sntp_init(&sntpCfg));

  static WeatherClient weather(settings);
  static Sensors sensors(settings, nullptr);  // notify handle set below
  static App app{&settings, &panel, &weather, &sensors, &wifi};

  auto reconfigure = [] {
    panel.configure(settings.panelW, settings.panelH,
                    static_cast<epd::Rotation>(settings.rotation),
                    settings.invertRed != 0,
                    static_cast<uint8_t>(settings.border),
                    static_cast<uint8_t>(settings.srcMode),
                    static_cast<uint8_t>(settings.updateMode));
    if (s_displayTask) xTaskNotifyGive(s_displayTask);
  };
  for (const char* key : {"panel_w", "panel_h", "rotation", "invert_red",
                          "border", "src_mode", "update_mode"}) {
    settings.onChange(key, reconfigure);
  }
  // Anything that changes what is drawn or how often — takes effect immediately.
  for (const char* key : {"inside_label", "outside_label", "sensor_stale_min",
                          "min_interval_min", "weather_poll_min"}) {
    settings.onChange(key, [] {
      if (s_displayTask) xTaskNotifyGive(s_displayTask);
    });
  }
  // MQTT subscriptions are established once in Sensors::attach() below, and
  // MqttClient has no way to REMOVE a handler binding, so re-pointing a topic
  // means a restart. Say so rather than leaving it silently ineffective.
  for (const char* key : {"mqtt_server", "mqtt_port", "inside_topic",
                          "inside_field", "outside_topic", "outside_field"}) {
    settings.onChange(key, [] {
      ESP_LOGW(TAG, "MQTT settings changed — POST /reboot to apply");
    });
  }

  xTaskCreate(otaVerifyTask, "ota_verify", 4096, nullptr, 4, nullptr);
  // Stack 8192: the mbedTLS handshake dominates. Priority 4 is far below Wi-Fi
  // (23) and lwIP (18), and the ~25 s blocking refresh yields continuously via
  // waitBusy_'s vTaskDelay.
  xTaskCreatePinnedToCore(displayTask, "display", 8192, &app, 4, &s_displayTask, 1);
  sensors.setNotify(s_displayTask);

  // MQTT last: its handlers notify the display task, which must exist first.
  static esp_mqtt_client_config_t mqttCfg = {};
  static std::string broker = "mqtt://" + settings.mqttServer + ":" +
                              std::to_string(settings.mqttPort);
  mqttCfg.broker.address.uri = broker.c_str();
  static MqttClient mqtt(mqttCfg, settings.sensorName);
  sensors.attach(mqtt);
  mqtt.start();

  if (settings.enableWeb) {
    static WebContext webctx(&wifi);
    static ClockWebServer web(&webctx, settings, app, s_displayTask);
    ESP_ERROR_CHECK(web.start());
  } else {
    ESP_LOGW(TAG, "web server disabled (enable_web=0)");
  }

  ESP_LOGI(TAG, "einkclock started; free heap %u",
           static_cast<unsigned>(esp_get_free_heap_size()));
}
