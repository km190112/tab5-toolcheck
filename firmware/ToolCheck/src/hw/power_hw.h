// 起動時に切って、以後使わない電源 (docs/設計.md「省電力」)。M5.begin() の後に 1 回呼ぶ。
// マイクと IMU は ToolCheck.ino の M5.config() で切る。状態ごとの電力 (明るさ・QR の間欠) は 6 段目で power_manager から配線する。
#pragma once

namespace toolcheck {
namespace power_hw {

void begin();

}  // namespace power_hw
}  // namespace toolcheck
