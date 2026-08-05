#include "Epaper.h"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace epd {
namespace {

constexpr char TAG[] = "epaper";

// SSD1680 commands.
constexpr uint8_t CMD_DRIVER_OUTPUT = 0x01;
constexpr uint8_t CMD_DEEP_SLEEP = 0x10;
constexpr uint8_t CMD_DATA_ENTRY = 0x11;
constexpr uint8_t CMD_SW_RESET = 0x12;
constexpr uint8_t CMD_TEMP_SENSOR = 0x18;
constexpr uint8_t CMD_MASTER_ACTIVATE = 0x20;
constexpr uint8_t CMD_UPDATE_CTRL1 = 0x21;
constexpr uint8_t CMD_UPDATE_CTRL2 = 0x22;
constexpr uint8_t CMD_WRITE_RAM_BW = 0x24;
constexpr uint8_t CMD_WRITE_RAM_RED = 0x26;
constexpr uint8_t CMD_BORDER_WAVEFORM = 0x3C;
constexpr uint8_t CMD_RAM_X_WINDOW = 0x44;
constexpr uint8_t CMD_RAM_Y_WINDOW = 0x45;
constexpr uint8_t CMD_RAM_X_COUNTER = 0x4E;
constexpr uint8_t CMD_RAM_Y_COUNTER = 0x4F;

// DC is driven from the SPI pre-callback rather than a bare gpio_set_level before
// each transmit: this guarantees DC is correct at the instant CS asserts, and
// stays correct if anything else is ever added to the bus.
void IRAM_ATTR dcPreCb(spi_transaction_t* t) {
  const auto* tag = static_cast<const DcTag*>(t->user);
  gpio_set_level(tag->dc, tag->level);  // 0 = command, 1 = data
}

}  // namespace

Panel::Panel(const Pins& pins)
    : pins_(pins), dcCmd_{pins.dc, 0}, dcData_{pins.dc, 1} {
  clear(Color::White);
}

esp_err_t Panel::init() {
  gpio_config_t out = {};
  out.pin_bit_mask = (1ULL << pins_.dc) | (1ULL << pins_.rst);
  out.mode = GPIO_MODE_OUTPUT;
  out.pull_up_en = GPIO_PULLUP_DISABLE;
  out.pull_down_en = GPIO_PULLDOWN_DISABLE;
  out.intr_type = GPIO_INTR_DISABLE;
  esp_err_t err = gpio_config(&out);
  if (err != ESP_OK) return err;

  gpio_set_level(pins_.rst, 1);  // RST idles high
  gpio_set_level(pins_.dc, 0);

  // BUSY is actively driven by the panel — no pulls.
  gpio_config_t in = {};
  in.pin_bit_mask = 1ULL << pins_.busy;
  in.mode = GPIO_MODE_INPUT;
  in.pull_up_en = GPIO_PULLUP_DISABLE;
  in.pull_down_en = GPIO_PULLDOWN_DISABLE;
  in.intr_type = GPIO_INTR_DISABLE;
  err = gpio_config(&in);
  if (err != ESP_OK) return err;

  spi_bus_config_t bus = {};
  bus.mosi_io_num = pins_.mosi;
  bus.miso_io_num = -1;  // write-only panel; frees GPIO19
  bus.sclk_io_num = pins_.sclk;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  // One whole plane per transaction. The DMA default is 4092 and a 122x250 plane
  // is 4000, so it would fit — but the 128x296 fallback is 4736, so be explicit.
  bus.max_transfer_sz = static_cast<int>(kMaxPlaneBytes);
  err = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_initialize(SPI3_HOST) failed: %s",
             esp_err_to_name(err));
    return err;
  }

  spi_device_interface_config_t dev = {};
  // SSD1680 tolerates ~20MHz; GxEPD2 uses 4. 10 is a safe middle — drop to 4 if
  // bring-up stage 3 shows garbled output.
  dev.clock_speed_hz = 10 * 1000 * 1000;
  dev.mode = 0;  // CPOL=0 CPHA=0
  dev.spics_io_num = pins_.cs;
  dev.queue_size = 2;
  dev.pre_cb = dcPreCb;
  err = spi_bus_add_device(SPI3_HOST, &dev, &dev_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
    return err;
  }

  // Wiring smoke test: log BUSY before we touch the panel.
  ESP_LOGI(TAG,
           "init ok: SPI3 mosi=%d sclk=%d cs=%d dc=%d rst=%d busy=%d "
           "(BUSY reads %d)",
           pins_.mosi, pins_.sclk, pins_.cs, pins_.dc, pins_.rst, pins_.busy,
           gpio_get_level(pins_.busy));
  return ESP_OK;
}

