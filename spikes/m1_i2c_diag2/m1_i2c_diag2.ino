/*
 * M1 I2C の切り分け 2: M5.begin() の前後で、Port A の Wire (I2C0) の読み取りがどう変わるか
 *
 * これまで: M5Unified 無し (m1_i2c_diag) なら QR ユニットを読める。M5Unified あり (m1_bringup) では
 *           読み取りが 0xFF になり、Wire の i2c_master ISR で落ちる。
 * ここで確かめる:
 *   A. M5.begin() の前に Wire で読めるか
 *   B. M5.begin() の直後に同じ Wire で読めるか
 *   C. Wire.end() → Wire.begin() で開き直すと読めるか
 *   D. loop で M5.update() (タッチ = 内部 I2C) を回しながら読めるか (upd on|off で切替)
 *
 * シリアルコマンド: upd on|off / rebegin / auto on|off / once
 */
#include <M5Unified.h>
#include <Wire.h>

static const int     PORTA_SDA = 53;
static const int     PORTA_SCL = 54;
static const uint8_t QR_ADDR   = 0x21;

bool     useUpdate = true;
bool     autoRun   = true;
String   lineBuf   = "";
uint32_t lastRun   = 0;
uint32_t okCount   = 0;
uint32_t ngCount   = 0;

int readReg(uint16_t reg, uint8_t* out, uint8_t len) {
  Wire.beginTransmission(QR_ADDR);
  Wire.write((uint8_t)(reg & 0xFF));
  Wire.write((uint8_t)(reg >> 8));
  uint8_t et = Wire.endTransmission(false);
  if (et != 0) {
    Serial.printf("  endTransmission(reg=0x%04X) -> %u\n", reg, et);
    return -1000 - (int)et;
  }
  size_t n = Wire.requestFrom((uint16_t)QR_ADDR, (size_t)len, true);
  int got = 0;
  while (Wire.available() && got < len) out[got++] = (uint8_t)Wire.read();
  if (n != len || got != len) {
    Serial.printf("  requestFrom(reg=0x%04X len=%u) -> n=%u got=%d\n", reg, len, (unsigned)n, got);
  }
  return got;
}

void readAll(const char* label) {
  Wire.beginTransmission(QR_ADDR);
  uint8_t probe = Wire.endTransmission();
  uint8_t fw = 0, mode = 0, ready = 0;
  int nfw = readReg(0x00FE, &fw, 1);
  int nmode = readReg(0x0030, &mode, 1);
  int nready = readReg(0x0010, &ready, 1);
  bool ok = (probe == 0 && nfw == 1 && nmode == 1 && nready == 1);
  ok ? okCount++ : ngCount++;
  Serial.printf("#DIAG %s probe=%u fw=%d:0x%02X mode=%d:%u ready=%d:%u upd=%d ok=%lu ng=%lu\n", label, probe, nfw, fw,
                nmode, mode, nready, ready, (int)useUpdate, (unsigned long)okCount, (unsigned long)ngCount);
}

void handleCommand(const String& cmd) {
  Serial.printf("#CMD %s\n", cmd.c_str());
  if (cmd == "upd on") {
    useUpdate = true;
  } else if (cmd == "upd off") {
    useUpdate = false;
  } else if (cmd == "rebegin") {
    Wire.end();
    delay(20);
    Serial.printf("#OK Wire.begin=%d\n", (int)Wire.begin(PORTA_SDA, PORTA_SCL, 100000));
  } else if (cmd == "auto on") {
    autoRun = true;
  } else if (cmd == "auto off") {
    autoRun = false;
  } else if (cmd == "once") {
    readAll("once");
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
  // PC がポートを開くまで待つ (setup の出力を取りこぼさないため。最大 12 秒)
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 12000) delay(10);
  delay(500);

  Serial.println("#STEP A: M5.begin() の前");
  Serial.printf("#INFO Wire.begin=%d\n", (int)Wire.begin(PORTA_SDA, PORTA_SCL, 100000));
  readAll("A");
  readAll("A");

  auto cfg = M5.config();
  cfg.internal_imu = false;
  cfg.internal_mic = false;
  cfg.external_display_value = 0;
  M5.begin(cfg);
  Serial.println("#STEP B: M5.begin() の直後");
  readAll("B");
  readAll("B");

  Serial.println("#STEP C: Wire を開き直す");
  Wire.end();
  delay(20);
  Serial.printf("#INFO Wire.begin=%d\n", (int)Wire.begin(PORTA_SDA, PORTA_SCL, 100000));
  readAll("C");
  readAll("C");
  Serial.println("#READY m1_i2c_diag2");
}

void loop() {
  pollSerial();
  if (useUpdate) M5.update();
  if (autoRun && millis() - lastRun >= 1000) {
    lastRun = millis();
    readAll("D");
  }
  delay(5);
}
