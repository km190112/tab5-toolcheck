// 画面の描画とタッチ (docs/設計.md「画面」)。どの画面を出すかは core/screen_state が決め、ここは描いてタッチを操作に変える。
// M5 4 段目:
//   4a ヘッダ・タブ・貸出中 / 返却履歴 / 開閉ログの一覧とページ送り・セッションの全面表示・名札だけの一覧・記録領域を読めない画面
//   4b 赤帯の写真 (タップで全面)・一覧の写真のある行をタップで全面・写真の全面表示
//   4d 検索 (英数キーボードで物品番号の先頭 → 候補 → 借りている人 / 返した日時。赤帯は出さない)
// 描き直しは「見た目が変わったときに、変わった区画だけ」(M1 で全面の書き直しが遅いと分かった)。
// 写真は直近に撮った 1 枚なら撮影タスクの RAM から、それ以外は SD タスクに読みを頼む。赤帯の縮小写真は PSRAM の画像に作り置く
// (JPEG を描くたびに展開するとループが止まるため)。
#pragma once

#include <M5GFX.h>

#include <cstddef>
#include <cstdint>

#include "../core/config.h"
#include "../core/loan_book.h"
#include "../core/open_log.h"
#include "../core/screen_state.h"
#include "../core/session.h"
#include "../hw/camera_task.h"
#include "../hw/drawer_sensor.h"
#include "../hw/qr_reader.h"
#include "../hw/rtc_clock.h"
#include "../hw/sd_task.h"
#include "settings_ui.h"

namespace toolcheck {

enum class TouchResult : uint8_t {
  None,            // 何もしなかった
  Handled,         // 画面の操作 (タブ・ページ送り・閉じる・写真を開く)
  SessionChanged,  // セッションの行を変えた (×・持出に切替)
  CancelSession,   // 「取りやめ」を押した (取りやめて記録するのは App。2026-09-15)
  CompleteReturn,  // 「返却完了」を押した (終えて記録するのは App。2026-09-16)
  CompleteTagWindow,  // 名札先の画面で「完了」を押した (終えて記録するのは App。2026-09-16)
};

class UiController {
 public:
  UiController(LoanBook& book, OpenLog& log, Session& session, const Config& cfg, RtcClock& rtc, QrReader& qr,
               CameraTask& camera, SdTask& sd, const DrawerSensor& drawer, SettingsActions& settings_actions);

  // storage_ok = false なら「記録領域を読めません」だけを出す
  void begin(uint32_t now_ms, bool storage_ok, const char* storage_error);
  void loop(uint32_t now_ms);

  // 押した瞬間の座標
  TouchResult onTouch(int x, int y, uint32_t now_ms);
  // 1 秒押し続けた座標 (「設定」の長押しで設定画面を開く)
  void onHold(int x, int y, uint32_t now_ms);
  // 押し続けている (held_ms は押し始めてから)。データ削除と「記録領域を読めません」の全削除の 3 秒
  void onPressing(int x, int y, uint32_t held_ms);
  void onReleased();

  // 操作中として扱う画面を出している (設定・写真・名札の一覧・検索。省電力の「操作中」に使う)
  bool busy() const;
  // 設定画面の「引き出しセンサ」(校正) を出している。出している間は開閉を判定しない (M6)
  bool tofScreenOpen() const;
  // 設定画面を開いている。開いている間は開閉を判定しない (2026-09-16: ToF が反応してもセッション画面に移さない)
  bool settingsOpen() const;

  void onRecordsChanged();  // 貸出・履歴・開閉ログ・赤帯・設定が変わった
  void onSessionChanged();  // セッションの状態・行・警告が変わった
  void onStatusChanged();   // QR・SD・カメラ・時刻の状態が変わった
  void onUserLoans(const char* user, uint32_t now_ms);
  void setEventText(const char* text, uint32_t now_ms);

