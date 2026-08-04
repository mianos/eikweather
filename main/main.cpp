// einkclock — Lonely Binary ESP32 e-ink board: 2.13" tri-colour SSD1680 showing a
// clock and current weather, repainting on the wall-clock 5-minute boundary.
//
// THE CONSTRAINT THAT SHAPES EVERYTHING: this tri-colour panel has no partial
// refresh and takes ~27-30 s for a full one. A ticking clock is not possible. So
// the time is shown rounded to the refresh boundary, and the paint is STARTED
// early (renderLeadS) so that it FINISHES at :00/:05/... rather than beginning
// there.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "App.h"
#include "ClockWebServer.h"
#include "Epaper.h"
#include "Gfx.h"
#include "NvsStorageManager.h"
#include "ScreenModel.h"
#include "Settings.h"
#include "WeatherClient.h"
#include "WifiManager.h"
#include "driver/gpio.h"
#include "fonts.h"  // test patterns draw text
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

constexpr char TAG[] = "einkclock";

constexpr gpio_num_t kLedPin = GPIO_NUM_2;     // lit while the panel is refreshing
constexpr gpio_num_t kButtonPin = GPIO_NUM_34;  // probed only; see the note below

// Anything below this is not a real clock — it is the epoch. Same sentinel
// ws-voice uses. NEVER strftime for display before time(nullptr) clears it: a
// wrong time burned into e-paper for five minutes is worse than a blank area.
constexpr time_t kPlausibleTime = 1700000000;

SemaphoreHandle_t s_gotIp = nullptr;
TaskHandle_t s_displayTask = nullptr;


void onGotIp(void*, esp_event_base_t, int32_t, void*) {
  if (s_gotIp) xSemaphoreGive(s_gotIp);
}

void onSntpSync(struct timeval* tv) {
  ESP_LOGI(TAG, "sntp: time synced (epoch %lld)",
           static_cast<long long>(tv->tv_sec));
  if (s_displayTask) xTaskNotifyGive(s_displayTask);  // paint the real clock now
}

// strtod sanity check on the lat/lon strings. Warn only — a bad value simply makes
// Open-Meteo return 400, which the fetch-failure path already handles.
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

// Required because CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y: a freshly-OTA'd image
// boots PENDING_VERIFY and must be marked valid, or the bootloader rolls it back.
// Gate on actually reaching the network, so an image that cannot associate is
// rejected rather than blessed.
void otaVerifyTask(void*) {
  esp_ota_img_states_t state;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    if (s_gotIp && xSemaphoreTake(s_gotIp, pdMS_TO_TICKS(120000)) == pdTRUE) {
      ESP_LOGI(TAG, "OTA image verified (got IP), cancelling rollback");
      esp_ota_mark_app_valid_cancel_rollback();
      xSemaphoreGive(s_gotIp);  // let anyone else waiting through
    } else {
      ESP_LOGE(TAG, "no IP within 120 s — rolling back");
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
  }
  vTaskDelete(nullptr);
}

// --- test patterns (bring-up stages 3-6) ---------------------------------

void drawCalibration(epd::Panel& p) {
  // NATIVE coordinates, bypassing rotation, so this tests geometry alone.
  // A 1px border on all four panel edges + a diagonal + 10px ticks every 20px +
  // an 8x8 block at the native origin.
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
  // Black bar beside a red bar, plus text of each. Diagnosis:
  //   red bar RED             -> correct
  //   red bar BLACK           -> polarity inverted, set invert_red=1
  //   red bar WHITE/invisible -> the 0x26 plane is not landing at all
  const int w = p.width(), h = p.height();
  epd::fillRect(p, 4, 4, w / 2 - 8, h / 2 - 8, epd::Color::Black);
  epd::fillRect(p, w / 2 + 4, 4, w / 2 - 8, h / 2 - 8, epd::Color::Red);
  epd::drawText(p, epd::Font_Date, 6, h - 8, "BLACK", epd::Color::Black);
  epd::drawText(p, epd::Font_Date, w / 2 + 6, h - 8, "RED", epd::Color::Red);
}

void drawRotationProbe(epd::Panel& p) {
  // Unambiguous only if you can tell which corner is which.
  epd::drawRect(p, 0, 0, p.width(), p.height(), epd::Color::Black);
  epd::drawText(p, epd::Font_Date, 4, 24, "TOP-LEFT", epd::Color::Black);
  epd::fillRect(p, 0, 0, 12, 12, epd::Color::Red);
  epd::drawTextRight(p, epd::Font_Date, p.width() - 4, p.height() - 6,
                     "bottom-right", epd::Color::Black);
}

