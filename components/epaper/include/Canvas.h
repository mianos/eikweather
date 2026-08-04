#pragma once
#include <cstdint>

// Kept deliberately free of any esp_* / driver/ include so that Gfx.cpp and the
// app's Layout.cpp compile on the host. A hardware iteration on this panel costs
// ~30 s; a host iteration costs 0.2 s. See tools/preview.

namespace epd {

enum class Color : uint8_t { White = 0, Black = 1, Red = 2 };

// Minimal drawing surface. Panel implements it against the two-plane e-paper
// framebuffer; tools/preview implements it against a byte-per-pixel array.
class Canvas {
 public:
  virtual ~Canvas() = default;
  virtual int width() const = 0;
  virtual int height() const = 0;
  // Must clip: off-canvas coordinates are a silent no-op, not undefined
  // behaviour. Gfx relies on this instead of bounds-checking every glyph pixel.
  virtual void setPixel(int x, int y, Color c) = 0;
};

}  // namespace epd
