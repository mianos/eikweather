// einkweather — Lonely Binary ESP32 e-ink board: 2.13" tri-colour SSD1680 showing
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
#include "WebApi.h"
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
#include "esp_netif.h"
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

constexpr char TAG[] = "einkweather";

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

// Do we actually have a DHCP lease? Deliberately STATE-based (interrogate the
// netif) rather than event-based: anything created after WiFiManager may already
// have missed a one-shot IP_EVENT_STA_GOT_IP, and a missed edge here would mean
// waiting out the whole timeout for a network that is already up.
bool haveIp() {
  esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!sta) return false;
  esp_netif_ip_info_t ip{};
  return esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;
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
                   bool* valid, Trend* trend) {
  if (r.fresh(staleMin)) {
    snprintf(out, n, "%.1f", r.value);
    *valid = true;
    *trend = r.trend;
  } else {
    snprintf(out, n, "%s", "--");
    *valid = false;
    // No number to qualify, so no arrow. Also stops a dead sensor's last known
    // direction sitting on the glass indefinitely.
    *trend = Trend::Unknown;
  }
}

void buildModel(const App& app, ScreenModel& m) {
  const Settings& s = *app.settings;

  snprintf(m.insideLabel, sizeof m.insideLabel, "%s", s.insideLabel.c_str());
  snprintf(m.outsideLabel, sizeof m.outsideLabel, "%s", s.outsideLabel.c_str());
  formatReading(app.sensors->inside(), s.sensorStaleMin, m.insideTemp,
                sizeof m.insideTemp, &m.insideValid, &m.insideTrend);
  formatReading(app.sensors->outside(), s.sensorStaleMin, m.outsideTemp,
                sizeof m.outsideTemp, &m.outsideValid, &m.outsideTrend);

  // The heat pump hot water tank, right-aligned on the forecast line.
  //
  // INTEGER degrees and NO label by default. Both are width decisions: this line
  // already carries the condition plus today's lo/hi (up to 188px of 246), so the
  // number gets the ~39px that is left. A tenth of a degree on a hot water tank is
  // not something anyone acts on, and a label ("HWS ") costs another 52px, which
  // would truncate the forecast text — set water_label if you want it anyway.
  //
  // Stale or unconfigured leaves it EMPTY rather than "--": the forecast then gets
  // that width back, whereas the two big rows have a dedicated row each that would
  // look broken if blank.
  const Reading& w = app.sensors->water();
  if (w.fresh(s.sensorStaleMin)) {
    // Clamped for the same reason as the rain percentage: a garbage payload must
    // not render as "-2147483648".
    long t = lroundf(w.value);
    if (t < -99) t = -99;
    if (t > 999) t = 999;
    if (s.waterLabel.empty()) {
      snprintf(m.water, sizeof m.water, "%ld", t);
    } else {
      snprintf(m.water, sizeof m.water, "%s %ld", s.waterLabel.c_str(), t);
    }
  }

  // "<condition> <lo>/<hi>" on the forecast line; the rain chance goes on the date
  // line, labelled, because a bare trailing "0%" did not say what it measured.
  if (app.current.valid) {
    snprintf(m.forecast, sizeof m.forecast, "%s %d/%d", wmoText(app.current.code),
             static_cast<int>(lroundf(app.current.lo)),
             static_cast<int>(lroundf(app.current.hi)));
    m.forecastValid = true;
    if (app.current.rainPct >= 0) {
      // Clamp: it is a percentage, and a bogus API value must not render as
      // "rain -2147483648%". Also keeps snprintf inside the buffer, which
      // -Werror=format-truncation checks against the full int range.
      const int pct = app.current.rainPct > 100 ? 100 : app.current.rainPct;
      snprintf(m.rain, sizeof m.rain, "rain %d%%", pct);
    }
  } else {
    snprintf(m.forecast, sizeof m.forecast, "%s", "forecast unavailable");
  }

  // Changes once a day, so it costs one refresh a day. Left empty until SNTP has
  // synced: a wrong date burned into e-paper is worse than no date.
  //
  // %-d is a glibc extension newlib does not support, and %e space-pads
  // ("Wed  5 Aug", double space), so assemble it explicitly.
  const time_t now = time(nullptr);
  if (now >= kPlausibleTime) {
    struct tm t;
    localtime_r(&now, &t);
    char wd[8], mon[8];
    strftime(wd, sizeof wd, "%a", &t);
    strftime(mon, sizeof mon, "%b", &t);
    snprintf(m.date, sizeof m.date, "%s %d %s", wd, t.tm_mday, mon);
  }
}

// Is the screen showing everything it is meant to? Used to let the very first
// complete picture through immediately: at boot the MQTT readings arrive within
// a second (the topics are retained) but the forecast needs DNS, TLS and often a
// retry, so without this exception the panel sits on "forecast unavailable" for a
// full min_interval_min. One extra refresh per boot is nothing against panel life.
// A flag, not strstr() on the forecast text: sniffing for "unavailable" broke
// silently the moment that string was shortened for the new layout.
bool complete(const ScreenModel& m) {
  return m.insideValid && m.outsideValid && m.forecastValid;
}

