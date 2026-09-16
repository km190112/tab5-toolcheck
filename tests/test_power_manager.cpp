// power_manager: 電力状態と周辺機器への指示 (docs/設計.md「省電力」)
#include "core/power_manager.h"
#include "testing.h"

using toolcheck::ActivityKind;
using toolcheck::Config;
using toolcheck::kActiveHoldMs;
using toolcheck::kCpuFastMhz;
using toolcheck::PowerInputs;
using toolcheck::PowerManager;
using toolcheck::PowerOutputs;
using toolcheck::PowerState;

namespace {

const uint32_t kMin = 60000;

// 既定: 通常 60% / 5 分で減光 15% / 15 分で消灯
Config defaults() { return Config{}; }

PowerOutputs at(PowerManager& pm, uint32_t now) {
  PowerInputs in;
  in.now_ms = now;
  return pm.update(in);
}

}  // namespace

// --- 時間で落ちていく ---

TEST(power_active_right_after_start) {
  PowerManager pm(defaults(), 0);
  auto o = at(pm, 100);
  CHECK_EQ(o.state, PowerState::Active);
  CHECK_EQ(o.cpu_mhz, kCpuFastMhz);
}

TEST(power_idle_after_active_hold) {
  PowerManager pm(defaults(), 0);
  auto o = at(pm, kActiveHoldMs);
  CHECK_EQ(o.state, PowerState::Idle);
  CHECK_EQ(o.cpu_mhz, kCpuFastMhz);  // 画面 (MIPI-DSI) を点けている間は落とせない (M2 実測)
  CHECK_EQ(o.brightness_pct, uint8_t{60});
  CHECK(o.qr_continuous);
  CHECK_EQ(o.tof_period_ms, uint16_t{100});
  CHECK(!o.speaker_enabled);
}

TEST(power_dim_after_dim_minutes) {
  PowerManager pm(defaults(), 0);
  CHECK_EQ(at(pm, 5 * kMin - 1).state, PowerState::Idle);
  auto o = at(pm, 5 * kMin);
  CHECK_EQ(o.state, PowerState::Dim);
  CHECK_EQ(o.brightness_pct, uint8_t{15});
  CHECK(!o.qr_continuous);
  CHECK_EQ(o.tof_period_ms, uint16_t{200});
  CHECK_EQ(o.cpu_mhz, kCpuFastMhz);  // 画面 (MIPI-DSI) を点けている間は落とせない (M2 実測)
}

TEST(power_off_after_off_minutes) {
  PowerManager pm(defaults(), 0);
  CHECK_EQ(at(pm, 15 * kMin - 1).state, PowerState::Dim);
  auto o = at(pm, 15 * kMin);
  CHECK_EQ(o.state, PowerState::Off);
  CHECK_EQ(o.brightness_pct, uint8_t{0});
  CHECK(!o.panel_sleep);  // 消灯はバックライト 0 だけ (パネルスリープ中はタッチが効かない。M2 実測)
  CHECK(!o.qr_continuous);
  CHECK(!o.speaker_enabled);
}

TEST(power_zero_dim_minutes_never_dims) {
  Config c = defaults();
  c.dim_after_min = 0;
  PowerManager pm(c, 0);
  CHECK_EQ(at(pm, 14 * kMin).state, PowerState::Idle);
  CHECK_EQ(at(pm, 15 * kMin).state, PowerState::Off);
}

TEST(power_zero_off_minutes_never_turns_off) {
  Config c = defaults();
  c.off_after_min = 0;
  PowerManager pm(c, 0);
  CHECK_EQ(at(pm, 1000 * kMin).state, PowerState::Dim);
}

// 2026-09-13 実測: パネルスリープ中はタッチが取れず、バックライト 0 だけなら取れた (電流の差は約 0.02A)。
// 消灯でもパネルは寝かせずタッチで起こせるようにする
TEST(power_panel_never_sleeps_so_touch_can_wake) {
  PowerManager pm(defaults(), 0);
  CHECK(!at(pm, kActiveHoldMs).panel_sleep);
  CHECK(!at(pm, 5 * kMin).panel_sleep);
  CHECK(!at(pm, 15 * kMin).panel_sleep);
}

TEST(power_idle_timer_survives_millis_wraparound) {
  const uint32_t start = 0xFFFFFFFFu - kMin;  // 1 分後に一周する
  PowerManager pm(defaults(), start);
  CHECK_EQ(at(pm, start + 4 * kMin).state, PowerState::Idle);  // 一周した後の値
  CHECK_EQ(at(pm, start + 5 * kMin).state, PowerState::Dim);
}

// --- 操作中 ---

TEST(power_busy_keeps_active_and_restarts_idle_timer) {
  PowerManager pm(defaults(), 0);
  PowerInputs busy;
  busy.busy = true;
  busy.now_ms = 10 * kMin;
  auto o = pm.update(busy);
  CHECK_EQ(o.state, PowerState::Active);
  CHECK_EQ(o.cpu_mhz, kCpuFastMhz);
  CHECK(o.speaker_enabled);
  CHECK_EQ(o.tof_period_ms, uint16_t{50});
  // 操作が終わったところから数え直す
  CHECK_EQ(at(pm, 10 * kMin + 4 * kMin).state, PowerState::Idle);
  CHECK_EQ(at(pm, 10 * kMin + 5 * kMin).state, PowerState::Dim);
}