void Panel::configure(int panelW, int panelH, Rotation rot, bool invertRed,
                      uint8_t border, uint8_t srcMode, uint8_t updateMode) {
  // Clamp to what the statically-sized framebuffer can hold.
  if (panelW < 1) panelW = 1;
  if (panelW > kRowBytes * 8) panelW = kRowBytes * 8;
  if (panelH < 1) panelH = 1;
  if (panelH > kMaxGateLines) panelH = kMaxGateLines;

  panelW_ = panelW;
  panelH_ = panelH;
  rot_ = rot;
  invertRed_ = invertRed;
  border_ = border;
  srcMode_ = srcMode;
  updateMode_ = updateMode;
  hibernating_ = true;  // force a full re-init on the next refresh
  clear(Color::White);

  ESP_LOGI(TAG,
           "configure: native %dx%d, logical %dx%d, rot=%u invertRed=%d "
           "border=0x%02X src=0x%02X update=0x%02X (plane %u B)",
           panelW_, panelH_, width(), height(), static_cast<unsigned>(rot_),
           invertRed_ ? 1 : 0, border_, srcMode_, updateMode_,
           static_cast<unsigned>(activePlaneBytes()));
}

size_t Panel::activePlaneBytes() const {
  return static_cast<size_t>(kRowBytes) * static_cast<size_t>(panelH_);
}

bool Panel::busyLevel() const { return gpio_get_level(pins_.busy) != 0; }

int Panel::width() const {
  return (rot_ == Rotation::Landscape90 || rot_ == Rotation::Landscape270)
             ? panelH_
             : panelW_;
}

int Panel::height() const {
  return (rot_ == Rotation::Landscape90 || rot_ == Rotation::Landscape270)
             ? panelW_
             : panelH_;
}

// Rotate here rather than on shift-out, so the framebuffer stays in native panel
// order and the SPI push is two flat contiguous transfers. Worst case is
// 250*122 = 30500 pixels at ~20 cycles each = ~3.8 ms at 160 MHz, against a
// 27000 ms refresh. This is free; do not optimise it.
bool Panel::mapPixel_(int lx, int ly, int& px, int& py) const {
  if (lx < 0 || ly < 0 || lx >= width() || ly >= height()) return false;
  switch (rot_) {
    case Rotation::Portrait0:
      px = lx;
      py = ly;
      break;
    case Rotation::Landscape90:
      px = ly;
      py = panelH_ - 1 - lx;
      break;
    case Rotation::Portrait180:
      px = panelW_ - 1 - lx;
      py = panelH_ - 1 - ly;
      break;
    case Rotation::Landscape270:
    default:
      px = panelW_ - 1 - ly;
      py = lx;
      break;
  }
  return px >= 0 && py >= 0 && px < panelW_ && py < panelH_;
}

void Panel::setPixelNative(int px, int py, Color c) {
  if (px < 0 || py < 0 || px >= panelW_ || py >= panelH_) return;
  const size_t idx = static_cast<size_t>(py) * kRowBytes + (px >> 3);
  const uint8_t mask = 0x80 >> (px & 7);

  // White: bw 1, red 0.  Black: bw 0, red 0.  Red: bw 1, red 1.
  //
  // Setting bw=1 under a red pixel is deliberate: if the red plane turns out to
  // be misconfigured, red content renders WHITE (invisible) rather than black.
  // That makes bring-up stage 6 unambiguous — "the red bar vanished" and "the red
  // bar came out black" are two different diagnoses (plane not landing vs
  // polarity inverted).
  switch (c) {
    case Color::White:
      bw_[idx] |= mask;
      red_[idx] &= static_cast<uint8_t>(~mask);
      break;
    case Color::Black:
      bw_[idx] &= static_cast<uint8_t>(~mask);
      red_[idx] &= static_cast<uint8_t>(~mask);
      break;
    case Color::Red:
      bw_[idx] |= mask;
      red_[idx] |= mask;
      break;
  }
}

void Panel::setPixel(int x, int y, Color c) {
  int px, py;
  if (!mapPixel_(x, y, px, py)) return;  // clipped
  setPixelNative(px, py, c);
}

void Panel::clear(Color c) {
  // Fill the WHOLE allocation including the off-panel padding columns (native x
  // 122..127 when panelW_ is 122), so no stale bits are ever shifted out.
  // setPixel clips on logical coordinates, so it can never reach that padding.
  const uint8_t bwFill = (c == Color::Black) ? 0x00 : 0xFF;
  const uint8_t redFill = (c == Color::Red) ? 0xFF : 0x00;
  memset(bw_, bwFill, sizeof bw_);
  memset(red_, redFill, sizeof red_);
}

