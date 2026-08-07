# einkweather

Indoor/outdoor temperature and forecast on a Lonely Binary ESP32 e-ink board
(2.13" tri-colour, 250×122, SSD1680). ESP-IDF v6.0.1, target `esp32`.

Temperatures come from local MQTT (published by Node-RED); the forecast comes from
Open-Meteo. Wi-Fi, MQTT, persisted settings, an HTTP config API and push-OTA come
from the shared [mianesp](https://github.com/mianos/mianesp) component library; the
SSD1680 driver, the text renderer and the layout are local.

```
┌─────────────────────────────────────┐
│  IN     ▲                 21.4°     │  Font_Big 44pt · BLACK · rising
│  OUT    ▼                  7.8°     │  RED · falling
│═════════════════════════════════════│  RED rule
│  Partly cloudy 6/17          48°    │  condition · low/high · water tank
│  Wed 5 Aug              rain 10%    │  date · chance of rain
└─────────────────────────────────────┘
```

`6/17` is today's forecast low/high (`temperature_2m_min` / `_max`); `rain 10%` is
`precipitation_probability_max`, labelled and on its own line because an unlabelled
trailing `0%` gives no clue what it measures. `48°` is the heat pump's hot water
tank. The triangles are rise/fall marks.

The date earns its place on a display that refuses to be a clock: it changes once a
day, so it costs one refresh a day. A time would cost one a minute.

## Why this is not a clock, and when it repaints

The panel has **no partial refresh** and a full refresh measures **18.7 s at room
temperature, 24.6 s at 7.8 °C** — waveform duration rises as the panel gets colder.
Good Display advise ≥180 s between refreshes on tri-colour panels. A ticking clock
would leave the panel mid-refresh a third of the time; no display-technology
argument survives that ratio.

So it repaints **only when the drawn content changes**, compared on the *formatted
strings* — exactly the question "would the screen look different?". A 0.01 °C wobble
that alters no displayed digit costs nothing. Comparing formatted output needs no
threshold tuning and automatically accounts for rounding, the `--` staleness
fallback and forecast text changes. `min_interval_min` (default 10) rate-limits
repaints, clamped to ≥3 so a bad setting cannot outpace the vendor's guidance.

### Partial refresh is not available on this panel

Setting `0x22` to `0xFC` — the value that gives fast partial update on SSD1680
*mono* panels such as GDEY029T94 — completes in 175 ms and changes nothing on the
glass. The controller releases BUSY without running a display phase because there
is no partial waveform in this panel's OTP. The newer tri-colour SKUs that do
support it (GDEY0213Z98, GDEY042Z98, GDEY0579Z93) ship one; GDEM0213C90 does not.
Overriding OTP with a custom LUT via `0x32` is the only remaining route, and is
untested speculation.

A sweep of every plausible `0x22` value confirms exactly ONE real display mode in
this panel's OTP. **Do not re-run this:**

| `0x22` | Measured | Verdict |
|---|---|---|
| `0xF7` | 24,587 ms | the real full refresh (what we use) |
| `0xF4` | 24,459 ms | same waveform without the power-down bits — no faster, leaves the analog rail on |
| `0xFC` | 175 ms | no-op, nothing on the glass (confirmed visually) |
| `0xFF` | 316 ms | no-op (Mode 2 waveform slot is empty) |
| `0xC7` | 314 ms | no-op (no LUT load) |

Anything under ~400 ms means the controller powered up, found no usable waveform and
released BUSY without driving the panel. The ~20 s of the 24.5 s that looks "wasted"
after the first clean-looking pass is the red phase plus settling — cutting it short
looks fine for a minute, then fades and accumulates ghosting.

## The rise / fall triangle

A solid 23×12 triangle in the space between the label and the digits — up if rising,
down if falling. **Only a moving temperature is marked.** Steady and "don't know
yet" are both blank: the mark answers "which way is it going", and a third symbol
for "it isn't" would be one more thing to learn for the case where the number alone
already says everything.

The trend compares the current reading against the one from `trend_win_min`
(default 10) minutes ago and claims a direction once the move reaches `trend_tenths`
(default **1** tenth = 0.1 °C), from a **single anchor re-set once per window**
rather than a rolling comparison — so each mark changes at most once per window.

One tenth is deliberate, and the obvious worry about it is misplaced. A twitchy mark
sounds expensive, but the readings are drawn to a tenth, so `16.4` → `16.5` already
forces a repaint on its own; the mark rides along on repaints that were happening
anyway. There is no reason for it to move slower than the screen does, hence a
window matching `min_interval_min`. Conservative values (0.3 °C over 30 min) are
indistinguishable from the feature being broken — indoors, that is a rare event, so
both rows sit blank all afternoon while the visible digits change.

Temperatures are stored as **integer tenths**, so this comparison is plain integer
subtraction — see the fixed-point note below.

Consequences worth knowing:

- **No mark for the first `trend_win_min` after a reboot**, so every OTA blanks both
  marks for 10 minutes. `/healthz` separates the two blank cases that look identical
  on glass: `inside_trend` `unknown` vs `steady`, and `inside_trend_age_s` below
  `trend_win_min * 60` means "not enough history yet", not "sensor is dead".
- **A gap longer than 3× the window resets to unknown** rather than comparing across
  it, since a publisher that vanished for hours would otherwise manufacture a trend
  from a meaningless difference. This also means a sensor publishing less often than
  ~2× the window can never resolve a trend — seen on `home/temperature/outside`
  (~6 min between messages) at `trend_win_min=1`.
- `--` (stale or never seen) gets no mark, so a dead sensor's last known direction
  cannot sit on the glass indefinitely.

### Why a bare triangle

`drawTrendArrow` takes **no height parameter**. The height is `w/2 + 1`, the only
ratio that holds the edges at exactly 45° — one pixel per row. Choosing height
independently of width forces the diagonals to quantise (`hw = i * half /
(headH - 1)`), stepping unevenly by 1 px then 2 px, which on a 1-bit panel reads as
a ragged edge. Use an odd `w`.

A bare triangle also has no thin features left to go ragged. At ~20 px there is no
stem width that works next to 44 pt digits: thin looks spindly, thick looks blobby.

It is **centred on the digits' cap height**, not stood on their baseline — Font_Big's
caps run 31 px above the baseline, so a 12 px mark on the baseline looks like it has
slipped.

Both rows share **one** column, derived from the wider of the two labels, so the
marks line up vertically. If a long label plus a wide reading (`LOUNGE` + `-12.4`)
leaves no room the mark is dropped and the number keeps the space — the mark is
decoration, the temperature is the point. `tools/preview` asserts the column fits
for the default labels.

## The stale-data alert

A red `!` in the top-right corner once any source that **was** working has gone quiet
for `alert_age_min` (default 60). It answers the question e-paper is structurally bad
at: the panel holds its last image indefinitely, so `15.3°` looks the same whether it
arrived two minutes or two days ago.

```
                                             #      <- x=243..246, y=3..19
   IN     /\              1 5 . 3 °         ##         red, Font_Label '!'
                                            ##
   OUT    \/              1 2 . 1 °          #
========================================================
   Partly cloudy 6/16                              43°
   Thu 6 Aug                                 rain 0%
```

**It is not a heartbeat.** A wedged or unpowered board cannot draw a warning about
itself, and the glass keeps the last picture either way. This covers MQTT, Wi-Fi and
the weather API failing underneath a display task that is still running.

### What counts

Any of `inside`, `outside`, `water` or the **forecast** older than the threshold.
The forecast is the reason this exists: `app.current` deliberately keeps
last-known-good forever so a failed fetch never blanks the line, which means a
12-hour-old `Partly cloudy 6/16` renders identically to a fresh one with no other
way to tell. The three readings are only *partly* covered by `sensor_stale_min`,
which blanks an individual number to `--` after 30 minutes.

A source **never** seen does not count. An empty topic is a choice, and a
configured-but-silent one already shows `--` or `forecast unavailable`, which says
more than 4 px of ink in a corner can.

**It must key off data age, never "time since last repaint."** That version
self-references: the mark appearing *is* a content change, so it triggers a repaint,
which makes the last repaint recent, which clears the mark, which is another change.
It flaps at ~25 s of flashing per flip. Keyed off data age, the worst case is two
repaints per outage — one on, one off.

`ScreenModel::operator==` compares the flag, or the mark would never earn a repaint
and would only appear by riding along on some other change.

### Why a corner mark and not a badge

That corner is the only space on this screen free by **construction** rather than by
luck. The readings right-align at `kTempRightX` (232) and their degree ring ends at
242, so nothing above the rule can ever reach `x=243..249` — 7 × 76 px.

Everything else that looks empty is only what a particular date, forecast or label
happened to leave. Taking the union of inked pixels across every `tools/preview`
case, the screen is 51% covered and the next-largest guaranteed-free box is 12 px
wide, narrowing as labels lengthen. The date line does have ~35 px of slack (widest
date `Wed 28 May` = 113 px, widest `rain 100%` = 90 px, 8 px gap, against 246 px),
enough for a 14 px warning triangle — but it would compete with text, and the corner
competes with nothing.

7 px sounds too narrow for a glyph until you measure one: `Font_Label` covers all of
`0x20..0x7E`, so a properly tapered `!` already exists, and its **ink is only 4 px
wide** (advance 8, inset at +2..+5) by 17 px tall. No hand-drawn rectangles needed.
Its baseline puts the top at `y=3`, exactly the cap top of the inside digits.

`tools/preview` asserts the geometry three ways. The third exists because bounding
boxes can pass while glyphs visually collide — the ring ends at 242 and the ink
starts at 243 — so it also renders the widest case with and without the mark and
requires **every added pixel to be ≥2 px from any pre-existing ink, diagonals
included**. Verified against the device's own `GET /screen.pbm`: 51 px at
`x=243..246, y=3..19`, identical to the host render.

### Testing it without waiting an hour

```sh
curl -X POST -d '{"alert_age_min":1}' http://<host>/config   # fires within a minute
curl -X POST -d '{"alert_age_min":60}' http://<host>/config  # back to normal
```

`/healthz` flips `alert` immediately; the glass follows once `min_interval_min`
allows, so allow up to 10 minutes. `alert` and `alert_age_min` sit next to the
per-source ages, which are the answer to "why is it on".

## Hardware

Confirmed with `esptool -p /dev/cu.usbserial-1440 flash-id`: **ESP32-D0WD-V3 rev
v3.0, 4 MB flash, no PSRAM**.

| Signal | GPIO | Note |
|---|---|---|
| MOSI | 23 | SPI3/VSPI IOMUX pin → zero-hop fast path |
| SCLK | 18 | SPI3 IOMUX |
| CS | 5 | SPI3 IOMUX. Also a strapping pin (boot pulse is harmless) |
| DC | 17 | |
| RST | 16 | |
| BUSY | 4 | HIGH = busy |
| LED | 2 | lit during each refresh. Strapping pin — only driven long after boot |
| BUTTON | 34 | **no external pull-up on this board** (measured: idles LOW) |
| Battery sense | 35 | ADC1_CH7, unused |

MISO is unused — the panel is write-only. GPIO16/17 carrying DC/RST is itself proof
this is not a WROVER: those are the pins a WROVER burns on PSRAM CS/CLK.

**GPIO34 has no internal pull-up** (34–39 physically cannot), and mianesp's `Button`
calls `gpio_pullup_en()` and ignores the `ESP_ERR_INVALID_ARG` it gets back — so
`Button(GPIO_NUM_34)` would compile, run, and read noise. Measured idle level is 0,
so there is no external pull-up either. Adopting the button needs a 10 kΩ to 3V3.

## Power and heat — light sleep, not deep sleep

Mains powered, so this is about **running cooler behind the panel**, not battery
life. The number that decides the design came off the live device: **111 refreshes of
24.6 s in 19.3 h of uptime**, one every ~10.4 min, a **3.94% duty cycle**. For the
other 96% the chip did nothing but hold a Wi-Fi association and an MQTT socket, at
roughly 30 mA — so the win is entirely in the idle term, and automatic light sleep
collects almost all of it.

| | idle draw | avg current | per day |
|---|---|---|---|
| no power management | ~30 mA | ~30 mA | ~730 mAh |
| **DFS + automatic light sleep** (current) | ~2–4 mA | **~3–4.5 mA** | ~80–110 mAh |
| deep sleep, waking every 10 min | ~10 µA + board floor | ~1.5–2.4 mA | ~40–60 mAh |

**These are datasheet-derived estimates, not measurements.** There is no meter on
this board, and the ESP32 (unlike the S2/S3) has no usable internal temperature
sensor, so there is no software proxy either. Only the duty cycle and the refresh
duration are measured.

### Why not deep sleep

Feasibility is not the blocker: all three MQTT topics are published `retain: true`
so a woken device gets every reading within about a second, and the panel driver is
already deep-sleep clean (`hibernate()` after every refresh, `hibernating_` starts
true, framebuffer rebuilt from scratch, no pins needing `gpio_hold`). It simply buys
almost nothing here and costs a lot:

- The 3.94% refresh duty is a floor no sleep strategy gets under, so deep sleep is
  only ~1–2 mA better than light sleep — about **0.15 W** on a mains-powered device.
- Every wake is a cold boot, so the **rise/fall marks would never appear again**:
  `updateTrend` needs `trend_win_min` of history and reports `Unknown` until it has
  it. The anchors, the drawn `ScreenModel` and the last-good `Weather` would all have
  to move to `RTC_DATA_ATTR` — cheap in bytes, but this is the kind of thing that
  fails silently.
- **The web API and OTA would be gone 96% of the time.** That needs a retained MQTT
  command topic (`{"stay_awake":600}`, picked up on the next wake) built *first*, or
  the board is unreachable by design.
- It stops being event-driven: repaint phase gets set by the wake timer rather than
  by when a reading arrives.

### What is enabled

`CONFIG_PM_ENABLE` + `CONFIG_FREERTOS_USE_TICKLESS_IDLE`, with
`esp_pm_configure(160 MHz max / 40 MHz min, light_sleep_enable)` from `app_main`,
gated on the `light_sleep` setting. `/healthz` reads the values back **from the
driver** rather than from settings: `cpu_max_mhz`, `cpu_min_mhz`, `light_sleep`,
`wifi_ps`.

Four details are load-bearing:

- **`wifi_ps` must not be 0.** Under `WIFI_PS_NONE` the Wi-Fi driver holds an
  `ESP_PM_NO_LIGHT_SLEEP` lock for as long as it is associated, and
  `light_sleep_enable` silently achieves nothing. `main.cpp` sets `WIFI_PS_MIN_MODEM`
  explicitly for that reason even though it is the IDF default. (ws-voice sets
  `NONE` — it needs the latency for real-time audio.)
- **The panel BUSY poll is 50 ms, not 20.** FreeRTOS light-sleeps only when no task
  needs to run for `CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP` ticks, which is 3 at the
  100 Hz tick. A 20 ms delay is 2 ticks — just under — so the chip stayed fully awake
  for the whole ~25 s refresh, the longest stretch of the cycle. 50 ms is 5 ticks.
  Cost: `refresh_ms` went 24,600 → 24,636.
- **SPI is safe from mid-transfer sleep.** `spi_device_acquire_bus()` holds
  `ESP_PM_APB_FREQ_MAX`, and any held lock blocks light sleep. The driver acquires
  around both plane writes and releases *before* the 25 s BUSY wait — transfers
  protected, long wait sleeping.
- **The serial log stays readable.** `esp_pm_impl_init()` re-clocks the console UART
  onto `REF_TICK`, which DFS does not affect.

`CONFIG_PM_SLP_DISABLE_GPIO` is deliberately **off**. It saves 200–300 µA by
disabling every GPIO during sleep, which would drop panel RST low mid-refresh and
reset the controller in the middle of a 24 s burn.

### If it misbehaves

Light sleep is the one change here that could plausibly degrade the network: this
board has no 32.768 kHz crystal (`CONFIG_RTC_CLK_SRC_INT_RC`), so sleep timing comes
from the internal 150 kHz RC oscillator. IDF calibrates it against the crystal, but
the residual error means Wi-Fi wakes with more margin before each beacon, and an
occasional missed beacon is expected.

```sh
curl -X POST -d '{"light_sleep":0}' http://<host>/config
```

takes effect **immediately, with no reboot** — that is why it is a setting rather
than a compile-time choice. It pins 160 MHz and disables DFS too, restoring the
pre-power-management behaviour exactly.

Measured after enabling: `/healthz` latency 0.12–0.14 s, MQTT readings still
arriving, `busy_timeouts` 0, `heap_min` down ~860 bytes, and a 1.4 MB OTA upload took
18.2 s against 17.3 s before — DTIM buffering costs about 5%.

### Soak

19.3 h continuous: `heap_min` fell 3,836 bytes in the first 2.7 h as the watermark
found the deep TLS handshake peak, then **80 bytes over the next 15.4 h** — two
orders of magnitude apart, and `heap_free` flat at ~150.6 KB throughout. That is
watermark sampling, not a leak; a leak shows as `heap_free` declining. Zero
`busy_timeouts` across 111 refreshes, zero weather failures across ~73 fetches.

## Build and flash

The shared components come from [mianos/mianesp](https://github.com/mianos/mianesp),
which is public — but **you still need an SSH key on your GitHub account**
(`ssh -T git@github.com` should greet you). Any key works; no special access is
needed. The reason is not this repo: `main/idf_component.yml` asks for its six
components over HTTPS, but three of them (`mqttwrapper`, `settingsbase`,
`webserver`) declare their own cross-dependencies as `git@github.com:...` *inside
mianesp*, so resolution still reaches for SSH. Fixing that properly means changing
those three manifests upstream.

If the first build dies in dependency resolution rather than in the compiler, that is
what happened — it is not your toolchain.

```sh
./build.sh                                  # sets target esp32 on first run
./flash.sh                                  # PORT=/dev/cu.usbserial-XXXX to override
curl --data-binary @build/einkweather.bin http://einkweather.local/firmware   # OTA
```

`dependencies.lock` **is committed** on purpose: the mianesp deps use `version:
main`, so the lock is the only thing making builds reproducible. Move it forward
deliberately with `idf.py update-dependencies`.

Footprint: **1.34 MB** of a 1.875 MB OTA slot (29% free); 55 KB static DRAM with
~122 KB left for heap; ~150 KB free heap at runtime.

## Wi-Fi provisioning

mianesp's `WiFiManager` supports **ESP-Touch V2 (SmartConfig) only** — no SoftAP, no
captive portal. An unprovisioned board paints an on-screen prompt and waits for
Espressif's EspTouch app on a 2.4 GHz network. Credentials live in the Wi-Fi driver's
own `nvs.net80211` namespace, not in the settings blob.

`POST /config/reset` with `{"wifi":true}` clears them and reboots into provisioning.

## MQTT sources

Node-RED decodes Zigbee sensors off `tele/zigbridge/SENSOR` and republishes them to
**retained** topics (QoS 1), so this display gets a value the instant it subscribes
rather than waiting for the next sensor report. Measured on a real boot: all three
land ~3 s after power-on, which is DHCP plus MQTT connect.

| Topic | Source |
|---|---|
| `home/temperature/lounge` | Zigbee — the indoor reading |
| `home/temperature/outside` | Zigbee |
| `home/temperature/hotwater` | heat pump tank — **published by a node this project added**, see below |

Payloads are flat JSON with a named numeric field —
`{"temperature":15.3,"unit":"C",...}` — matching the rest of this broker. That is
also the only shape mianesp's `MqttClient` can express, since it hands handlers a
parsed `JsonWrapper`.

```sh
curl -X POST http://<host>/config -d '{
  "mqtt_server":   "broker.example.lan",
  "inside_topic":  "home/temperature/lounge",  "inside_field":  "temperature",
  "outside_topic": "home/temperature/outside", "outside_field": "temperature"}'
curl -X POST http://<host>/reboot
```

**The reboot is required.** Subscriptions are established once in
`Sensors::attach()` at startup and `MqttClient` has no way to remove a handler
binding, so re-pointing a topic cannot take effect in place. Changing one logs
"MQTT settings changed — POST /reboot to apply". Note mianesp's base `POST /reset` is
*not* a reboot: it clears Wi-Fi credentials.

### The hot water tank topic did not exist

The heat pump is a Tuya device read by a `tuya-smart-device` node in a Node-RED flow
on the local automation host. The tank temperature is Tuya datapoint **3** (falling
back to **102**), merged into flow context by the `merge dps` node — and it only ever
went to a `ui_chart`. **Nothing published it to MQTT.**

Two nodes were added to that flow, additively — `hp_parse` gained one extra wire and
no existing node was altered:

| Node | Does |
|---|---|
| `hp_tank_mqtt_fn` (function) | reads dp 3 / 102 off `hp_parse`, emits `{"temperature": t}` |
| `hp_tank_mqtt_out` (mqtt out) | publishes to `home/temperature/hotwater`, QoS 0, **retained** |

`retain` is the part that matters: the Tuya device pushes only on change, so without
it a display reboot would show nothing until the heat pump next moved.

The function extracts the value exactly the way the existing `tank temp -> chart`
node does, rather than inventing a second interpretation of the datapoints. If the
tank reading goes blank, check that flow first — `/healthz` `water_seen: false` with a
non-empty `water_topic` means the publisher is gone, not the display.

## HTTP API

Base routes from mianesp's `WebServer`: `POST /reset`, `POST /set_hostname`,
`GET /healthz`. Added here:

| Route | Purpose |
|---|---|
| `GET /config` | all settings as JSON |
| `POST /config` | apply any subset; panel tunables take effect immediately |
| `POST /config/reset` | defaults; `{"wifi":true}` also clears credentials |
| `POST /refresh` | repaint now instead of waiting for the rate limit |
| `POST /test` | `{"pattern":"black"\|"white"\|"calib"\|"colors"\|"rot"}` |
| `GET /screen.pbm` | dump the framebuffer as PBM |
| `POST /firmware` | push-OTA, raw `.bin` body |
| `POST /reboot` | plain restart (needed after changing MQTT topics) |

`GET /healthz` reports uptime, `heap_free`, `heap_min`, display-task stack headroom,
refresh count and duration, BUSY timeouts, panel geometry and local time, plus:

- **`reset_reason` and `boot_count`.** `reset_reason` explains the current boot
  (`sw` for an OTA or `POST /reboot`; `panic`, `task_wdt`, `brownout` for the
  interesting cases). `boot_count` is persisted in NVS, so comparing it across two
  polls reveals a restart that happened while nobody was watching — a falling uptime
  reads as a slow reply.
- **Freshness of every value**, measured on the monotonic clock: `weather_age_s`,
  `inside_age_s`, `outside_age_s`, `water_age_s`, and the `*_fresh` booleans that
  decide whether a reading draws as `--`.
- `weather_status` **and** `weather_error`. Status `0` means it never reached HTTP at
  all, and `weather_error` carries the `esp_err_to_name` — which is how
  `ESP_ERR_HTTP_CONNECT` on the first post-boot fetch was identified.
- `inside_topic` / `outside_topic` and `mqtt_messages`, enough to diagnose a wrong
  topic or field name without a serial cable.
- `alert` and `alert_age_min`, immediately after the per-source ages that cause it.
- `cpu_max_mhz` / `cpu_min_mhz` / `light_sleep` / `wifi_ps`, read back from the
  **driver** rather than from settings, so a `light_sleep: 1` setting against a build
  that lost `CONFIG_PM_ENABLE` is visible rather than silent.
  `cpu_min_mhz == cpu_max_mhz` means DFS is off; `wifi_ps: 0` means the Wi-Fi driver
  is holding a no-light-sleep lock and nothing ever sleeps.

### Settings

| Key | Default | Note |
|---|---|---|
| `latitude` / `longitude` | `-33.8688` / `151.2093` | **strings**, passed verbatim to Open-Meteo |
| `tz` | `AEST-10AEDT,M10.1.0,M4.1.0/3` | POSIX TZ |
| `mqtt_server` / `mqtt_port` | `mqtt.local` / `1883` | placeholder — point it at your broker |
| `inside_topic` / `inside_field` / `inside_label` | `""` / `temperature` / `IN` | empty topic ⇒ shows `--` |
| `outside_topic` / `outside_field` / `outside_label` | `""` / `temperature` / `OUT` | labels are short on purpose — see below |
| `water_topic` / `water_field` | `""` / `temperature` | optional third reading; empty ⇒ not drawn at all |
| `water_label` | `""` | empty on purpose — a label truncates the forecast, see below |
| `sensor_stale_min` | `30` | a reading older than this shows `--`; `0` disables |
| `trend_win_min` / `trend_tenths` | `10` / `1` | rise/fall window, and deadband in tenths of a degree |
| `alert_age_min` | `60` | red `!` once a working source goes quiet this long; `0` disables |
| `min_interval_min` | `10` | rate-limits repaints, clamped to ≥3 |
| `weather_poll_min` | `15` | a failed fetch retries after 60 s regardless |
| `panel_w` / `panel_h` | `122` / `250` | fallback `128` / `296` — see below |
| `rotation`, `invert_red` | `3`, `0` | |
| `border`, `src_mode`, `update_mode` | `0x05`, `0x80`, `0xF7` | SSD1680 init bytes |
| `enable_web` | `1` | `0` reclaims ~28 KB of heap, losing the config API |
| `light_sleep` | `1` | DFS + automatic light sleep; `0` pins 160 MHz. Applies immediately |

`SettingsBase` supports only `std::string` and `int` — no float, no bool. Lat/lon are
strings because their only consumer is a URL, so text goes in with no conversion and
nothing to round-trip wrong.

**Everything persists as one JSON blob in NVS**, which has a consequence worth
knowing: once a device has stored a value, that value wins over any later compiled
default forever. Changing a default in `Settings.h` does not move a configured
board — it needs a `POST /config`.

**Why the labels are short.** They share a row with the big digits, so every pixel a
label takes is stolen from the number. Measured at `Font_Label`, `OUTSIDE` is 107 px,
leaving 108 px for the temperature — but `-12.4` needs 118 px and `100.0` needs
130 px. `IN`/`OUT` give the number a 158 px budget, which fits everything.

**The rate limit is bypassed exactly twice per boot:** the first paint of any kind,
and the first paint that has *all* the data. The MQTT readings arrive in under a
second (retained topics) but the forecast needs DNS, TLS and usually one retry, so
without that exception the panel would sit on "forecast unavailable" for a full
`min_interval_min`. One extra refresh per boot is nothing against panel life.

## Bring-up results

All stages passed on the first flash. Recorded because several of these values were
*inferred* from GxEPD2 rather than read from a datasheet, and now are not.

| Stage | Result |
|---|---|
| 0 · chip/flash | ESP32-D0WD-V3, 4 MB, no PSRAM ✓ |
| 2 · panel responds | `BUSY reads 0` at init; cycles correctly on reset ✓ |
| 3 · paints | `refresh ok in 18671 ms` at room temp; 24600 ms at 7.8 °C |
| 4 · geometry | **122×250 confirmed** — the 128×296 fallback was not needed |
| 5 · rotation | `rotation=3` (Landscape270) correct first try ✓ |
| 6 · red plane | `invert_red=0` correct — red renders red ✓ |
| 8 · integration | weather HTTP 200 over TLS (CMN bundle is sufficient); OTA into `ota_1` ✓ |
| 9 · cadence | repaints on content change only, rate-limited ✓ |

That validates the three SSD1680 init bytes taken from GxEPD2 (`0x3C`=0x05, `0x21`
byte B=0x80, `0x22`=0xF7) and the `0x4E`/`0x4F` counter re-home between the two plane
writes. They remain runtime settings, since a different panel batch could differ.

### If a future board misbehaves

| Symptom | Fix |
|---|---|
| ~6 px unpainted along one edge | `{"panel_w":128}` |
| image repeats or squashes vertically | `{"panel_h":296}` |
| shifted horizontally by exactly 8 px | `{"src_mode":0}` |
| stray white/grey frame | `{"border":1}` or `3` |
| nothing paints but BUSY cycles | `{"update_mode":244}` (0xF4) |
| red comes out black | `{"invert_red":1}` |
| red invisible | the `0x26` plane isn't landing — check the `0x4E`/`0x4F` re-home |
| garbled output | drop `clock_speed_hz` in `Epaper.cpp` to 4 MHz |
| `BUSY stuck high` in the log | FPC seating, or **the side power switch is off** |

`{"panel_w":128,"panel_h":296}` reproduces the vendor's own `GxEPD2_290_C90c`
configuration exactly, and is the guaranteed-working fallback.

## Layout development — do not iterate on hardware

A hardware iteration costs 19–25 s; a host iteration costs 0.2 s. `Layout.cpp`,
`Gfx.cpp` and `fonts.cpp` therefore include **no** `esp_*` headers, so they compile
natively:

```sh
cd tools/preview && make run && open out-*.ppm
```

This prints measured width and vertical budgets for every string and renders seven
cases (normal, widest, no-mqtt, steady/no-arrow, long-label, alert, banner). A budget
failure exits non-zero. It catches what inspection does not — a condition baseline of
119, the obvious choice from cap height, clips the descender of "Partly cloudy" off
the bottom edge.

It also asserts the non-graphical invariants that are easy to break silently:
`Tenths.h` conversions, and that **every** `ScreenModel` field affects equality (see
below).

**Add a check whenever you add an item to a shared line.** Both bottom lines and both
temperature rows carry two items, and every pairing is asserted numerically.

**Keep those three files free of `esp_*` includes.**

### Fonts

Generated from stock macOS fonts via CoreText — no freetype, Homebrew or Python
dependency:

```sh
./tools/fontgen/genfonts.sh     # rewrites components/epaper/fonts/fonts.cpp
```

| Face | Font | Size | Glyphs | Data |
|---|---|---|---|---|
| `Font_Big` | Arial Bold | 44 pt | 13 (`-`–`9`) | 0.9 KB |
| `Font_Label` | Arial Bold | 24 pt | 95 | 2.1 KB |
| `Font_Cond` | Arial | 20 pt | 95 | 1.4 KB |

9,445 bytes total, confirmed in `.rodata` (flash), 0 in DRAM.

Sizes come from measurement, not metrics tables. **`Font_Big` is 44 pt and not 52**
because the forecast and the date each need a full-width line: the widest date is
113 px and the widest forecast 245 px against a 246 px line, so sharing one line
always truncates one of them. 52 pt figures (38 px) leave room for only one small
line; 44 pt (32 px) buys two.

Tables must stay `const` and live in exactly **one** translation unit; including a
font header from two `.cpp` files duplicates the data into RAM.

## Design notes

**Ages are measured on a monotonic clock** (`Monotonic.h`), never `time(nullptr)`.
Every stamp in `Reading` and `Weather` answers a duration question — how old is this
reading, how long since the trend anchor, has this source gone quiet — and a duration
must not be measured against a clock that can step. MQTT starts as soon as there is
an IP, before SNTP has synced, so wall-clock stamps could be taken near the epoch and
then appear decades old the instant the clock jumped: `fresh()` blanked seconds-old
readings to `--`, and `updateTrend()` hit its `age > 3 * win` guard and discarded the
anchor, owing another blank window. It was a race — retained messages land at T+3 s
and SNTP within a second or two of them — which is what made it easy to miss.

Wall clock is still used where an absolute instant is genuinely meant: the date line,
`/healthz`'s `local_time`, and gating the first weather fetch so mbedTLS can validate
the certificate's validity window.

**`ScreenModel::operator==` is defaulted, not hand-written.** It decides whether to
spend ~25 s of flashing on a repaint. The hand-written field-by-field version silently
omitted any newly added field — no compile error, just a field that never updated on
the glass. Two things make the defaulted version safe, and `tools/preview` asserts
both: arrays compare element-wise *including bytes past the NUL*, so it works only
because `buildModel()` fills a brand-new `ScreenModel` every pass (a reused instance
would compare unequal on stale tail bytes and cost a spurious repaint); and
`banner`/`banner2` compare by pointer, which is harmless because the banner path is
the boot screen and both are `nullptr` in the normal loop.

**Temperatures are fixed-point integer tenths** (`Tenths.h`), not floats. 17.3 °C is
`173`. Tenths is exactly the resolution the screen displays, so every question
downstream — did the number change, which way did it move, what do I print — is exact
integer arithmetic, and the double-to-int conversion happens once per input instead of
being re-derived with `lroundf` at each use.

This fixed a real bug rather than being tidiness: `17.3f - 17.2f` is `0.100000381f`,
so a `>= 0.1f` trend test misses genuine one-digit changes depending on which values
it lands on. It only looked correct while the threshold was 0.3, far from any
boundary.

**It is not a size or speed win and does not remove floating point.** The binary grew
~500 bytes. cJSON parses every JSON number into a double and prints them back with
`sprintf("%1.15g")`, so double support is linked either way, and the ESP32 has
hardware single-precision FP regardless. Two conversions remain, both at edges where
they cannot affect a decision: `tenthsFromDegrees` at MQTT ingest, and dividing back
out in `/healthz` because JSON has no fixed-point type.

The one trap is **signs below one degree**. For `tenths = -5`, `tenths / 10` is `0`
in C++ (truncation toward zero) and `0` carries no sign, so splitting the value first
loses the minus for everything between −0.9 and −0.1 — a bug that would first appear
on a frosty morning. `formatTenths` takes the sign from the original value before
splitting. `tools/preview` asserts it along with half-away-from-zero rounding in both
directions and the range clamps; that is why these helpers live in their own esp-free
header.

**The framebuffer is sized for 128×296, not 122×250** — 9,472 bytes of `.bss` instead
of 8,000. The extra 1,472 bytes buy the ability to switch to the vendor's geometry
over HTTP. Geometry turned out to be 122×250, but the option cost almost nothing and
would have saved a reflash.

**Rotation happens in `setPixel`, not on shift-out**, so the buffer stays in native
order and the SPI push is two flat contiguous transfers. Worst case ~3.8 ms at
160 MHz against an 18,700 ms refresh — free.

**BUSY is polled with `vTaskDelay`, never a spin.** The display task is priority 4
pinned to core 1; a 19-second busy-spin would starve `IDLE1` and panic the task
watchdog. The poll also has a 45 s timeout, so a dead panel logs and moves on instead
of wedging the task forever.

**One task, no `esp_timer`.** An `esp_timer` callback runs on the shared timer task;
blocking it for 19 s would stall every software timer in the system, including inside
lwIP and Wi-Fi.

**Fetch then render, strictly sequential.** `Weather` has exactly one writer, so no
mutex and no torn reads; the TLS handshake's 25–45 KB is fully freed before the panel
work starts, so the two largest memory peaks never coexist on a 320 KB part; and the
fetch result is known before anything is drawn, so the stale indicator comes from fact
rather than a race.

**MQTT connects when there is an IP, not when `app_main` ends.** `app_main` finishes
about a second *before* DHCP hands over the lease, so calling
`esp_mqtt_client_start()` there guaranteed a failed first connect (`getaddrinfo()
returns 202`) and made the first reading wait out esp-mqtt's 10 s reconnect backoff.
The display task starts the client after polling the netif for an actual address;
first reading now arrives ~2 s after boot instead of ~11 s. The client is still
*constructed* in `app_main` — subscriptions queue and `MqttClient::resubscribe()`
replays them on connect — so only the connect is deferred. The check is state-based
(interrogate the netif) rather than event-based on purpose: the display task is
created after `WiFiManager`, so a one-shot `IP_EVENT_STA_GOT_IP` may already have been
missed.

**Never draw a wrong date.** Nothing formats the date until `time(nullptr) >=
1700000000`; the line is simply absent until SNTP has synced. A blank line beats
`1 Jan 1970` burned into e-paper.

**A failed weather fetch never costs you the temperatures** — those come from MQTT and
are independent. One attempt per cycle with an 8 s timeout; the previous forecast is
preserved, and a failed fetch retries after 60 s rather than waiting the full
`weather_poll_min`, because the first attempt after boot reliably loses a race with
the network and returns `ESP_ERR_HTTP_CONNECT`. The device never reboots over a
weather failure.

**HTTPS via the IDF cert bundle**, not plain HTTP. mianesp's `HttpClient` has no
`crt_bundle_attach` and physically cannot do TLS, so `WeatherClient` calls
`esp_http_client` directly rather than patching a component six other projects share.

**Weather JSON is parsed with raw cJSON.** `JsonWrapper::GetField` reaches only flat
top-level keys and `temperature_2m` lives under `current`. A `GetObject()` cannot be
made safe as `JsonWrapper` stands: it owns its tree via `unique_ptr<cJSON,
cJSON_Delete>`, so a wrapper around a child the parent still owns would double-free.

**The tank temperature is unlabelled because the line is full.** It rides
right-aligned on the forecast line, above the rain chance, with a small degree ring.
Measured against a 246 px line at 20 pt:

| Forecast line contents | Width |
|---|---|
| `Heavy showers -9/45` (pathological worst) | 188 px |
| `Partly cloudy 6/17` (realistic worst) | 159 px |
| bare `100` + degree ring | 39 px |
| `HWS 100` + degree ring | 91 px |

188 + 8 + 39 = 235 fits; 188 + 8 + 91 = 287 does not, and even the realistic
159 + 8 + 91 = 258 overflows. So a labelled tank temperature and today's lo/hi cannot
coexist on that line. `water_label` remains a setting for anyone who wants the label
and will accept the forecast truncating; `tools/preview` prints what it would cost so
the trade stays visible.

Integer degrees for the same reason, plus a tenth of a degree on a hot water tank is
not something anyone acts on. Stale or unconfigured draws **nothing** rather than
`--`: the forecast then reclaims the width, whereas the two big rows each own a
dedicated row that would look broken if blank.

## Not done

- **Deep sleep** — evaluated and declined; automatic light sleep is enabled instead.
  Still structured for it if that changes, but it would save only ~1–2 mA while
  costing OTA, the web API and the rise/fall marks. See
  [Power and heat](#power-and-heat--light-sleep-not-deep-sleep).
- **Battery display** from GPIO35, and the **button** (needs an external pull-up).
- **Proving the monotonic-clock fix empirically.** It is correct by construction —
  `fresh()` and `updateTrend()` no longer reference the wall clock at all — but every
  boot since has won the SNTP race, so the failure condition has not been reproduced
  post-fix. Forcing it means pointing `ntp_server` at an unroutable address
  (`192.0.2.1`) and watching a boot.
- **mDNS.** `einkweather.local` does not resolve; use the IP.
