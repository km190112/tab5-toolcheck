/*
 * M2 の残り (音量・明るさ・電流・パネルスリープ中のタッチ) と M3 の microSD の実機確認 (Tab5 + Unit QRCode)
 *
 * 確かめること:
 *   - 操作音・警告 1 (断続)・警告 2 (連続) を 30 / 60 / 100% で鳴らし、聞き分けと大きさを耳で確かめる (sounddemo)
 *   - 明るさ 100〜2% の見え方を目で確かめる (brightdemo)
 *   - 状態ごとの電流を USB 電力チェッカーで読む (seq)。画面の「手順 N」と、手順の頭の N 回の「ピッ」で知らせる
 *   - パネルスリープ中にタッチが取れるか (#TOUCH の screen=sleep)
 *   - microSD を SPI で開き、長いファイル名の書込・読み戻し・削除ができるか
 *   - (M3 の残り) 100 枚を超えた写真の削除 (sd fill → sd prune)・抜き差しの検出 (sd watch)・書込中の電源断 (sd stress → USB を抜く → sd check)
 *   - (M3 の残り) RTC の保持時間 (rtc set → USB を抜いて待つ → rtc)
 *
 * シリアルコマンド (改行終端):
 *   info / io / log / clear
 *   sounddemo / op <音量%> / w1 <音量%> [秒] / w2 <音量%> [秒] / stop
 *   brightdemo / bright <%>
 *   seq [1 手順の秒数] [始める手順]   … 途中から始めるときは、それより前の手順の状態を鳴らさずに作ってから始める
 *   c6 on|off / usba on|off / spk on|off / qr auto|manual / panel sleep|wake
 *   sd mount / sd info / sd test [KB] / sd ls / sd end
 *   sd fill [件数] [0 バイトの件数] / sd prune [残す件数] / sd probe / sd watch on|off / sd remount
 *   sd stress / sd stress stop / sd check / sd clean   … 試験のファイルは /ToolCheck/prunetest と /ToolCheck/stress にだけ作る
 *   rtc / rtc set YYYY-MM-DDThh:mm:ss
 */
#include <M5Unified.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <M5UnitQRCode.h>
#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <vector>

static const int      PORTA_SDA   = 53;
static const int      PORTA_SCL   = 54;
static const uint8_t  QR_ADDR     = 0x21;
static const uint16_t QR_READ_MAX = 64;
static const uint32_t QR_POLL_MS  = 20;
static const int      LOG_MAX     = 80;

// microSD (SPI)。M5Stack の Tab5 の例と同じピン。カード検出ピンは無い
static const int      SD_CS   = 42;
static const int      SD_SCK  = 43;
static const int      SD_MOSI = 44;
static const int      SD_MISO = 39;
static const uint32_t SD_FREQ = 20000000;

static const uint8_t IOE0_ADDR = 0x43;
static const uint8_t IOE1_ADDR = 0x44;

// 鳴らし方: {周波数 Hz (0 = 無音), 長さ ms} の並びを 1 周として繰り返す
static const uint16_t PAT_OP[]    = {2400, 70, 0, 930};    // 操作音 (聞き比べでは 1 秒に 1 回)
static const uint16_t PAT_W1[]    = {2000, 300, 0, 300};   // 警告 1: 断続
static const uint16_t PAT_W2[]    = {2800, 250, 2000, 250}; // 警告 2: 連続 (高低を交互に)
static const uint16_t PAT_COUNT[] = {1500, 80, 0, 170};    // 手順番号の「ピッ」
static const int PAT_ID_OP = 1;
static const int PAT_ID_W1 = 2;
static const int PAT_ID_W2 = 3;
static const int PAT_ID_COUNT = 4;

static const int SEQ_LAST = 10;
static const char* SEQ_LABELS[SEQ_LAST + 1] = {
  "",
  "基準: 明るさ 60%・C6 ON・USB-A ON・QR 自動",
  "QR を手動にする (読み取りを止める)",
  "QR を自動に戻し、C6 を OFF",
  "さらに USB-A の 5V を OFF",
  "さらに明るさ 15%",
  "さらに明るさ 0 (バックライト消灯)",
  "さらにパネルスリープ",
  "さらに QR を手動",
  "さらにスピーカー停止 (アンプ OFF)",
  "全部戻す",
};

static const int BRIGHT_LEVELS[] = {100, 60, 30, 15, 10, 5, 2};
static const int BRIGHT_N = sizeof(BRIGHT_LEVELS) / sizeof(BRIGHT_LEVELS[0]);

M5UnitQRCodeI2C qrcode;
bool     qrOk       = false;
bool     qrAuto     = true;
String   lastQr     = "";
uint32_t qrCount    = 0;
uint32_t lastQrPoll = 0;
String   lineBuf    = "";
String   evLog[LOG_MAX];
int      evLogCount = 0;

bool   screenOn    = true;
int    brightPct   = 60;
String statusTitle = "待機中";
String statusLine  = "シリアルのコマンドを待っています";
int    lastCountdown = -1;

