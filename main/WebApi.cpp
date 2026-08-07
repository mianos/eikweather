#include "WebApi.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include "Monotonic.h"
#include "WifiManager.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

namespace {

constexpr char TAG[] = "webapi";

// /config bodies are tiny — bound the input so a hostile Content-Length cannot
// trigger a huge std::string allocation. (Pattern copied from ws-voice's
// VoiceWebServer.cpp:26-47.)
constexpr size_t kMaxJsonBodyBytes = 4 * 1024;

std::string read_request_body(httpd_req_t* req) {
  std::string body;
  body.reserve(req->content_len);
  char buf[256];
  int remaining = req->content_len;
  while (remaining > 0) {
    int got = httpd_req_recv(
        req, buf, std::min<int>(remaining, static_cast<int>(sizeof(buf))));
    if (got <= 0) break;
    body.append(buf, got);
    remaining -= got;
  }
  return body;
}

esp_err_t send_json(httpd_req_t* req, const JsonWrapper& json) {
  httpd_resp_set_type(req, "application/json");
  std::string out = json.ToString();
  return httpd_resp_sendstr(req, out.c_str());
}

// Chunked-response sink for Panel::writePbm.
void pbmEmit(void* ctx, const char* data, size_t len) {
  httpd_resp_send_chunk(static_cast<httpd_req_t*>(ctx), data, len);
}

}  // namespace

esp_err_t WebApi::start() {
  esp_err_t r = WebServer::start();
  if (r != ESP_OK) return r;

  struct Route {
    const char* uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t*);
  };
  const std::array<Route, 8> routes = {{
      {"/config", HTTP_GET, config_get_handler},
      {"/config", HTTP_POST, config_post_handler},
      {"/config/reset", HTTP_POST, config_reset_post_handler},
      {"/refresh", HTTP_POST, refresh_post_handler},
      {"/test", HTTP_POST, test_post_handler},
      {"/screen.pbm", HTTP_GET, screen_get_handler},
      {"/firmware", HTTP_POST, firmware_post_handler},
      {"/reboot", HTTP_POST, reboot_post_handler},
  }};

  for (const Route& route : routes) {
    httpd_uri_t uri = {
        .uri = route.uri,
        .method = route.method,
        .handler = route.handler,
        .user_ctx = this,
    };
    esp_err_t err = httpd_register_uri_handler(server, &uri);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "register %s %s: %s",
               route.method == HTTP_POST ? "POST" : "GET", route.uri,
               esp_err_to_name(err));
      return err;
    }
  }
  return ESP_OK;
}

