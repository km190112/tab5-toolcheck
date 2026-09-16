// 設定値・既定値・範囲と相互の制約 (docs/設計.md「画面の明るさと音量」「設定画面」)。
// NVS への保存 (版数・移行) は別のモジュールで扱う。ここは値の正しさだけを決める。
// 制約に反する値は丸めずに拒否する (黙って丸めると「設定したのに変わらない」になり原因が分からない)。
#pragma once

#include <cstdint>

namespace toolcheck {

constexpr uint8_t kConfigVersion = 1;

struct Config {
  // 画面 (百分率・分)
  uint8_t brightness_pct     = 60;  // 通常の明るさ 10〜100 (10 未満は真っ暗になって設定画面に辿り着けなくなる)
  uint8_t dim_after_min      = 5;   // 減光までの無操作時間 0〜60 (0 = 減光しない)
  uint8_t dim_brightness_pct = 15;  // 減光時の明るさ 5〜100、通常の明るさ以下
  uint8_t off_after_min      = 15;  // 消灯までの無操作時間 0〜120 (0 = 消灯しない)。減光を使うなら減光より長く

  // 音量 (百分率)
  // 2026-09-14: 操作音はうるさいので既定は無音。同日夕に警告音 (ブザー) も既定は無音にした (設定画面で上げる)。
  // 上げるときの目安は M2 で聞き比べた 警告 1 40% / 警告 2 60% (100% はうるさすぎた)
  uint8_t volume_operation_pct = 0;    // 操作音 0〜100 (0 = 鳴らさない)
  uint8_t volume_warning1_pct  = 0;    // 警告 1 0〜100 (0 = 鳴らさない。画面の赤だけで知らせる)
  uint8_t volume_warning2_pct  = 0;    // 警告 2 0〜100、警告 1 以上

  // 警告の秒数
  uint16_t warning1_sec = 40;   // 10〜600
  uint16_t warning2_sec = 60;   // 警告 1 より長く、900 以下
  uint16_t giveup_sec   = 120;  // 警告 2 から打ち切りまで 30〜1800

  // 利用者 ID・時刻・PIN
  uint8_t user_id_digits = 7;                      // 1〜15 (英字・記号も使うときは文字数)
  // 2026-09-15: 少人数の現場では名前を英字にして記号で桁埋めした ID を名札にする (マスタ無しで誰か分かる)。
  // true なら user_id_digits 文字ちょうどの、物品番号と同じ文字 (空白・カンマ以外の印字可能 ASCII) を利用者 ID とみなす
  bool    user_id_alnum  = false;
  int16_t tz_offset_min  = 540;                    // -720〜840 (既定は日本 +9 時間)
  char    pin[5]         = {'0', '0', '0', '0', '\0'};  // 数字 4 桁 + NUL

  // 写真
  bool     camera_enabled    = false;  // 2026-09-14 夕: 既定はオフ (設定画面の「写真」で入れる。入切は再起動で反映)
  bool     save_photos_to_sd = true;
  uint16_t capture_delay_ms  = 500;  // 開放を検知してから撮るまで 0〜3000

  // 引き出し (ToF)
  bool     drawer_sensor_enabled = true;  // 引き出しセンサ (Unit ToF) を使う (2026-09-14: 使わない設定でも貸出管理ができるように)
  uint16_t tof_baseline_mm = 0;    // 0 = 未校正、それ以外は 30〜2000
  uint16_t open_delta_mm   = 100;  // 20〜1000
  uint16_t close_delta_mm  = 50;   // 開の閾値より小さく

  // 開発・保守
  bool dev_sim    = false;  // シリアルの sim 系コマンド (開閉・QR の注入) を受け付ける
  bool serial_log = false;  // 使わない。動作ログは 2026-09-14 に取りやめ。保存の形 ([15] bit3) を変えないために残す
  // 消灯中の手動ライトスリープの入切は持たない (M2 で本体ごと止まって復帰しなかったので不採用)
};

enum class ConfigError : uint8_t {
  None,
  BrightnessOutOfRange,
  DimAfterOutOfRange,
  DimBrightnessOutOfRange,
  DimBrightnessAboveNormal,
  OffAfterOutOfRange,
  OffNotAfterDim,
  VolumeOperationOutOfRange,
  VolumeWarning1OutOfRange,
  VolumeWarning2OutOfRange,
  VolumeWarning2BelowWarning1,
  Warning1OutOfRange,
  Warning2OutOfRange,
  Warning2NotAfterWarning1,
  GiveupOutOfRange,
  UserIdDigitsOutOfRange,
  TzOffsetOutOfRange,
  PinNotFourDigits,
  CaptureDelayOutOfRange,
  TofBaselineOutOfRange,
  OpenDeltaOutOfRange,
  CloseDeltaNotBelowOpen,
  Count,  // 番兵 (テストで全部に理由があるかを見るため)
};

// 最初に見つかった違反を返す。全部守られていれば None
ConfigError validateConfig(const Config& cfg);

// 違反の理由 (画面とシリアルに出す日本語。UTF-8)
const char* describeConfigError(ConfigError err);

// candidate が正しければ current に写して true。違反なら current は変えずに false を返し、error に理由を入れる
bool applyConfig(Config& current, const Config& candidate, ConfigError* error);

}  // namespace toolcheck