// 鳴らしている音
const uint16_t* patData      = nullptr;
int             patSteps     = 0;
int             patIndex     = 0;
int             patCyclesLeft = 0;
uint32_t        patStepAt    = 0;
bool            patActive    = false;

// 音の聞き比べ (-1 = していない)
int      soundDemoIndex  = -1;
uint32_t soundDemoNextAt = 0;

// 明るさの見比べ (-1 = していない)
int      brightDemoIndex  = -1;
uint32_t brightDemoNextAt = 0;

// 電流の手順 (0 = していない)
int      seqStep      = 0;
bool     seqBeeping   = false;
uint32_t seqHoldUntil = 0;
uint32_t seqStepMs    = 20000;

bool sdMounted = false;

void pushLog(const String& entry) {
  if (evLogCount < LOG_MAX) {
    evLog[evLogCount++] = entry;
    return;
  }
  for (int i = 1; i < LOG_MAX; i++) evLog[i - 1] = evLog[i];
  evLog[LOG_MAX - 1] = entry;
}

void logf(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
  pushLog(String((unsigned long)millis()) + " " + buf);
}

bool timeReached(uint32_t at) { return (int32_t)(millis() - at) >= 0; }

// ---------------------------------------------------------------- 画面

void drawScreen() {
  if (!screenOn) return;
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);
  d.setTextSize(1);
  d.setFont(&fonts::lgfxJapanGothicP_32);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("M2 音・明るさ・電流 / M3 SD", 20, 16);

  d.setFont(&fonts::lgfxJapanGothicP_40);
  d.setTextSize(2);
  d.setTextColor(TFT_YELLOW, TFT_BLACK);
  d.drawString(statusTitle, 20, 90);
  d.setTextSize(1);
  d.setFont(&fonts::lgfxJapanGothicP_28);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString(statusLine, 20, 200);

  // 明るさの見本: 黒から白までの 8 段
  for (int i = 0; i < 8; i++) {
    uint8_t v = (uint8_t)(i * 255 / 7);
    d.fillRect(20 + i * 86, 320, 80, 200, d.color565(v, v, v));
  }
  d.setFont(&fonts::lgfxJapanGothicP_32);
  d.drawString("明るさの見本 (左の暗い段が見分けられるか)", 20, 530);

  char buf[96];
  d.setFont(&fonts::lgfxJapanGothicP_28);
  snprintf(buf, sizeof(buf), "明るさ %d%%  QR %s  SD %s", brightPct, qrAuto ? "自動" : "手動", sdMounted ? "あり" : "-");
  d.drawString(buf, 20, 600);
  snprintf(buf, sizeof(buf), "QR %lu 件: %s", (unsigned long)qrCount, lastQr.length() ? lastQr.c_str() : "(未読取)");
  d.drawString(buf, 20, 650);
  d.endWrite();
  lastCountdown = -1;
}

void setStatus(const String& title, const String& line) {
  statusTitle = title;
  statusLine = line;
  drawScreen();
}

void drawCountdown(int sec) {
  if (!screenOn || sec == lastCountdown) return;
  lastCountdown = sec;
  auto& d = M5.Display;
  char buf[48];
  d.startWrite();
  d.fillRect(0, 250, d.width(), 50, TFT_BLACK);
  d.setTextSize(1);
  d.setFont(&fonts::lgfxJapanGothicP_32);
  d.setTextColor(TFT_CYAN, TFT_BLACK);
  snprintf(buf, sizeof(buf), "残り %d 秒", sec);
  d.drawString(buf, 20, 255);
  d.endWrite();
}

// ---------------------------------------------------------------- 電源・周辺

void setBrightPct(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  brightPct = pct;
  M5.Display.setBrightness((uint8_t)((pct * 255 + 50) / 100));
}

void setC6(bool on) { M5.getIOExpander(1).digitalWrite(0, on); }  // 0x44 bit0 = WLAN_PWR_EN

void setUsbA(bool on) { M5.Power.setExtOutput(on, m5::ext_port_mask_t::ext_USB); }

bool setQrAuto(bool autoScan) {
  if (!qrOk) return false;
  uint8_t mode = autoScan ? AUTO_SCAN_MODE : MANUAL_SCAN_MODE;
  for (int i = 0; i < 3; i++) {
    qrcode.setTriggerMode((qrcode_scan_mode_t)mode);
    delay(50);
    if (qrcode.getTriggerMode() == mode) {
      qrAuto = autoScan;
      return true;
    }
  }
  return false;
}

void printIo() {
  uint8_t dir0 = M5.In_I2C.readRegister8(IOE0_ADDR, 0x03, 400000);
  uint8_t out0 = M5.In_I2C.readRegister8(IOE0_ADDR, 0x05, 400000);
  uint8_t dir1 = M5.In_I2C.readRegister8(IOE1_ADDR, 0x03, 400000);
  uint8_t out1 = M5.In_I2C.readRegister8(IOE1_ADDR, 0x05, 400000);
  logf("#IO 0x43 dir=0x%02X out=0x%02X / 0x44 dir=0x%02X out=0x%02X", dir0, out0, dir1, out1);
}

// ---------------------------------------------------------------- 音