// --- the display task -----------------------------------------------------

// Seconds until the next paint should START, derived from the LOCAL minute of the
// hour rather than from epoch arithmetic.
//
// Epoch arithmetic (((now/period)+1)*period) happens to coincide with local
// :00/:05/... for every real timezone offset, because all offsets are multiples of
// 15 minutes — but it silently breaks for a period that does not divide 15 (a
// 4-minute period against UTC+05:45, say). The tm_min form is correct for any
// period dividing 60, and is inherently DST-safe because localtime_r is
// re-evaluated every iteration.
int secondsToNextStart(const Settings& s) {
  const int period = s.refreshPeriodMin() * 60;
  const time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  const int intoHour = t.tm_min * 60 + t.tm_sec;
  const int toBoundary = period - (intoHour % period);  // 1..period
  int wait = toBoundary - s.renderLeadS;
  // `while`, not `if`: a renderLeadS larger than the whole period (a plausible
  // misconfiguration, e.g. lead=40 with refresh_min=1) would leave wait negative
  // after a single correction, and the caller's `while (wait > 0)` loop would then
  // never sleep — painting flat out in a hot loop.
  while (wait <= 0) wait += period;
  return wait;
}

// Fill in the model for the boundary we are painting FOR — i.e. now + the lead —
// rounded down to the period. strftime straight from that tm, with no mktime round
// trip.
void buildModel(const App& app, ScreenModel& m) {
  const Settings& s = *app.settings;
  const time_t target = time(nullptr) + s.renderLeadS;

  if (target >= kPlausibleTime) {
    struct tm tt;
    localtime_r(&target, &tt);
    tt.tm_min -= tt.tm_min % s.refreshPeriodMin();  // the period divides 60
    tt.tm_sec = 0;
    strftime(m.clock, sizeof m.clock, "%H:%M", &tt);

    // %-d is a glibc extension that newlib does not support, and %e space-pads
    // ("Wed  4 Sep", double space). Build it explicitly. The weekday is always
    // abbreviated — see the note in ScreenModel.h.
    char wd[8], mon[8];
    strftime(wd, sizeof wd, "%a", &tt);
    strftime(mon, sizeof mon, "%b", &tt);
    snprintf(m.date, sizeof m.date, "%s %d %s", wd, tt.tm_mday, mon);
  }

  if (app.current.valid) {
    snprintf(m.temp, sizeof m.temp, "%d",
             static_cast<int>(lroundf(app.current.temp)));
    m.haveTemp = true;
    snprintf(m.cond, sizeof m.cond, "%s", wmoText(app.current.code));

    // Stale after two full periods without a successful fetch. Shown as a dashed
    // rule rather than a status line — zero layout cost.
    const int period = app.settings->refreshPeriodMin() * 60;
    m.stale = (time(nullptr) - app.current.fetchedAt) > 2 * period;
  } else {
    snprintf(m.temp, sizeof m.temp, "%s", "--");
    m.haveTemp = false;
  }
}

// One paint: draw, push to the panel, hibernate. The LED marks the ~30 s burn.
void paint(App& app, const ScreenModel& m) {
  epd::Panel& p = *app.panel;
  p.clear(epd::Color::White);
  renderScreen(p, m);

  gpio_set_level(kLedPin, 1);
  const esp_err_t err = p.refresh();
  gpio_set_level(kLedPin, 0);

  if (err == ESP_OK) {
    ++app.renderCount;
    snprintf(app.lastClock, sizeof app.lastClock, "%s", m.clock);
  }
  // hibernate() regardless: on a BUSY timeout the panel may still be wedged, and
  // 0x10 is the best attempt at leaving it in a low state.
  p.hibernate();
}

