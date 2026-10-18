#include "ui_font.h"

#include <TFT_eSPI.h>
#include <string.h>

#include "ui_font_data.h"

extern TFT_eSPI tft;

static const uint16_t kUiBg = 0x0000;
static uint16_t kUiBuf[236 * 40];

static const UiGlyph* findGlyph(const UiGlyph* glyphs, int count, uint16_t cp) {
  int lo = 0;
  int hi = count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint16_t cur = glyphs[mid].cp;
    if (cur == cp) return &glyphs[mid];
    if (cur < cp) lo = mid + 1;
    else hi = mid - 1;
  }
  return nullptr;
}

static uint16_t decodeUtf8(const char*& p) {
  const uint8_t* s = (const uint8_t*)p;
  if (!s || !s[0]) return 0;
  if (s[0] < 0x80) {
    p += 1;
    return s[0];
  }
  if ((s[0] & 0xE0) == 0xC0 && s[1]) {
    p += 2;
    return ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
  }
  if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
    p += 3;
    return ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
  }
  p += 1;
  return s[0];
}

struct UiFace {
  int ascent;
  int descent;
  const UiGlyph* glyphs;
  int count;
  const uint8_t* bits;
};

static UiFace faceOf(bool large) {
  if (large) {
    return {kUiLgAscent, kUiLgDescent, kUiLgGlyphs,
            (int)(sizeof(kUiLgGlyphs) / sizeof(kUiLgGlyphs[0])), kUiLgBits};
  }
  return {kUiSmAscent, kUiSmDescent, kUiSmGlyphs,
          (int)(sizeof(kUiSmGlyphs) / sizeof(kUiSmGlyphs[0])), kUiSmBits};
}

int uiTextWidth(const char* text, bool large) {
  if (!text || !text[0]) return 0;
  UiFace face = faceOf(large);
  int width = 0;
  const char* p = text;
  while (*p) {
    uint16_t cp = decodeUtf8(p);
    const UiGlyph* g = findGlyph(face.glyphs, face.count, cp);
    width += g ? g->adv : 8;
  }
  return width;
}

void uiDrawCenter(const char* text, int cx, int cy, uint16_t color, bool large) {
  if (!text || !text[0]) return;
  UiFace face = faceOf(large);
  int minx = 0;
  int maxx = 0;
  int miny = -face.ascent;
  int maxy = face.descent;
  int pen = 0;
  bool any = false;
  const char* p = text;
  while (*p) {
    uint16_t cp = decodeUtf8(p);
    const UiGlyph* g = findGlyph(face.glyphs, face.count, cp);
    int adv = g ? g->adv : 8;
    if (g && g->w && g->h) {
      int x0 = pen + g->ox;
      int x1 = x0 + g->w;
      int y0 = g->oy;
      int y1 = g->oy + g->h;
      if (!any || x0 < minx) minx = x0;
      if (!any || x1 > maxx) maxx = x1;
      if (y0 < miny) miny = y0;
      if (y1 > maxy) maxy = y1;
      any = true;
    }
    pen += adv;
  }
  if (!any) {
    minx = 0;
    maxx = pen;
  }
  if (pen > maxx) maxx = pen;
  int width = maxx - minx;
  int height = maxy - miny;
  if (width < 1 || height < 1) return;
  if (width > 236) width = 236;
  if (height > 40) height = 40;

  uint16_t* buf = kUiBuf;
  for (int i = 0; i < width * height; i++) buf[i] = kUiBg;

  pen = 0;
  p = text;
  while (*p) {
    uint16_t cp = decodeUtf8(p);
    const UiGlyph* g = findGlyph(face.glyphs, face.count, cp);
    int adv = g ? g->adv : 8;
    if (g && g->w && g->h) {
      int bit = 0;
      for (int gy = 0; gy < g->h; gy++) {
        for (int gx = 0; gx < g->w; gx++) {
          int idx = bit++;
          uint8_t byte = face.bits[g->off + (idx >> 3)];
          if ((byte >> (7 - (idx & 7))) & 1) {
            int px = pen + g->ox - minx + gx;
            int py = g->oy - miny + gy;
            if (px >= 0 && py >= 0 && px < width && py < height) buf[py * width + px] = color;
          }
        }
      }
    }
    pen += adv;
    if (pen - minx >= width) break;
  }

  int x = cx - width / 2;
  int y = cy - height / 2;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x + width > 240) width = 240 - x;
  if (y + height > 240) height = 240 - y;
  if (width < 1 || height < 1) return;
  tft.setSwapBytes(false);
  tft.pushImage(x, y, width, height, buf);
}