esp_err_t Panel::write_(const uint8_t* d, size_t n, bool isData, bool invert) {
  if (n == 0) return ESP_OK;

  void* tagged = isData ? static_cast<void*>(&dcData_) : static_cast<void*>(&dcCmd_);

  if (!invert) {
    spi_transaction_t t = {};
    t.length = n * 8;  // BITS, not bytes — the classic first-time bug
    t.tx_buffer = d;
    t.user = tagged;
    return spi_device_transmit(dev_, &t);
  }

  // Inverted red plane: chunk through a small stack buffer so we never need a
  // second full-plane allocation.
  uint8_t chunk[256];
  size_t off = 0;
  while (off < n) {
    const size_t m = (n - off < sizeof chunk) ? (n - off) : sizeof chunk;
    for (size_t i = 0; i < m; ++i) chunk[i] = static_cast<uint8_t>(~d[off + i]);
    spi_transaction_t t = {};
    t.length = m * 8;
    t.tx_buffer = chunk;
    t.user = tagged;
    const esp_err_t err = spi_device_transmit(dev_, &t);
    if (err != ESP_OK) return err;
    off += m;
  }
  return ESP_OK;
}

esp_err_t Panel::cmd_(uint8_t c) { return write_(&c, 1, false, false); }

esp_err_t Panel::cmd_(uint8_t c, const uint8_t* d, size_t n) {
  const esp_err_t err = cmd_(c);
  if (err != ESP_OK) return err;
  return write_(d, n, true, false);
}

