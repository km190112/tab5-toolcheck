/*
 * M2 省電力と描画速度の実機確認 (Tab5 + Unit QRCode)
 *
 * 確かめること:
 *   - 画面の書き換えにかかる時間 (全面を塗り直す / 変わった所だけ) と CPU 周波数 (360 / 40 / 20MHz) の関係
 *   - 40MHz / 20MHz で I2C (QR)・USB シリアル・画面・タッチが動くか。
 *     通信が止まっても調べられるように、結果は RAM の記録 (`log`) にも残し、CPU_REVERT_MS で 360MHz に戻す
 *   - 無線モジュール ESP32-C6 の電源 OFF、USB-A の 5V OFF、パネルスリープ、バックライト、スピーカー
 *   - INA226 の値 (USB 給電だけのときに全体の消費が読めるか)
 *
 * シリアルコマンド (改行終端):
 *   info / scan / screenshot / bench [回数] / mode full|partial / cpu 360|40|20 /
 *   c6 on|off / usba on|off / pwr / bright <0-255> / panel sleep|wake / tone <Hz> <ms> <音量0-255> / log / clear
 */
#include <M5Unified.h>
#include <Wire.h>
#include <M5UnitQRCode.h>
#include <cstdarg>

static const int      PORTA_SDA     = 53;
static const int      PORTA_SCL     = 54;
static const uint8_t  QR_ADDR       = 0x21;
static const uint16_t QR_READ_MAX   = 64;
static const uint32_t QR_POLL_MS    = 20;
static const uint32_t CPU_REVERT_MS = 20000;
static const int      LOG_MAX       = 80;

// 画面の区画 (縦 720x1280)
static const int PERF_Y   = 80;
static const int PERF_H   = 56;
static const int QR_Y     = 150;
static const int QR_H     = 110;
static const int TOUCH_Y  = 270;
static const int TOUCH_H  = 50;
static const int PAD_Y    = 330;
static const uint16_t PAD_BG = 0x2104;  // 暗い灰色

M5UnitQRCodeI2C qrcode;
bool     qrOk        = false;
String   lastQr      = "";
uint32_t qrCount     = 0;
uint32_t qrFailCount = 0;
bool     partialMode = true;
int      touchX      = -1;
int      touchY      = -1;
int      markX       = -1;
int      markY       = -1;
uint32_t lastDrawUs  = 0;
uint32_t lastQrPoll  = 0;
uint32_t cpuRevertAt = 0;
String   lineBuf     = "";
String   evLog[LOG_MAX];
int      evLogCount  = 0;

void pushLog(const String& entry) {
  if (evLogCount < LOG_MAX) {
    evLog[evLogCount++] = entry;
    return;
  }
  for (int i = 1; i < LOG_MAX; i++) evLog[i - 1] = evLog[i];
  evLog[LOG_MAX - 1] = entry;
}

// シリアルに出し、RAM の記録にも残す (40MHz などで USB シリアルが止まっても後で `log` で取り出せる)
void logf(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
  pushLog(String((unsigned long)millis()) + " " + buf);
}

bool setScanMode(bool autoScan) {
  uint8_t mode = autoScan ? AUTO_SCAN_MODE : MANUAL_SCAN_MODE;
  for (int i = 0; i < 3; i++) {
    qrcode.setTriggerMode((qrcode_scan_mode_t)mode);
    delay(50);
    if (qrcode.getTriggerMode() == mode) return true;
  }
  return false;
}

void drawPerfArea() {
  auto& d = M5.Display;
  char buf[96];
  d.fillRect(0, PERF_Y, d.width(), PERF_H, TFT_BLACK);
  d.setFont(&fonts::lgfxJapanGothicP_32);
  d.setTextColor(TFT_CYAN, TFT_BLACK);
  snprintf(buf, sizeof(buf), "CPU %luMHz  描画 %.1fms  %s", (unsigned long)getCpuFrequencyMhz(), lastDrawUs / 1000.0,
           partialMode ? "部分" : "全面");
  d.drawString(buf, 20, PERF_Y + 10);
}

