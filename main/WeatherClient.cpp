#include "WeatherClient.h"

#include <cJSON.h>

#include "Settings.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

namespace {

constexpr char TAG[] = "weather";

// The response is ~350 bytes. Cap hard: a hostile or broken endpoint must not be
// able to grow this std::string without bound (a flaw in mianesp's HttpClient,
// which is one of the reasons we don't use it here).
constexpr size_t kMaxBodyBytes = 8 * 1024;

esp_err_t onEvent(esp_http_client_event_t* evt) {
  if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  auto* body = static_cast<std::string*>(evt->user_data);
  if (!body) return ESP_OK;
  if (body->size() + evt->data_len > kMaxBodyBytes) {
    ESP_LOGW(TAG, "response exceeded %u bytes, truncating",
             static_cast<unsigned>(kMaxBodyBytes));
    return ESP_OK;
  }
  body->append(static_cast<const char*>(evt->data), evt->data_len);
  return ESP_OK;
}

}  // namespace

bool WeatherClient::fetch(Weather& out) {
  // temperature_unit takes Open-Meteo's own vocabulary ("celsius"/"fahrenheit"),
  // so the setting passes straight through with no mapping table.
  //
  // `timezone` is deliberately NOT requested: we already have the local clock from
  // SNTP, and measuring staleness against our own time() at the moment of success
  // is more honest than trusting the server's `current.time`.
  std::string url = "https://api.open-meteo.com/v1/forecast?latitude=";
  url += settings_.latitude;
  url += "&longitude=";
  url += settings_.longitude;
  url += "&current=temperature_2m,weather_code&temperature_unit=";
  url += settings_.tempUnit;

  std::string body;
  body.reserve(512);

  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.method = HTTP_METHOD_GET;
  cfg.timeout_ms = 8000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;  // TLS, verified against the bundle
  cfg.event_handler = onEvent;
  cfg.user_data = &body;
  cfg.buffer_size = 1024;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    ++failures_;
    ESP_LOGE(TAG, "esp_http_client_init failed");
    return false;
  }

  bool ok = false;
  const esp_err_t err = esp_http_client_perform(client);
  lastStatus_ = esp_http_client_get_status_code(client);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "fetch failed: %s", esp_err_to_name(err));
  } else if (lastStatus_ != 200) {
    // A 400 here almost always means a malformed latitude/longitude setting.
    ESP_LOGW(TAG, "HTTP %d (check latitude/longitude): %.*s", lastStatus_,
             static_cast<int>(body.size() > 160 ? 160 : body.size()),
             body.c_str());
  } else {
    // JsonWrapper::GetField reaches only FLAT top-level keys, and
    // temperature_2m lives under "current" — so parse with cJSON directly. A
    // GetObject() on JsonWrapper cannot be made safe as it stands: it owns its
    // tree via unique_ptr<cJSON, cJSON_Delete>, so a wrapper around a child the
    // parent still owns would double-free.
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) {
      ESP_LOGW(TAG, "unparseable JSON (%u bytes)",
               static_cast<unsigned>(body.size()));
    } else {
      if (cJSON* cur = cJSON_GetObjectItemCaseSensitive(root, "current")) {
        cJSON* t = cJSON_GetObjectItemCaseSensitive(cur, "temperature_2m");
        cJSON* wc = cJSON_GetObjectItemCaseSensitive(cur, "weather_code");
        if (cJSON_IsNumber(t) && cJSON_IsNumber(wc)) {
          // Only now do we touch `out` — a partial parse must not corrupt the
          // last-known-good reading.
          out.temp = static_cast<float>(t->valuedouble);
          out.code = wc->valueint;
          out.valid = true;
          out.fetchedAt = time(nullptr);
          ok = true;
        } else {
          ESP_LOGW(TAG, "current.{temperature_2m,weather_code} missing/not numeric");
        }
      } else {
        ESP_LOGW(TAG, "no \"current\" object in response");
      }
      cJSON_Delete(root);  // single exit point; no early return after Parse
    }
  }

  esp_http_client_cleanup(client);  // unconditional: nothing TLS-related stays
                                    // resident between fetches

  if (ok) {
    failures_ = 0;
    ESP_LOGI(TAG, "%.1f deg, code %d (%s)", out.temp, out.code,
             wmoText(out.code));
  } else {
    ++failures_;
    // Log loudly once an hour's worth of attempts have failed, but never reboot:
    // a wall clock that restarts because an HTTP request timed out is worse than
    // one showing a slightly stale temperature.
    if (failures_ == 12) {
      ESP_LOGE(TAG, "12 consecutive weather failures (~1 hour)");
    }
  }
  return ok;
}

// All 28 documented WMO codes. Kept short: the condition column is 164px, and
// tools/preview asserts the widest of these ("Heavy showers", 136px) fits.
const char* wmoText(int code) {
  switch (code) {
    case 0:  return "Clear";
    case 1:  return "Mainly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: return "Fog";
    case 48: return "Freezing fog";
    case 51: return "Light drizzle";
    case 53: return "Drizzle";
    case 55: return "Heavy drizzle";
    case 56:
    case 57: return "Icy drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66:
    case 67: return "Icy rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Light showers";
    case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85:
    case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: return "Storm, hail";
    case 99: return "Severe storm";
    default: return "";  // unknown or absent -> draw nothing
  }
}
