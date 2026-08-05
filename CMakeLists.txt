# einkweather — Lonely Binary ESP32 e-ink board: 2.13" tri-colour SSD1680 showing
# indoor and outdoor temperature from local MQTT, plus an Open-Meteo forecast.
#
# The panel has NO partial refresh and a full one costs 19-25 s, so this repaints
# only when the drawn content actually changes, rate-limited. It is deliberately
# not a clock. See README.md.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(einkweather)
