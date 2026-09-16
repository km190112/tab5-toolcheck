// config: 設定値の範囲と相互の制約 (docs/設計.md「画面の明るさと音量」「設定画面」)
#include <cstring>
#include <string>

#include "core/config.h"
#include "testing.h"

using toolcheck::applyConfig;
using toolcheck::Config;
using toolcheck::ConfigError;
using toolcheck::describeConfigError;
using toolcheck::validateConfig;

TEST(config_defaults_are_valid) { CHECK_EQ(validateConfig(Config{}), ConfigError::None); }

// 2026-09-13 に実機で聞き比べ・見比べて決めた既定値 (spikes/README.md「M2 音・明るさ」)
TEST(config_defaults_follow_device_check) {
  const Config c;
  CHECK_EQ(c.volume_operation_pct, uint8_t{0});  // 2026-09-14: 実機で触って操作音がうるさい。既定は無音 (設定画面で上げる)
  // 2026-09-14 夕: 警告音 (ブザー) とカメラも既定はオフ (設定画面で入れる)。
  // 入れるときの目安は M2 で聞き比べた 警告 1 40% / 警告 2 60% (100% はうるさすぎた)
  CHECK_EQ(c.volume_warning1_pct, uint8_t{0});
  CHECK_EQ(c.volume_warning2_pct, uint8_t{0});
  CHECK(!c.camera_enabled);
  CHECK_EQ(c.brightness_pct, uint8_t{60});
  CHECK_EQ(c.dim_brightness_pct, uint8_t{15});
}

// 2026-09-14: Unit ToF が無くても貸出管理ができるように、引き出しセンサを使う / 使わないを設定で切り替える。既定は使う
TEST(config_default_uses_drawer_sensor) {
  const Config c;
  CHECK(c.drawer_sensor_enabled);
  Config off;
  off.drawer_sensor_enabled = false;
  CHECK_EQ(validateConfig(off), ConfigError::None);  // 使わない設定でも ToF の値の制約は変わらない
}

// 2026-09-15: 利用者 ID に英字・記号も使えるようにする (設定で選ぶ)。既定は今までどおり数字だけ
TEST(config_default_user_id_is_digits_only) {
  const Config c;
  CHECK(!c.user_id_alnum);
  Config alnum;
  alnum.user_id_alnum = true;
  CHECK_EQ(validateConfig(alnum), ConfigError::None);  // 文字数の範囲 (1〜15) は同じ
}

// --- 画面 ---

TEST(config_brightness_range) {
  Config c;
  c.dim_brightness_pct = 5;
  c.brightness_pct = 10;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.brightness_pct = 100;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.brightness_pct = 9;
  CHECK_EQ(validateConfig(c), ConfigError::BrightnessOutOfRange);
  c.brightness_pct = 101;
  CHECK_EQ(validateConfig(c), ConfigError::BrightnessOutOfRange);
}

TEST(config_dim_brightness_range) {
  Config c;
  c.dim_brightness_pct = 5;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.dim_brightness_pct = 4;
  CHECK_EQ(validateConfig(c), ConfigError::DimBrightnessOutOfRange);
}

TEST(config_dim_brightness_must_not_exceed_normal) {
  Config c;
  c.brightness_pct = 50;
  c.dim_brightness_pct = 50;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.dim_brightness_pct = 51;
  CHECK_EQ(validateConfig(c), ConfigError::DimBrightnessAboveNormal);
}

TEST(config_dim_after_range) {
  Config c;
  c.dim_after_min = 0;  // 減光しない
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.dim_after_min = 60;
  c.off_after_min = 61;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.dim_after_min = 61;
  c.off_after_min = 0;
  CHECK_EQ(validateConfig(c), ConfigError::DimAfterOutOfRange);
}

TEST(config_off_after_range) {
  Config c;
  c.off_after_min = 0;  // 消灯しない
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.off_after_min = 120;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.off_after_min = 121;
  CHECK_EQ(validateConfig(c), ConfigError::OffAfterOutOfRange);
}

TEST(config_off_must_be_after_dim) {
  Config c;
  c.dim_after_min = 5;
  c.off_after_min = 5;
  CHECK_EQ(validateConfig(c), ConfigError::OffNotAfterDim);
  c.off_after_min = 4;
  CHECK_EQ(validateConfig(c), ConfigError::OffNotAfterDim);
  c.off_after_min = 6;
  CHECK_EQ(validateConfig(c), ConfigError::None);
}

TEST(config_off_without_dim_is_fine) {
  Config c;
  c.dim_after_min = 0;
  c.off_after_min = 1;
  CHECK_EQ(validateConfig(c), ConfigError::None);
}

// --- 音量 ---

TEST(config_operation_volume_range) {
  Config c;
  c.volume_operation_pct = 0;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.volume_operation_pct = 101;
  CHECK_EQ(validateConfig(c), ConfigError::VolumeOperationOutOfRange);
}

// 2026-09-14: 持ち出しの記録だけを取りたい現場もあるので、警告音も 0% (無音) にできる (下限 30% をやめた)
TEST(config_warning_volume_range) {
  Config c;
  c.volume_warning1_pct = 0;
  c.volume_warning2_pct = 0;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.volume_warning2_pct = 100;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.volume_warning1_pct = 101;
  c.volume_warning2_pct = 101;
  CHECK_EQ(validateConfig(c), ConfigError::VolumeWarning1OutOfRange);
  c.volume_warning1_pct = 100;
  c.volume_warning2_pct = 101;
  CHECK_EQ(validateConfig(c), ConfigError::VolumeWarning2OutOfRange);
}

TEST(config_warning2_volume_not_below_warning1) {
  Config c;
  c.volume_warning1_pct = 70;
  c.volume_warning2_pct = 70;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.volume_warning2_pct = 69;
  CHECK_EQ(validateConfig(c), ConfigError::VolumeWarning2BelowWarning1);
}

