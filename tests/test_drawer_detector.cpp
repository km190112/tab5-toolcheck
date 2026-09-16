// drawer_detector: ToF の距離から引き出しの開閉・異常を決める (docs/設計.md「引き出し検知」)
#include "core/drawer_detector.h"
#include "testing.h"

using toolcheck::DrawerDetector;
using toolcheck::DrawerDetectorConfig;
using toolcheck::DrawerEvent;
using toolcheck::DrawerState;

namespace {

const uint16_t kBase = 800;       // 基準距離 (全段閉)
const uint16_t kOpenMm = 650;     // 150mm 縮む = 開の候補
const uint16_t kMiddleMm = 730;   // 70mm 縮む = ヒステリシスの間
const uint16_t kClosedMm = 790;   // 10mm 縮む = 閉の候補

DrawerDetector calibrated() {
  DrawerDetector d{DrawerDetectorConfig{}};
  d.setBaseline(kBase);
  return d;
}

// 開いた状態まで進めて、最後のサンプルの時刻を返す
uint32_t openIt(DrawerDetector& d, uint32_t t) {
  d.update(kOpenMm, true, t);
  d.update(kOpenMm, true, t + 100);
  d.update(kOpenMm, true, t + 200);
  return t + 200;
}

}  // namespace

TEST(drawer_uncalibrated_never_opens) {
  DrawerDetector d{DrawerDetectorConfig{}};
  for (uint32_t i = 0; i < 10; ++i) {
    DrawerEvent e = d.update(100, true, i * 100);
    CHECK(!e.opened);
  }
  CHECK_EQ(d.state(), DrawerState::Uncalibrated);
}

TEST(drawer_closed_after_baseline) {
  auto d = calibrated();
  CHECK_EQ(d.state(), DrawerState::Closed);
}

TEST(drawer_opens_on_third_consecutive_sample) {
  auto d = calibrated();
  CHECK(!d.update(kOpenMm, true, 0).opened);
  CHECK(!d.update(kOpenMm, true, 100).opened);
  CHECK(d.update(kOpenMm, true, 200).opened);
  CHECK_EQ(d.state(), DrawerState::Open);
}

TEST(drawer_single_spike_does_not_open) {
  auto d = calibrated();
  d.update(kOpenMm, true, 0);
  d.update(kOpenMm, true, 100);
  d.update(kClosedMm, true, 200);  // 途切れた
  CHECK(!d.update(kOpenMm, true, 300).opened);
  CHECK(!d.update(kOpenMm, true, 400).opened);
  CHECK(d.update(kOpenMm, true, 500).opened);
}

TEST(drawer_open_event_fires_once) {
  auto d = calibrated();
  uint32_t t = openIt(d, 0);
  CHECK(!d.update(kOpenMm, true, t + 100).opened);
  CHECK(!d.update(kOpenMm, true, t + 200).opened);
  CHECK_EQ(d.state(), DrawerState::Open);
}

TEST(drawer_hysteresis_zone_keeps_open) {
  auto d = calibrated();
  uint32_t t = openIt(d, 0);
  for (uint32_t i = 1; i <= 50; ++i) {
    CHECK(!d.update(kMiddleMm, true, t + i * 100).closed);
  }
  CHECK_EQ(d.state(), DrawerState::Open);
}

TEST(drawer_closes_after_hold_time) {
  auto d = calibrated();
  uint32_t t = openIt(d, 0);
  uint32_t start = t + 100;
  CHECK(!d.update(kClosedMm, true, start).closed);
  CHECK(!d.update(kClosedMm, true, start + 999).closed);
  CHECK(d.update(kClosedMm, true, start + 1000).closed);
  CHECK_EQ(d.state(), DrawerState::Closed);
}

TEST(drawer_close_hold_resets_when_interrupted) {
  auto d = calibrated();
  uint32_t t = openIt(d, 0);
  uint32_t start = t + 100;
  d.update(kClosedMm, true, start);
  d.update(kMiddleMm, true, start + 500);  // 途切れた
  d.update(kClosedMm, true, start + 600);
  CHECK(!d.update(kClosedMm, true, start + 1500).closed);  // 600 から数えて 900ms
  CHECK(d.update(kClosedMm, true, start + 1600).closed);   // 600 から数えて 1000ms
}

