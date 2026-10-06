# Design notes

## When it repaints

A full refresh is 19–25 s and there is no partial refresh on this panel (see
[hardware.md](hardware.md)), so a ticking clock would leave the panel mid-refresh a
third of the time. No display-technology argument survives that ratio.

So it repaints **only when the drawn content changes**, compared on the *formatted
strings* — exactly the question "would the screen look different?". A 0.01 °C wobble
that alters no displayed digit costs nothing. Comparing formatted output needs no
threshold tuning and automatically accounts for rounding, the `--` staleness fallback
and forecast text changes. `min_interval_min` (default 15) rate-limits repaints,
clamped to ≥3 so a bad setting cannot outpace the vendor's guidance.

**The rate limit is bypassed exactly twice per boot:** the first paint of any kind, and
the first paint that has *all* the data. The MQTT readings arrive in under a second
(retained topics) but the forecast needs DNS, TLS and usually one retry, so without
that exception the panel would sit on "forecast unavailable" for a full
`min_interval_min`. One extra refresh per boot is nothing against panel life.

**`ScreenModel::operator==` is defaulted, not hand-written.** It decides whether to
spend ~25 s of flashing on a repaint. The hand-written field-by-field version silently
omitted any newly added field — no compile error, just a field that never updated on
the glass. Two things make the defaulted version safe, and `tools/preview` asserts
both: arrays compare element-wise *including bytes past the NUL*, so it works only
because `buildModel()` fills a brand-new `ScreenModel` every pass (a reused instance
would compare unequal on stale tail bytes and cost a spurious repaint); and
`banner`/`banner2` compare by pointer, harmless because the banner path is the boot
screen and both are `nullptr` in the normal loop.

## The rise / fall triangle

A solid 23×12 triangle in the space between the label and the digits — up if rising,
down if falling. **Only a moving temperature is marked.** Steady and "don't know yet"
are both blank: the mark answers "which way is it going", and a third symbol for "it
isn't" would be one more thing to learn for the case where the number alone already
says everything.

The trend compares the current reading against the one from `trend_win_min` (default
10) minutes ago and claims a direction once the move reaches `trend_tenths` (default
**1** tenth = 0.1 °C), from a **single anchor re-set once per window** rather than a
rolling comparison — so each mark changes at most once per window.

One tenth is deliberate, and the obvious worry about it is misplaced. A twitchy mark
sounds expensive, but the readings are drawn to a tenth, so `16.4` → `16.5` already
forces a repaint on its own; the mark rides along on repaints that were happening
anyway. Conservative values (0.3 °C over 30 min) are indistinguishable from the feature
being broken — indoors that is a rare event, so both rows sit blank all afternoon while
the visible digits change.

Consequences worth knowing:

- **No mark for the first `trend_win_min` after a reboot**, so every OTA blanks both
  marks for 10 minutes. `/healthz` separates the two blank cases that look identical on
  glass: `inside_trend` `unknown` vs `steady`, and `inside_trend_age_s` below
  `trend_win_min * 60` means "not enough history yet", not "sensor is dead".
- **A gap longer than 3× the window resets to unknown** rather than comparing across
  it, since a publisher that vanished for hours would otherwise manufacture a trend
  from a meaningless difference. This also means a sensor publishing less often than
  ~2× the window can never resolve a trend.
- `--` (stale or never seen) gets no mark, so a dead sensor's last known direction
  cannot sit on the glass indefinitely.

### Why a bare triangle

`drawTrendArrow` takes **no height parameter**. The height is `w/2 + 1`, the only ratio
that holds the edges at exactly 45° — one pixel per row. Choosing height independently
of width forces the diagonals to quantise (`hw = i * half / (headH - 1)`), stepping
unevenly by 1 px then 2 px, which on a 1-bit panel reads as a ragged edge. Use an odd
`w`. A bare triangle also has no thin features left to go ragged; at ~20 px there is no
stem width that works next to 44 pt digits.

It is **centred on the digits' cap height**, not stood on their baseline — Font_Big's
caps run 31 px above the baseline, so a 12 px mark on the baseline looks like it has
slipped. Both rows share **one** column, derived from the wider of the two labels, so
the marks line up. If a long label plus a wide reading (`LOUNGE` + `-12.4`) leaves no
room the mark is dropped and the number keeps the space.

## The stale-data alert

A red `!` in the top-right corner once any source that **was** working has gone quiet
for `alert_age_min` (default 60). It answers the question e-paper is structurally bad
at: the panel holds its last image indefinitely, so `15.3°` looks the same whether it
arrived two minutes or two days ago.

**It is not a heartbeat.** A wedged or unpowered board cannot draw a warning about
itself, and the glass keeps the last picture either way. This covers MQTT, Wi-Fi and the
weather API failing underneath a display task that is still running.

### What counts