void WebApi::populate_healthz_fields(WebContext*, JsonWrapper& json) {
  const esp_app_desc_t* desc = esp_app_get_description();
  const esp_partition_t* running = esp_ota_get_running_partition();
  json.AddItem("version", std::string(desc->version));
  json.AddItem("partition", std::string(running->label));

  json.AddItem("uptime_s", static_cast<int>(esp_timer_get_time() / 1000000));
  // Immediately after uptime, because they are what uptime alone cannot tell you.
  // reset_reason explains THIS boot ("sw" for POST /reboot or an OTA, "panic" /
  // "task_wdt" / "brownout" for the interesting cases). boot_count is persisted in
  // NVS, so comparing it between two polls reveals a restart that happened while
  // nobody was looking — a falling uptime is easy to misread as a slow reply.
  json.AddItem("reset_reason", std::string(app_.resetReason));
  json.AddItem("boot_count", app_.bootCount);
  json.AddItem("heap_free", static_cast<int>(esp_get_free_heap_size()));
  // The number that actually matters for the RAM budget: it captures the TLS
  // handshake peak. Watch it across a soak — a downward trend is a leak.
  json.AddItem("heap_min", static_cast<int>(esp_get_minimum_free_heap_size()));
  json.AddItem("display_stack_free", static_cast<int>(app_.lastStackHighWater));

  json.AddItem("render_count", static_cast<int>(app_.renderCount));
  json.AddItem("refresh_count", static_cast<int>(app_.panel->refreshCount()));
  json.AddItem("refresh_ms",
               static_cast<int>(app_.panel->lastRefreshUs() / 1000));
  json.AddItem("busy_timeouts", static_cast<int>(app_.panel->busyTimeouts()));
  json.AddItem("busy_level", app_.panel->busyLevel());

  json.AddItem("weather_valid", app_.current.valid);
  json.AddItem("weather_failures",
               static_cast<int>(app_.weather->consecutiveFailures()));
  json.AddItem("weather_status", app_.weather->lastStatus());
  json.AddItem("weather_error", std::string(app_.weather->lastError()));
  json.AddItem("wmo_code", app_.current.code);
  json.AddItem("wmo_text", std::string(wmoText(app_.current.code)));
  json.AddItem("lo", app_.current.lo);
  json.AddItem("hi", app_.current.hi);
  json.AddItem("rain_pct", app_.current.rainPct);
  // Every *_age_s below is computed against the MONOTONIC clock (Monotonic.h), the
  // same base the stamps were taken on. It used to be time(nullptr), which meant an
  // age could read as ~56 years for a reading that had arrived seconds earlier, if it
  // landed before SNTP synced. local_time further down is the one field here that
  // genuinely wants the wall clock.
  const int64_t now = nowMonoS();
  json.AddItem("weather_age_s",
               app_.current.valid ? static_cast<int>(now - app_.current.fetchedAt)
                                  : -1);

  // Local MQTT readings — the primary content, so surface enough to debug a
  // wrong topic or field name without a serial cable.
  //
  // Readings are stored as integer tenths and divided back out HERE, at the
  // serialisation edge, because JSON has no fixed-point type. That division is the
  // only floating point left downstream of ingest, and it never feeds a decision.
  const Reading& in = app_.sensors->inside();
  const Reading& out = app_.sensors->outside();
  const Reading& hw = app_.sensors->water();
  json.AddItem("mqtt_messages", static_cast<int>(app_.sensors->messages()));
  json.AddItem("inside_topic", settings_.insideTopic);
  json.AddItem("inside_seen", in.everSeen);
  json.AddItem("inside_value", in.tenths / 10.0);
  json.AddItem("inside_age_s", in.everSeen ? static_cast<int>(now - in.at) : -1);
  json.AddItem("inside_fresh", in.fresh(settings_.sensorStaleMin));
  // trend_age_s is how long the current anchor has been held: an "unknown" trend
  // with an age below trend_win_min * 60 just means not enough history yet, which
  // is a very different diagnosis from a sensor that is not publishing.
  json.AddItem("inside_trend", trendName(in.trend));
  json.AddItem("inside_trend_age_s",
               in.haveRef ? static_cast<int>(now - in.refAt) : -1);
  json.AddItem("outside_topic", settings_.outsideTopic);
  json.AddItem("outside_seen", out.everSeen);
  json.AddItem("outside_value", out.tenths / 10.0);
  json.AddItem("outside_age_s", out.everSeen ? static_cast<int>(now - out.at) : -1);
  json.AddItem("outside_fresh", out.fresh(settings_.sensorStaleMin));
  json.AddItem("outside_trend", trendName(out.trend));
  json.AddItem("outside_trend_age_s",
               out.haveRef ? static_cast<int>(now - out.refAt) : -1);
  // The optional third reading. water_topic "" means it is not configured, which
  // is indistinguishable on screen from "configured but stale" — both are blank —
  // so surface enough here to tell them apart.
  json.AddItem("water_topic", settings_.waterTopic);
  json.AddItem("water_seen", hw.everSeen);
  json.AddItem("water_value", hw.tenths / 10.0);
  json.AddItem("water_age_s", hw.everSeen ? static_cast<int>(now - hw.at) : -1);
  json.AddItem("water_fresh", hw.fresh(settings_.sensorStaleMin));
  // Computed for free by the shared handler and worth surfacing even though there
  // is no room to draw it: "is the heat pump actually heating right now" is the
  // most useful thing about a tank temperature.
  json.AddItem("water_trend", trendName(hw.trend));
  json.AddItem("water_trend_age_s",
               hw.haveRef ? static_cast<int>(now - hw.refAt) : -1);

  // The red "!" in the top-right corner. Reported right after the per-source ages
  // above, because those are the answer to "why is it on": whichever of
  // inside/outside/water/weather has an age past alert_age_min * 60 and was once
  // seen. A source that has NEVER been seen never triggers it — that case shows as
  // "--" on the glass instead.
  json.AddItem("alert", app_.alertActive);
  json.AddItem("alert_age_min", settings_.alertAgeMin);

  json.AddItem("panel_w", settings_.panelW);
  json.AddItem("panel_h", settings_.panelH);
  json.AddItem("rotation", settings_.rotation);

  // Power management, read back from the DRIVER rather than from settings_: the
  // setting is what was asked for, this is what esp_pm actually applied. They
  // differ if the sdkconfig lost CONFIG_PM_ENABLE, which is otherwise invisible.
  // cpu_min_mhz == cpu_max_mhz means DFS is off.
  //
  // wifi_ps is here because it GATES light sleep: WIFI_PS_NONE (0) means the Wi-Fi
  // driver is holding a no-light-sleep lock and the chip never sleeps regardless of
  // what light_sleep says.
  esp_pm_config_t pm = {};
  if (esp_pm_get_configuration(&pm) == ESP_OK) {
    json.AddItem("cpu_max_mhz", pm.max_freq_mhz);
    json.AddItem("cpu_min_mhz", pm.min_freq_mhz);
    json.AddItem("light_sleep", pm.light_sleep_enable);
  }
  wifi_ps_type_t ps = WIFI_PS_NONE;
  if (esp_wifi_get_ps(&ps) == ESP_OK) {
    json.AddItem("wifi_ps", static_cast<int>(ps));
  }

  // The one field here that genuinely wants the WALL clock, so it takes its own
  // reading rather than reusing `now` — which is monotonic seconds since boot and
  // would otherwise have been formatted as a date somewhere in January 1970.
  const time_t wallNow = time(nullptr);
  char localNow[32] = "";
  if (wallNow > 1700000000) {
    struct tm t;
    localtime_r(&wallNow, &t);
    strftime(localNow, sizeof localNow, "%F %T", &t);
  }
  json.AddItem("local_time", std::string(localNow));
}

