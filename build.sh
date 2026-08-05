#!/usr/bin/env bash
# Stable build wrapper so the command string never changes (one approval covers
# all builds). Sets the esp32 target on first run, then builds.
set -e
set -o pipefail   # without this a failing build exits 0 through the tee|tail pipe
export IDF_PATH=/Users/robertfowler/.espressif/v6.0.1/esp-idf
. "$IDF_PATH/export.sh" >/dev/null 2>&1
cd "$(dirname "$0")"   # path-relative: survives a project rename
if [ ! -f sdkconfig ] || ! grep -q 'CONFIG_IDF_TARGET="esp32"' sdkconfig 2>/dev/null; then
    idf.py set-target esp32
fi
idf.py build 2>&1 | tee /tmp/einkweather_build.log | tail -n 60
