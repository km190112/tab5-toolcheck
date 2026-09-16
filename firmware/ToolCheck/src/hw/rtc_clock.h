// RTC (RX8130CE、内部 I2C 0x32) を UTC で読み書きする (docs/設計.md「時刻」)。
// 読むのは起動時と 60 秒ごとだけで、その間は millis() で足す (カメラの取り込み中に内部 I2C を使わないよう、後で止められるようにする)。
// 時刻が狂ったかは「読めない / 2026 年より前 / フラグレジスタ 0x1D の VLF (bit1、発振が止まった) か VBLF (bit7、バックアップ電圧低下)」で見る。
// M5Unified の getVoltLow() は VBLF しか見ないので 0x1D を直接読む。電源断で実際にどちらが立つかは未確認 (M3 の 5 時間では立たなかった)。
#pragma once

#include <cstdint>

namespace toolcheck {

constexpr uint8_t kRtcFlagVlf = 1 << 1;
constexpr uint8_t kRtcFlagVblf = 1 << 7;

class RtcClock {
 public:
  bool begin(uint32_t now_ms);
  // 60 秒ごとに読み直す
  void update(uint32_t now_ms);

  // UTC の UNIX 秒 (最後に読んだ値 + 経過)。読めていなければ 0
  int64_t now(uint32_t now_ms) const;
  bool ok() const;
  uint8_t flags() const;  // 最後に読んだ 0x1D
  bool timeLost(uint32_t now_ms) const;

  // UTC で書き、VLF と VBLF を落として読み直す。2026 年より前・書けない・読み直せないなら false
  bool set(int64_t epoch, uint32_t now_ms);

 private:
  bool read(uint32_t now_ms);

  bool ok_ = false;
  int64_t epoch_ = 0;
  uint32_t read_ms_ = 0;
  uint8_t flags_ = 0;
};

}  // namespace toolcheck
