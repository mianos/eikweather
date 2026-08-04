# einkclock — Lonely Binary ESP32 e-ink board: 2.13" tri-colour SSD1680 wall clock
# with Open-Meteo current conditions. Repaints on the wall-clock 5-minute boundary;
# mains powered, no deep sleep.
#
# The panel has NO partial refresh and a ~27-30 s full refresh, which is why the
# cadence is 5 minutes rather than per-minute. See README.md.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(einkclock)