const uint16_t* patternOf(int id, int* steps) {
  *steps = 2;
  switch (id) {
    case PAT_ID_OP: return PAT_OP;
    case PAT_ID_W1: return PAT_W1;
    case PAT_ID_W2: return PAT_W2;
    case PAT_ID_COUNT: return PAT_COUNT;
  }
  *steps = 0;
  return nullptr;
}

int cyclesFor(int id, uint32_t durationMs) {
  int steps = 0;
  const uint16_t* p = patternOf(id, &steps);
  uint32_t cycleMs = 0;
  for (int i = 0; i < steps; i++) cycleMs += p[i * 2 + 1];
  if (cycleMs == 0) return 1;
  int cycles = (int)((durationMs + cycleMs - 1) / cycleMs);
  return cycles < 1 ? 1 : cycles;
}

void toneStep() {
  uint16_t hz = patData[patIndex * 2];
  uint16_t ms = patData[patIndex * 2 + 1];
  if (hz > 0) {
    M5.Speaker.tone((float)hz, ms);
  } else {
    M5.Speaker.stop();
  }
}

void playPattern(int id, int volPct, int cycles) {
  int steps = 0;
  const uint16_t* p = patternOf(id, &steps);
  if (p == nullptr || cycles <= 0) return;
  if (!M5.Speaker.isEnabled()) M5.Speaker.begin();
  M5.Speaker.setVolume((uint8_t)((volPct * 255 + 50) / 100));
  patData = p;
  patSteps = steps;
  patIndex = 0;
  patCyclesLeft = cycles;
  patStepAt = millis();
  patActive = true;
  toneStep();
}

void stopPattern() {
  patActive = false;
  M5.Speaker.stop();
}

void updatePattern() {
  if (!patActive) return;
  uint32_t now = millis();
  if (now - patStepAt < patData[patIndex * 2 + 1]) return;
  patStepAt = now;
  if (++patIndex >= patSteps) {
    patIndex = 0;
    if (--patCyclesLeft <= 0) {
      stopPattern();
      return;
    }
  }
  toneStep();
}

void soundDemoUpdate() {
  if (soundDemoIndex < 0) return;
  if (patActive) {
    soundDemoNextAt = millis() + 1500;  // 鳴り終わってから 1.5 秒あける
    return;
  }
  if (!timeReached(soundDemoNextAt)) return;
  if (soundDemoIndex >= 9) {
    soundDemoIndex = -1;
    setStatus("音 終わり", "聞き分けと大きさを教えてください");
    logf("#SOUND done");
    return;
  }
  static const int VOLS[3] = {30, 60, 100};
  static const int IDS[3] = {PAT_ID_OP, PAT_ID_W1, PAT_ID_W2};
  static const char* NAMES[3] = {"操作音", "警告 1 (断続)", "警告 2 (連続)"};
  int kind = soundDemoIndex / 3;
  int vol = VOLS[soundDemoIndex % 3];
  char title[32];
  char line[64];
  snprintf(title, sizeof(title), "音 %d / 9", soundDemoIndex + 1);
  snprintf(line, sizeof(line), "%s  音量 %d%%", NAMES[kind], vol);
  setStatus(title, line);
  logf("#SOUND step=%d kind=%s vol=%d", soundDemoIndex + 1, NAMES[kind], vol);
  playPattern(IDS[kind], vol, cyclesFor(IDS[kind], kind == 0 ? 3000 : 4000));
  soundDemoIndex++;
}

// ---------------------------------------------------------------- 明るさ

void brightDemoUpdate() {
  if (brightDemoIndex < 0 || !timeReached(brightDemoNextAt)) return;
  if (brightDemoIndex >= BRIGHT_N) {
    brightDemoIndex = -1;
    setBrightPct(60);
    setStatus("明るさ 終わり", "60% に戻しました");
    logf("#BRIGHT done");
    return;
  }
  int pct = BRIGHT_LEVELS[brightDemoIndex];
  setBrightPct(pct);
  char title[32];
  char line[64];
  snprintf(title, sizeof(title), "明るさ %d%%", pct);
  snprintf(line, sizeof(line), "%d / %d (4 秒ごとに暗くします)", brightDemoIndex + 1, BRIGHT_N);
  setStatus(title, line);
  logf("#BRIGHT step=%d pct=%d", brightDemoIndex + 1, pct);
  brightDemoIndex++;
  brightDemoNextAt = millis() + 4000;
}

// ---------------------------------------------------------------- 電流の手順

void seqApply(int step) {
  switch (step) {
    case 1:
      setBrightPct(60);
      setC6(true);
      setUsbA(true);
      setQrAuto(true);
      break;
    case 2:
      setQrAuto(false);
      break;
    case 3:
      setQrAuto(true);
      setC6(false);
      break;
    case 4:
      setUsbA(false);
      break;
    case 5:
      setBrightPct(15);
      break;
    case 6:
      setBrightPct(0);
      break;
    case 7:
      M5.Display.sleep();
      screenOn = false;
      break;
    case 8:
      setQrAuto(false);
      break;
    case 9:
      M5.Speaker.end();
      break;
    case 10:
      M5.Speaker.begin();
      M5.Display.wakeup();
      screenOn = true;
      setBrightPct(60);
      setC6(true);
      setUsbA(true);
      setQrAuto(true);
      break;
  }
}

