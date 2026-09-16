// Unit QRCode (STM32F030、Port A の I2C 0x21) を読む自前のドライバ (docs/設計.md「QR の判別」、spikes/README.md の M1・M2)。
// M5UnitQRCode 1.0.0 は使わない (速度を uint8_t に切り詰めて 160Hz にする・requestFrom の失敗を検出しない)。
// 別の機器 (M5Stack Basic) で同じユニットを使ったときの対策を移植する:
//   - READY は 1 とは限らない (2 も出る)。0 以外なら必ずデータを読み出して落とす (読まないと以後どの QR も読めなくなる)
//   - 長さが異常でも上限まで読み出して落とす
//   - トリガモードは書いたら読み戻して確かめる (3 回まで)
//   - 読めない状態が 20 回続いたら作り直す。3 回作り直しても 1 枚も読めなければ failed() (画面に出す)
// 手動 → 自動の切替の直後に、かざしていないのに `"aA` が読めたことがある (M2) ので、自動にしてから kDiscardAfterAutoMs の読み取りは捨てる。
#pragma once

#include <cstddef>
#include <cstdint>

namespace toolcheck {

constexpr size_t kQrReadMax = 64;  // 一度に読み出す上限 (Wire の受信バッファに収める)

class QrReader {
 public:
  // PortABus::begin (Wire を開いて PaHub の ch を繋ぐ) の後に呼ぶ。ユニットを探して自動スキャンにする
  bool begin(uint32_t now_ms);

  // 読めたコードを out に入れて長さを返す (無ければ 0)。out は NUL 終端 (cap - 1 バイトまで)
  size_t poll(uint32_t now_ms, char* out, size_t cap);

  // 間欠読み取り (1400ms ごとに 700ms だけ読む。減光・消灯中に 6 段目で使う)。false で連続の自動スキャンに戻す
  void setDuty(bool on, uint32_t now_ms);

  bool present() const;  // ユニットが応答した
  bool failed() const;   // 作り直しても読めない
  uint32_t readCount() const;
  uint32_t errorCount() const;
  uint32_t discardCount() const;
  uint8_t firmwareVersion() const;

 private:
  bool writeReg(uint16_t reg, uint8_t value);
  bool readReg(uint16_t reg, uint8_t* buf, size_t len);
  bool probe();
  bool setModeVerified(uint8_t mode);
  void noteStuck(uint32_t now_ms);
  void recover(uint32_t now_ms);
  void dutyTick(uint32_t now_ms);

  bool present_ = false;
  bool scanning_ = false;
  bool failed_ = false;
  bool duty_ = false;
  bool duty_open_ = false;
  uint32_t duty_at_ = 0;
  bool discard_armed_ = false;
  uint32_t auto_at_ms_ = 0;
  uint8_t stuck_ = 0;
  uint8_t recover_count_ = 0;
  uint32_t retry_at_ = 0;
  uint32_t reads_ = 0;
  uint32_t errors_ = 0;
  uint32_t discards_ = 0;
  uint8_t fw_ = 0;
};

}  // namespace toolcheck
