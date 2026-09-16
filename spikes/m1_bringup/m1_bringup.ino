/*
 * M1 ブリングアップ (Tab5 + Unit QRCode)
 *
 * 確かめること:
 *   - 起動・チップ情報 (rev → ChipVariant)・PSRAM
 *   - 画面: 日本語フォント・向き (setRotation)・明るさ・タッチ座標
 *   - Port A (G53 SDA / G54 SCL) の I2C: 0x21 (QR) あり / 0x29 (ToF) なし
 *   - QR ユニットの読取 (READY は 1 とは限らない。0 以外なら必ず読み出す)
 *   - screenshot: 画面を PNG にして base64 でシリアルへ送る
 *
 * シリアルコマンド (改行終端):
 *   info / scan / rot <0-3> / bright <0-255> / screenshot / qr auto / qr manual
 */
#include <M5Unified.h>
#include <Wire.h>
#include <M5UnitQRCode.h>

static const int      PORTA_SDA      = 53;
static const int      PORTA_SCL      = 54;
static const uint8_t  QR_ADDR        = 0x21;
static const uint8_t  TOF_ADDR       = 0x29;
static const uint16_t QR_READ_MAX    = 64;
static const uint32_t QR_POLL_MS     = 20;
static const uint32_t HEARTBEAT_MS   = 5000;

M5UnitQRCodeI2C qrcode;
bool     qrOk         = false;
String   lastQr       = "";
uint32_t qrCount      = 0;
String   lineBuf      = "";
uint32_t lastQrPoll   = 0;
uint32_t lastBeat     = 0;
int      touchX       = -1;
int      touchY       = -1;

// 読取・タッチの記録。QR をかざしてもらった後に `qrlog` でまとめて取り出す
static const int QR_LOG_MAX = 40;
String   qrLog[QR_LOG_MAX];
int      qrLogCount   = 0;
uint32_t qrFailCount  = 0;

bool probeI2C(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void printInfo() {
  Serial.println("#INFO begin");
  Serial.printf("#INFO board=%d\n", (int)M5.getBoard());
  Serial.printf("#INFO chip=%s rev=%d cores=%d\n", ESP.getChipModel(), (int)ESP.getChipRevision(), (int)ESP.getChipCores());
  Serial.printf("#INFO cpu_mhz=%lu\n", (unsigned long)getCpuFrequencyMhz());
  Serial.printf("#INFO flash=%lu psram=%lu free_psram=%lu free_heap=%lu\n",
                (unsigned long)ESP.getFlashChipSize(), (unsigned long)ESP.getPsramSize(),
                (unsigned long)ESP.getFreePsram(), (unsigned long)ESP.getFreeHeap());
  Serial.printf("#INFO display=%dx%d rotation=%d\n", M5.Display.width(), M5.Display.height(), (int)M5.Display.getRotation());
  Serial.printf("#INFO rtc=%d speaker=%d\n", (int)M5.Rtc.isEnabled(), (int)M5.Speaker.isEnabled());
  Serial.printf("#INFO sdk=%s\n", ESP.getSdkVersion());
  Serial.printf("#INFO qr=%s i2c_clock=%lu\n", qrOk ? "ok" : "ng", (unsigned long)Wire.getClock());
  Serial.println("#INFO end");
}

void scanI2C() {
  Serial.println("#I2C begin (Port A)");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    if (probeI2C(a)) {
      const char* name = (a == QR_ADDR) ? " (QR)" : ((a == TOF_ADDR) ? " (ToF)" : "");
      Serial.printf("#I2C found 0x%02X%s\n", a, name);
      found++;
    }
  }
  Serial.printf("#I2C end found=%d qr=%s tof=%s\n", found,
                probeI2C(QR_ADDR) ? "yes" : "no", probeI2C(TOF_ADDR) ? "yes" : "no");
}

// トリガモードを書いて読み戻す。setTriggerMode() は結果を返さないので、読み戻す以外に確かめようがない
bool setScanMode(bool autoScan) {
  uint8_t mode = autoScan ? AUTO_SCAN_MODE : MANUAL_SCAN_MODE;
  for (int i = 0; i < 3; i++) {
    qrcode.setTriggerMode((qrcode_scan_mode_t)mode);
    delay(50);
    if (qrcode.getTriggerMode() == mode) return true;
  }
  return false;
}