void seqBegin(int step) {
  char title[32];
  snprintf(title, sizeof(title), "手順 %d / %d", step, SEQ_LAST);
  if (screenOn) setStatus(title, SEQ_LABELS[step]);
  logf("#SEQ step=%d label=%s", step, SEQ_LABELS[step]);
  if (step < SEQ_LAST) {
    playPattern(PAT_ID_COUNT, 20, step);  // 手順番号の回数だけ鳴らしてから切り替える
    seqBeeping = true;
  } else {
    seqApply(step);
    setStatus(title, SEQ_LABELS[step]);
    seqHoldUntil = millis() + 3000;
  }
}

void seqUpdate() {
  if (seqStep == 0) return;
  if (seqBeeping) {
    if (patActive) return;
    seqBeeping = false;
    seqApply(seqStep);
    seqHoldUntil = millis() + seqStepMs;
    logf("#SEQ step=%d applied", seqStep);
    return;
  }
  if (!timeReached(seqHoldUntil)) {
    drawCountdown((int)((seqHoldUntil - millis() + 999) / 1000));
    return;
  }
  if (seqStep >= SEQ_LAST) {
    seqStep = 0;
    setStatus("電流 終わり", "手順ごとの値を教えてください");
    logf("#SEQ done");
    return;
  }
  seqStep++;
  seqBegin(seqStep);
}

// ---------------------------------------------------------------- microSD

void sdMount() {
  if (sdMounted) {
    logf("#SD mount=1 already=1");
    return;
  }
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  uint32_t t0 = millis();
  sdMounted = SD.begin(SD_CS, SPI, SD_FREQ);
  uint32_t ms = millis() - t0;
  if (!sdMounted) {
    logf("#SD mount=0 ms=%lu", (unsigned long)ms);
    return;
  }
  logf("#SD mount=1 ms=%lu type=%d card_mb=%lu total_mb=%lu used_mb=%lu", (unsigned long)ms, (int)SD.cardType(),
       (unsigned long)(SD.cardSize() / (1024ULL * 1024ULL)), (unsigned long)(SD.totalBytes() / (1024ULL * 1024ULL)),
       (unsigned long)(SD.usedBytes() / (1024ULL * 1024ULL)));
}

int countFiles(const char* dirPath) {
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) return -1;
  int n = 0;
  File entry = dir.openNextFile();
  while (entry) {
    n++;
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  return n;
}

void sdTest(int kb) {
  if (!sdMounted) sdMount();
  if (!sdMounted) return;
  SD.mkdir("/ToolCheck");
  SD.mkdir("/ToolCheck/photos");
  static const char* PATH = "/ToolCheck/photos/20260913-061500-2.jpg";  // 8.3 に収まらない長い名前
  static uint8_t buf[4096];
  uint32_t total = (uint32_t)kb * 1024;

  uint32_t sumW = 0;
  uint32_t written = 0;
  uint32_t t0 = millis();
  File f = SD.open(PATH, FILE_WRITE);
  if (!f) {
    logf("#SD test open_write=0");
    return;
  }
  while (written < total) {
    uint32_t n = total - written;
    if (n > sizeof(buf)) n = sizeof(buf);
    for (uint32_t i = 0; i < n; i++) {
      buf[i] = (uint8_t)((written + i) * 31 + 7);
      sumW += buf[i];
    }
    if (f.write(buf, n) != n) break;
    written += n;
  }
  f.close();
  uint32_t writeMs = millis() - t0;

  uint32_t sumR = 0;
  uint32_t readBytes = 0;
  t0 = millis();
  File r = SD.open(PATH, FILE_READ);
  if (r) {
    while (true) {
      int n = r.read(buf, sizeof(buf));
      if (n <= 0) break;
      for (int i = 0; i < n; i++) sumR += buf[i];
      readBytes += (uint32_t)n;
    }
    r.close();
  }
  uint32_t readMs = millis() - t0;

  bool existed = SD.exists(PATH);
  int files = countFiles("/ToolCheck/photos");
  bool removed = SD.remove(PATH);
  bool existsAfter = SD.exists(PATH);
  logf("#SD test kb=%d written=%lu write_ms=%lu read=%lu read_ms=%lu verify=%d exists=%d files=%d removed=%d exists_after=%d",
       kb, (unsigned long)written, (unsigned long)writeMs, (unsigned long)readBytes, (unsigned long)readMs,
       (int)(written == total && readBytes == written && sumR == sumW), (int)existed, files, (int)removed, (int)existsAfter);
}

void sdLs() {
  if (!sdMounted) sdMount();
  if (!sdMounted) return;
  File root = SD.open("/");
  if (!root) {
    logf("#SDLS open=0");
    return;
  }
  int n = 0;
  File entry = root.openNextFile();
  while (entry) {
    logf("#SDLS name=%s dir=%d size=%lu", entry.name(), (int)entry.isDirectory(), (unsigned long)entry.size());
    n++;
    entry.close();
    entry = root.openNextFile();
  }
  root.close();
  logf("#SDLS count=%d", n);
}