TEST(power_alarm_forces_max_brightness) {
  PowerManager pm(defaults(), 0);
  PowerInputs in;
  in.now_ms = 20 * kMin;  // 放っておけば消灯の時間
  in.busy = true;
  in.alarm = true;
  auto o = pm.update(in);
  CHECK_EQ(o.state, PowerState::Active);
  CHECK_EQ(o.brightness_pct, uint8_t{100});
  CHECK(!o.panel_sleep);
}

// --- 復帰 ---

TEST(power_activity_returns_fast_cpu_first) {
  PowerManager pm(defaults(), 0);
  at(pm, 20 * kMin);  // 消灯
  bool swallow = true;
  auto o = pm.onActivity(ActivityKind::DrawerOpen, 20 * kMin + 1, &swallow);
  CHECK_EQ(o.state, PowerState::Active);
  CHECK_EQ(o.cpu_mhz, kCpuFastMhz);
  CHECK(!o.panel_sleep);
  CHECK(o.qr_continuous);
  CHECK(!swallow);
}

TEST(power_first_touch_while_dim_is_swallowed) {
  PowerManager pm(defaults(), 0);
  at(pm, 6 * kMin);  // 減光
  bool swallow = false;
  pm.onActivity(ActivityKind::Touch, 6 * kMin + 1, &swallow);
  CHECK(swallow);
  // 起きた後のタッチは操作として扱う
  pm.onActivity(ActivityKind::Touch, 6 * kMin + 500, &swallow);
  CHECK(!swallow);
}

TEST(power_first_touch_while_off_is_swallowed) {
  PowerManager pm(defaults(), 0);
  at(pm, 20 * kMin);  // 消灯
  bool swallow = false;
  pm.onActivity(ActivityKind::Touch, 20 * kMin + 1, &swallow);
  CHECK(swallow);
}

TEST(power_touch_while_idle_is_not_swallowed) {
  PowerManager pm(defaults(), 0);
  at(pm, 1 * kMin);  // 待機
  bool swallow = true;
  pm.onActivity(ActivityKind::Touch, 1 * kMin + 1, &swallow);
  CHECK(!swallow);
}

TEST(power_qr_read_while_off_is_not_swallowed) {
  PowerManager pm(defaults(), 0);
  at(pm, 20 * kMin);
  bool swallow = true;
  pm.onActivity(ActivityKind::QrRead, 20 * kMin + 1, &swallow);
  CHECK(!swallow);
}

TEST(power_activity_restarts_idle_timer) {
  PowerManager pm(defaults(), 0);
  at(pm, 20 * kMin);
  bool swallow = false;
  pm.onActivity(ActivityKind::QrRead, 20 * kMin, &swallow);
  CHECK_EQ(at(pm, 20 * kMin + 1000).state, PowerState::Active);
  CHECK_EQ(at(pm, 20 * kMin + kActiveHoldMs).state, PowerState::Idle);
  CHECK_EQ(at(pm, 25 * kMin).state, PowerState::Dim);
}

// --- 無登録・ライトスリープ ---

TEST(power_unconfirmed_unregistered_stops_at_dim) {
  PowerManager pm(defaults(), 0);
  PowerInputs in;
  in.now_ms = 30 * kMin;
  in.unconfirmed_unregistered = true;
  auto o = pm.update(in);
  CHECK_EQ(o.state, PowerState::Dim);
  CHECK_EQ(o.brightness_pct, uint8_t{15});
  CHECK(!o.panel_sleep);
}

// 2026-09-12 M2 実測: 消灯中に esp_light_sleep_start() を回すと、画面も USB シリアルも QR も止まり本体ごと復帰しなかった。
// 不採用。入切のフラグも持たず、どの状態でもライトスリープを指示しない
TEST(power_never_light_sleeps) {
  PowerManager pm(defaults(), 0);
  CHECK(!at(pm, kActiveHoldMs).light_sleep);  // 待機
  CHECK(!at(pm, 6 * kMin).light_sleep);       // 減光
  CHECK(!at(pm, 20 * kMin).light_sleep);      // 消灯
}

TEST(power_light_sleep_off_by_default) {
  PowerManager pm(defaults(), 0);
  CHECK(!at(pm, 20 * kMin).light_sleep);
}

TEST(power_no_light_sleep_while_settings_open) {
  PowerManager pm(defaults(), 0);
  PowerInputs in;
  in.now_ms = 20 * kMin;
  in.settings_open = true;
  CHECK(!pm.update(in).light_sleep);
}

// M2 実測: CPU を 40MHz / 20MHz に落とすと MIPI-DSI の送出が間に合わず
// (`E lcd.dsi: ... underrun happens`) ハードウェアのウォッチドッグで再起動する。どの状態でも 360MHz のまま
TEST(power_cpu_stays_fast_in_every_state) {
  PowerManager pm(defaults(), 0);
  CHECK_EQ(at(pm, kActiveHoldMs).cpu_mhz, kCpuFastMhz);  // 待機
  CHECK_EQ(at(pm, 5 * kMin).cpu_mhz, kCpuFastMhz);       // 減光
  CHECK_EQ(at(pm, 15 * kMin).cpu_mhz, kCpuFastMhz);      // 消灯
}

TEST(power_set_config_changes_brightness) {
  PowerManager pm(defaults(), 0);
  Config c = defaults();
  c.brightness_pct = 90;
  pm.setConfig(c);
  CHECK_EQ(at(pm, kActiveHoldMs).brightness_pct, uint8_t{90});
}
