// tof_reading: Unit ToF の距離が使えるか・校正の基準距離・開閉判定の設定 (docs/設計.md「引き出し検知」)
#include <cstdint>
#include <vector>

#include "core/tof_reading.h"
#include "testing.h"

using toolcheck::calibrateTofBaseline;
using toolcheck::Config;
using toolcheck::DrawerDetectorConfig;
using toolcheck::drawerDetectorConfigFrom;
using toolcheck::TofCalibration;
using toolcheck::TofCalibrationResult;
using toolcheck::tofDistanceValid;
using toolcheck::tofRangeStatusUsable;

namespace {

TofCalibration calibrate(const std::vector<uint16_t>& v) { return calibrateTofBaseline(v.data(), v.size()); }

std::vector<uint16_t> repeat(uint16_t mm, size_t n) { return std::vector<uint16_t>(n, mm); }

}  // namespace

TEST(tof_distance_valid_excludes_sensor_codes) {
  CHECK(!tofDistanceValid(0));
  CHECK(tofDistanceValid(1));
  CHECK(tofDistanceValid(800));
  CHECK(tofDistanceValid(8189));
  CHECK(!tofDistanceValid(8190));   // 範囲外
  CHECK(!tofDistanceValid(8191));
  CHECK(!tofDistanceValid(65535));  // タイムアウト
}

TEST(tof_range_status_usable_for_vl53l1x) {
  CHECK(tofRangeStatusUsable(0));   // RangeValid
  CHECK(tofRangeStatusUsable(3));   // RangeValidMinRangeClipped (すぐ前に物がある)
  CHECK(tofRangeStatusUsable(6));   // RangeValidNoWrapCheckFail (測り始め)
  CHECK(!tofRangeStatusUsable(1));  // SigmaFail
  CHECK(!tofRangeStatusUsable(2));  // SignalFail
  CHECK(!tofRangeStatusUsable(4));  // OutOfBoundsFail (範囲に何も無い)
  CHECK(!tofRangeStatusUsable(5));  // HardwareFail
  CHECK(!tofRangeStatusUsable(7));  // WrapTargetFail
  CHECK(!tofRangeStatusUsable(8));
  CHECK(!tofRangeStatusUsable(9));    // XtalkSignalFail
  CHECK(!tofRangeStatusUsable(10));   // SynchronizationInt
  CHECK(!tofRangeStatusUsable(13));   // MinRangeFail
  CHECK(!tofRangeStatusUsable(255));  // None (更新なし)
}

TEST(tof_calibration_median_of_odd_count) {
  const TofCalibration c = calibrate({810, 790, 800, 805, 795, 800, 799, 801, 803, 797, 800});
  CHECK_EQ(c.result, TofCalibrationResult::Ok);
  CHECK_EQ(c.baseline_mm, 800);
  CHECK_EQ(c.min_mm, 790);
  CHECK_EQ(c.max_mm, 810);
  CHECK_EQ(c.valid_samples, 11);
}

TEST(tof_calibration_median_of_even_count_rounds_down) {
  // 並べると ... 800, 803 ... が真ん中 → 801 (801.5 の切り捨て)
  const TofCalibration c = calibrate({790, 792, 795, 798, 800, 803, 805, 806, 808, 810});
  CHECK_EQ(c.result, TofCalibrationResult::Ok);
  CHECK_EQ(c.baseline_mm, 801);
}

TEST(tof_calibration_ignores_invalid_values) {
  std::vector<uint16_t> v = repeat(750, 10);
  v.push_back(0);
  v.push_back(8190);
  v.push_back(8191);
  v.push_back(65535);
  const TofCalibration c = calibrate(v);
  CHECK_EQ(c.result, TofCalibrationResult::Ok);
  CHECK_EQ(c.baseline_mm, 750);
  CHECK_EQ(c.max_mm, 750);
  CHECK_EQ(c.valid_samples, 10);
}