// ---------------------------------------------------------------- microSD: 100 枚超の削除・抜き差し・書込中の電源断 (M3 の残り)

static const char*    PRUNE_DIR         = "/ToolCheck/prunetest";  // 既存の写真に触らないよう、試験専用のフォルダで試す
static const char*    STRESS_DIR        = "/ToolCheck/stress";
static const uint32_t PRUNE_FILE_BYTES  = 40 * 1024;
static const uint32_t STRESS_FILE_BYTES = 200 * 1024;

bool     sdWatch       = false;
uint32_t sdWatchNextAt = 0;
uint32_t sdWatchCount  = 0;

bool     stressActive  = false;
File     stressFile;
uint32_t stressIndex   = 0;
uint32_t stressWritten = 0;
uint32_t stressFileAt  = 0;

// 試験用の写真名 (2026-09-13 06:00:00 から 1 分ずつ)。count 件は 40KB、その後ろの zeros 件は 0 バイト (新しい名前でも消えるか)
void sdFill(int count, int zeros) {
  if (!sdMounted) sdMount();
  if (!sdMounted) return;
  SD.mkdir("/ToolCheck");
  SD.mkdir(PRUNE_DIR);
  static uint8_t buf[4096];
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(i * 13 + 5);
  uint32_t t0 = millis();
  int made = 0;
  int failed = 0;
  for (int i = 0; i < count + zeros; i++) {
    char path[64];
    snprintf(path, sizeof(path), "%s/20260913-%02d%02d00.jpg", PRUNE_DIR, 6 + i / 60, i % 60);
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
      failed++;
      continue;
    }
    bool ok = true;
    if (i < count) {
      for (uint32_t w = 0; w < PRUNE_FILE_BYTES && ok; w += sizeof(buf)) ok = f.write(buf, sizeof(buf)) == sizeof(buf);
    }
    f.close();
    ok ? made++ : failed++;
  }
  logf("#SDFILL made=%d failed=%d ms=%lu files=%d", made, failed, (unsigned long)(millis() - t0), countFiles(PRUNE_DIR));
}

// 本体の写真の削除と同じ形: 0 バイトは消し、残りは名前順で新しい keep 件を残す。列挙の順と時間を見る
void sdPrune(int keep) {
  if (!sdMounted) sdMount();
  if (!sdMounted) return;
  uint32_t t0 = millis();
  File dir = SD.open(PRUNE_DIR);
  if (!dir || !dir.isDirectory()) {
    logf("#SDPRUNE dir=0");
    return;
  }
  std::vector<String> paths;
  std::vector<String> zeroPaths;
  int unsortedPairs = 0;  // 列挙が名前順でなかった隣り合わせの数
  String prev = "";
  File e = dir.openNextFile();
  while (e) {
    if (!e.isDirectory()) {
      String p = e.path();
      if (prev.length() && strcmp(prev.c_str(), p.c_str()) > 0) unsortedPairs++;
      prev = p;
      (e.size() == 0 ? zeroPaths : paths).push_back(p);
    }
    e.close();
    e = dir.openNextFile();
  }
  dir.close();
  uint32_t listMs = millis() - t0;

  std::sort(paths.begin(), paths.end(), [](const String& a, const String& b) { return strcmp(a.c_str(), b.c_str()) < 0; });
  t0 = millis();
  int removed = 0;
  int failed = 0;
  for (const String& p : zeroPaths) SD.remove(p) ? removed++ : failed++;
  int excess = (int)paths.size() - keep;
  for (int i = 0; i < excess; i++) SD.remove(paths[i]) ? removed++ : failed++;
  uint32_t removeMs = millis() - t0;
  logf("#SDPRUNE listed=%u zero=%u unsorted_pairs=%d list_ms=%lu keep=%d removed=%d failed=%d remove_ms=%lu left=%d",
       (unsigned)(paths.size() + zeroPaths.size()), (unsigned)zeroPaths.size(), unsortedPairs, (unsigned long)listMs, keep, removed,
       failed, (unsigned long)removeMs, countFiles(PRUNE_DIR));
  if (!paths.empty()) {
    logf("#SDPRUNE oldest_left=%s newest=%s", paths[excess > 0 ? excess : 0].c_str(), paths.back().c_str());
  }
}

// 抜けたことを何で検出できるか: 方法ごとの結果と所要時間を 1 行に出す
void sdProbe() {
  uint32_t t0 = millis();
  int type = (int)SD.cardType();
  uint32_t typeMs = millis() - t0;
  t0 = millis();
  unsigned long cardMb = (unsigned long)(SD.cardSize() / (1024ULL * 1024ULL));
  uint32_t sizeMs = millis() - t0;
  t0 = millis();
  bool exists = SD.exists("/ToolCheck");
  uint32_t existsMs = millis() - t0;
  t0 = millis();
  bool wrote = false;
  File f = SD.open("/ToolCheck/probe.txt", FILE_WRITE);
  if (f) {
    wrote = f.print("probe") == 5;
    f.close();
  }
  uint32_t writeMs = millis() - t0;
  logf("#SDPROBE n=%lu mounted=%d type=%d(%lums) card_mb=%lu(%lums) exists=%d(%lums) write=%d(%lums)", (unsigned long)sdWatchCount,
       (int)sdMounted, type, (unsigned long)typeMs, cardMb, (unsigned long)sizeMs, (int)exists, (unsigned long)existsMs, (int)wrote,
       (unsigned long)writeMs);
}

