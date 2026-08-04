# Redundant under IDF 6 (which already compiles C++ as gnu++26) but documents the
# minimum the mianesp components need: SettingsBase.h uses abbreviated function
# templates and JsonWrapper.h uses static_assert(false) in a discarded constexpr
# branch, both C++20.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

idf_component_register(
    SRCS "main.cpp" "Layout.cpp" "WeatherClient.cpp" "ClockWebServer.cpp"
    INCLUDE_DIRS "."
    REQUIRES
      # mianesp (git deps — see idf_component.yml).
      # jsonwrapper brings espressif__cjson transitively, so <cJSON.h> works here
      # with no explicit dependency.
      nvsstoragemanager
      wifimanager
      settingsbase
      webserver
      jsonwrapper
      # app-local
      epaper
      # IDF
      esp_http_client
      mbedtls              # esp_crt_bundle_attach() lives here
      esp_wifi
      esp_netif            # esp_netif_sntp.h
      esp_event
      nvs_flash
      app_update           # OTA + rollback marking
      esp_timer
      esp_driver_gpio)     # LED / button probe
