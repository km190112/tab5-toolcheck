#include "app.h"

#include <M5Unified.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../core/about.h"
#include "../core/code_classifier.h"
#include "../core/csv_export.h"
#include "../core/license_text.h"
#include "../core/time_format.h"
#include "../core/tof_reading.h"
#include "esp_heap_caps.h"
#include "../core/photo_store.h"
#include "../hw/internal_i2c.h"
#include "../hw/power_hw.h"
#include "../ui/jp_text.h"

namespace toolcheck {
namespace {

const char* const kFirmwareVersion = about::kFirmwareVersion;  // 版数とリリース日は core/about.h で持つ
const char* const kPartition = "tooldb";
constexpr uint32_t kClockRedrawMs = 1000;
constexpr uint32_t kQrPollMs = 20;
constexpr uint32_t kSessionTickMs = 100;
constexpr uint32_t kSessionRedrawMs = 1000;  // 残り秒・経過秒の書き直し
constexpr uint32_t kSdCheckMs = 5000;        // 待機中の SD の確認 (減光・消灯中の 30 秒は 6 段目)
constexpr uint32_t kCameraStuckMs = 5000;    // 取り込みがこれを超えたら、押さえを待たずにタッチと時計を読む
constexpr uint32_t kPhotoIdleHoldMs = 10000; // 古い写真を消すアイドル判定の無操作時間
constexpr size_t kPhotoViewBytes = 1024 * 1024;

// 画面の区画 (縦 720x1280)
constexpr int kMargin = 20;
constexpr int kLineH = 52;
constexpr int kClockY = 90;
constexpr int kSessionY = 540;
constexpr size_t kSessionRowsShown = 12;

const char* cfgStatusName(ConfigLoadStatus s) {
  switch (s) {
    case ConfigLoadStatus::Loaded: return "loaded";
    case ConfigLoadStatus::NotFound: return "not_found";
    case ConfigLoadStatus::Unreadable: return "unreadable";
    case ConfigLoadStatus::UnknownVersion: return "unknown_version";
    case ConfigLoadStatus::Invalid: return "invalid";
  }
  return "?";
}

const char* cfgStatusText(ConfigLoadStatus s) {
  switch (s) {
    case ConfigLoadStatus::Loaded: return "保存された値";
    case ConfigLoadStatus::NotFound: return "保存なし (既定値)";
    case ConfigLoadStatus::Unreadable: return "読めない (既定値。保存された値は消していない)";
    case ConfigLoadStatus::UnknownVersion: return "知らない版数 (既定値。保存された値は消していない)";
    case ConfigLoadStatus::Invalid: return "値が正しくない (既定値。保存された値は消していない)";
  }
  return "?";
}

const char* logKindName(OpenLogKind k) {
  switch (k) {
    case OpenLogKind::Session: return "session";
    case OpenLogKind::PausedOpen: return "paused_open";
    case OpenLogKind::PauseStarted: return "pause_started";
    case OpenLogKind::PauseEnded: return "pause_ended";
    case OpenLogKind::RecordsCleared: return "records_cleared";
    case OpenLogKind::PhotosCleared: return "photos_cleared";
  }
  return "?";
}

const char* codeKindName(CodeKind k) {
  switch (k) {
    case CodeKind::User: return "user";
    case CodeKind::Item: return "item";
    case CodeKind::Invalid: return "invalid";
  }
  return "?";
}

const char* reasonName(InvalidReason r) {
  switch (r) {
    case InvalidReason::None: return "none";
    case InvalidReason::Empty: return "empty";
    case InvalidReason::TooShort: return "too_short";
    case InvalidReason::TooLong: return "too_long";
    case InvalidReason::ForbiddenChar: return "forbidden_char";
  }
  return "?";
}

const char* reasonText(InvalidReason r) {
  switch (r) {
    case InvalidReason::None: return "";
    case InvalidReason::Empty: return "中身の無い QR です";
    case InvalidReason::TooShort: return "物品番号が短すぎます (2 文字以上)";
    case InvalidReason::TooLong: return "物品番号が長すぎます (15 文字まで)";
    // 一覧の下の知らせに収まる長さにする (「読めない QR: 使えない文字 (空白・カンマなど) が入っています」が右端で切れた。2026-09-15 実機)
    case InvalidReason::ForbiddenChar: return "空白・カンマなどの文字は使えません";
  }
  return "";
}

const char* stateName(SessionState s) {
  switch (s) {
    case SessionState::Idle: return "idle";
    case SessionState::Collecting: return "collecting";
    case SessionState::TagWindow: return "tag_window";
  }
  return "?";
}

const char* endName(SessionEnd e) {
  switch (e) {
    case SessionEnd::Confirmed: return "confirmed";
    case SessionEnd::GaveUp: return "gave_up";
    case SessionEnd::PausedOpen: return "paused_open";
    case SessionEnd::Cancelled: return "cancelled";
  }
  return "?";
}

const char* rowKindName(RowKind k) {
  switch (k) {
    case RowKind::Checkout: return "checkout";
    case RowKind::Return: return "return";
    case RowKind::Switch: return "switch";
  }
  return "?";
}

const char* rowStatusName(RowStatus s) {
  switch (s) {
    case RowStatus::Pending: return "pending";
    case RowStatus::Done: return "done";
    case RowStatus::Failed: return "failed";
  }
  return "?";
}

const char* bookResultName(BookResult r) {
  switch (r) {
    case BookResult::Ok: return "ok";
    case BookResult::NotLent: return "not_lent";
    case BookResult::AlreadyLent: return "already_lent";
    case BookResult::Full: return "full";
    case BookResult::InvalidArg: return "invalid_arg";
    case BookResult::StorageError: return "storage_error";
  }
  return "?";
}

// line を空白で区切り、語の先頭を words に入れる (line を書き換える)。語の数を返す (cap を超えた分は cap + 1 を返す)
size_t splitWords(char* line, char** words, size_t cap) {
  size_t count = 0;
  char* p = line;
  while (*p != '\0') {
    while (*p == ' ') ++p;
    if (*p == '\0') break;
    if (count == cap) return cap + 1;
    words[count++] = p;
    while (*p != '\0' && *p != ' ') ++p;
    if (*p == ' ') *p++ = '\0';
  }
  return count;
}

// 0 以上の 10 進の整数。読めなければ false
bool parseIndex(const char* text, size_t* out) {
  if (text == nullptr || *text == '\0') return false;
  size_t value = 0;
  for (const char* p = text; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') return false;
    value = value * 10 + static_cast<size_t>(*p - '0');
    if (value > 100000) return false;
  }
  *out = value;
  return true;
}

// dump は行ごとに送信リングの空きを待ってから書く (USB が抜けたと一瞬見えると、一杯のリングから古いバイトが捨てられるため。
// serial_port.h の waitWritable)。待ちきれなくても書く (PC が繋がっていなければ読む人もいない)
constexpr uint32_t kDumpWaitMs = 500;

void printSectionBegin(const char* name) {
  char buf[48];
  formatSectionBegin(name, buf, sizeof(buf));
  SerialPort::waitWritable(256, kDumpWaitMs);  // 続く見出しの行のぶんも待つ
  Serial.println(buf);
}

void printSectionEnd(const char* name, size_t rows) {
  char buf[64];
  formatSectionEnd(name, rows, buf, sizeof(buf));
  SerialPort::waitWritable(std::strlen(buf) + 2, kDumpWaitMs);
  Serial.println(buf);
}

// 1 行を出す。バッファに収まらなかったら出さずに false
bool printCsv(const CsvLine& line) {
  if (!line.ok()) {
    Serial.println("#WARN 行が長すぎて出せません");
    return false;
  }
  SerialPort::waitWritable(std::strlen(line.c_str()) + 2, kDumpWaitMs);
  Serial.println(line.c_str());
  return true;
}

// "2026-09-13(日) 10:42:05"。時刻が無効なら false
bool formatLocalDateTime(int64_t epoch, int32_t tz, char* buf, size_t len) {
  static const char* const kWeekdays[7] = {"日", "月", "火", "水", "木", "金", "土"};
  LocalDateTime t;
  if (!toLocalDateTime(epoch, tz, &t)) return false;
  std::snprintf(buf, len, "%04d-%02d-%02d(%s) %02d:%02d:%02d", t.year, t.month, t.day, kWeekdays[t.weekday], t.hour,
                t.minute, t.second);
  return true;
}

const char* drawerStateName(DrawerState s) {
  switch (s) {
    case DrawerState::Uncalibrated:
      return "uncalibrated";
    case DrawerState::Closed:
      return "closed";
    case DrawerState::Open:
      return "open";
  }
  return "?";
}

const char* tofCalibrationResultName(TofCalibrationResult r) {
  switch (r) {
    case TofCalibrationResult::Ok:
      return "ok";
    case TofCalibrationResult::TooFewSamples:
      return "too_few_samples";
    case TofCalibrationResult::OutOfRange:
      return "out_of_range";
    case TofCalibrationResult::Unstable:
      return "unstable";
  }
  return "?";
}

const char* powerStateName(PowerState s) {
  switch (s) {
    case PowerState::Active: return "active";
    case PowerState::Idle: return "idle";
    case PowerState::Dim: return "dim";
    case PowerState::Off: return "off";
  }
  return "?";
}

const char* powerStateText(PowerState s) {
  switch (s) {
    case PowerState::Active: return "操作中";
    case PowerState::Idle: return "待機";
    case PowerState::Dim: return "減光";
    case PowerState::Off: return "消灯";
  }
  return "?";
}

bool hasEvents(const SessionOutput& out) {
  return out.record_count > 0 || out.pause_started || out.pause_ended || out.show_user_loans || out.capture_photo ||
         out.rows_full || out.cancelled;
}

}  // namespace

App::App()
    : kv_(kPartition),
      book_(kv_),
      log_(kv_),
      session_(book_, cfg_),
      power_(cfg_, 0),
      ui_(book_, log_, session_, cfg_, rtc_, qr_, camera_, sd_, drawer_, *this) {}

void App::begin() {
  const uint32_t now = millis();
  power_hw::begin();
  rtc_.begin(now);
  storage_err_ = kv_.begin();  // 開けなくても消さない
  loadRecords();
  power_ = PowerManager(cfg_, millis());
  bus_.begin(cfg_.drawer_sensor_enabled, millis());  // Port A の Wire を開き、PaHub の ch を探して繋ぐ (QR・ToF より先)
  qr_.begin(millis());
  drawer_.begin(cfg_.drawer_sensor_enabled, cfg_, tof_period_ms_, millis());
  // スピーカーは起動時に始めない (始めるときの音がうるさかった。2026-09-14)。鳴らすときに Alarm が内部 I2C を押さえてから始める
  M5.Touch.setHoldThresh(1000);  // 「設定」の長押し
  internal_i2c::begin();
  sd_.begin(kSdCheckMs);
  camera_.begin(cfg_.camera_enabled, &sd_);  // 内部 I2C を使う。ループと撮影タスクの取り合いが始まる前に済ませる
  applyBrightness();
  ui_.begin(millis(), kv_.ready(), esp_err_to_name(storage_err_));
  printBootReport();
  Serial.printf("#READY toolcheck %s\n", kFirmwareVersion);
}

void App::loop() {
  const uint32_t entry = millis();
  if (last_loop_entry_ms_ != 0) {
    const uint32_t gap = entry - last_loop_entry_ms_;
    if (gap > loop_max_ms_) loop_max_ms_ = gap;
    if (gap >= kSlowLoopMs) ++loop_slow_;
  }
  last_loop_entry_ms_ = entry;
  char line[256];
  if (serial_.poll(line, sizeof(line))) handleCommand(line);
  // コマンドの処理より後に時刻を取る。先に取ると、コマンドがセッションを進めた時点 (millis()) より「今」が前になり、
  // Session::alarmLevel の経過時間が一周して警告 2 が一瞬鳴った (2026-09-13 M5 2 段目)。Session は now_ms が減らない前提
  const uint32_t now = millis();
  pollInternalI2c(now);
  pollPortA(now);
  pollQr(now);
  pollDrawerSensor(now);
  updateSession(now);
  pollCamera();
  pollSd(now);
  updatePower(now);
  ui_.loop(now);
}

// 起動直後は読むだけで書かない (loadConfig / LoanBook::load / OpenLog::load はどれも書かない)
void App::loadRecords() {
  book_report_ = LoadReport{};
  log_report_ = OpenLogLoadReport{};
  if (!kv_.ready()) {
    cfg_ = Config{};
    cfg_result_ = ConfigLoadResult{};
    cfg_result_.status = ConfigLoadStatus::Unreadable;
    session_.setConfig(cfg_);
    return;
  }
  cfg_result_ = loadConfig(kv_, &cfg_);
  session_.setConfig(cfg_);
  book_report_ = book_.load();
  log_report_ = log_.load();
}

void App::printBootReport() {
  const uint32_t now = millis();
  Serial.printf("#BOOT storage=%s err=%s cfg=%s cfg_error=%d loans=%u loans_ok=%d skipped_loans=%u repair_pending=%d "
                "hist=%u hist_ok=%d openlog=%u openlog_ok=%d alerts=%u alerts_ok=%d skipped_alerts=%u "
                "dropped_alerts=%u rtc_ok=%d time_lost=%d rtc_flags=0x%02x qr=%d qr_fw=0x%02x drawer_sensor=%d tof=%d "
                "camera_enabled=%d camera_ready=%d camera_init_ms=%lu\n",
                kv_.ready() ? "ok" : "ng", esp_err_to_name(storage_err_), cfgStatusName(cfg_result_.status),
                static_cast<int>(cfg_result_.error), static_cast<unsigned>(book_.loanCount()),
                static_cast<int>(book_report_.loans_ok), static_cast<unsigned>(book_report_.skipped_loans),
                static_cast<int>(book_report_.repair_pending), static_cast<unsigned>(book_.returnCount()),
                static_cast<int>(book_report_.history_ok), static_cast<unsigned>(log_.count()),
                static_cast<int>(log_report_.log_ok), static_cast<unsigned>(log_.alertCount()),
                static_cast<int>(log_report_.alerts_ok), static_cast<unsigned>(log_report_.skipped_alerts),
                static_cast<unsigned>(log_.droppedAlerts()), static_cast<int>(rtc_.ok()),
                static_cast<int>(rtc_.timeLost(now)), static_cast<unsigned>(rtc_.flags()),
                static_cast<int>(qr_.present()), static_cast<unsigned>(qr_.firmwareVersion()),
                static_cast<int>(drawer_.enabled()), static_cast<int>(drawer_.present()),
                static_cast<int>(camera_.enabled()), static_cast<int>(camera_.ready()),
                static_cast<unsigned long>(camera_.initMs()));
  printI2cRoute("boot");
  if (!qr_.present()) Serial.println("[起動] QR ユニットが見つかりません (Port A の配線を確かめてください)");
  if (drawer_.enabled() && !drawer_.present()) {
    Serial.println("[起動] 引き出しセンサ (Unit ToF) が見つかりません。付けないなら設定画面の「引き出しセンサ」か "
                   "cfg set drawer_sensor_enabled 0 で使わない設定にしてください");
  }
  if (camera_.enabled() && !camera_.ready()) Serial.printf("[起動] カメラを使えません: %s\n", camera_.initError());
  if (!kv_.ready()) {
    Serial.printf("[起動] 記録領域 (%s) を読めません (%s)。自動では消しません。控えが要らなければ nvs erase all YES\n",
                  kPartition, esp_err_to_name(storage_err_));
    return;
  }
  if (cfg_result_.status != ConfigLoadStatus::Loaded) {
    Serial.printf("[起動] 設定: %s\n", cfgStatusText(cfg_result_.status));
  }
  if (cfg_result_.status == ConfigLoadStatus::Invalid) {
    Serial.printf("[起動] 設定の誤り: %s\n", describeConfigError(cfg_result_.error));
  }
  if (!book_report_.loans_ok) Serial.println("[起動] 貸出中のキーを列挙できません");
  if (book_report_.skipped_loans > 0) {
    Serial.printf("[起動] 読めなかった貸出が %u 件あります (消さずに残しています)\n",
                  static_cast<unsigned>(book_report_.skipped_loans));
  }
  if (book_report_.repair_pending) {
    Serial.println("[起動] 閉じる途中で電源が落ちた貸出を一覧から外しました (キーは次の変更の直前に消します)");
  }
  if (!book_report_.history_ok) Serial.println("[起動] 返却履歴の管理情報が壊れています (0 件として扱います)");
  if (!log_report_.log_ok) Serial.println("[起動] 開閉ログの管理情報が壊れています (0 件として扱います)");
  if (!log_report_.alerts_ok) Serial.println("[起動] 赤帯のキーを列挙できません");
  if (log_report_.skipped_alerts > 0) {
    Serial.printf("[起動] 読めなかった赤帯が %u 件あります (消さずに残しています)\n",
                  static_cast<unsigned>(log_report_.skipped_alerts));
  }
  if (rtc_.timeLost(now)) {
    Serial.println("[起動] 時刻が未設定か、電源断で狂っています。time set YYYY-MM-DDTHH:MM (現地時刻) で合わせてください");
  }
}

void App::applyBrightness() {
  M5.Display.setBrightness(static_cast<uint8_t>(cfg_.brightness_pct * 255 / 100));
  applied_brightness_ = cfg_.brightness_pct;  // 減光・消灯中なら、次の updatePower で状態の明るさに戻る
}

// --- 省電力 (6 段目) ---

void App::updatePower(uint32_t now_ms) {
  PowerInputs in;
  in.now_ms = now_ms;
  // セッション・設定・検索・写真表示・名札の一覧・撮影の間は操作中 (計画「省電力」)
  // 基準を取っている間も操作中 (ToF を 50ms で測る。減光中の 200ms だと 2 秒で 10 個しか集まらず、校正の下限すれすれだった。2026-09-15 実機)
  in.busy = session_.state() != SessionState::Idle || ui_.busy() || camera_.busyMs(now_ms) > 0 || calib_run_.active;
  in.alarm = session_.alarmLevel(now_ms) > 0;
  in.unconfirmed_unregistered = log_.alertCount() > 0;  // 未確認の無登録があるあいだは消灯せず減光で止める
  applyPower(power_.update(in), now_ms);
}

// 変わったものだけ反映する。CPU は画面を点けている間 360MHz から落とせない (M2)、パネルスリープとライトスリープは使わない。
// スピーカーは入切しない: 状態が変わるたびに止めたり始めたりすると始めるときの音が出るおそれがある (2026-09-14「起動音がうるさい」)。
// M2 の実測で差は約 0.01A。入切しないと決めた (2026-09-15)
void App::applyPower(const PowerOutputs& out, uint32_t now_ms) {
  if (out.state != power_state_) {
    Serial.printf("#POWER state=%s brightness=%u qr=%s\n", powerStateName(out.state),
                  static_cast<unsigned>(out.brightness_pct), out.qr_continuous ? "continuous" : "duty");
    power_state_ = out.state;
  }
  if (out.brightness_pct != applied_brightness_) {
    M5.Display.setBrightness(static_cast<uint8_t>(out.brightness_pct * 255 / 100));
    applied_brightness_ = out.brightness_pct;
  }
  const bool duty = !out.qr_continuous;
  if (duty != qr_duty_) {
    qr_.setDuty(duty, now_ms);  // 減光・消灯中は 1400ms ごとに 700ms だけ読む
    qr_duty_ = duty;
  }
  const uint32_t sd_interval = (out.state == PowerState::Dim || out.state == PowerState::Off) ? 30000 : 5000;
  if (sd_interval != sd_interval_ms_) {
    sd_.setCheckInterval(sd_interval);  // 計画: 待機 5 秒 / 減光・消灯 30 秒
    sd_interval_ms_ = sd_interval;
  }
  if (out.tof_period_ms != tof_period_ms_) {
    drawer_.setPeriod(out.tof_period_ms);  // 計画: 操作中 50 / 待機 100 / 減光・消灯 200ms
    tof_period_ms_ = out.tof_period_ms;
  }
}

void App::cmdPower() {
  Serial.printf("#POWER state=%s brightness=%u qr=%s sd_check_ms=%lu dim_after_min=%u off_after_min=%u unconfirmed=%u\n",
                powerStateName(power_state_), static_cast<unsigned>(applied_brightness_), qr_duty_ ? "duty" : "continuous",
                static_cast<unsigned long>(sd_interval_ms_), static_cast<unsigned>(cfg_.dim_after_min),
                static_cast<unsigned>(cfg_.off_after_min), static_cast<unsigned>(log_.alertCount()));
}

// --- 設定画面から頼まれる操作 (SettingsActions) ---

const Config& App::currentConfig() const { return cfg_; }

bool App::applySettings(const Config& cfg, ConfigError* error) {
  ConfigError e = ConfigError::None;
  if (!kv_.ready() || !saveConfig(kv_, cfg, &e)) {
    if (error != nullptr) *error = e;
    Serial.printf("#ERR settings %s\n", e != ConfigError::None ? describeConfigError(e) : "保存できませんでした");
    return false;
  }
  if (error != nullptr) *error = ConfigError::None;
  const bool camera_changed = cfg.camera_enabled != camera_.enabled();
  cfg_ = cfg;
  cfg_result_ = ConfigLoadResult{};
  cfg_result_.status = ConfigLoadStatus::Loaded;
  session_.setConfig(cfg_);
  power_.setConfig(cfg_);
  applyDrawerSetting();
  applyBrightness();
  drawStatusScreen();
  Serial.println("#OK settings saved (設定画面)");
  if (camera_changed) Serial.println("[設定] カメラの入切は再起動で反映します");
  return true;
}

void App::previewBrightness(uint8_t pct) { M5.Display.setBrightness(static_cast<uint8_t>(pct * 255 / 100)); }

void App::testSound(uint8_t kind, uint8_t volume_pct) {
  const uint32_t now = millis();
  if (kind == 0) {
    alarm_.beep(volume_pct);
  } else {
    alarm_.preview(kind, volume_pct, now, 1500);
  }
  Serial.printf("#OK settings test_sound kind=%u volume=%u muted=%d\n", static_cast<unsigned>(kind),
                static_cast<unsigned>(volume_pct), static_cast<int>(alarm_.muted()));
}

void App::takeTestPhoto() { cmdPhotoTest(millis()); }

int64_t App::nowEpoch() const { return rtc_.now(millis()); }

bool App::timeLost() const { return rtc_.timeLost(millis()); }

bool App::setClock(int64_t epoch) {
  const bool ok = rtc_.set(epoch, millis());
  Serial.printf("%s settings clock epoch=%lld\n", ok ? "#OK" : "#ERR", static_cast<long long>(epoch));
  if (ok) drawClockLine();
  return ok;
}

bool App::warningsPaused(uint32_t* remaining_ms) const {
  if (remaining_ms != nullptr) *remaining_ms = session_.pauseRemainingMs(millis());
  return session_.isPaused();
}

void App::pauseWarnings(uint32_t minutes) {
  const uint32_t now = millis();
  const SessionOutput out = session_.pause(minutes * 60000, now, rtc_.now(now));
  handleSessionOutput(out, now);
  printSession(now);
  drawClockLine();
}

void App::resumeWarnings() {
  const uint32_t now = millis();
  const SessionOutput out = session_.resume(now, rtc_.now(now));
  handleSessionOutput(out, now);
  printSession(now);
  drawClockLine();
}

bool App::sdPresent() const { return sd_.present(); }

uint16_t App::sdPhotoCount() const { return sd_.photoCount(); }

// SD の写真の全削除 (設定画面と USB シリアルの sd erase photos YES で同じ)。結果は pollSd の PhotosDeleted で開閉ログに残す
bool App::eraseSdPhotos() {
  if (!sd_.present()) {
    Serial.println("#ERR sd erase photos SD がありません");
    return false;
  }
  const bool queued = sd_.requestDeleteAllPhotos();
  Serial.println(queued ? "#OK sd erase photos (結果は #SD photos_deleted)" : "#ERR sd erase photos キューが一杯です");
  return queued;
}

bool App::drawerSensorPresent() const { return drawer_.present(); }

bool App::confirmAlert(uint16_t index) {
  const bool ok = log_.confirmAlert(index);
  Serial.printf("%s settings confirm_alert index=%u left=%u dropped=%u\n", ok ? "#OK" : "#ERR",
                static_cast<unsigned>(index), static_cast<unsigned>(log_.alertCount()),
                static_cast<unsigned>(log_.droppedAlerts()));
  if (ok) drawStatusScreen();
  return ok;
}

bool App::cancelLoan(const char* item) {
  const uint32_t now = millis();
  const BookResult r = book_.cancelLoan(item, rtc_.now(now));
  Serial.printf("%s settings cancel_loan item=%s result=%s\n", r == BookResult::Ok ? "#OK" : "#ERR", item,
                bookResultName(r));
  if (r == BookResult::Ok) drawStatusScreen();
  return r == BookResult::Ok;
}

void App::exportRecords(const char* what) { cmdDump(what); }

// 記録の削除 (設定画面と USB シリアルの nvs erase records YES で同じ)
bool App::eraseRecords() {
  if (!kv_.ready()) return false;
  const bool loans_cleared = book_.clear();
  const bool log_cleared = log_.clear(rtc_.now(millis()));
  Serial.printf("%s nvs erase records loans_hist=%d openlog_alerts=%d (設定は残しています)\n",
                (loans_cleared && log_cleared) ? "#OK" : "#ERR", static_cast<int>(loans_cleared),
                static_cast<int>(log_cleared));
  book_report_ = LoadReport{};
  log_report_ = OpenLogLoadReport{};
  drawStatusScreen();
  return loans_cleared && log_cleared;
}

void App::eraseAll() { cmdNvsErase("all", "YES"); }

size_t App::deviceInfo(char lines[][kInfoLineLen], size_t max_lines) {
  const uint32_t now = millis();
  size_t n = 0;
  const int rev = static_cast<int>(ESP.getChipRevision());
  if (n < max_lines) std::snprintf(lines[n++], kInfoLineLen, "版数 %s", kFirmwareVersion);
  if (n < max_lines) {
    std::snprintf(lines[n++], kInfoLineLen, "%s rev %d.%d / CPU %lu MHz", ESP.getChipModel(), rev / 100, rev % 100,
                  static_cast<unsigned long>(getCpuFrequencyMhz()));
  }
  if (n < max_lines) {
    std::snprintf(lines[n++], kInfoLineLen, "空きメモリ %lu KB / PSRAM %lu MB",
                  static_cast<unsigned long>(ESP.getFreeHeap() / 1024),
                  static_cast<unsigned long>(ESP.getFreePsram() / (1024 * 1024)));
  }
  nvs_stats_t st;
  if (n < max_lines) {
    if (kv_.ready() && kv_.stats(&st)) {
      std::snprintf(lines[n++], kInfoLineLen, "記録領域 使用 %u / %u エントリ", static_cast<unsigned>(st.used_entries),
                    static_cast<unsigned>(st.total_entries));
    } else {
      std::snprintf(lines[n++], kInfoLineLen, "記録領域を読めません");
    }
  }
  if (n < max_lines) {
    std::snprintf(lines[n++], kInfoLineLen, "貸出中 %u / 返却履歴 %u / 開閉ログ %u / 赤帯 %u",
                  static_cast<unsigned>(book_.loanCount()), static_cast<unsigned>(book_.returnCount()),
                  static_cast<unsigned>(log_.count()), static_cast<unsigned>(log_.alertCount()));
  }
  if (n < max_lines) {
    std::snprintf(lines[n++], kInfoLineLen, "QR ユニット: %s (読取 %lu・エラー %lu)",
                  !qr_.present() ? "見つかりません" : qr_.failed() ? "読めません" : "接続",
                  static_cast<unsigned long>(qr_.readCount()), static_cast<unsigned long>(qr_.errorCount()));
  }
  if (n < max_lines) {
    char route[64];
    bus_.formatHuman(route, sizeof(route));
    std::snprintf(lines[n++], kInfoLineLen, "Port A: %s", route);
  }
  if (n < max_lines) {
    char tof[56];  // 行は kInfoLineLen (80) で、見出しの「引き出しセンサ: 」が 23 バイト (64 だと -Wformat-truncation)
    if (!drawer_.enabled()) {
      std::snprintf(tof, sizeof(tof), "使わない (名札で撮影)");
    } else if (!drawer_.present()) {
      std::snprintf(tof, sizeof(tof), "Unit ToF が見つかりません");
    } else if (!drawer_.hasSample() || !drawer_.lastValid()) {
      std::snprintf(tof, sizeof(tof), "範囲外%s", drawer_.fault() ? " (異常)" : "");
    } else if (!drawer_.calibrated()) {
      std::snprintf(tof, sizeof(tof), "%u mm (未校正)", static_cast<unsigned>(drawer_.lastMm()));
    } else {
      std::snprintf(tof, sizeof(tof), "%u mm / 基準 %u mm", static_cast<unsigned>(drawer_.lastMm()),
                    static_cast<unsigned>(cfg_.tof_baseline_mm));
    }
    std::snprintf(lines[n++], kInfoLineLen, "引き出しセンサ: %s", tof);
  }
  if (n < max_lines) {
    std::snprintf(lines[n++], kInfoLineLen, "カメラ: %s",
                  !camera_.enabled() ? "使わない設定" : camera_.ready() ? "準備 OK" : camera_.initError());
  }
  if (n < max_lines) {
    if (sd_.present()) {
      std::snprintf(lines[n++], kInfoLineLen, "SD: あり (写真 %u 枚)", static_cast<unsigned>(sd_.photoCount()));
    } else {
      std::snprintf(lines[n++], kInfoLineLen, "SD: なし");
    }
  }
  if (n < max_lines) {
    const unsigned long minutes = static_cast<unsigned long>(now / 60000);
    std::snprintf(lines[n++], kInfoLineLen, "稼働時間 %lu 時間 %02lu 分", minutes / 60, minutes % 60);
  }
  if (n < max_lines) {
    std::snprintf(lines[n++], kInfoLineLen, "時刻: %s%s%s", rtc_.timeLost(now) ? "未設定か狂っている" : "正常",
                  (rtc_.flags() & kRtcFlagVlf) != 0 ? " (発振停止)" : "",
                  (rtc_.flags() & kRtcFlagVblf) != 0 ? " (電圧低下)" : "");
  }
  if (n < max_lines) {
    std::snprintf(lines[n++], kInfoLineLen, "電力状態: %s (明るさ %u%%・QR %s)", powerStateText(power_state_),
                  static_cast<unsigned>(applied_brightness_), qr_duty_ ? "間欠" : "連続");
  }
  if (n < max_lines) {
    std::snprintf(lines[n++], kInfoLineLen, "開発フラグ: %s / 消音: %s", cfg_.dev_sim ? "ON" : "OFF",
                  alarm_.muted() ? "ON" : "OFF");
  }
  return n;
}

// --- 画面への合図 (描くのは UiController) ---

void App::drawStatusScreen() {
  ui_.onRecordsChanged();
  ui_.onStatusChanged();
}

void App::drawClockLine() { ui_.onStatusChanged(); }

void App::drawSessionArea(uint32_t now_ms) {
  (void)now_ms;
  session_dirty_ = false;
  ui_.onSessionChanged();
}

void App::setEventText(const char* text) {
  ui_.setEventText(text, millis());
  ui_.onSessionChanged();
}

// --- QR・引き出し・セッション ---

void App::pollQr(uint32_t now_ms) {
  if (now_ms - last_qr_ms_ < kQrPollMs) return;
  last_qr_ms_ = now_ms;
  char raw[kQrReadMax + 1];
  const size_t len = qr_.poll(now_ms, raw, sizeof(raw));
  if (len > 0) handleCode(raw, len, false, now_ms);
}

void App::handleCode(const char* raw, size_t len, bool simulated, uint32_t now_ms) {
  ClassifierConfig cc;
  cc.user_id_digits = cfg_.user_id_digits;
  cc.user_id_alnum = cfg_.user_id_alnum;
  const ClassifiedCode code = classifyCode(raw, len, cc);
  // 減光・消灯中の間欠読み取りでは、トリガを開けた直後に、かざしていないのに読めない中身 (NUL や記号混じり) が返ることがあり、
  // それで毎回起きて減光が続かなかった (2026-09-14 6 段目)。読めない QR では起こさず、記録だけ出して捨てる
  if (code.kind == CodeKind::Invalid && (power_state_ == PowerState::Dim || power_state_ == PowerState::Off)) {
    Serial.printf("#QR src=%s kind=invalid reason=%s len=%u ignored=1 (減光・消灯中の読めない QR では起こさない)\n",
                  simulated ? "sim" : "unit", reasonName(code.reason), static_cast<unsigned>(len));
    return;
  }
  last_activity_ms_ = now_ms;
  applyPower(power_.onActivity(ActivityKind::QrRead, now_ms, nullptr), now_ms);  // 処理より先に操作中へ
  if (!kv_.ready()) {
    Serial.printf("#QR src=%s kind=%s text=%s ignored=1 (記録領域を読めないので登録しません)\n",
                  simulated ? "sim" : "unit", codeKindName(code.kind), code.text);
    return;
  }
  // 引き出しセンサを使わない設定では、名札で確定したときに撮る (撮るかは Session が決める)。写真名は撮るときだけ確定する
  char photo[kPhotoFieldLen] = {0};
  PhotoNameKey photo_key;
  const bool offer_photo = code.kind == CodeKind::User && camera_.ready();
  if (offer_photo) peekPhotoName(now_ms, photo, sizeof(photo), &photo_key);
  const SessionOutput out = session_.onCode(code, now_ms, rtc_.now(now_ms), offer_photo ? photo : nullptr);
  Serial.printf("#QR src=%s kind=%s reason=%s len=%u text=%s ignored=%d\n", simulated ? "sim" : "unit",
                codeKindName(code.kind), reasonName(code.reason), static_cast<unsigned>(code.length), code.text,
                static_cast<int>(out.ignored));
  if (out.capture_photo && offer_photo) {
    commitPhotoName(photo_key);
    requestCapture(photo);
  }
  if (code.kind == CodeKind::Invalid) {
    char text[128];
    std::snprintf(text, sizeof(text), "読めない QR: %s", reasonText(code.reason));
    setEventText(text);
  }
  if (!out.ignored) {
    alarm_.beep(cfg_.volume_operation_pct);
    ui_.onSessionChanged();  // 行が増えた・状態が変わった (4 段目の最初の版で抜けて、行が描かれなかった)
  }
  handleSessionOutput(out, now_ms);
  // 名札で確定した (名札先に入った) = 登録。作業者が前にいる間は次の開放を作らない (2026-09-16)
  if (code.kind == CodeKind::User && session_.state() == SessionState::TagWindow) holdDrawerOpen("tag");
  if (!out.ignored) printSession(now_ms);
}

void App::handleDrawer(bool open, const char* src, uint32_t now_ms) {
  if (!cfg_.drawer_sensor_enabled) {
    Serial.println("#ERR 引き出しセンサを使わない設定なので開閉は受け付けません (cfg set drawer_sensor_enabled 1)");
    return;
  }
  if (open && ui_.settingsOpen()) {
    // ToF は設定画面の間は判定を止めている。シリアルの注入も同じに扱う (2026-09-16)
    Serial.println("#OK drawer 設定画面を開いているので開放を受け付けません");
    return;
  }
  if (open == drawer_open_) {
    Serial.println(open ? "#OK drawer 既に開いています" : "#OK drawer 既に閉じています");
    return;
  }
  drawer_open_ = open;
  last_activity_ms_ = now_ms;
  if (open) applyPower(power_.onActivity(ActivityKind::DrawerOpen, now_ms, nullptr), now_ms);
  Serial.printf("#DRAWER %s src=%s\n", open ? "open" : "close", src);
  if (!open) return;
  if (!kv_.ready()) {
    Serial.println("#WARN 記録領域を読めないのでセッションを起こしません");
    return;
  }
  // 写真名は開放を検知した時点の時刻で決め、記録と SD の保存で同じ名前を使う
  char photo[kPhotoFieldLen] = {0};
  const bool camera = camera_.ready();
  if (camera) {
    PhotoNameKey key;
    peekPhotoName(now_ms, photo, sizeof(photo), &key);
    commitPhotoName(key);  // 開放では必ず撮る
  }
  const SessionOutput out = session_.onDrawerOpened(now_ms, rtc_.now(now_ms), camera ? photo : nullptr);
  if (out.capture_photo && camera) requestCapture(photo);
  handleSessionOutput(out, now_ms);
  printSession(now_ms);
}

// 取りやめ (2026-09-15: QR を読んで持って行くのをやめるとき、セッションごと 1 回押しで)
void App::cancelSession(const char* src, uint32_t now_ms) {
  const SessionOutput out = session_.cancel(now_ms, rtc_.now(now_ms));
  Serial.printf("#CANCEL src=%s cancelled=%d records=%u\n", src, static_cast<int>(out.cancelled),
                static_cast<unsigned>(out.record_count));
  handleSessionOutput(out, now_ms);
  if (out.cancelled && out.record_count == 0) setEventText("取りやめました");  // 記録するものが無かった
  session_dirty_ = true;
  printSession(now_ms);
}

// 返却完了 (2026-09-16: 返却だけのセッションを 20 秒待たずに終えて、すぐ貸し出せるように)
void App::completeReturn(const char* src, uint32_t now_ms) {
  const SessionOutput out = session_.completeReturn(now_ms, rtc_.now(now_ms));
  Serial.printf("#COMPLETE src=%s kind=return records=%u\n", src, static_cast<unsigned>(out.record_count));
  handleSessionOutput(out, now_ms);
  session_dirty_ = true;
  printSession(now_ms);
}

// 完了 (2026-09-16: 名札先の「続けて物品を読めます」の画面でも、20 秒を待たずにすぐ次の人を登録したい)
void App::completeTagWindow(const char* src, uint32_t now_ms) {
  const SessionOutput out = session_.completeTagWindow(now_ms, rtc_.now(now_ms));
  Serial.printf("#COMPLETE src=%s kind=tag records=%u\n", src, static_cast<unsigned>(out.record_count));
  handleSessionOutput(out, now_ms);
  session_dirty_ = true;
  printSession(now_ms);
}

void App::holdDrawerOpen(const char* reason) {
  if (!drawer_.enabled()) return;
  const bool was_held = drawer_.openHeld();
  drawer_.holdOpenUntilQuiet();
  if (!was_held) Serial.printf("#DRAWER hold reason=%s (閉の範囲に 2 秒続けて戻るまで開放を受け付けません)\n", reason);
}

void App::handleSessionOutput(const SessionOutput& out, uint32_t now_ms) {
  const int64_t epoch = rtc_.now(now_ms);
  bool records_changed = false;
  // セッションが終わった (確定・返却完了・取りやめ・打ち切り) = 登録。開放で次のセッションに区切ったときは押さえない
  if ((out.record_count > 0 || out.cancelled) && session_.state() == SessionState::Idle) holdDrawerOpen("record");
  for (uint8_t i = 0; i < out.record_count && i < 2; ++i) {
    const SessionRecord& r = out.records[i];
    const bool saved = log_.appendSession(r);
    records_changed = true;
    Serial.printf("#RECORD end=%s opened=%d user=%s photo=%s checkouts=%u returns=%u switches=%u failures=%u "
                  "unregistered_open=%d unknown_checkout=%d saved=%d\n",
                  endName(r.end), static_cast<int>(r.opened), r.user, r.photo, static_cast<unsigned>(r.checkouts),
                  static_cast<unsigned>(r.returns), static_cast<unsigned>(r.switches),
                  static_cast<unsigned>(r.failures), static_cast<int>(r.unregistered_open),
                  static_cast<int>(r.unknown_checkout), static_cast<int>(saved));
    if (!saved) Serial.println("[記録] 開閉ログ・赤帯に書けませんでした");
    if (r.unregistered_open) {
      setEventText("登録せずに持ち出されました (赤帯)");
    } else if (r.unknown_checkout) {
      setEventText("利用者不明の持出があります (赤帯)");
    } else if (r.end == SessionEnd::Confirmed || r.end == SessionEnd::Cancelled) {
      // 0 件の数は省く (「持出 0 / 返却 1 / 切替 1 を記録しました」が一覧の下で右端から切れた。2026-09-15 実機)
      char counts[64] = {0};
      size_t used = 0;
      const auto add = [&](const char* label, unsigned n) {
        if (n == 0 || used >= sizeof(counts)) return;
        const int w = std::snprintf(counts + used, sizeof(counts) - used, "%s%s %u", used > 0 ? "・" : "", label, n);
        if (w > 0) used += static_cast<size_t>(w);
      };
      add("持出", r.checkouts);
      add("返却", r.returns);
      add("切替", r.switches);
      char text[160];
      if (r.end == SessionEnd::Cancelled) {
        // 取りやめ (2026-09-15)。読んだ時点で済んだ返却は残っている
        std::snprintf(text, sizeof(text), "取りやめました%s%s%s", used > 0 ? " (" : "", used > 0 ? counts : "",
                      used > 0 ? " は記録済み)" : "");
      } else if (r.user[0] != '\0') {
        std::snprintf(text, sizeof(text), "%s さん: %s%s%s", r.user, used > 0 ? counts : "物品の読み取りなし",
                      used > 0 ? " を記録しました" : "で記録しました", r.failures > 0 ? " (失敗あり)" : "");
      } else {
        // 返却だけ (名札を読まずに終えた。2026-09-15: 返した人は記録しない)
        std::snprintf(text, sizeof(text), "%s%s", used > 0 ? counts : "返却を記録できませんでした",
                      used > 0 ? (r.failures > 0 ? " を記録しました (失敗あり)" : " を記録しました") : "");
      }
      setEventText(text);
    }
  }
  if (out.pause_started) {
    records_changed = true;
    if (!log_.appendEvent(OpenLogKind::PauseStarted, epoch)) Serial.println("[記録] 一時停止の開始を書けませんでした");
    Serial.println("#PAUSE started");
  }
  if (out.pause_ended) {
    records_changed = true;
    if (!log_.appendEvent(OpenLogKind::PauseEnded, epoch)) Serial.println("[記録] 一時停止の終了を書けませんでした");
    Serial.println("#PAUSE ended");
  }
  if (out.show_user_loans) {
    printUserLoans(out.loans_user);
    ui_.onUserLoans(out.loans_user, now_ms);
  }
  if (out.rows_full) setEventText("1 回に扱えるのは 20 件までです");
  // 撮影の依頼は写真名が要るので handleDrawer で出す
  if (records_changed) {
    drawStatusScreen();  // 件数が変わる
  } else if (hasEvents(out)) {
    session_dirty_ = true;
  }
}

void App::updateSession(uint32_t now_ms) {
  if (!kv_.ready()) return;
  if (now_ms - last_tick_ms_ >= kSessionTickMs) {
    last_tick_ms_ = now_ms;
    const SessionOutput out = session_.tick(now_ms, rtc_.now(now_ms));
    if (hasEvents(out)) {
      handleSessionOutput(out, now_ms);
      printSession(now_ms);
    }
  }
  const uint8_t level = session_.alarmLevel(now_ms);
  // 試し鳴らしの間は、セッションの段で上書きしない (試し鳴らしは短く、終われば次のループで段に戻る)
  if (!alarm_.previewing()) {
    alarm_.setLevel(level, level == 2 ? cfg_.volume_warning2_pct : cfg_.volume_warning1_pct, now_ms);
  }
  alarm_.update(now_ms);
  if (level != last_alarm_level_) {
    Serial.printf("#ALARM level=%u\n", static_cast<unsigned>(level));
    last_alarm_level_ = level;
    session_dirty_ = true;
  }
  if (session_.state() != last_state_) {
    last_state_ = session_.state();
    session_dirty_ = true;
  }
  if (session_dirty_) drawSessionArea(now_ms);  // 残り秒・点滅の書き直しは UiController が回す
}

void App::printSession(uint32_t now_ms) {
  Serial.printf("#SESSION state=%s rows=%u alarm=%u since_ms=%lu opened=%d tag_user=%s paused=%d pause_left_ms=%lu "
                "counting=%d can_cancel=%d can_complete=%d can_complete_tag=%d\n",
                stateName(session_.state()), static_cast<unsigned>(session_.rowCount()),
                static_cast<unsigned>(session_.alarmLevel(now_ms)),
                static_cast<unsigned long>(session_.sinceProgressMs(now_ms)), static_cast<int>(session_.hasOpened()),
                session_.tagUser(), static_cast<int>(session_.isPaused()),
                static_cast<unsigned long>(session_.pauseRemainingMs(now_ms)), static_cast<int>(session_.isCounting()),
                static_cast<int>(session_.canCancel()), static_cast<int>(session_.canCompleteReturn()),
                static_cast<int>(session_.canCompleteTagWindow()));
  for (size_t i = 0; i < session_.rowCount(); ++i) {
    SessionRow row;
    if (!session_.getRow(i, &row)) continue;
    Serial.printf("#ROW index=%u kind=%s status=%s result=%s item=%s borrower=%s\n", static_cast<unsigned>(i),
                  rowKindName(row.kind), rowStatusName(row.status), bookResultName(row.result), row.item,
                  row.borrower);
  }
}

void App::printUserLoans(const char* user) {
  std::vector<Loan> loans(kMaxLoans);
  const size_t n = book_.listLoansOf(user, loans.data(), loans.size());
  Serial.printf("#USERLOANS user=%s count=%u\n", user, static_cast<unsigned>(n));
  char when[64];
  for (size_t i = 0; i < n; ++i) {
    if (!formatLocalDateTime(loans[i].checkout_epoch, cfg_.tz_offset_min, when, sizeof(when))) {
      std::snprintf(when, sizeof(when), "-");
    }
    Serial.printf("#LOAN item=%s checkout=%s\n", loans[i].item, when);
  }
  char text[128];
  int used = std::snprintf(text, sizeof(text), "%s さんの貸出中 %u 件", user, static_cast<unsigned>(n));
  for (size_t i = 0; i < n && used > 0 && static_cast<size_t>(used) < sizeof(text) - 1; ++i) {
    used += std::snprintf(text + used, sizeof(text) - static_cast<size_t>(used), "%s%s", i == 0 ? ": " : ", ",
                          loans[i].item);
  }
  setEventText(text);
}

// --- 内部 I2C・撮影・SD ---

void App::pollInternalI2c(uint32_t now_ms) {
  if (internal_i2c::tryLock()) {
    M5.update();
    rtc_.update(now_ms);
    internal_i2c::unlock();
    camera_stuck_reported_ = false;
  } else if (camera_.busyMs(now_ms) > kCameraStuckMs) {
    // 取り込みが終わらない (VIDIOC_DQBUF はタイムアウトしない)。画面とタッチを止め続けないよう、押さえを待たずに読む
    if (!camera_stuck_reported_) {
      Serial.println("#ERR camera 取り込みが 5 秒を超えました。押さえを待たずにタッチと時計を読みます");
      camera_stuck_reported_ = true;
    }
    M5.update();
    rtc_.update(now_ms);
  } else {
    return;  // 取り込み中 (約 0.6 秒) はタッチと時計を読まない
  }
  if (M5.Touch.getCount() == 0) {
    // 離した瞬間だけ知らせる (毎ループ知らせると、sim press で入れた押し続けがすぐ消えた)
    if (touching_) ui_.onReleased();
    touching_ = false;
    press_swallowed_ = false;
    return;
  }
  touching_ = true;
  last_activity_ms_ = now_ms;
  const auto& t = M5.Touch.getDetail();
  if (t.wasPressed()) handleTouch(t.x, t.y, now_ms);  // 押した瞬間 (手袋でも押せるよう、離したときを待たない)
  if (press_swallowed_) return;                        // 起こすだけに使った押し方は、長押しにも押し続けにも使わない
  if (t.wasHold()) ui_.onHold(t.x, t.y, now_ms);      // 1 秒押し続けた (「設定」の長押し)
  if (t.isPressed()) ui_.onPressing(t.x, t.y, now_ms - t.base_msec);  // 押し続けた時間 (削除の 3 秒)
}

void App::handleTouch(int x, int y, uint32_t now_ms) {
  last_activity_ms_ = now_ms;
  // 減光・消灯中の最初のタッチは起こすだけ (計画「省電力」。見えていない画面を押してしまわないため)。処理より先に反映する
  bool swallow = false;
  applyPower(power_.onActivity(ActivityKind::Touch, now_ms, &swallow), now_ms);
  press_swallowed_ = swallow;
  if (swallow) {
    Serial.println("#POWER wake (最初のタッチは起こすだけ)");
    return;
  }
  const TouchResult r = ui_.onTouch(x, y, now_ms);
  if (r != TouchResult::None) alarm_.beep(cfg_.volume_operation_pct);
  if (r == TouchResult::SessionChanged) printSession(now_ms);
  if (r == TouchResult::CancelSession) cancelSession("touch", now_ms);
  if (r == TouchResult::CompleteReturn) completeReturn("touch", now_ms);
  if (r == TouchResult::CompleteTagWindow) completeTagWindow("touch", now_ms);
}

void App::pollCamera() {
  CaptureResult r;
  while (camera_.pollResult(&r)) {
    Serial.printf("#PHOTO name=%s ok=%d test=%d first_frame_ms=%lu used_frame_ms=%lu frames=%lu encode_ms=%lu "
                  "bytes=%lu sd_queued=%d error=%s\n",
                  r.name, static_cast<int>(r.ok), static_cast<int>(r.test),
                  static_cast<unsigned long>(r.first_frame_ms), static_cast<unsigned long>(r.used_frame_ms),
                  static_cast<unsigned long>(r.frames), static_cast<unsigned long>(r.encode_ms),
                  static_cast<unsigned long>(r.jpeg_bytes), static_cast<int>(r.sd_queued), r.error);
    if (!r.ok) {
      char text[128];
      std::snprintf(text, sizeof(text), "写真を撮れませんでした: %s", r.error);
      setEventText(text);
    } else if (r.test) {
      showLatestPhoto();
    }
  }
}

void App::pollSd(uint32_t now_ms) {
  SdEvent ev;
  while (sd_.pollEvent(&ev)) {
    switch (ev.kind) {
      case SdEventKind::Mounted:
        Serial.printf("#SD mounted ms=%lu photos=%u\n", static_cast<unsigned long>(ev.ms),
                      static_cast<unsigned>(ev.photos));
        prune_pending_ = true;
        drawStatusScreen();
        break;
      case SdEventKind::Unmounted:
        Serial.printf("#SD unmounted ms=%lu\n", static_cast<unsigned long>(ev.ms));
        setEventText("microSD が抜けました (写真は保存されません)");
        drawStatusScreen();
        break;
      case SdEventKind::Saved:
        Serial.printf("#SD saved name=%s bytes=%lu ms=%lu photos=%u\n", ev.name, static_cast<unsigned long>(ev.bytes),
                      static_cast<unsigned long>(ev.ms), static_cast<unsigned>(ev.photos));
        prune_pending_ = true;
        break;
      case SdEventKind::SaveFailed:
        Serial.printf("#SD save_failed name=%s ms=%lu\n", ev.name, static_cast<unsigned long>(ev.ms));
        setEventText("写真を microSD に書けませんでした");
        drawStatusScreen();
        break;
      case SdEventKind::SaveSkipped:
        Serial.printf("#SD save_skipped name=%s (SD なし)\n", ev.name);
        break;
      case SdEventKind::Pruned:
        Serial.printf("#SD pruned removed=%u photos=%u ms=%lu\n", static_cast<unsigned>(ev.removed),
                      static_cast<unsigned>(ev.photos), static_cast<unsigned long>(ev.ms));
        if (ev.removed > 0) drawStatusScreen();
        break;
      case SdEventKind::PruneFailed:
        Serial.printf("#SD prune_failed ms=%lu\n", static_cast<unsigned long>(ev.ms));
        break;
      case SdEventKind::PhotosDeleted: {
        Serial.printf("#SD photos_deleted removed=%u photos=%u ms=%lu\n", static_cast<unsigned>(ev.removed),
                      static_cast<unsigned>(ev.photos), static_cast<unsigned long>(ev.ms));
        // 写真を消したことを後から追えるように、開閉ログに残す (2026-09-14)
        const bool logged = kv_.ready() && log_.appendEvent(OpenLogKind::PhotosCleared, rtc_.now(now_ms));
        if (!logged) Serial.println("[記録] 写真の全削除を開閉ログに書けませんでした");
        ui_.onPhotosErased(true, ev.removed, ev.photos);
        drawStatusScreen();
        break;
      }
      case SdEventKind::PhotosDeleteFailed:
        Serial.printf("#SD photos_delete_failed photos=%u ms=%lu (SD なし・写真のフォルダを開けない)\n",
                      static_cast<unsigned>(ev.photos), static_cast<unsigned long>(ev.ms));
        ui_.onPhotosErased(false, 0, ev.photos);
        break;
      case SdEventKind::ReadDone:
      case SdEventKind::ReadFailed:
        Serial.printf("#SD read name=%s ok=%d bytes=%lu ms=%lu\n", ev.name,
                      static_cast<int>(ev.kind == SdEventKind::ReadDone), static_cast<unsigned long>(ev.bytes),
                      static_cast<unsigned long>(ev.ms));
        ui_.onPhotoRead(ev);  // data の持ち主は UiController に移る
        break;
    }
  }
  if (prune_pending_ && sd_.present() &&
      isPhotoCleanupIdle(session_.state() != SessionState::Idle, drawer_open_, now_ms - last_activity_ms_,
                         kPhotoIdleHoldMs)) {
    if (sd_.requestPrune()) prune_pending_ = false;
  }
}

// Port A の経路 (PaHub の ch) を確かめ、変わったら知らせる (挿し替えの自動検知。2026-09-15)
void App::pollPortA(uint32_t now_ms) {
  if (!bus_.update(now_ms)) return;
  printI2cRoute("changed");
  ui_.onStatusChanged();
}

void App::printI2cRoute(const char* reason) {
  char route[48];
  bus_.formatMachine(route, sizeof(route));
  Serial.printf("#I2C %s searches=%lu search_ms=%lu reason=%s\n", route,
                static_cast<unsigned long>(bus_.searchCount()), static_cast<unsigned long>(bus_.lastSearchMs()),
                reason);
}

void App::pollDrawerSensor(uint32_t now_ms) {
  updateDrawerJudging();
  const DrawerSensorUpdate u = drawer_.update(now_ms);
  if (u.sample) {
    if (tof_raw_active_) {
      Serial.printf("#TOF ms=%lu mm=%u status=%u valid=%d state=%s\n", static_cast<unsigned long>(now_ms),
                    static_cast<unsigned>(u.mm), static_cast<unsigned>(u.range_status), static_cast<int>(u.valid),
                    drawerStateName(drawer_.state()));
    }
    if (calib_run_.active && calib_run_.count < kTofCalibrationBufLen) calib_run_.mm[calib_run_.count++] = u.mm;
  }
  if (tof_raw_active_ && static_cast<int32_t>(now_ms - tof_raw_until_ms_) >= 0) {
    tof_raw_active_ = false;
    Serial.println("#TOF raw end");
  }
  if (calib_run_.active && (static_cast<int32_t>(now_ms - calib_run_.until_ms) >= 0 || !drawer_.present())) {
    finishTofCalibration();
  }
  if (u.rearmed) Serial.println("#DRAWER rearm (閉の範囲に 2 秒続けて戻ったので、次の開放を受け付けます)");
  if (u.opened) handleDrawer(true, "tof", now_ms);
  if (u.closed) handleDrawer(false, "tof", now_ms);
  if (u.status_changed) {
    Serial.printf("#DRAWER sensor enabled=%d present=%d fault=%d calibrated=%d\n", static_cast<int>(drawer_.enabled()),
                  static_cast<int>(drawer_.present()), static_cast<int>(drawer_.fault()),
                  static_cast<int>(drawer_.calibrated()));
    ui_.onStatusChanged();
  }
}

// 引き出しセンサの入切・基準距離・閾値を反映する (設定画面の保存と cfg set の後)
void App::applyDrawerSetting() {
  if (drawer_.setConfig(cfg_)) {
    Serial.printf("[設定] 引き出しセンサの基準 %u mm・開 %u mm・閉 %u mm (閉から数え直します)\n",
                  static_cast<unsigned>(cfg_.tof_baseline_mm), static_cast<unsigned>(cfg_.open_delta_mm),
                  static_cast<unsigned>(cfg_.close_delta_mm));
    if (drawer_open_) {
      drawer_open_ = false;  // 判定を作り直したので、開いたままの状態を持ち越さない
      Serial.println("#DRAWER close src=reset");
    }
    ui_.onStatusChanged();
  }
  if (cfg_.drawer_sensor_enabled == drawer_.enabled()) return;
  drawer_.setEnabled(cfg_.drawer_sensor_enabled, millis());
  bus_.setTofWanted(cfg_.drawer_sensor_enabled);  // 使わない設定なら ToF が無くても探し直さない
  if (!cfg_.drawer_sensor_enabled) drawer_open_ = false;  // 注入で開いたままの状態を持ち越さない
  Serial.printf("[設定] 引き出しセンサを%s (tof=%d)\n",
                cfg_.drawer_sensor_enabled ? "使います" : "使いません。名札で確定したときに撮影します",
                static_cast<int>(drawer_.present()));
  ui_.onStatusChanged();
}

void App::updateDrawerJudging() {
  // 2026-09-16: 設定画面を開いている間は ToF が反応してもセッション画面に移さない (前は「引き出しセンサ」の画面だけ)
  const bool judging = !calib_run_.active && !ui_.settingsOpen();
  if (judging == drawer_.judging()) return;
  drawer_.setJudging(judging);
  Serial.printf("[ToF] 開閉の判定を%s\n", judging ? "再開します (閉から数え直す)" : "止めます (設定画面・校正中)");
  if (!judging) return;
  if (drawer_open_) {
    drawer_open_ = false;  // 判定は閉から数え直すので、開いたままの状態を持ち越さない
    Serial.println("#DRAWER close src=reset");
  }
  holdDrawerOpen("resume");  // 設定画面の前に立っていた人で開放を作らない
}

bool App::beginTofCalibration(bool save, uint32_t now_ms) {
  if (!drawer_.present() || calib_run_.active) return false;
  calib_run_.active = true;
  calib_run_.save = save;
  calib_run_.until_ms = now_ms + kTofCalibrationMs;
  calib_run_.count = 0;
  updateDrawerJudging();
  ui_.onStatusChanged();
  return true;
}

void App::finishTofCalibration() {
  calib_run_.active = false;
  last_calibration_ = calibrateTofBaseline(calib_run_.mm, calib_run_.count);
  ++calibration_seq_;
  Serial.printf("#TOF calibrate result=%s baseline_mm=%u min_mm=%u max_mm=%u valid=%u samples=%u save=%d\n",
                tofCalibrationResultName(last_calibration_.result), static_cast<unsigned>(last_calibration_.baseline_mm),
                static_cast<unsigned>(last_calibration_.min_mm), static_cast<unsigned>(last_calibration_.max_mm),
                static_cast<unsigned>(last_calibration_.valid_samples), static_cast<unsigned>(calib_run_.count),
                static_cast<int>(calib_run_.save));
  if (calib_run_.save && last_calibration_.result == TofCalibrationResult::Ok) {
    Config candidate = cfg_;
    candidate.tof_baseline_mm = last_calibration_.baseline_mm;
    ConfigError error = ConfigError::None;
    if (applySettings(candidate, &error)) {  // 違反・保存できないときは applySettings が #ERR を出す
      Serial.printf("#OK tof calibrate saved baseline_mm=%u\n", static_cast<unsigned>(candidate.tof_baseline_mm));
    }
  }
  updateDrawerJudging();
  ui_.onStatusChanged();
}

void App::tofStatus(TofStatus* out) const {
  out->enabled = drawer_.enabled();
  out->present = drawer_.present();
  out->fault = drawer_.fault();
  out->has_sample = drawer_.hasSample();
  out->mm = drawer_.lastMm();
  out->valid = drawer_.lastValid();
  out->calibrating = calib_run_.active;
  out->calibration_seq = calibration_seq_;
  out->calibration = last_calibration_;
  bus_.formatHuman(out->route, sizeof(out->route));
}

bool App::startTofCalibration() { return beginTofCalibration(false, millis()); }

// 写真名 (現地時刻。同じ秒の 2 枚目は -2、時刻未設定は 00000000-<起動からの秒>)
void App::peekPhotoName(uint32_t now_ms, char* buf, size_t cap, PhotoNameKey* key) {
  const bool lost = rtc_.timeLost(now_ms);
  const int64_t epoch = lost ? 0 : rtc_.now(now_ms);
  const uint32_t uptime_s = now_ms / 1000;
  key->key = lost ? -static_cast<int64_t>(uptime_s) - 1 : epoch;
  key->seq = (key->key == last_photo_key_ && photo_seq_ < 255) ? static_cast<uint8_t>(photo_seq_ + 1) : 1;
  if (!makePhotoName(epoch, cfg_.tz_offset_min, uptime_s, key->seq, buf, cap)) {
    std::snprintf(buf, cap, "photo-%lu.jpg", static_cast<unsigned long>(now_ms));
  }
}

void App::commitPhotoName(const PhotoNameKey& key) {
  last_photo_key_ = key.key;
  photo_seq_ = key.seq;
}

void App::requestCapture(const char* photo) {
  const bool save = shouldSavePhotoToSd(cfg_.camera_enabled, cfg_.save_photos_to_sd, sd_.present());
  const bool queued = camera_.request(photo, cfg_.capture_delay_ms, save, false);
  Serial.printf("#CAPTURE name=%s delay_ms=%u save=%d queued=%d\n", photo, static_cast<unsigned>(cfg_.capture_delay_ms),
                static_cast<int>(save), static_cast<int>(queued));
}

void App::showLatestPhoto() {
  char name[kPhotoFieldLen] = {0};
  camera_.latestName(name, sizeof(name));
  if (name[0] == '\0') {
    Serial.println("#ERR photo show 直近の写真がありません");
    return;
  }
  const bool opened = ui_.openPhoto(name, millis());
  Serial.printf("%s photo show name=%s%s\n", opened ? "#OK" : "#ERR", name, opened ? "" : " (セッション中は開けません)");
}

void App::cmdCam(uint32_t now_ms) {
  Serial.printf("#CAM enabled=%d ready=%d init_ms=%lu busy_ms=%lu latest_bytes=%u delay_ms=%u init_error=%s\n",
                static_cast<int>(camera_.enabled()), static_cast<int>(camera_.ready()),
                static_cast<unsigned long>(camera_.initMs()), static_cast<unsigned long>(camera_.busyMs(now_ms)),
                static_cast<unsigned>(camera_.latestSize()), static_cast<unsigned>(cfg_.capture_delay_ms),
                camera_.initError());
}

void App::cmdPhotoTest(uint32_t now_ms) {
  if (!camera_.ready()) {
    Serial.printf("#ERR photo test カメラを使えません (%s)\n", camera_.initError());
    return;
  }
  char name[kPhotoFieldLen];
  std::snprintf(name, sizeof(name), "test-%lu.jpg", static_cast<unsigned long>(now_ms / 1000));
  const bool queued = camera_.request(name, cfg_.capture_delay_ms, false, true);
  Serial.printf("%s photo test name=%s delay_ms=%u (SD には保存しない)\n", queued ? "#OK" : "#ERR", name,
                static_cast<unsigned>(cfg_.capture_delay_ms));
}

// --- コマンド ---

void App::handleCommand(char* line) {
  Serial.printf("#CMD %s\n", line);
  // sim qr は空白を含むコードも渡せるように、分ける前に残りをそのまま取っておく
  char qr_raw[kQrReadMax + 1] = {0};
  const bool is_sim_qr = std::strncmp(line, "sim qr ", 7) == 0;
  if (is_sim_qr) std::snprintf(qr_raw, sizeof(qr_raw), "%s", line + 7);

  char* w[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  const size_t n = splitWords(line, w, 5);
  auto is = [&](size_t i, const char* s) { return i < n && i < 5 && std::strcmp(w[i], s) == 0; };
  const uint32_t now = millis();

  if (n == 1 && is(0, "info")) {
    cmdInfo();
  } else if (n == 1 && is(0, "about")) {
    cmdAbout();
  } else if (n == 1 && is(0, "report")) {
    printBootReport();
  } else if (n == 1 && is(0, "screenshot")) {
    SerialPort::sendScreenshot();
  } else if (n == 1 && is(0, "restart")) {
    Serial.println("#OK restart");
    SerialPort::drain(300);
    ESP.restart();
  } else if (n == 1 && is(0, "time")) {
    cmdTime();
  } else if (n == 3 && is(0, "time") && is(1, "set")) {
    cmdTimeSet(w[2]);
  } else if ((n == 3 || n == 4) && is(0, "nvs") && is(1, "erase")) {
    cmdNvsErase(w[2], n == 4 ? w[3] : "");
  } else if (n == 1 && is(0, "cam")) {
    cmdCam(now);
  } else if (n == 2 && is(0, "photo") && is(1, "test")) {
    cmdPhotoTest(now);
  } else if (n == 2 && is(0, "photo") && is(1, "show")) {
    showLatestPhoto();
  } else if (n == 1 && is(0, "sd")) {
    Serial.printf("#SD present=%d photos=%u save_setting=%d\n", static_cast<int>(sd_.present()),
                  static_cast<unsigned>(sd_.photoCount()), static_cast<int>(cfg_.save_photos_to_sd));
  } else if (n == 2 && is(0, "sd") && is(1, "prune")) {
    Serial.println(sd_.requestPrune() ? "#OK sd prune (結果は #SD pruned)" : "#ERR sd prune キューが一杯です");
  } else if (n == 2 && is(0, "sd") && is(1, "check")) {
    Serial.println(sd_.requestCheck() ? "#OK sd check" : "#ERR sd check キューが一杯です");
  } else if (n == 1 && is(0, "i2c")) {
    printI2cRoute("cmd");
  } else if (n == 2 && is(0, "i2c") && is(1, "rescan")) {
    const bool changed = bus_.rescan(now);
    printI2cRoute(changed ? "rescan_changed" : "rescan");
    if (changed) ui_.onStatusChanged();
  } else if (n >= 1 && is(0, "tof")) {
    cmdTof(w, n, now);
  } else if (!kv_.ready()) {
    Serial.println("#ERR 記録領域を読めません (使えるのは info / report / time / screenshot / restart / nvs erase all YES)");
  } else if (n == 2 && is(0, "dump")) {
    cmdDump(w[1]);
  } else if (n == 1 && is(0, "cfg")) {
    dumpCfg();
  } else if (n == 3 && is(0, "cfg") && is(1, "get")) {
    cmdCfgGet(w[2]);
  } else if (n == 4 && is(0, "cfg") && is(1, "set")) {
    cmdCfgSet(w[2], w[3]);
  } else if (n == 1 && is(0, "power")) {
    cmdPower();
  } else if (n == 1 && is(0, "session")) {
    printSession(now);
  } else if ((n == 3 || n == 4) && is(0, "sd") && is(1, "erase") && is(2, "photos")) {
    if (n == 4 && is(3, "YES")) {
      eraseSdPhotos();
    } else {
      Serial.println("#ERR 消すときは最後に YES を付けてください (sd erase photos YES)");
    }
  } else if (n >= 1 && is(0, "sim")) {
    cmdSim(w, n, is_sim_qr ? qr_raw : nullptr, now);
  } else {
    Serial.println("#ERR unknown command");
  }
}

void App::cmdSim(char** w, size_t n, const char* qr_raw, uint32_t now_ms) {
  if (!cfg_.dev_sim) {
    Serial.println("#ERR sim は開発フラグが OFF のときは使えません (cfg set dev_sim 1)");
    return;
  }
  auto is = [&](size_t i, const char* s) { return i < n && i < 5 && std::strcmp(w[i], s) == 0; };
  size_t index = 0;
  size_t touch_y = 0;
  if (n == 4 && is(1, "touch") && parseIndex(w[2], &index) && parseIndex(w[3], &touch_y) && index < 720 &&
      touch_y < 1280) {
    Serial.printf("#OK sim touch %u %u\n", static_cast<unsigned>(index), static_cast<unsigned>(touch_y));
    handleTouch(static_cast<int>(index), static_cast<int>(touch_y), now_ms);
  } else if (n == 4 && is(1, "hold") && parseIndex(w[2], &index) && parseIndex(w[3], &touch_y) && index < 720 &&
             touch_y < 1280) {
    Serial.printf("#OK sim hold %u %u\n", static_cast<unsigned>(index), static_cast<unsigned>(touch_y));
    last_activity_ms_ = now_ms;
    ui_.onHold(static_cast<int>(index), static_cast<int>(touch_y), now_ms);
  } else if (n == 5 && is(1, "press") && parseIndex(w[2], &index) && parseIndex(w[3], &touch_y) && index < 720 &&
             touch_y < 1280) {
    size_t held = 0;
    if (!parseIndex(w[4], &held)) held = 0;
    Serial.printf("#OK sim press %u %u %u\n", static_cast<unsigned>(index), static_cast<unsigned>(touch_y),
                  static_cast<unsigned>(held));
    ui_.onPressing(static_cast<int>(index), static_cast<int>(touch_y), static_cast<uint32_t>(held));
  } else if (n == 2 && is(1, "release")) {
    Serial.println("#OK sim release");
    ui_.onReleased();
  } else if (n == 3 && is(1, "mute") && (is(2, "on") || is(2, "off"))) {
    alarm_.setMuted(is(2, "on"));  // 保存しない (再起動で解除)
    Serial.printf("#OK sim mute %s\n", alarm_.muted() ? "on" : "off");
  } else if (n == 2 && is(1, "open")) {
    handleDrawer(true, "sim", now_ms);
  } else if (n == 2 && is(1, "close")) {
    handleDrawer(false, "sim", now_ms);
  } else if (qr_raw != nullptr) {
    handleCode(qr_raw, std::strlen(qr_raw), true, now_ms);
  } else if (n == 3 && is(1, "remove") && parseIndex(w[2], &index)) {
    const bool ok = session_.removeRow(index);
    Serial.printf("%s sim remove %u\n", ok ? "#OK" : "#ERR", static_cast<unsigned>(index));
    session_dirty_ = true;
    printSession(now_ms);
  } else if (n == 3 && is(1, "switch") && parseIndex(w[2], &index)) {
    const bool ok = session_.toggleSwitch(index, rtc_.now(now_ms));
    Serial.printf("%s sim switch %u\n", ok ? "#OK" : "#ERR", static_cast<unsigned>(index));
    session_dirty_ = true;
    printSession(now_ms);
  } else if (n == 2 && is(1, "cancel")) {
    cancelSession("sim", now_ms);
  } else if (n == 2 && is(1, "complete")) {
    // 画面の同じ場所のボタンと同じく、そのとき終えられる方を終える (返却だけ / 名札先。2026-09-16)
    if (session_.canCompleteTagWindow()) {
      completeTagWindow("sim", now_ms);
    } else {
      completeReturn("sim", now_ms);
    }
  } else if (n == 3 && is(1, "pause") && parseIndex(w[2], &index) && index >= 1 && index <= 120) {
    const SessionOutput out = session_.pause(static_cast<uint32_t>(index) * 60000, now_ms, rtc_.now(now_ms));
    handleSessionOutput(out, now_ms);
    session_dirty_ = true;
    printSession(now_ms);
  } else if (n == 2 && is(1, "resume")) {
    const SessionOutput out = session_.resume(now_ms, rtc_.now(now_ms));
    handleSessionOutput(out, now_ms);
    session_dirty_ = true;
    printSession(now_ms);
  } else {
    Serial.println("#ERR sim open|close|qr <コード>|remove <行>|switch <行>|cancel|complete|pause <分 1〜120>|resume|touch <x> <y>|hold <x> <y>|press <x> <y> <ms>|release|mute on|off");
  }
}

// about: このソフトについて (設定画面「このソフトについて」と同じ中身) と、画面に出すライセンスの節
void App::cmdAbout() {
  Serial.printf("#ABOUT product=%s version=%s release=%s developer=%s license=%s url=%s sections=%u\n",
                about::kProductName, about::kFirmwareVersion, about::kReleaseDate, about::kDeveloper,
                about::kLicenseName, about::kRepoUrl, static_cast<unsigned>(kLicenseSectionCount));
  for (size_t i = 0; i < kLicenseSectionCount; ++i) {
    Serial.printf("#LICENSE index=%u bytes=%u source=%s title=%s\n", static_cast<unsigned>(i),
                  static_cast<unsigned>(std::strlen(kLicenseSections[i].text)), kLicenseSections[i].source,
                  kLicenseSections[i].title);
  }
  for (size_t i = 0; i < kLicenseNoteLineCount; ++i) Serial.printf("#LICENSE note=%s\n", kLicenseNoteLines[i]);
}

void App::cmdInfo() {
  nvs_stats_t st;
  const bool have_stats = kv_.ready() && kv_.stats(&st);
  Serial.printf("#INFO fw=%s chip=%s rev=%d cpu=%lu heap=%lu psram=%lu uptime_s=%lu storage=%s nvs_used=%u "
                "nvs_free=%u nvs_total=%u namespaces=%u dev_sim=%d qr=%d qr_failed=%d qr_reads=%lu qr_errors=%lu "
                "qr_discards=%lu drawer=%s drawer_sensor=%d tof=%d loop_max_ms=%lu loop_slow=%lu\n",
                kFirmwareVersion, ESP.getChipModel(), static_cast<int>(ESP.getChipRevision()),
                static_cast<unsigned long>(getCpuFrequencyMhz()), static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getFreePsram()), static_cast<unsigned long>(millis() / 1000),
                kv_.ready() ? "ok" : "ng", have_stats ? static_cast<unsigned>(st.used_entries) : 0u,
                have_stats ? static_cast<unsigned>(st.free_entries) : 0u,
                have_stats ? static_cast<unsigned>(st.total_entries) : 0u,
                have_stats ? static_cast<unsigned>(st.namespace_count) : 0u, static_cast<int>(cfg_.dev_sim),
                static_cast<int>(qr_.present()), static_cast<int>(qr_.failed()),
                static_cast<unsigned long>(qr_.readCount()), static_cast<unsigned long>(qr_.errorCount()),
                static_cast<unsigned long>(qr_.discardCount()), drawer_open_ ? "open" : "closed",
                static_cast<int>(drawer_.enabled()), static_cast<int>(drawer_.present()),
                static_cast<unsigned long>(loop_max_ms_), static_cast<unsigned long>(loop_slow_));
  printI2cRoute("info");
  // 前回の info からの最大と回数 (出したら戻す)
  loop_max_ms_ = 0;
  loop_slow_ = 0;
}

// tof: 状態 / tof raw <秒>: 読むたびに距離を出す (各段の距離の CSV 化) / tof calibrate: 2 秒集めて基準距離を保存
void App::cmdTof(char** w, size_t n, uint32_t now_ms) {
  auto is = [&](size_t i, const char* s) { return i < n && i < 5 && std::strcmp(w[i], s) == 0; };
  size_t seconds = 0;
  if (n == 1) {
    Serial.printf("#TOF enabled=%d present=%d fault=%d calibrated=%d judging=%d state=%s mm=%u status=%u valid=%d baseline_mm=%u "
                  "open_delta_mm=%u close_delta_mm=%u period_ms=%u init_ms=%lu samples=%lu invalid=%lu starts=%lu "
                  "calibrating=%d\n",
                  static_cast<int>(drawer_.enabled()), static_cast<int>(drawer_.present()),
                  static_cast<int>(drawer_.fault()), static_cast<int>(drawer_.calibrated()),
                  static_cast<int>(drawer_.judging()), drawerStateName(drawer_.state()),
                  static_cast<unsigned>(drawer_.lastMm()), static_cast<unsigned>(drawer_.lastRangeStatus()),
                  static_cast<int>(drawer_.lastValid()), static_cast<unsigned>(cfg_.tof_baseline_mm),
                  static_cast<unsigned>(cfg_.open_delta_mm),
                  static_cast<unsigned>(cfg_.close_delta_mm), static_cast<unsigned>(drawer_.periodMs()),
                  static_cast<unsigned long>(drawer_.initMs()), static_cast<unsigned long>(drawer_.sampleCount()),
                  static_cast<unsigned long>(drawer_.invalidCount()), static_cast<unsigned long>(drawer_.startCount()),
                  static_cast<int>(calib_run_.active));
  } else if (n == 3 && is(1, "raw") && parseIndex(w[2], &seconds) && seconds >= 1 && seconds <= 600) {
    if (!drawer_.present()) {
      Serial.println("#ERR tof raw ToF を測っていません (引き出しセンサを使う設定で、Unit ToF が見つかっているか)");
      return;
    }
    tof_raw_active_ = true;
    tof_raw_until_ms_ = now_ms + static_cast<uint32_t>(seconds) * 1000;
    Serial.printf("#OK tof raw %u 秒 (読むたびに #TOF ms= mm= valid= state=)\n", static_cast<unsigned>(seconds));
  } else if (n == 2 && is(1, "calibrate")) {
    if (!kv_.ready()) {
      Serial.println("#ERR tof calibrate 記録領域を読めないので保存できません");
    } else if (!beginTofCalibration(true, now_ms)) {
      Serial.println("#ERR tof calibrate ToF を測っていないか、基準を取っている最中です");
    } else {
      Serial.println("#OK tof calibrate 2 秒集めます。全段を閉めて、前に立たないでください (結果は #TOF calibrate)");
    }
  } else {
    Serial.println("#ERR tof | tof raw <秒 1〜600> | tof calibrate");
  }
}

void App::cmdTime() {
  const uint32_t now = millis();
  const int64_t epoch = rtc_.now(now);
  char local[64];
  if (!formatLocalDateTime(epoch, cfg_.tz_offset_min, local, sizeof(local))) std::snprintf(local, sizeof(local), "-");
  Serial.printf("#TIME epoch=%lld local=%s tz_offset_min=%d rtc_ok=%d time_lost=%d vlf=%d vblf=%d\n",
                static_cast<long long>(epoch), local, static_cast<int>(cfg_.tz_offset_min),
                static_cast<int>(rtc_.ok()), static_cast<int>(rtc_.timeLost(now)),
                static_cast<int>((rtc_.flags() & kRtcFlagVlf) != 0), static_cast<int>((rtc_.flags() & kRtcFlagVblf) != 0));
}

void App::cmdTimeSet(const char* text) {
  int64_t epoch = 0;
  if (!parseLocalDateTime(text, cfg_.tz_offset_min, &epoch)) {
    Serial.println("#ERR time set の書式は YYYY-MM-DDTHH:MM か YYYY-MM-DDTHH:MM:SS (現地時刻)");
    return;
  }
  if (!isValidEpoch(epoch)) {
    Serial.println("#ERR 2026 年より前には合わせられません");
    return;
  }
  if (!rtc_.set(epoch, millis())) {
    Serial.println("#ERR RTC に書けませんでした");
    return;
  }
  Serial.print("#OK ");
  cmdTime();
  drawClockLine();
}

void App::cmdCfgGet(const char* name) {
  char buf[32];
  if (!formatConfigField(cfg_, name, buf, sizeof(buf))) {
    Serial.printf("#ERR cfg その名前の設定はありません: %s\n", name);
    return;
  }
  Serial.printf("#CFG %s=%s\n", name, buf);
}

void App::cmdCfgSet(const char* name, const char* value) {
  Config candidate = cfg_;
  const ConfigFieldResult r = setConfigField(&candidate, name, value);
  if (r == ConfigFieldResult::UnknownField) {
    Serial.printf("#ERR cfg その名前の設定はありません: %s\n", name);
    return;
  }
  if (r == ConfigFieldResult::BadValue) {
    Serial.printf("#ERR cfg 値を読めません: %s=%s\n", name, value);
    return;
  }
  ConfigError e = ConfigError::None;
  if (!saveConfig(kv_, candidate, &e)) {
    if (e != ConfigError::None) {
      Serial.printf("#ERR cfg %s\n", describeConfigError(e));
    } else {
      Serial.println("#ERR cfg 保存できませんでした");
    }
    return;
  }
  if (cfg_result_.status != ConfigLoadStatus::Loaded && cfg_result_.status != ConfigLoadStatus::NotFound) {
    Serial.println("[設定] 読めなかった設定を、既定値にこの変更を足した値で上書きしました");
  }
  if (std::strcmp(name, "camera_enabled") == 0 && candidate.camera_enabled != camera_.enabled()) {
    Serial.println("[設定] カメラの入切は再起動で反映します");
  }
  cfg_ = candidate;
  cfg_result_ = ConfigLoadResult{};
  cfg_result_.status = ConfigLoadStatus::Loaded;
  session_.setConfig(cfg_);
  power_.setConfig(cfg_);
  applyDrawerSetting();
  applyBrightness();
  drawStatusScreen();
  char buf[32];
  formatConfigField(cfg_, name, buf, sizeof(buf));
  Serial.printf("#OK cfg %s=%s\n", name, buf);
}

void App::cmdDump(const char* what) {
  const bool all = std::strcmp(what, "all") == 0;
  bool known = all;
  if (all || std::strcmp(what, "cfg") == 0) {
    dumpCfg();
    known = true;
  }
  if (all || std::strcmp(what, "loans") == 0) {
    dumpLoans();
    known = true;
  }
  if (all || std::strcmp(what, "hist") == 0) {
    dumpHist();
    known = true;
  }
  if (all || std::strcmp(what, "openlog") == 0) {
    dumpOpenLog();
    known = true;
  }
  if (all || std::strcmp(what, "alerts") == 0) {
    dumpAlerts();
    known = true;
  }
  if (!known) Serial.println("#ERR dump loans|hist|openlog|alerts|cfg|all");
}

// 各セクションの 1 行目は見出し (rows= には数えない)
void App::dumpCfg() {
  printSectionBegin("cfg");
  Serial.println("name,value");
  char row[96];
  char value[32];
  size_t rows = 0;
  for (size_t i = 0; i < configFieldCount(); ++i) {
    const char* name = configFieldName(i);
    if (!formatConfigField(cfg_, name, value, sizeof(value))) continue;
    CsvLine line(row, sizeof(row));
    line.addText(name).addText(value);
    if (printCsv(line)) ++rows;
  }
  printSectionEnd("cfg", rows);
}

void App::dumpLoans() {
  std::vector<Loan> loans(kMaxLoans);
  const size_t n = book_.listLoans(loans.data(), loans.size());
  printSectionBegin("loans");
  Serial.println("item,user,checkout,checkout_photo,order,user_unknown,switched");
  char row[256];
  size_t rows = 0;
  for (size_t i = 0; i < n; ++i) {
    const Loan& l = loans[i];
    CsvLine line(row, sizeof(row));
    line.addText(l.item)
        .addText(l.user)
        .addDateTime(l.checkout_epoch, cfg_.tz_offset_min)
        .addText(l.checkout_photo)
        .addInt(l.order)
        .addInt((l.flags & kLoanUserUnknown) ? 1 : 0)
        .addInt((l.flags & kLoanSwitched) ? 1 : 0);
    if (printCsv(line)) ++rows;
  }
  printSectionEnd("loans", rows);
}

void App::dumpHist() {
  printSectionBegin("hist");
  Serial.println("item,borrower,returner,checkout,return,checkout_photo,return_photo,proxy,returner_unknown,missed,cancelled");
  char row[320];
  size_t rows = 0;
  for (uint16_t i = 0; i < book_.returnCount(); ++i) {
    ReturnEntry r;
    if (!book_.getReturn(i, &r)) {
      Serial.printf("#WARN hist index=%u 壊れていて読めません\n", static_cast<unsigned>(i));
      continue;
    }
    CsvLine line(row, sizeof(row));
    line.addText(r.item)
        .addText(r.borrower)
        .addText(r.returner)
        .addDateTime(r.checkout_epoch, cfg_.tz_offset_min)
        .addDateTime(r.return_epoch, cfg_.tz_offset_min)
        .addText(r.checkout_photo)
        .addText(r.return_photo)
        .addInt((r.flags & kReturnProxy) ? 1 : 0)
        .addInt((r.flags & kReturnReturnerUnknown) ? 1 : 0)
        .addInt((r.flags & kReturnMissed) ? 1 : 0)
        .addInt((r.flags & kReturnCancelled) ? 1 : 0);
    if (printCsv(line)) ++rows;
  }
  printSectionEnd("hist", rows);
}

void App::dumpOpenLog() {
  printSectionBegin("openlog");
  Serial.println("kind,time,opened,gave_up,unregistered_open,unknown_checkout,user,photo,checkouts,returns,switches,failures,cancelled");
  char row[256];
  size_t rows = 0;
  for (uint16_t i = 0; i < log_.count(); ++i) {
    OpenLogEntry e;
    if (!log_.get(i, &e)) {
      Serial.printf("#WARN openlog index=%u 壊れていて読めません\n", static_cast<unsigned>(i));
      continue;
    }
    CsvLine line(row, sizeof(row));
    line.addText(logKindName(e.kind))
        .addDateTime(e.epoch, cfg_.tz_offset_min)
        .addInt(e.opened ? 1 : 0)
        .addInt(e.gave_up ? 1 : 0)
        .addInt(e.unregistered_open ? 1 : 0)
        .addInt(e.unknown_checkout ? 1 : 0)
        .addText(e.user)
        .addText(e.photo)
        .addInt(e.checkouts)
        .addInt(e.returns)
        .addInt(e.switches)
        .addInt(e.failures)
        .addInt(e.cancelled ? 1 : 0);  // 2026-09-15 追加 (取りやめ)
    if (printCsv(line)) ++rows;
  }
  printSectionEnd("openlog", rows);
}

void App::dumpAlerts() {
  printSectionBegin("alerts");
  Serial.println("seq,time,unregistered_open,unknown_checkout,photo");
  char row[160];
  size_t rows = 0;
  for (uint16_t i = 0; i < log_.alertCount(); ++i) {
    Alert a;
    if (!log_.getAlert(i, &a)) continue;
    CsvLine line(row, sizeof(row));
    line.addInt(a.seq)
        .addDateTime(a.epoch, cfg_.tz_offset_min)
        .addInt(a.unregistered_open ? 1 : 0)
        .addInt(a.unknown_checkout ? 1 : 0)
        .addText(a.photo);
    if (printCsv(line)) ++rows;
  }
  printSectionEnd("alerts", rows);
  Serial.printf("#ALERTS count=%u dropped=%u\n", static_cast<unsigned>(log_.alertCount()),
                static_cast<unsigned>(log_.droppedAlerts()));
}

void App::cmdNvsErase(const char* what, const char* confirm) {
  const bool records = std::strcmp(what, "records") == 0;
  const bool all = std::strcmp(what, "all") == 0;
  if (!records && !all) {
    Serial.println("#ERR nvs erase records YES / nvs erase all YES");
    return;
  }
  if (std::strcmp(confirm, "YES") != 0) {
    Serial.println("#ERR 消すときは最後に YES を付けてください (nvs erase records YES / nvs erase all YES)");
    return;
  }
  if (records) {
    if (!kv_.ready()) {
      Serial.println("#ERR 記録領域を読めません。消せるのは nvs erase all YES だけです");
      return;
    }
    eraseRecords();
    return;
  }
  const esp_err_t e = kv_.eraseAll();
  if (e != ESP_OK) {
    Serial.printf("#ERR nvs erase all err=%s\n", esp_err_to_name(e));
    return;
  }
  Serial.println("#OK nvs erase all (再起動します)");
  SerialPort::drain(300);
  ESP.restart();
}

}  // namespace toolcheck
