/*
 * M2 保存・時刻・ライトスリープの実機確認 (Tab5 + Unit QRCode)
 *
 * 確かめること:
 *   - スケッチ直下の partitions.csv が使われ、NVS パーティション tooldb (1MB) が見えるか
 *   - Preferences.begin(ns, false, "tooldb") で読み書きできるか / 記号入り 15 文字のキー (16 文字は拒否されるか) /
 *     nvs_entry_find で列挙できるか / nvs_flash_erase_partition で消して開き直せるか / 再起動で残るか
 *   - RTC (RX8130CE) の設定と読み出し、再起動で残るか
 *   - 消灯中の手動ライトスリープ (esp_light_sleep_start) で、画面 (MIPI-DSI)・QR の I2C・USB が壊れないか
 *   - QR の読み取りログ (本物のバーコードの確認を続けられるように)
 *
 * シリアルコマンド (改行終端):
 *   info / nvs info / nvs put <ns> <key> <値> / nvs get <ns> <key> / nvs keys <ns> / nvs fill <件数> /
 *   nvs clearns <ns> / nvs erase / time / time set YYYY-MM-DDTHH:MM:SS / restart / lsleep <回数> /
 *   qrlog / log / clear
 */
#include <M5Unified.h>
#include <Wire.h>
#include <M5UnitQRCode.h>
#include <Preferences.h>
#include <cstdarg>
#include "esp_partition.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

static const int      PORTA_SDA   = 53;
static const int      PORTA_SCL   = 54;
static const uint8_t  QR_ADDR     = 0x21;
static const uint16_t QR_READ_MAX = 64;
static const uint32_t QR_POLL_MS  = 20;
static const char*    DB_PART     = "tooldb";
static const int      LOG_MAX     = 80;
static const int      STATUS_Y    = 90;
static const int      STATUS_H    = 120;
static const int      QRAREA_Y    = 230;
static const int      QRAREA_H    = 110;

M5UnitQRCodeI2C qrcode;
bool     qrOk        = false;
String   lastQr      = "";
uint32_t qrCount     = 0;
uint32_t qrFailCount = 0;
uint32_t lastQrPoll  = 0;
String   lineBuf     = "";
String   statusText  = "起動しました";
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

// シリアルに出し、RAM の記録にも残す (ライトスリープ中は USB が切れるので、後で `log` で取り出す)
void logf(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
  pushLog(String((unsigned long)millis()) + " " + buf);
}

void drawStatus() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillRect(0, STATUS_Y, d.width(), STATUS_H, TFT_BLACK);
  d.setFont(&fonts::lgfxJapanGothicP_28);
  d.setTextColor(TFT_CYAN, TFT_BLACK);
  d.drawString(statusText.c_str(), 20, STATUS_Y + 10);
  d.endWrite();
}

void drawQrArea() {
  auto& d = M5.Display;
  char buf[64];
  d.startWrite();
  d.fillRect(0, QRAREA_Y, d.width(), QRAREA_H, TFT_NAVY);
  d.setFont(&fonts::lgfxJapanGothicP_28);
  d.setTextColor(TFT_WHITE, TFT_NAVY);
  snprintf(buf, sizeof(buf), "QR 読取 %lu 件 / 失敗 %lu", (unsigned long)qrCount, (unsigned long)qrFailCount);
  d.drawString(buf, 20, QRAREA_Y + 8);
  d.setFont(&fonts::lgfxJapanGothicP_40);
  d.setTextColor(TFT_YELLOW, TFT_NAVY);
  d.drawString(lastQr.length() ? lastQr.c_str() : "(未読取)", 20, QRAREA_Y + 52);
  d.endWrite();
}

void drawAll() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);
  d.setFont(&fonts::lgfxJapanGothicP_40);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("M2 保存・時刻・スリープ", 20, 16);
  d.endWrite();
  drawStatus();
  drawQrArea();
}

void setStatus(const String& text) {
  statusText = text;
  drawStatus();
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
  logf("#QR n=%lu len=%u data=%s", (unsigned long)qrCount, (unsigned)len, lastQr.c_str());
  drawQrArea();
}

// --- NVS ---