// Everything that is actually drawn. Comparing this is what decides whether a
// repaint is worth 20+ seconds of flashing — far more robust than a temperature
// delta threshold, because it is exactly the question "would the screen differ?".
bool sameAsDrawn(const ScreenModel& a, const ScreenModel& b) {
  // The trends are compared too, so a direction change earns a repaint on its
  // own. Bounded by construction: Reading::updateTrend re-evaluates at most once
  // per trend_win_min, so this can add at most two repaints per window.
  return strcmp(a.insideLabel, b.insideLabel) == 0 &&
         strcmp(a.insideTemp, b.insideTemp) == 0 &&
         a.insideValid == b.insideValid && a.insideTrend == b.insideTrend &&
         strcmp(a.outsideLabel, b.outsideLabel) == 0 &&
         strcmp(a.outsideTemp, b.outsideTemp) == 0 &&
         a.outsideValid == b.outsideValid && a.outsideTrend == b.outsideTrend &&
         strcmp(a.forecast, b.forecast) == 0 && strcmp(a.date, b.date) == 0 &&
         strcmp(a.rain, b.rain) == 0 && strcmp(a.water, b.water) == 0;
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

  // MQTT is started HERE, not at the end of app_main, because app_main finishes
  // roughly a second BEFORE DHCP hands over the lease. Starting the client then
  // guaranteed a failed connect —
  //     esp-tls: couldn't get hostname for :mqtt2.mianos.com: getaddrinfo() 202
  // — and the first reading then had to wait out esp-mqtt's 10 s reconnect
  // backoff. Nothing was broken, but the first useful paint was 10 s late for no
  // reason. The client is created in app_main and its subscriptions are already
  // queued (MqttClient::subscribe replays them from resubscribe() on connect),
  // so all that is deferred is the connect itself.
  for (int i = 0; i < 240 && !haveIp(); ++i) vTaskDelay(pdMS_TO_TICKS(250));
  if (!haveIp()) {
    ESP_LOGW(TAG, "no IP after 60 s — starting MQTT anyway, it will keep retrying");
  }
  app.mqtt->start();

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
      ESP_LOGI(TAG,
               "painted %s %s %s | %s %s %s | %s | %s | heap %u min %u | stack %d",
               want.insideLabel, want.insideTemp, trendName(want.insideTrend),
               want.outsideLabel, want.outsideTemp,
               trendName(want.outsideTrend), want.water, want.forecast,
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
  static App app{&settings, &panel, &weather, &sensors, &wifi, nullptr};

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
  for (const char* key : {"inside_label", "outside_label", "water_label",
                          "sensor_stale_min", "min_interval_min",
                          "weather_poll_min"}) {
    settings.onChange(key, [] {
      if (s_displayTask) xTaskNotifyGive(s_displayTask);
    });
  }
  // MQTT subscriptions are established once in Sensors::attach() below, and
  // MqttClient has no way to REMOVE a handler binding, so re-pointing a topic
  // means a restart. Say so rather than leaving it silently ineffective.
  for (const char* key : {"mqtt_server", "mqtt_port", "inside_topic",
                          "inside_field", "outside_topic", "outside_field",
                          "water_topic", "water_field"}) {
    settings.onChange(key, [] {
      ESP_LOGW(TAG, "MQTT settings changed — POST /reboot to apply");
    });
  }

  xTaskCreate(otaVerifyTask, "ota_verify", 4096, nullptr, 4, nullptr);

  // MQTT is CONSTRUCTED and subscribed here but NOT started — the display task
  // starts it once there is an IP. So it must exist before that task is created,
  // or the task could dereference a null app.mqtt. Constructing the client only
  // calls esp_mqtt_client_init; nothing touches the network until start().
  static esp_mqtt_client_config_t mqttCfg = {};
  static std::string broker = "mqtt://" + settings.mqttServer + ":" +
                              std::to_string(settings.mqttPort);
  mqttCfg.broker.address.uri = broker.c_str();
  static MqttClient mqtt(mqttCfg, settings.sensorName);
  sensors.attach(mqtt);  // queues the subscriptions; replayed on connect
  app.mqtt = &mqtt;

  // Stack 8192: the mbedTLS handshake dominates. Priority 4 is far below Wi-Fi
  // (23) and lwIP (18), and the ~25 s blocking refresh yields continuously via
  // waitBusy_'s vTaskDelay.
  xTaskCreatePinnedToCore(displayTask, "display", 8192, &app, 4, &s_displayTask, 1);
  sensors.setNotify(s_displayTask);

  if (settings.enableWeb) {
    static WebContext webctx(&wifi);
    static WebApi web(&webctx, settings, app, s_displayTask);
    ESP_ERROR_CHECK(web.start());
  } else {
    ESP_LOGW(TAG, "web server disabled (enable_web=0)");
  }

  ESP_LOGI(TAG, "einkweather started; free heap %u",
           static_cast<unsigned>(esp_get_free_heap_size()));
}
