#pragma once
#include <cstdint>

#include "Canvas.h"

// Text and shape rendering over a Canvas. No esp_* includes — host-compilable.

namespace epd {

// Verbatim Adafruit_GFX layout so existing GFXfont headers and fontconvert
// output drop in unmodified.
//
// The bitmap is a packed 1-bit stream, MSB first, and rows are NOT byte-padded:
// a glyph's bits run contiguously for width*height bits from
// bitmap[bitmapOffset]. Misreading this as row-aligned is the classic bug.
struct GFXglyph {
  uint16_t bitmapOffset;
  uint8_t width, height;
  uint8_t xAdvance;
  int8_t xOffset, yOffset;  // yOffset is from the BASELINE, and is negative
};

struct GFXfont {
  const uint8_t* bitmap;
  const GFXglyph* glyph;
  uint16_t first, last;  // inclusive codepoint range
  uint8_t yAdvance;      // line height
};

struct TextMetrics {
  int advance;               // total cursor advance — use this for layout
  int inkLeft, inkRight;     // ink bounding box relative to the start x
  int inkTop, inkBottom;     // relative to the baseline; inkTop is negative
};

// Characters outside [first, last] are skipped entirely (no advance, no ink).
TextMetrics measureText(const GFXfont& f, const char* s);

// baselineY is the text BASELINE, not the top of the glyph box.
void drawText(Canvas&, const GFXfont&, int x, int baselineY, const char* s, Color);
void drawTextRight(Canvas&, const GFXfont&, int rightX, int baselineY, const char* s, Color);
void drawTextCentered(Canvas&, const GFXfont&, int cx, int baselineY, const char* s, Color);

// Draws s, truncating with a trailing ".." if it would exceed maxW.
// Returns the advance actually used.
int drawTextClipped(Canvas&, const GFXfont&, int x, int baselineY, int maxW,
                    const char* s, Color);

// x of the nth occurrence of `ch` (0-based), relative to a string drawn at x=0.
// Used to repaint the ':' of "HH:MM" in red as a second pass. Returns -1 if the
// character does not occur that many times.
int glyphOffsetOf(const GFXfont& f, const char* s, char ch, int nth = 0);

// --- shapes -------------------------------------------------------------
void fillRect(Canvas&, int x, int y, int w, int h, Color);
void drawRect(Canvas&, int x, int y, int w, int h, Color);  // 1px outline
void drawHLine(Canvas&, int x, int y, int w, Color);
void drawVLine(Canvas&, int x, int y, int h, Color);
void drawDashedHLine(Canvas&, int x, int y, int w, int on, int off, Color);
void drawLine(Canvas&, int x0, int y0, int x1, int y1, Color);  // Bresenham

// A hollow ring, for the degree sign. Generating a font range up to 0xB0 just to
// get U+00B0 would pull in ~130 junk glyphs.
void drawDegree(Canvas&, int cx, int cy, int r, Color);

}  // namespace epd