void cmdNvsInfo() {
  const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, DB_PART);
  if (p == nullptr) {
    logf("#NVS part=none (partitions.csv が使われていない)");
    setStatus("tooldb が見つかりません");
    return;
  }
  logf("#NVS part=%s addr=0x%06lx size=0x%06lx", DB_PART, (unsigned long)p->address, (unsigned long)p->size);
  esp_err_t e = nvs_flash_init_partition(DB_PART);
  logf("#NVS init=%s", esp_err_to_name(e));
  nvs_stats_t st;
  memset(&st, 0, sizeof(st));
  esp_err_t se = nvs_get_stats(DB_PART, &st);
  logf("#NVS stats=%s used=%u free=%u total=%u namespaces=%u", esp_err_to_name(se), (unsigned)st.used_entries,
       (unsigned)st.free_entries, (unsigned)st.total_entries, (unsigned)st.namespace_count);
  setStatus(String("tooldb init=") + esp_err_to_name(e) + " used=" + (unsigned)st.used_entries);
}

void cmdNvsPut(const char* ns, const char* key, const char* value) {
  Preferences p;
  if (!p.begin(ns, false, DB_PART)) {
    logf("#NVS put begin 失敗 ns=%s", ns);
    return;
  }
  size_t n = p.putString(key, value);
  p.end();
  logf("#NVS put ns=%s key=%s keylen=%u -> %u", ns, key, (unsigned)strlen(key), (unsigned)n);
}

void cmdNvsGet(const char* ns, const char* key) {
  Preferences p;
  if (!p.begin(ns, true, DB_PART)) {
    logf("#NVS get begin 失敗 ns=%s", ns);
    return;
  }
  bool exists = p.isKey(key);
  String v = p.getString(key, "(なし)");
  p.end();
  logf("#NVS get ns=%s key=%s exists=%d value=%s", ns, key, (int)exists, v.c_str());
}

void cmdNvsKeys(const char* ns) {
  nvs_iterator_t it = nullptr;
  esp_err_t e = nvs_entry_find(DB_PART, ns, NVS_TYPE_ANY, &it);
  int count = 0;
  while (e == ESP_OK) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    if (count < 5) logf("#NVS key ns=%s key=%s type=0x%02x", info.namespace_name, info.key, (unsigned)info.type);
    count++;
    e = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
  logf("#NVS keys ns=%s count=%d end=%s", ns, count, esp_err_to_name(e));
}

void cmdNvsFill(int n) {
  Preferences p;
  if (!p.begin("loans", false, DB_PART)) {
    logf("#NVS fill begin 失敗");
    return;
  }
  uint32_t t0 = millis();
  int ok = 0;
  uint8_t blob[32];
  for (int i = 0; i < n; i++) {
    char key[16];
    snprintf(key, sizeof(key), "A-/_#.%09d", i);  // 記号入りの 15 文字
    memset(blob, i & 0xFF, sizeof(blob));
    if (p.putBytes(key, blob, sizeof(blob)) == sizeof(blob)) ok++;
  }
  p.end();
  logf("#NVS fill n=%d ok=%d ms=%lu", n, ok, (unsigned long)(millis() - t0));
  cmdNvsInfo();
}

void cmdNvsClearNs(const char* ns) {
  Preferences p;
  if (!p.begin(ns, false, DB_PART)) {
    logf("#NVS clearns begin 失敗 ns=%s", ns);
    return;
  }
  bool ok = p.clear();
  p.end();
  logf("#NVS clearns ns=%s ok=%d", ns, (int)ok);
}

void cmdNvsErase() {
  esp_err_t d = nvs_flash_deinit_partition(DB_PART);
  esp_err_t e = nvs_flash_erase_partition(DB_PART);
  esp_err_t i = nvs_flash_init_partition(DB_PART);
  logf("#NVS erase deinit=%s erase=%s init=%s", esp_err_to_name(d), esp_err_to_name(e), esp_err_to_name(i));
  setStatus(String("tooldb を消去 init=") + esp_err_to_name(i));
}

// --- RTC ---

void cmdTime() {
  auto dt = M5.Rtc.getDateTime();
  logf("#RTC %04d-%02d-%02dT%02d:%02d:%02d enabled=%d volt_low=%d", (int)dt.date.year, (int)dt.date.month,
       (int)dt.date.date, (int)dt.time.hours, (int)dt.time.minutes, (int)dt.time.seconds, (int)M5.Rtc.isEnabled(),
       (int)M5.Rtc.getVoltLow());
}