TEST(drawer_farther_than_baseline_counts_as_closed) {
  auto d = calibrated();
  uint32_t t = openIt(d, 0);
  d.update(kBase + 50, true, t + 100);
  CHECK(d.update(kBase + 50, true, t + 1100).closed);
}

TEST(drawer_close_hold_across_millis_wraparound) {
  auto d = calibrated();
  uint32_t t = openIt(d, 0xFFFFFC00u);  // もうすぐ一周する
  uint32_t start = t + 100;             // 0xFFFFFD2C
  d.update(kClosedMm, true, start);
  CHECK(!d.update(kClosedMm, true, start + 999).closed);  // 一周した後の値
  CHECK(d.update(kClosedMm, true, start + 1000).closed);
}

TEST(drawer_fault_after_consecutive_invalid_samples) {
  auto d = calibrated();
  for (uint32_t i = 0; i < 9; ++i) {
    CHECK(!d.update(0, false, i * 100).fault_changed);
  }
  CHECK(!d.isFault());
  DrawerEvent e = d.update(0, false, 900);
  CHECK(e.fault_changed);
  CHECK(d.isFault());
}

TEST(drawer_valid_sample_clears_fault) {
  auto d = calibrated();
  for (uint32_t i = 0; i < 10; ++i) d.update(0, false, i * 100);
  CHECK(d.isFault());
  DrawerEvent e = d.update(kClosedMm, true, 1000);
  CHECK(e.fault_changed);
  CHECK(!d.isFault());
}

TEST(drawer_invalid_sample_breaks_open_count) {
  auto d = calibrated();
  d.update(kOpenMm, true, 0);
  d.update(kOpenMm, true, 100);
  d.update(0, false, 200);  // 無効が挟まった
  CHECK(!d.update(kOpenMm, true, 300).opened);
}

TEST(drawer_set_baseline_returns_to_closed) {
  auto d = calibrated();
  openIt(d, 0);
  CHECK_EQ(d.state(), DrawerState::Open);
  d.setBaseline(700);
  CHECK_EQ(d.state(), DrawerState::Closed);
}

TEST(drawer_zero_baseline_means_uncalibrated) {
  auto d = calibrated();
  d.setBaseline(0);
  CHECK_EQ(d.state(), DrawerState::Uncalibrated);
  CHECK(!d.update(100, true, 0).opened);
}

// --- 登録後は反応が消えて 2 秒たってから「開」を受け付ける ---
// 2026-09-16: 登録後に ToF が反応したまま (引き出しを開けたまま・作業者が前にいる) なら、
// 距離が閉の範囲に戻ってから 2 秒続いたら次の開放を受け付ける (途中でまた反応したら数え直し)

TEST(drawer_hold_default_is_2s) { CHECK_EQ(DrawerDetectorConfig{}.rearm_quiet_ms, uint32_t{2000}); }

TEST(drawer_hold_blocks_open_while_reacting) {
  auto d = calibrated();
  d.update(kOpenMm, true, 0);  // 作業者が前にいる
  d.holdOpenUntilQuiet();
  CHECK(d.openHeld());
  for (uint32_t i = 1; i <= 30; ++i) CHECK(!d.update(kOpenMm, true, i * 100).opened);
  CHECK_EQ(d.state(), DrawerState::Closed);
  CHECK(d.openHeld());
}

TEST(drawer_hold_releases_after_quiet_2s) {
  auto d = calibrated();
  d.holdOpenUntilQuiet();
  d.update(kOpenMm, true, 0);
  CHECK(!d.update(kClosedMm, true, 100).rearmed);   // 反応が消えた
  CHECK(!d.update(kClosedMm, true, 2099).rearmed);  // 1.999 秒
  const DrawerEvent e = d.update(kClosedMm, true, 2100);
  CHECK(e.rearmed);
  CHECK(!e.opened);
  CHECK(!d.openHeld());
  CHECK(!d.update(kClosedMm, true, 2200).rearmed);  // 知らせは 1 回だけ
  // 解いた後の開は今までどおり 3 サンプル
  CHECK(!d.update(kOpenMm, true, 2300).opened);
  CHECK(!d.update(kOpenMm, true, 2400).opened);
  CHECK(d.update(kOpenMm, true, 2500).opened);
}

