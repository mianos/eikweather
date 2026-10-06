# einkweather

Indoor/outdoor temperature and a forecast on a Lonely Binary ESP32 e-ink board
(2.13" tri-colour, 250×122, SSD1680). ESP-IDF v6.0.1, target `esp32`.

```
┌─────────────────────────────────────┐
│  IN     ▲                 21.4°     │  inside, BLACK, rising
│  OUT    ▼                  7.8°     │  outside, RED, falling
│═════════════════════════════════════│
│  Partly cloudy 6/17          +1.2   │  condition · today's lo/hi · grid kW (+import / -export)
│  Wed 5 Aug              rain 10%    │  date · chance of rain
└─────────────────────────────────────┘
```

Temperatures come from local MQTT; the forecast comes from Open-Meteo. Wi-Fi, MQTT,
settings, an HTTP config API and push-OTA come from
[mianesp](https://github.com/mianos/mianesp); the SSD1680 driver, text renderer and
layout are local.

**It is not a clock.** A full refresh takes 19–25 s and there is no partial refresh
on this panel, so it repaints only when the drawn content actually changes — about
once every 15 minutes in practice.

## Build and flash

Needs an SSH key on your GitHub account (`ssh -T git@github.com`); any key works, no
special access. See [docs/build.md](docs/build.md) if resolution fails.

```sh
./build.sh                                  # sets target esp32 on first run
./flash.sh                                  # PORT=/dev/cu.usbserial-XXXX to override
curl --data-binary @build/einkweather.bin http://<host>/firmware    # OTA
```

An unprovisioned board paints a prompt and waits for Espressif's EspTouch app
(ESP-Touch V2 only — no SoftAP, no captive portal).

## Configure

Everything is a runtime setting; nothing here needs a reflash.

```sh
curl -X POST http://<host>/config -d '{
  "mqtt_server":   "broker.example.lan",
  "inside_topic":  "home/temperature/lounge",  "inside_field":  "temperature",
  "outside_topic": "home/temperature/outside", "outside_field": "temperature",
  "latitude": "-33.8688", "longitude": "151.2093"}'
curl -X POST http://<host>/reboot     # required after changing MQTT topics
```

Payloads are flat JSON with a named numeric field: `{"temperature":15.3,...}`.
Publish the topics **retained** so the display gets a value the moment it subscribes.

`GET /healthz` is the diagnostic surface — per-source ages, freshness, heap, refresh
count, reset reason. `GET /config` lists every setting.

## Docs

| | |
|---|---|
| [docs/api.md](docs/api.md) | HTTP routes and the full settings table |
| [docs/hardware.md](docs/hardware.md) | pinout, panel findings, **no partial refresh on this panel**, bring-up results, what to change if a board misbehaves |
| [docs/power.md](docs/power.md) | light sleep vs deep sleep, and the soak result |
| [docs/design.md](docs/design.md) | why it repaints when it does, the trend mark, the stale-data alert, layout and fonts |

## Not done

- **Deep sleep** — evaluated and declined; see [docs/power.md](docs/power.md).
- **Battery display** from GPIO35, and the **button** (needs an external pull-up).
- **mDNS** — `einkweather.local` does not resolve; use the IP.