void drawScreen() {
  auto& d = M5.Display;
  int w = d.width();
  int h = d.height();
  char buf[96];

  d.startWrite();
  d.fillScreen(TFT_BLACK);
  d.drawRect(0, 0, w, h, TFT_GREEN);
  d.fillRect(0, 0, 60, 60, TFT_RED);             // 左上の目印
  d.fillRect(w - 60, h - 60, 60, 60, TFT_BLUE);  // 右下の目印

  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setFont(&fonts::lgfxJapanGothicP_40);
  d.drawString("物品持ち出し M1", 80, 10);

  d.setFont(&fonts::lgfxJapanGothicP_28);
  snprintf(buf, sizeof(buf), "向き %d / %d x %d", (int)d.getRotation(), w, h);
  d.drawString(buf, 20, 90);
  snprintf(buf, sizeof(buf), "QRユニット: %s", qrOk ? "接続" : "なし");
  d.drawString(buf, 20, 140);
  d.drawString("QRコードをかざしてください", 20, 190);

  d.fillRect(10, 250, w - 20, 70, TFT_NAVY);
  d.setFont(&fonts::lgfxJapanGothicP_40);
  d.setTextColor(TFT_YELLOW, TFT_NAVY);
  d.drawString(lastQr.length() ? lastQr.c_str() : "(未読取)", 20, 262);

  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setFont(&fonts::lgfxJapanGothicP_24);
  snprintf(buf, sizeof(buf), "読取 %lu 件", (unsigned long)qrCount);
  d.drawString(buf, 20, 340);
  if (touchX >= 0) {
    snprintf(buf, sizeof(buf), "タッチ x=%d y=%d", touchX, touchY);
    d.drawString(buf, 20, 380);
    d.fillCircle(touchX, touchY, 12, TFT_ORANGE);
  }

  // 文字の大きさの見本 (一覧の 1 行に何が収まるかを見る)
  int y = 460;
  d.setFont(&fonts::lgfxJapanGothicP_24);
  d.drawString("24px 貸出中 TW-025-A-123456 23時間前", 20, y);
  y += 44;
  d.setFont(&fonts::lgfxJapanGothicP_32);
  d.drawString("32px 貸出中 TW-025-A-1234 23時間前", 20, y);
  y += 54;
  d.setFont(&fonts::lgfxJapanGothicP_40);
  d.drawString("40px ABCDEFGHIJKLMNO", 20, y);
  y += 60;
  d.drawString("40px 1234567 23時間前", 20, y);
  d.endWrite();
}

void pushLog(const String& entry) {
  if (qrLogCount < QR_LOG_MAX) {
    qrLog[qrLogCount++] = entry;
    return;
  }
  for (int i = 1; i < QR_LOG_MAX; i++) qrLog[i - 1] = qrLog[i];
  qrLog[QR_LOG_MAX - 1] = entry;
}

void pollQr() {
  if (!qrOk) return;
  uint8_t ready = qrcode.getDecodeReadyStatus();
  if (ready == 0) return;
  if (ready == 0xFF) {
    qrFailCount++;
    Serial.println("[QR] I2C の読み取りに失敗しました (ready=0xFF)");
    return;
  }
  uint16_t len = qrcode.getDecodeLength();
  bool bad = (len == 0 || len > QR_READ_MAX);
  uint16_t readLen = bad ? QR_READ_MAX : len;
  uint8_t data[QR_READ_MAX + 1] = {0};
  qrcode.getDecodeData(data, readLen);  // READY を落とすため、長さが異常でも必ず読み出す
  data[readLen] = 0;
  if (bad) {
    Serial.printf("[QR] 長さが異常です len=%u ready=%u\n", (unsigned)len, (unsigned)ready);
    pushLog(String("BADLEN len=") + len + " ready=" + ready);
    return;
  }

  String hex = "";
  char h[3];
  for (uint16_t i = 0; i < len; i++) {
    snprintf(h, sizeof(h), "%02X", data[i]);
    hex += h;
  }
  String s = String((char*)data);
  s.trim();
  qrCount++;
  lastQr = s;
  Serial.printf("#QR ready=%u len=%u hex=%s data=%s\n", (unsigned)ready, (unsigned)len, hex.c_str(), s.c_str());
  pushLog(String("QR n=") + qrCount + " ready=" + ready + " len=" + len + " hex=" + hex + " data=" + s);
  drawScreen();
}

