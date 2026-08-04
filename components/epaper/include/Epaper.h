#pragma once
#include <cstddef>
#include <cstdint>

#include "Canvas.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

namespace epd {

// Lonely Binary ESP32 E-Ink board, pins from the vendor's Arduino sample:
//   GxEPD2_3C<GxEPD2_290_C90c, ...> display(GxEPD2_290_C90c(5, 17, 16, 4));
//   MOSI-23  SCLK-18  CS-5  DC-17  RST-16  BUSY-4
// MISO is unused: the panel is write-only.
//
// GPIO 23/18/5 are exactly SPI3/VSPI's IOMUX pins on classic ESP32
// (SPI3_IOMUX_PIN_NUM_MOSI/CLK/CS), so SPI3_HOST gets the zero-hop fast path.
//
// Strapping-pin notes: GPIO5 (CS) emits a boot-time pulse — harmless for e-paper
// CS and standard practice. GPIO2 (the board LED, not touched here) must be low
// or floating to enter download mode; an LED to GND is exactly that.
struct Pins {
  gpio_num_t mosi = GPIO_NUM_23;
  gpio_num_t sclk = GPIO_NUM_18;
  gpio_num_t cs = GPIO_NUM_5;
  gpio_num_t dc = GPIO_NUM_17;
  gpio_num_t rst = GPIO_NUM_16;
  gpio_num_t busy = GPIO_NUM_4;
};

// GEOMETRY — the project's biggest unknown, deliberately kept runtime.
//
// The display sticker says 2.13" 250x122 (model LB213BWR01, a GDEM0213C90-class
// SSD1680 panel). But GxEPD2 has NO class for GDEM0213C90, which is why the
// vendor's own sample drives it with GxEPD2_290_C90c — the 2.9" GDEM029C90 at
// 128x296. A 296x128 RAM window over-covers a 122x250 panel, so their sample
// working does NOT prove the geometry either way.
//
// So: allocate the 128x296 worst case, keep the ACTIVE window runtime-settable.
// Costs 1472 extra bytes of .bss and buys a POST /config fallback to the vendor's
// known-good geometry with no reflash. See bring-up stage 4 in the README.
constexpr int kRowBytes = 16;  // ceil(122/8) == ceil(128/8) — same either way
constexpr int kMaxGateLines = 296;
constexpr size_t kMaxPlaneBytes = kRowBytes * kMaxGateLines;  // 4736
constexpr size_t kMaxFrameBytes = kMaxPlaneBytes * 2;         // 9472 total .bss

// Passed to the SPI pre-callback as user data so DC is set at the instant CS
// asserts. Two pre-built instances (command / data) avoid any per-transaction
// allocation or pointer trickery.
struct DcTag {
  gpio_num_t dc;
  int level;  // 0 = command, 1 = data
};

enum class Rotation : uint8_t {
  Portrait0 = 0,
  Landscape90 = 1,
  Portrait180 = 2,
  Landscape270 = 3,
};

class Panel final : public Canvas {
 public:
  explicit Panel(const Pins& pins = {});

  // SPI bus + GPIO only — no panel traffic, so this cannot block or fail on a
  // dead/unplugged panel. Safe to call early in app_main, before Wi-Fi.
  esp_err_t init();

  // Call before the first draw. Safe to call again during bring-up (framebuffer
  // contents are invalidated, so redraw before the next refresh()).
  void configure(int panelW, int panelH, Rotation rot, bool invertRed,
                 uint8_t border, uint8_t srcMode, uint8_t updateMode);

  // --- Canvas: logical coordinates, honouring rotation -------------------
  int width() const override;   // 250 in landscape
  int height() const override;  // 122 in landscape
  void setPixel(int x, int y, Color c) override;

  // --- framebuffer only, no panel traffic -------------------------------
  void clear(Color c = Color::White);

  // Native-coordinate access, bypassing rotation. For the bring-up stage 4
  // calibration pattern only — normal drawing goes through Canvas/Gfx.
  void setPixelNative(int px, int py, Color c);
  int nativeW() const { return panelW_; }
  int nativeH() const { return panelH_; }

  // --- panel traffic ----------------------------------------------------
  // Init sequence, both planes to RAM, then 0x22/0x20 and wait for BUSY.
  // BLOCKS for ~27-30 s, yielding throughout (BUSY is polled with vTaskDelay).
  // Returns ESP_ERR_TIMEOUT if BUSY never releases within kBusyTimeoutMs — a
  // disconnected FPC or the board's side power switch left OFF must not wedge
  // the caller forever.
  esp_err_t refresh();

  // 0x10 deep sleep mode 1. The next refresh() re-runs the hard reset + init,
  // because deep sleep loses the controller's configuration.
  esp_err_t hibernate();

  // --- bring-up / diagnostics -------------------------------------------
  bool busyLevel() const;           // raw BUSY pin, 1 == busy
  size_t activePlaneBytes() const;  // kRowBytes * panelH_
  int64_t lastRefreshUs() const { return lastRefreshUs_; }
  uint32_t refreshCount() const { return refreshCount_; }
  uint32_t busyTimeouts() const { return busyTimeouts_; }

  // Dumps the current framebuffer as a P1 PBM in LOGICAL orientation, so
  // GET /screen.pbm shows what the device thinks it drew. Separates "the layout
  // is wrong" from "the panel is wrong".
  void writePbm(void* ctx, void (*emit)(void*, const char*, size_t)) const;

  static constexpr int kBusyTimeoutMs = 45000;  // ~1.5x the 27-30 s spec

 private:
  esp_err_t hardReset_();
  esp_err_t cmd_(uint8_t c);
  esp_err_t cmd_(uint8_t c, const uint8_t* d, size_t n);
  esp_err_t write_(const uint8_t* d, size_t n, bool isData, bool invert);
  esp_err_t waitBusy_(const char* what, int timeoutMs = kBusyTimeoutMs);
  esp_err_t initSequence_();
  bool mapPixel_(int lx, int ly, int& px, int& py) const;

  Pins pins_;
  DcTag dcCmd_;
  DcTag dcData_;
  spi_device_handle_t dev_ = nullptr;

  int panelW_ = 122, panelH_ = 250;
  Rotation rot_ = Rotation::Landscape270;
  bool invertRed_ = false;
  // These three come from GxEPD2's SSD1680 sequences, NOT from a datasheet, and
  // are the likeliest things to be wrong on first flash — hence runtime.
  uint8_t border_ = 0x05;      // 0x3C border waveform
  uint8_t srcMode_ = 0x80;     // 0x21 byte B (source output range)
  uint8_t updateMode_ = 0xF7;  // 0x22 update sequence
  bool hibernating_ = true;    // start true so the first refresh() hard-resets

  int64_t lastRefreshUs_ = 0;
  uint32_t refreshCount_ = 0;
  uint32_t busyTimeouts_ = 0;

  // .bss, internal DMA-capable DRAM. alignas(4) is required by the SPI DMA path
  // (spi_transaction_t.tx_buffer must be 4-byte aligned).
  alignas(4) uint8_t bw_[kMaxPlaneBytes];   // bit 1 = white, 0 = black
  alignas(4) uint8_t red_[kMaxPlaneBytes];  // bit 1 = red (subject to invertRed_)
};

}  // namespace epd