esp_err_t Panel::waitBusy_(const char* what, int timeoutMs) {
  const int64_t deadline = esp_timer_get_time() + int64_t{timeoutMs} * 1000;
  while (gpio_get_level(pins_.busy) != 0) {  // HIGH == busy
    if (esp_timer_get_time() > deadline) {
      ++busyTimeouts_;
      ESP_LOGE(TAG,
               "BUSY stuck high for %d ms during %s — check the FPC and the "
               "board's side power switch",
               timeoutMs, what);
      return ESP_ERR_TIMEOUT;
    }
    // MUST yield, never spin. This task runs at priority 4 pinned to core 1; a
    // 30-second busy-spin would starve IDLE1 and panic the task watchdog
    // (CONFIG_ESP_TASK_WDT_INIT defaults on and watches both idle tasks).
    // Do not "optimise" this into a tight loop.
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return ESP_OK;
}

esp_err_t Panel::hardReset_() {
  gpio_set_level(pins_.rst, 1);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(pins_.rst, 0);
  vTaskDelay(pdMS_TO_TICKS(10));  // datasheet min is microseconds; 10ms is free
  gpio_set_level(pins_.rst, 1);
  vTaskDelay(pdMS_TO_TICKS(20));
  return waitBusy_("hard reset", 5000);
}

esp_err_t Panel::initSequence_() {
  esp_err_t err = hardReset_();
  if (err != ESP_OK) return err;

  err = cmd_(CMD_SW_RESET);
  if (err != ESP_OK) return err;
  vTaskDelay(pdMS_TO_TICKS(10));
  err = waitBusy_("swreset", 5000);
  if (err != ESP_OK) return err;

  const uint16_t lastGate = static_cast<uint16_t>(panelH_ - 1);
  const uint8_t gateLo = static_cast<uint8_t>(lastGate & 0xFF);
  const uint8_t gateHi = static_cast<uint8_t>(lastGate >> 8);

  // 250 -> F9,00,00   |   296 -> 27,01,00 (matches the vendor's GxEPD2 class)
  const uint8_t driverOut[3] = {gateLo, gateHi, 0x00};
  if ((err = cmd_(CMD_DRIVER_OUTPUT, driverOut, 3)) != ESP_OK) return err;

  const uint8_t dataEntry = 0x03;  // X increment, Y increment, counter tracks X
  if ((err = cmd_(CMD_DATA_ENTRY, &dataEntry, 1)) != ESP_OK) return err;

  // RAM X window is in BYTE units: 0x00..0x0F for 16 bytes per line.
  const uint8_t xWin[2] = {0x00, kRowBytes - 1};
  if ((err = cmd_(CMD_RAM_X_WINDOW, xWin, 2)) != ESP_OK) return err;

  const uint8_t yWin[4] = {0x00, 0x00, gateLo, gateHi};
  if ((err = cmd_(CMD_RAM_Y_WINDOW, yWin, 4)) != ESP_OK) return err;

  if ((err = cmd_(CMD_BORDER_WAVEFORM, &border_, 1)) != ESP_OK) return err;

  const uint8_t temp = 0x80;  // use the internal temperature sensor
  if ((err = cmd_(CMD_TEMP_SENSOR, &temp, 1)) != ESP_OK) return err;

  const uint8_t updCtrl1[2] = {0x00, srcMode_};
  if ((err = cmd_(CMD_UPDATE_CTRL1, updCtrl1, 2)) != ESP_OK) return err;

  const uint8_t xc = 0x00;
  if ((err = cmd_(CMD_RAM_X_COUNTER, &xc, 1)) != ESP_OK) return err;
  const uint8_t yc[2] = {0x00, 0x00};
  if ((err = cmd_(CMD_RAM_Y_COUNTER, yc, 2)) != ESP_OK) return err;

  hibernating_ = false;
  return ESP_OK;
}

esp_err_t Panel::refresh() {
  const int64_t started = esp_timer_get_time();

  esp_err_t err = initSequence_();
  if (err != ESP_OK) return err;

  const size_t planeBytes = activePlaneBytes();
  const uint8_t xc = 0x00;
  const uint8_t yc[2] = {0x00, 0x00};

  // Hold the bus only across the two data pushes, NOT across the 27 s BUSY wait.
  spi_device_acquire_bus(dev_, portMAX_DELAY);
  do {
    if ((err = cmd_(CMD_WRITE_RAM_BW)) != ESP_OK) break;
    if ((err = write_(bw_, planeBytes, true, false)) != ESP_OK) break;

    // Re-home the address counters between planes: after planeBytes the counter
    // has walked off the end of the window, and without this the red plane lands
    // nowhere. Symptom if omitted: red silently does nothing.
    if ((err = cmd_(CMD_RAM_X_COUNTER, &xc, 1)) != ESP_OK) break;
    if ((err = cmd_(CMD_RAM_Y_COUNTER, yc, 2)) != ESP_OK) break;

    if ((err = cmd_(CMD_WRITE_RAM_RED)) != ESP_OK) break;
    if ((err = write_(red_, planeBytes, true, invertRed_)) != ESP_OK) break;
  } while (false);
  spi_device_release_bus(dev_);
  if (err != ESP_OK) return err;

  if ((err = cmd_(CMD_UPDATE_CTRL2, &updateMode_, 1)) != ESP_OK) return err;
  if ((err = cmd_(CMD_MASTER_ACTIVATE)) != ESP_OK) return err;  // the ~27-30 s burn

  err = waitBusy_("full refresh", kBusyTimeoutMs);
  lastRefreshUs_ = esp_timer_get_time() - started;
  if (err == ESP_OK) ++refreshCount_;
  ESP_LOGI(TAG, "refresh %s in %lld ms", err == ESP_OK ? "ok" : "FAILED",
           lastRefreshUs_ / 1000);
  return err;
}

esp_err_t Panel::hibernate() {
  const uint8_t mode = 0x01;  // deep sleep mode 1 (RAM retained)
  const esp_err_t err = cmd_(CMD_DEEP_SLEEP, &mode, 1);
  hibernating_ = true;  // forces a hard reset + init on the next refresh()
  return err;
}

void Panel::writePbm(void* ctx, void (*emit)(void*, const char*, size_t)) const {
  char hdr[64];
  const int w = width(), h = height();
  int n = snprintf(hdr, sizeof hdr, "P1\n# einkweather framebuffer\n%d %d\n", w, h);
  emit(ctx, hdr, static_cast<size_t>(n));

  // P1 is monochrome, so red renders as ink (1) alongside black — this dump is
  // for checking layout and geometry, not colour. Colour is verified on glass.
  for (int y = 0; y < h; ++y) {
    char line[512];
    int len = 0;
    for (int x = 0; x < w && len < static_cast<int>(sizeof line) - 3; ++x) {
      int px, py;
      int ink = 0;
      if (mapPixel_(x, y, px, py)) {
        const size_t idx = static_cast<size_t>(py) * kRowBytes + (px >> 3);
        const uint8_t mask = 0x80 >> (px & 7);
        const bool isRed = (red_[idx] & mask) != 0;
        const bool bwWhite = (bw_[idx] & mask) != 0;
        ink = (isRed || !bwWhite) ? 1 : 0;
      }
      line[len++] = static_cast<char>('0' + ink);
      line[len++] = ' ';
    }
    line[len++] = '\n';
    emit(ctx, line, static_cast<size_t>(len));
  }
}

}  // namespace epd
