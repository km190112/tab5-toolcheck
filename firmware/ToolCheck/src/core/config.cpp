#include "config.h"

namespace toolcheck {
namespace {

bool inRange(int value, int lo, int hi) { return value >= lo && value <= hi; }

bool isFourDigitPin(const char pin[5]) {
  for (int i = 0; i < 4; ++i) {
    if (pin[i] < '0' || pin[i] > '9') return false;
  }
  return pin[4] == '\0';
}

}  // namespace

ConfigError validateConfig(const Config& c) {
  // 画面
  if (!inRange(c.brightness_pct, 10, 100)) return ConfigError::BrightnessOutOfRange;
  if (!inRange(c.dim_after_min, 0, 60)) return ConfigError::DimAfterOutOfRange;
  if (!inRange(c.dim_brightness_pct, 5, 100)) return ConfigError::DimBrightnessOutOfRange;
  if (c.dim_brightness_pct > c.brightness_pct) return ConfigError::DimBrightnessAboveNormal;
  if (!inRange(c.off_after_min, 0, 120)) return ConfigError::OffAfterOutOfRange;
  if (c.dim_after_min != 0 && c.off_after_min != 0 && c.off_after_min <= c.dim_after_min) {
    return ConfigError::OffNotAfterDim;
  }

  // 音量
  if (!inRange(c.volume_operation_pct, 0, 100)) return ConfigError::VolumeOperationOutOfRange;
  // 警告音も 0 (鳴らさない) にできる。記録だけ取りたい現場がある (2026-09-14。以前は下限 30%)
  if (!inRange(c.volume_warning1_pct, 0, 100)) return ConfigError::VolumeWarning1OutOfRange;
  if (!inRange(c.volume_warning2_pct, 0, 100)) return ConfigError::VolumeWarning2OutOfRange;
  if (c.volume_warning2_pct < c.volume_warning1_pct) return ConfigError::VolumeWarning2BelowWarning1;

  // 警告の秒数
  if (!inRange(c.warning1_sec, 10, 600)) return ConfigError::Warning1OutOfRange;
  if (c.warning2_sec > 900) return ConfigError::Warning2OutOfRange;
  if (c.warning2_sec <= c.warning1_sec) return ConfigError::Warning2NotAfterWarning1;
  if (!inRange(c.giveup_sec, 30, 1800)) return ConfigError::GiveupOutOfRange;

  // 利用者 ID・時差・PIN
  if (!inRange(c.user_id_digits, 1, 15)) return ConfigError::UserIdDigitsOutOfRange;
  if (!inRange(c.tz_offset_min, -720, 840)) return ConfigError::TzOffsetOutOfRange;
  if (!isFourDigitPin(c.pin)) return ConfigError::PinNotFourDigits;

  // 写真・ToF
  if (!inRange(c.capture_delay_ms, 0, 3000)) return ConfigError::CaptureDelayOutOfRange;
  if (c.tof_baseline_mm != 0 && !inRange(c.tof_baseline_mm, 30, 2000)) return ConfigError::TofBaselineOutOfRange;
  if (!inRange(c.open_delta_mm, 20, 1000)) return ConfigError::OpenDeltaOutOfRange;
  if (c.close_delta_mm >= c.open_delta_mm) return ConfigError::CloseDeltaNotBelowOpen;

  return ConfigError::None;
}

const char* describeConfigError(ConfigError err) {
  switch (err) {
    case ConfigError::None: return "問題ありません";
    case ConfigError::BrightnessOutOfRange: return "通常の明るさは 10〜100% で指定してください";
    case ConfigError::DimAfterOutOfRange: return "減光までの時間は 0〜60 分で指定してください";
    case ConfigError::DimBrightnessOutOfRange: return "減光時の明るさは 5〜100% で指定してください";
    case ConfigError::DimBrightnessAboveNormal: return "減光時の明るさは通常の明るさ以下にしてください";
    case ConfigError::OffAfterOutOfRange: return "消灯までの時間は 0〜120 分で指定してください";
    case ConfigError::OffNotAfterDim: return "消灯までの時間は減光までの時間より長くしてください";
    case ConfigError::VolumeOperationOutOfRange: return "操作音は 0〜100% で指定してください";
    case ConfigError::VolumeWarning1OutOfRange: return "警告 1 の音量は 0〜100% で指定してください";
    case ConfigError::VolumeWarning2OutOfRange: return "警告 2 の音量は 0〜100% で指定してください";
    case ConfigError::VolumeWarning2BelowWarning1: return "警告 2 の音量は警告 1 以上にしてください";
    case ConfigError::Warning1OutOfRange: return "警告 1 までの秒数は 10〜600 秒で指定してください";
    case ConfigError::Warning2OutOfRange: return "警告 2 までの秒数は 900 秒以下で指定してください";
    case ConfigError::Warning2NotAfterWarning1: return "警告 2 までの秒数は警告 1 より長くしてください";
    case ConfigError::GiveupOutOfRange: return "打ち切りまでの秒数は 30〜1800 秒で指定してください";
    case ConfigError::UserIdDigitsOutOfRange: return "利用者 ID の桁数 (文字数) は 1〜15 で指定してください";
    case ConfigError::TzOffsetOutOfRange: return "時差は -720〜840 分で指定してください";
    case ConfigError::PinNotFourDigits: return "PIN は数字 4 桁にしてください";
    case ConfigError::CaptureDelayOutOfRange: return "撮影までの時間は 0〜3000 ms で指定してください";
    case ConfigError::TofBaselineOutOfRange: return "ToF の基準距離は 30〜2000 mm で指定してください (0 は未校正)";
    case ConfigError::OpenDeltaOutOfRange: return "開の閾値は 20〜1000 mm で指定してください";
    case ConfigError::CloseDeltaNotBelowOpen: return "閉の閾値は開の閾値より小さくしてください";
    default: return "不明な設定の誤りです";
  }
}

bool applyConfig(Config& current, const Config& candidate, ConfigError* error) {
  const ConfigError e = validateConfig(candidate);
  if (error != nullptr) *error = e;
  if (e != ConfigError::None) return false;
  current = candidate;
  return true;
}

}  // namespace toolcheck
