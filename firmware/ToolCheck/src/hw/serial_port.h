// USB シリアル: コマンドを 1 行ずつ読む / 画面を PNG にして base64 で送る (#BEGIN png 〜 #END png)。
// 機械向けの行は '#' で始め、人向けのログは "[タグ] 日本語" (CLAUDE.md の規約)。
#pragma once

#include <cstddef>
#include <cstdint>

namespace toolcheck {

class SerialPort {
 public:
  // 改行までたまったら、前後の空白を除いて line に写して true (空行は返さない)。
  // 長すぎた行は捨てて "#ERR" を出す。CR は無視する
  bool poll(char* line, size_t cap);

  // 送信リングの大きさ (setup で Serial.begin の前に setTxBufferSize する)。
  // 書き込みの待ちは 0 にする (setTxTimeoutMs(0)): PC に繋いだままポートを閉じていると誰も読まず、
  // 既定の 100ms では一杯のリングへの書き込みが 1 回 2 秒止まり、dump でメインループが 20 秒止まった (2026-09-14 実機)。
  // 待ちが 0 だと、空きの無い行はまるごと捨てる
  static constexpr size_t kTxBufferSize = 4096;

  static void sendScreenshot();

  // 送信リングに n バイト入る空きができるまで待つ (timeout_ms で諦めて false)。
  // HWCDC は USB が抜けたと一瞬でも見えると (SOF の見張りのゆらぎ)、リングが一杯なら古いバイトを捨てて新しいバイトを入れる。
  // まとめて出すとき (スクリーンショット・dump) は、書く前にこれで待てば捨てられない (2026-09-14 実機で確認)
  static bool waitWritable(size_t n, uint32_t timeout_ms);

  // 送信リングが空くまで待つ (timeout_ms まで)。再起動の前に使う
  // (待ちが 0 だと Serial.flush() は送り終える前にリングを捨てる)
  static void drain(uint32_t timeout_ms);

 private:
  char buf_[256] = {0};
  size_t len_ = 0;
  bool overflow_ = false;
};

}  // namespace toolcheck