void drawQrArea() {
  auto& d = M5.Display;
  char buf[64];
  d.fillRect(0, QR_Y, d.width(), QR_H, TFT_NAVY);
  d.setFont(&fonts::lgfxJapanGothicP_28);
  d.setTextColor(TFT_WHITE, TFT_NAVY);
  snprintf(buf, sizeof(buf), "QR 読取 %lu 件 / 失敗 %lu", (unsigned long)qrCount, (unsigned long)qrFailCount);
  d.drawString(buf, 20, QR_Y + 8);
  d.setFont(&fonts::lgfxJapanGothicP_40);
  d.setTextColor(TFT_YELLOW, TFT_NAVY);
  d.drawString(lastQr.length() ? lastQr.c_str() : "(未読取)", 20, QR_Y + 52);
}

void drawTouchArea() {
  auto& d = M5.Display;
  char buf[64];
  d.fillRect(0, TOUCH_Y, d.width(), TOUCH_H, TFT_BLACK);
  d.setFont(&fonts::lgfxJapanGothicP_28);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  if (touchX >= 0) {
    snprintf(buf, sizeof(buf), "タッチ x=%d y=%d", touchX, touchY);
  } else {
    snprintf(buf, sizeof(buf), "下の灰色の所をなぞってください");
  }
  d.drawString(buf, 20, TOUCH_Y + 8);
}

void drawMarker() {
  auto& d = M5.Display;
  if (markX >= 0) d.fillCircle(markX, markY, 22, PAD_BG);  // 前の印を消す
  if (touchX >= 0 && touchY >= PAD_Y + 24) {
    d.fillCircle(touchX, touchY, 20, TFT_ORANGE);
    markX = touchX;
    markY = touchY;
  }
}

void drawAll() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);
  d.setFont(&fonts::lgfxJapanGothicP_40);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("M2 省電力・描画", 20, 16);
  drawPerfArea();
  drawQrArea();
  drawTouchArea();
  d.fillRect(0, PAD_Y, d.width(), d.height() - PAD_Y, PAD_BG);
  markX = -1;
  drawMarker();
  d.endWrite();
}

// 全面の書き直しを n 回して平均の時間を返す (マイクロ秒)
uint32_t benchFull(int n) {
  uint32_t total = 0;
  for (int i = 0; i < n; i++) {
    uint32_t t0 = micros();
    drawAll();
    total += micros() - t0;
  }
  return total / (uint32_t)n;
}

// QR の区画だけを n 回書き直して平均の時間を返す (マイクロ秒)
uint32_t benchPartial(int n) {
  uint32_t total = 0;
  for (int i = 0; i < n; i++) {
    uint32_t t0 = micros();
    M5.Display.startWrite();
    drawQrArea();
    M5.Display.endWrite();
    total += micros() - t0;
  }
  return total / (uint32_t)n;
}

void redraw(bool qrChanged, bool touchChanged) {
  uint32_t t0 = micros();
  if (!partialMode) {
    drawAll();
  } else {
    M5.Display.startWrite();
    if (qrChanged) drawQrArea();
    if (touchChanged) {
      drawTouchArea();
      drawMarker();
    }
    M5.Display.endWrite();
  }
  lastDrawUs = micros() - t0;
  M5.Display.startWrite();
  drawPerfArea();
  M5.Display.endWrite();
}

void pollQr() {
  if (!qrOk) return;
  uint8_t ready = qrcode.getDecodeReadyStatus();
  if (ready == 0) return;
  if (ready == 0xFF) {
    qrFailCount++;
    return;
  }
  uint16_t len = qrcode.getDecodeLength();
  bool bad = (len == 0 || len > QR_READ_MAX);
  uint16_t readLen = bad ? QR_READ_MAX : len;
  uint8_t data[QR_READ_MAX + 1] = {0};
  qrcode.getDecodeData(data, readLen);  // READY を落とすため、長さが異常でも必ず読み出す
  data[readLen] = 0;
  if (bad) {
    qrFailCount++;
    return;
  }
  lastQr = String((char*)data);
  lastQr.trim();
  qrCount++;
  logf("#QR n=%lu cpu=%lu data=%s", (unsigned long)qrCount, (unsigned long)getCpuFrequencyMhz(), lastQr.c_str());
  redraw(true, false);
}