TEST(drawer_hold_quiet_restarts_when_reacting_again) {
  auto d = calibrated();
  d.holdOpenUntilQuiet();
  d.update(kClosedMm, true, 0);
  d.update(kOpenMm, true, 1500);  // また反応
  d.update(kClosedMm, true, 1600);
  CHECK(!d.update(kClosedMm, true, 3599).rearmed);
  CHECK(d.update(kClosedMm, true, 3600).rearmed);
}

TEST(drawer_hold_quiet_restarts_on_middle_or_invalid) {
  auto d = calibrated();
  d.holdOpenUntilQuiet();
  d.update(kClosedMm, true, 0);
  d.update(kMiddleMm, true, 1000);  // 間の値 (閉の範囲の外)
  d.update(kClosedMm, true, 1100);
  CHECK(!d.update(kClosedMm, true, 3099).rearmed);
  d.update(0, false, 3100);  // 無効
  d.update(kClosedMm, true, 3200);
  CHECK(!d.update(kClosedMm, true, 5199).rearmed);
  CHECK(d.update(kClosedMm, true, 5200).rearmed);
}

TEST(drawer_hold_while_open_still_closes_after_1s) {
  auto d = calibrated();
  const uint32_t t = openIt(d, 0);  // 引き出しを開けたまま名札で確定
  d.holdOpenUntilQuiet();
  const uint32_t start = t + 100;
  CHECK(!d.update(kClosedMm, true, start).closed);
  DrawerEvent e = d.update(kClosedMm, true, start + 1000);
  CHECK(e.closed);  // 閉は今までどおり 1 秒
  CHECK(!e.rearmed);
  CHECK(d.openHeld());
  e = d.update(kClosedMm, true, start + 2000);
  CHECK(e.rearmed);  // 閉の範囲に戻ってから 2 秒
}

TEST(drawer_hold_blocks_reopen_right_after_close) {
  auto d = calibrated();
  const uint32_t t = openIt(d, 0);
  d.holdOpenUntilQuiet();
  const uint32_t start = t + 100;
  d.update(kClosedMm, true, start);
  CHECK(d.update(kClosedMm, true, start + 1000).closed);
  // 閉じた直後にまた反応しても開にしない
  CHECK(!d.update(kOpenMm, true, start + 1100).opened);
  CHECK(!d.update(kOpenMm, true, start + 1200).opened);
  CHECK(!d.update(kOpenMm, true, start + 1300).opened);
  CHECK_EQ(d.state(), DrawerState::Closed);
  d.update(kClosedMm, true, start + 1400);
  CHECK(!d.update(kClosedMm, true, start + 3399).rearmed);
  CHECK(d.update(kClosedMm, true, start + 3400).rearmed);
}

TEST(drawer_hold_already_quiet_releases_on_next_sample) {
  auto d = calibrated();
  d.update(kClosedMm, true, 0);
  d.update(kClosedMm, true, 2500);  // もう 2 秒以上静か
  d.holdOpenUntilQuiet();
  CHECK(d.openHeld());
  CHECK(d.update(kClosedMm, true, 2600).rearmed);
  CHECK(!d.openHeld());
}

TEST(drawer_set_baseline_restarts_quiet) {
  auto d = calibrated();
  d.update(kClosedMm, true, 0);
  d.update(kClosedMm, true, 2500);
  d.setBaseline(kBase);  // 判定の再開 (設定画面を閉じた)
  d.holdOpenUntilQuiet();
  CHECK(!d.update(kClosedMm, true, 2600).rearmed);  // 静かな時間は数え直し
  CHECK(!d.update(kClosedMm, true, 4599).rearmed);
  CHECK(d.update(kClosedMm, true, 4600).rearmed);
}

TEST(drawer_hold_quiet_across_millis_wraparound) {
  auto d = calibrated();
  d.holdOpenUntilQuiet();
  const uint32_t start = 0xFFFFFC00u;
  d.update(kClosedMm, true, start);
  CHECK(!d.update(kClosedMm, true, start + 1999).rearmed);  // 一周した後の値
  CHECK(d.update(kClosedMm, true, start + 2000).rearmed);
}

TEST(drawer_without_hold_never_reports_rearmed) {
  auto d = calibrated();
  CHECK(!d.openHeld());
  d.update(kClosedMm, true, 0);
  CHECK(!d.update(kClosedMm, true, 5000).rearmed);
}
