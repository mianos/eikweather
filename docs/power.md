# Power and heat — light sleep, not deep sleep

Mains powered, so this is about **running cooler behind the panel**, not battery life.
The number that decides the design came off the live device: **111 refreshes of 24.6 s
in 19.3 h of uptime**, one every ~10.4 min, a **3.94% duty cycle**. For the other 96%
the chip did nothing but hold a Wi-Fi association and an MQTT socket, at roughly
30 mA — so the win is entirely in the idle term, and automatic light sleep collects
almost all of it.

| | idle draw | avg current | per day |
|---|---|---|---|
| no power management | ~30 mA | ~30 mA | ~730 mAh |
| **DFS + automatic light sleep** (current) | ~2–4 mA | **~3–4.5 mA** | ~80–110 mAh |
| deep sleep, waking every 10 min | ~10 µA + board floor | ~1.5–2.4 mA | ~40–60 mAh |

**These are datasheet-derived estimates, not measurements.** There is no meter on this
board, and the ESP32 (unlike the S2/S3) has no usable internal temperature sensor, so
there is no software proxy either. Only the duty cycle and the refresh duration are
measured.

## Why not deep sleep

Feasibility is not the blocker: all three MQTT topics are published `retain: true` so a
woken device gets every reading within about a second, and the panel driver is already
deep-sleep clean (`hibernate()` after every refresh, `hibernating_` starts true,
framebuffer rebuilt from scratch, no pins needing `gpio_hold`). It simply buys almost
nothing here and costs a lot:

- The 3.94% refresh duty is a floor no sleep strategy gets under, so deep sleep is only
  ~1–2 mA better than light sleep — about **0.15 W** on a mains-powered device.
- Every wake is a cold boot, so the **rise/fall marks would never appear again**:
  `updateTrend` needs `trend_win_min` of history and reports `Unknown` until it has it.
  The anchors, the drawn `ScreenModel` and the last-good `Weather` would all have to
  move to `RTC_DATA_ATTR` — cheap in bytes, but this is the kind of thing that fails
  silently.
- **The web API and OTA would be gone 96% of the time.** That needs a retained MQTT
  command topic (`{"stay_awake":600}`, picked up on the next wake) built *first*, or the
  board is unreachable by design.
- It stops being event-driven: repaint phase gets set by the wake timer rather than by
  when a reading arrives.

## What is enabled

`CONFIG_PM_ENABLE` + `CONFIG_FREERTOS_USE_TICKLESS_IDLE`, with
`esp_pm_configure(160 MHz max / 40 MHz min, light_sleep_enable)` from `app_main`, gated
on the `light_sleep` setting. `/healthz` reads the values back **from the driver**
rather than from settings: `cpu_max_mhz`, `cpu_min_mhz`, `light_sleep`, `wifi_ps`.

Four details are load-bearing:

- **`wifi_ps` must not be 0.** Under `WIFI_PS_NONE` the Wi-Fi driver holds an
  `ESP_PM_NO_LIGHT_SLEEP` lock for as long as it is associated, and
  `light_sleep_enable` silently achieves nothing. `main.cpp` sets `WIFI_PS_MIN_MODEM`
  explicitly for that reason even though it is the IDF default. (ws-voice sets `NONE` —
  it needs the latency for real-time audio.)
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

`CONFIG_PM_SLP_DISABLE_GPIO` is deliberately **off**. It saves 200–300 µA by disabling
every GPIO during sleep, which would drop panel RST low mid-refresh and reset the
controller in the middle of a 24 s burn.

## If it misbehaves

Light sleep is the one change here that could plausibly degrade the network: this board
has no 32.768 kHz crystal (`CONFIG_RTC_CLK_SRC_INT_RC`), so sleep timing comes from the
internal 150 kHz RC oscillator. IDF calibrates it against the crystal, but the residual
error means Wi-Fi wakes with more margin before each beacon, and an occasional missed
beacon is expected.

```sh
curl -X POST -d '{"light_sleep":0}' http://<host>/config
```

takes effect **immediately, with no reboot** — that is why it is a setting rather than
a compile-time choice. It pins 160 MHz and disables DFS too, restoring the
pre-power-management behaviour exactly.

Measured after enabling: `/healthz` latency 0.12–0.14 s, MQTT readings still arriving,
`busy_timeouts` 0, `heap_min` down ~860 bytes, and a 1.4 MB OTA upload took 18.2 s
against 17.3 s before — DTIM buffering costs about 5%.

## Soak

19.3 h continuous: `heap_min` fell 3,836 bytes in the first 2.7 h as the watermark
found the deep TLS handshake peak, then **80 bytes over the next 15.4 h** — two orders
of magnitude apart, with `heap_free` flat at ~150.6 KB throughout. That is watermark
sampling, not a leak; a leak shows as `heap_free` declining. Zero `busy_timeouts`
across 111 refreshes, zero weather failures across ~73 fetches.
