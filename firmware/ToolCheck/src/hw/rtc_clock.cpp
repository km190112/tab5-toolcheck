#include "rtc_clock.h"

#include <M5Unified.h>

#include "../core/time_format.h"

namespace toolcheck {
namespace {

constexpr uint8_t kRtcAddress = 0x32;
constexpr uint8_t kRtcFlagRegister = 0x1D;
constexpr uint32_t kRtcFreq = 400000;
constexpr uint32_t kRereadMs = 60000;

}  // namespace

bool RtcClock::begin(uint32_t now_ms) { return read(now_ms); }

void RtcClock::update(uint32_t now_ms) {
  if (now_ms - read_ms_ >= kRereadMs) read(now_ms);
}

int64_t RtcClock::now(uint32_t now_ms) const {
  if (!ok_) return 0;
  return epoch_ + static_cast<int64_t>((now_ms - read_ms_) / 1000);
}

bool RtcClock::ok() const { return ok_; }

uint8_t RtcClock::flags() const { return flags_; }

bool RtcClock::timeLost(uint32_t now_ms) const {
  return !ok_ || (flags_ & (kRtcFlagVlf | kRtcFlagVblf)) != 0 || !isValidEpoch(now(now_ms));
}

bool RtcClock::set(int64_t epoch, uint32_t now_ms) {
  LocalDateTime t;
  if (!M5.Rtc.isEnabled() || !toLocalDateTime(epoch, 0, &t)) return false;
  m5::rtc_datetime_t dt;
  dt.date.year = static_cast<int16_t>(t.year);
  dt.date.month = static_cast<int8_t>(t.month);
  dt.date.date = static_cast<int8_t>(t.day);
  dt.date.weekDay = static_cast<int8_t>(t.weekday);
  dt.time.hours = static_cast<int8_t>(t.hour);
  dt.time.minutes = static_cast<int8_t>(t.minute);
  dt.time.seconds = static_cast<int8_t>(t.second);
  M5.Rtc.setDateTime(dt);
  // 0x1D は 0 を書いたビットだけが落ち、1 を書いたビットはそのまま (M5Unified の clearIRQ と同じ書き方)
  M5.In_I2C.writeRegister8(kRtcAddress, kRtcFlagRegister, static_cast<uint8_t>(~(kRtcFlagVlf | kRtcFlagVblf)),
                           kRtcFreq);
  return read(now_ms) && isValidEpoch(epoch_);
}

bool RtcClock::read(uint32_t now_ms) {
  read_ms_ = now_ms;
  if (!M5.Rtc.isEnabled()) {
    ok_ = false;
    return false;
  }
  const m5::rtc_datetime_t dt = M5.Rtc.getDateTime();
  LocalDateTime t;
  t.year = dt.date.year;
  t.month = dt.date.month;
  t.day = dt.date.date;
  t.hour = dt.time.hours;
  t.minute = dt.time.minutes;
  t.second = dt.time.seconds;
  int64_t epoch = 0;
  if (!fromLocalDateTime(t, 0, &epoch)) {  // 日付として成り立たない値 (読み誤り)
    ok_ = false;
    return false;
  }
  epoch_ = epoch;
  flags_ = M5.In_I2C.readRegister8(kRtcAddress, kRtcFlagRegister, kRtcFreq);
  ok_ = true;
  return true;
}

}  // namespace toolcheck
