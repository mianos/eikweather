# HTTP API and settings

Base routes come from mianesp's `WebServer`: `POST /reset`, `POST /set_hostname`,
`GET /healthz`. Added here:

| Route | Purpose |
|---|---|
| `GET /config` | all settings as JSON |
| `POST /config` | apply any subset; panel tunables take effect immediately |
| `POST /config/reset` | defaults; `{"wifi":true}` also clears Wi-Fi credentials |
| `POST /refresh` | repaint now instead of waiting for the rate limit |
| `POST /test` | `{"pattern":"black"\|"white"\|"calib"\|"colors"\|"rot"}` |
| `GET /screen.pbm` | dump the framebuffer as PBM |
| `POST /firmware` | push-OTA, raw `.bin` body |
| `POST /reboot` | plain restart (needed after changing MQTT topics) |

Note mianesp's base `POST /reset` is **not** a reboot — it clears Wi-Fi credentials.

## /healthz

Reports uptime, `heap_free`, `heap_min`, display-task stack headroom, refresh count and
duration, BUSY timeouts, panel geometry and local time, plus:

- **`reset_reason` and `boot_count`.** `reset_reason` explains the current boot (`sw`
  for an OTA or `POST /reboot`; `panic`, `task_wdt`, `brownout` for the interesting
  cases). `boot_count` is persisted in NVS, so comparing it across two polls reveals a
  restart that happened while nobody was watching — a falling uptime just reads as a
  slow reply.
- **Freshness of every value**, measured on the monotonic clock: `weather_age_s`,
  `inside_age_s`, `outside_age_s`, `grid_age_s`, and the `*_fresh` booleans that decide
  whether a reading draws as `--`.
- **Trend state**: `*_trend` and `*_trend_age_s`. An `unknown` trend with an age below
  `trend_win_min * 60` means "not enough history yet", not "sensor is dead".
- `weather_status` **and** `weather_error`. Status `0` means it never reached HTTP at
  all, and `weather_error` carries the `esp_err_to_name` — which is how
  `ESP_ERR_HTTP_CONNECT` on the first post-boot fetch was identified.
- `inside_topic` / `outside_topic` / `grid_topic` and `mqtt_messages`, enough to
  diagnose a wrong topic or field name without a serial cable. `grid_seen: false` with
  a non-empty `grid_topic` means the publisher is gone, not the display.
- Grid: `grid_import_kwh` / `grid_export_kwh` (raw counters — import should rise at
  night, export in sun; if not, the fields are swapped), `grid_samples`, `grid_day` /
  `grid_day_base_kwh` (where "today" started counting), and `grid_avg_kw` /
  `grid_today_kwh`, which are **absent** exactly when the screen shows `zz`.
- `alert` and `alert_age_min`, immediately after the per-source ages that cause it.
- `cpu_max_mhz` / `cpu_min_mhz` / `light_sleep` / `wifi_ps`, read back from the
  **driver** rather than from settings, so a `light_sleep: 1` setting against a build
  that lost `CONFIG_PM_ENABLE` is visible rather than silent. `cpu_min_mhz ==
  cpu_max_mhz` means DFS is off; `wifi_ps: 0` means the Wi-Fi driver is holding a
  no-light-sleep lock and nothing ever sleeps.

## Settings

| Key | Default | Note |
|---|---|---|
| `latitude` / `longitude` | `-33.8688` / `151.2093` | **strings**, passed verbatim to Open-Meteo |
| `tz` | `AEST-10AEDT,M10.1.0,M4.1.0/3` | POSIX TZ |
| `ntp_server` | `pool.ntp.org` | |
| `temp_unit` | `celsius` | Open-Meteo's own vocabulary, passed through |
| `mqtt_server` / `mqtt_port` | `mqtt.local` / `1883` | placeholder — point it at your broker |
| `inside_topic` / `inside_field` / `inside_label` | `""` / `temperature` / `IN` | empty topic ⇒ shows `--` |
| `outside_topic` / `outside_field` / `outside_label` | `""` / `temperature` / `OUT` | labels are short on purpose |
| `grid_topic` | `""` | optional grid meter; empty ⇒ neither grid figure is drawn |
| `grid_import_field` / `grid_export_field` | `forwardEnergy` / `reverseEnergy` | the meter's cumulative kWh counters in that topic's JSON |
| `sensor_stale_min` | `30` | a reading older than this shows `--`; `0` disables |
| `trend_win_min` / `trend_tenths` | `10` / `1` | rise/fall window, and deadband in tenths of a degree |
| `alert_age_min` | `60` | red `!` once a working source goes quiet this long; `0` disables |
| `min_interval_min` | `15` | rate-limits repaints, clamped to ≥3 |
| `weather_poll_min` | `15` | a failed fetch retries after 60 s regardless |
| `boot_screen` | `1` | `0` skips the boot/status paint |
| `panel_w` / `panel_h` | `122` / `250` | fallback `128` / `296` |
| `rotation`, `invert_red` | `3`, `0` | |
| `border`, `src_mode`, `update_mode` | `0x05`, `0x80`, `0xF7` | SSD1680 init bytes |
| `enable_web` | `1` | `0` reclaims ~28 KB of heap, losing this API |
| `light_sleep` | `1` | DFS + automatic light sleep; `0` pins 160 MHz. Applies immediately |
| `sensor_name` | `einkweather` | also the DHCP hostname |

`SettingsBase` supports only `std::string` and `int` — no float, no bool. Lat/lon are
strings because their only consumer is a URL, so text goes in with no conversion and
nothing to round-trip wrong.

## NVS shadowing — the trap worth knowing

Everything persists as **one JSON blob** in NVS. Once a device has stored a value, that
value wins over any later compiled default forever. Changing a default in `Settings.h`
does not move a configured board; it needs a `POST /config`. This has caught this
project twice, with `sensor_name` and `water_label`.

## Changing MQTT topics needs a reboot

Subscriptions are established once in `Sensors::attach()` at startup, and `MqttClient`
has no way to remove a handler binding, so re-pointing a topic cannot take effect in
place. Changing one logs `MQTT settings changed — POST /reboot to apply`.
