#include "serial_port.h"

#include <M5Unified.h>

#include <cstdlib>
#include <cstring>

namespace toolcheck {

namespace {

// 1 行ぶんの空きを待つ上限。PC が本当に抜けていればリングは減らないので、ここで諦める
constexpr uint32_t kScreenshotWaitMs = 500;
// 待ちきれなかった後、待たずに答える時間
constexpr uint32_t kStallHoldMs = 2000;

}  // namespace

bool SerialPort::poll(char* line, size_t cap) {
  while (Serial.available() > 0) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r') continue;
    if (ch != '\n') {
      if (len_ + 1 < sizeof(buf_)) {
        buf_[len_++] = ch;
      } else {
        overflow_ = true;
      }
      continue;
    }
    if (overflow_) {
      Serial.printf("#ERR 行が長すぎます (%u バイトまで)\n", static_cast<unsigned>(sizeof(buf_) - 1));
      overflow_ = false;
      len_ = 0;
      continue;
    }
    size_t begin = 0;
    size_t end = len_;
    while (begin < end && buf_[begin] == ' ') ++begin;
    while (end > begin && buf_[end - 1] == ' ') --end;
    len_ = 0;
    if (begin == end) continue;
    if (end - begin + 1 > cap) {
      Serial.println("#ERR 行が長すぎます");
      continue;
    }
    std::memcpy(line, buf_ + begin, end - begin);
    line[end - begin] = '\0';
    return true;
  }
  return false;
}

bool SerialPort::waitWritable(size_t n, uint32_t timeout_ms) {
  // PC が繋がっていないとリングは減らない。一度待ちきれなかったら、しばらくは待たずに答える
  // (設定画面の「USB シリアル出力」を PC なしで押しても、行ごとに待って画面が止まらないように)
  static bool stalled = false;
  static uint32_t stalled_ms = 0;
  const uint32_t start = millis();
  if (stalled && start - stalled_ms < kStallHoldMs) {
    return Serial.availableForWrite() >= static_cast<int>(n);
  }
  while (Serial.availableForWrite() < static_cast<int>(n)) {
    if (millis() - start >= timeout_ms) {
      stalled = true;
      stalled_ms = millis();
      return false;
    }
    delay(1);
  }
  stalled = false;
  return true;
}

void SerialPort::drain(uint32_t timeout_ms) {
  const uint32_t start = millis();
  while (Serial.availableForWrite() < static_cast<int>(kTxBufferSize) && millis() - start < timeout_ms) {
    delay(1);
  }
}

void SerialPort::sendScreenshot() {
  size_t len = 0;
  uint8_t* png = static_cast<uint8_t*>(M5.Display.createPng(&len, 0, 0, M5.Display.width(), M5.Display.height()));
  if (png == nullptr || len == 0) {
    Serial.println("#ERR screenshot createPng failed");
    return;
  }
  Serial.printf("#BEGIN png %u\n", static_cast<unsigned>(len));
  static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char out[80];
  // USB が抜けたと見えた行の数 (0 以外でも、空きを待ってから書くので落ちない。ゆらぎの記録として出す)
  unsigned unplugged = 0;
  for (size_t i = 0; i < len; i += 57) {
    const size_t n = (len - i < 57) ? (len - i) : 57;
    size_t o = 0;
    for (size_t j = 0; j < n; j += 3) {
      uint32_t v = static_cast<uint32_t>(png[i + j]) << 16;
      if (j + 1 < n) v |= static_cast<uint32_t>(png[i + j + 1]) << 8;
      if (j + 2 < n) v |= static_cast<uint32_t>(png[i + j + 2]);
      out[o++] = kB64[(v >> 18) & 63];
      out[o++] = kB64[(v >> 12) & 63];
      out[o++] = (j + 1 < n) ? kB64[(v >> 6) & 63] : '=';
      out[o++] = (j + 2 < n) ? kB64[v & 63] : '=';
    }
    out[o++] = '\r';
    out[o++] = '\n';
    if (!HWCDC::isPlugged()) ++unplugged;
    if (!waitWritable(o, kScreenshotWaitMs)) {
      // PC が読んでいない。#END png を出さずにやめる (受け取り側は「#END png が来ない」で気づく)
      free(png);
      return;
    }
    Serial.write(out, o);
  }
  if (!waitWritable(48, kScreenshotWaitMs)) {
    free(png);
    return;
  }
  Serial.printf("#END png unplugged=%u\n", unplugged);
  free(png);
}

}  // namespace toolcheck