void displayTask(void* arg) {
  App& app = *static_cast<App*>(arg);
  Settings& s = *app.settings;

  // 1. Unprovisioned? Say so on the glass. MANDATORY, not a nicety: WiFiManager's
  //    only provisioning path is ESP-Touch V2 — no SoftAP, no captive portal — so
  //    with one LED and no message the device is indistinguishable from a brick.
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

  // 2. Wait for a plausible clock from SNTP.
  for (int i = 0; i < 50 && time(nullptr) < kPlausibleTime; ++i) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  // 3. If time never arrived and a boot screen is wanted, say what we know and
  //    keep waiting. Otherwise fall straight through so the FIRST paint is the
  //    real clock — no wasted 27 s and no wrong time.
  if (time(nullptr) < kPlausibleTime) {
    if (s.bootScreen) {
      ESP_LOGW(TAG, "no time yet after 25 s — painting the waiting screen");
      ScreenModel m;
      m.banner = s.sensorName.c_str();
      m.banner2 = "waiting for network time...";
      paint(app, m);
    }
    while (time(nullptr) < kPlausibleTime) vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "clock acquired");
  }

  // 4. Main loop.
  for (;;) {
    // Wait until an ABSOLUTE deadline, sleeping in <=5 s slices so a task
    // notification (POST /refresh, an SNTP sync, a live settings change) still
    // wakes us promptly.
    //
    // The deadline MUST be absolute. An earlier version re-derived a relative
    // "seconds remaining" every slice and looped `while (wait > 0)` — but
    // secondsToNextStart() clamps its result positive (it rolls forward to the
    // next period rather than returning <= 0), so that loop could never
    // terminate. The only thing that ever broke it was a notification, which
    // meant the display repainted once per SNTP resync — hourly instead of every
    // 5 minutes. It looked fine for the first cycle and was invisible until
    // render_count was compared against uptime.
    const time_t deadline = time(nullptr) + secondsToNextStart(s);
    ESP_LOGI(TAG, "next paint in %d s (period %d min, lead %d s)",
             static_cast<int>(deadline - time(nullptr)), s.refreshPeriodMin(),
             s.renderLeadS);
    bool forced = false;
    for (;;) {
      const time_t now = time(nullptr);
      if (now >= deadline) break;  // reached the paint point
      const time_t remain = deadline - now;
      const uint32_t slice = remain > 5 ? 5 : static_cast<uint32_t>(remain);
      if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(slice * 1000)) > 0) {
        forced = true;
        break;
      }
    }

    // A queued test pattern pre-empts the clock for exactly one cycle.
    const int test = app.pendingTest;
    if (test != kTestNone) {
      app.pendingTest = kTestNone;
      epd::Panel& p = *app.panel;
      p.clear(test == kTestBlack ? epd::Color::Black : epd::Color::White);
      switch (test) {
        case kTestCalib:  drawCalibration(p); break;
        case kTestColors: drawColorBars(p); break;
        case kTestRot:    drawRotationProbe(p); break;
        default: break;  // black/white are just the clear()
      }
      gpio_set_level(kLedPin, 1);
      p.refresh();
      gpio_set_level(kLedPin, 0);
      p.hibernate();
      ESP_LOGI(TAG, "test pattern %d done in %lld ms", test,
               p.lastRefreshUs() / 1000);
      continue;
    }

    // Strictly sequential: fetch, then draw, then burn. Three reasons this beats a
    // separate weather task — (a) `current` has exactly one writer, so no mutex and
    // no torn reads; (b) the TLS handshake's 25-45 KB is allocated and fully freed
    // BEFORE the panel work starts, so the two largest memory peaks never coexist
    // on a 320 KB part; (c) the fetch result is known before anything is drawn, so
    // the stale indicator comes from fact rather than a race.
    app.weather->fetch(app.current);  // failure leaves app.current untouched

    ScreenModel m;
    buildModel(app, m);
    paint(app, m);

    app.lastStackHighWater =
        static_cast<int32_t>(uxTaskGetStackHighWaterMark(nullptr));
    ESP_LOGI(TAG,
             "painted %s %s | heap %u min %u | stack free %d | %s%s",
             m.clock, m.date, static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(esp_get_minimum_free_heap_size()),
             static_cast<int>(app.lastStackHighWater), m.temp,
             m.stale ? " (STALE)" : "");
    (void)forced;
  }
}

}  // namespace