Any of `inside`, `outside`, `grid` or the **forecast** older than the threshold. The
forecast is the reason this exists: `app.current` deliberately keeps last-known-good
forever so a failed fetch never blanks the line, which means a 12-hour-old `Partly
cloudy 6/16` renders identically to a fresh one with no other way to tell. The three
readings are only *partly* covered by `sensor_stale_min`, which blanks an individual
number to `--` after 30 minutes.

A source **never** seen does not count. An empty topic is a choice, and a
configured-but-silent one already shows `--` or `forecast unavailable`, which says more
than 4 px of ink in a corner can.

**It must key off data age, never "time since last repaint."** That version
self-references: the mark appearing *is* a content change, so it triggers a repaint,
which makes the last repaint recent, which clears the mark, which is another change. It
flaps at ~25 s of flashing per flip. Keyed off data age, the worst case is two repaints
per outage — one on, one off.

### Why a corner mark and not a badge

That corner is the only space on this screen free by **construction** rather than by
luck. The readings right-align at `kTempRightX` (232) and their degree ring ends at
242, so nothing above the rule can ever reach `x=243..249` — 7 × 76 px.

Everything else that looks empty is only what a particular date, forecast or label
happened to leave. Taking the union of inked pixels across every `tools/preview` case,
the screen is 51% covered and the next-largest guaranteed-free box is 12 px wide,
narrowing as labels lengthen. The date line does have ~35 px of slack (widest date
`Wed 28 May` = 113 px, widest `rain 100%` = 90 px, 8 px gap, against 246 px), enough
for a 14 px warning triangle — but it would compete with text, and the corner competes
with nothing.

7 px sounds too narrow for a glyph until you measure one: `Font_Label` covers all of
`0x20..0x7E`, so a properly tapered `!` already exists, and its **ink is only 4 px
wide** (advance 8, inset at +2..+5) by 17 px tall. No hand-drawn rectangles needed. Its
baseline puts the top at `y=3`, exactly the cap top of the inside digits.

`tools/preview` asserts the geometry three ways. The third exists because bounding
boxes can pass while glyphs visually collide — the ring ends at 242 and the ink starts
at 243 — so it also renders the widest case with and without the mark and requires
**every added pixel to be ≥2 px from any pre-existing ink, diagonals included**.
Verified against the device's own `GET /screen.pbm`: 51 px at `x=243..246, y=3..19`,
identical to the host render.

### Testing it without waiting an hour

```sh
curl -X POST -d '{"alert_age_min":1}' http://<host>/config   # fires within a minute
curl -X POST -d '{"alert_age_min":60}' http://<host>/config  # back to normal
```

`/healthz` flips `alert` immediately; the glass follows once `min_interval_min` allows.

## Time

**Ages are measured on a monotonic clock** (`Monotonic.h`), never `time(nullptr)`.
Every stamp in `Reading` and `Weather` answers a duration question — how old is this
reading, how long since the trend anchor, has this source gone quiet — and a duration
must not be measured against a clock that can step. MQTT starts as soon as there is an
IP, before SNTP has synced, so wall-clock stamps could be taken near the epoch and then
appear decades old the instant the clock jumped: `fresh()` blanked seconds-old readings
to `--`, and `updateTrend()` hit its `age > 3 * win` guard and discarded the anchor,
owing another blank window. It was a race — retained messages land at T+3 s and SNTP
within a second or two of them — which is what made it easy to miss.

Wall clock is still used where an absolute instant is genuinely meant: the date line,
`/healthz`'s `local_time`, and gating the first weather fetch so mbedTLS can validate
the certificate's validity window.

This is correct by construction — `fresh()` and `updateTrend()` no longer reference the
wall clock at all, so there is no path by which SNTP can reach them — but it has **not
been demonstrated empirically**. Every boot since the change has won the race. To force
it: point `ntp_server` at an unroutable address (`192.0.2.1`), reboot, and check the
readings still report `*_fresh: true` with no anchor discarded.

**Never draw a wrong date.** Nothing formats the date until
`time(nullptr) >= 1700000000`; the line is simply absent until SNTP has synced. A blank
line beats `1 Jan 1970` burned into e-paper.

## Fixed-point temperatures

**Integer tenths** (`Tenths.h`), not floats. 17.3 °C is `173`. Tenths is exactly the
resolution the screen displays, so every question downstream — did the number change,
which way did it move, what do I print — is exact integer arithmetic, and the
double-to-int conversion happens once per input instead of being re-derived with
`lroundf` at each use.

This fixed a real bug rather than being tidiness: `17.3f - 17.2f` is `0.100000381f`, so
a `>= 0.1f` trend test misses genuine one-digit changes depending on which values it
lands on. It only looked correct while the threshold was 0.3, far from any boundary.

