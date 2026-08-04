// fontgen — emit Adafruit-GFX-compatible font tables from any .ttf/.ttc, using
// macOS CoreText. No freetype, no Homebrew, no Python deps: clang and the system
// frameworks are enough.
//
//   clang -framework CoreText -framework CoreGraphics -framework Foundation \
//         -o fontgen fontgen.m
//   ./fontgen "/System/Library/Fonts/Supplemental/Arial Narrow Bold.ttf" 88 48 58 Font_Clock
//
// Emits to stdout:
//   static const uint8_t <Name>_Bitmaps[] = {...};
//   static const GFXglyph <Name>_Glyphs[] = {...};
//   const GFXfont <Name> = {...};
// and prints measured metrics to stderr so sizes can be tuned without flashing.
//
// FORMAT CONTRACT (must match Gfx.cpp's blitter):
//   * Each glyph STARTS on a byte boundary -> bitmapOffset is a byte index.
//   * Within a glyph, bits run CONTIGUOUSLY for width*height bits, MSB first,
//     with NO per-row padding.
//   * yOffset is measured from the BASELINE and is negative for ink above it.

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int width, height, xAdvance, xOffset, yOffset;
  unsigned char *bits;  // width*height bits, contiguous, MSB first
  size_t nbits;
  int present;
} Glyph;

static CTFontRef loadFont(const char *path, double pt) {
  CFStringRef p = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
  CFURLRef url = CFURLCreateWithFileSystemPath(NULL, p, kCFURLPOSIXPathStyle, false);
  CFArrayRef descs = CTFontManagerCreateFontDescriptorsFromURL(url);
  if (!descs || CFArrayGetCount(descs) == 0) {
    fprintf(stderr, "fontgen: cannot read font at %s\n", path);
    exit(1);
  }
  CTFontDescriptorRef d = (CTFontDescriptorRef)CFArrayGetValueAtIndex(descs, 0);
  CTFontRef f = CTFontCreateWithFontDescriptor(d, pt, NULL);
  CFRelease(descs);
  CFRelease(url);
  CFRelease(p);
  return f;
}