extern "C" void app_main(void) {
  // 1. FIRST: NvsStorageManager's constructor calls nvs_flash_init(), which both
  //    Settings and the Wi-Fi driver's own nvs.net80211 namespace depend on.
  static NvsStorageManager nvs;
  static Settings settings(nvs);
  settings.log();
  validateLatLon(settings);

  // 2. Panel BEFORE Wi-Fi. init() touches only SPI and GPIO — no panel traffic —
  //    so a dead or unplugged panel cannot stall boot, and a bus/pin
  //    misconfiguration shows up first in the log. It also logs the BUSY level as
  //    a wiring smoke test.
  static epd::Panel panel;
  ESP_ERROR_CHECK(panel.init());
  panel.configure(settings.panelW, settings.panelH,
                  static_cast<epd::Rotation>(settings.rotation),
                  settings.invertRed != 0,
                  static_cast<uint8_t>(settings.border),
                  static_cast<uint8_t>(settings.srcMode),
                  static_cast<uint8_t>(settings.updateMode));

  // 3. LED. GPIO2 is an ESP32 strapping pin, but it only needs to be low/floating
  //    at boot to allow download mode — we drive it long afterwards, mid-refresh.
  gpio_reset_pin(kLedPin);
  gpio_set_direction(kLedPin, GPIO_MODE_OUTPUT);
  gpio_set_level(kLedPin, 0);

  // 4. Button probe only, no behaviour. GPIO34-39 are input-only with NO internal
  //    pull-up, so mianesp's Button (whose ctor calls gpio_pullup_en and ignores
  //    the ESP_ERR_INVALID_ARG it gets back) would silently read noise unless this
  //    board fits an external pull-up. One log line tells us for free: a stable 1
  //    with the button released means an external pull-up is present and Button is
  //    safe to adopt later.
  gpio_reset_pin(kButtonPin);
  gpio_set_direction(kButtonPin, GPIO_MODE_INPUT);
  ESP_LOGI(TAG, "GPIO34 idle level = %d (1 => external pull-up present)",
           gpio_get_level(kButtonPin));

  s_gotIp = xSemaphoreCreateBinary();

  // 5. WiFiManager's CONSTRUCTOR does all the bring-up: esp_netif_init, the
  //    default event loop, esp_netif_create_default_wifi_sta, esp_wifi_init /
  //    set_mode / start, and ESP-Touch V2 if unprovisioned. We must NOT call
  //    esp_netif_init or esp_event_loop_create_default ourselves.
  static WiFiManager wifi(nvs, onGotIp, nullptr);
  std::string host = settings.sensorName;
  wifi.configSetHostName(host);

  // 6. TZ + SNTP, AFTER WiFiManager because esp_netif_sntp_init needs the STA netif
  //    to exist. mianesp has no SNTP helper; inline, as ws-voice does.
  setenv("TZ", settings.tz.c_str(), 1);
  tzset();
  esp_sntp_config_t sntpCfg =
      ESP_NETIF_SNTP_DEFAULT_CONFIG(settings.ntpServer.c_str());
  sntpCfg.sync_cb = onSntpSync;  // log the sync and repaint immediately
  ESP_ERROR_CHECK(esp_netif_sntp_init(&sntpCfg));

  static WeatherClient weather(settings);
  static App app{&settings, &panel, &weather, &wifi};

  // 7. Live-apply hooks. This is what makes bring-up stages 4-6 a matter of
  //    minutes: every panel tunable is a POST /config away from being tested.
  //    Attached AFTER construction, per SettingsBase's contract.
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
  settings.onChange("tz", [] {
    setenv("TZ", settings.tz.c_str(), 1);
    tzset();
    if (s_displayTask) xTaskNotifyGive(s_displayTask);
  });
  settings.onChange("refresh_min", [] {
    if (s_displayTask) xTaskNotifyGive(s_displayTask);
  });

  // 8. Tasks. The display task is created before the web server so the server has
  //    a valid handle to notify.
  xTaskCreate(otaVerifyTask, "ota_verify", 4096, nullptr, 4, nullptr);
  // Stack 8192: the mbedTLS handshake dominates (IDF's own esp_http_client
  // examples use 8192). Priority 4 is far below Wi-Fi (23) and lwIP (18), and the
  // 27 s blocking section yields continuously via waitBusy_'s vTaskDelay. Pinned
  // to core 1 to keep it off the core carrying the network tasks.
  xTaskCreatePinnedToCore(displayTask, "display", 8192, &app, 4, &s_displayTask, 1);

  if (settings.enableWeb) {
    static WebContext webctx(&wifi);
    static ClockWebServer web(&webctx, settings, app, s_displayTask);
    ESP_ERROR_CHECK(web.start());
  } else {
    ESP_LOGW(TAG, "web server disabled (enable_web=0) — no way to change settings");
  }

  ESP_LOGI(TAG, "einkclock started; free heap %u",
           static_cast<unsigned>(esp_get_free_heap_size()));
}
