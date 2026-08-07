# Hardware and panel

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
`Button(GPIO_NUM_34)` would compile, run, and read noise. Measured idle level is 0, so
there is no external pull-up either. Adopting the button needs a 10 kΩ to 3V3.

## Refresh timing

**18.7 s at room temperature, 24.6 s at 7.8 °C** — waveform duration rises as the
panel gets colder. Good Display advise ≥180 s between refreshes on tri-colour panels.

## Partial refresh is not available on this panel

Setting `0x22` to `0xFC` — the value that gives fast partial update on SSD1680 *mono*
panels such as GDEY029T94 — completes in 175 ms and changes nothing on the glass. The
controller releases BUSY without running a display phase because there is no partial
waveform in this panel's OTP. The newer tri-colour SKUs that do support it
(GDEY0213Z98, GDEY042Z98, GDEY0579Z93) ship one; GDEM0213C90 does not. Overriding OTP
with a custom LUT via `0x32` is the only remaining route, and is untested speculation.

A sweep of every plausible `0x22` value confirms exactly ONE real display mode in this
panel's OTP. **Do not re-run this:**

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

## If a board misbehaves

Every one of these is a `POST /config` away, with no reflash.

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

## Driver notes

**The framebuffer is sized for 128×296, not 122×250** — 9,472 bytes of `.bss` instead
of 8,000. The extra 1,472 bytes buy the ability to switch to the vendor's geometry
over HTTP. Geometry turned out to be 122×250, but the option cost almost nothing and
would have saved a reflash.

**Rotation happens in `setPixel`, not on shift-out**, so the buffer stays in native
order and the SPI push is two flat contiguous transfers. Worst case ~3.8 ms at 160 MHz
against an 18,700 ms refresh — free.

**BUSY is polled with `vTaskDelay`, never a spin.** The display task is priority 4
pinned to core 1; a 19-second busy-spin would starve `IDLE1` and panic the task
watchdog. The poll has a 45 s timeout, so a dead panel logs and moves on instead of
wedging the task forever. The interval is 50 ms rather than 20 for a power-management
reason — see [power.md](power.md).
