/*
 * M1 I2C の切り分け: M5Unified を使わず、素の Arduino Wire だけで Port A の QR ユニットを読む
 *
 * 目的: m1_bringup で QR の読み取りが毎回 0xFF になり、i2c_master の ISR
 *       (i2c_isr_receive_handler → i2c_ll_read_rxfifo) で Store access fault になった。
 *       原因が「M5Unified との同居」なのか「Wire / ESP32-P4 そのもの」なのかを分ける。
 * 前提: Port A の 5V は Tab5 の IO エキスパンダが保持している (直前のファームで M5Unified が ON にした)。
 *       0x21 が応答しなければ電源が来ていない。
 *
 * シリアルコマンド: rs (レジスタ指定の後に STOP を入れない) / stop (STOP を入れる) /
 *                   speed <hz> / gap <ms> (書いてから読むまでの待ち) / auto on|off / once
 */
#include <Arduino.h>
#include <Wire.h>
#include "esp_log.h"

static const int     PORTA_SDA = 53;
static const int     PORTA_SCL = 54;
static const uint8_t QR_ADDR   = 0x21;

uint32_t speedHz          = 100000;
bool     useRepeatedStart = true;
uint32_t gapMs            = 0;
bool     autoRun          = true;
String   lineBuf          = "";
uint32_t lastRun          = 0;
uint32_t okCount          = 0;
uint32_t ngCount          = 0;

// 16 ビットのレジスタを読む。戻り値は読めたバイト数。負なら endTransmission の失敗 (-1000 - エラーコード)
int readReg(uint16_t reg, uint8_t* out, uint8_t len) {
  Wire.beginTransmission(QR_ADDR);
  Wire.write((uint8_t)(reg & 0xFF));
  Wire.write((uint8_t)(reg >> 8));
  uint8_t et = Wire.endTransmission(!useRepeatedStart);
  if (et != 0) {
    Serial.printf("  endTransmission(reg=0x%04X stop=%d) -> %u\n", reg, (int)!useRepeatedStart, et);
    return -1000 - (int)et;
  }
  if (gapMs) delay(gapMs);
  size_t n = Wire.requestFrom((uint16_t)QR_ADDR, (size_t)len, true);
  int got = 0;
  while (Wire.available() && got < len) out[got++] = (uint8_t)Wire.read();
  if (n != len || got != len) {
    Serial.printf("  requestFrom(reg=0x%04X len=%u) -> n=%u got=%d\n", reg, len, (unsigned)n, got);
  }
  return got;
}

void runOnce() {
  Wire.beginTransmission(QR_ADDR);
  uint8_t probe = Wire.endTransmission();

  uint8_t fw = 0, mode = 0, ready = 0;
  int nfw = readReg(0x00FE, &fw, 1);
  int nmode = readReg(0x0030, &mode, 1);
  int nready = readReg(0x0010, &ready, 1);
  bool ok = (probe == 0 && nfw == 1 && nmode == 1 && nready == 1);
  ok ? okCount++ : ngCount++;
  Serial.printf("#DIAG probe=%u fw=%d:0x%02X mode=%d:%u ready=%d:%u rs=%d speed=%lu gap=%lu ok=%lu ng=%lu\n",
                probe, nfw, fw, nmode, mode, nready, ready, (int)useRepeatedStart,
                (unsigned long)speedHz, (unsigned long)gapMs, (unsigned long)okCount, (unsigned long)ngCount);

  if (nready == 1 && ready != 0) {
    uint8_t lenBuf[2] = {0, 0};
    if (readReg(0x0020, lenBuf, 2) == 2) {
      uint16_t len = (uint16_t)(lenBuf[0] | (lenBuf[1] << 8));
      uint8_t data[65] = {0};
      uint8_t readLen = (len == 0 || len > 64) ? 64 : (uint8_t)len;
      int nd = readReg(0x1000, data, readLen);  // READY を落とすために必ず読み出す
      Serial.printf("#QR len=%u read=%d data=%s\n", len, nd, (char*)data);
    }
  }
}

void handleCommand(const String& cmd) {
  Serial.printf("#CMD %s\n", cmd.c_str());
  if (cmd == "rs") {
    useRepeatedStart = true;
  } else if (cmd == "stop") {
    useRepeatedStart = false;
  } else if (cmd.startsWith("speed ")) {
    speedHz = (uint32_t)cmd.substring(6).toInt();
    Serial.printf("#OK setClock=%d\n", (int)Wire.setClock(speedHz));
  } else if (cmd.startsWith("gap ")) {
    gapMs = (uint32_t)cmd.substring(4).toInt();
  } else if (cmd == "auto on") {
    autoRun = true;
  } else if (cmd == "auto off") {
    autoRun = false;
  } else if (cmd == "once") {
    runOnce();
  } else {
    Serial.println("#ERR unknown command");
  }
}

void pollSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String cmd = lineBuf;
      lineBuf = "";
      cmd.trim();
      if (cmd.length()) handleCommand(cmd);
    } else if (lineBuf.length() < 100) {
      lineBuf += c;
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  esp_log_level_set("*", ESP_LOG_WARN);
  esp_log_level_set("i2c.master", ESP_LOG_DEBUG);
  delay(1500);
  bool ok = Wire.begin(PORTA_SDA, PORTA_SCL, speedHz);
  Serial.printf("#INFO Wire.begin=%d timeout_ms=%u\n", (int)ok, (unsigned)Wire.getTimeOut());
  Serial.println("#READY m1_i2c_diag");
}

void loop() {
  pollSerial();
  if (autoRun && millis() - lastRun >= 1000) {
    lastRun = millis();
    runOnce();
  }
  delay(5);
}
