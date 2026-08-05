# einkweather

Indoor/outdoor temperature and forecast on a Lonely Binary ESP32 e-ink board
(2.13" tri-colour, 250×122, SSD1680). ESP-IDF v6.0.1, target `esp32`.

Indoor and outdoor readings come from local MQTT (published by Node-RED); the
forecast comes from Open-Meteo. Wi-Fi, MQTT, persisted settings, an HTTP config
API and push-OTA come from the shared
[mianesp](https://github.com/mianos/mianesp) component library; the SSD1680
driver, the text renderer and the layout are local.

```
┌─────────────────────────────────────┐
│  IN     ▲                 21.4°     │  Font_Big 44pt · BLACK · rising
│  OUT    ▼                  7.8°     │  RED · falling
│═════════════════════════════════════│  RED rule
│  Partly cloudy 6/17          48°    │  condition · low/high · water tank
│  Wed 5 Aug              rain 10%    │  date · chance of rain
└─────────────────────────────────────┘
```

The date belongs on a display that refuses to be a clock: it changes once a day,
so it costs one refresh a day. A time would cost one a minute.

`6/17` is today's forecast low/high (`temperature_2m_min` / `_max`) and `rain 10%`
is `precipitation_probability_max`. The rain figure is **labelled** and on its own
line because an unlabelled trailing `0%` on the forecast line was unreadable —
nothing said what it was a percentage of.

`48°` is the heat pump's hot water tank, from local MQTT. It is unlabelled and
integer because that line is the tightest on the screen — see below. The
triangles are a rise/fall mark on each reading, also below.

**It is not a clock, deliberately.** See below.

## Why this is not a clock, and when it repaints

The panel has **no partial refresh** and a full refresh measured at **18.7 s at
room temperature, 24.6 s at 7.8 degC** — waveform duration rises as the panel gets
colder. Good Display advise >=180 s between refreshes on tri-colour panels.

Partial refresh was tested and is **not available on this panel**: setting
`0x22` to `0xFC` (the value that gives fast partial update on SSD1680 *mono*
panels such as GDEY029T94) completes in 175 ms and changes nothing on the glass —
the controller releases BUSY without running a display phase, because there is no
partial waveform in this panel's OTP. The newer tri-colour SKUs that do support it
(GDEY0213Z98, GDEY042Z98, GDEY0579Z93) ship a partial waveform; GDEM0213C90 does
not. Overriding OTP with a custom LUT via `0x32` is the only remaining route and
is untested speculation.

A sweep of every plausible `0x22` value confirms there is exactly ONE real display
mode in this panel's OTP. Do not re-run this:

| `0x22` | Measured | Verdict |
|---|---|---|
| `0xF7` | 24,587 ms | the real full refresh (what we use) |
| `0xF4` | 24,459 ms | same waveform without the power-down bits — no faster, leaves the analog rail on |
| `0xFC` | 175 ms | no-op, nothing on the glass (confirmed visually) |
| `0xFF` | 316 ms | no-op (Mode 2 waveform slot is empty) |
| `0xC7` | 314 ms | no-op (no LUT load) |

Anything under ~400 ms means the controller powered up, found no usable waveform
and released BUSY without driving the panel. The ~20 s of that 24.5 s that looks
"wasted" after the first clean-looking pass is the red phase plus the settling
passes — cutting it short looks fine for a minute, then fades and accumulates
ghosting.

This started life as a clock and that was a mistake: advancing a minute costs
~25 s of visible flashing, so the panel would have been mid-refresh a third of
the time for no benefit. There is no display technology argument that survives
that ratio.

So it repaints **only when the drawn content actually changes**, and the
comparison is made on the *formatted strings* — exactly the question "would the
screen look different?". A 0.01 degC wobble that does not alter a displayed digit
costs nothing. `min_interval_min` (default 10) rate-limits repaints and is
clamped to >=3 so a bad setting cannot drive the glass harder than the vendor
allows.

Comparing formatted output beats a temperature-delta threshold: there is no
threshold to tune, and it automatically accounts for rounding, the staleness
fallback to `--`, and forecast text changes.

## The rise / fall triangle

Each reading gets a solid 23x12 triangle in what used to be dead space between the
label and the digits — up if rising, down if falling. **Only a moving temperature
is marked.** Steady and "don't know yet" are both simply blank, because the mark
answers "which way is it going", and a third symbol for "it isn't" would be one
more thing to learn for the case where the number alone already says everything.

The trend compares the current reading against the one from `trend_win_min`
(default 10) minutes ago, and claims a direction once the move reaches
`trend_tenths` (default **1** tenth = 0.1 degC). It comes from a **single anchor
re-set once per window**, not a rolling comparison, so each reading's mark changes
at most once per window.

**Both of those numbers started out wrong** (3 tenths over 30 min) and the feature
looked broken as a result: measured indoors, 0.3 degC per 30 min is a rare event,
so both rows sat blank all afternoon while the visible digits changed. The
reasoning behind the conservative values was also wrong — the fear was that a
twitchy mark would cost refreshes, but the readings are drawn to a tenth, so
`16.4` -> `16.5` already forces a repaint on its own. The mark rides along on
repaints that were happening anyway. Hence one tenth (the smallest change the
screen can display) over a window matching `min_interval_min` (no reason for the
mark to move slower than the screen does).

The comparison is done in **integer tenths**, not floats. `17.3f - 17.2f` is
`0.100000381f`, so a float `>= 0.1f` test misses genuine one-digit changes
depending on which values you land on. Integer tenths also ask exactly the question
the repaint logic asks — "did the number you can see change?".

Consequences worth knowing:

- **No mark for the first `trend_win_min` after a reboot** — there is genuinely no
  basis for one yet, so every OTA blanks both marks for 10 minutes. Do not read
  anything into a blank column right after a flash; that is what made this look
  broken during development. `/healthz` separates the two blank cases that are
  identical on screen: `inside_trend` `unknown` vs `steady`, and
  `inside_trend_age_s` below `trend_win_min * 60` means "not enough history yet",
  not "sensor is dead".
- **A gap longer than 3x the window resets to unknown** instead of comparing
  across it. A publisher that went away for hours, or SNTP stepping the clock at
  boot, would otherwise manufacture a trend out of a meaningless difference. This
  also means a sensor publishing less often than ~2x the window can never resolve
  a trend — seen for real on `home/temperature/outside` (~6 min between messages)
  while testing at `trend_win_min=1`.
- `--` (stale or never seen) gets no mark, so a dead sensor's last known direction
  cannot sit on the glass indefinitely.

### Why a bare triangle

The first version was a stemmed arrow and it looked crude, for two reasons worth
not repeating. The head height was chosen independently of its width, so the
diagonals had to quantise (`hw = i * half / (headH - 1)`) and stepped unevenly —
some rows widening by 1px, some by 2 — which on a 1-bit panel reads as a ragged
edge. And at ~20px there is no stem width that looks right next to 44pt digits:
thin looks spindly, thick looks blobby.

So `drawTrendArrow` takes **no height parameter**. The height is `w/2 + 1`, the
only ratio that holds the edges at exactly 45 degrees, one pixel per row. A bare
triangle then has no thin features left to go ragged. Use an odd `w`.

It is **centred on the digits' cap height**, not stood on their baseline: Font_Big's
caps run 31px above the baseline, so a 12px mark sitting on the baseline looks like
it has slipped.

Both rows share **one** column, derived from the wider of the two labels, so the
marks line up vertically. If a long custom label plus a wide reading (`LOUNGE` +
`-12.4`) leaves no room, the mark is dropped and the number keeps the space — it is
the decoration, the temperature is the point. `tools/preview` asserts the column
fits for the default labels rather than leaving it to be noticed on glass.

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
| Battery sense | 35 | ADC1_CH7, unused in v1 |

MISO is unused — the panel is write-only.

GPIO16/17 carrying DC/RST is itself proof this is not a WROVER: those are the pins
a WROVER burns on PSRAM CS/CLK.

**GPIO34 has no internal pull-up** (34–39 physically cannot), and mianesp's
`Button` calls `gpio_pullup_en()` and ignores the `ESP_ERR_INVALID_ARG` it gets
back — so `Button(GPIO_NUM_34)` would compile, run, and read noise. Measured idle
level is 0, i.e. there is no external pull-up either. Adopting the button needs a
10 kΩ to 3V3 first.

## Build and flash

Requires an SSH key with read access to `git@github.com:mianos/mianesp.git`
(`ssh -T git@github.com` should greet you). The component manager clones the
dependencies on the first build.

```sh
./build.sh                                  # sets target esp32 on first run
./flash.sh                                  # PORT=/dev/cu.usbserial-XXXX to override
curl --data-binary @build/einkweather.bin http://einkweather.local/firmware   # OTA
```

`dependencies.lock` **is committed** on purpose: the mianesp deps use
`version: main`, so the lock is the only thing making builds reproducible. Move
it forward deliberately with `idf.py update-dependencies`.

Current footprint: **1.10 MB** of a 1.875 MB OTA slot (44% free); 48 KB static
DRAM, ~132 KB left for heap; ~170 KB free heap at runtime after Wi-Fi comes up.

## Wi-Fi provisioning

mianesp's `WiFiManager` supports **ESP-Touch V2 (SmartConfig) only** — no SoftAP,
no captive portal. An unprovisioned board paints an on-screen prompt and waits for
Espressif's EspTouch app on a 2.4 GHz network. Credentials live in the Wi-Fi
driver's own `nvs.net80211` namespace, not in the settings blob.

`POST /config/reset` with `{"wifi":true}` clears them and reboots into provisioning.

## MQTT sources

Node-RED decodes Zigbee sensors off `tele/zigbridge/SENSOR` and republishes them
to **retained** topics (QoS 1), so this display gets a value the instant it
subscribes rather than waiting for the next sensor report:

| Topic | Source |
|---|---|
| `home/temperature/lounge` | Zigbee `0xD1EB` — the indoor reading |
| `home/temperature/outside` | Zigbee `0x8C4D` |
| `home/temperature/hotwater` | heat pump tank — **published by a node this project added**, see below |
| `home/temperature/office`, `.../bedroom` | also published, not used here |

### The hot water tank topic did not exist

The heat pump is a Tuya device read by a `tuya-smart-device` node in Node-RED flow
`bd780c1b6099c93d` (`nr2.mianos.com:1880`). The tank temperature is Tuya datapoint
**3** (falling back to **102**), merged into flow context by the `merge dps` node —
and it only ever went to a `ui_chart`. **Nothing published it to MQTT**, so there
was no topic for this display to subscribe to.

Two nodes were added to that flow (additive — `hp_parse` gained one extra wire,
no existing node was altered):

| Node | Does |
|---|---|
| `hp_tank_mqtt_fn` (function) | reads dp 3 / 102 off `hp_parse`, emits `{"temperature": t}` |
| `hp_tank_mqtt_out` (mqtt out) | publishes to `home/temperature/hotwater`, QoS 0, **retained** |

`retain` is the part that matters: the Tuya device pushes only on change, so
without it a display reboot would show nothing until the heat pump next moved.
With it, the tank temperature arrives within a second of connecting — measured at
16 s after a cold boot, which was DHCP and MQTT connect, not the topic.

It extracts the value exactly the way the existing `tank temp -> chart` node does,
rather than inventing a second interpretation of the datapoints. If the tank
reading ever goes blank, check that flow first — `/healthz` `water_seen: false`
with a non-empty `water_topic` means the publisher is gone, not the display.

Payloads are `{"temperature":15.3,"unit":"C","timestamp":"..."}` — flat JSON with
a named numeric field, matching the rest of this broker (`tele/ldr/lux` publishes
`{"lux":71.2682,...}`). That is also the only shape mianesp's `MqttClient` can
express, since it hands handlers a parsed `JsonWrapper`.

```sh
curl -X POST http://<host>/config -d '{
  "inside_topic":  "home/temperature/lounge",  "inside_field":  "temperature",
  "outside_topic": "home/temperature/outside", "outside_field": "temperature"}'
curl -X POST http://<host>/reboot
```

**The reboot is required.** Subscriptions are established once in
`Sensors::attach()` at startup, and `MqttClient` has no way to remove a handler
binding, so re-pointing a topic cannot take effect in place. Changing one logs
"MQTT settings changed — POST /reboot to apply". Note mianesp's base
`POST /reset` is *not* a reboot: it clears Wi-Fi credentials.

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

`GET /healthz` reports uptime, `heap_free`, `heap_min`, display-task stack
headroom, refresh count and duration, BUSY timeouts, panel geometry and local time,
plus:

- **Freshness of every value**, each measured against the device's own `time()` at
  the moment the value arrived rather than anything a server claimed:
  `weather_age_s`, `inside_age_s`, `outside_age_s`, and the `*_fresh` booleans that
  decide whether a reading draws as `--`.
- `weather_status` **and** `weather_error`. A status of `0` means it never reached
  HTTP at all, and `weather_error` carries the `esp_err_to_name` — which is how
  `ESP_ERR_HTTP_CONNECT` on the first post-boot fetch was identified.
- `inside_topic` / `outside_topic` and `mqtt_messages`, enough to diagnose a wrong
  topic or field name without a serial cable.

### Settings worth knowing

| Key | Default | Note |
|---|---|---|
| `latitude` / `longitude` | `-33.8688` / `151.2093` | **strings**, passed verbatim to Open-Meteo |
| `tz` | `AEST-10AEDT,M10.1.0,M4.1.0/3` | POSIX TZ |
| `inside_topic` / `inside_field` / `inside_label` | `""` / `temperature` / `IN` | empty topic => shows `--` |
| `outside_topic` / `outside_field` / `outside_label` | `""` / `temperature` / `OUT` | labels are short on purpose — see below |
| `mqtt_server` / `mqtt_port` | `mqtt2.mianos.com` / `1883` | |
| `sensor_stale_min` | `30` | a reading older than this shows `--`; `0` disables |
| `trend_win_min` / `trend_tenths` | `10` / `1` | rise/fall window, and deadband in tenths of a degree |
| `water_topic` / `water_field` | `""` / `temperature` | optional third reading; empty => not drawn at all |
| `water_label` | `""` | empty on purpose — a label truncates the forecast, see below |
| `min_interval_min` | `10` | rate-limits repaints, clamped to >=3 |
| `weather_poll_min` | `15` | a failed fetch retries after 60 s regardless |
| `panel_w`/`panel_h` | `122`/`250` | fallback `128`/`296` — see below |
| `rotation`, `invert_red` | `3`, `0` | |
| `border`, `src_mode`, `update_mode` | `0x05`, `0x80`, `0xF7` | SSD1680 init bytes |
| `enable_web` | `1` | `0` reclaims ~28 KB of heap, losing the config API |

`SettingsBase` supports only `std::string` and `int` — no float, no bool. Lat/lon
are strings because their only consumer is a URL, so text goes in with no
conversion and nothing to round-trip wrong.

**Why the labels are short.** They share a row with the big digits, so every pixel
a label takes is stolen from the number. Measured at `Font_Label`, `OUTSIDE` is
107px, leaving only 108px for the temperature — and `-12.4` needs 118px, `100.0`
needs 130px. `IN`/`OUT` give the number a 158px budget, which fits everything.
`tools/preview` asserts this.

**Why 44 pt and not 52.** The forecast and the date each need a full-width line:
measured, the widest date is 113 px and the widest forecast 245 px against a 246 px
line, so sharing one line means one always truncates. 44 pt figures (32 px) buy the
second line; 52 pt (38 px) did not. Temperatures are still the dominant element.

**The rate limit is bypassed exactly twice per boot:** the first paint of any kind,
and the first paint that has *all* the data. At boot the MQTT readings arrive in
under a second (retained topics) but the forecast needs DNS, TLS and usually one
retry, so without that exception the panel would sit on "forecast unavailable" for
a full `min_interval_min`. One extra refresh per boot is nothing against panel life.

## Bring-up results

All stages passed on the first flash. Recorded because several of these values
were *inferred* from GxEPD2 rather than read from a datasheet, and now are not.

| Stage | Result |
|---|---|
| 0 · chip/flash | ESP32-D0WD-V3, 4 MB, no PSRAM ✓ |
| 2 · panel responds | `BUSY reads 0` at init; cycles correctly on reset ✓ |
| 3 · paints | `refresh ok in 18671 ms` at room temp; 24600 ms at 7.8 degC |
| 4 · geometry | **122×250 confirmed** — the 128×296 fallback was not needed |
| 5 · rotation | `rotation=3` (Landscape270) correct first try ✓ |
| 6 · red plane | `invert_red=0` correct — red renders red ✓ |
| 8 · integration | weather HTTP 200 over TLS (CMN bundle is sufficient); OTA into `ota_1` ✓ |
| 9 · cadence | repaints on content change only, rate-limited; verified against the live board ✓ |

That also validates the three SSD1680 init bytes taken from GxEPD2 (`0x3C`=0x05,
`0x21` byte B=0x80, `0x22`=0xF7) and the `0x4E`/`0x4F` counter re-home between the
two plane writes. They remain runtime settings anyway, since a different panel
batch could differ.

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

A hardware iteration costs 19-25 s; a host iteration costs 0.2 s. `Layout.cpp`,
`Gfx.cpp` and `fonts.cpp` therefore include **no** `esp_*` headers, so they
compile natively:

```sh
cd tools/preview && make run && open out-*.ppm
```

This prints measured width and vertical budgets for every string and renders six
cases (normal, widest, no-weather, steady/no-arrow, long-label, banner). It catches
things inspection does not — it found that a condition baseline of 119, the obvious
choice from cap height, clips the descender of "Partly cloudy" off the bottom edge,
and it is what settled whether the tank temperature could carry a label.

**Add a check whenever you add an item to a shared line.** Both bottom lines and
both temperature rows now carry two items, and every one of those pairings is
asserted numerically here. A budget failure exits non-zero.

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

Sizes are chosen by measurement, not from metrics tables. `Font_Big` is 44 pt
because the forecast and the date each need a full-width line: the widest date is
113 px and the widest forecast 245 px against a 246 px line, so sharing one line
always truncates one of them. 52 pt figures (38 px) left room for only one small
line; 44 pt (32 px) buys two.

Tables must stay `const` and live in exactly **one** translation unit; including a
font header from two `.cpp` files duplicates the data into RAM.

## Design notes

**The framebuffer is sized for 128×296, not 122×250.** 9,472 bytes of `.bss`
instead of 8,000. The extra 1,472 bytes buy the ability to switch to the vendor's
geometry over HTTP. Geometry turned out to be 122×250, but the option cost almost
nothing and would have saved a reflash.

**Rotation happens in `setPixel`, not on shift-out**, so the buffer stays in native
order and the SPI push is two flat contiguous transfers. Worst case ~3.8 ms at
160 MHz against an 18,700 ms refresh — free.

**BUSY is polled with `vTaskDelay`, never a spin.** The display task is priority 4
pinned to core 1; a 19-second busy-spin would starve `IDLE1` and panic the task
watchdog. The poll also has a 45 s timeout so a dead panel logs and moves on
instead of wedging the task forever.

**One task, no `esp_timer`.** An `esp_timer` callback runs on the shared timer
task; blocking it for 19 s would stall every software timer in the system,
including inside lwIP and Wi-Fi.

**Fetch then render, strictly sequential.** `Weather` has exactly one writer, so no
mutex and no torn reads; the TLS handshake's 25–45 KB is fully freed before the
panel work starts, so the two largest memory peaks never coexist on a 320 KB part;
and the fetch result is known before anything is drawn, so the stale indicator
comes from fact rather than a race.

**The tank temperature is unlabelled because the line is full.** It rides
right-aligned on the forecast line, directly above the rain chance, with a small
degree ring. Measured against a 246px line at 20pt:

| Forecast line contents | Width |
|---|---|
| `Heavy showers -9/45` (pathological worst) | 188 px |
| `Partly cloudy 6/17` (realistic worst) | 159 px |
| bare `100` + degree ring | 39 px |
| `HWS 100` + degree ring | 91 px |

188 + 8 + 39 = 235 fits; 188 + 8 + 91 = 287 does not, and even the realistic
159 + 8 + 91 = 258 overflows. So a labelled tank temperature and today's lo/hi
cannot coexist on that line — one of them has to go. `water_label` is still a
setting for anyone who wants the label and will accept the forecast text
truncating; it just defaults to empty. `tools/preview` prints what a label would
cost so the trade stays visible rather than being rediscovered.

Integer degrees for the same reason, plus a tenth of a degree on a hot water tank
is not something anyone acts on. Stale or unconfigured draws **nothing** rather
than `--`: the forecast then reclaims the width, whereas the two big rows each own
a dedicated row that would look broken if blank.

**MQTT connects when there is an IP, not when `app_main` ends.** `app_main`
finishes about a second *before* DHCP hands over the lease, so calling
`esp_mqtt_client_start()` there guaranteed a failed first connect
(`getaddrinfo() returns 202`) and then made the first reading wait out esp-mqtt's
10 s reconnect backoff. The display task starts the client after polling the netif
for an actual address; measured, the first reading now arrives ~2 s after boot
instead of ~11 s. The client is still *constructed* in `app_main` — subscriptions
queue up and `MqttClient::resubscribe()` replays them on connect — so only the
connect itself is deferred. The check is state-based (interrogate the netif) rather
than event-based on purpose: the display task is created after `WiFiManager`, so a
one-shot `IP_EVENT_STA_GOT_IP` may already have been missed.

**Never draw a wrong date.** Nothing formats the date until
`time(nullptr) >= 1700000000`; the line is simply absent until SNTP has synced. A
blank line beats `1 Jan 1970` burned into e-paper. SNTP matters even with no clock
on screen: mbedTLS needs a plausible time to validate the Open-Meteo certificate.

**A failed weather fetch never costs you the temperatures.** Those come from MQTT
and are independent. One attempt per cycle with an 8 s timeout; the previous
forecast is preserved, and a FAILED fetch retries after 60 s rather than waiting
the full `weather_poll_min` — the first attempt after boot reliably loses a race
with the network and returns `ESP_ERR_HTTP_CONNECT`. The
device never reboots over a weather failure. 
**HTTPS via the IDF cert bundle**, not plain HTTP. mianesp's `HttpClient` has no
`crt_bundle_attach` and physically cannot do TLS, so `WeatherClient` calls
`esp_http_client` directly rather than patching a component six other projects
share.

**Weather JSON is parsed with raw cJSON.** `JsonWrapper::GetField` reaches only
flat top-level keys and `temperature_2m` lives under `current`. A `GetObject()`
cannot be made safe as `JsonWrapper` stands: it owns its tree via
`unique_ptr<cJSON, cJSON_Delete>`, so a wrapper around a child the parent still
owns would double-free.

## Not done

- **Deep sleep.** Structured for it: `refresh()` assumes nothing about staying
  powered, `hibernate()` already runs after every paint, and the framebuffer is
  redrawn from scratch. `Weather` would need `RTC_DATA_ATTR` to survive a wake.
- **Battery display** from GPIO35, and the **button** (needs an external pull-up).
- A **24-hour soak**: watch `heap_min` for a monotonic downward trend (the TLS/HTTP
  path is the likeliest place for a leak), any `busy_timeouts`, and drift of the
  displayed values going stale without the `--` fallback engaging.
