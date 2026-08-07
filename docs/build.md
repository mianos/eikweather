# Building

Toolchain: **ESP-IDF v6.0.1**, target `esp32`. `build.sh` and `flash.sh` export
`IDF_PATH` per invocation, so no shell rc setup is needed.

```sh
./build.sh                                  # sets target esp32 on first run
./flash.sh                                  # PORT=/dev/cu.usbserial-XXXX to override
curl --data-binary @build/einkweather.bin http://<host>/firmware    # OTA
```

The board's side power switch must be ON, or bring-up stops at `BUSY stuck high`.

## You need an SSH key, and it is not this repo's fault

The shared components come from [mianos/mianesp](https://github.com/mianos/mianesp),
which is **public** — but you still need an SSH key on your GitHub account
(`ssh -T git@github.com` should greet you). Any key works; no special access is needed.

`main/idf_component.yml` asks for its six components over HTTPS, but three of them
(`mqttwrapper`, `settingsbase`, `webserver`) declare their own cross-dependencies as
`git@github.com:...` *inside mianesp*, so resolution still reaches for SSH. Fixing it
properly means changing those three manifests upstream.

**If the first build dies in dependency resolution rather than in the compiler, that is
what happened — it is not your toolchain.**

## dependencies.lock is committed on purpose

The mianesp deps use `version: main`, so the lock is the only thing making builds
reproducible. It records commit SHAs plus `component_hash`, so changing a URL does not
move a pin. Move it forward deliberately:

```sh
idf.py update-dependencies
```

## Footprint

**1.34 MB** of a 1.875 MB OTA slot (29% free); 55 KB static DRAM with ~122 KB left for
heap; ~150 KB free heap at runtime.

The partition table is a custom dual-OTA layout for 4 MB (`partitions.csv`) at the
default offset. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, so a freshly OTA'd image
boots `PENDING_VERIFY` and `otaVerifyTask` marks it valid once there is an IP; an image
that never associates rolls back.

## Host-side layout preview

`Layout.cpp`, `Gfx.cpp` and `fonts.cpp` include no `esp_*` headers, so they compile
natively. A hardware iteration costs 19–25 s; this costs 0.2 s.

```sh
cd tools/preview && make run && open out-*.ppm
```

It asserts every width and vertical budget and exits non-zero on failure. See
[design.md](design.md#layout--do-not-iterate-on-hardware).