**It is not a size or speed win and does not remove floating point.** The binary grew
~500 bytes. cJSON parses every JSON number into a double and prints them back with
`sprintf("%1.15g")`, so double support is linked either way, and the ESP32 has hardware
single-precision FP regardless. Two conversions remain, both at edges where they cannot
affect a decision: `tenthsFromDegrees` at MQTT ingest, and dividing back out in
`/healthz` because JSON has no fixed-point type.

The one trap is **signs below one degree**. For `tenths = -5`, `tenths / 10` is `0` in
C++ (truncation toward zero) and `0` carries no sign, so splitting the value first
loses the minus for everything between −0.9 and −0.1 — a bug that would first appear on
a frosty morning. `formatTenths` takes the sign from the original value before
splitting. `tools/preview` asserts it along with half-away-from-zero rounding in both
directions and the range clamps; that is why these helpers live in their own esp-free
header.

## Task and network structure

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
The display task starts the client after polling the netif for an actual address; first
reading now arrives ~2 s after boot instead of ~11 s. The client is still *constructed*
in `app_main` — subscriptions queue and `MqttClient::resubscribe()` replays them on
connect — so only the connect is deferred. The check is state-based (interrogate the
netif) rather than event-based on purpose: the display task is created after
`WiFiManager`, so a one-shot `IP_EVENT_STA_GOT_IP` may already have been missed.

**A failed weather fetch never costs you the temperatures** — those come from MQTT and
are independent. One attempt per cycle with an 8 s timeout; the previous forecast is
preserved, and a failed fetch retries after 60 s rather than waiting the full
`weather_poll_min`, because the first attempt after boot reliably loses a race with the
network and returns `ESP_ERR_HTTP_CONNECT`. The device never reboots over a weather
failure.

**HTTPS via the IDF cert bundle**, not plain HTTP. mianesp's `HttpClient` has no
`crt_bundle_attach` and physically cannot do TLS, so `WeatherClient` calls
`esp_http_client` directly rather than patching a component six other projects share.

**Weather JSON is parsed with raw cJSON.** `JsonWrapper::GetField` reaches only flat
top-level keys and `temperature_2m` lives under `current`. A `GetObject()` cannot be
made safe as `JsonWrapper` stands: it owns its tree via `unique_ptr<cJSON,
cJSON_Delete>`, so a wrapper around a child the parent still owns would double-free.

## Layout — do not iterate on hardware

A hardware iteration costs 19–25 s; a host iteration costs 0.2 s. `Layout.cpp`,
`Gfx.cpp` and `fonts.cpp` therefore include **no** `esp_*` headers, so they compile
natively:

```sh
cd tools/preview && make run && open out-*.ppm
```

This prints measured width and vertical budgets for every string and renders seven
cases (normal, widest, no-mqtt, steady/no-arrow, long-label, alert, banner). A budget
failure exits non-zero. It catches what inspection does not — a condition baseline of
119, the obvious choice from cap height, clips the descender of "Partly cloudy" off the
bottom edge. It also asserts the non-graphical invariants that are easy to break
silently: the `Tenths.h` conversions, and that **every** `ScreenModel` field affects
equality.

**Add a check whenever you add an item to a shared line.** Both bottom lines and both
temperature rows carry two items, and every pairing is asserted numerically.

**Keep those three files free of `esp_*` includes.**

### Why the labels are short

They share a row with the big digits, so every pixel a label takes is stolen from the
number. Measured at `Font_Label`, `OUTSIDE` is 107 px, leaving 108 px for the
temperature — but `-12.4` needs 118 px and `100.0` needs 130 px. `IN`/`OUT` give the
number a 158 px budget, which fits everything.

### Why grid power is one unlabelled signed number

It rides right-aligned on the forecast line, above the rain chance. At any instant the
meter is either importing or exporting, never both, so one signed value covers both
directions: `+3.4` exporting, `-1.2` importing. Measured against a 246 px line at 20 pt:

| Forecast line contents | Width |
|---|---|
| `Heavy showers -9/45` (pathological worst) | 188 px |
| `Partly cloudy 6/17` (realistic worst) | 159 px |
| `-88.8` (sizing case) | 46 px |
| `+1.2kW` | 69 px |

188 + 8 + 46 = 242 fits. A `kW` unit would fit the realistic worst (159 + 8 + 69 = 236)
but not the pathological one, so it is left off. Stale or unconfigured draws **nothing**
rather than `--`: the forecast then reclaims the width, whereas the two big rows each
own a dedicated row that would look broken if blank.

`grid_div` converts the payload to kW. It is required rather than cosmetic: readings are
stored as tenths clamped to -99.9..999.9, so raw watts would pin at the clamp.

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
113 px and the widest forecast 245 px against a 246 px line, so sharing one line always
truncates one of them. 52 pt figures (38 px) leave room for only one small line; 44 pt
(32 px) buys two.

Tables must stay `const` and live in exactly **one** translation unit; including a font
header from two `.cpp` files duplicates the data into RAM.
