// 日本語の描画はここに集める (CLAUDE.md の規約)。
// setTextWrap(false, false) は M5.begin() の後に ToolCheck.ino で済ませてある前提。
// 色は RGB565 (uint16_t) で受け取る。uint32_t で渡すと LovyanGFX は RGB888 として読むので、
// TFT_WHITE (0xFFFF) が水色、TFT_GREEN (0x07E0) が青に出る (2026-09-13 M5 4 段目で踏んだ)。
#pragma once

#include <M5GFX.h>

#include <cstdint>

namespace toolcheck {
namespace ui {

enum class TextSize : uint8_t {
  Small,   // 28px
  Medium,  // 32px
  Large,   // 40px
};

// (x, y) を左上にして 1 行描く。bg で文字の背景を塗る
void drawText(LovyanGFX& gfx, int x, int y, const char* text, TextSize size, uint16_t fg, uint16_t bg);

// right_x を右端にして描く
void drawTextRight(LovyanGFX& gfx, int right_x, int y, const char* text, TextSize size, uint16_t fg, uint16_t bg);

// center_x を中央にして描く
void drawTextCenter(LovyanGFX& gfx, int center_x, int y, const char* text, TextSize size, uint16_t fg, uint16_t bg);

// 描いたときの幅 (ピクセル)
int textWidth(LovyanGFX& gfx, const char* text, TextSize size);

// (x, y) から max_w に収めて 1 行描く。preferred で入らなければ 1〜2 段小さくし、Small でも入らなければ max_w で切る。
// y は preferred で描くときの上端 (小さくしたときは縦の中央がそろうようにずらす)。描いた大きさを返す。
// 15 文字の幅の広い物品番号が右の文字やボタンに重なった (2026-09-15 実機) ので、物品番号はこれで描く
TextSize drawTextFit(LovyanGFX& gfx, int x, int y, int max_w, const char* text, TextSize preferred, uint16_t fg,
                     uint16_t bg);

}  // namespace ui
}  // namespace toolcheck
