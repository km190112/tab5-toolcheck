#include "ui_controller.h"

#include <M5Unified.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_heap_caps.h"
#include "../core/status_marks.h"
#include "../core/time_format.h"
#include "../ui/jp_text.h"
#include "../ui/theme.h"

namespace toolcheck {
namespace {

using ui::TextSize;

constexpr uint32_t kHeaderRedrawMs = 1000;
constexpr uint32_t kListRefreshMs = 60000;  // 一覧の経過時間 (N分前) を書き直す間隔
constexpr uint32_t kEventShowMs = 5000;     // 一覧の下に直前の出来事を出しておく時間
constexpr uint32_t kBlinkMs = 500;          // 警告 1 の点滅
constexpr int kTabCount = 4;
const char* const kTabLabels[kTabCount] = {"貸出中", "返却履歴", "開閉ログ", "検索"};
constexpr int kTabW = 150;  // 4 つのタブ + 右端の「設定」(120px)
constexpr int kGearX = kTabW * kTabCount;
// 「記録領域を読めません」の全削除 (計画: 画面から PIN 無しで消せるのはここだけ)
constexpr int kStorageEraseY = 520;
constexpr int kStorageEraseH = 170;

// 赤帯の縮小写真 (撮った写真 720x1280 の 1/3)
constexpr int kThumbW = 240;
constexpr int kThumbH = 427;
constexpr float kThumbScale = 1.0f / 3.0f;
constexpr int kAlertPhotoBandH = kThumbH + 28;
// 写真の全面表示 (ヘッダの下 1200px に収める)
constexpr float kViewScale = 1200.0f / 1280.0f;
constexpr int kViewW = 675;
constexpr int kViewH = 1200;

// 検索 (上に入力欄、中に候補、下に英数キーボード)
constexpr int kSearchBoxY = 92;
constexpr int kSearchBoxH = 80;
constexpr int kSearchResultsY = kSearchBoxY + kSearchBoxH + 12;
constexpr int kSearchRowH = 92;
constexpr size_t kSearchShown = 5;
constexpr int kKeyRows = 5;
constexpr int kKeyH = 84;
constexpr int kKeyGap = 4;
constexpr int kKeyW = 66;
constexpr int kKeyX0 = 12;  // (720 - 10 × 66 - 9 × 4) / 2
constexpr int kKeyboardY = 1180 - kKeyRows * (kKeyH + kKeyGap) - 6;
const char* const kKeyLetters[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM_/."};
// いちばん下の行: 小文字切替 / # / 削除 / クリア
constexpr int kSpecialX[4] = {kKeyX0, kKeyX0 + 174, kKeyX0 + 268, kKeyX0 + 482};
constexpr int kSpecialW[4] = {170, 90, 210, 210};

// "9/13 21:32"。時刻が無効なら "--/-- --:--"
void formatShortDateTime(int64_t epoch, int32_t tz, char* buf, size_t len) {
  LocalDateTime t;
  if (!toLocalDateTime(epoch, tz, &t)) {
    std::snprintf(buf, len, "--/-- --:--");
    return;
  }
  std::snprintf(buf, len, "%d/%d %02d:%02d", t.month, t.day, t.hour, t.minute);
}

const char* rowKindText(RowKind k) {
  switch (k) {
    case RowKind::Checkout: return "持出";
    case RowKind::Return: return "返却";
    case RowKind::Switch: return "持出に切替";
  }
  return "";
}

const char* bookResultText(BookResult r) {
  switch (r) {
    case BookResult::Ok: return "";
    case BookResult::NotLent: return "貸出中ではありません";
    case BookResult::AlreadyLent: return "もう貸出中です";
    case BookResult::Full: return "貸出中が上限です";
    case BookResult::InvalidArg: return "番号が正しくありません";
    case BookResult::StorageError: return "保存できませんでした";
  }
  return "";
}

const char* openLogKindText(const OpenLogEntry& e) {
  switch (e.kind) {
    case OpenLogKind::Session: return e.opened ? "開放" : "QR だけ";
    case OpenLogKind::PausedOpen: return "一時停止中の開放";
    case OpenLogKind::PauseStarted: return "警告の一時停止を開始";
    case OpenLogKind::PauseEnded: return "警告の一時停止が終了";
    case OpenLogKind::RecordsCleared: return "記録を削除";
    case OpenLogKind::PhotosCleared: return "SD の写真を全部削除";
  }
  return "";
}

bool inRect(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

void drawButton(LovyanGFX& d, int x, int y, int w, int h, const char* label, TextSize size) {
  const int text_h = size == TextSize::Large ? 40 : (size == TextSize::Medium ? 32 : 28);
  d.fillRoundRect(x, y, w, h, 10, ui::kColorButton);
  ui::drawTextCenter(d, x + w / 2, y + (h - text_h) / 2, label, size, ui::kColorText, ui::kColorButton);
}

}  // namespace

UiController::UiController(LoanBook& book, OpenLog& log, Session& session, const Config& cfg, RtcClock& rtc,
                           QrReader& qr, CameraTask& camera, SdTask& sd, const DrawerSensor& drawer,
                           SettingsActions& settings_actions)
    : book_(book),
      log_(log),
      session_(session),
      cfg_(cfg),
      rtc_(rtc),
      qr_(qr),
      camera_(camera),
      sd_(sd),
      drawer_(drawer),
      actions_(settings_actions),
      screen_(0),
      settings_(settings_actions, book, log) {}

void UiController::begin(uint32_t now_ms, bool storage_ok, const char* storage_error) {
  storage_ok_ = storage_ok;
  std::snprintf(storage_error_, sizeof(storage_error_), "%s", storage_error != nullptr ? storage_error : "");
  screen_ = ScreenState(now_ms);
  started_ = true;
  if (!storage_ok_) {
    drawStorageError();
    return;
  }
  M5.Display.fillScreen(ui::kColorBg);
  drawHeader(now_ms);
  drawBody(now_ms);
}

void UiController::loop(uint32_t now_ms) {
  if (!started_ || !storage_ok_) return;
  screen_.setSessionActive(session_.state() != SessionState::Idle, now_ms);
  screen_.tick(now_ms);

  if (screen_.revision() != drawn_revision_ || screen_.view() != drawn_view_ || body_dirty_) {
    drawBody(now_ms);
  } else if (screen_.view() == View::Session) {
    if (session_dirty_) {
      drawSessionRows();
      drawSessionBottom(now_ms);
      session_dirty_ = false;
    }
    drawSessionTop(now_ms, false);
  } else if (screen_.view() == View::Tab) {
    const bool toast_now = event_text_[0] != '\0' && static_cast<int32_t>(event_until_ms_ - now_ms) > 0;
    if (now_ms - last_list_ms_ >= kListRefreshMs || toast_now != toast_shown_) drawBody(now_ms);
  } else if (screen_.view() == View::Settings) {
    if (settings_.needsRedraw(now_ms)) {
      drawBody(now_ms);
    } else if (settings_.needsLiveRedraw(now_ms)) {
      settings_.drawLive(now_ms);  // 「引き出しセンサ」の距離だけ (M6)
    }
  }
  if (header_dirty_ || now_ms - last_header_ms_ >= kHeaderRedrawMs) drawHeader(now_ms);
}

TouchResult UiController::onTouch(int x, int y, uint32_t now_ms) {
  if (!started_ || !storage_ok_) return TouchResult::None;
  screen_.onTouch(now_ms);
  switch (screen_.view()) {
    case View::UserLoans:
    case View::Photo:
      screen_.dismiss(now_ms);
      return TouchResult::Handled;
    case View::Session:
      return touchSession(x, y, now_ms);
    case View::Tab:
      return touchTab(x, y, now_ms);
    case View::Settings: {
      const SettingsResult r = settings_.onTouch(x, y, now_ms);
      if (r == SettingsResult::Close) {
        screen_.closeSettings(now_ms);
        return TouchResult::Handled;
      }
      return r == SettingsResult::Handled ? TouchResult::Handled : TouchResult::None;
    }
  }
  return TouchResult::None;
}

void UiController::onHold(int x, int y, uint32_t now_ms) {
  if (!started_ || !storage_ok_ || screen_.view() != View::Tab) return;
  if (y < ui::kTabBarY || x < kGearX) return;
  settings_.open(now_ms);
  screen_.openSettings(now_ms);
}

bool UiController::tofScreenOpen() const { return screen_.view() == View::Settings && settings_.tofScreenOpen(); }

bool UiController::settingsOpen() const { return screen_.view() == View::Settings; }

bool UiController::busy() const {
  if (!started_ || !storage_ok_) return false;
  const View v = screen_.view();
  return v == View::Settings || v == View::Photo || v == View::UserLoans ||
         (v == View::Tab && screen_.tab() == Tab::Search);
}

void UiController::onRecordsChanged() {
  body_dirty_ = true;
  header_dirty_ = true;
}

void UiController::onSessionChanged() { session_dirty_ = true; }

void UiController::onStatusChanged() {
  header_dirty_ = true;
  // SD が挿さった・抜けたら、「写真はありません」だった写真を読み直せるようにする
  if (photo_status_ == PhotoStatus::Missing) {
    releasePhoto();
    body_dirty_ = true;
  }
}

void UiController::onUserLoans(const char* user, uint32_t now_ms) { screen_.showUserLoans(user, now_ms); }

void UiController::setEventText(const char* text, uint32_t now_ms) {
  std::snprintf(event_text_, sizeof(event_text_), "%s", text != nullptr ? text : "");
  event_until_ms_ = now_ms + kEventShowMs;
  session_dirty_ = true;
}

void UiController::onPhotoRead(const SdEvent& ev) {
  const bool wanted = photo_status_ == PhotoStatus::Loading && std::strcmp(ev.name, photo_name_) == 0;
  if (wanted && ev.kind == SdEventKind::ReadDone && ev.data != nullptr) {
    photo_data_ = ev.data;
    photo_len_ = ev.bytes;
    photo_status_ = PhotoStatus::Ready;
    body_dirty_ = true;
    return;
  }
  if (ev.data != nullptr) heap_caps_free(ev.data);  // 頼み直した後に届いた古い写真
  if (wanted) {
    photo_status_ = PhotoStatus::Missing;
    body_dirty_ = true;
  }
}

void UiController::onPhotosErased(bool ok, uint16_t removed, uint16_t left) {
  settings_.onPhotosErased(ok, removed, left);
  // 消した写真の縮小や読み込んだ写真を持ち続けない (直近に撮った 1 枚は撮影タスクの RAM に残る)
  thumb_name_[0] = '\0';
  releasePhoto();
  body_dirty_ = true;
  header_dirty_ = true;
}

bool UiController::openPhoto(const char* name, uint32_t now_ms) {
  if (!started_ || !storage_ok_ || session_.state() != SessionState::Idle || name == nullptr || name[0] == '\0') {
    return false;
  }
  screen_.openPhoto(name, now_ms);
  return screen_.view() == View::Photo;
}

// --- 写真 ---

void UiController::requestPhoto(const char* name) {
  if (name == nullptr || name[0] == '\0') return;
  if (photo_status_ != PhotoStatus::None && std::strcmp(photo_name_, name) == 0) return;  // 読んだ・読んでいる・無い
  releasePhoto();
  std::snprintf(photo_name_, sizeof(photo_name_), "%s", name);
  // 直近に撮った 1 枚なら RAM から写す
  char latest[kPhotoFieldLen] = {0};
  camera_.latestName(latest, sizeof(latest));
  const size_t latest_size = camera_.latestSize();
  if (latest_size > 0 && std::strcmp(latest, name) == 0) {
    uint8_t* data = static_cast<uint8_t*>(heap_caps_malloc(latest_size, MALLOC_CAP_SPIRAM));
    char got[kPhotoFieldLen] = {0};
    const size_t n = data != nullptr ? camera_.copyLatest(data, latest_size, got, sizeof(got)) : 0;
    if (n > 0 && std::strcmp(got, name) == 0) {
      photo_data_ = data;
      photo_len_ = n;
      photo_status_ = PhotoStatus::Ready;
      return;
    }
    if (data != nullptr) heap_caps_free(data);
  }
  if (sd_.present() && sd_.requestRead(name)) {
    photo_status_ = PhotoStatus::Loading;
    return;
  }
  photo_status_ = PhotoStatus::Missing;
}

void UiController::releasePhoto() {
  if (photo_data_ != nullptr) heap_caps_free(photo_data_);
  photo_data_ = nullptr;
  photo_len_ = 0;
  photo_name_[0] = '\0';
  photo_status_ = PhotoStatus::None;
}

bool UiController::photoReady(const char* name) const {
  return photo_status_ == PhotoStatus::Ready && photo_data_ != nullptr && std::strcmp(photo_name_, name) == 0;
}

// 赤帯の縮小写真を作る。作れた (作ってある) なら true
bool UiController::ensureThumbnail(const char* name) {
  if (std::strcmp(thumb_name_, name) == 0) return true;
  requestPhoto(name);
  if (!photoReady(name)) return false;
  if (thumb_.width() == 0) {
    thumb_.setPsram(true);
    thumb_.setColorDepth(16);
    if (!thumb_.createSprite(kThumbW, kThumbH)) return false;
  }
  thumb_.fillSprite(ui::kColorPanel);
  const uint32_t t0 = millis();
  const bool ok = thumb_.drawJpg(photo_data_, static_cast<uint32_t>(photo_len_), 0, 0, kThumbW, kThumbH, 0, 0,
                                 kThumbScale, kThumbScale);
  Serial.printf("#UI thumb name=%s ok=%d ms=%lu\n", name, static_cast<int>(ok), static_cast<unsigned long>(millis() - t0));
  if (!ok) return false;
  std::snprintf(thumb_name_, sizeof(thumb_name_), "%s", name);
  return true;
}

void UiController::setRowPhoto(size_t index, const char* name) {
  if (index >= kMaxRowPhotos) return;
  std::snprintf(row_photos_[index], sizeof(row_photos_[index]), "%s", name != nullptr ? name : "");
}

// --- 描画 ---

void UiController::drawBody(uint32_t now_ms) {
  switch (screen_.view()) {
    case View::Tab:
      drawTabView(now_ms);
      break;
    case View::Session:
      drawSessionView(now_ms);
      break;
    case View::UserLoans:
      drawUserLoansView(now_ms);
      break;
    case View::Photo:
      drawPhotoView();
      break;
    case View::Settings:
      settings_.draw(now_ms);
      break;
  }
  drawn_revision_ = screen_.revision();  // 描く途中の setListSize で増えた分も含める
  drawn_view_ = screen_.view();
  body_dirty_ = false;
  session_dirty_ = false;
  last_list_ms_ = now_ms;
}

void UiController::drawHeader(uint32_t now_ms) {
  auto& d = M5.Display;
  char buf[64];
  const uint16_t bg = ui::kColorHeaderBg;
  d.startWrite();
  d.fillRect(0, 0, ui::kScreenW, ui::kHeaderH, bg);
  // 右端から カメラ● SD● QR● ToF● (緑 = 使える / 黄 = 未校正 / 赤 = 使うのに無い・故障)。出すかと色は core/status_marks。
  // 2026-09-15: 使わない設定の ● は灰色で残さず出さない (カメラ● はカメラ、SD● は写真を SD に保存、ToF● は引き出しセンサ)
  StatusMarkInputs in;
  in.camera_enabled = camera_.enabled();
  in.camera_ready = camera_.ready();
  in.save_photos_to_sd = cfg_.save_photos_to_sd;
  in.sd_present = sd_.present();
  in.qr_present = qr_.present();
  in.qr_failed = qr_.failed();
  in.drawer_enabled = drawer_.enabled();
  in.drawer_present = drawer_.present();
  in.drawer_fault = drawer_.fault();
  in.drawer_calibrated = drawer_.calibrated();
  const StatusMarks marks = statusMarks(in);
  int x = ui::kScreenW - ui::kMargin;
  const auto put = [&](const StatusMark& m, const char* label) {
    if (!m.shown) return;
    const uint16_t color = m.level == MarkLevel::Ok     ? ui::kColorOk
                           : m.level == MarkLevel::Warn ? ui::kColorWarn
                                                        : ui::kColorAlert;
    ui::drawTextRight(d, x, 24, label, TextSize::Small, color, bg);
    x -= ui::textWidth(d, label, TextSize::Small) + 12;
  };
  put(marks.camera, "カメラ●");
  put(marks.sd, "SD●");
  put(marks.qr, "QR●");
  put(marks.tof, "ToF●");
  // 左の見出しと時刻は、右の ● の手前 (x) まで。ToF● を足すと「物品持ち出し」の後ろに時刻が収まらず、
  // 実機で時刻が「9/14(月) 2」で切れた (2026-09-14 夕)。収まらないときは見出しを省いて時刻を左に寄せる
  buf[0] = '\0';
  uint16_t left_color = ui::kColorAccent;
  if (session_.isPaused()) {
    const uint32_t left_min = (session_.pauseRemainingMs(now_ms) + 59999) / 60000;
    std::snprintf(buf, sizeof(buf), "警告停止中 残り%u分", static_cast<unsigned>(left_min));
    left_color = ui::kColorWarn;
  } else if (rtc_.timeLost(now_ms)) {
    std::snprintf(buf, sizeof(buf), "時刻未設定");
    left_color = ui::kColorAlert;
  } else {
    static const char* const kWeekdays[7] = {"日", "月", "火", "水", "木", "金", "土"};
    LocalDateTime t;
    if (toLocalDateTime(rtc_.now(now_ms), cfg_.tz_offset_min, &t)) {
      std::snprintf(buf, sizeof(buf), "%d/%d(%s) %02d:%02d", t.month, t.day, kWeekdays[t.weekday], t.hour, t.minute);
    }
  }
  d.setClipRect(0, 0, x, ui::kHeaderH);  // それでも収まらない長さのときは重ねずに切る
  int status_x = 200;
  if (buf[0] != '\0' && status_x + ui::textWidth(d, buf, TextSize::Small) > x) {
    status_x = ui::kMargin;
  } else {
    ui::drawText(d, ui::kMargin, 24, "物品持ち出し", TextSize::Small, ui::kColorText, bg);
  }
  if (buf[0] != '\0') ui::drawText(d, status_x, 24, buf, TextSize::Small, left_color, bg);
  d.clearClipRect();
  d.endWrite();
  last_header_ms_ = now_ms;
  header_dirty_ = false;
}

void UiController::drawStorageError() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(ui::kColorAlertBg);
  ui::drawText(d, ui::kMargin, 120, "記録領域を読めません", TextSize::Large, ui::kColorText, ui::kColorAlertBg);
  ui::drawText(d, ui::kMargin, 200, storage_error_, TextSize::Small, ui::kColorWarn, ui::kColorAlertBg);
  ui::drawText(d, ui::kMargin, 280, "自動では消しません。", TextSize::Medium, ui::kColorText, ui::kColorAlertBg);
  ui::drawText(d, ui::kMargin, 330, "USB シリアルで控えを取ってから", TextSize::Medium, ui::kColorText,
               ui::kColorAlertBg);
  ui::drawText(d, ui::kMargin, 380, "nvs erase all YES で消せます", TextSize::Medium, ui::kColorText,
               ui::kColorAlertBg);
  d.fillRoundRect(ui::kMargin, kStorageEraseY, ui::kScreenW - ui::kMargin * 2, kStorageEraseH, 14, ui::kColorAlert);
  ui::drawTextCenter(d, ui::kScreenW / 2, kStorageEraseY + 36, "全削除", TextSize::Large, ui::kColorText,
                     ui::kColorAlert);
  ui::drawTextCenter(d, ui::kScreenW / 2, kStorageEraseY + 96, "記録と設定を消して再起動します。3 秒押し続ける",
                     TextSize::Small, ui::kColorText, ui::kColorAlert);
  d.endWrite();
}

void UiController::drawStorageHold(uint32_t held_ms) {
  auto& d = M5.Display;
  const int w_max = ui::kScreenW - ui::kMargin * 2 - 20;
  const uint32_t ms = held_ms < kEraseHoldMs ? held_ms : kEraseHoldMs;
  const int w = static_cast<int>(static_cast<uint64_t>(w_max) * ms / kEraseHoldMs);
  const int y = kStorageEraseY + kStorageEraseH - 26;
  d.startWrite();
  d.fillRect(ui::kMargin + 10, y, w_max, 16, ui::kColorAlert);
  if (w > 0) d.fillRect(ui::kMargin + 10, y, w, 16, ui::kColorWarn);
  d.endWrite();
}

void UiController::onPressing(int x, int y, uint32_t held_ms) {
  if (!started_) return;
  if (!storage_ok_) {
    const bool inside = inRect(x, y, ui::kMargin, kStorageEraseY, ui::kScreenW - ui::kMargin * 2, kStorageEraseH);
    const uint32_t ms = inside ? held_ms : 0;
    if (ms / 100 != storage_hold_ms_ / 100) drawStorageHold(ms);
    storage_hold_ms_ = ms;
    if (inside && !storage_hold_fired_ && held_ms >= kEraseHoldMs) {
      storage_hold_fired_ = true;
      actions_.eraseAll();  // 消せたら再起動して戻ってこない
    }
    return;
  }
  if (screen_.view() == View::Settings) settings_.onPressing(x, y, held_ms);
}

void UiController::onReleased() {
  if (!started_) return;
  if (!storage_ok_) {
    if (storage_hold_ms_ > 0) drawStorageHold(0);
    storage_hold_ms_ = 0;
    storage_hold_fired_ = false;
    return;
  }
  settings_.onReleased();
}

void UiController::drawTabView(uint32_t now_ms) {
  auto& d = M5.Display;
  char buf[96];
  row_count_ = 0;
  band_h_ = 0;
  band_photo_[0] = '\0';
  d.startWrite();
  d.fillRect(0, ui::kHeaderH, ui::kScreenW, ui::kTabBarY - ui::kHeaderH, ui::kColorBg);
  if (screen_.tab() != Tab::Search) search_query_[0] = '\0';  // ほかのタブに移ったら入力を消す
  int top = ui::kHeaderH;
  if (log_.alertCount() > 0 && screen_.tab() != Tab::Search) top = drawAlertBand(top);  // 検索はキーボードの場所が要る
  const int list_top = top + ui::kListTitleH;
  const uint16_t rows = static_cast<uint16_t>((ui::kTabBarY - list_top) / ui::kRowH);
  list_top_ = list_top;
  switch (screen_.tab()) {
    case Tab::Loans:
      screen_.setListSize(book_.loanCount(), rows);
      std::snprintf(buf, sizeof(buf), "貸出中 %u 件 (経過の長い順)", static_cast<unsigned>(book_.loanCount()));
      ui::drawText(d, ui::kMargin, top + 12, buf, TextSize::Medium, ui::kColorText, ui::kColorBg);
      drawLoansRows(list_top, rows, now_ms);
      break;
    case Tab::History:
      screen_.setListSize(book_.returnCount(), rows);
      std::snprintf(buf, sizeof(buf), "返却履歴 %u 件 (新しい順)", static_cast<unsigned>(book_.returnCount()));
      ui::drawText(d, ui::kMargin, top + 12, buf, TextSize::Medium, ui::kColorText, ui::kColorBg);
      drawHistoryRows(list_top, rows);
      break;
    case Tab::OpenLog:
      screen_.setListSize(log_.count(), rows);
      std::snprintf(buf, sizeof(buf), "開閉ログ %u 件 (新しい順)", static_cast<unsigned>(log_.count()));
      ui::drawText(d, ui::kMargin, top + 12, buf, TextSize::Medium, ui::kColorText, ui::kColorBg);
      drawOpenLogRows(list_top, rows);
      break;
    case Tab::Search:
      screen_.setListSize(0, rows);
      drawSearchTab(now_ms);
      break;
  }
  drawPager(list_top);
  if (screen_.tab() == Tab::Search) {
    // 直前の出来事の帯はキーボードに重なるので出さない (出したことにして、消えるまでの描き直しを繰り返さない)
    toast_shown_ = event_text_[0] != '\0' && static_cast<int32_t>(event_until_ms_ - now_ms) > 0;
  } else {
    drawToast(now_ms);
  }
  drawTabBar();
  d.endWrite();
}

int UiController::drawAlertBand(int top) {
  auto& d = M5.Display;
  char buf[96];
  Alert a;
  const bool have = log_.getAlert(0, &a);
  const bool with_photo = have && a.photo[0] != '\0';
  const int h = with_photo ? kAlertPhotoBandH : ui::kAlertBandH;
  band_top_ = top;
  band_h_ = h;
  d.fillRect(0, top, ui::kScreenW, h, ui::kColorAlertBg);
  const unsigned others = static_cast<unsigned>(log_.alertCount() > 0 ? log_.alertCount() - 1 : 0) +
                          static_cast<unsigned>(log_.droppedAlerts());
  if (have) formatShortDateTime(a.epoch, cfg_.tz_offset_min, buf, sizeof(buf));
  if (!with_photo) {
    if (have) {
      ui::drawText(d, ui::kMargin, top + 14,
                   a.unregistered_open ? "登録せずに持ち出されました" : "利用者不明の持出があります", TextSize::Large,
                   ui::kColorText, ui::kColorAlertBg);
      ui::drawText(d, ui::kMargin, top + 72, buf, TextSize::Medium, ui::kColorWarn, ui::kColorAlertBg);
    }
    if (others > 0) {
      std::snprintf(buf, sizeof(buf), "他 %u 件", others);
      ui::drawTextRight(d, ui::kScreenW - ui::kMargin, top + 72, buf, TextSize::Medium, ui::kColorText,
                        ui::kColorAlertBg);
    }
    return top + h;
  }
  std::snprintf(band_photo_, sizeof(band_photo_), "%s", a.photo);
  const int px = ui::kMargin;
  const int py = top + 14;
  if (ensureThumbnail(a.photo)) {
    thumb_.pushSprite(&d, px, py);
  } else {
    d.fillRect(px, py, kThumbW, kThumbH, ui::kColorPanel);
    const bool loading = photo_status_ == PhotoStatus::Loading && std::strcmp(photo_name_, a.photo) == 0;
    ui::drawTextCenter(d, px + kThumbW / 2, py + kThumbH / 2 - 14, loading ? "読み込み中" : "写真はありません",
                       TextSize::Small, ui::kColorDim, ui::kColorPanel);
  }
  const int tx = px + kThumbW + 24;
  ui::drawText(d, tx, top + 24, a.unregistered_open ? "登録せずに" : "利用者不明の", TextSize::Large, ui::kColorText,
               ui::kColorAlertBg);
  ui::drawText(d, tx, top + 74, a.unregistered_open ? "持ち出されました" : "持出があります", TextSize::Large,
               ui::kColorText, ui::kColorAlertBg);
  ui::drawText(d, tx, top + 150, buf, TextSize::Medium, ui::kColorWarn, ui::kColorAlertBg);
  if (others > 0) {
    char more[32];
    std::snprintf(more, sizeof(more), "他 %u 件", others);
    ui::drawText(d, tx, top + 210, more, TextSize::Medium, ui::kColorText, ui::kColorAlertBg);
  }
  ui::drawText(d, tx, top + h - 56, "タップで大きく表示", TextSize::Small, ui::kColorText, ui::kColorAlertBg);
  return top + h;
}

void UiController::drawLoansRows(int list_top, uint16_t rows, uint32_t now_ms) {
  auto& d = M5.Display;
  std::vector<Loan> loans(kMaxLoans);
  const size_t n = book_.listLoans(loans.data(), loans.size());
  const int64_t now_epoch = rtc_.now(now_ms);
  const size_t start = static_cast<size_t>(screen_.page()) * rows;
  const int right = ui::kScreenW - ui::kPagerW - ui::kMargin;
  char buf[64];
  for (size_t i = 0; i < rows && start + i < n; ++i) {
    const Loan& l = loans[start + i];
    const int y = list_top + static_cast<int>(i) * ui::kRowH;
    setRowPhoto(i, l.checkout_photo);
    row_count_ = i + 1;
    const bool previous_day = isPreviousDay(now_epoch, l.checkout_epoch, cfg_.tz_offset_min);
    const uint16_t color = previous_day ? ui::kColorAlert : ui::kColorText;
    formatElapsed(now_epoch, l.checkout_epoch, buf, sizeof(buf));
    ui::drawTextRight(d, right, y + 12, buf, TextSize::Medium, color, ui::kColorBg);
    // 物品番号は経過時間の手前まで。入らなければ縮める (15 文字の W が「120時間前」に重なった。2026-09-15 実機)
    const int item_w = right - ui::textWidth(d, buf, TextSize::Medium) - 16 - ui::kMargin;
    ui::drawTextFit(d, ui::kMargin, y + 8, item_w, l.item, TextSize::Large, color, ui::kColorBg);
    const bool unknown = (l.flags & kLoanUserUnknown) != 0 || l.user[0] == '\0';
    ui::drawText(d, ui::kMargin, y + 62, unknown ? "利用者不明" : l.user, TextSize::Small,
                 unknown ? ui::kColorWarn : ui::kColorAccent, ui::kColorBg);
    char when[16];
    formatClock(l.checkout_epoch, cfg_.tz_offset_min, when, sizeof(when));
    std::snprintf(buf, sizeof(buf), "%s%s 持出%s", previous_day ? "前日 " : "", when, l.checkout_photo[0] ? " 写真" : "");
    ui::drawTextRight(d, right, y + 62, buf, TextSize::Small, previous_day ? ui::kColorAlert : ui::kColorDim,
                      ui::kColorBg);
    d.drawFastHLine(ui::kMargin, y + ui::kRowH - 2, right - ui::kMargin, ui::kColorPanel);
  }
  if (n == 0) {
    ui::drawText(d, ui::kMargin, list_top + 20, "貸出中の物品はありません", TextSize::Medium, ui::kColorDim,
                 ui::kColorBg);
  }
}

void UiController::drawHistoryRows(int list_top, uint16_t rows) {
  auto& d = M5.Display;
  const int right = ui::kScreenW - ui::kPagerW - ui::kMargin;
  const uint16_t start = static_cast<uint16_t>(screen_.page() * rows);
  char buf[96];
  for (uint16_t i = 0; i < rows && start + i < book_.returnCount(); ++i) {
    const int y = list_top + i * ui::kRowH;
    row_count_ = static_cast<size_t>(i) + 1;
    ReturnEntry r;
    if (!book_.getReturn(static_cast<uint16_t>(start + i), &r)) {
      setRowPhoto(i, "");
      ui::drawText(d, ui::kMargin, y + 30, "(壊れていて読めません)", TextSize::Small, ui::kColorWarn, ui::kColorBg);
      continue;
    }
    setRowPhoto(i, r.return_photo[0] ? r.return_photo : r.checkout_photo);
    // 1 行目: 物品番号 (返却の日時の手前まで。入らなければ縮める) と日時
    formatShortDateTime(r.return_epoch, cfg_.tz_offset_min, buf, sizeof(buf));
    ui::drawTextRight(d, right, y + 16, buf, TextSize::Small, ui::kColorDim, ui::kColorBg);
    const int item_w = right - ui::textWidth(d, buf, TextSize::Small) - 16 - ui::kMargin;
    ui::drawTextFit(d, ui::kMargin, y + 8, item_w, r.item, TextSize::Large, ui::kColorText, ui::kColorBg);
    // 2 行目: 借りた人 (左) と印 (右)。印は物品番号の後ろに置いていたが、15 文字だと日時に重なって
    // 画面の外へ出た (2026-09-15 実機)。
    // 2026-09-15: 返却は名札を読まず、返した人は記録しない。前の記録の返した人・代理・返却者不明の印も出さない
    char marks[64];
    std::snprintf(marks, sizeof(marks), "%s%s", (r.flags & kReturnMissed) ? "返却漏れ " : "",
                  (r.flags & kReturnCancelled) ? "取消 " : "");
    size_t marks_len = std::strlen(marks);
    while (marks_len > 0 && marks[marks_len - 1] == ' ') marks[--marks_len] = '\0';
    int marks_w = 0;
    if (marks_len > 0) {
      ui::drawTextRight(d, right, y + 62, marks, TextSize::Small, ui::kColorWarn, ui::kColorBg);
      marks_w = ui::textWidth(d, marks, TextSize::Small) + 16;
    }
    std::snprintf(buf, sizeof(buf), "借 %s%s", r.borrower[0] ? r.borrower : "不明",
                  (r.checkout_photo[0] || r.return_photo[0]) ? "  写真" : "");
    d.setClipRect(0, y + 56, right - marks_w, 40);  // 印とページ送りの手前で切る
    ui::drawText(d, ui::kMargin, y + 62, buf, TextSize::Small, ui::kColorAccent, ui::kColorBg);
    d.clearClipRect();
    d.drawFastHLine(ui::kMargin, y + ui::kRowH - 2, right - ui::kMargin, ui::kColorPanel);
  }
  if (book_.returnCount() == 0) {
    ui::drawText(d, ui::kMargin, list_top + 20, "返却の記録はありません", TextSize::Medium, ui::kColorDim,
                 ui::kColorBg);
  }
}

void UiController::drawOpenLogRows(int list_top, uint16_t rows) {
  auto& d = M5.Display;
  const int right = ui::kScreenW - ui::kPagerW - ui::kMargin;
  const uint16_t start = static_cast<uint16_t>(screen_.page() * rows);
  char buf[96];
  for (uint16_t i = 0; i < rows && start + i < log_.count(); ++i) {
    const int y = list_top + i * ui::kRowH;
    row_count_ = static_cast<size_t>(i) + 1;
    OpenLogEntry e;
    if (!log_.get(static_cast<uint16_t>(start + i), &e)) {
      setRowPhoto(i, "");
      ui::drawText(d, ui::kMargin, y + 30, "(壊れていて読めません)", TextSize::Small, ui::kColorWarn, ui::kColorBg);
      continue;
    }
    setRowPhoto(i, e.photo);
    formatShortDateTime(e.epoch, cfg_.tz_offset_min, buf, sizeof(buf));
    ui::drawText(d, ui::kMargin, y + 10, buf, TextSize::Medium, ui::kColorText, ui::kColorBg);
    const int kind_x = ui::kMargin + ui::textWidth(d, buf, TextSize::Medium) + 16;
    const bool alert = e.unregistered_open || e.unknown_checkout;
    ui::drawText(d, kind_x, y + 12, openLogKindText(e), TextSize::Small, alert ? ui::kColorAlert : ui::kColorAccent,
                 ui::kColorBg);
    // 2 行目: 0 件の数は省き、印と「写真」を続けて書く。長いときはページ送りの手前で切る
    // (最初の版は「持出 0 / 返却 0 / 切替 0 打ち切り 無登録」が右端の「写真」に重なった)
    char line2[128];
    size_t used = 0;
    const auto append = [&](const char* text) {
      if (used < sizeof(line2)) used += static_cast<size_t>(std::snprintf(line2 + used, sizeof(line2) - used, "%s", text));
    };
    line2[0] = '\0';
    if (e.kind == OpenLogKind::Session) {
      // 利用者が空: 打ち切りなら「不明」。返却だけ・取りやめ (名札を読まずに終えた。2026-09-15) なら出さない
      if (e.user[0] != '\0') {
        ui::drawTextRight(d, right, y + 12, e.user, TextSize::Small, ui::kColorText, ui::kColorBg);
      } else if (e.gave_up) {
        ui::drawTextRight(d, right, y + 12, "不明", TextSize::Small, ui::kColorWarn, ui::kColorBg);
      }
      char count[24];
      if (e.checkouts > 0) {
        std::snprintf(count, sizeof(count), "持出 %u  ", static_cast<unsigned>(e.checkouts));
        append(count);
      }
      if (e.returns > 0) {
        std::snprintf(count, sizeof(count), "返却 %u  ", static_cast<unsigned>(e.returns));
        append(count);
      }
      if (e.switches > 0) {
        std::snprintf(count, sizeof(count), "切替 %u  ", static_cast<unsigned>(e.switches));
        append(count);
      }
      if (e.checkouts == 0 && e.returns == 0 && e.switches == 0) append("物品の読み取りなし  ");
      if (e.cancelled) append("取りやめ ");
      if (e.gave_up) append("打ち切り ");
      if (e.unregistered_open) append("無登録 ");
      if (e.unknown_checkout) append("利用者不明 ");
      if (e.failures > 0) append("失敗あり ");
    }
    if (e.photo[0] != '\0') append(" 写真");
    if (line2[0] != '\0') {
      d.setClipRect(0, y + 56, right, 40);
      ui::drawText(d, ui::kMargin, y + 62, line2, TextSize::Small, alert ? ui::kColorWarn : ui::kColorDim, ui::kColorBg);
      d.clearClipRect();
    }
    d.drawFastHLine(ui::kMargin, y + ui::kRowH - 2, right - ui::kMargin, ui::kColorPanel);
  }
  if (log_.count() == 0) {
    ui::drawText(d, ui::kMargin, list_top + 20, "開閉の記録はありません", TextSize::Medium, ui::kColorDim,
                 ui::kColorBg);
  }
}

void UiController::drawPager(int list_top) {
  if (screen_.pageCount() <= 1) return;
  auto& d = M5.Display;
  char buf[24];
  const int x = ui::kScreenW - ui::kPagerW + 10;
  const int w = ui::kPagerW - 20;
  const int bottom = ui::kTabBarY - 10;
  const int button_h = (bottom - list_top) >= 400 ? 160 : 110;  // 赤帯の写真があると一覧が短い
  const bool can_prev = screen_.page() > 0;
  const bool can_next = screen_.page() + 1 < screen_.pageCount();
  d.fillRoundRect(x, list_top, w, button_h, 12, can_prev ? ui::kColorButton : ui::kColorPanel);
  ui::drawTextCenter(d, x + w / 2, list_top + button_h / 2 - 20, "▲", TextSize::Large,
                     can_prev ? ui::kColorText : ui::kColorDim, can_prev ? ui::kColorButton : ui::kColorPanel);
  d.fillRoundRect(x, bottom - button_h, w, button_h, 12, can_next ? ui::kColorButton : ui::kColorPanel);
  ui::drawTextCenter(d, x + w / 2, bottom - button_h / 2 - 20, "▼", TextSize::Large,
                     can_next ? ui::kColorText : ui::kColorDim, can_next ? ui::kColorButton : ui::kColorPanel);
  std::snprintf(buf, sizeof(buf), "%u/%u", static_cast<unsigned>(screen_.page() + 1),
                static_cast<unsigned>(screen_.pageCount()));
  ui::drawTextCenter(d, x + w / 2, (list_top + bottom) / 2 - 14, buf, TextSize::Small, ui::kColorText, ui::kColorBg);
}

void UiController::drawTabBar() {
  auto& d = M5.Display;
  const int w = kTabW;
  d.fillRect(0, ui::kTabBarY, ui::kScreenW, ui::kTabBarH, ui::kColorBg);
  for (int i = 0; i < kTabCount; ++i) {
    const bool active = static_cast<int>(screen_.tab()) == i;
    const uint16_t bg = active ? ui::kColorTabActive : ui::kColorPanel;
    d.fillRoundRect(i * w + 4, ui::kTabBarY + 6, w - 8, ui::kTabBarH - 12, 12, bg);
    ui::drawTextCenter(d, i * w + w / 2, ui::kTabBarY + 34, kTabLabels[i], TextSize::Small, ui::kColorText, bg);
  }
  // 「設定」は長押しで開く (触っただけで開かないように)
  const int gear_w = ui::kScreenW - kGearX;
  d.fillRoundRect(kGearX + 4, ui::kTabBarY + 6, gear_w - 8, ui::kTabBarH - 12, 12, ui::kColorButton);
  ui::drawTextCenter(d, kGearX + gear_w / 2, ui::kTabBarY + 20, "設定", TextSize::Small, ui::kColorText, ui::kColorButton);
  ui::drawTextCenter(d, kGearX + gear_w / 2, ui::kTabBarY + 54, "長押し", TextSize::Small, ui::kColorDim,
                     ui::kColorButton);
}

void UiController::drawToast(uint32_t now_ms) {
  toast_shown_ = event_text_[0] != '\0' && static_cast<int32_t>(event_until_ms_ - now_ms) > 0;
  if (!toast_shown_) return;
  auto& d = M5.Display;
  const int y = ui::kTabBarY - ui::kToastH;
  d.fillRect(0, y, ui::kScreenW, ui::kToastH, ui::kColorGuideBg);
  // 長い知らせは右端で切る (「… 切替 1 を記録しました」が画面の外へ出た。2026-09-15 実機)
  ui::drawTextFit(d, ui::kMargin, y + 14, ui::kScreenW - ui::kMargin * 2, event_text_, TextSize::Small, ui::kColorWarn,
                  ui::kColorGuideBg);
}

void UiController::drawSessionView(uint32_t now_ms) {
  auto& d = M5.Display;
  d.startWrite();
  d.fillRect(0, ui::kHeaderH, ui::kScreenW, ui::kScreenH - ui::kHeaderH, ui::kColorBg);
  d.endWrite();
  drawSessionTop(now_ms, true);
  drawSessionRows();
  drawSessionBottom(now_ms);
}

void UiController::drawSessionTop(uint32_t now_ms, bool force) {
  const SessionState state = session_.state();
  const uint8_t level = session_.alarmLevel(now_ms);
  const bool blink_on = level == 1 && (now_ms / kBlinkMs) % 2 == 0;
  const uint32_t since = session_.sinceProgressMs(now_ms);
  const uint32_t w1 = static_cast<uint32_t>(cfg_.warning1_sec) * 1000;
  const uint32_t w2 = static_cast<uint32_t>(cfg_.warning2_sec) * 1000;
  const uint32_t giveup = w2 + static_cast<uint32_t>(cfg_.giveup_sec) * 1000;
  uint32_t goal = w1;
  uint32_t from = 0;
  if (since >= w2) {
    from = w2;
    goal = giveup;
  } else if (since >= w1) {
    from = w1;
    goal = w2;
  }
  const uint32_t left_s = goal > since ? (goal - since + 999) / 1000 : 0;
  // 名札待ちの行が無い (返却だけの) セッションは警告を数えない (2026-09-15)
  const bool counting = session_.isCounting() && !session_.isPaused();
  const bool can_cancel = session_.canCancel();
  const bool can_complete = session_.canCompleteReturn();
  const bool can_complete_tag = session_.canCompleteTagWindow();
  const int bar_w = ui::kSessionBarW;
  const int filled =
      (counting && goal > from) ? static_cast<int>((since - from) * static_cast<uint64_t>(bar_w) / (goal - from)) : 0;
  // 見た目が変わるときだけ描き直す (状態・段・点滅・一時停止・行があるか・数えているか・取りやめ・残り秒・バーの長さ)
  const uint32_t key = (static_cast<uint32_t>(state) << 28) ^ (static_cast<uint32_t>(level) << 26) ^
                       (static_cast<uint32_t>(blink_on) << 25) ^ (static_cast<uint32_t>(session_.isPaused()) << 24) ^
                       (static_cast<uint32_t>(session_.rowCount() > 0) << 23) ^ (static_cast<uint32_t>(counting) << 30) ^
                       (static_cast<uint32_t>(can_cancel) << 31) ^ (static_cast<uint32_t>(can_complete) << 21) ^
                       (static_cast<uint32_t>(can_complete_tag) << 20) ^ (left_s << 10) ^ static_cast<uint32_t>(filled);
  if (!force && key == session_top_key_) return;
  session_top_key_ = key;

  auto& d = M5.Display;
  char buf[96];
  uint16_t bg = ui::kColorGuideBg;
  if (state == SessionState::TagWindow) bg = ui::kColorTagBg;
  if (level == 2) bg = ui::kColorAlert;
  if (level == 1) bg = blink_on ? ui::kColorAlert : ui::kColorAlertBg;
  d.startWrite();
  d.fillRect(0, ui::kSessionTopY, ui::kScreenW, ui::kSessionGuideH, bg);
  if (state == SessionState::TagWindow) {
    std::snprintf(buf, sizeof(buf), "%s さん: 続けて物品を読めます", session_.tagUser());
    // 英字の利用者 ID は 15 文字まであるので、入らなければ縮める (2026-09-15)
    ui::drawTextFit(d, ui::kMargin, ui::kSessionTopY + 34, ui::kScreenW - ui::kMargin * 2, buf, TextSize::Large,
                    ui::kColorText, bg);
  } else {
    const char* guide = session_.rowCount() == 0 ? "QR を読ませてください"
                        : session_.isCounting()  ? "最後に名札を読ませてください"
                                                 : "返却しました";
    ui::drawText(d, ui::kMargin, ui::kSessionTopY + 34, guide, TextSize::Large, ui::kColorText, bg);
  }
  const int y = ui::kSessionTopY + ui::kSessionGuideH;
  d.fillRect(0, y, ui::kScreenW, ui::kSessionRowsY - y, ui::kColorBg);
  if (session_.isPaused()) {
    ui::drawText(d, ui::kMargin, y + 14, "警告の一時停止中 (音と打ち切りは止めています)", TextSize::Small,
                 ui::kColorWarn, ui::kColorBg);
  } else if (state == SessionState::TagWindow) {
    // 「完了」ボタンはこの行より下 (y + kSessionCancelDy) から始まるので、幅は画面いっぱいに使う
    // (ボタンの左端までに縮めると Small でも入らず「同じ人で記」で切れた。2026-09-16 実機)
    ui::drawText(d, ui::kMargin, y + 14, "名札を読んでから 20 秒は、同じ人で記録します", TextSize::Small,
                 ui::kColorAccent, ui::kColorBg);
  } else if (counting) {
    if (since < w1) {
      std::snprintf(buf, sizeof(buf), "警告まで %u 秒", static_cast<unsigned>(left_s));
    } else if (since < w2) {
      std::snprintf(buf, sizeof(buf), "警告 1  強い警告まで %u 秒", static_cast<unsigned>(left_s));
    } else {
      std::snprintf(buf, sizeof(buf), "警告 2  打ち切りまで %u 秒", static_cast<unsigned>(left_s));
    }
    ui::drawText(d, ui::kMargin, y + 14, buf, TextSize::Medium, level > 0 ? ui::kColorAlert : ui::kColorText,
                 ui::kColorBg);
    d.drawRect(ui::kMargin, y + 64, bar_w, 36, ui::kColorDim);
    d.fillRect(ui::kMargin + 2, y + 66, filled > 4 ? filled - 4 : 0, 32, level > 0 ? ui::kColorAlert : ui::kColorWarn);
  } else if (state == SessionState::Collecting) {
    ui::drawText(d, ui::kMargin, y + 14, "返却は記録しました (20 秒で閉じます)", TextSize::Small, ui::kColorAccent,
                 ui::kColorBg);
  }
  if (state == SessionState::Collecting) {
    const char* hint = !session_.isCounting()  ? "持って行く物があれば続けて読んでください"
                       : session_.hasOpened() ? "持って行く物・返す物の QR を読ませてください"
                                              : "続けて物品か名札の QR を読ませてください";
    ui::drawText(d, ui::kMargin, y + 120, hint, TextSize::Small, ui::kColorDim, ui::kColorBg);
  }
  // 取りやめ (2026-09-15: 持って行くのをやめるとき、セッションごと 1 回押しで)。警告の秒の右、バーより上
  if (can_cancel) {
    drawButton(d, ui::kSessionCancelX, y + ui::kSessionCancelDy, ui::kSessionCancelW, ui::kSessionButtonH, "取りやめ",
               TextSize::Medium);
  } else if (can_complete) {
    // 返却完了 (2026-09-16: 返却だけのセッションを 20 秒待たずに終えて、すぐ貸し出せるように)。取りやめと同じ場所
    drawButton(d, ui::kSessionCancelX, y + ui::kSessionCancelDy, ui::kSessionCancelW, ui::kSessionButtonH, "返却完了",
               TextSize::Medium);
  } else if (can_complete_tag) {
    // 完了 (2026-09-16:「続けて物品を読めます」の画面でも、20 秒を待たずにすぐ次の人を登録したい)。同じ場所
    drawButton(d, ui::kSessionCancelX, y + ui::kSessionCancelDy, ui::kSessionCancelW, ui::kSessionButtonH, "完了",
               TextSize::Medium);
  }
  d.endWrite();
}

void UiController::drawSessionRows() {
  auto& d = M5.Display;
  char buf[64];
  const size_t rows = session_.rowCount();
  const int bottom = ui::kSessionRowsY + ui::kSessionRowsShown * ui::kSessionRowH;
  d.startWrite();
  d.fillRect(0, ui::kSessionRowsY, ui::kScreenW, bottom - ui::kSessionRowsY, ui::kColorBg);
  for (size_t i = 0; i < rows && i < static_cast<size_t>(ui::kSessionRowsShown); ++i) {
    SessionRow row;
    if (!session_.getRow(i, &row)) continue;
    const int y = ui::kSessionRowsY + static_cast<int>(i) * ui::kSessionRowH;
    d.fillRoundRect(ui::kMargin / 2, y + 2, ui::kScreenW - ui::kMargin, ui::kSessionRowH - 6, 10, ui::kColorPanel);
    // 右のボタン・結果の手前までを使う。以前は 1 行目に「種類  物品番号」を書いていて、返却の行では
    // 15 文字の番号が「持出に切替」のボタンの下に隠れた (2026-09-15 実機)。1 行目は物品番号だけ、種類は 2 行目にする
    // 2026-09-15: 返却は読んだ時点で記録済み。返却の行と名札待ちの切替の行には「持出に切替 / 返却に戻す」、× は名札待ちの持出だけ
    const bool can_switch = session_.canToggleSwitch(i);
    const bool removable = row.status == RowStatus::Pending && row.kind == RowKind::Checkout;
    int text_right = ui::kScreenW - ui::kMargin * 2 - ui::textWidth(d, "記録済み", TextSize::Medium) - 16;
    if (can_switch) {
      text_right = ui::kSessionSwitchX - 12;
    } else if (removable) {
      text_right = ui::kSessionRemoveX - 12;
    } else if (row.status == RowStatus::Failed) {
      text_right = ui::kScreenW - ui::kMargin * 2 - ui::textWidth(d, bookResultText(row.result), TextSize::Small) - 16;
    }
    ui::drawTextFit(d, ui::kMargin, y + 8, text_right - ui::kMargin, row.item, TextSize::Medium, ui::kColorText,
                    ui::kColorPanel);
    const char* kind =
        (row.kind == RowKind::Return && row.status == RowStatus::Done) ? "返却済み" : rowKindText(row.kind);
    ui::drawText(d, ui::kMargin, y + 48, kind, TextSize::Small,
                 row.kind == RowKind::Checkout ? ui::kColorText : ui::kColorAccent, ui::kColorPanel);
    if (row.kind != RowKind::Checkout) {
      std::snprintf(buf, sizeof(buf), "借 %s", row.borrower[0] ? row.borrower : "不明");  // 借りていた人
      ui::drawText(d, ui::kMargin + ui::textWidth(d, kind, TextSize::Small) + 16, y + 48, buf, TextSize::Small,
                   ui::kColorDim, ui::kColorPanel);
    }
    if (can_switch) {
      drawButton(d, ui::kSessionSwitchX, y + 12, ui::kSessionSwitchW, ui::kSessionButtonH,
                 row.kind == RowKind::Return ? "持出に切替" : "返却に戻す", TextSize::Small);
    }
    if (removable) {
      // "✕" (U+2715) は日本語フォントに無く四角になった。JIS の "×" を大きな文字で使う
      drawButton(d, ui::kSessionRemoveX, y + 12, ui::kSessionRemoveW, ui::kSessionButtonH, "×", TextSize::Large);
    } else if (row.status == RowStatus::Failed) {
      ui::drawTextRight(d, ui::kScreenW - ui::kMargin * 2, y + 26, bookResultText(row.result), TextSize::Small,
                        ui::kColorAlert, ui::kColorPanel);
    } else if (!can_switch && row.status == RowStatus::Done) {
      ui::drawTextRight(d, ui::kScreenW - ui::kMargin * 2, y + 26, "記録済み", TextSize::Medium, ui::kColorOk,
                        ui::kColorPanel);
    }
  }
  d.endWrite();
}

void UiController::drawSessionBottom(uint32_t now_ms) {
  auto& d = M5.Display;
  char buf[96];
  const int y = ui::kSessionRowsY + ui::kSessionRowsShown * ui::kSessionRowH;
  d.startWrite();
  d.fillRect(0, y, ui::kScreenW, ui::kScreenH - y, ui::kColorBg);
  const size_t rows = session_.rowCount();
  if (rows > static_cast<size_t>(ui::kSessionRowsShown)) {
    std::snprintf(buf, sizeof(buf), "ほかに %u 件", static_cast<unsigned>(rows - ui::kSessionRowsShown));
    ui::drawTextRight(d, ui::kScreenW - ui::kMargin, y + 8, buf, TextSize::Small, ui::kColorDim, ui::kColorBg);
  }
  if (event_text_[0] != '\0' && static_cast<int32_t>(event_until_ms_ - now_ms) > 0) {
    ui::drawTextFit(d, ui::kMargin, y + 8, ui::kScreenW - ui::kMargin * 2, event_text_, TextSize::Small, ui::kColorWarn,
                    ui::kColorBg);
  } else if (log_.alertCount() > 0) {
    Alert a;
    if (log_.getAlert(0, &a)) {
      char when[24];
      formatShortDateTime(a.epoch, cfg_.tz_offset_min, when, sizeof(when));
      std::snprintf(buf, sizeof(buf), "未確認の無登録 %u 件 (最新 %s)", static_cast<unsigned>(log_.alertCount()), when);
      ui::drawText(d, ui::kMargin, y + 8, buf, TextSize::Small, ui::kColorAlert, ui::kColorBg);
    }
  }
  d.endWrite();
}

void UiController::drawUserLoansView(uint32_t now_ms) {
  auto& d = M5.Display;
  char buf[96];
  const char* user = screen_.userLoansUser();
  std::vector<Loan> loans(kMaxLoans);
  const size_t n = book_.listLoansOf(user, loans.data(), loans.size());
  const int64_t now_epoch = rtc_.now(now_ms);
  d.startWrite();
  d.fillRect(0, ui::kHeaderH, ui::kScreenW, ui::kScreenH - ui::kHeaderH, ui::kColorBg);
  d.fillRect(0, ui::kHeaderH, ui::kScreenW, ui::kSessionGuideH, ui::kColorGuideBg);
  std::snprintf(buf, sizeof(buf), "%s さんの貸出中 %u 件", user, static_cast<unsigned>(n));
  // 英字の利用者 ID は 15 文字まであるので、入らなければ縮める (2026-09-15)
  ui::drawTextFit(d, ui::kMargin, ui::kHeaderH + 34, ui::kScreenW - ui::kMargin * 2, buf, TextSize::Large,
                  ui::kColorText, ui::kColorGuideBg);
  const int top = ui::kHeaderH + ui::kSessionGuideH + 20;
  const size_t shown = static_cast<size_t>((ui::kTabBarY - top) / ui::kRowH);
  for (size_t i = 0; i < n && i < shown; ++i) {
    const Loan& l = loans[i];
    const int y = top + static_cast<int>(i) * ui::kRowH;
    const bool previous_day = isPreviousDay(now_epoch, l.checkout_epoch, cfg_.tz_offset_min);
    const uint16_t color = previous_day ? ui::kColorAlert : ui::kColorText;
    formatElapsed(now_epoch, l.checkout_epoch, buf, sizeof(buf));
    ui::drawTextRight(d, ui::kScreenW - ui::kMargin, y + 12, buf, TextSize::Medium, color, ui::kColorBg);
    // 物品番号は経過時間の手前まで (15 文字の W が「120時間前」に重なった。2026-09-15 実機)
    const int item_w = ui::kScreenW - ui::kMargin - ui::textWidth(d, buf, TextSize::Medium) - 16 - ui::kMargin;
    ui::drawTextFit(d, ui::kMargin, y + 8, item_w, l.item, TextSize::Large, color, ui::kColorBg);
    formatShortDateTime(l.checkout_epoch, cfg_.tz_offset_min, buf, sizeof(buf));
    ui::drawText(d, ui::kMargin, y + 62, buf, TextSize::Small, ui::kColorDim, ui::kColorBg);
    d.drawFastHLine(ui::kMargin, y + ui::kRowH - 2, ui::kScreenW - ui::kMargin * 2, ui::kColorPanel);
  }
  if (n == 0) {
    ui::drawText(d, ui::kMargin, top + 20, "貸出中の物品はありません", TextSize::Medium, ui::kColorDim, ui::kColorBg);
  }
  if (n > shown) {
    std::snprintf(buf, sizeof(buf), "ほかに %u 件", static_cast<unsigned>(n - shown));
    ui::drawTextRight(d, ui::kScreenW - ui::kMargin, ui::kTabBarY + 8, buf, TextSize::Small, ui::kColorDim,
                      ui::kColorBg);
  }
  ui::drawText(d, ui::kMargin, ui::kScreenH - 52, "タップで閉じます (10 秒で閉じます)", TextSize::Small,
               ui::kColorDim, ui::kColorBg);
  d.endWrite();
}

void UiController::drawPhotoView() {
  auto& d = M5.Display;
  const char* name = screen_.photoName();
  requestPhoto(name);
  d.startWrite();
  d.fillRect(0, ui::kHeaderH, ui::kScreenW, ui::kScreenH - ui::kHeaderH, ui::kColorBg);
  if (photoReady(name)) {
    const uint32_t t0 = millis();
    const bool ok = d.drawJpg(photo_data_, static_cast<uint32_t>(photo_len_), (ui::kScreenW - kViewW) / 2, ui::kHeaderH,
                              kViewW, kViewH, 0, 0, kViewScale, kViewScale);
    Serial.printf("#UI photo name=%s ok=%d ms=%lu\n", name, static_cast<int>(ok),
                  static_cast<unsigned long>(millis() - t0));
  } else {
    const bool loading = photo_status_ == PhotoStatus::Loading;
    ui::drawTextCenter(d, ui::kScreenW / 2, 600, loading ? "写真を読み込んでいます" : "写真はありません",
                       TextSize::Large, ui::kColorDim, ui::kColorBg);
  }
  d.fillRect(0, ui::kScreenH - 60, ui::kScreenW, 60, ui::kColorBg);
  ui::drawText(d, ui::kMargin, ui::kScreenH - 48, name, TextSize::Small, ui::kColorText, ui::kColorBg);
  ui::drawTextRight(d, ui::kScreenW - ui::kMargin, ui::kScreenH - 48, "タップで閉じます", TextSize::Small,
                    ui::kColorDim, ui::kColorBg);
  d.endWrite();
}

void UiController::drawSearchTab(uint32_t now_ms) {
  auto& d = M5.Display;
  char buf[96];
  row_count_ = 0;
  d.fillRoundRect(ui::kMargin, kSearchBoxY, ui::kScreenW - ui::kMargin * 2, kSearchBoxH, 12, ui::kColorPanel);
  if (search_query_[0] != '\0') {
    ui::drawText(d, ui::kMargin + 16, kSearchBoxY + 20, search_query_, TextSize::Large, ui::kColorText, ui::kColorPanel);
  } else {
    ui::drawText(d, ui::kMargin + 16, kSearchBoxY + 24, "物品番号の先頭を入力", TextSize::Medium, ui::kColorDim,
                 ui::kColorPanel);
  }
  if (search_query_[0] == '\0') {
    ui::drawText(d, ui::kMargin, kSearchResultsY + 20, "入力すると、その文字で始まる物品を出します", TextSize::Small,
                 ui::kColorDim, ui::kColorBg);
    ui::drawText(d, ui::kMargin, kSearchResultsY + 64, "貸出中なら借りている人、返却済みなら返した日時", TextSize::Small,
                 ui::kColorDim, ui::kColorBg);
  } else {
    ItemSearchHit hits[kSearchShown];
    const size_t found = book_.searchItems(search_query_, hits, kSearchShown);
    const int64_t now_epoch = rtc_.now(now_ms);
    for (size_t i = 0; i < found && i < kSearchShown; ++i) {
      const ItemSearchHit& h = hits[i];
      const int y = kSearchResultsY + static_cast<int>(i) * kSearchRowH;
      row_count_ = i + 1;
      // 物品番号は右の「貸出中」「返却済み」の手前まで (入らなければ縮める)
      const char* status = h.lent ? "貸出中" : h.has_return ? "返却済み" : "";
      const int status_w = status[0] != '\0' ? ui::textWidth(d, status, TextSize::Medium) + 16 : 0;
      ui::drawTextFit(d, ui::kMargin, y + 6, ui::kScreenW - ui::kMargin * 2 - status_w, h.item, TextSize::Large,
                      ui::kColorText, ui::kColorBg);
      char when[32];
      if (h.lent) {
        setRowPhoto(i, h.loan.checkout_photo);
        ui::drawTextRight(d, ui::kScreenW - ui::kMargin, y + 10, "貸出中", TextSize::Medium, ui::kColorWarn, ui::kColorBg);
        formatElapsed(now_epoch, h.loan.checkout_epoch, when, sizeof(when));
        std::snprintf(buf, sizeof(buf), "借りている人 %s (%s)%s", h.loan.user[0] ? h.loan.user : "不明", when,
                      h.loan.checkout_photo[0] ? "  写真" : "");
      } else if (h.has_return) {
        const char* photo = h.last_return.return_photo[0] ? h.last_return.return_photo : h.last_return.checkout_photo;
        setRowPhoto(i, photo);
        ui::drawTextRight(d, ui::kScreenW - ui::kMargin, y + 10, "返却済み", TextSize::Medium, ui::kColorOk,
                          ui::kColorBg);
        formatShortDateTime(h.last_return.return_epoch, cfg_.tz_offset_min, when, sizeof(when));
        // 2026-09-15: 返した人は記録しないので、返した日時だけ出す
        std::snprintf(buf, sizeof(buf), "返却 %s%s", when, photo[0] ? "  写真" : "");
      } else {
        setRowPhoto(i, "");
        std::snprintf(buf, sizeof(buf), "記録なし");
      }
      ui::drawText(d, ui::kMargin, y + 56, buf, TextSize::Small, ui::kColorAccent, ui::kColorBg);
      d.drawFastHLine(ui::kMargin, y + kSearchRowH - 2, ui::kScreenW - ui::kMargin * 2, ui::kColorPanel);
    }
    const int after = kSearchResultsY + static_cast<int>(kSearchShown) * kSearchRowH;
    if (found == 0) {
      std::snprintf(buf, sizeof(buf), "「%s」で始まる物品の記録はありません", search_query_);
      ui::drawText(d, ui::kMargin, kSearchResultsY + 20, buf, TextSize::Medium, ui::kColorDim, ui::kColorBg);
    } else if (found > kSearchShown) {
      std::snprintf(buf, sizeof(buf), "ほかに %u 件 (続けて入力すると絞り込めます)",
                    static_cast<unsigned>(found - kSearchShown));
      ui::drawText(d, ui::kMargin, after + 8, buf, TextSize::Small, ui::kColorDim, ui::kColorBg);
    }
  }
  drawKeyboard();
}

void UiController::drawKeyboard() {
  auto& d = M5.Display;
  char label[2] = {0, 0};
  for (int r = 0; r < 4; ++r) {
    const int y = kKeyboardY + r * (kKeyH + kKeyGap);
    for (int c = 0; c < 10; ++c) {
      char ch = kKeyLetters[r][c];
      if (search_lower_ && ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
      label[0] = ch;
      drawButton(d, kKeyX0 + c * (kKeyW + kKeyGap), y, kKeyW, kKeyH, label, TextSize::Large);
    }
  }
  const int y = kKeyboardY + 4 * (kKeyH + kKeyGap);
  const char* const special[4] = {search_lower_ ? "ABC" : "abc", "#", "削除", "クリア"};
  for (int i = 0; i < 4; ++i) drawButton(d, kSpecialX[i], y, kSpecialW[i], kKeyH, special[i], TextSize::Medium);
}

// --- タッチ ---

TouchResult UiController::touchSearch(int x, int y, uint32_t now_ms) {
  if (y >= kKeyboardY) {
    const int row = (y - kKeyboardY) / (kKeyH + kKeyGap);
    if (row >= kKeyRows) return TouchResult::None;
    size_t len = std::strlen(search_query_);
    if (row < 4) {
      const int col = (x - kKeyX0) / (kKeyW + kKeyGap);
      if (x < kKeyX0 || col < 0 || col >= 10) return TouchResult::None;
      char ch = kKeyLetters[row][col];
      if (search_lower_ && ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
      if (len + 1 >= sizeof(search_query_)) return TouchResult::None;  // 15 文字まで
      search_query_[len] = ch;
      search_query_[len + 1] = '\0';
    } else {
      int key = -1;
      for (int i = 0; i < 4; ++i) {
        if (x >= kSpecialX[i] && x < kSpecialX[i] + kSpecialW[i]) key = i;
      }
      if (key == 0) {
        search_lower_ = !search_lower_;
      } else if (key == 1) {
        if (len + 1 >= sizeof(search_query_)) return TouchResult::None;
        search_query_[len] = '#';
        search_query_[len + 1] = '\0';
      } else if (key == 2) {
        if (len == 0) return TouchResult::None;
        search_query_[len - 1] = '\0';
      } else if (key == 3) {
        if (len == 0) return TouchResult::None;
        search_query_[0] = '\0';
      } else {
        return TouchResult::None;
      }
    }
    body_dirty_ = true;
    return TouchResult::Handled;
  }
  if (y >= kSearchResultsY && y < kSearchResultsY + static_cast<int>(kSearchShown) * kSearchRowH) {
    const size_t index = static_cast<size_t>((y - kSearchResultsY) / kSearchRowH);
    if (index < row_count_ && index < kMaxRowPhotos && row_photos_[index][0] != '\0') {
      screen_.openPhoto(row_photos_[index], now_ms);
      return TouchResult::Handled;
    }
  }
  return TouchResult::None;
}

TouchResult UiController::touchTab(int x, int y, uint32_t now_ms) {
  if (y >= ui::kTabBarY) {
    if (x >= kGearX) {
      setEventText("設定は「設定」を 1 秒押し続けると開きます", now_ms);
      return TouchResult::None;
    }
    int index = x / kTabW;
    if (index >= kTabCount) index = kTabCount - 1;
    screen_.selectTab(static_cast<Tab>(index), now_ms);
    return TouchResult::Handled;
  }
  if (screen_.tab() == Tab::Search) return touchSearch(x, y, now_ms);
  if (band_h_ > 0 && y >= band_top_ && y < band_top_ + band_h_) {
    if (band_photo_[0] == '\0') return TouchResult::None;
    screen_.openPhoto(band_photo_, now_ms);
    return TouchResult::Handled;
  }
  if (screen_.pageCount() > 1 && x >= ui::kScreenW - ui::kPagerW && y >= list_top_) {
    const bool upper = y < (list_top_ + ui::kTabBarY) / 2;
    const bool moved = upper ? screen_.prevPage(now_ms) : screen_.nextPage(now_ms);
    return moved ? TouchResult::Handled : TouchResult::None;
  }
  if (y >= list_top_ && x < ui::kScreenW - ui::kPagerW) {
    const size_t index = static_cast<size_t>((y - list_top_) / ui::kRowH);
    if (index < row_count_ && index < kMaxRowPhotos && row_photos_[index][0] != '\0') {
      screen_.openPhoto(row_photos_[index], now_ms);
      return TouchResult::Handled;
    }
  }
  return TouchResult::None;
}

TouchResult UiController::touchSession(int x, int y, uint32_t now_ms) {
  // 取りやめ (2026-09-15: セッションごと、1 回押し)。取りやめて記録するのは App
  const int cancel_y = ui::kSessionTopY + ui::kSessionGuideH + ui::kSessionCancelDy;
  if (session_.canCancel() && inRect(x, y, ui::kSessionCancelX, cancel_y, ui::kSessionCancelW, ui::kSessionButtonH)) {
    return TouchResult::CancelSession;
  }
  if (session_.canCompleteReturn() &&
      inRect(x, y, ui::kSessionCancelX, cancel_y, ui::kSessionCancelW, ui::kSessionButtonH)) {
    return TouchResult::CompleteReturn;  // 同じ場所の「返却完了」(2026-09-16)
  }
  if (session_.canCompleteTagWindow() &&
      inRect(x, y, ui::kSessionCancelX, cancel_y, ui::kSessionCancelW, ui::kSessionButtonH)) {
    return TouchResult::CompleteTagWindow;  // 同じ場所の「完了」(2026-09-16。名札先の画面ですぐ次の人を登録)
  }
  if (y < ui::kSessionRowsY) return TouchResult::None;
  const int index = (y - ui::kSessionRowsY) / ui::kSessionRowH;
  if (index >= ui::kSessionRowsShown) return TouchResult::None;
  const size_t row_index = static_cast<size_t>(index);
  SessionRow row;
  if (!session_.getRow(row_index, &row)) return TouchResult::None;
  const int row_y = ui::kSessionRowsY + index * ui::kSessionRowH + 12;
  bool changed = false;
  if (row.status == RowStatus::Pending && row.kind == RowKind::Checkout &&
      inRect(x, y, ui::kSessionRemoveX, row_y, ui::kSessionRemoveW, ui::kSessionButtonH)) {
    changed = session_.removeRow(row_index);
  } else if (session_.canToggleSwitch(row_index) &&
             inRect(x, y, ui::kSessionSwitchX, row_y, ui::kSessionSwitchW, ui::kSessionButtonH)) {
    changed = session_.toggleSwitch(row_index, rtc_.now(now_ms));  // 名札先ならその場で貸し出す
  }
  if (!changed) return TouchResult::None;
  session_dirty_ = true;
  return TouchResult::SessionChanged;
}

}  // namespace toolcheck
