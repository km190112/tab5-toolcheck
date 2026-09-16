// 設定画面に入る 4 桁の PIN の入力と、失敗が続いたときのロック (docs/設計.md「設定画面」: PIN 5 回失敗で 1 分ロック)。
// Arduino 非依存。PIN の中身は設定値 (Config::pin) を呼び出し側が渡す。
#pragma once

#include <cstdint>

namespace toolcheck {

constexpr uint8_t kPinLength = 4;
constexpr uint8_t kPinMaxFailures = 5;  // これだけ続けて間違えたらロックする
constexpr uint32_t kPinLockMs = 60000;  // ロックの長さ

enum class PinResult : uint8_t {
  Incomplete,  // まだ 4 桁そろっていない (数字以外は読み捨てる)
  Accepted,    // 合っていた
  Rejected,    // 間違っていた (入力は消える)
  Locked,      // ロック中なので受け付けなかった
};

class PinLock {
 public:
  // 数字を 1 つ足す。4 桁そろったら pin と照合して結果を返し、入力を消す
  PinResult pressDigit(char digit, const char* pin, uint32_t now_ms);
  void backspace();
  void clear();

  uint8_t length() const;  // いま入っている桁数
  uint8_t failures() const;
  bool locked(uint32_t now_ms) const;
  uint32_t lockRemainingMs(uint32_t now_ms) const;

 private:
  char entry_[kPinLength + 1] = {0};
  uint8_t length_ = 0;
  uint8_t failures_ = 0;
  bool locking_ = false;
  uint32_t lock_start_ms_ = 0;
};

}  // namespace toolcheck
