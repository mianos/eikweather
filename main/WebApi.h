#pragma once
#include "App.h"
#include "WebServer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Adds the config/diagnostic API on top of mianesp's WebServer, which itself
// provides POST /reset, POST /set_hostname and GET /healthz.
//
// The bring-up affordances here are not polish — they are what turns a
// 30-second-per-iteration hardware debug into a fast loop:
//   POST /refresh      wake the display task now instead of waiting up to 5 min
//   POST /test         draw a named test pattern (stages 3-6)
//   GET  /screen.pbm   dump the framebuffer, separating "layout is wrong" from
//                      "panel is wrong"
//   GET/POST /config   the seven panel tunables, changeable without a reflash
class WebApi : public WebServer {
 public:
  WebApi(WebContext* ctx, Settings& settings, App& app,
                 TaskHandle_t displayTask)
      : WebServer(ctx), settings_(settings), app_(app), displayTask_(displayTask) {}

  esp_err_t start() override;

 protected:
  void populate_healthz_fields(WebContext* ctx, JsonWrapper& json) override;

 private:
  static esp_err_t config_get_handler(httpd_req_t* req);
  static esp_err_t config_post_handler(httpd_req_t* req);
  static esp_err_t config_reset_post_handler(httpd_req_t* req);
  static esp_err_t refresh_post_handler(httpd_req_t* req);
  static esp_err_t test_post_handler(httpd_req_t* req);
  static esp_err_t screen_get_handler(httpd_req_t* req);
  static esp_err_t firmware_post_handler(httpd_req_t* req);
  static esp_err_t reboot_post_handler(httpd_req_t* req);

  Settings& settings_;
  App& app_;
  TaskHandle_t displayTask_;
};
