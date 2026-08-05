#!/usr/bin/env bash
# Flash + monitor. PORT overridable: PORT=/dev/cu.usbserial-XXXX ./flash.sh
# The board's side power switch must be ON.
set -e
set -o pipefail
export IDF_PATH=/Users/robertfowler/.espressif/v6.0.1/esp-idf
. "$IDF_PATH/export.sh" >/dev/null 2>&1
cd "$(dirname "$0")"   # path-relative: survives a project rename
idf.py -p "${PORT:-/dev/cu.usbserial-1440}" flash monitor