TEST(tof_calibration_needs_ten_valid_samples) {
  std::vector<uint16_t> v = repeat(800, 9);
  v.push_back(8190);
  v.push_back(65535);
  const TofCalibration c = calibrate(v);
  CHECK_EQ(c.result, TofCalibrationResult::TooFewSamples);
  CHECK_EQ(c.valid_samples, 9);
  CHECK_EQ(calibrate(repeat(800, 10)).result, TofCalibrationResult::Ok);  // 境界
}

TEST(tof_calibration_nothing_read) {
  const TofCalibration c = calibrateTofBaseline(nullptr, 0);
  CHECK_EQ(c.result, TofCalibrationResult::TooFewSamples);
  CHECK_EQ(c.baseline_mm, 0);
  CHECK_EQ(calibrate(repeat(8190, 40)).result, TofCalibrationResult::TooFewSamples);  // ずっと範囲外
}

TEST(tof_calibration_baseline_range) {
  CHECK_EQ(calibrate(repeat(29, 10)).result, TofCalibrationResult::OutOfRange);
  CHECK_EQ(calibrate(repeat(30, 10)).result, TofCalibrationResult::Ok);
  CHECK_EQ(calibrate(repeat(2000, 10)).result, TofCalibrationResult::Ok);
  const TofCalibration far = calibrate(repeat(2001, 10));
  CHECK_EQ(far.result, TofCalibrationResult::OutOfRange);
  CHECK_EQ(far.baseline_mm, 2001);  // 失敗でも中央値は見せる
}

TEST(tof_calibration_rejects_scattered_values) {
  // 半分が 600mm (前に人がいる・引き出しが開いている)
  std::vector<uint16_t> v = repeat(800, 10);
  const std::vector<uint16_t> near = repeat(600, 10);
  v.insert(v.end(), near.begin(), near.end());
  CHECK_EQ(calibrate(v).result, TofCalibrationResult::Unstable);
}

TEST(tof_calibration_stable_boundary) {
  // 中央値 800 から ±50mm に入るのが 16 / 20 = 80% → 通る
  std::vector<uint16_t> ok = {750, 850, 760, 840, 770, 830, 780, 820, 790, 810, 800, 800, 800, 800, 800, 800};
  ok.push_back(400);
  ok.push_back(400);
  ok.push_back(1200);
  ok.push_back(1200);
  const TofCalibration c = calibrate(ok);
  CHECK_EQ(c.result, TofCalibrationResult::Ok);
  CHECK_EQ(c.baseline_mm, 800);
  CHECK_EQ(c.min_mm, 400);
  CHECK_EQ(c.max_mm, 1200);

  // 14 / 20 = 70% → 通らない (749 と 851 も幅の外)
  std::vector<uint16_t> ng = {749, 851, 760, 840, 770, 830, 780, 820, 790, 810, 800, 800, 800, 800, 800, 800};
  ng.push_back(400);
  ng.push_back(400);
  ng.push_back(1200);
  ng.push_back(1200);
  CHECK_EQ(calibrate(ng).result, TofCalibrationResult::Unstable);
}

TEST(tof_calibration_does_not_modify_input) {
  const std::vector<uint16_t> original = {820, 780, 800, 810, 790, 805, 795, 801, 799, 803};
  std::vector<uint16_t> v = original;
  calibrateTofBaseline(v.data(), v.size());
  CHECK(v == original);
}

TEST(tof_detector_config_from_settings) {
  Config cfg;
  cfg.open_delta_mm = 150;
  cfg.close_delta_mm = 60;
  const DrawerDetectorConfig d = drawerDetectorConfigFrom(cfg);
  CHECK_EQ(d.open_delta_mm, 150);
  CHECK_EQ(d.close_delta_mm, 60);
  const DrawerDetectorConfig defaults;
  CHECK_EQ(d.open_samples, defaults.open_samples);
  CHECK_EQ(d.close_hold_ms, defaults.close_hold_ms);
  CHECK_EQ(d.fault_samples, defaults.fault_samples);
}
