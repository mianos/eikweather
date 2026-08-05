#include "Gfx.h"

#include <cstdio>
#include <cstring>

// Host-compilable: no esp_* includes. Keep it that way — tools/preview builds
// this file directly with clang so the layout can be settled without flashing.

namespace epd {
namespace {

// Returns nullptr if the codepoint is outside the font's range. fontconvert
// emits only the range you ask for, so e.g. the 11-glyph clock face legitimately
// has no 'A'.
const GFXglyph* glyphFor(const GFXfont& f, unsigned char c) {
  if (c < f.first || c > f.last) return nullptr;
  return &f.glyph[c - f.first];
}

}  // namespace

TextMetrics measureText(const GFXfont& f, const char* s) {
  TextMetrics m{0, 0, 0, 0, 0};
  if (!s || !*s) return m;

  bool anyInk = false;
  int x = 0;
  for (const char* p = s; *p; ++p) {
    const GFXglyph* g = glyphFor(f, static_cast<unsigned char>(*p));
    if (!g) continue;
    if (g->width && g->height) {
      const int l = x + g->xOffset;
      const int r = l + g->width - 1;
      const int t = g->yOffset;
      const int b = t + g->height - 1;
      if (!anyInk) {
        m.inkLeft = l;
        m.inkRight = r;
        m.inkTop = t;
        m.inkBottom = b;
        anyInk = true;
      } else {
        if (l < m.inkLeft) m.inkLeft = l;
        if (r > m.inkRight) m.inkRight = r;
        if (t < m.inkTop) m.inkTop = t;
        if (b > m.inkBottom) m.inkBottom = b;
      }
    }
    x += g->xAdvance;
  }
  m.advance = x;
  return m;
}

void drawText(Canvas& c, const GFXfont& f, int x, int baselineY, const char* s,
              Color col) {
  if (!s) return;
  for (const char* p = s; *p; ++p) {
    const GFXglyph* g = glyphFor(f, static_cast<unsigned char>(*p));
    if (!g) continue;

    // Walk width*height bits CONTIGUOUSLY — rows are not byte-padded.
    const uint8_t* bits = f.bitmap + g->bitmapOffset;
    uint32_t bit = 0;
    for (int gy = 0; gy < g->height; ++gy) {
      for (int gx = 0; gx < g->width; ++gx, ++bit) {
        if (bits[bit >> 3] & (0x80 >> (bit & 7))) {
          // Canvas::setPixel clips, so no bounds check is needed here.
          c.setPixel(x + g->xOffset + gx, baselineY + g->yOffset + gy, col);
        }
      }
    }
    x += g->xAdvance;
  }
}

void drawTextRight(Canvas& c, const GFXfont& f, int rightX, int baselineY,
                   const char* s, Color col) {
  drawText(c, f, rightX - measureText(f, s).advance, baselineY, s, col);
}

void drawTextCentered(Canvas& c, const GFXfont& f, int cx, int baselineY,
                      const char* s, Color col) {
  drawText(c, f, cx - measureText(f, s).advance / 2, baselineY, s, col);
}

int drawTextClipped(Canvas& c, const GFXfont& f, int x, int baselineY, int maxW,
                    const char* s, Color col) {
  if (!s || !*s) return 0;

  const int full = measureText(f, s).advance;
  if (full <= maxW) {
    drawText(c, f, x, baselineY, s, col);
    return full;
  }

  // Truncate to the longest prefix that fits alongside "..".
  const int ellipsisW = measureText(f, "..").advance;
  char buf[64];
  const size_t n = strlen(s);
  const size_t cap = (n < sizeof(buf) - 3) ? n : sizeof(buf) - 3;

  size_t keep = 0;
  for (size_t i = 1; i <= cap; ++i) {
    memcpy(buf, s, i);
    buf[i] = '\0';
    if (measureText(f, buf).advance + ellipsisW > maxW) break;
    keep = i;
  }

  memcpy(buf, s, keep);
  buf[keep] = '.';
  buf[keep + 1] = '.';
  buf[keep + 2] = '\0';
  const int used = measureText(f, buf).advance;
  drawText(c, f, x, baselineY, buf, col);
  return used;
}

int glyphOffsetOf(const GFXfont& f, const char* s, char ch, int nth) {
  if (!s) return -1;
  int x = 0, seen = 0;
  for (const char* p = s; *p; ++p) {
    const GFXglyph* g = glyphFor(f, static_cast<unsigned char>(*p));
    if (!g) continue;
    if (*p == ch) {
      if (seen == nth) return x;
      ++seen;
    }
    x += g->xAdvance;
  }
  return -1;
}

// --- shapes ---------------------------------------------------------------

void fillRect(Canvas& c, int x, int y, int w, int h, Color col) {
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i) c.setPixel(x + i, y + j, col);
}