void sendScreenshot() {
  size_t len = 0;
  uint8_t* png = (uint8_t*)M5.Display.createPng(&len, 0, 0, M5.Display.width(), M5.Display.height());
  if (png == nullptr || len == 0) {
    Serial.println("#ERR screenshot createPng failed");
    return;
  }
  Serial.printf("#BEGIN png %u\n", (unsigned)len);
  static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char out[80];
  for (size_t i = 0; i < len; i += 57) {
    size_t n = (len - i < 57) ? (len - i) : 57;
    size_t o = 0;
    for (size_t j = 0; j < n; j += 3) {
      uint32_t v = (uint32_t)png[i + j] << 16;
      if (j + 1 < n) v |= (uint32_t)png[i + j + 1] << 8;
      if (j + 2 < n) v |= (uint32_t)png[i + j + 2];
      out[o++] = B64[(v >> 18) & 63];
      out[o++] = B64[(v >> 12) & 63];
      out[o++] = (j + 1 < n) ? B64[(v >> 6) & 63] : '=';
      out[o++] = (j + 2 < n) ? B64[v & 63] : '=';
    }
    out[o] = 0;
    Serial.println(out);
  }
  Serial.println("#END png");
  free(png);
}

void printInfo() {
  logf("#INFO chip=%s rev=%d cpu=%lu psram_free=%lu heap_free=%lu qr=%s i2c_clock=%lu", ESP.getChipModel(),
       (int)ESP.getChipRevision(), (unsigned long)getCpuFrequencyMhz(), (unsigned long)ESP.getFreePsram(),
       (unsigned long)ESP.getFreeHeap(), qrOk ? "ok" : "ng", (unsigned long)Wire.getClock());
}

void printPower() {
  logf("#PWR bat_mv=%d bat_ma=%ld vbus_mv=%d charging=%d level=%d", (int)M5.Power.getBatteryVoltage(),
       (long)M5.Power.getBatteryCurrent(), (int)M5.Power.getVBUSVoltage(), (int)M5.Power.isCharging(),
       (int)M5.Power.getBatteryLevel());
}

