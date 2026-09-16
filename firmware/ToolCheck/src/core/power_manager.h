// 電力状態 (操作中 / 待機 / 減光 / 消灯) を決め、周辺機器への指示をまとめて出す (docs/設計.md「省電力」)。
// Arduino 非依存。実際に CPU 周波数や明るさを変えるのは hw 層 (power_hw)。
//
//   操作中: セッション・警告・設定画面・検索・写真表示・撮影・SD 書込の間と、操作の直後
//   待機  : 操作が終わってから減光まで
//   減光  : 無操作 dim_after_min 分
//   消灯  : 無操作 off_after_min 分 (未確認の無登録があるあいだは消灯せず減光で止める)
#pragma once

#include <cstdint>

#include "config.h"

namespace toolcheck {

enum class PowerState : uint8_t { Active, Idle, Dim, Off };

enum class ActivityKind : uint8_t { DrawerOpen, QrRead, Touch };

// 操作があってから「操作中」のまま保つ時間 (画面の書き換えや確定処理を 360MHz で済ませるため)
constexpr uint32_t kActiveHoldMs = 2000;

constexpr uint16_t kCpuFastMhz = 360;
// 画面を点けている間は使えない。M2 の実測で、40MHz / 20MHz に落とすと MIPI-DSI の送出が間に合わず
// (`E lcd.dsi: ... underrun happens`) ハードウェアのウォッチドッグで再起動した。
// 消灯中に DSI の送出そのものを止める実装ができたら、そのときだけ使う
constexpr uint16_t kCpuSlowMhz = 40;

struct PowerInputs {
  uint32_t now_ms = 0;
  bool busy = false;                      // セッション・設定画面・検索・写真表示・撮影・SD 書込の最中
  bool alarm = false;                     // 警告中 (最大の明るさ)
  bool unconfirmed_unregistered = false;  // 未確認の無登録がある (消灯しない)
  bool settings_open = false;             // 設定画面を開いている (いまは状態の決定に使っていない。ライトスリープは不採用)
};

struct PowerOutputs {
  PowerState state = PowerState::Active;
  uint16_t cpu_mhz = kCpuFastMhz;
  uint8_t brightness_pct = 0;  // 0 = バックライト消灯
  bool panel_sleep = false;    // いまはどの状態でも false (パネルスリープ中はタッチで起こせないため。M2 実測)
  bool qr_continuous = true;   // false = 間欠トリガ
  uint16_t tof_period_ms = 50;
  bool speaker_enabled = true;
  bool light_sleep = false;    // いまはどの状態でも false (手動ライトスリープは不採用。M2 で本体ごと止まって復帰しなかった)
};

class PowerManager {
 public:
  PowerManager(const Config& cfg, uint32_t now_ms);

  void setConfig(const Config& cfg);

  // 開放・QR 読取・タッチがあった。指示は処理より先に適用する (CPU を先に 360MHz に上げる)。
  // swallow_touch には「減光・消灯中の最初のタッチなので、復帰だけに使って操作としては扱わない」を返す
  PowerOutputs onActivity(ActivityKind kind, uint32_t now_ms, bool* swallow_touch);

  PowerOutputs update(const PowerInputs& in);

 private:
  PowerOutputs outputsFor(PowerState state, const PowerInputs& in) const;

  Config cfg_;
  uint32_t last_activity_ms_ = 0;
  PowerState state_ = PowerState::Active;
};

}  // namespace toolcheck
