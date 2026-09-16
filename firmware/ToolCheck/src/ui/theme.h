// 画面の寸法と色 (縦 720x1280。docs/設計.md「画面」)。色は RGB565
#pragma once

#include <cstdint>

namespace toolcheck {
namespace ui {

constexpr int kScreenW = 720;
constexpr int kScreenH = 1280;
constexpr int kMargin = 20;

constexpr int kHeaderH = 80;
constexpr int kTabBarH = 100;
constexpr int kTabBarY = kScreenH - kTabBarH;
constexpr int kAlertBandH = 120;   // 赤帯 (写真は 4b で足す)
constexpr int kListTitleH = 56;
constexpr int kRowH = 110;         // 一覧の 1 件 (2 行)
constexpr int kPagerW = 110;       // 右端のページ送り
constexpr int kToastH = 56;        // 一覧の下に出す直前の出来事

constexpr int kSessionTopY = kHeaderH;
constexpr int kSessionGuideH = 110;
constexpr int kSessionRowsY = 350;  // 320 だと案内の行 (28px) が 1 行目に重なった
constexpr int kSessionRowH = 88;
constexpr int kSessionRowsShown = 10;
// 「持出に切替」は 200px だと 15 文字の物品番号がボタンの下に隠れたので細くした (2026-09-15 実機。× の手前 20px)
constexpr int kSessionSwitchX = 420;
constexpr int kSessionSwitchW = 170;
constexpr int kSessionRemoveX = 610;
constexpr int kSessionRemoveW = 90;
constexpr int kSessionButtonH = 64;
// 「取りやめ」(2026-09-15): 警告の秒の右。警告のバーは幅 680 → 480 に縮めて重ならないようにした
constexpr int kSessionCancelX = 520;
constexpr int kSessionCancelW = 180;
constexpr int kSessionCancelDy = 50;  // 案内の帯の下から
constexpr int kSessionBarW = 480;

constexpr uint16_t kColorBg = 0x0000;
constexpr uint16_t kColorText = 0xFFFF;
constexpr uint16_t kColorDim = 0x8410;       // 灰
constexpr uint16_t kColorAccent = 0x07FF;    // 水色
constexpr uint16_t kColorWarn = 0xFFE0;      // 黄
constexpr uint16_t kColorAlert = 0xF800;     // 赤
constexpr uint16_t kColorOk = 0x07E0;        // 緑
constexpr uint16_t kColorPanel = 0x2104;     // 暗い灰
constexpr uint16_t kColorHeaderBg = 0x18C3;  // ヘッダ
constexpr uint16_t kColorTabActive = 0x04DF; // 選んでいるタブ
constexpr uint16_t kColorAlertBg = 0x8000;   // 暗い赤
constexpr uint16_t kColorGuideBg = 0x0010;   // 紺
constexpr uint16_t kColorTagBg = 0x0320;     // 暗い緑
constexpr uint16_t kColorButton = 0x4208;    // ボタン

}  // namespace ui
}  // namespace toolcheck
