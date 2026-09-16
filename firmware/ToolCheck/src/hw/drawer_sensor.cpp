#include "drawer_sensor.h"

#include <Arduino.h>
#include <VL53L1X.h>
#include <Wire.h>

#include "../core/tof_reading.h"
#include "port_a_bus.h"

namespace toolcheck {
namespace {

constexpr uint16_t kIoTimeoutMs = 100;       // VL53L1X の待ち (既定の 0 は無限に待つ)
constexpr uint32_t kProbeMs = 1000;          // 見つからない間に探す間隔 (応答の無いアドレスは NACK ですぐ返る)
constexpr uint32_t kInitRetryMs = 5000;      // 応答はあるのに初期化できないときにやり直す間隔
constexpr uint8_t kIoErrorLimit = 3;         // I2C の失敗がこれだけ続いたら見失ったとみる
constexpr uint32_t kStallBaseMs = 1000;      // 結果が「これ + 間隔 3 回分」来なければ見失ったとみる
constexpr uint32_t kMinPollMs = 10;
constexpr uint32_t kTimingBudgetUs = 33000;  // 1 回の測定時間。操作中の間隔 50ms に収める (Pololu の init の既定は 50ms)
constexpr uint8_t kVl53l0xModelIdReg = 0xC0;  // Unit ToF (VL53L0X) の型番 (0xEE)。見分けてログに出すだけ

VL53L1X g_tof;  // Unit ToF4M は 1 台だけ

bool reached(uint32_t now_ms, uint32_t at_ms) { return static_cast<int32_t>(now_ms - at_ms) >= 0; }

bool readReg8(uint8_t addr, uint8_t reg, uint8_t* value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<size_t>(1), true) != 1) {
    while (Wire.available() > 0) Wire.read();
    return false;
  }
  *value = static_cast<uint8_t>(Wire.read());
  return true;
}

}  // namespace

void DrawerSensor::begin(bool enabled, const Config& cfg, uint16_t period_ms, uint32_t now_ms) {
  period_ms_ = period_ms;
  setConfig(cfg);
  enabled_ = enabled;
  present_ = false;
  next_probe_ms_ = now_ms;
  if (enabled_) start(now_ms);
}

void DrawerSensor::setEnabled(bool enabled, uint32_t now_ms) {
  if (enabled == enabled_) return;
  enabled_ = enabled;
  if (!enabled_) {
    if (present_) g_tof.stopContinuous();  // 使わない間は測らない
    present_ = false;
    has_sample_ = false;
    return;
  }
  next_probe_ms_ = now_ms;
  start(now_ms);
}

bool DrawerSensor::setConfig(const Config& cfg) {
  if (cfg.tof_baseline_mm == baseline_mm_ && cfg.open_delta_mm == open_delta_mm_ &&
      cfg.close_delta_mm == close_delta_mm_) {
    return false;
  }
  baseline_mm_ = cfg.tof_baseline_mm;
  open_delta_mm_ = cfg.open_delta_mm;
  close_delta_mm_ = cfg.close_delta_mm;
  detector_ = DrawerDetector(drawerDetectorConfigFrom(cfg));
  detector_.setBaseline(baseline_mm_);
  return true;
}

void DrawerSensor::setPeriod(uint16_t period_ms) {
  if (period_ms == period_ms_) return;
  period_ms_ = period_ms;
  if (!present_) return;
  g_tof.stopContinuous();
  g_tof.startContinuous(period_ms_);
  last_result_ms_ = millis();  // 測り直しの最初の結果までを「来ない」と数えない
}

void DrawerSensor::setJudging(bool judging) {
  if (judging == judging_) return;
  judging_ = judging;
  if (judging_) detector_.setBaseline(baseline_mm_);  // 判定を止めていた間の数え途中を捨てる
}

DrawerSensorUpdate DrawerSensor::update(uint32_t now_ms) {
  DrawerSensorUpdate u;
  if (!enabled_) return u;
  if (!present_) {
    if (!reached(now_ms, next_probe_ms_)) return u;
    next_probe_ms_ = now_ms + kProbeMs;
    u.status_changed = start(now_ms);
    return u;
  }
  if (!reached(now_ms, next_poll_ms_)) return u;
  const uint32_t poll_ms = period_ms_ / 2 > kMinPollMs ? period_ms_ / 2 : kMinPollMs;
  next_poll_ms_ = now_ms + poll_ms;

  const bool ready = g_tof.dataReady();
  if (g_tof.last_status != 0) {
    noteIoError(now_ms, &u);
    return u;
  }
  if (!ready) {
    if (now_ms - last_result_ms_ > kStallBaseMs + static_cast<uint32_t>(period_ms_) * 3) {
      lose(now_ms, "結果が来ません");
      u.status_changed = true;
    }
    return u;
  }
  const uint16_t mm = g_tof.read(false);  // 結果を読み、割り込みを消す (待たない)
  if (g_tof.last_status != 0) {
    noteIoError(now_ms, &u);
    return u;
  }
  io_errors_ = 0;
  last_result_ms_ = now_ms;

  const uint8_t status = static_cast<uint8_t>(g_tof.ranging_data.range_status);
  const bool valid = tofRangeStatusUsable(status) && tofDistanceValid(mm);
  has_sample_ = true;
  last_mm_ = mm;
  last_range_status_ = status;
  last_valid_ = valid;
  ++samples_;
  if (!valid) ++invalid_;
  u.sample = true;
  u.mm = mm;
  u.range_status = status;
  u.valid = valid;
  if (!judging_) return u;
  const DrawerEvent ev = detector_.update(mm, valid, now_ms);
  u.opened = ev.opened;
  u.closed = ev.closed;
  u.status_changed = ev.fault_changed;
  u.rearmed = ev.rearmed;
  return u;
}