void sdWatchUpdate() {
  if (!sdWatch || !timeReached(sdWatchNextAt)) return;
  sdWatchCount++;
  sdProbe();
  sdWatchNextAt = millis() + 1000;
}

void sdRemount() {
  SD.end();
  sdMounted = false;
  sdMount();
  drawScreen();
}

// 書込中に USB を抜く試験: 200KB のファイルを 4KB ずつ書き続ける (loop から少しずつ進める)
uint8_t stressByte(uint32_t index, uint32_t offset) { return (uint8_t)(index * 7 + offset); }

void stressOpenNext() {
  char path[64];
  snprintf(path, sizeof(path), "%s/%05lu.bin", STRESS_DIR, (unsigned long)stressIndex);
  stressFile = SD.open(path, FILE_WRITE);
  stressWritten = 0;
  stressFileAt = millis();
  if (!stressFile) {
    stressActive = false;
    logf("#STRESS open=0 path=%s", path);
  }
}

void stressStart() {
  if (!sdMounted) sdMount();
  if (!sdMounted) return;
  SD.mkdir("/ToolCheck");
  SD.mkdir(STRESS_DIR);
  int existing = countFiles(STRESS_DIR);
  stressIndex = existing < 0 ? 0 : (uint32_t)existing;
  stressActive = true;
  stressOpenNext();
  setStatus("SD 書込中", "いつでも USB を抜いてください");
  logf("#STRESS start index=%lu", (unsigned long)stressIndex);
}

void stressUpdate() {
  if (!stressActive) return;
  static uint8_t buf[4096];
  uint32_t n = STRESS_FILE_BYTES - stressWritten;
  if (n > sizeof(buf)) n = sizeof(buf);
  for (uint32_t i = 0; i < n; i++) buf[i] = stressByte(stressIndex, stressWritten + i);
  if (stressFile.write(buf, n) != n) {
    stressFile.close();
    stressActive = false;
    logf("#STRESS write_fail index=%lu at=%lu", (unsigned long)stressIndex, (unsigned long)stressWritten);
    return;
  }
  stressWritten += n;
  if (stressWritten < STRESS_FILE_BYTES) return;
  stressFile.close();
  uint32_t ms = millis() - stressFileAt;
  stressIndex++;
  if (stressIndex % 5 == 0) {
    logf("#STRESS files=%lu last_ms=%lu", (unsigned long)stressIndex, (unsigned long)ms);
    char line[64];
    snprintf(line, sizeof(line), "%lu 件目 — いつでも USB を抜いてください", (unsigned long)stressIndex);
    setStatus("SD 書込中", line);
  }
  stressOpenNext();
}

// 電源断の後: 書込試験のファイルを全部読み、大きさと中身を確かめる
void sdCheck() {
  if (!sdMounted) sdMount();
  if (!sdMounted) return;
  uint32_t t0 = millis();
  File dir = SD.open(STRESS_DIR);
  if (!dir || !dir.isDirectory()) {
    logf("#SDCHECK dir=0");
    return;
  }
  static uint8_t buf[4096];
  int ok = 0;
  int shortFiles = 0;
  int zero = 0;
  int badContent = 0;
  File e = dir.openNextFile();
  while (e) {
    if (!e.isDirectory()) {
      uint32_t size = (uint32_t)e.size();
      uint32_t index = (uint32_t)strtoul(e.name(), nullptr, 10);
      bool same = true;
      uint32_t offset = 0;
      while (true) {
        int n = e.read(buf, sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n && same; i++) same = buf[i] == stressByte(index, offset + (uint32_t)i);
        offset += (uint32_t)n;
      }
      if (size == 0) {
        zero++;
      } else if (!same || offset != size) {
        badContent++;
      } else if (size < STRESS_FILE_BYTES) {
        shortFiles++;
      } else {
        ok++;
      }
      if (size != STRESS_FILE_BYTES || !same) {
        logf("#SDCHECK file=%s size=%lu read=%lu content=%d", e.name(), (unsigned long)size, (unsigned long)offset, (int)same);
      }
    }
    e.close();
    e = dir.openNextFile();
  }
  dir.close();
  logf("#SDCHECK ok=%d short=%d zero=%d bad_content=%d ms=%lu", ok, shortFiles, zero, badContent, (unsigned long)(millis() - t0));
}