int main(int argc, char **argv) {
  if (argc != 6) {
    fprintf(stderr,
            "usage: %s <font-file> <pt> <first> <last> <SymbolName>\n", argv[0]);
    return 2;
  }
  const char *path = argv[1];
  const double pt = atof(argv[2]);
  const int first = atoi(argv[3]);
  const int last = atoi(argv[4]);
  const char *name = argv[5];

  CTFontRef font = loadFont(path, pt);
  const double ascent = CTFontGetAscent(font);
  const double descent = CTFontGetDescent(font);
  const double leading = CTFontGetLeading(font);

  const int pad = 8;
  const int ascI = (int)ceil(ascent) + pad;
  const int descI = (int)ceil(descent) + pad;
  const int H = ascI + descI;

  const int nglyphs = last - first + 1;
  Glyph *glyphs = calloc((size_t)nglyphs, sizeof(Glyph));

  int maxInkTop = 0, minInkTop = 0, maxFigureH = 0;

  for (int ch = first; ch <= last; ++ch) {
    Glyph *G = &glyphs[ch - first];

    UniChar uc = (UniChar)ch;
    CGGlyph g = 0;
    if (!CTFontGetGlyphsForCharacters(font, &uc, &g, 1) || g == 0) {
      // Not in the face: zero-size, zero-advance. The blitter skips it.
      G->present = 0;
      continue;
    }

    CGSize adv;
    CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, &g, &adv, 1);

    const int W = (int)ceil(adv.width) + 4 * pad;

    // 8bpp grayscale, black background, white glyph. CG origin is bottom-left;
    // the buffer's first row is the TOP of the image, so buffer row r maps to
    // CG y = H-1-r. We convert once, below, rather than flipping the CTM (which
    // would also mirror the glyph).
    unsigned char *buf = calloc((size_t)W * (size_t)H, 1);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(buf, (size_t)W, (size_t)H, 8,
                                             (size_t)W, cs, kCGImageAlphaNone);
    CGColorSpaceRelease(cs);

    // Antialiasing ON, then threshold at 50%: preserves stem shape far better
    // than aliased rendering, which drops thin strokes at these sizes.
    CGContextSetShouldAntialias(ctx, true);
    CGContextSetShouldSmoothFonts(ctx, false);
    CGContextSetGrayFillColor(ctx, 1.0, 1.0);

    const double penXcg = (double)(2 * pad);
    const double penYcg = (double)descI;  // baseline, in CG (bottom-up) coords
    CGPoint pos = CGPointMake(penXcg, penYcg);
    CTFontDrawGlyphs(font, &g, &pos, 1, ctx);

    // Ink bbox in BUFFER rows/cols.
    int x0 = W, x1 = -1, y0 = H, y1 = -1;
    for (int r = 0; r < H; ++r) {
      const unsigned char *row = buf + (size_t)r * (size_t)W;
      for (int c = 0; c < W; ++c) {
        if (row[c] >= 128) {
          if (c < x0) x0 = c;
          if (c > x1) x1 = c;
          if (r < y0) y0 = r;
          if (r > y1) y1 = r;
        }
      }
    }

    G->present = 1;
    G->xAdvance = (int)lround(adv.width);

    if (x1 < 0) {
      // Blank glyph (space): no ink, but it still advances.
      G->width = G->height = 0;
      G->xOffset = G->yOffset = 0;
      G->bits = NULL;
      G->nbits = 0;
    } else {
      G->width = x1 - x0 + 1;
      G->height = y1 - y0 + 1;
      // Baseline in buffer rows: CG y=penYcg  ->  row H-1-penYcg.
      const int baselineRow = H - 1 - (int)penYcg;
      G->xOffset = x0 - (int)penXcg;
      G->yOffset = y0 - baselineRow;

      G->nbits = (size_t)G->width * (size_t)G->height;
      G->bits = calloc((G->nbits + 7) / 8, 1);
      size_t bit = 0;
      for (int r = y0; r <= y1; ++r) {
        for (int c = x0; c <= x1; ++c, ++bit) {
          if (buf[(size_t)r * (size_t)W + (size_t)c] >= 128)
            G->bits[bit >> 3] |= (unsigned char)(0x80 >> (bit & 7));
        }
      }

      if (G->yOffset < minInkTop) minInkTop = G->yOffset;
      if (G->yOffset > maxInkTop) maxInkTop = G->yOffset;
      if (ch >= '0' && ch <= '9' && G->height > maxFigureH) maxFigureH = G->height;
    }

    CGContextRelease(ctx);
    free(buf);
  }

  // --- emit ---------------------------------------------------------------
  printf("\n// ---------------------------------------------------------------"
         "-------\n");
  printf("// %s — %s @ %gpt, chars %d..%d\n", name, path, pt, first, last);
  printf("// Generated by tools/fontgen (CoreText). Do not hand-edit.\n");

  printf("static const uint8_t %s_Bitmaps[] = {\n ", name);
  size_t total = 0;
  int col = 0;
  // Two passes: assign byte offsets (glyph-aligned), then emit.
  size_t *offsets = calloc((size_t)nglyphs, sizeof(size_t));
  for (int i = 0; i < nglyphs; ++i) {
    offsets[i] = total;
    total += (glyphs[i].nbits + 7) / 8;  // pad each glyph to a byte boundary
  }
  for (int i = 0; i < nglyphs; ++i) {
    const size_t nb = (glyphs[i].nbits + 7) / 8;
    for (size_t b = 0; b < nb; ++b) {
      printf(" 0x%02X,", glyphs[i].bits[b]);
      if (++col % 12 == 0) printf("\n ");
    }
  }
  if (total == 0) printf(" 0x00,");  // never emit an empty array
  printf("\n};\n\n");

  printf("static const GFXglyph %s_Glyphs[] = {\n", name);
  for (int i = 0; i < nglyphs; ++i) {
    const Glyph *G = &glyphs[i];
    printf("  {%5zu, %3d, %3d, %3d, %4d, %4d},   // 0x%02X",
           offsets[i], G->width, G->height, G->xAdvance, G->xOffset, G->yOffset,
           first + i);
    const int ch = first + i;
    if (ch >= 33 && ch <= 126) printf(" '%c'", (char)ch);
    printf("\n");
  }
  printf("};\n\n");

  const int yAdv = (int)lround(ascent + descent + leading);
  printf("const GFXfont %s = {%s_Bitmaps, %s_Glyphs, 0x%02X, 0x%02X, %d};\n",
         name, name, name, first, last, yAdv);

  fprintf(stderr,
          "%-12s %-40s %5.1fpt  bitmap=%5zu B  glyphs=%3d  yAdvance=%3d  "
          "figureH=%3d  inkTop=[%d..%d]\n",
          name, strrchr(path, '/') ? strrchr(path, '/') + 1 : path, pt, total,
          nglyphs, yAdv, maxFigureH, minInkTop, maxInkTop);

  CFRelease(font);
  return 0;
}