void drawHLine(Canvas& c, int x, int y, int w, Color col) {
  for (int i = 0; i < w; ++i) c.setPixel(x + i, y, col);
}

void drawVLine(Canvas& c, int x, int y, int h, Color col) {
  for (int j = 0; j < h; ++j) c.setPixel(x, y + j, col);
}

void drawRect(Canvas& c, int x, int y, int w, int h, Color col) {
  if (w <= 0 || h <= 0) return;
  drawHLine(c, x, y, w, col);
  drawHLine(c, x, y + h - 1, w, col);
  drawVLine(c, x, y, h, col);
  drawVLine(c, x + w - 1, y, h, col);
}

void drawDashedHLine(Canvas& c, int x, int y, int w, int on, int off,
                     Color col) {
  if (on <= 0) return;
  const int period = on + (off > 0 ? off : 0);
  for (int i = 0; i < w; ++i)
    if (i % period < on) c.setPixel(x + i, y, col);
}

void drawLine(Canvas& c, int x0, int y0, int x1, int y1, Color col) {
  int dx = x1 - x0, dy = y1 - y0;
  const int sx = dx >= 0 ? 1 : -1;
  const int sy = dy >= 0 ? 1 : -1;
  if (dx < 0) dx = -dx;
  if (dy < 0) dy = -dy;

  int err = dx - dy;
  for (;;) {
    c.setPixel(x0, y0, col);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = err * 2;
    if (e2 > -dy) {
      err -= dy;
      x0 += sx;
    }
    if (e2 < dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void drawDegree(Canvas& c, int cx, int cy, int r, Color col) {
  // Midpoint circle, outline only.
  int x = r, y = 0, err = 1 - r;
  while (x >= y) {
    c.setPixel(cx + x, cy + y, col);
    c.setPixel(cx + y, cy + x, col);
    c.setPixel(cx - y, cy + x, col);
    c.setPixel(cx - x, cy + y, col);
    c.setPixel(cx - x, cy - y, col);
    c.setPixel(cx - y, cy - x, col);
    c.setPixel(cx + y, cy - x, col);
    c.setPixel(cx + x, cy - y, col);
    ++y;
    if (err < 0) {
      err += 2 * y + 1;
    } else {
      --x;
      err += 2 * (y - x) + 1;
    }
  }
}

void drawTrendArrow(Canvas& c, int x, int bottomY, int w, int dir, Color col) {
  if (w < 5 || dir == 0) return;

  const int half = w / 2;
  const int cx = x + half;
  const int h = half + 1;  // 45 degrees: half-width grows exactly 1px per row
  const int top = bottomY - h + 1;

  // Row i counted from the APEX — top when pointing up, bottom when pointing
  // down — so one loop draws both directions.
  for (int i = 0; i < h; ++i) {
    const int y = dir > 0 ? top + i : bottomY - i;
    drawHLine(c, cx - i, y, i * 2 + 1, col);
  }
}

}  // namespace epd