// 試験で作ったファイルだけを消す (prunetest / stress / probe.txt)。既存の写真には触らない
void sdCleanDir(const char* dirPath, int* removed, int* failed) {
  File dir = SD.open(dirPath);
  if (!dir || !dir.isDirectory()) return;
  std::vector<String> paths;
  File e = dir.openNextFile();
  while (e) {
    if (!e.isDirectory()) paths.push_back(e.path());
    e.close();
    e = dir.openNextFile();
  }
  dir.close();
  for (const String& p : paths) SD.remove(p) ? (*removed)++ : (*failed)++;
  SD.rmdir(dirPath);
}

void sdClean() {
  if (!sdMounted) sdMount();
  if (!sdMounted) return;
  int removed = 0;
  int failed = 0;
  sdCleanDir(PRUNE_DIR, &removed, &failed);
  sdCleanDir(STRESS_DIR, &removed, &failed);
  if (SD.exists("/ToolCheck/probe.txt")) SD.remove("/ToolCheck/probe.txt") ? removed++ : failed++;
  logf("#SDCLEAN removed=%d failed=%d", removed, failed);
}

// ---------------------------------------------------------------- RTC (保持時間の試験)

void rtcPrint() {
  auto dt = M5.Rtc.getDateTime();
  logf("#RTC %04d-%02d-%02dT%02d:%02d:%02d enabled=%d volt_low=%d up_ms=%lu", (int)dt.date.year, (int)dt.date.month, (int)dt.date.date,
       (int)dt.time.hours, (int)dt.time.minutes, (int)dt.time.seconds, (int)M5.Rtc.isEnabled(), (int)M5.Rtc.getVoltLow(),
       (unsigned long)millis());
}

void rtcSet(const String& text) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  if (sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) {
    logf("#ERR rtc set YYYY-MM-DDThh:mm:ss");
    return;
  }
  M5.Rtc.setDateTime({{(int16_t)y, (int8_t)mo, (int8_t)d}, {(int8_t)h, (int8_t)mi, (int8_t)se}});
  rtcPrint();
}

// ---------------------------------------------------------------- 入力

void pollQr() {
  if (!qrOk) return;
  uint8_t ready = qrcode.getDecodeReadyStatus();
  if (ready == 0 || ready == 0xFF) return;
  uint16_t len = qrcode.getDecodeLength();
  bool bad = (len == 0 || len > QR_READ_MAX);
  uint16_t readLen = bad ? QR_READ_MAX : len;
  uint8_t data[QR_READ_MAX + 1] = {0};
  qrcode.getDecodeData(data, readLen);  // READY を落とすため、長さが異常でも必ず読み出す
  data[readLen] = 0;
  if (bad) return;
  lastQr = String((char*)data);
  lastQr.trim();
  qrCount++;
  logf("#QR n=%lu data=%s", (unsigned long)qrCount, lastQr.c_str());
  if (seqStep == 0 && soundDemoIndex < 0 && brightDemoIndex < 0) drawScreen();
}

void pollTouch() {
  if (M5.Touch.getCount() == 0) return;
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) logf("#TOUCH x=%d y=%d screen=%s", t.x, t.y, screenOn ? "on" : "sleep");
}

void printInfo() {
  logf("#INFO chip=%s rev=%d cpu=%lu heap=%lu psram=%lu qr=%s qr_auto=%d bright=%d screen=%d spk=%d sd=%d",
       ESP.getChipModel(), (int)ESP.getChipRevision(), (unsigned long)getCpuFrequencyMhz(), (unsigned long)ESP.getFreeHeap(),
       (unsigned long)ESP.getFreePsram(), qrOk ? "ok" : "ng", (int)qrAuto, brightPct, (int)screenOn,
       (int)M5.Speaker.isEnabled(), (int)sdMounted);
}

