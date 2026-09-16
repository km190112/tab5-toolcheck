// pin_lock: 設定画面に入る 4 桁の PIN と、5 回失敗で 1 分ロック (docs/設計.md「設定画面」)
#include <cstdint>

#include "core/pin_lock.h"
#include "testing.h"

using toolcheck::kPinLockMs;
using toolcheck::kPinMaxFailures;
using toolcheck::PinLock;
using toolcheck::PinResult;

namespace {

const uint32_t kT0 = 5000;
const char* const kPin = "4821";

// 4 桁を順に押し、最後の結果を返す
PinResult enter(PinLock& lock, const char* digits, uint32_t now_ms) {
  PinResult r = PinResult::Incomplete;
  for (int i = 0; digits[i] != '\0'; ++i) r = lock.pressDigit(digits[i], kPin, now_ms);
  return r;
}

}  // namespace

TEST(pin_starts_empty_and_unlocked) {
  PinLock lock;
  CHECK_EQ(lock.length(), uint8_t{0});
  CHECK_EQ(lock.failures(), uint8_t{0});
  CHECK(!lock.locked(kT0));
  CHECK_EQ(lock.lockRemainingMs(kT0), uint32_t{0});
}

TEST(pin_digits_accumulate_until_four) {
  PinLock lock;
  CHECK_EQ(lock.pressDigit('4', kPin, kT0), PinResult::Incomplete);
  CHECK_EQ(lock.pressDigit('8', kPin, kT0), PinResult::Incomplete);
  CHECK_EQ(lock.pressDigit('2', kPin, kT0), PinResult::Incomplete);
  CHECK_EQ(lock.length(), uint8_t{3});
}

TEST(pin_correct_is_accepted_and_clears_entry) {
  PinLock lock;
  CHECK_EQ(enter(lock, "4821", kT0), PinResult::Accepted);
  CHECK_EQ(lock.length(), uint8_t{0});
  CHECK_EQ(lock.failures(), uint8_t{0});
}

TEST(pin_wrong_is_rejected_and_counts) {
  PinLock lock;
  CHECK_EQ(enter(lock, "1234", kT0), PinResult::Rejected);
  CHECK_EQ(lock.length(), uint8_t{0});
  CHECK_EQ(lock.failures(), uint8_t{1});
  CHECK(!lock.locked(kT0));
}

TEST(pin_accepted_resets_failures) {
  PinLock lock;
  enter(lock, "0000", kT0);
  enter(lock, "1111", kT0);
  CHECK_EQ(enter(lock, "4821", kT0), PinResult::Accepted);
  CHECK_EQ(lock.failures(), uint8_t{0});
}

TEST(pin_non_digits_are_ignored) {
  PinLock lock;
  CHECK_EQ(lock.pressDigit('a', kPin, kT0), PinResult::Incomplete);
  CHECK_EQ(lock.pressDigit(' ', kPin, kT0), PinResult::Incomplete);
  CHECK_EQ(lock.length(), uint8_t{0});
  CHECK_EQ(enter(lock, "48x21", kT0), PinResult::Accepted);
}

TEST(pin_backspace_and_clear) {
  PinLock lock;
  enter(lock, "48", kT0);
  lock.backspace();
  CHECK_EQ(lock.length(), uint8_t{1});
  CHECK_EQ(enter(lock, "821", kT0), PinResult::Accepted);  // 4 + 821
  enter(lock, "12", kT0);
  lock.clear();
  CHECK_EQ(lock.length(), uint8_t{0});
  lock.backspace();  // 空で消しても何も起きない
  CHECK_EQ(lock.length(), uint8_t{0});
}

TEST(pin_fifth_failure_locks_for_one_minute) {
  PinLock lock;
  for (int i = 0; i < kPinMaxFailures - 1; ++i) {
    CHECK_EQ(enter(lock, "0000", kT0), PinResult::Rejected);
    CHECK(!lock.locked(kT0));
  }
  CHECK_EQ(enter(lock, "0000", kT0 + 100), PinResult::Rejected);
  CHECK(lock.locked(kT0 + 100));
  CHECK_EQ(lock.lockRemainingMs(kT0 + 100), kPinLockMs);
  CHECK(lock.locked(kT0 + 100 + kPinLockMs - 1));
  CHECK_EQ(lock.lockRemainingMs(kT0 + 100 + kPinLockMs - 1), uint32_t{1});
  CHECK(!lock.locked(kT0 + 100 + kPinLockMs));
  CHECK_EQ(lock.lockRemainingMs(kT0 + 100 + kPinLockMs), uint32_t{0});
}

TEST(pin_locked_ignores_digits_even_correct_ones) {
  PinLock lock;
  for (int i = 0; i < kPinMaxFailures; ++i) enter(lock, "0000", kT0);
  CHECK_EQ(lock.pressDigit('4', kPin, kT0 + 1000), PinResult::Locked);
  CHECK_EQ(lock.length(), uint8_t{0});
  CHECK_EQ(enter(lock, "4821", kT0 + 2000), PinResult::Locked);
}

TEST(pin_after_lock_counts_from_zero_again) {
  PinLock lock;
  for (int i = 0; i < kPinMaxFailures; ++i) enter(lock, "0000", kT0);
  const uint32_t after = kT0 + kPinLockMs;
  CHECK_EQ(enter(lock, "1111", after), PinResult::Rejected);
  CHECK_EQ(lock.failures(), uint8_t{1});
  CHECK(!lock.locked(after));
  CHECK_EQ(enter(lock, "4821", after + 10), PinResult::Accepted);
}

TEST(pin_lock_works_across_millis_wrap) {
  PinLock lock;
  const uint32_t start = 0xFFFFF000u;
  for (int i = 0; i < kPinMaxFailures; ++i) enter(lock, "0000", start);
  CHECK(lock.locked(start + kPinLockMs - 1));  // 一周している
  CHECK(!lock.locked(start + kPinLockMs));
}

TEST(pin_null_pin_is_rejected) {
  PinLock lock;
  PinResult r = PinResult::Incomplete;
  for (int i = 0; i < 4; ++i) r = lock.pressDigit('1', nullptr, kT0);
  CHECK_EQ(r, PinResult::Rejected);
  CHECK_EQ(lock.failures(), uint8_t{1});
}