esp_err_t WebApi::config_get_handler(httpd_req_t* req) {
  auto* self = static_cast<WebApi*>(req->user_ctx);
  JsonWrapper resp = self->settings_.toJson();
  return send_json(req, resp);
}

// POST /config — apply any subset of the settings keys, persist, return the new
// full settings. The panel tunables take effect immediately via onChange hooks
// registered in main.cpp, which also kick the display task to repaint.
esp_err_t WebApi::config_post_handler(httpd_req_t* req) {
  auto* self = static_cast<WebApi*>(req->user_ctx);
  if (req->content_len > kMaxJsonBodyBytes)
    return sendJsonError(req, 413, "request body too large");
  std::string body = read_request_body(req);
  if (body.empty()) return sendJsonError(req, 400, "empty body");
  JsonWrapper json = JsonWrapper::Parse(body);
  if (json.Empty()) return sendJsonError(req, 400, "invalid JSON");

  self->settings_.loadFromJson(json);
  self->settings_.save();
  self->settings_.log();

  JsonWrapper resp = self->settings_.toJson();
  return send_json(req, resp);
}

// POST /config/reset — restore every setting to its default and persist.
// Optional body {"wifi": true} also clears Wi-Fi credentials, rebooting into
// ESP-Touch v2 provisioning.
esp_err_t WebApi::config_reset_post_handler(httpd_req_t* req) {
  auto* self = static_cast<WebApi*>(req->user_ctx);

  bool wipe_wifi = false;
  if (req->content_len > 0) {
    if (req->content_len > kMaxJsonBodyBytes)
      return sendJsonError(req, 413, "request body too large");
    std::string body = read_request_body(req);
    if (!body.empty()) {
      JsonWrapper json = JsonWrapper::Parse(body);
      if (json.Empty()) return sendJsonError(req, 400, "invalid JSON");
      json.GetField("wifi", wipe_wifi);
    }
  }

  self->settings_.resetToDefaults();
  self->settings_.save();
  self->settings_.log();
  ESP_LOGW(TAG, "config reset to defaults (wipe_wifi=%d)", wipe_wifi);

  JsonWrapper resp;
  resp.AddItem("status", std::string(wipe_wifi ? "reset+wifi_clear+reboot"
                                               : "reset"));
  resp.AddItem("wifi_cleared", wipe_wifi);
  esp_err_t r = send_json(req, resp);

  if (wipe_wifi && self->webContext && self->webContext->wifiManager) {
    ESP_LOGW(TAG, "clearing wifi credentials and rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    self->webContext->wifiManager->clear();
  }
  return r;
}

// POST /refresh — wake the display task immediately. Without this every bring-up
// experiment costs up to a full refresh period.
esp_err_t WebApi::refresh_post_handler(httpd_req_t* req) {
  auto* self = static_cast<WebApi*>(req->user_ctx);
  if (self->displayTask_) xTaskNotifyGive(self->displayTask_);
  JsonWrapper resp;
  resp.AddItem("status", std::string("refresh queued"));
  return send_json(req, resp);
}

// POST /test {"pattern":"black"|"white"|"calib"|"colors"|"rot"} — draws a bring-up
// pattern on the next wake. See the bring-up stages in README.md.
esp_err_t WebApi::test_post_handler(httpd_req_t* req) {
  auto* self = static_cast<WebApi*>(req->user_ctx);
  if (req->content_len > kMaxJsonBodyBytes)
    return sendJsonError(req, 413, "request body too large");
  std::string body = read_request_body(req);
  if (body.empty()) return sendJsonError(req, 400, "empty body");
  JsonWrapper json = JsonWrapper::Parse(body);
  if (json.Empty()) return sendJsonError(req, 400, "invalid JSON");

  std::string pattern;
  if (!json.GetField("pattern", pattern) || pattern.empty())
    return sendJsonError(req, 400, "missing \"pattern\"");

  int which = kTestNone;
  if (pattern == "black") which = kTestBlack;
  else if (pattern == "white") which = kTestWhite;
  else if (pattern == "calib") which = kTestCalib;
  else if (pattern == "colors") which = kTestColors;
  else if (pattern == "rot") which = kTestRot;
  else return sendJsonError(req, 400,
                            "pattern must be black|white|calib|colors|rot");

  self->app_.pendingTest = which;
  if (self->displayTask_) xTaskNotifyGive(self->displayTask_);

  JsonWrapper resp;
  resp.AddItem("status", std::string("queued"));
  resp.AddItem("pattern", pattern);
  return send_json(req, resp);
}

// GET /screen.pbm — what the device THINKS it drew. Separates a layout bug from a
// panel bug without a camera.
esp_err_t WebApi::screen_get_handler(httpd_req_t* req) {
  auto* self = static_cast<WebApi*>(req->user_ctx);
  httpd_resp_set_type(req, "image/x-portable-bitmap");
  self->app_.panel->writePbm(req, pbmEmit);
  return httpd_resp_send_chunk(req, nullptr, 0);  // terminate the chunked response
}

// POST /firmware — raw .bin body streamed into the inactive OTA slot, which is
// then set as the next boot partition, followed by a reboot.
// Deploy: curl --data-binary @build/einkweather.bin http://<host>/firmware
esp_err_t WebApi::firmware_post_handler(httpd_req_t* req) {
  const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
  if (!target) return sendJsonError(req, 500, "no OTA partition available");
  if (req->content_len <= 0)
    return sendJsonError(req, 400, "empty body (POST the .bin)");
  if (static_cast<size_t>(req->content_len) > target->size)
    return sendJsonError(req, 413, "image larger than the OTA partition");

  ESP_LOGW(TAG, "OTA: %d bytes -> %s", req->content_len, target->label);

  esp_ota_handle_t handle = 0;
  esp_err_t err = esp_ota_begin(target, req->content_len, &handle);
  if (err != ESP_OK) return sendJsonError(req, 500, esp_err_to_name(err));

  char buf[1024];
  int remaining = req->content_len;
  while (remaining > 0) {
    const int got = httpd_req_recv(
        req, buf, std::min<int>(remaining, static_cast<int>(sizeof buf)));
    if (got <= 0) {
      esp_ota_abort(handle);
      return sendJsonError(req, 400, "upload aborted");
    }
    err = esp_ota_write(handle, buf, got);
    if (err != ESP_OK) {
      esp_ota_abort(handle);
      return sendJsonError(req, 500, esp_err_to_name(err));
    }
    remaining -= got;
  }

  err = esp_ota_end(handle);
  if (err != ESP_OK) return sendJsonError(req, 500, esp_err_to_name(err));
  err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) return sendJsonError(req, 500, esp_err_to_name(err));

  JsonWrapper resp;
  resp.AddItem("status", std::string("ok, rebooting"));
  resp.AddItem("partition", std::string(target->label));
  esp_err_t r = send_json(req, resp);
  ESP_LOGW(TAG, "OTA complete, rebooting into %s", target->label);
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return r;
}

// POST /reboot — a plain restart.
//
// Needed because MQTT subscriptions are established once, in Sensors::attach()
// during app_main, so changing inside_topic/outside_topic takes effect only on
// restart. mianesp's base WebServer offers POST /reset, but that CLEARS Wi-FI
// CREDENTIALS and reboots into provisioning, which is not what you want after
// merely editing a topic.
esp_err_t WebApi::reboot_post_handler(httpd_req_t* req) {
  JsonWrapper resp;
  resp.AddItem("status", std::string("rebooting"));
  esp_err_t r = send_json(req, resp);
  ESP_LOGW(TAG, "reboot requested over HTTP");
  vTaskDelay(pdMS_TO_TICKS(500));  // let the response flush
  esp_restart();
  return r;
}