void handleCommand(const String& cmd) {
  Serial.printf("#CMD %s\n", cmd.c_str());
  int a = 0;
  int b = 0;
  if (cmd == "info") {
    printInfo();
  } else if (cmd == "io") {
    printIo();
  } else if (cmd == "sounddemo") {
    brightDemoIndex = -1;
    seqStep = 0;
    soundDemoIndex = 0;
    soundDemoNextAt = millis();
  } else if (cmd.startsWith("op ")) {
    a = cmd.substring(3).toInt();
    playPattern(PAT_ID_OP, a, 1);
    logf("#OK op vol=%d", a);
  } else if (cmd.startsWith("w1 ") || cmd.startsWith("w2 ")) {
    b = 4;
    sscanf(cmd.c_str() + 3, "%d %d", &a, &b);
    int id = cmd.startsWith("w1") ? PAT_ID_W1 : PAT_ID_W2;
    playPattern(id, a, cyclesFor(id, (uint32_t)b * 1000));
    logf("#OK %s vol=%d sec=%d", cmd.startsWith("w1") ? "w1" : "w2", a, b);
  } else if (cmd == "stop") {
    soundDemoIndex = -1;
    stopPattern();
    logf("#OK stop");
  } else if (cmd == "brightdemo") {
    soundDemoIndex = -1;
    seqStep = 0;
    brightDemoIndex = 0;
    brightDemoNextAt = millis();
  } else if (cmd.startsWith("bright ")) {
    setBrightPct(cmd.substring(7).toInt());
    drawScreen();
    logf("#OK bright=%d", brightPct);
  } else if (cmd == "seq" || cmd.startsWith("seq ")) {
    a = 20;  // 1 手順の秒数
    b = 1;   // 始める手順
    sscanf(cmd.c_str() + 3, "%d %d", &a, &b);
    if (a < 5) a = 5;
    if (b < 1 || b > SEQ_LAST) b = 1;
    soundDemoIndex = -1;
    brightDemoIndex = -1;
    // 途中から始めるときは、それより前の手順の状態を (鳴らさず・待たずに) 作ってから始める
    for (int s = 1; s < b; s++) seqApply(s);
    seqStepMs = (uint32_t)a * 1000;
    seqStep = b;
    seqBegin(seqStep);
  } else if (cmd == "c6 on" || cmd == "c6 off") {
    setC6(cmd == "c6 on");
    logf("#OK %s", cmd.c_str());
  } else if (cmd == "usba on" || cmd == "usba off") {
    setUsbA(cmd == "usba on");
    logf("#OK %s", cmd.c_str());
  } else if (cmd == "spk on" || cmd == "spk off") {
    if (cmd == "spk on") {
      M5.Speaker.begin();
    } else {
      M5.Speaker.end();
    }
    logf("#OK %s enabled=%d", cmd.c_str(), (int)M5.Speaker.isEnabled());
  } else if (cmd == "qr auto" || cmd == "qr manual") {
    bool ok = setQrAuto(cmd == "qr auto");
    logf("#OK %s verified=%d", cmd.c_str(), (int)ok);
  } else if (cmd == "panel sleep") {
    M5.Display.sleep();
    screenOn = false;
    logf("#OK panel=sleep (タッチすると #TOUCH screen=sleep が出るか)");
  } else if (cmd == "panel wake") {
    M5.Display.wakeup();
    screenOn = true;
    setBrightPct(brightPct);
    drawScreen();
    logf("#OK panel=wake");
  } else if (cmd == "sd mount") {
    sdMount();
    drawScreen();
  } else if (cmd == "sd info") {
    logf("#SD mounted=%d type=%d", (int)sdMounted, sdMounted ? (int)SD.cardType() : -1);
  } else if (cmd == "sd test" || cmd.startsWith("sd test ")) {
    a = (cmd.length() > 8) ? cmd.substring(8).toInt() : 200;
    if (a < 1) a = 1;
    sdTest(a);
  } else if (cmd == "sd ls") {
    sdLs();
  } else if (cmd == "sd end") {
    SD.end();
    sdMounted = false;
    drawScreen();
    logf("#OK sd end");
  } else if (cmd == "sd fill" || cmd.startsWith("sd fill ")) {
    a = 105;  // 40KB の写真
    b = 3;    // 0 バイトの写真
    sscanf(cmd.c_str() + 7, "%d %d", &a, &b);
    a = constrain(a, 0, 500);
    b = constrain(b, 0, 20);
    sdFill(a, b);
  } else if (cmd == "sd prune" || cmd.startsWith("sd prune ")) {
    a = 100;
    sscanf(cmd.c_str() + 8, "%d", &a);
    sdPrune(a < 0 ? 0 : a);
  } else if (cmd == "sd probe") {
    sdProbe();
  } else if (cmd == "sd watch on" || cmd == "sd watch off") {
    sdWatch = (cmd == "sd watch on");
    sdWatchCount = 0;
    sdWatchNextAt = millis();
    logf("#OK sd watch=%d", (int)sdWatch);
  } else if (cmd == "sd remount") {
    sdRemount();
  } else if (cmd == "sd stress") {
    stressStart();
  } else if (cmd == "sd stress stop") {
    if (stressActive) {
      stressFile.close();
      stressActive = false;
    }
    logf("#OK sd stress stop files=%lu", (unsigned long)stressIndex);
  } else if (cmd == "sd check") {
    sdCheck();
  } else if (cmd == "sd clean") {
    sdClean();
  } else if (cmd == "rtc") {
    rtcPrint();
  } else if (cmd.startsWith("rtc set ")) {
    rtcSet(cmd.substring(8));
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
  setBrightPct(60);

  // M5UnitQRCode の begin() は速度を uint8_t に切り詰めて 160Hz にするので、先に 100kHz で開いておく (M1 で確認)
  Wire.begin(PORTA_SDA, PORTA_SCL, 100000);
  for (int i = 0; i < 5 && !qrOk; i++) {
    qrOk = qrcode.begin(&Wire, QR_ADDR, PORTA_SDA, PORTA_SCL, 100000U);
    if (!qrOk) delay(200);
  }
  Wire.setClock(100000);
  if (qrOk) setQrAuto(true);

  drawScreen();
  printInfo();
  printIo();
  logf("#READY m2_bench");
}

void loop() {
  M5.update();
  pollSerial();
  uint32_t now = millis();
  if (now - lastQrPoll >= QR_POLL_MS) {
    lastQrPoll = now;
    pollQr();
  }
  pollTouch();
  updatePattern();
  soundDemoUpdate();
  brightDemoUpdate();
  seqUpdate();
  sdWatchUpdate();
  stressUpdate();
  delay(2);
}