void cmdTimeSet(const char* s) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) {
    logf("#ERR time set の書式は YYYY-MM-DDTHH:MM:SS");
    return;
  }
  M5.Rtc.setDateTime({{(int16_t)y, (int8_t)mo, (int8_t)d}, {(int8_t)h, (int8_t)mi, (int8_t)se}});
  cmdTime();
}

// --- ライトスリープ ---

void cmdLightSleep(int n) {
  uint8_t brightness = M5.Display.getBrightness();
  logf("#LSLEEP begin n=%d", n);
  M5.Display.setBrightness(0);
  M5.Display.sleep();
  Serial.flush();
  delay(100);
  uint32_t ok = 0;
  uint32_t fail = 0;
  int64_t totalUs = 0;
  for (int i = 0; i < n; i++) {
    esp_sleep_enable_timer_wakeup(200000);  // 200ms
    int64_t t0 = esp_timer_get_time();
    esp_err_t r = esp_light_sleep_start();
    totalUs += esp_timer_get_time() - t0;
    (r == ESP_OK) ? ok++ : fail++;
    pollQr();  // 起きている間に QR を 1 回見る
  }
  M5.Display.wakeup();
  M5.Display.setBrightness(brightness);
  drawAll();
  logf("#LSLEEP end n=%d ok=%lu fail=%lu avg_ms=%.1f qr=%lu qr_fail=%lu", n, (unsigned long)ok, (unsigned long)fail,
       n ? (double)totalUs / 1000.0 / n : 0.0, (unsigned long)qrCount, (unsigned long)qrFailCount);
}

// --- コマンド ---

void handleCommand(const String& cmd) {
  Serial.printf("#CMD %s\n", cmd.c_str());
  char a[32] = {0};
  char b[40] = {0};
  char c[64] = {0};
  if (cmd == "info") {
    logf("#INFO chip=%s rev=%d cpu=%lu qr=%s i2c_clock=%lu heap=%lu", ESP.getChipModel(), (int)ESP.getChipRevision(),
         (unsigned long)getCpuFrequencyMhz(), qrOk ? "ok" : "ng", (unsigned long)Wire.getClock(),
         (unsigned long)ESP.getFreeHeap());
  } else if (cmd == "nvs info") {
    cmdNvsInfo();
  } else if (cmd.startsWith("nvs put ") && sscanf(cmd.c_str() + 8, "%31s %39s %63s", a, b, c) == 3) {
    cmdNvsPut(a, b, c);
  } else if (cmd.startsWith("nvs get ") && sscanf(cmd.c_str() + 8, "%31s %39s", a, b) == 2) {
    cmdNvsGet(a, b);
  } else if (cmd.startsWith("nvs keys ") && sscanf(cmd.c_str() + 9, "%31s", a) == 1) {
    cmdNvsKeys(a);
  } else if (cmd.startsWith("nvs fill ")) {
    cmdNvsFill(cmd.substring(9).toInt());
  } else if (cmd.startsWith("nvs clearns ") && sscanf(cmd.c_str() + 12, "%31s", a) == 1) {
    cmdNvsClearNs(a);
  } else if (cmd == "nvs erase") {
    cmdNvsErase();
  } else if (cmd == "time") {
    cmdTime();
  } else if (cmd.startsWith("time set ")) {
    cmdTimeSet(cmd.c_str() + 9);
  } else if (cmd == "restart") {
    logf("#OK restart");
    Serial.flush();
    delay(200);
    ESP.restart();
  } else if (cmd.startsWith("lsleep ")) {
    cmdLightSleep(cmd.substring(7).toInt());
  } else if (cmd == "qrlog" || cmd == "log") {
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
    char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      String cmd = lineBuf;
      lineBuf = "";
      cmd.trim();
      if (cmd.length()) handleCommand(cmd);
    } else if (lineBuf.length() < 160) {
      lineBuf += ch;
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
  // 起動直後は NVS に書かない。見るだけにする
  logf("#READY m2_nvs");
}

void loop() {
  M5.update();
  pollSerial();
  uint32_t now = millis();
  if (now - lastQrPoll >= QR_POLL_MS) {
    lastQrPoll = now;
    pollQr();
  }
  delay(2);
}
