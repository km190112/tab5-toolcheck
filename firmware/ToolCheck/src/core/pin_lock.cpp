#include "pin_lock.h"

#include <cstring>

namespace toolcheck {

PinResult PinLock::pressDigit(char digit, const char* pin, uint32_t now_ms) {
  if (locked(now_ms)) return PinResult::Locked;
  if (locking_) {  // ロックが明けたので数え直す
    locking_ = false;
    failures_ = 0;
  }
  if (digit < '0' || digit > '9') return PinResult::Incomplete;
  entry_[length_++] = digit;
  entry_[length_] = '\0';
  if (length_ < kPinLength) return PinResult::Incomplete;

  const bool ok = pin != nullptr && std::strncmp(entry_, pin, kPinLength) == 0 && pin[kPinLength] == '\0';
  clear();
  if (ok) {
    failures_ = 0;
    return PinResult::Accepted;
  }
  if (++failures_ >= kPinMaxFailures) {
    locking_ = true;
    lock_start_ms_ = now_ms;
  }
  return PinResult::Rejected;
}

void PinLock::backspace() {
  if (length_ == 0) return;
  entry_[--length_] = '\0';
}

void PinLock::clear() {
  length_ = 0;
  entry_[0] = '\0';
}

uint8_t PinLock::length() const { return length_; }

uint8_t PinLock::failures() const { return failures_; }

bool PinLock::locked(uint32_t now_ms) const { return locking_ && now_ms - lock_start_ms_ < kPinLockMs; }

uint32_t PinLock::lockRemainingMs(uint32_t now_ms) const {
  return locked(now_ms) ? kPinLockMs - (now_ms - lock_start_ms_) : 0;
}

}  // namespace toolcheck