bool DrawerSensor::enabled() const { return enabled_; }
bool DrawerSensor::present() const { return present_; }
bool DrawerSensor::fault() const { return detector_.isFault(); }
bool DrawerSensor::calibrated() const { return baseline_mm_ != 0; }
bool DrawerSensor::judging() const { return judging_; }
void DrawerSensor::holdOpenUntilQuiet() { detector_.holdOpenUntilQuiet(); }
bool DrawerSensor::openHeld() const { return detector_.openHeld(); }
DrawerState DrawerSensor::state() const { return detector_.state(); }
bool DrawerSensor::hasSample() const { return has_sample_; }
uint16_t DrawerSensor::lastMm() const { return last_mm_; }
uint8_t DrawerSensor::lastRangeStatus() const { return last_range_status_; }
bool DrawerSensor::lastValid() const { return last_valid_; }
uint16_t DrawerSensor::periodMs() const { return period_ms_; }
uint32_t DrawerSensor::initMs() const { return init_ms_; }
uint32_t DrawerSensor::sampleCount() const { return samples_; }
uint32_t DrawerSensor::invalidCount() const { return invalid_; }
uint32_t DrawerSensor::startCount() const { return starts_; }

bool DrawerSensor::start(uint32_t now_ms) {
  Wire.beginTransmission(kTofI2cAddr);
  if (Wire.endTransmission() != 0) return false;  // まだ見えない (PortABus が ch を繋ぐのを待つ)
  const uint32_t t0 = millis();
  g_tof.setTimeout(kIoTimeoutMs);
  const bool ok = g_tof.init() && g_tof.setDistanceMode(VL53L1X::Medium) &&
                  g_tof.setMeasurementTimingBudget(kTimingBudgetUs);
  if (!ok) {
    ++init_failures_;
    if (init_failures_ == 1 || init_failures_ % 12 == 0) {  // 5 秒ごとに流さない (1 回目と 1 分ごと)
      const uint32_t spent = millis() - t0;
      const uint16_t id16 = g_tof.readReg16Bit(VL53L1X::IDENTIFICATION__MODEL_ID);
      uint8_t id8 = 0;
      const bool ok8 = readReg8(kTofI2cAddr, kVl53l0xModelIdReg, &id8);
      Serial.printf("[ToF] 応答はあるのに初期化できませんでした (%lu 回目・%lu ms)。型番: 0x010F=0x%04x (Unit ToF4M の VL53L1X なら 0xEACC) / "
                    "0xC0=0x%02x%s (Unit ToF の VL53L0X なら 0xEE。VL53L0X には対応していません)。%lu 秒ごとにやり直します\n",
                    static_cast<unsigned long>(init_failures_), static_cast<unsigned long>(spent),
                    static_cast<unsigned>(id16), static_cast<unsigned>(id8), ok8 ? "" : " (読めない)",
                    static_cast<unsigned long>(kInitRetryMs / 1000));
    }
    next_probe_ms_ = now_ms + kInitRetryMs;
    return false;
  }
  g_tof.startContinuous(period_ms_);
  init_ms_ = millis() - t0;
  present_ = true;
  io_errors_ = 0;
  last_result_ms_ = millis();
  next_poll_ms_ = last_result_ms_;
  ++starts_;
  Serial.printf("[ToF] 測り始めました (VL53L1X・初期化 %lu ms・間隔 %u ms・%lu 回目)\n", static_cast<unsigned long>(init_ms_),
                static_cast<unsigned>(period_ms_), static_cast<unsigned long>(starts_));
  return true;
}

void DrawerSensor::lose(uint32_t now_ms, const char* why) {
  present_ = false;
  has_sample_ = false;
  io_errors_ = 0;
  next_probe_ms_ = now_ms + kProbeMs;
  Serial.printf("[ToF] 見失いました (%s)。探し直します\n", why);
}

void DrawerSensor::noteIoError(uint32_t now_ms, DrawerSensorUpdate* u) {
  if (++io_errors_ < kIoErrorLimit) return;
  lose(now_ms, "I2C の失敗が続きました");
  u->status_changed = true;
}

}  // namespace toolcheck
