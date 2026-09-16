// Unit ToF (VL53L0X) の距離の扱い: 使える値か・校正の基準距離・開閉判定の設定 (docs/設計.md「引き出し検知」)。
// Arduino 非依存。距離を I2C で読むのは hw/drawer_sensor、開閉を決めるのは core/drawer_detector。
#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"
#include "drawer_detector.h"

namespace toolcheck {

// VL53L0X (Pololu 1.3.1) の距離: 8190・8191 = 範囲外 (何も返ってこない)、65535 = タイムアウト。0 も使わない
constexpr uint16_t kTofOutOfRangeMm = 8190;

constexpr size_t kTofCalibrationMinSamples = 10;  // 使える値がこれより少なければ校正しない
constexpr uint16_t kTofBaselineMinMm = 30;        // config の tof_baseline_mm の範囲と同じ
constexpr uint16_t kTofBaselineMaxMm = 2000;
constexpr uint16_t kTofStableMm = 50;             // 中央値からこの幅に入る値を「落ち着いている」とみる
constexpr uint8_t kTofStablePercent = 80;         // 落ち着いている値がこの割合より少なければ校正しない

bool tofDistanceValid(uint16_t mm);

// VL53L1X (Unit ToF4M、Pololu VL53L1X 1.3.1 の RangeStatus) の読み取りの状態が、距離として使えるか (2026-09-15 実機は ToF4M だった)。
// 使う: 0 = 正常 / 3 = 近すぎて最小距離に切った (すぐ前に物がある) / 6 = 距離は正常で折り返しの確認だけしていない (測り始めの 1 回目)
// 使わない: 1 = ばらつき大 / 2 = 信号が弱い / 4 = 範囲に何も無い / 5 = 故障 / 7 = 折り返し / 9・10・13・255 ほか
bool tofRangeStatusUsable(uint8_t status);

enum class TofCalibrationResult : uint8_t {
  Ok,
  TooFewSamples,  // 使える値が kTofCalibrationMinSamples より少ない (ToF が読めていない・範囲外)
  OutOfRange,     // 中央値が 30〜2000mm の外
  Unstable,       // 値がばらついている (前に人がいる・引き出しが動いている)
};

struct TofCalibration {
  TofCalibrationResult result = TofCalibrationResult::TooFewSamples;
  uint16_t baseline_mm = 0;  // 使える値の中央値 (偶数個なら真ん中 2 つの平均を切り捨て)。Ok 以外でも中央値が出せれば入れる
  uint16_t min_mm = 0;       // 使える値の最小・最大 (画面とログに出す)
  uint16_t max_mm = 0;
  uint16_t valid_samples = 0;
};

// 全段を閉めた状態で集めた距離 (使えない値を含んでよい) から基準距離を決める。mm の並びは書き換えない
TofCalibration calibrateTofBaseline(const uint16_t* mm, size_t n);

// 設定の開閉の閾値を開閉判定に渡す (回数・閉の保持時間・異常の回数は DrawerDetectorConfig の既定のまま)
DrawerDetectorConfig drawerDetectorConfigFrom(const Config& cfg);

}  // namespace toolcheck
