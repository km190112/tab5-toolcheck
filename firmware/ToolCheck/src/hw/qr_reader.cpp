#include "qr_reader.h"

#include <Arduino.h>
#include <Wire.h>

#include <cstring>

namespace toolcheck {
namespace {

constexpr uint16_t kAddr = 0x21;  // Port A の Wire は PortABus::begin が開き、PaHub の ch もそこで繋ぐ

constexpr uint16_t kTriggerReg = 0x0000;
constexpr uint16_t kReadyReg = 0x0010;
constexpr uint16_t kLengthReg = 0x0020;
constexpr uint16_t kTriggerModeReg = 0x0030;
constexpr uint16_t kDataReg = 0x1000;
constexpr uint16_t kFirmwareReg = 0x00FE;

constexpr uint8_t kAutoScanMode = 0;    // M5UnitQRCode の AUTO_SCAN_MODE
constexpr uint8_t kManualScanMode = 1;  // MANUAL_SCAN_MODE

constexpr uint8_t kModeRetry = 3;
constexpr uint32_t kModeSettleMs = 50;      // STM32F030 のモード切替待ち
constexpr uint8_t kStuckLimit = 20;         // READY なのに読めない回数 → 作り直す
constexpr uint8_t kRecoverLimit = 3;        // 作り直しても読めない回数 → failed
constexpr uint8_t kInitRetry = 10;
constexpr uint32_t kRetryMs = 1000;         // スキャンを貼り直す間隔
constexpr uint32_t kRetryFailedMs = 5000;   // 読めないと分かった後の間隔
constexpr uint32_t kDutyPeriodMs = 1400;    // 別の機器 (M5Stack Basic) で使っていた値
constexpr uint32_t kDutyWindowMs = 700;
constexpr uint32_t kDiscardAfterAutoMs = 500;

bool reached(uint32_t now_ms, uint32_t at_ms) { return static_cast<int32_t>(now_ms - at_ms) >= 0; }

}  // namespace

bool QrReader::begin(uint32_t now_ms) {
  for (uint8_t i = 0; i < kInitRetry && !present_; ++i) {
    present_ = probe();
    if (!present_) delay(200);
  }
  if (!present_) {
    retry_at_ = now_ms + kRetryFailedMs;
    return false;
  }
  writeReg(kTriggerReg, 0);
  scanning_ = setModeVerified(kAutoScanMode);
  if (!scanning_) retry_at_ = now_ms + kRetryMs;
  return scanning_;
}

size_t QrReader::poll(uint32_t now_ms, char* out, size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  out[0] = '\0';
  if (!present_ || !scanning_) {
    if (!reached(now_ms, retry_at_)) return 0;
    if (!present_) present_ = probe();
    if (present_) {
      writeReg(kTriggerReg, 0);
      scanning_ = setModeVerified(duty_ ? kManualScanMode : kAutoScanMode);
    }
    retry_at_ = now_ms + ((present_ && !failed_) ? kRetryMs : kRetryFailedMs);
    if (!scanning_) return 0;
  }
  dutyTick(now_ms);
  if (discard_armed_ && now_ms - auto_at_ms_ >= kDiscardAfterAutoMs) discard_armed_ = false;

  uint8_t ready = 0;
  if (!readReg(kReadyReg, &ready, 1)) {
    ++errors_;
    noteStuck(now_ms);
    return 0;
  }
  if (ready == 0) {
    stuck_ = 0;
    return 0;
  }
  uint8_t len_buf[2] = {0, 0};
  if (!readReg(kLengthReg, len_buf, 2)) {
    ++errors_;
    noteStuck(now_ms);
    return 0;
  }
  const uint16_t length = static_cast<uint16_t>(len_buf[0] | (len_buf[1] << 8));
  const bool bad_length = (length == 0 || length > kQrReadMax);
  // READY が立った以上、長さが異常でも必ずデータを読み出して落とす
  const size_t read_len = bad_length ? kQrReadMax : length;
  uint8_t data[kQrReadMax + 1] = {0};
  const bool data_ok = readReg(kDataReg, data, read_len);
  if (!data_ok || bad_length) {
    ++errors_;
    Serial.printf("[QR] 読めませんでした (ready=%u length=%u data_ok=%d)\n", static_cast<unsigned>(ready),
                  static_cast<unsigned>(length), static_cast<int>(data_ok));
    noteStuck(now_ms);
    return 0;
  }
  stuck_ = 0;
  recover_count_ = 0;
  failed_ = false;
  if (discard_armed_) {
    ++discards_;
    Serial.printf("[QR] 自動スキャンにした直後なので読み捨てました (%u バイト)\n", static_cast<unsigned>(length));
    return 0;
  }
  const size_t n = (length < cap - 1) ? length : cap - 1;
  std::memcpy(out, data, n);
  out[n] = '\0';
  ++reads_;
  return n;
}

void QrReader::setDuty(bool on, uint32_t now_ms) {
  if (on == duty_) return;
  duty_ = on;
  duty_open_ = false;
  duty_at_ = now_ms;
  if (!present_) return;
  writeReg(kTriggerReg, 0);
  scanning_ = setModeVerified(on ? kManualScanMode : kAutoScanMode);
  if (!scanning_) retry_at_ = now_ms + kRetryMs;
}

bool QrReader::present() const { return present_; }
bool QrReader::failed() const { return failed_; }
uint32_t QrReader::readCount() const { return reads_; }
uint32_t QrReader::errorCount() const { return errors_; }
uint32_t QrReader::discardCount() const { return discards_; }
uint8_t QrReader::firmwareVersion() const { return fw_; }

bool QrReader::writeReg(uint16_t reg, uint8_t value) {
  Wire.beginTransmission(static_cast<uint8_t>(kAddr));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool QrReader::readReg(uint16_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(static_cast<uint8_t>(kAddr));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  Wire.write(static_cast<uint8_t>(reg >> 8));
  if (Wire.endTransmission(false) != 0) return false;
  const size_t got = Wire.requestFrom(kAddr, len, true);
  if (got != len) {
    while (Wire.available() > 0) Wire.read();
    return false;
  }
  for (size_t i = 0; i < len; ++i) buf[i] = static_cast<uint8_t>(Wire.read());
  return true;
}

bool QrReader::probe() {
  Wire.beginTransmission(static_cast<uint8_t>(kAddr));
  if (Wire.endTransmission() != 0) return false;
  uint8_t fw = 0;
  if (readReg(kFirmwareReg, &fw, 1)) fw_ = fw;
  return true;
}

bool QrReader::setModeVerified(uint8_t mode) {
  for (uint8_t i = 0; i < kModeRetry; ++i) {
    if (writeReg(kTriggerModeReg, mode)) {
      delay(kModeSettleMs);
      uint8_t back = 0xFF;
      if (readReg(kTriggerModeReg, &back, 1) && back == mode) {
        if (mode == kAutoScanMode) {
          discard_armed_ = true;
          auto_at_ms_ = millis();
        }
        return true;
      }
    }
    Serial.printf("[QR] トリガモードの書き込みをやり直します (%u/%u)\n", static_cast<unsigned>(i + 1),
                  static_cast<unsigned>(kModeRetry));
  }
  Serial.println("[QR] トリガモードを書けませんでした (I2C)");
  return false;
}

void QrReader::noteStuck(uint32_t now_ms) {
  if (++stuck_ >= kStuckLimit) recover(now_ms);
}

void QrReader::recover(uint32_t now_ms) {
  stuck_ = 0;
  Serial.println("[QR] 読めない状態が続いたので、ユニットを作り直します");
  writeReg(kTriggerReg, 0);
  present_ = probe();
  scanning_ = present_ && setModeVerified(duty_ ? kManualScanMode : kAutoScanMode);
  // 作り直しの I2C が通っても読める保証は無いので、回数を戻すのは実際に 1 枚読めたときだけ
  if (++recover_count_ >= kRecoverLimit) {
    if (!failed_) Serial.println("[QR] 作り直しても読めません (配線を確かめてください)");
    failed_ = true;
  }
  retry_at_ = now_ms + (failed_ ? kRetryFailedMs : kRetryMs);
}

void QrReader::dutyTick(uint32_t now_ms) {
  if (!duty_) return;
  if (!duty_open_) {
    if (reached(now_ms, duty_at_)) {
      writeReg(kTriggerReg, 1);
      duty_open_ = true;
      duty_at_ = now_ms + kDutyWindowMs;
    }
  } else if (reached(now_ms, duty_at_)) {
    writeReg(kTriggerReg, 0);
    duty_open_ = false;
    duty_at_ = now_ms + (kDutyPeriodMs - kDutyWindowMs);
  }
}

}  // namespace toolcheck