  // SD タスクの ReadDone / ReadFailed。ReadDone の data の持ち主はここに移る
  void onPhotoRead(const SdEvent& ev);
  // SD の写真を全部消した (設定画面に結果を出し、持っている写真と縮小を捨てる)
  void onPhotosErased(bool ok, uint16_t removed, uint16_t left);
  // 写真を全面に出す。セッション中は出さずに false
  bool openPhoto(const char* name, uint32_t now_ms);

 private:
  enum class PhotoStatus : uint8_t { None, Loading, Ready, Missing };

  void drawBody(uint32_t now_ms);
  void drawHeader(uint32_t now_ms);
  void drawStorageError();
  void drawStorageHold(uint32_t held_ms);
  void drawTabView(uint32_t now_ms);
  int drawAlertBand(int top);
  void drawLoansRows(int list_top, uint16_t rows, uint32_t now_ms);
  void drawHistoryRows(int list_top, uint16_t rows);
  void drawOpenLogRows(int list_top, uint16_t rows);
  void drawPager(int list_top);
  void drawTabBar();
  void drawToast(uint32_t now_ms);
  void drawSessionView(uint32_t now_ms);
  void drawSessionTop(uint32_t now_ms, bool force);
  void drawSessionRows();
  void drawSessionBottom(uint32_t now_ms);
  void drawUserLoansView(uint32_t now_ms);
  void drawPhotoView();
  void drawSearchTab(uint32_t now_ms);
  void drawKeyboard();

  void requestPhoto(const char* name);
  void releasePhoto();
  bool photoReady(const char* name) const;
  bool ensureThumbnail(const char* name);
  void setRowPhoto(size_t index, const char* name);

  TouchResult touchTab(int x, int y, uint32_t now_ms);
  TouchResult touchSearch(int x, int y, uint32_t now_ms);
  TouchResult touchSession(int x, int y, uint32_t now_ms);

  LoanBook& book_;
  OpenLog& log_;
  Session& session_;
  const Config& cfg_;
  RtcClock& rtc_;
  QrReader& qr_;
  CameraTask& camera_;
  SdTask& sd_;
  const DrawerSensor& drawer_;
  SettingsActions& actions_;
  ScreenState screen_;
  SettingsUi settings_;
  uint32_t storage_hold_ms_ = 0;  // 「記録領域を読めません」の全削除を押し続けている時間
  bool storage_hold_fired_ = false;

  bool storage_ok_ = true;
  char storage_error_[48] = {0};
  bool started_ = false;
  bool body_dirty_ = true;
  bool session_dirty_ = true;
  bool header_dirty_ = true;
  uint32_t drawn_revision_ = 0xFFFFFFFFu;
  View drawn_view_ = View::Tab;
  uint32_t last_header_ms_ = 0;
  uint32_t last_list_ms_ = 0;
  uint32_t session_top_key_ = 0xFFFFFFFFu;
  int list_top_ = 0;
  char event_text_[128] = {0};
  uint32_t event_until_ms_ = 0;
  bool toast_shown_ = false;

  // 写真 (1 枚だけ持つ)
  PhotoStatus photo_status_ = PhotoStatus::None;
  char photo_name_[kPhotoFieldLen] = {0};
  uint8_t* photo_data_ = nullptr;  // heap_caps_malloc (PSRAM)
  size_t photo_len_ = 0;
  // 赤帯の縮小写真
  M5Canvas thumb_;
  char thumb_name_[kPhotoFieldLen] = {0};
  int band_top_ = 0;
  int band_h_ = 0;
  char band_photo_[kPhotoFieldLen] = {0};
  // いま描いている一覧の行の写真名 (タップで開く)
  static constexpr size_t kMaxRowPhotos = 12;
  char row_photos_[kMaxRowPhotos][kPhotoFieldLen] = {};
  size_t row_count_ = 0;
  // 検索
  char search_query_[kCodeFieldLen] = {0};  // 物品番号と同じく 15 文字まで
  bool search_lower_ = false;               // 小文字で打つ (物品番号は大文字・小文字を区別する)
};

}  // namespace toolcheck
