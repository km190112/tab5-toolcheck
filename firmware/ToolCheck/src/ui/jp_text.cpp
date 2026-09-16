#include "jp_text.h"

namespace toolcheck {
namespace ui {
namespace {

void setFont(LovyanGFX& gfx, TextSize size) {
  switch (size) {
    case TextSize::Small:
      gfx.setFont(&fonts::lgfxJapanGothicP_28);
      break;
    case TextSize::Medium:
      gfx.setFont(&fonts::lgfxJapanGothicP_32);
      break;
    case TextSize::Large:
      gfx.setFont(&fonts::lgfxJapanGothicP_40);
      break;
  }
}

void drawWithDatum(LovyanGFX& gfx, int x, int y, const char* text, TextSize size, uint16_t fg, uint16_t bg,
                   textdatum_t datum) {
  setFont(gfx, size);
  gfx.setTextColor(fg, bg);  // uint16_t なので RGB565 として読まれる
  gfx.setTextDatum(datum);
  gfx.drawString(text, x, y);
  gfx.setTextDatum(textdatum_t::top_left);
}

}  // namespace

void drawText(LovyanGFX& gfx, int x, int y, const char* text, TextSize size, uint16_t fg, uint16_t bg) {
  drawWithDatum(gfx, x, y, text, size, fg, bg, textdatum_t::top_left);
}

void drawTextRight(LovyanGFX& gfx, int right_x, int y, const char* text, TextSize size, uint16_t fg, uint16_t bg) {
  drawWithDatum(gfx, right_x, y, text, size, fg, bg, textdatum_t::top_right);
}

void drawTextCenter(LovyanGFX& gfx, int center_x, int y, const char* text, TextSize size, uint16_t fg, uint16_t bg) {
  drawWithDatum(gfx, center_x, y, text, size, fg, bg, textdatum_t::top_center);
}

int textWidth(LovyanGFX& gfx, const char* text, TextSize size) {
  setFont(gfx, size);
  return gfx.textWidth(text);
}

namespace {

int textHeight(TextSize size) {
  switch (size) {
    case TextSize::Small: return 28;
    case TextSize::Medium: return 32;
    case TextSize::Large: return 40;
  }
  return 28;
}

}  // namespace

TextSize drawTextFit(LovyanGFX& gfx, int x, int y, int max_w, const char* text, TextSize preferred, uint16_t fg,
                     uint16_t bg) {
  TextSize size = preferred;
  while (size != TextSize::Small && textWidth(gfx, text, size) > max_w) {
    size = (size == TextSize::Large) ? TextSize::Medium : TextSize::Small;
  }
  if (max_w <= 0) return size;
  const int top = y + (textHeight(preferred) - textHeight(size)) / 2;
  if (textWidth(gfx, text, size) <= max_w) {
    drawText(gfx, x, top, text, size, fg, bg);
    return size;
  }
  // 一番小さくしても入らない: 幅で切る (呼び出し側の切り取り範囲は描いた後に戻す)
  int32_t cx = 0;
  int32_t cy = 0;
  int32_t cw = 0;
  int32_t ch = 0;
  gfx.getClipRect(&cx, &cy, &cw, &ch);
  gfx.setClipRect(x, top - 4, max_w, textHeight(size) + 8);
  drawText(gfx, x, top, text, size, fg, bg);
  gfx.setClipRect(cx, cy, cw, ch);
  return size;
}

}  // namespace ui
}  // namespace toolcheck
