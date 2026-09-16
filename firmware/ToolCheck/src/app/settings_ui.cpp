#include "settings_ui.h"

#include <M5Unified.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../core/about.h"
#include "../core/config_store.h"
#include "../core/license_text.h"
#include "../ui/jp_text.h"
#include "../ui/theme.h"

namespace toolcheck {
namespace {

using ui::TextSize;

struct FieldSpec {
  const char* name;   // config_store の設定の名前
  const char* label;
  int32_t min;        // ボタンで動かせる範囲 (制約は保存するときに applyConfig が確かめる)
  int32_t max;
  int32_t step;
  const char* unit;
  bool toggle;
};

enum class Extra : uint8_t { None, Sound, Photo, Pin, Drawer };

struct EditorSpec {
  const char* title;
  const FieldSpec* fields;
  uint8_t count;
  Extra extra;
};

constexpr FieldSpec kBrightnessFields[] = {
    {"brightness_pct", "通常の明るさ", 10, 100, 5, "%", false},
    {"dim_after_min", "減光までの時間 (0 = しない)", 0, 60, 1, "分", false},
    {"dim_brightness_pct", "減光時の明るさ", 5, 100, 5, "%", false},
    {"off_after_min", "消灯までの時間 (0 = しない)", 0, 120, 5, "分", false},
};
constexpr FieldSpec kVolumeFields[] = {
    {"volume_operation_pct", "操作音 (0 = 鳴らさない)", 0, 100, 10, "%", false},
    {"volume_warning1_pct", "警告 1 (0 = 鳴らさない)", 0, 100, 10, "%", false},  // 0 は記録だけ取る現場向け (2026-09-14)
    {"volume_warning2_pct", "警告 2 (0 = 鳴らさない)", 0, 100, 10, "%", false},
};
constexpr FieldSpec kWarningFields[] = {
    {"warning1_sec", "警告 1 まで", 10, 600, 5, "秒", false},
    {"warning2_sec", "警告 2 まで", 15, 900, 5, "秒", false},
    {"giveup_sec", "警告 2 から打ち切りまで", 30, 1800, 10, "秒", false},
};
constexpr FieldSpec kPhotoFields[] = {
    {"camera_enabled", "カメラ (入切は再起動で反映)", 0, 1, 1, "", true},
    {"save_photos_to_sd", "写真を SD に保存", 0, 1, 1, "", true},
    {"capture_delay_ms", "撮影までの待ち時間", 0, 3000, 100, "ms", false},  // 開放 (センサを使わない設定では名札) から
};
constexpr FieldSpec kUserFields[] = {
    // 2026-09-15: 名前を英字にして記号で桁埋めした ID も使えるように。英字・記号も使うなら桁数は文字数
    {"user_id_alnum", "利用者 ID に英字・記号も使う", 0, 1, 1, "", true},
    {"user_id_digits", "利用者 ID の文字数", 1, 15, 1, "文字", false},
};
constexpr FieldSpec kDrawerFields[] = {
    {"drawer_sensor_enabled", "引き出しセンサ (Unit ToF)", 0, 1, 1, "", true},  // 2026-09-14
    // 閉の閾値 (既定 50mm) より大きくないと保存で断られるので、ボタンの下限は 60 (M6)
    {"open_delta_mm", "開の閾値 (基準から近づいた距離)", 60, 1000, 10, "mm", false},
};

constexpr uint8_t kEdBrightness = 0;
constexpr uint8_t kEdVolume = 1;
constexpr uint8_t kEdWarning = 2;
constexpr uint8_t kEdPhoto = 3;
constexpr uint8_t kEdUser = 4;
constexpr uint8_t kEdDrawer = 5;
const EditorSpec kEditors[] = {
    {"画面の明るさ", kBrightnessFields, 4, Extra::None},
    {"音量", kVolumeFields, 3, Extra::Sound},
    {"警告の秒数", kWarningFields, 3, Extra::None},
    {"写真", kPhotoFields, 3, Extra::Photo},
    {"利用者 ID・PIN", kUserFields, 2, Extra::Pin},
    {"引き出しセンサ", kDrawerFields, 2, Extra::Drawer},
};

constexpr uint32_t kLiveMs = 250;  // 「引き出しセンサ」の距離を描き直す間隔

// 一覧の 13 項目 (計画の順。2026-09-14 に「ToF 校正」→「引き出しセンサ」、「NVS データ削除」→「データ削除」。
// 2026-09-17 に「このソフトについて」)
constexpr uint8_t kItemCount = 13;
const char* const kItemLabels[kItemCount] = {"警告の一時停止", "時刻",           "画面の明るさ",     "音量",
                                             "警告の秒数",     "写真",           "引き出しセンサ",   "利用者 ID・PIN",
                                             "赤帯・貸出の取消", "USB シリアル出力", "データ削除",     "機器情報",
                                             "このソフトについて"};
constexpr uint8_t kItemPause = 0;
constexpr uint8_t kItemClock = 1;
constexpr uint8_t kItemBrightness = 2;
constexpr uint8_t kItemVolume = 3;
constexpr uint8_t kItemWarning = 4;
constexpr uint8_t kItemPhoto = 5;
constexpr uint8_t kItemTof = 6;
constexpr uint8_t kItemUser = 7;
constexpr uint8_t kItemCancel = 8;
constexpr uint8_t kItemExport = 9;
constexpr uint8_t kItemErase = 10;
constexpr uint8_t kItemInfo = 11;
constexpr uint8_t kItemAbout = 12;

// 寸法
constexpr int kTitleY = ui::kHeaderH;
constexpr int kTitleH = 90;
constexpr int kBackX = 540;
constexpr int kBackW = 160;
constexpr int kBackH = 64;
constexpr int kBodyY = kTitleY + kTitleH;
constexpr int kMenuY = kBodyY + 8;
constexpr int kMenuRowH = 80;  // 13 行で下端 1218 (88 のままだと 13 行目が画面の外)
constexpr int kFieldY = kBodyY + 12;
constexpr int kFieldH = 150;
constexpr int kMinusX = 400;
constexpr int kPlusX = 560;
constexpr int kStepW = 140;
constexpr int kStepH = 76;
constexpr int kTestX = 540;
constexpr int kTestW = 160;
constexpr int kTestH = 44;  // 52 だと下の「- / +」(y+58) に 2px 重なった
constexpr int kSaveY = 1160;
constexpr int kSaveH = 100;
constexpr int kWideX = ui::kMargin;
constexpr int kWideW = ui::kScreenW - ui::kMargin * 2;
// PIN のテンキー
constexpr int kPadW = 200;
constexpr int kPadH = 140;
constexpr int kPadGap = 20;
constexpr int kPadX0 = 40;
constexpr int kPadY0 = 480;
const char* const kPadLabels[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "消す", "0", "戻る"};
constexpr int kPadDelete = 9;
constexpr int kPadBack = 11;
// 時刻
constexpr int kColW = 132;
constexpr int kColGap = 8;
constexpr int kColX0 = 14;
constexpr int kUpY = 320;
constexpr int kValueY = 450;
constexpr int kDownY = 530;
constexpr int kArrowH = 100;
constexpr int kTzY = 720;
// 赤帯・貸出の取消
constexpr int kSubTabY = kBodyY + 10;
constexpr int kSubTabH = 70;
constexpr int kSubTabW = 330;
constexpr int kListY = kSubTabY + kSubTabH + 16;
constexpr int kListRowH = 120;
constexpr uint16_t kListRows = 6;
constexpr int kActionX = 500;
constexpr int kActionW = 200;
constexpr int kActionH = 84;
constexpr int kPagerY = kListY + kListRows * kListRowH + 10;
constexpr int kPagerH = 84;
// USB シリアル出力
const char* const kExportWhat[6] = {"loans", "hist", "openlog", "alerts", "cfg", "all"};
const char* const kExportLabels[6] = {"貸出中", "返却履歴", "開閉ログ", "赤帯", "設定値", "すべて"};
constexpr int kExportY = kBodyY + 130;
constexpr int kExportW = 330;
constexpr int kExportH = 110;
constexpr int kExportGap = 20;
// データ削除 (記録の削除・全削除・SD の写真)
constexpr int kEraseRecordsY = 520;
constexpr int kEraseAllY = 740;
constexpr int kErasePhotosY = 960;
constexpr int kEraseH = 170;
// このソフトについて・ライセンス全文
constexpr int kAboutQrW = 420;
constexpr int kAboutQrY = 620;
constexpr int kLicenseButtonY = kBodyY + 20;
constexpr int kLicenseButtonH = 110;
constexpr int kLicenseButtonGap = 16;
constexpr int kLicenseTextY = kBodyY + 12;
constexpr int kLicenseLineH = 38;
constexpr size_t kLicenseLinesPerPage = 26;  // 下端 182 + 26 × 38 = 1170。その下にページ送り
constexpr int kLicensePagerY = 1180;

// 1 文字の幅 (ASCII は測った値を覚えておく。ライセンス全文を開くときに 1 回だけ折り返す)
struct GlyphMeasure {
  LovyanGFX* gfx;
  int16_t ascii[128];
};

int measureGlyph(const char* utf8, size_t len, void* ctx) {
  auto* m = static_cast<GlyphMeasure*>(ctx);
  const auto first = static_cast<unsigned char>(utf8[0]);
  if (len == 1 && first < 128 && m->ascii[first] >= 0) return m->ascii[first];
  char buf[8];
  const size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  std::memcpy(buf, utf8, n);
  buf[n] = '\0';
  const int w = ui::textWidth(*m->gfx, buf, TextSize::Small);
  if (len == 1 && first < 128) m->ascii[first] = static_cast<int16_t>(w);
  return w;
}

bool inRect(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

void drawButton(LovyanGFX& d, int x, int y, int w, int h, const char* label, TextSize size,
                uint16_t bg = ui::kColorButton) {
  const int text_h = size == TextSize::Large ? 40 : (size == TextSize::Medium ? 32 : 28);
  d.fillRoundRect(x, y, w, h, 12, bg);
  ui::drawTextCenter(d, x + w / 2, y + (h - text_h) / 2, label, size, ui::kColorText, bg);
}

int32_t fieldValue(const Config& c, const char* name) {
  char buf[16];
  if (!formatConfigField(c, name, buf, sizeof(buf))) return 0;
  return static_cast<int32_t>(std::atol(buf));
}

void setFieldValue(Config* c, const char* name, int32_t value) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%ld", static_cast<long>(value));
  setConfigField(c, name, buf);
}

bool validLocal(const LocalDateTime& t) {
  int64_t e = 0;
  return fromLocalDateTime(t, 0, &e);
}

// 月や年を変えて日が月末を超えたら、その月の末日に寄せる
void clampDay(LocalDateTime* t) {
  while (t->day > 28 && !validLocal(*t)) --t->day;
}

int wrap(int value, int lo, int hi) {
  if (value < lo) return hi;
  if (value > hi) return lo;
  return value;
}

// "9/13 21:32"。時刻が無効なら "--/-- --:--"
void formatShortDateTime(int64_t epoch, int32_t tz, char* buf, size_t len) {
  LocalDateTime t;
  if (!toLocalDateTime(epoch, tz, &t)) {
    std::snprintf(buf, len, "--/-- --:--");
    return;
  }
  std::snprintf(buf, len, "%d/%d %02d:%02d", t.month, t.day, t.hour, t.minute);
}

uint16_t pageCountOf(size_t items) {
  const size_t pages = (items + kListRows - 1) / kListRows;
  return static_cast<uint16_t>(pages == 0 ? 1 : pages);
}

}  // namespace

SettingsUi::SettingsUi(SettingsActions& actions, const LoanBook& book, const OpenLog& log)
    : actions_(actions), book_(book), log_(log) {}

void SettingsUi::open(uint32_t now_ms) {
  (void)now_ms;
  page_ = Page::Pin;
  pin_.clear();
  setMessage("", false);
}

bool SettingsUi::needsRedraw(uint32_t now_ms) const {
  if (dirty_) return true;
  if (page_ == Page::Erase && hold_ms_ / 100 != drawn_hold_step_) return true;
  const bool ticking = page_ == Page::Pause || page_ == Page::Info || (page_ == Page::Pin && pin_.locked(now_ms));
  return ticking && now_ms / 1000 != drawn_second_;
}

bool SettingsUi::needsLiveRedraw(uint32_t now_ms) const { return tofScreenOpen() && now_ms / kLiveMs != drawn_live_tick_; }

void SettingsUi::drawLive(uint32_t now_ms) {
  drawn_live_tick_ = now_ms / kLiveMs;
  pollTofCalibration();
  if (dirty_ || !tofScreenOpen()) return;  // 全体の描き直しを待つ
  auto& d = M5.Display;
  d.startWrite();
  drawDrawerLive(kFieldY + kEditors[kEdDrawer].count * kFieldH + 8);
  d.endWrite();
}

bool SettingsUi::tofScreenOpen() const { return page_ == Page::Editor && editor_ == kEdDrawer; }

// 基準を取り終えていたら結果を読み、取れていれば下書きに入れる (保存を押すまでは反映しない)
void SettingsUi::pollTofCalibration() {
  if (!tofScreenOpen()) return;
  TofStatus st;
  actions_.tofStatus(&st);
  if (st.calibration_seq == seen_calibration_seq_) return;
  seen_calibration_seq_ = st.calibration_seq;
  const TofCalibration& c = st.calibration;
  calib_error_ = c.result != TofCalibrationResult::Ok;
  switch (c.result) {
    case TofCalibrationResult::Ok:
      draft_.tof_baseline_mm = c.baseline_mm;
      std::snprintf(calib_text_, sizeof(calib_text_), "取れました %u mm (%u〜%u mm・%u 個)",
                    static_cast<unsigned>(c.baseline_mm), static_cast<unsigned>(c.min_mm),
                    static_cast<unsigned>(c.max_mm), static_cast<unsigned>(c.valid_samples));
      break;
    case TofCalibrationResult::TooFewSamples:
      std::snprintf(calib_text_, sizeof(calib_text_), "取れません: 距離を読めていません (%u 個)",
                    static_cast<unsigned>(c.valid_samples));
      break;
    case TofCalibrationResult::OutOfRange:
      std::snprintf(calib_text_, sizeof(calib_text_), "取れません: %u mm は 30〜2000 mm の外です",
                    static_cast<unsigned>(c.baseline_mm));
      break;
    case TofCalibrationResult::Unstable:
      std::snprintf(calib_text_, sizeof(calib_text_), "取れません: 値がばらついています (%u〜%u mm)",
                    static_cast<unsigned>(c.min_mm), static_cast<unsigned>(c.max_mm));
      break;
  }
}

// 「引き出しセンサ」の状態・距離・基準・校正の結果 (この区画だけを kLiveMs ごとに描き直す。全体を描くとちらつく)
void SettingsUi::drawDrawerLive(int extra_y) {
  auto& d = M5.Display;
  TofStatus st;
  actions_.tofStatus(&st);
  char buf[128];
  const int full_w = ui::kScreenW - ui::kMargin * 2;

  d.fillRect(0, extra_y, ui::kScreenW, 38, ui::kColorBg);
  uint16_t color = ui::kColorOk;
  if (!st.enabled) {
    std::snprintf(buf, sizeof(buf), "Unit ToF: いまは使わない設定です");
    color = ui::kColorDim;
  } else if (!st.present) {
    std::snprintf(buf, sizeof(buf), "Unit ToF: 見つかりません (%s)", st.route);
    color = ui::kColorAlert;
  } else {
    std::snprintf(buf, sizeof(buf), "Unit ToF: %s%s", st.route, st.fault ? " 範囲外が続いています" : "");
    color = st.fault ? ui::kColorAlert : ui::kColorOk;
  }
  ui::drawTextFit(d, ui::kMargin, extra_y + 4, full_w, buf, TextSize::Small, color, ui::kColorBg);

  d.fillRect(0, extra_y + 40, kMinusX - 8, kStepH, ui::kColorBg);
  if (!st.present || !st.has_sample) {
    std::snprintf(buf, sizeof(buf), "距離 -");
  } else if (!st.valid) {
    std::snprintf(buf, sizeof(buf), "距離 範囲外");
  } else {
    std::snprintf(buf, sizeof(buf), "距離 %u mm", static_cast<unsigned>(st.mm));
  }
  ui::drawText(d, ui::kMargin, extra_y + 58, buf, TextSize::Large,
               st.present && st.has_sample && st.valid ? ui::kColorAccent : ui::kColorDim, ui::kColorBg);

  d.fillRect(0, extra_y + 124, ui::kScreenW, 92, ui::kColorBg);
  const uint16_t saved = actions_.currentConfig().tof_baseline_mm;
  if (draft_.tof_baseline_mm == 0) {
    std::snprintf(buf, sizeof(buf), "基準 未校正 (開閉を判定しません)");
  } else {
    std::snprintf(buf, sizeof(buf), "基準 %u mm%s", static_cast<unsigned>(draft_.tof_baseline_mm),
                  draft_.tof_baseline_mm != saved ? " (保存すると反映)" : "");
  }
  ui::drawTextFit(d, ui::kMargin, extra_y + 128, full_w, buf, TextSize::Medium,
                  draft_.tof_baseline_mm == 0 ? ui::kColorWarn : ui::kColorText, ui::kColorBg);
  const char* result = st.calibrating ? "基準を取っています (2 秒)" : calib_text_;
  ui::drawTextFit(d, ui::kMargin, extra_y + 178, full_w, result, TextSize::Small,
                  st.calibrating ? ui::kColorAccent : calib_error_ ? ui::kColorAlert : ui::kColorOk, ui::kColorBg);
}

void SettingsUi::draw(uint32_t now_ms) {
  switch (page_) {
    case Page::Pin:
      drawPin(now_ms);
      break;
    case Page::Menu:
      drawMenu();
      break;
    case Page::Editor:
      drawEditor();
      break;
    case Page::Pause:
      drawPause();
      break;
    case Page::Clock:
      drawClock();
      break;
    case Page::Cancel:
      drawCancel();
      break;
    case Page::Export:
      drawExport();
      break;
    case Page::Erase:
      drawErase();
      break;
    case Page::Info:
      drawInfo();
      break;
    case Page::PinChange:
      drawPinChange();
      break;
    case Page::NotYet:
      drawNotYet();
      break;
    case Page::About:
      drawAbout();
      break;
    case Page::Licenses:
      drawLicenses();
      break;
    case Page::LicenseText:
      drawLicenseText();
      break;
  }
  dirty_ = false;
  drawn_second_ = now_ms / 1000;
  drawn_hold_step_ = hold_ms_ / 100;
}

SettingsResult SettingsUi::onTouch(int x, int y, uint32_t now_ms) {
  SettingsResult r = SettingsResult::None;
  switch (page_) {
    case Page::Pin:
      r = touchPin(x, y, now_ms);
      break;
    case Page::Menu:
      r = touchMenu(x, y);
      break;
    case Page::Editor:
      r = touchEditor(x, y);
      break;
    case Page::Pause:
      r = touchPause(x, y);
      break;
    case Page::Clock:
      r = touchClock(x, y);
      break;
    case Page::Cancel:
      r = touchCancel(x, y);
      break;
    case Page::Export:
      r = touchExport(x, y);
      break;
    case Page::PinChange:
      r = touchPinChange(x, y);
      break;
    case Page::About:
      r = touchAbout(x, y);
      break;
    case Page::Licenses:
      r = touchLicenses(x, y);
      break;
    case Page::LicenseText:
      r = touchLicenseText(x, y);
      break;
    case Page::Erase:
    case Page::Info:
    case Page::NotYet:
      if (backPressed(x, y)) {
        setMessage("", false);
        page_ = Page::Menu;
        r = SettingsResult::Handled;
      }
      break;
  }
  if (r != SettingsResult::None) dirty_ = true;
  return r;
}

void SettingsUi::onPressing(int x, int y, uint32_t held_ms) {
  if (page_ != Page::Erase) return;
  uint8_t target = 0;
  if (inRect(x, y, kWideX, kEraseRecordsY, kWideW, kEraseH)) target = 1;
  if (inRect(x, y, kWideX, kEraseAllY, kWideW, kEraseH)) target = 2;
  if (inRect(x, y, kWideX, kErasePhotosY, kWideW, kEraseH)) target = 3;
  if (target != hold_target_) {
    hold_target_ = target;
    hold_fired_ = false;
  }
  hold_ms_ = target != 0 ? held_ms : 0;
  if (target == 0 || hold_fired_ || held_ms < kEraseHoldMs) return;
  hold_fired_ = true;
  if (target == 1) {
    const bool ok = actions_.eraseRecords();
    setMessage(ok ? "記録を削除しました (設定は残しています)" : "記録を削除できませんでした", !ok);
  } else if (target == 2) {
    setMessage("全削除しています。再起動します", false);
    actions_.eraseAll();  // 消せたら戻ってこない
  } else {
    const bool queued = actions_.eraseSdPhotos();  // 終わったら onPhotosErased
    setMessage(queued ? "SD の写真を消しています" : "SD が無いか、いまは消せません", !queued);
  }
}

void SettingsUi::onReleased() {
  if (hold_target_ == 0 && hold_ms_ == 0) return;
  hold_target_ = 0;
  hold_ms_ = 0;
  hold_fired_ = false;
  dirty_ = true;
}

void SettingsUi::onPhotosErased(bool ok, uint16_t removed, uint16_t left) {
  if (!ok) {
    setMessage("SD の写真を消せませんでした (SD を確かめてください)", true);
    return;
  }
  char buf[96];
  if (left > 0) {
    std::snprintf(buf, sizeof(buf), "SD の写真を %u 枚消しました (残り %u 枚)", static_cast<unsigned>(removed),
                  static_cast<unsigned>(left));
  } else {
    std::snprintf(buf, sizeof(buf), "SD の写真を %u 枚消しました", static_cast<unsigned>(removed));
  }
  setMessage(buf, left > 0);
}

// --- 描画 ---

void SettingsUi::drawTitle(const char* title, const char* back_label) {
  auto& d = M5.Display;
  d.fillRect(0, ui::kHeaderH, ui::kScreenW, ui::kScreenH - ui::kHeaderH, ui::kColorBg);
  d.fillRect(0, kTitleY, ui::kScreenW, kTitleH, ui::kColorGuideBg);
  ui::drawText(d, ui::kMargin, kTitleY + 25, title, TextSize::Large, ui::kColorText, ui::kColorGuideBg);
  if (back_label != nullptr) drawButton(d, kBackX, kTitleY + 13, kBackW, kBackH, back_label, TextSize::Medium);
}

void SettingsUi::drawMessage(int y) {
  if (message_[0] == '\0') return;
  ui::drawText(M5.Display, ui::kMargin, y, message_, TextSize::Small, message_error_ ? ui::kColorAlert : ui::kColorOk,
               ui::kColorBg);
}

void SettingsUi::drawPad() {
  auto& d = M5.Display;
  for (int i = 0; i < 12; ++i) {
    const int col = i % 3;
    const int row = i / 3;
    drawButton(d, kPadX0 + col * (kPadW + kPadGap), kPadY0 + row * (kPadH + kPadGap), kPadW, kPadH, kPadLabels[i],
               i == kPadDelete || i == kPadBack ? TextSize::Medium : TextSize::Large);
  }
}

void SettingsUi::drawPin(uint32_t now_ms) {
  auto& d = M5.Display;
  char buf[96];
  d.startWrite();
  drawTitle("設定: PIN を入力", "戻る");
  const int dots_x = ui::kScreenW / 2 - 150;
  for (int i = 0; i < kPinLength; ++i) {
    const int cx = dots_x + i * 100;
    if (i < pin_.length()) {
      d.fillCircle(cx, 280, 26, ui::kColorText);
    } else {
      d.drawCircle(cx, 280, 26, ui::kColorDim);
    }
  }
  if (pin_.locked(now_ms)) {
    std::snprintf(buf, sizeof(buf), "5 回間違えたので %u 秒お待ちください",
                  static_cast<unsigned>((pin_.lockRemainingMs(now_ms) + 999) / 1000));
    ui::drawTextCenter(d, ui::kScreenW / 2, 360, buf, TextSize::Medium, ui::kColorAlert, ui::kColorBg);
  } else if (message_[0] != '\0') {
    ui::drawTextCenter(d, ui::kScreenW / 2, 360, message_, TextSize::Medium,
                       message_error_ ? ui::kColorAlert : ui::kColorOk, ui::kColorBg);
  }
  drawPad();
  d.endWrite();
}

void SettingsUi::drawMenu() {
  auto& d = M5.Display;
  const Config& c = actions_.currentConfig();
  char summary[64];
  d.startWrite();
  drawTitle("設定", "閉じる");
  for (uint8_t i = 0; i < kItemCount; ++i) {
    const int y = kMenuY + i * kMenuRowH;
    d.fillRoundRect(ui::kMargin / 2, y + 3, ui::kScreenW - ui::kMargin, kMenuRowH - 8, 10, ui::kColorPanel);
    ui::drawText(d, ui::kMargin, y + 24, kItemLabels[i], TextSize::Medium, ui::kColorText, ui::kColorPanel);
    summary[0] = '\0';
    uint32_t remaining = 0;
    switch (i) {
      case kItemPause:
        if (actions_.warningsPaused(&remaining)) {
          std::snprintf(summary, sizeof(summary), "停止中 残り %u 分", static_cast<unsigned>((remaining + 59999) / 60000));
        } else {
          std::snprintf(summary, sizeof(summary), "有効");
        }
        break;
      case kItemClock: {
        LocalDateTime t;
        if (actions_.timeLost() || !toLocalDateTime(actions_.nowEpoch(), c.tz_offset_min, &t)) {
          std::snprintf(summary, sizeof(summary), "未設定");
        } else {
          std::snprintf(summary, sizeof(summary), "%d/%d %02d:%02d", t.month, t.day, t.hour, t.minute);
        }
        break;
      }
      case kItemBrightness:
        std::snprintf(summary, sizeof(summary), "%u%%", static_cast<unsigned>(c.brightness_pct));
        break;
      case kItemVolume:
        std::snprintf(summary, sizeof(summary), "操作音 %u%% / 警告 %u・%u%%", static_cast<unsigned>(c.volume_operation_pct),
                      static_cast<unsigned>(c.volume_warning1_pct), static_cast<unsigned>(c.volume_warning2_pct));
        break;
      case kItemWarning:
        std::snprintf(summary, sizeof(summary), "%u / %u / %u 秒", static_cast<unsigned>(c.warning1_sec),
                      static_cast<unsigned>(c.warning2_sec), static_cast<unsigned>(c.giveup_sec));
        break;
      case kItemPhoto:
        std::snprintf(summary, sizeof(summary), "%s", !c.camera_enabled      ? "カメラを使わない"
                                                      : c.save_photos_to_sd ? "カメラ・SD に保存"
                                                                            : "カメラ (SD に保存しない)");
        break;
      case kItemTof:
        std::snprintf(summary, sizeof(summary), "%s", !c.drawer_sensor_enabled        ? "使わない (名札で撮影)"
                                                      : actions_.drawerSensorPresent() ? "使う"
                                                                                       : "使う (見つかりません)");
        break;
      case kItemUser:
        std::snprintf(summary, sizeof(summary), "%u %s", static_cast<unsigned>(c.user_id_digits),
                      c.user_id_alnum ? "文字 (英数字)" : "桁 (数字)");
        break;
      case kItemCancel:
        std::snprintf(summary, sizeof(summary), "赤帯 %u 件 / 貸出中 %u 件", static_cast<unsigned>(log_.alertCount()),
                      static_cast<unsigned>(book_.loanCount()));
        break;
      case kItemAbout:
        std::snprintf(summary, sizeof(summary), "版 %s", about::kFirmwareVersion);
        break;
      default:
        break;
    }
    if (summary[0] != '\0') {
      ui::drawTextRight(d, ui::kScreenW - ui::kMargin * 2, y + 28, summary, TextSize::Small, ui::kColorDim,
                        ui::kColorPanel);
    }
  }
  d.endWrite();
}

void SettingsUi::drawEditor() {
  auto& d = M5.Display;
  const EditorSpec& spec = kEditors[editor_];
  char buf[64];
  d.startWrite();
  drawTitle(spec.title, "戻る");
  for (uint8_t i = 0; i < spec.count; ++i) {
    const FieldSpec& f = spec.fields[i];
    const int y = kFieldY + i * kFieldH;
    d.fillRoundRect(ui::kMargin / 2, y + 4, ui::kScreenW - ui::kMargin, kFieldH - 12, 12, ui::kColorPanel);
    ui::drawText(d, ui::kMargin, y + 16, f.label, TextSize::Medium, ui::kColorText, ui::kColorPanel);
    const int32_t v = fieldValue(draft_, f.name);
    if (f.toggle) {
      if (std::strcmp(f.name, "save_photos_to_sd") == 0) {
        std::snprintf(buf, sizeof(buf), "%s", v ? (actions_.sdPresent() ? "ON" : "ON (いまは SD なし)") : "OFF");
      } else {
        std::snprintf(buf, sizeof(buf), "%s", v ? "使う" : "使わない");
      }
      drawButton(d, kMinusX, y + 58, kStepW * 2 + 20, kStepH, "切り替え", TextSize::Medium);
    } else {
      std::snprintf(buf, sizeof(buf), "%ld %s", static_cast<long>(v), f.unit);
      drawButton(d, kMinusX, y + 58, kStepW, kStepH, "-", TextSize::Large);
      drawButton(d, kPlusX, y + 58, kStepW, kStepH, "+", TextSize::Large);
    }
    ui::drawText(d, ui::kMargin, y + 76, buf, TextSize::Large, ui::kColorAccent, ui::kColorPanel);
    if (spec.extra == Extra::Sound) drawButton(d, kTestX, y + 8, kTestW, kTestH, "試す", TextSize::Small);
  }
  const int extra_y = kFieldY + spec.count * kFieldH + 8;
  if (spec.extra == Extra::Photo) {
    if (actions_.sdPresent()) {
      std::snprintf(buf, sizeof(buf), "SD の写真 %u / 100 枚", static_cast<unsigned>(actions_.sdPhotoCount()));
    } else {
      std::snprintf(buf, sizeof(buf), "SD なし (写真は保存されません)");
    }
    ui::drawText(d, ui::kMargin, extra_y + 20, buf, TextSize::Small, ui::kColorDim, ui::kColorBg);
    drawButton(d, kMinusX, extra_y, kStepW * 2 + 20, kStepH, "テスト撮影", TextSize::Medium);
  } else if (spec.extra == Extra::Pin) {
    ui::drawText(d, ui::kMargin, extra_y + 24, "設定画面の PIN (数字 4 桁)", TextSize::Small, ui::kColorDim,
                 ui::kColorBg);
    drawButton(d, kMinusX, extra_y, kStepW * 2 + 20, kStepH, "PIN を変える", TextSize::Medium);
    ui::drawText(d, ui::kMargin, extra_y + kStepH + 24, "英字・記号も使うと、この文字数の物品番号は", TextSize::Small,
                 ui::kColorDim, ui::kColorBg);
    ui::drawText(d, ui::kMargin, extra_y + kStepH + 60, "利用者 ID として読みます", TextSize::Small, ui::kColorDim,
                 ui::kColorBg);
  } else if (spec.extra == Extra::Drawer) {
    // M6: 状態・今の距離・基準・校正の結果は drawDrawerLive (この区画だけ kLiveMs ごとに描き直す)
    pollTofCalibration();
    drawDrawerLive(extra_y);
    drawButton(d, kMinusX, extra_y + 40, kStepW * 2 + 20, kStepH, "基準を取る", TextSize::Medium);
    ui::drawText(d, ui::kMargin, extra_y + 224, "全段を閉めて前に立たずに「基準を取る」→「保存」", TextSize::Small,
                 ui::kColorDim, ui::kColorBg);
    // いま選んでいる方の説明を明るく出す (保存するまでは draft_)
    const bool on = draft_.drawer_sensor_enabled;
    ui::drawText(d, ui::kMargin, extra_y + 290, "使う: 開放で撮影し、無登録の開放を警告します", TextSize::Small,
                 on ? ui::kColorText : ui::kColorDim, ui::kColorBg);
    ui::drawText(d, ui::kMargin, extra_y + 330, "使わない: 物品と名札の QR だけで記録します", TextSize::Small,
                 on ? ui::kColorDim : ui::kColorText, ui::kColorBg);
    ui::drawText(d, ui::kMargin + 28, extra_y + 370, "写真は名札を読んだときに撮ります", TextSize::Small,
                 on ? ui::kColorDim : ui::kColorText, ui::kColorBg);
    ui::drawText(d, ui::kMargin, extra_y + 430, "この画面を開いている間は開閉を判定しません", TextSize::Small,
                 ui::kColorDim, ui::kColorBg);
  }
  drawMessage(kSaveY - 48);
  drawButton(d, ui::kMargin, kSaveY, ui::kScreenW - ui::kMargin * 2, kSaveH, "保存", TextSize::Large, ui::kColorTabActive);
  d.endWrite();
}

void SettingsUi::drawPause() {
  auto& d = M5.Display;
  char buf[96];
  uint32_t remaining = 0;
  const bool paused = actions_.warningsPaused(&remaining);
  d.startWrite();
  drawTitle("警告の一時停止", "戻る");
  if (paused) {
    const uint32_t s = (remaining + 999) / 1000;
    std::snprintf(buf, sizeof(buf), "一時停止中 残り %u 分 %02u 秒", static_cast<unsigned>(s / 60),
                  static_cast<unsigned>(s % 60));
    ui::drawText(d, ui::kMargin, kBodyY + 40, buf, TextSize::Large, ui::kColorWarn, ui::kColorBg);
  } else {
    ui::drawText(d, ui::kMargin, kBodyY + 40, "警告は有効です", TextSize::Large, ui::kColorOk, ui::kColorBg);
  }
  ui::drawText(d, ui::kMargin, kBodyY + 110, "補充や棚卸しで開け閉めするときに、警告を止めます", TextSize::Small,
               ui::kColorDim, ui::kColorBg);
  ui::drawText(d, ui::kMargin, kBodyY + 150, "QR の登録はできます。時間が来たら自動で戻ります", TextSize::Small,
               ui::kColorDim, ui::kColorBg);
  drawButton(d, 40, 460, 200, 140, "10 分", TextSize::Large);
  drawButton(d, 260, 460, 200, 140, "30 分", TextSize::Large);
  drawButton(d, 480, 460, 200, 140, "60 分", TextSize::Large);
  if (paused) drawButton(d, 40, 640, 640, 120, "今すぐ再開", TextSize::Large, ui::kColorTabActive);
  drawMessage(800);
  d.endWrite();
}

void SettingsUi::drawClock() {
  auto& d = M5.Display;
  char buf[32];
  static const char* const kColLabels[5] = {"年", "月", "日", "時", "分"};
  d.startWrite();
  drawTitle("時刻", "戻る");
  if (actions_.timeLost()) {
    ui::drawText(d, ui::kMargin, kBodyY + 20, "いまの時刻は未設定です。合わせてください", TextSize::Small, ui::kColorAlert,
                 ui::kColorBg);
  } else {
    ui::drawText(d, ui::kMargin, kBodyY + 20, "現地の時刻を合わせて「時刻を合わせる」を押します", TextSize::Small,
                 ui::kColorDim, ui::kColorBg);
  }
  const int values[5] = {clock_draft_.year, clock_draft_.month, clock_draft_.day, clock_draft_.hour,
                         clock_draft_.minute};
  for (int i = 0; i < 5; ++i) {
    const int x = kColX0 + i * (kColW + kColGap);
    ui::drawTextCenter(d, x + kColW / 2, kUpY - 44, kColLabels[i], TextSize::Medium, ui::kColorDim, ui::kColorBg);
    drawButton(d, x, kUpY, kColW, kArrowH, "▲", TextSize::Large);
    std::snprintf(buf, sizeof(buf), i == 0 ? "%04d" : "%02d", values[i]);
    ui::drawTextCenter(d, x + kColW / 2, kValueY + 20, buf, TextSize::Large, ui::kColorAccent, ui::kColorBg);
    drawButton(d, x, kDownY, kColW, kArrowH, "▼", TextSize::Large);
  }
  ui::drawText(d, ui::kMargin, kTzY, "UTC からの時差", TextSize::Medium, ui::kColorText, ui::kColorBg);
  const int tz = tz_draft_;
  const int tz_abs = tz < 0 ? -tz : tz;
  std::snprintf(buf, sizeof(buf), "%c%d:%02d", tz < 0 ? '-' : '+', tz_abs / 60, tz_abs % 60);
  ui::drawText(d, ui::kMargin, kTzY + 60, buf, TextSize::Large, ui::kColorAccent, ui::kColorBg);
  drawButton(d, kMinusX, kTzY + 40, kStepW, kStepH, "-", TextSize::Large);
  drawButton(d, kPlusX, kTzY + 40, kStepW, kStepH, "+", TextSize::Large);
  drawMessage(kSaveY - 48);
  drawButton(d, ui::kMargin, kSaveY, ui::kScreenW - ui::kMargin * 2, kSaveH, "時刻を合わせる", TextSize::Large,
             ui::kColorTabActive);
  d.endWrite();
}

void SettingsUi::drawCancel() {
  auto& d = M5.Display;
  const Config& c = actions_.currentConfig();
  char buf[96];
  char when[24];
  d.startWrite();
  drawTitle("赤帯・貸出の取消", "戻る");
  std::snprintf(buf, sizeof(buf), "赤帯 %u 件", static_cast<unsigned>(log_.alertCount()));
  drawButton(d, ui::kMargin, kSubTabY, kSubTabW, kSubTabH, buf, TextSize::Medium,
             cancel_alerts_ ? ui::kColorTabActive : ui::kColorPanel);
  std::snprintf(buf, sizeof(buf), "貸出中 %u 件", static_cast<unsigned>(book_.loanCount()));
  drawButton(d, ui::kMargin + kSubTabW + 20, kSubTabY, kSubTabW, kSubTabH, buf, TextSize::Medium,
             cancel_alerts_ ? ui::kColorPanel : ui::kColorTabActive);

  size_t count = 0;
  if (cancel_alerts_) {
    count = log_.alertCount();
    if (cancel_page_ >= pageCountOf(count)) cancel_page_ = static_cast<uint16_t>(pageCountOf(count) - 1);
    for (uint16_t i = 0; i < kListRows; ++i) {
      const uint16_t index = static_cast<uint16_t>(cancel_page_ * kListRows + i);
      Alert a;
      if (index >= count || !log_.getAlert(index, &a)) break;
      const int y = kListY + i * kListRowH;
      d.fillRoundRect(ui::kMargin / 2, y + 3, ui::kScreenW - ui::kMargin, kListRowH - 8, 10, ui::kColorPanel);
      formatShortDateTime(a.epoch, c.tz_offset_min, when, sizeof(when));
      ui::drawText(d, ui::kMargin, y + 14, when, TextSize::Medium, ui::kColorText, ui::kColorPanel);
      ui::drawText(d, ui::kMargin, y + 64, a.unregistered_open ? "登録せずに持ち出された" : "利用者不明の持出",
                   TextSize::Small, ui::kColorAlert, ui::kColorPanel);
      drawButton(d, kActionX, y + 16, kActionW, kActionH, "確認して消す", TextSize::Small);
    }
    if (count == 0) {
      ui::drawText(d, ui::kMargin, kListY + 20, "未確認の赤帯はありません", TextSize::Medium, ui::kColorDim, ui::kColorBg);
    }
    if (log_.droppedAlerts() > 0) {
      std::snprintf(buf, sizeof(buf), "20 件を超えて消えた赤帯 %u 件 (全部確認すると 0 に戻ります)",
                    static_cast<unsigned>(log_.droppedAlerts()));
      ui::drawText(d, ui::kMargin, kPagerY + kPagerH + 8, buf, TextSize::Small, ui::kColorWarn, ui::kColorBg);
    }
  } else {
    std::vector<Loan> loans(kMaxLoans);
    count = book_.listLoans(loans.data(), loans.size());
    if (cancel_page_ >= pageCountOf(count)) cancel_page_ = static_cast<uint16_t>(pageCountOf(count) - 1);
    for (uint16_t i = 0; i < kListRows; ++i) {
      const size_t index = static_cast<size_t>(cancel_page_) * kListRows + i;
      if (index >= count) break;
      const Loan& l = loans[index];
      const int y = kListY + i * kListRowH;
      const bool pending = std::strcmp(pending_cancel_, l.item) == 0;
      d.fillRoundRect(ui::kMargin / 2, y + 3, ui::kScreenW - ui::kMargin, kListRowH - 8, 10, ui::kColorPanel);
      // 物品番号は「取消」のボタンの手前まで (入らなければ縮める)
      ui::drawTextFit(d, ui::kMargin, y + 10, kActionX - 16 - ui::kMargin, l.item, TextSize::Large, ui::kColorText,
                      ui::kColorPanel);
      formatShortDateTime(l.checkout_epoch, c.tz_offset_min, when, sizeof(when));
      std::snprintf(buf, sizeof(buf), "%s  %s 持出", l.user[0] ? l.user : "利用者不明", when);
      ui::drawText(d, ui::kMargin, y + 66, buf, TextSize::Small, ui::kColorAccent, ui::kColorPanel);
      drawButton(d, kActionX, y + 16, kActionW, kActionH, pending ? "もう一度で取消" : "取消", TextSize::Small,
                 pending ? ui::kColorAlert : ui::kColorButton);
    }
    if (count == 0) {
      ui::drawText(d, ui::kMargin, kListY + 20, "貸出中の物品はありません", TextSize::Medium, ui::kColorDim, ui::kColorBg);
    }
  }
  const uint16_t pages = pageCountOf(count);
  if (pages > 1) {
    drawButton(d, ui::kMargin, kPagerY, 200, kPagerH, "前へ", TextSize::Medium,
               cancel_page_ > 0 ? ui::kColorButton : ui::kColorPanel);
    drawButton(d, ui::kScreenW - ui::kMargin - 200, kPagerY, 200, kPagerH, "次へ", TextSize::Medium,
               cancel_page_ + 1 < pages ? ui::kColorButton : ui::kColorPanel);
    std::snprintf(buf, sizeof(buf), "%u / %u", static_cast<unsigned>(cancel_page_ + 1), static_cast<unsigned>(pages));
    ui::drawTextCenter(d, ui::kScreenW / 2, kPagerY + 26, buf, TextSize::Medium, ui::kColorText, ui::kColorBg);
  }
  drawMessage(ui::kScreenH - 44);
  d.endWrite();
}

void SettingsUi::drawExport() {
  auto& d = M5.Display;
  d.startWrite();
  drawTitle("USB シリアル出力", "戻る");
  ui::drawText(d, ui::kMargin, kBodyY + 24, "PC を USB でつなぎ、記録を CSV で出します", TextSize::Small, ui::kColorDim,
               ui::kColorBg);
  ui::drawText(d, ui::kMargin, kBodyY + 64, "(115200bps。tools/serial.ps1 などで受け取ります)", TextSize::Small,
               ui::kColorDim, ui::kColorBg);
  for (int i = 0; i < 6; ++i) {
    const int col = i % 2;
    const int row = i / 2;
    drawButton(d, ui::kMargin + col * (kExportW + kExportGap), kExportY + row * (kExportH + kExportGap), kExportW,
               kExportH, kExportLabels[i], TextSize::Large);
  }
  drawMessage(kExportY + 3 * (kExportH + kExportGap) + 80);
  d.endWrite();
}

void SettingsUi::drawErase() {
  auto& d = M5.Display;
  char buf[96];
  d.startWrite();
  drawTitle("データ削除", "戻る");
  std::snprintf(buf, sizeof(buf), "貸出中 %u 件 / 返却履歴 %u 件", static_cast<unsigned>(book_.loanCount()),
                static_cast<unsigned>(book_.returnCount()));
  ui::drawText(d, ui::kMargin, kBodyY + 24, buf, TextSize::Medium, ui::kColorText, ui::kColorBg);
  std::snprintf(buf, sizeof(buf), "開閉ログ %u 件 / 赤帯 %u 件", static_cast<unsigned>(log_.count()),
                static_cast<unsigned>(log_.alertCount()));
  ui::drawText(d, ui::kMargin, kBodyY + 74, buf, TextSize::Medium, ui::kColorText, ui::kColorBg);
  ui::drawText(d, ui::kMargin, kBodyY + 140, "先に「USB シリアル出力」で控えを取ってください", TextSize::Small,
               ui::kColorWarn, ui::kColorBg);
  ui::drawText(d, ui::kMargin, kBodyY + 180, "記録の削除・全削除では SD の写真は消えません", TextSize::Small,
               ui::kColorWarn, ui::kColorBg);
  ui::drawText(d, ui::kMargin, kBodyY + 220, "消したものは元に戻せません", TextSize::Small, ui::kColorWarn,
               ui::kColorBg);
  char photo_note[64];
  if (actions_.sdPresent()) {
    std::snprintf(photo_note, sizeof(photo_note), "写真 %u 枚。3 秒押し続けると消します",
                  static_cast<unsigned>(actions_.sdPhotoCount()));
  } else {
    std::snprintf(photo_note, sizeof(photo_note), "SD がありません");
  }
  const struct {
    int y;
    const char* label;
    const char* note;
    uint8_t target;
  } buttons[3] = {{kEraseRecordsY, "記録の削除", "設定は残します。3 秒押し続けると消します", 1},
                  {kEraseAllY, "全削除", "設定も消して再起動。3 秒押し続けると消します", 2},  // 長いとボタンからはみ出した
                  {kErasePhotosY, "SD の写真を全部消す", photo_note, 3}};  // 2026-09-14。開閉ログに残す
  for (const auto& b : buttons) {
    d.fillRoundRect(kWideX, b.y, kWideW, kEraseH, 14, ui::kColorAlertBg);
    ui::drawTextCenter(d, ui::kScreenW / 2, b.y + 36, b.label, TextSize::Large, ui::kColorText, ui::kColorAlertBg);
    ui::drawTextCenter(d, ui::kScreenW / 2, b.y + 96, b.note, TextSize::Small, ui::kColorText, ui::kColorAlertBg);
    if (hold_target_ == b.target && hold_ms_ > 0) {
      const uint32_t ms = hold_ms_ < kEraseHoldMs ? hold_ms_ : kEraseHoldMs;
      const int w = static_cast<int>(static_cast<uint64_t>(kWideW - 20) * ms / kEraseHoldMs);
      d.fillRect(kWideX + 10, b.y + kEraseH - 26, w, 16, ui::kColorWarn);
    }
  }
  drawMessage(kErasePhotosY + kEraseH + 20);
  d.endWrite();
}

void SettingsUi::drawInfo() {
  auto& d = M5.Display;
  char lines[kInfoMaxLines][kInfoLineLen];
  const size_t n = actions_.deviceInfo(lines, kInfoMaxLines);
  d.startWrite();
  drawTitle("機器情報", "戻る");
  for (size_t i = 0; i < n; ++i) {
    ui::drawText(d, ui::kMargin, kBodyY + 20 + static_cast<int>(i) * 64, lines[i], TextSize::Small, ui::kColorText,
                 ui::kColorBg);
  }
  d.endWrite();
}

void SettingsUi::drawPinChange() {
  auto& d = M5.Display;
  d.startWrite();
  drawTitle("PIN を変える", "戻る");
  ui::drawTextCenter(d, ui::kScreenW / 2, kBodyY + 16,
                     pin_step_ == 0 ? "新しい PIN (数字 4 桁) を入力" : "確認のため、もう一度入力", TextSize::Medium,
                     ui::kColorText, ui::kColorBg);
  const int dots_x = ui::kScreenW / 2 - 150;
  for (int i = 0; i < kPinLength; ++i) {
    const int cx = dots_x + i * 100;
    if (i < new_len_) {
      d.fillCircle(cx, 300, 26, ui::kColorText);
    } else {
      d.drawCircle(cx, 300, 26, ui::kColorDim);
    }
  }
  if (message_[0] != '\0') {
    ui::drawTextCenter(d, ui::kScreenW / 2, 370, message_, TextSize::Small, message_error_ ? ui::kColorAlert : ui::kColorOk,
                       ui::kColorBg);
  }
  drawPad();
  d.endWrite();
}

// このソフトについて: 製品名・版数・リリース日・開発者・ライセンス・公開リポジトリの URL と QR
void SettingsUi::drawAbout() {
  auto& d = M5.Display;
  const int full_w = ui::kScreenW - ui::kMargin * 2;
  char buf[96];
  d.startWrite();
  drawTitle("このソフトについて", "戻る");
  ui::drawText(d, ui::kMargin, kBodyY + 24, about::kProductName, TextSize::Large, ui::kColorAccent, ui::kColorBg);
  const char* const labels[4] = {"版数", "リリース日", "開発者", "ライセンス"};
  const char* const values[4] = {about::kFirmwareVersion, about::kReleaseDate, about::kDeveloper, about::kLicenseName};
  for (int i = 0; i < 4; ++i) {
    std::snprintf(buf, sizeof(buf), "%s  %s", labels[i], values[i]);
    ui::drawTextFit(d, ui::kMargin, kBodyY + 90 + i * 60, full_w, buf, TextSize::Medium, ui::kColorText, ui::kColorBg);
  }
  ui::drawText(d, ui::kMargin, kBodyY + 340, "GitHub", TextSize::Small, ui::kColorDim, ui::kColorBg);
  ui::drawTextFit(d, ui::kMargin, kBodyY + 376, full_w, about::kRepoUrl, TextSize::Small, ui::kColorText, ui::kColorBg);
  d.qrcode(about::kRepoUrl, (ui::kScreenW - kAboutQrW) / 2, kAboutQrY, kAboutQrW, 1, true);
  drawButton(d, kWideX, kSaveY, kWideW, kSaveH, "ライセンス全文", TextSize::Medium);
  d.endWrite();
}

// ライセンスの節の一覧 (全文を載せないものは下に注記)
void SettingsUi::drawLicenses() {
  auto& d = M5.Display;
  d.startWrite();
  drawTitle("ライセンス", "戻る");
  for (size_t i = 0; i < kLicenseSectionCount; ++i) {
    const int y = kLicenseButtonY + static_cast<int>(i) * (kLicenseButtonH + kLicenseButtonGap);
    d.fillRoundRect(kWideX, y, kWideW, kLicenseButtonH, 12, ui::kColorButton);
    ui::drawTextFit(d, kWideX + 24, y + (kLicenseButtonH - 32) / 2, kWideW - 48, kLicenseSections[i].title,
                    TextSize::Medium, ui::kColorText, ui::kColorButton);
  }
  int y = kLicenseButtonY + static_cast<int>(kLicenseSectionCount) * (kLicenseButtonH + kLicenseButtonGap) + 20;
  for (size_t i = 0; i < kLicenseNoteLineCount; ++i) {
    ui::drawTextFit(d, ui::kMargin, y, ui::kScreenW - ui::kMargin * 2, kLicenseNoteLines[i], TextSize::Small,
                    ui::kColorDim, ui::kColorBg);
    y += 40;
  }
  d.endWrite();
}

// 選んだ節の全文をページで
void SettingsUi::drawLicenseText() {
  auto& d = M5.Display;
  const LicenseSection& sec = kLicenseSections[license_section_];
  d.startWrite();
  drawTitle("", "戻る");
  ui::drawTextFit(d, ui::kMargin, kTitleY + 25, kBackX - ui::kMargin - 16, sec.title, TextSize::Large, ui::kColorText,
                  ui::kColorGuideBg);
  const size_t lines = license_pager_.lineCount();
  const size_t pages = TextPager::pageCount(lines, kLicenseLinesPerPage);
  const size_t first = TextPager::firstLineOfPage(license_page_, kLicenseLinesPerPage);
  char buf[512];
  for (size_t i = 0; i < kLicenseLinesPerPage && first + i < lines; ++i) {
    const TextLine l = license_pager_.line(first + i);
    const size_t n = l.length < sizeof(buf) - 1 ? l.length : sizeof(buf) - 1;
    std::memcpy(buf, sec.text + l.offset, n);
    buf[n] = '\0';
    for (size_t k = 0; k < n; ++k) {
      if (buf[k] == '\n' || buf[k] == '\r') buf[k] = ' ';  // つないだ改行は空白として描く
    }
    ui::drawText(d, ui::kMargin, kLicenseTextY + static_cast<int>(i) * kLicenseLineH, buf, TextSize::Small,
                 ui::kColorText, ui::kColorBg);
  }
  drawButton(d, ui::kMargin, kLicensePagerY, 200, kPagerH, "前へ", TextSize::Medium,
             license_page_ > 0 ? ui::kColorButton : ui::kColorPanel);
  drawButton(d, ui::kScreenW - ui::kMargin - 200, kLicensePagerY, 200, kPagerH, "次へ", TextSize::Medium,
             license_page_ + 1 < pages ? ui::kColorButton : ui::kColorPanel);
  std::snprintf(buf, sizeof(buf), "%u / %u", static_cast<unsigned>(license_page_ + 1), static_cast<unsigned>(pages));
  ui::drawTextCenter(d, ui::kScreenW / 2, kLicensePagerY + 26, buf, TextSize::Medium, ui::kColorText, ui::kColorBg);
  d.endWrite();
}

void SettingsUi::drawNotYet() {
  auto& d = M5.Display;
  d.startWrite();
  drawTitle(kItemLabels[item_], "戻る");
  ui::drawText(d, ui::kMargin, kBodyY + 40, "この項目の画面はまだありません", TextSize::Medium, ui::kColorDim,
               ui::kColorBg);
  d.endWrite();
}

// --- タッチ ---

bool SettingsUi::backPressed(int x, int y) const { return inRect(x, y, kBackX, kTitleY + 13, kBackW, kBackH); }

int SettingsUi::padKey(int x, int y) const {
  if (x < kPadX0 || y < kPadY0) return -1;
  const int col = (x - kPadX0) / (kPadW + kPadGap);
  const int row = (y - kPadY0) / (kPadH + kPadGap);
  if (col >= 3 || row >= 4) return -1;
  if (!inRect(x, y, kPadX0 + col * (kPadW + kPadGap), kPadY0 + row * (kPadH + kPadGap), kPadW, kPadH)) return -1;
  return row * 3 + col;
}

SettingsResult SettingsUi::touchPin(int x, int y, uint32_t now_ms) {
  if (backPressed(x, y)) return SettingsResult::Close;
  const int key = padKey(x, y);
  if (key < 0) return SettingsResult::None;
  if (key == kPadBack) return SettingsResult::Close;
  if (key == kPadDelete) {
    pin_.backspace();
    return SettingsResult::Handled;
  }
  switch (pin_.pressDigit(kPadLabels[key][0], actions_.currentConfig().pin, now_ms)) {
    case PinResult::Accepted:
      setMessage("", false);
      page_ = Page::Menu;
      break;
    case PinResult::Rejected: {
      char buf[96];
      if (pin_.locked(now_ms)) {
        std::snprintf(buf, sizeof(buf), "PIN が違います");
      } else {
        std::snprintf(buf, sizeof(buf), "PIN が違います (あと %u 回で 1 分ロック)",
                      static_cast<unsigned>(kPinMaxFailures - pin_.failures()));
      }
      setMessage(buf, true);
      break;
    }
    case PinResult::Locked:
    case PinResult::Incomplete:
      break;
  }
  return SettingsResult::Handled;
}

SettingsResult SettingsUi::touchAbout(int x, int y) {
  if (backPressed(x, y)) {
    page_ = Page::Menu;
    return SettingsResult::Handled;
  }
  if (inRect(x, y, kWideX, kSaveY, kWideW, kSaveH)) {
    page_ = Page::Licenses;
    return SettingsResult::Handled;
  }
  return SettingsResult::None;
}

SettingsResult SettingsUi::touchLicenses(int x, int y) {
  if (backPressed(x, y)) {
    page_ = Page::About;
    return SettingsResult::Handled;
  }
  for (size_t i = 0; i < kLicenseSectionCount; ++i) {
    const int top = kLicenseButtonY + static_cast<int>(i) * (kLicenseButtonH + kLicenseButtonGap);
    if (inRect(x, y, kWideX, top, kWideW, kLicenseButtonH)) {
      openLicense(static_cast<uint8_t>(i));
      return SettingsResult::Handled;
    }
  }
  return SettingsResult::None;
}

SettingsResult SettingsUi::touchLicenseText(int x, int y) {
  if (backPressed(x, y)) {
    page_ = Page::Licenses;
    return SettingsResult::Handled;
  }
  const size_t pages = TextPager::pageCount(license_pager_.lineCount(), kLicenseLinesPerPage);
  if (inRect(x, y, ui::kMargin, kLicensePagerY, 200, kPagerH)) {
    if (license_page_ == 0) return SettingsResult::None;
    --license_page_;
    return SettingsResult::Handled;
  }
  if (inRect(x, y, ui::kScreenW - ui::kMargin - 200, kLicensePagerY, 200, kPagerH)) {
    if (license_page_ + 1u >= pages) return SettingsResult::None;
    ++license_page_;
    return SettingsResult::Handled;
  }
  return SettingsResult::None;
}

// 節を開く。折り返しはここで 1 回だけ計算する
void SettingsUi::openLicense(uint8_t section) {
  if (section >= kLicenseSectionCount) return;
  license_section_ = section;
  license_page_ = 0;
  const LicenseSection& sec = kLicenseSections[section];
  GlyphMeasure m;
  m.gfx = &M5.Display;
  for (auto& w : m.ascii) w = -1;
  const uint32_t started = millis();
  // 英文のライセンスは 80 桁ほどで改行してあるので、つないで画面の幅で流し込む (文字は変えない)
  license_pager_.layout(sec.text, std::strlen(sec.text), ui::kScreenW - ui::kMargin * 2 - 8, &measureGlyph, &m, true);
  Serial.printf("[About] %s を折り返しました: %u バイト・%u 行・%u ページ・%lu ms\n", sec.title,
                static_cast<unsigned>(std::strlen(sec.text)), static_cast<unsigned>(license_pager_.lineCount()),
                static_cast<unsigned>(TextPager::pageCount(license_pager_.lineCount(), kLicenseLinesPerPage)),
                static_cast<unsigned long>(millis() - started));
  page_ = Page::LicenseText;
}

SettingsResult SettingsUi::touchMenu(int x, int y) {
  if (backPressed(x, y)) return SettingsResult::Close;
  if (y < kMenuY) return SettingsResult::None;
  const int index = (y - kMenuY) / kMenuRowH;
  if (index < 0 || index >= kItemCount) return SettingsResult::None;
  openItem(static_cast<uint8_t>(index));
  return SettingsResult::Handled;
}

SettingsResult SettingsUi::touchEditor(int x, int y) {
  const EditorSpec& spec = kEditors[editor_];
  const Config& current = actions_.currentConfig();
  if (backPressed(x, y)) {
    if (editor_ == kEdBrightness) actions_.previewBrightness(current.brightness_pct);  // 保存しなかった明るさを戻す
    setMessage("", false);
    page_ = Page::Menu;
    return SettingsResult::Handled;
  }
  if (inRect(x, y, ui::kMargin, kSaveY, ui::kScreenW - ui::kMargin * 2, kSaveH)) {
    ConfigError error = ConfigError::None;
    if (actions_.applySettings(draft_, &error)) {
      setMessage("保存しました", false);
      page_ = Page::Menu;
    } else {
      setMessage(error != ConfigError::None ? describeConfigError(error) : "保存できませんでした", true);
    }
    return SettingsResult::Handled;
  }
  for (uint8_t i = 0; i < spec.count; ++i) {
    const FieldSpec& f = spec.fields[i];
    const int row_y = kFieldY + i * kFieldH;
    int32_t v = fieldValue(draft_, f.name);
    bool changed = false;
    if (f.toggle && inRect(x, y, kMinusX, row_y + 58, kStepW * 2 + 20, kStepH)) {
      v = v ? 0 : 1;
      changed = true;
    } else if (!f.toggle && inRect(x, y, kMinusX, row_y + 58, kStepW, kStepH)) {
      v = v - f.step < f.min ? f.min : v - f.step;
      changed = true;
    } else if (!f.toggle && inRect(x, y, kPlusX, row_y + 58, kStepW, kStepH)) {
      v = v + f.step > f.max ? f.max : v + f.step;
      changed = true;
    } else if (spec.extra == Extra::Sound && inRect(x, y, kTestX, row_y + 8, kTestW, kTestH)) {
      actions_.testSound(i, static_cast<uint8_t>(v));
      return SettingsResult::Handled;
    }
    if (changed) {
      setFieldValue(&draft_, f.name, v);
      if (std::strcmp(f.name, "brightness_pct") == 0) actions_.previewBrightness(static_cast<uint8_t>(v));
      setMessage("", false);
      return SettingsResult::Handled;
    }
  }
  const int extra_y = kFieldY + spec.count * kFieldH + 8;
  if (spec.extra == Extra::Photo && inRect(x, y, kMinusX, extra_y, kStepW * 2 + 20, kStepH)) {
    actions_.takeTestPhoto();  // 撮れたら写真の画面に切り替わる
    return SettingsResult::Handled;
  }
  if (spec.extra == Extra::Drawer && inRect(x, y, kMinusX, extra_y + 40, kStepW * 2 + 20, kStepH)) {
    calib_error_ = !actions_.startTofCalibration();
    std::snprintf(calib_text_, sizeof(calib_text_), "%s",
                  calib_error_ ? "基準を取れません: Unit ToF を測っていません (使う設定を保存してから)" : "");
    return SettingsResult::Handled;
  }
  if (spec.extra == Extra::Pin && inRect(x, y, kMinusX, extra_y, kStepW * 2 + 20, kStepH)) {
    pin_step_ = 0;
    new_len_ = 0;
    new_pin_[0] = '\0';
    first_pin_[0] = '\0';
    setMessage("", false);
    page_ = Page::PinChange;
    return SettingsResult::Handled;
  }
  return SettingsResult::None;
}

SettingsResult SettingsUi::touchPause(int x, int y) {
  if (backPressed(x, y)) {
    setMessage("", false);
    page_ = Page::Menu;
    return SettingsResult::Handled;
  }
  const uint32_t minutes[3] = {10, 30, 60};
  const int xs[3] = {40, 260, 480};
  for (int i = 0; i < 3; ++i) {
    if (inRect(x, y, xs[i], 460, 200, 140)) {
      actions_.pauseWarnings(minutes[i]);
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%u 分止めました", static_cast<unsigned>(minutes[i]));
      setMessage(buf, false);
      return SettingsResult::Handled;
    }
  }
  uint32_t remaining = 0;
  if (actions_.warningsPaused(&remaining) && inRect(x, y, 40, 640, 640, 120)) {
    actions_.resumeWarnings();
    setMessage("警告を再開しました", false);
    return SettingsResult::Handled;
  }
  return SettingsResult::None;
}

SettingsResult SettingsUi::touchClock(int x, int y) {
  if (backPressed(x, y)) {
    setMessage("", false);
    page_ = Page::Menu;
    return SettingsResult::Handled;
  }
  if (inRect(x, y, ui::kMargin, kSaveY, ui::kScreenW - ui::kMargin * 2, kSaveH)) {
    int64_t epoch = 0;
    if (!fromLocalDateTime(clock_draft_, tz_draft_, &epoch) || !isValidEpoch(epoch)) {
      setMessage("2026 年より前には合わせられません", true);
      return SettingsResult::Handled;
    }
    Config c = actions_.currentConfig();
    if (c.tz_offset_min != tz_draft_) {
      c.tz_offset_min = tz_draft_;
      ConfigError error = ConfigError::None;
      if (!actions_.applySettings(c, &error)) {
        setMessage(error != ConfigError::None ? describeConfigError(error) : "時差を保存できませんでした", true);
        return SettingsResult::Handled;
      }
    }
    if (actions_.setClock(epoch)) {
      setMessage("時刻を合わせました", false);
      page_ = Page::Menu;
    } else {
      setMessage("RTC に書けませんでした", true);
    }
    return SettingsResult::Handled;
  }
  for (int i = 0; i < 5; ++i) {
    const int cx = kColX0 + i * (kColW + kColGap);
    int delta = 0;
    if (inRect(x, y, cx, kUpY, kColW, kArrowH)) delta = 1;
    if (inRect(x, y, cx, kDownY, kColW, kArrowH)) delta = -1;
    if (delta == 0) continue;
    LocalDateTime& t = clock_draft_;
    switch (i) {
      case 0:
        t.year = t.year + delta < 2026 ? 2026 : (t.year + delta > 2099 ? 2099 : t.year + delta);
        break;
      case 1:
        t.month = wrap(t.month + delta, 1, 12);
        break;
      case 2:
        t.day = wrap(t.day + delta, 1, 31);
        break;
      case 3:
        t.hour = wrap(t.hour + delta, 0, 23);
        break;
      default:
        t.minute = wrap(t.minute + delta, 0, 59);
        break;
    }
    clampDay(&t);
    setMessage("", false);
    return SettingsResult::Handled;
  }
  if (inRect(x, y, kMinusX, kTzY + 40, kStepW, kStepH) && tz_draft_ - 30 >= -720) {
    tz_draft_ = static_cast<int16_t>(tz_draft_ - 30);
    return SettingsResult::Handled;
  }
  if (inRect(x, y, kPlusX, kTzY + 40, kStepW, kStepH) && tz_draft_ + 30 <= 840) {
    tz_draft_ = static_cast<int16_t>(tz_draft_ + 30);
    return SettingsResult::Handled;
  }
  return SettingsResult::None;
}

SettingsResult SettingsUi::touchCancel(int x, int y) {
  if (backPressed(x, y)) {
    setMessage("", false);
    pending_cancel_[0] = '\0';
    page_ = Page::Menu;
    return SettingsResult::Handled;
  }
  if (inRect(x, y, ui::kMargin, kSubTabY, kSubTabW, kSubTabH) ||
      inRect(x, y, ui::kMargin + kSubTabW + 20, kSubTabY, kSubTabW, kSubTabH)) {
    cancel_alerts_ = x < ui::kMargin + kSubTabW + 10;
    cancel_page_ = 0;
    pending_cancel_[0] = '\0';
    setMessage("", false);
    return SettingsResult::Handled;
  }
  const size_t count = cancel_alerts_ ? log_.alertCount() : book_.loanCount();
  const uint16_t pages = pageCountOf(count);
  if (pages > 1 && inRect(x, y, ui::kMargin, kPagerY, 200, kPagerH)) {
    if (cancel_page_ == 0) return SettingsResult::None;
    --cancel_page_;
    pending_cancel_[0] = '\0';
    return SettingsResult::Handled;
  }
  if (pages > 1 && inRect(x, y, ui::kScreenW - ui::kMargin - 200, kPagerY, 200, kPagerH)) {
    if (cancel_page_ + 1 >= pages) return SettingsResult::None;
    ++cancel_page_;
    pending_cancel_[0] = '\0';
    return SettingsResult::Handled;
  }
  if (y < kListY || y >= kListY + kListRows * kListRowH) return SettingsResult::None;
  const int row = (y - kListY) / kListRowH;
  if (!inRect(x, y, kActionX, kListY + row * kListRowH + 16, kActionW, kActionH)) return SettingsResult::None;
  const size_t index = static_cast<size_t>(cancel_page_) * kListRows + static_cast<size_t>(row);
  if (index >= count) return SettingsResult::None;
  char buf[96];
  if (cancel_alerts_) {
    const bool ok = actions_.confirmAlert(static_cast<uint16_t>(index));
    setMessage(ok ? "赤帯を 1 件確認して消しました" : "赤帯を消せませんでした", !ok);
    return SettingsResult::Handled;
  }
  std::vector<Loan> loans(kMaxLoans);
  const size_t n = book_.listLoans(loans.data(), loans.size());
  if (index >= n) return SettingsResult::None;
  const Loan& l = loans[index];
  if (std::strcmp(pending_cancel_, l.item) != 0) {
    std::snprintf(pending_cancel_, sizeof(pending_cancel_), "%s", l.item);
    std::snprintf(buf, sizeof(buf), "%s: もう一度押すと貸出を取り消します", l.item);
    setMessage(buf, false);
    return SettingsResult::Handled;
  }
  char item[kCodeFieldLen];
  std::snprintf(item, sizeof(item), "%s", l.item);
  pending_cancel_[0] = '\0';
  const bool ok = actions_.cancelLoan(item);
  if (ok) {
    std::snprintf(buf, sizeof(buf), "%s の貸出を取り消しました (返却履歴に「取消」で残ります)", item);
  } else {
    std::snprintf(buf, sizeof(buf), "%s の貸出を取り消せませんでした", item);
  }
  setMessage(buf, !ok);
  return SettingsResult::Handled;
}

SettingsResult SettingsUi::touchExport(int x, int y) {
  if (backPressed(x, y)) {
    setMessage("", false);
    page_ = Page::Menu;
    return SettingsResult::Handled;
  }
  for (int i = 0; i < 6; ++i) {
    const int col = i % 2;
    const int row = i / 2;
    if (inRect(x, y, ui::kMargin + col * (kExportW + kExportGap), kExportY + row * (kExportH + kExportGap), kExportW,
               kExportH)) {
      actions_.exportRecords(kExportWhat[i]);
      char buf[96];
      std::snprintf(buf, sizeof(buf), "USB シリアルに「%s」を出しました", kExportLabels[i]);
      setMessage(buf, false);
      return SettingsResult::Handled;
    }
  }
  return SettingsResult::None;
}

SettingsResult SettingsUi::touchPinChange(int x, int y) {
  if (backPressed(x, y)) {
    setMessage("", false);
    page_ = Page::Editor;
    return SettingsResult::Handled;
  }
  const int key = padKey(x, y);
  if (key < 0) return SettingsResult::None;
  if (key == kPadBack) {
    setMessage("", false);
    page_ = Page::Editor;
    return SettingsResult::Handled;
  }
  if (key == kPadDelete) {
    if (new_len_ > 0) new_pin_[--new_len_] = '\0';
    return SettingsResult::Handled;
  }
  if (new_len_ >= kPinLength) return SettingsResult::None;
  new_pin_[new_len_++] = kPadLabels[key][0];
  new_pin_[new_len_] = '\0';
  if (new_len_ < kPinLength) return SettingsResult::Handled;
  if (pin_step_ == 0) {
    std::snprintf(first_pin_, sizeof(first_pin_), "%s", new_pin_);
    pin_step_ = 1;
    new_len_ = 0;
    new_pin_[0] = '\0';
    setMessage("", false);
    return SettingsResult::Handled;
  }
  const bool same = std::strcmp(first_pin_, new_pin_) == 0;
  pin_step_ = 0;
  new_len_ = 0;
  new_pin_[0] = '\0';
  if (!same) {
    setMessage("2 回の PIN が違います。最初から入力してください", true);
    return SettingsResult::Handled;
  }
  Config c = actions_.currentConfig();
  std::memcpy(c.pin, first_pin_, sizeof(c.pin));
  ConfigError error = ConfigError::None;
  if (actions_.applySettings(c, &error)) {
    draft_.pin[0] = '\0';
    std::memcpy(draft_.pin, first_pin_, sizeof(draft_.pin));
    setMessage("PIN を変えました", false);
    page_ = Page::Menu;
  } else {
    setMessage(error != ConfigError::None ? describeConfigError(error) : "PIN を保存できませんでした", true);
  }
  return SettingsResult::Handled;
}

// --- 状態 ---

void SettingsUi::openItem(uint8_t item) {
  item_ = item;
  setMessage("", false);
  switch (item) {
    case kItemPause:
      page_ = Page::Pause;
      break;
    case kItemClock:
      startClockDraft();
      page_ = Page::Clock;
      break;
    case kItemBrightness:
      openEditor(kEdBrightness);
      break;
    case kItemVolume:
      openEditor(kEdVolume);
      break;
    case kItemWarning:
      openEditor(kEdWarning);
      break;
    case kItemPhoto:
      openEditor(kEdPhoto);
      break;
    case kItemUser:
      openEditor(kEdUser);
      break;
    case kItemTof:
      openEditor(kEdDrawer);  // 使う / 使わない・開の閾値・ToF 校正 (M6)
      break;
    case kItemCancel:
      cancel_alerts_ = log_.alertCount() > 0;
      cancel_page_ = 0;
      pending_cancel_[0] = '\0';
      page_ = Page::Cancel;
      break;
    case kItemExport:
      page_ = Page::Export;
      break;
    case kItemErase:
      hold_target_ = 0;
      hold_ms_ = 0;
      hold_fired_ = false;
      page_ = Page::Erase;
      break;
    case kItemInfo:
      page_ = Page::Info;
      break;
    case kItemAbout:
      page_ = Page::About;
      break;
    default:
      page_ = Page::NotYet;  // 項目を足したのに画面が無いとき
      break;
  }
}

void SettingsUi::openEditor(uint8_t editor) {
  editor_ = editor;
  draft_ = actions_.currentConfig();
  page_ = Page::Editor;
  if (editor == kEdDrawer) {
    TofStatus st;
    actions_.tofStatus(&st);
    seen_calibration_seq_ = st.calibration_seq;  // 前に開いたときの結果は入れない
    calib_text_[0] = '\0';
    calib_error_ = false;
  }
}

void SettingsUi::startClockDraft() {
  const Config& c = actions_.currentConfig();
  tz_draft_ = c.tz_offset_min;
  LocalDateTime t;
  if (actions_.timeLost() || !toLocalDateTime(actions_.nowEpoch(), c.tz_offset_min, &t)) {
    t = LocalDateTime{};
    t.year = 2026;
    t.month = 1;
    t.day = 1;
  }
  t.second = 0;
  clock_draft_ = t;
}

void SettingsUi::setMessage(const char* text, bool error) {
  std::snprintf(message_, sizeof(message_), "%s", text != nullptr ? text : "");
  message_error_ = error;
  dirty_ = true;
}

}  // namespace toolcheck