// --- 警告の秒数 ---

TEST(config_warning1_seconds_range) {
  Config c;
  c.warning1_sec = 10;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.warning1_sec = 9;
  CHECK_EQ(validateConfig(c), ConfigError::Warning1OutOfRange);
  c.warning1_sec = 601;
  c.warning2_sec = 700;
  CHECK_EQ(validateConfig(c), ConfigError::Warning1OutOfRange);
}

TEST(config_warning2_seconds_after_warning1) {
  Config c;
  c.warning1_sec = 40;
  c.warning2_sec = 40;
  CHECK_EQ(validateConfig(c), ConfigError::Warning2NotAfterWarning1);
  c.warning2_sec = 41;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.warning2_sec = 901;
  CHECK_EQ(validateConfig(c), ConfigError::Warning2OutOfRange);
}

TEST(config_giveup_seconds_range) {
  Config c;
  c.giveup_sec = 30;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.giveup_sec = 29;
  CHECK_EQ(validateConfig(c), ConfigError::GiveupOutOfRange);
  c.giveup_sec = 1801;
  CHECK_EQ(validateConfig(c), ConfigError::GiveupOutOfRange);
}

// --- 利用者 ID・時差・PIN ---

TEST(config_user_id_digits_range) {
  Config c;
  c.user_id_digits = 1;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.user_id_digits = 15;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.user_id_digits = 0;
  CHECK_EQ(validateConfig(c), ConfigError::UserIdDigitsOutOfRange);
  c.user_id_digits = 16;
  CHECK_EQ(validateConfig(c), ConfigError::UserIdDigitsOutOfRange);
}

TEST(config_tz_offset_range) {
  Config c;
  c.tz_offset_min = -720;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.tz_offset_min = 840;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.tz_offset_min = -721;
  CHECK_EQ(validateConfig(c), ConfigError::TzOffsetOutOfRange);
  c.tz_offset_min = 841;
  CHECK_EQ(validateConfig(c), ConfigError::TzOffsetOutOfRange);
}

TEST(config_pin_must_be_four_digits) {
  Config c;
  std::memcpy(c.pin, "1234", 5);
  CHECK_EQ(validateConfig(c), ConfigError::None);
  std::memcpy(c.pin, "12a4", 5);
  CHECK_EQ(validateConfig(c), ConfigError::PinNotFourDigits);
  std::memcpy(c.pin, "123\0", 5);
  CHECK_EQ(validateConfig(c), ConfigError::PinNotFourDigits);
  const char five[5] = {'1', '2', '3', '4', '5'};  // NUL で終わっていない
  std::memcpy(c.pin, five, 5);
  CHECK_EQ(validateConfig(c), ConfigError::PinNotFourDigits);
}

// --- 写真・ToF ---

TEST(config_capture_delay_range) {
  Config c;
  c.capture_delay_ms = 0;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.capture_delay_ms = 3000;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.capture_delay_ms = 3001;
  CHECK_EQ(validateConfig(c), ConfigError::CaptureDelayOutOfRange);
}

TEST(config_tof_baseline_range) {
  Config c;
  c.tof_baseline_mm = 0;  // 未校正
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.tof_baseline_mm = 30;
  CHECK_EQ(validateConfig(c), ConfigError::None);
  c.tof_baseline_mm = 29;
  CHECK_EQ(validateConfig(c), ConfigError::TofBaselineOutOfRange);
  c.tof_baseline_mm = 2001;
  CHECK_EQ(validateConfig(c), ConfigError::TofBaselineOutOfRange);
}

TEST(config_open_and_close_delta) {
  Config c;
  c.open_delta_mm = 19;
  CHECK_EQ(validateConfig(c), ConfigError::OpenDeltaOutOfRange);
  c.open_delta_mm = 1001;
  CHECK_EQ(validateConfig(c), ConfigError::OpenDeltaOutOfRange);
  c.open_delta_mm = 100;
  c.close_delta_mm = 100;
  CHECK_EQ(validateConfig(c), ConfigError::CloseDeltaNotBelowOpen);
  c.close_delta_mm = 0;
  CHECK_EQ(validateConfig(c), ConfigError::None);
}

// --- 反映 ---

TEST(config_apply_valid_updates_current) {
  Config current;
  Config candidate;
  candidate.brightness_pct = 80;
  ConfigError err = ConfigError::Count;
  CHECK(applyConfig(current, candidate, &err));
  CHECK_EQ(err, ConfigError::None);
  CHECK_EQ(current.brightness_pct, uint8_t{80});
}

TEST(config_apply_invalid_keeps_current) {
  Config current;
  current.brightness_pct = 70;
  current.volume_operation_pct = 20;  // 既定値に頼らない (2026-09-14 に既定を 50 → 0 に変えたとき、ここが既定値を前提にしていた)
  Config candidate = current;
  candidate.brightness_pct = 5;
  candidate.volume_operation_pct = 10;  // 正しい変更が混ざっていても丸ごと拒否
  ConfigError err = ConfigError::None;
  CHECK(!applyConfig(current, candidate, &err));
  CHECK_EQ(err, ConfigError::BrightnessOutOfRange);
  CHECK_EQ(current.brightness_pct, uint8_t{70});
  CHECK_EQ(current.volume_operation_pct, uint8_t{20});
}

TEST(config_every_error_has_reason) {
  for (int i = static_cast<int>(ConfigError::BrightnessOutOfRange); i < static_cast<int>(ConfigError::Count); ++i) {
    const char* reason = describeConfigError(static_cast<ConfigError>(i));
    CHECK(reason != nullptr && std::strlen(reason) > 0);
  }
}
