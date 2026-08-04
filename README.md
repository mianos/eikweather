# einkclock

Indoor/outdoor temperature and forecast on a Lonely Binary ESP32 e-ink board
(2.13" tri-colour, 250×122, SSD1680). ESP-IDF v6.0.1, target `esp32`.

Indoor and outdoor readings come from local MQTT (published by Node-RED); the
forecast comes from Open-Meteo. Wi-Fi, MQTT, persisted settings, an HTTP config
API and push-OTA come from the shared
[mianesp](https://github.com/mianos/mianesp) component library; the SSD1680
driver, the text renderer and the layout are local.

```
┌─────────────────────────────────────┐
│  IN                       21.4°     │  Font_Big 52pt · BLACK
│  OUT                       7.8°     │  RED
│═════════════════════════════════════│  RED rule
│  Partly cloudy 6/17 10%             │  Font_Cond
└─────────────────────────────────────┘
```

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
curl --data-binary @build/einkclock.bin http://einkclock.local/firmware   # OTA
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

## HTTP API

Base routes from mianesp's `WebServer`: `POST /reset`, `POST /set_hostname`,
`GET /healthz`. Added here:

| Route | Purpose |
|---|---|
| `GET /config` | all settings as JSON |
| `POST /config` | apply any subset; panel tunables take effect immediately |
| `POST /config/reset` | defaults; `{"wifi":true}` also clears credentials |
| `POST /refresh` | repaint now instead of waiting for the boundary |
| `POST /test` | `{"pattern":"black"\|"white"\|"calib"\|"colors"\|"rot"}` |
| `GET /screen.pbm` | dump the framebuffer as PBM |
| `POST /firmware` | push-OTA, raw `.bin` body |

`GET /healthz` reports uptime, `heap_free`, `heap_min`, display-task stack
headroom, refresh count and duration, BUSY timeouts, weather status/age/code, the
panel geometry and the local time.

### Settings worth knowing

| Key | Default | Note |
|---|---|---|
| `latitude` / `longitude` | `-33.8688` / `151.2093` | **strings**, passed verbatim to Open-Meteo |
| `tz` | `AEST-10AEDT,M10.1.0,M4.1.0/3` | POSIX TZ |
| `refresh_min` | `5` | clamped to a divisor of 60 |
| `render_lead_s` | `30` | ~2 s fetch + up to 25 s render (temperature-dependent); biased early |
| `panel_w`/`panel_h` | `122`/`250` | fallback `128`/`296` — see below |
| `rotation`, `invert_red` | `3`, `0` | |
| `border`, `src_mode`, `update_mode` | `0x05`, `0x80`, `0xF7` | SSD1680 init bytes |
| `enable_web` | `1` | `0` reclaims ~28 KB of heap, losing the config API |

`SettingsBase` supports only `std::string` and `int` — no float, no bool. Lat/lon
are strings because their only consumer is a URL, so text goes in with no
conversion and nothing to round-trip wrong.

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
| 9 · cadence | paints at 09:10 / 09:15 / 09:20, each completing 1-2 s before its boundary ✓ |

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

This prints measured width and vertical budgets for every string and renders five
cases (normal, widest, no-weather, stale, banner). It catches things inspection
does not — it found that a condition baseline of 119, the obvious choice from cap
height, clips the descender of "Partly cloudy" off the bottom edge.

**Keep those three files free of `esp_*` includes.**

### Fonts

Generated from stock macOS fonts via CoreText — no freetype, Homebrew or Python
dependency:

```sh
./tools/fontgen/genfonts.sh     # rewrites components/epaper/fonts/fonts.cpp
```

| Face | Font | Size | Glyphs | Data |
|---|---|---|---|---|
| `Font_Clock` | Arial Bold | 92 pt | 11 (`0`–`:`) | 3.6 KB |
| `Font_Temp` | Arial Bold | 38 pt | 13 (`-`–`9`) | 0.6 KB |
| `Font_Date` | Arial Bold | 24 pt | 95 | 2.1 KB |
| `Font_Cond` | Arial | 20 pt | 95 | 1.4 KB |

9,445 bytes total, confirmed in `.rodata` (flash), 0 in DRAM.

The clock size was chosen by measurement. Height is the binding constraint, not
width, so a condensed face is the wrong trade: Arial Narrow Bold at 88 pt measured
only 184 px for `"23:59"`, wasting 60 px of the 244 px budget. Arial Bold at 92 pt
is 235 px at the same 67 px figure height — same vertical footprint, much heavier
strokes.

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

**Alignment uses local minute-of-hour, not epoch arithmetic.**
`((now/period)+1)*period` happens to coincide with local :00/:05/… for every real
timezone, because all offsets are multiples of 15 minutes — but it breaks for a
period that doesn't divide 15 (a 4-minute period at UTC+05:45). The `tm_min` form
is correct for any period dividing 60 and is inherently DST-safe.

**Fetch then render, strictly sequential.** `Weather` has exactly one writer, so no
mutex and no torn reads; the TLS handshake's 25–45 KB is fully freed before the
panel work starts, so the two largest memory peaks never coexist on a 320 KB part;
and the fetch result is known before anything is drawn, so the stale indicator
comes from fact rather than a race.

**Never draw a wrong time.** Nothing calls `strftime` for display until
`time(nullptr) >= 1700000000`. A blank clock area beats `12:00 1 Jan 1970` burned
into e-paper for five minutes.

**A failed weather fetch never costs you the clock.** One attempt per cycle, 8 s
timeout, no retry; the previous reading is preserved and the render proceeds. The
device never reboots over a weather failure. Staleness shows as a dashed rule,
which costs zero layout space — hence no status footer.

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
  displayed minute away from the boundary.