void handleCommand(const String& cmd) {
  Serial.printf("#CMD %s\n", cmd.c_str());
  if (cmd == "info") {
    printInfo();
  } else if (cmd == "scan") {
    int found = 0;
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) found++;
    }
    logf("#I2C found=%d", found);
  } else if (cmd == "screenshot") {
    sendScreenshot();
  } else if (cmd == "bench" || cmd.startsWith("bench ")) {
    int n = (cmd.length() > 6) ? cmd.substring(6).toInt() : 5;
    if (n < 1) n = 1;
    uint32_t full = benchFull(n);
    uint32_t part = benchPartial(n);
    lastDrawUs = full;
    drawAll();
    logf("#BENCH cpu=%lu n=%d full_ms=%.1f partial_ms=%.2f", (unsigned long)getCpuFrequencyMhz(), n, full / 1000.0,
         part / 1000.0);
  } else if (cmd == "mode full" || cmd == "mode partial") {
    partialMode = (cmd == "mode partial");
    drawAll();
    logf("#OK mode=%s", partialMode ? "partial" : "full");
  } else if (cmd.startsWith("cpu ")) {
    uint32_t mhz = (uint32_t)cmd.substring(4).toInt();
    bool ok = setCpuFrequencyMhz(mhz);
    logf("#CPU set=%lu ok=%d now=%lu", (unsigned long)mhz, (int)ok, (unsigned long)getCpuFrequencyMhz());
    if (ok && mhz != 360) {
      cpuRevertAt = millis() + CPU_REVERT_MS;
      uint8_t fw = qrcode.getFirmwareVersion();  // この周波数で I2C が読めるか
      uint32_t full = benchFull(2);
      uint32_t part = benchPartial(3);
      lastDrawUs = full;
      drawAll();
      logf("#CPU at=%lu qr_fw=0x%02X full_ms=%.1f partial_ms=%.2f (%lu 秒後に 360MHz に戻す)",
           (unsigned long)getCpuFrequencyMhz(), fw, full / 1000.0, part / 1000.0,
           (unsigned long)(CPU_REVERT_MS / 1000));
    }
  } else if (cmd == "c6 on" || cmd == "c6 off") {
    bool on = (cmd == "c6 on");
    M5.getIOExpander(1).digitalWrite(0, on);  // 0x44 bit0 = WLAN_PWR_EN
    logf("#OK c6=%s", on ? "on" : "off");
  } else if (cmd == "usba on" || cmd == "usba off") {
    bool on = (cmd == "usba on");
    M5.Power.setExtOutput(on, m5::ext_port_mask_t::ext_USB);
    logf("#OK usba=%s", on ? "on" : "off");
  } else if (cmd == "pwr") {
    printPower();
  } else if (cmd.startsWith("bright ")) {
    int b = cmd.substring(7).toInt();
    M5.Display.setBrightness(b);
    logf("#OK bright=%d", b);
  } else if (cmd == "panel sleep") {
    M5.Display.sleep();
    logf("#OK panel=sleep");
  } else if (cmd == "panel wake") {
    M5.Display.wakeup();
    drawAll();
    logf("#OK panel=wake");
  } else if (cmd.startsWith("tone ")) {
    int hz = 0, ms = 0, vol = 0;
    sscanf(cmd.c_str() + 5, "%d %d %d", &hz, &ms, &vol);
    M5.Speaker.setVolume((uint8_t)vol);
    M5.Speaker.tone((float)hz, (uint32_t)ms);
    logf("#OK tone hz=%d ms=%d vol=%d", hz, ms, vol);
  } else if (cmd == "log") {
    Serial.printf("#LOG begin count=%d\n", evLogCount);
    for (int i = 0; i < evLogCount; i++) Serial.printf("#LOG %s\n", evLog[i].c_str());
    Serial.println("#LOG end");
  } else if (cmd == "clear") {
    evLogCount = 0;
    Serial.println("#OK clear");
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
    } else if (lineBuf.length() < 120) {
      lineBuf += c;
    }
  }
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_imu = false;
  cfg.internal_mic = false;
  cfg.external_display_value = 0;
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Display.setTextWrap(false, false);  // M5.begin() の後でないと効かない

  // M5UnitQRCode の begin() は速度を uint8_t に切り詰めて 160Hz にするので、先に 100kHz で開いておく (M1 で確認)
  Wire.begin(PORTA_SDA, PORTA_SCL, 100000);
  for (int i = 0; i < 5 && !qrOk; i++) {
    qrOk = qrcode.begin(&Wire, QR_ADDR, PORTA_SDA, PORTA_SCL, 100000U);
    if (!qrOk) delay(200);
  }
  Wire.setClock(100000);
  if (qrOk) setScanMode(true);

  drawAll();
  printInfo();
  logf("#READY m2_power");
}

void loop() {
  M5.update();
  pollSerial();

  uint32_t now = millis();
  if (now - lastQrPoll >= QR_POLL_MS) {
    lastQrPoll = now;
    pollQr();
  }

  if (M5.Touch.getCount() > 0) {
    auto t = M5.Touch.getDetail();
    if (t.isPressed() && (t.x != touchX || t.y != touchY)) {
      touchX = t.x;
      touchY = t.y;
      redraw(false, true);
    }
  }

  if (cpuRevertAt != 0 && (long)(millis() - cpuRevertAt) >= 0) {
    cpuRevertAt = 0;
    bool ok = setCpuFrequencyMhz(360);
    logf("#CPU reverted ok=%d now=%lu", (int)ok, (unsigned long)getCpuFrequencyMhz());
    drawAll();
  }
  delay(2);
}