void sendScreenshot() {
  size_t len = 0;
  uint32_t t0 = millis();
  uint8_t* png = (uint8_t*)M5.Display.createPng(&len, 0, 0, M5.Display.width(), M5.Display.height());
  if (png == nullptr || len == 0) {
    Serial.println("#ERR screenshot createPng failed");
    return;
  }
  Serial.printf("#BEGIN png %u encode_ms=%lu\n", (unsigned)len, (unsigned long)(millis() - t0));
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

void handleCommand(const String& cmd) {
  Serial.printf("#CMD %s\n", cmd.c_str());
  if (cmd == "info") {
    printInfo();
  } else if (cmd == "scan") {
    scanI2C();
  } else if (cmd.startsWith("rot ")) {
    M5.Display.setRotation(cmd.substring(4).toInt());
    drawScreen();
    Serial.printf("#OK rot=%d %dx%d\n", (int)M5.Display.getRotation(), M5.Display.width(), M5.Display.height());
  } else if (cmd.startsWith("bright ")) {
    int b = cmd.substring(7).toInt();
    M5.Display.setBrightness(b);
    Serial.printf("#OK bright=%d\n", b);
  } else if (cmd == "screenshot") {
    sendScreenshot();
  } else if (cmd == "qrlog") {
    Serial.printf("#QRLOG begin count=%d read=%lu fail=%lu\n", qrLogCount, (unsigned long)qrCount,
                  (unsigned long)qrFailCount);
    for (int i = 0; i < qrLogCount; i++) Serial.printf("#QRLOG %d %s\n", i + 1, qrLog[i].c_str());
    Serial.println("#QRLOG end");
  } else if (cmd == "qrclear") {
    qrLogCount = 0;
    qrCount = 0;
    qrFailCount = 0;
    lastQr = "";
    drawScreen();
    Serial.println("#OK qrclear");
  } else if (cmd == "qr auto" || cmd == "qr manual") {
    bool ok = setScanMode(cmd == "qr auto");
    Serial.printf("#%s qr mode=%s\n", ok ? "OK" : "ERR", cmd.substring(3).c_str());
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
    } else if (lineBuf.length() < 200) {
      lineBuf += c;
    }
  }
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_imu = false;  // 使わない
  cfg.internal_mic = false;  // 使わない
  cfg.external_display_value = 0;  // 外付けディスプレイは使わない (起動時に Port A を探しに行かせない)
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Display.setTextWrap(false, false);  // M5.begin() の後でないと効かない

  // **Wire は自分で 100kHz で開いてから QR ライブラリに渡す。**
  // M5UnitQRCode 1.0.0 の M5UnitQRCodeI2C は速度を `uint8_t _speed` に入れて Wire.begin() に渡すので、
  // 100000 が 160 (Hz) に切り詰められる。160Hz だと 1 回の読み取りがタイムアウト (50ms) を超えて毎回 0xFF になり、
  // 遅れて届いた受信割り込みで i2c_master の ISR が Store access fault で落ちる (2026-09-12 実機で確認)。
  // 先に開いておけば、ライブラリの Wire.begin() は開き済みとして何もしない想定。念のため setClock で戻して確かめる
  Wire.begin(PORTA_SDA, PORTA_SCL, 100000);
  for (int i = 0; i < 5 && !qrOk; i++) {
    qrOk = qrcode.begin(&Wire, QR_ADDR, PORTA_SDA, PORTA_SCL, 100000U);
    if (!qrOk) delay(200);
  }
  Wire.setClock(100000);
  if (qrOk) {
    bool autoOk = setScanMode(true);
    Serial.printf("[QR] 初期化しました。AUTO スキャン=%s\n", autoOk ? "OK" : "失敗");
  } else {
    Serial.println("[QR] 初期化に失敗しました。配線と側面スイッチ (I2C) を確認してください");
  }

  drawScreen();
  printInfo();
  scanI2C();
  Serial.println("#READY m1_bringup");
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
    if (t.wasPressed()) {
      touchX = t.x;
      touchY = t.y;
      Serial.printf("#TOUCH x=%d y=%d\n", touchX, touchY);
      pushLog(String("TOUCH x=") + touchX + " y=" + touchY);
      drawScreen();
    }
  }

  if (now - lastBeat >= HEARTBEAT_MS) {
    lastBeat = now;
    Serial.printf("#HB uptime_ms=%lu free_heap=%lu\n", (unsigned long)now, (unsigned long)ESP.getFreeHeap());
  }
  delay(5);
}
