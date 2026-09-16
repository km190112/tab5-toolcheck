#include "power_manager.h"

namespace toolcheck {
namespace {

const uint32_t kMsPerMinute = 60000u;

// 無操作の時間から状態を決める
PowerState stateForIdle(const Config& cfg, uint32_t idle_ms, bool keep_display) {
  if (idle_ms < kActiveHoldMs) return PowerState::Active;
  if (cfg.off_after_min != 0 && idle_ms >= cfg.off_after_min * kMsPerMinute) {
    // 未確認の無登録があるあいだは、見せることが目的なので消灯せず減光で止める
    return keep_display ? PowerState::Dim : PowerState::Off;
  }
  if (cfg.dim_after_min != 0 && idle_ms >= cfg.dim_after_min * kMsPerMinute) return PowerState::Dim;
  return PowerState::Idle;
}

}  // namespace

PowerManager::PowerManager(const Config& cfg, uint32_t now_ms) : cfg_(cfg), last_activity_ms_(now_ms) {}

void PowerManager::setConfig(const Config& cfg) { cfg_ = cfg; }

PowerOutputs PowerManager::onActivity(ActivityKind kind, uint32_t now_ms, bool* swallow_touch) {
  const bool wasAsleep = (state_ == PowerState::Dim || state_ == PowerState::Off);
  if (swallow_touch != nullptr) *swallow_touch = (kind == ActivityKind::Touch) && wasAsleep;
  last_activity_ms_ = now_ms;
  state_ = PowerState::Active;
  PowerInputs in;
  in.now_ms = now_ms;
  return outputsFor(state_, in);
}

PowerOutputs PowerManager::update(const PowerInputs& in) {
  if (in.busy || in.alarm) {
    last_activity_ms_ = in.now_ms;  // 操作が終わったところから数え直す
    state_ = PowerState::Active;
  } else {
    const uint32_t idle = in.now_ms - last_activity_ms_;  // uint32 の引き算なので millis の一周をまたいでも正しい
    state_ = stateForIdle(cfg_, idle, in.unconfirmed_unregistered);
  }
  return outputsFor(state_, in);
}

PowerOutputs PowerManager::outputsFor(PowerState state, const PowerInputs& in) const {
  PowerOutputs o;
  o.state = state;
  switch (state) {
    case PowerState::Active:
      o.cpu_mhz = kCpuFastMhz;
      o.brightness_pct = in.alarm ? static_cast<uint8_t>(100) : cfg_.brightness_pct;
      o.panel_sleep = false;
      o.qr_continuous = true;
      o.tof_period_ms = 50;
      o.speaker_enabled = true;
      o.light_sleep = false;
      break;
    case PowerState::Idle:
      o.cpu_mhz = kCpuFastMhz;  // 画面 (MIPI-DSI) を点けている間は落とせない (M2 実測)
      o.brightness_pct = cfg_.brightness_pct;
      o.panel_sleep = false;
      o.qr_continuous = true;
      o.tof_period_ms = 100;
      o.speaker_enabled = false;
      o.light_sleep = false;
      break;
    case PowerState::Dim:
      o.cpu_mhz = kCpuFastMhz;  // 画面 (MIPI-DSI) を点けている間は落とせない (M2 実測)
      o.brightness_pct = cfg_.dim_brightness_pct;
      o.panel_sleep = false;
      o.qr_continuous = false;
      o.tof_period_ms = 200;
      o.speaker_enabled = false;
      o.light_sleep = false;
      break;
    case PowerState::Off:
      o.cpu_mhz = kCpuFastMhz;  // 画面 (MIPI-DSI) を点けている間は落とせない (M2 実測)
      o.brightness_pct = 0;
      o.panel_sleep = false;  // バックライト 0 だけ。パネルスリープ中はタッチが効かない (M2 実測)
      o.qr_continuous = false;
      o.tof_period_ms = 200;
      o.speaker_enabled = false;
      o.light_sleep = false;  // 不採用: M2 で esp_light_sleep_start() の後に本体ごと止まって復帰しなかった
      break;
  }
  return o;
}

}  // namespace toolcheck
