#include "alarm.h"

#include <M5Unified.h>

#include "internal_i2c.h"

namespace toolcheck {
namespace {

constexpr ToneStep kWarning1[] = {{2000, 300}, {0, 300}};
constexpr ToneStep kWarning2[] = {{2800, 250}, {2000, 250}};
constexpr uint16_t kBeepHz = 2400;
constexpr uint32_t kBeepMs = 70;

// スピーカーを始める。起動時に始めると音が出てうるさい (2026-09-14) ので、鳴らすときに始める。
// 始めるときに IO エキスパンダ (内部 I2C) に触るので、撮影中で押さえられなければ始めずに false (次の呼び出しでやり直す)
bool ensureSpeaker() {
  if (M5.Speaker.isEnabled()) return true;
  if (!internal_i2c::tryLock()) return false;
  M5.Speaker.begin();
  internal_i2c::unlock();
  return M5.Speaker.isEnabled();
}

bool applyVolume(uint8_t pct) {
  if (!ensureSpeaker()) return false;
  M5.Speaker.setVolume(static_cast<uint8_t>((static_cast<uint32_t>(pct) * 255 + 50) / 100));
  return true;
}

}  // namespace

void Alarm::setLevel(uint8_t level, uint8_t volume_pct, uint32_t now_ms) {
  if (level > 2) level = 2;
  if (level == level_ && (level == 0 || volume_pct == volume_)) return;
  if (level == 0) {
    level_ = 0;
    volume_ = volume_pct;
    steps_ = nullptr;
    if (M5.Speaker.isEnabled()) M5.Speaker.stop();
    return;
  }
  // 撮影中でスピーカーを始められなければ次でやり直す。消音中と音量 0 (記録だけ取る現場) はスピーカーに触らない
  if (!muted_ && volume_pct > 0 && !applyVolume(volume_pct)) return;
  const bool changed = (level != level_);
  level_ = level;
  volume_ = volume_pct;
  if (changed) {
    steps_ = (level_ == 1) ? kWarning1 : kWarning2;
    step_count_ = 2;
    index_ = 0;
    startStep(now_ms);
  }
}

void Alarm::setMuted(bool muted) {
  muted_ = muted;
  if (muted_ && M5.Speaker.isEnabled()) M5.Speaker.stop();
}

bool Alarm::muted() const { return muted_; }

void Alarm::beep(uint8_t volume_pct) {
  if (muted_ || level_ != 0 || volume_pct == 0) return;  // 既定は操作音 0% (鳴らさない)
  if (!applyVolume(volume_pct)) return;
  M5.Speaker.tone(static_cast<float>(kBeepHz), kBeepMs);
}

void Alarm::preview(uint8_t level, uint8_t volume_pct, uint32_t now_ms, uint32_t duration_ms) {
  if (level_ != 0 && !previewing_) return;  // 本当の警告を鳴らしている
  if (level == 0 || level > 2) return;
  setLevel(0, volume_pct, now_ms);  // 前の試し鳴らしを止めて、段の切り替えとして始め直す
  setLevel(level, volume_pct, now_ms);
  previewing_ = level_ != 0;
  preview_until_ms_ = now_ms + duration_ms;
}

bool Alarm::previewing() const { return previewing_; }

void Alarm::update(uint32_t now_ms) {
  if (previewing_ && static_cast<int32_t>(now_ms - preview_until_ms_) >= 0) {
    previewing_ = false;
    setLevel(0, volume_, now_ms);
    return;
  }
  if (steps_ == nullptr) return;
  if (now_ms - step_at_ < steps_[index_].ms) return;
  index_ = static_cast<uint8_t>((index_ + 1) % step_count_);
  startStep(now_ms);
}

uint8_t Alarm::level() const { return level_; }

void Alarm::startStep(uint32_t now_ms) {
  step_at_ = now_ms;
  const ToneStep& s = steps_[index_];
  if (muted_ || volume_ == 0) return;  // 段と鳴らし方の進みは保ち、音だけ出さない (警告は画面の赤で知らせる)
  if (s.hz > 0) {
    M5.Speaker.tone(static_cast<float>(s.hz), s.ms);
  } else {
    M5.Speaker.stop();
  }
}

}  // namespace toolcheck
