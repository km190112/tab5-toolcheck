#include "drawer_detector.h"

namespace toolcheck {

DrawerDetector::DrawerDetector(const DrawerDetectorConfig& cfg) : cfg_(cfg) {}

void DrawerDetector::setBaseline(uint16_t baseline_mm) {
  baseline_mm_ = baseline_mm;
  state_ = (baseline_mm == 0) ? DrawerState::Uncalibrated : DrawerState::Closed;
  open_count_ = 0;
  close_timing_ = false;
  quiet_ = false;  // 静かな時間は数え直す (判定を止めていた間のサンプルは無い)
}

DrawerEvent DrawerDetector::update(uint16_t distance_mm, bool valid, uint32_t now_ms) {
  DrawerEvent ev;

  if (!valid) {
    // 無効なサンプルは開閉のどちらの数え方も途切れさせる (怪しい値で開閉を決めない)
    open_count_ = 0;
    close_timing_ = false;
    quiet_ = false;
    if (invalid_count_ < 255) ++invalid_count_;
    if (!fault_ && invalid_count_ >= cfg_.fault_samples) {
      fault_ = true;
      ev.fault_changed = true;
    }
    return ev;
  }

  invalid_count_ = 0;
  if (fault_) {
    fault_ = false;
    ev.fault_changed = true;
  }
  if (state_ == DrawerState::Uncalibrated) return ev;

  // 基準からどれだけ縮んだか (基準より遠いときは負)
  const int32_t shrink = static_cast<int32_t>(baseline_mm_) - static_cast<int32_t>(distance_mm);
  const bool inOpenZone = shrink >= static_cast<int32_t>(cfg_.open_delta_mm);
  const bool inCloseZone = shrink <= static_cast<int32_t>(cfg_.close_delta_mm);

  // 閉の範囲に続けて入っている時間。登録後の押さえは、これが rearm_quiet_ms に達したら解く (2026-09-16)
  if (inCloseZone) {
    if (!quiet_) {
      quiet_ = true;
      quiet_since_ms_ = now_ms;
    }
  } else {
    quiet_ = false;
  }
  if (hold_ && quiet_ && static_cast<uint32_t>(now_ms - quiet_since_ms_) >= cfg_.rearm_quiet_ms) {
    hold_ = false;
    ev.rearmed = true;
  }

  if (state_ == DrawerState::Closed) {
    if (!inOpenZone || hold_) {  // 押さえている間は開の範囲を数えない
      open_count_ = 0;
      return ev;
    }
    if (open_count_ < 255) ++open_count_;
    if (open_count_ >= cfg_.open_samples) {
      state_ = DrawerState::Open;
      open_count_ = 0;
      close_timing_ = false;
      ev.opened = true;
    }
    return ev;
  }

  // 開いているとき: 閉の候補が close_hold_ms 続いたら閉。間 (ヒステリシス) や開の候補が来たら計り直し
  if (!inCloseZone) {
    close_timing_ = false;
    return ev;
  }
  if (!close_timing_) {
    close_timing_ = true;
    close_since_ms_ = now_ms;
    return ev;
  }
  if (static_cast<uint32_t>(now_ms - close_since_ms_) >= cfg_.close_hold_ms) {  // millis の一周をまたいでも正しい
    state_ = DrawerState::Closed;
    close_timing_ = false;
    ev.closed = true;
  }
  return ev;
}

void DrawerDetector::holdOpenUntilQuiet() { hold_ = true; }

bool DrawerDetector::openHeld() const { return hold_; }

DrawerState DrawerDetector::state() const { return state_; }

bool DrawerDetector::isFault() const { return fault_; }

}  // namespace toolcheck
