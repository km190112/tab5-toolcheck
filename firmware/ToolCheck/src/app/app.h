// 本体の配線: 記録 (NVS)・設定・時刻・QR・セッション・警告音・撮影・microSD・USB シリアルのコマンド・画面 (docs/設計.md)。
// M5 1 段目 (骨組み): 起動時に記録を読んで報告し (壊れた記録は読まずに報告して消さない)、
// info / report / dump / cfg / time / nvs erase / screenshot / restart を受け付ける。
// M5 2 段目: QR → 判別 → セッション → 貸出・返却・開閉ログ・赤帯、警告音と操作音、引き出しの開閉はシリアル注入 (sim、開発フラグ ON のときだけ)。
// M5 3 段目: 開放で写真名を決めて撮影タスクに頼み、SD タスクが保存する。アイドル時に古い写真を消す。
//   撮影の間は内部 I2C を撮影タスクが押さえるので、タッチと RTC は押さえられたときだけ読む。
// M5 4 段目: 画面は UiController が描く (App は出来事を知らせるだけ)。設定画面 (5 段目) / 省電力 (6 段目) は後で足す。
// 2026-09-14: 引き出しセンサ (Unit ToF) を使わない設定 (開閉を受け付けず、名札で確定したときに撮影する) と、
//   SD の写真の全削除 (設定画面の「データ削除」・sd erase photos YES。開閉ログに残す)。
// M6 (2026-09-15): Port A を Unit PaHub v2.1 で分けた。QR と ToF がどの ch にいるかを PortABus が探して繋ぎ、5 秒ごとに確かめて挿し替えを拾う。
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "ui_controller.h"
#include "../core/config.h"
#include "../core/config_store.h"
#include "../core/loan_book.h"
#include "../core/open_log.h"
#include "../core/power_manager.h"
#include "../core/session.h"
#include "../hw/alarm.h"
#include "../hw/camera_task.h"
#include "../hw/drawer_sensor.h"
#include "../hw/nvs_kv_store.h"
#include "../hw/port_a_bus.h"
#include "../hw/qr_reader.h"
#include "../hw/rtc_clock.h"
#include "../hw/sd_task.h"
#include "../hw/serial_port.h"

namespace toolcheck {

class App : public SettingsActions {
 public:
  App();

  void begin();
  void loop();

 private:
  // --- 設定画面から頼まれる操作 (SettingsActions) ---
  const Config& currentConfig() const override;
  bool applySettings(const Config& cfg, ConfigError* error) override;
  void previewBrightness(uint8_t pct) override;
  void testSound(uint8_t kind, uint8_t volume_pct) override;
  void takeTestPhoto() override;
  int64_t nowEpoch() const override;
  bool timeLost() const override;
  bool setClock(int64_t epoch) override;
  bool warningsPaused(uint32_t* remaining_ms) const override;
  void pauseWarnings(uint32_t minutes) override;
  void resumeWarnings() override;
  bool sdPresent() const override;
  uint16_t sdPhotoCount() const override;
  bool eraseSdPhotos() override;
  bool drawerSensorPresent() const override;
  void tofStatus(TofStatus* out) const override;
  bool startTofCalibration() override;
  bool confirmAlert(uint16_t index) override;
  bool cancelLoan(const char* item) override;
  void exportRecords(const char* what) override;
  bool eraseRecords() override;
  void eraseAll() override;
  size_t deviceInfo(char lines[][kInfoLineLen], size_t max_lines) override;

  void loadRecords();
  void printBootReport();
  void applyBrightness();
  // 画面への合図 (描くのは UiController)
  void drawStatusScreen();
  void drawClockLine();
  void drawSessionArea(uint32_t now_ms);
  void setEventText(const char* text);

  void pollInternalI2c(uint32_t now_ms);
  void handleTouch(int x, int y, uint32_t now_ms);
  // 省電力 (6 段目): 状態を決めて、変わったものだけ反映する
  void updatePower(uint32_t now_ms);
  void applyPower(const PowerOutputs& out, uint32_t now_ms);
  void cmdPower();
  void pollQr(uint32_t now_ms);
  void handleCode(const char* raw, size_t len, bool simulated, uint32_t now_ms);
  // src: "tof" (Unit ToF の判定) / "sim" (シリアル注入)
  void handleDrawer(bool open, const char* src, uint32_t now_ms);
  // 取りやめ (2026-09-15)。src: "touch" (画面の「取りやめ」) / "sim" (シリアル)
  void cancelSession(const char* src, uint32_t now_ms);
  // 返却完了 (2026-09-16)。src: "touch" / "sim"
  void completeReturn(const char* src, uint32_t now_ms);
  // 完了 (名札先の画面。2026-09-16)。src: "touch" / "sim"
  void completeTagWindow(const char* src, uint32_t now_ms);
  // 登録した後に呼ぶ。ToF が閉の範囲に 2 秒続けて戻るまで次の開放を受け付けない (2026-09-16)
  void holdDrawerOpen(const char* reason);
  void handleSessionOutput(const SessionOutput& out, uint32_t now_ms);
  void updateSession(uint32_t now_ms);
  void printSession(uint32_t now_ms);
  void printUserLoans(const char* user);

  void pollCamera();
  void pollSd(uint32_t now_ms);
  void pollPortA(uint32_t now_ms);
  void printI2cRoute(const char* reason);
  void pollDrawerSensor(uint32_t now_ms);
  void applyDrawerSetting();
  // 校正の画面を開いている間と、基準を取っている間は開閉を判定しない
  void updateDrawerJudging();
  // 基準距離を 2 秒集め始める。save = true (tof calibrate) は終わったら保存、false (設定画面) は結果を見せるだけ
  bool beginTofCalibration(bool save, uint32_t now_ms);
  void finishTofCalibration();
  // 写真名は peek で作り、使うと決まったら commit で同じ秒の数を進める (名札では撮らないことが多いので)
  struct PhotoNameKey {
    int64_t key = 0;
    uint8_t seq = 0;
  };
  void peekPhotoName(uint32_t now_ms, char* buf, size_t cap, PhotoNameKey* key);
  void commitPhotoName(const PhotoNameKey& key);
  void requestCapture(const char* photo);
  void showLatestPhoto();

  void handleCommand(char* line);
  void cmdSim(char** words, size_t count, const char* qr_raw, uint32_t now_ms);
  void cmdInfo();
  void cmdAbout();
  void cmdTof(char** words, size_t count, uint32_t now_ms);
  void cmdCam(uint32_t now_ms);
  void cmdPhotoTest(uint32_t now_ms);
  void cmdTime();
  void cmdTimeSet(const char* text);
  void cmdCfgGet(const char* name);
  void cmdCfgSet(const char* name, const char* value);
  void cmdDump(const char* what);
  void cmdNvsErase(const char* what, const char* confirm);
  void dumpCfg();
  void dumpLoans();
  void dumpHist();
  void dumpOpenLog();
  void dumpAlerts();

  NvsKvStore kv_;  // book_ と log_ より先に置く (初期化の順)
  LoanBook book_;
  OpenLog log_;
  Config cfg_;
  Session session_;  // book_ と cfg_ より後に置く
  PowerManager power_;  // cfg_ より後に置く
  RtcClock rtc_;
  PortABus bus_;  // qr_ と drawer_ より先に begin する (Wire を開いて PaHub の ch を繋ぐ)
  QrReader qr_;
  Alarm alarm_;
  SdTask sd_;
  CameraTask camera_;
  DrawerSensor drawer_;
  UiController ui_;  // 上の全部より後に置く (参照を持つ)
  SerialPort serial_;

  esp_err_t storage_err_ = ESP_OK;
  ConfigLoadResult cfg_result_;
  LoadReport book_report_;
  OpenLogLoadReport log_report_;
  bool drawer_open_ = false;  // Unit ToF の判定かシリアル注入 (sim) で開いている
  uint16_t tof_period_ms_ = 50;  // 反映した ToF の測定間隔 (PowerManager は操作中から始まる)
  bool tof_raw_active_ = false;  // tof raw <秒>: 読むたびに #TOF を出す
  uint32_t tof_raw_until_ms_ = 0;
  static constexpr uint32_t kTofCalibrationMs = 2000;
  static constexpr uint16_t kTofCalibrationBufLen = 64;  // 2 秒 ÷ 操作中の間隔 50ms = 40 個
  struct TofCalibrationRun {
    bool active = false;
    bool save = false;
    uint32_t until_ms = 0;
    uint16_t count = 0;
    uint16_t mm[kTofCalibrationBufLen] = {};
  };
  TofCalibrationRun calib_run_;
  TofCalibration last_calibration_;
  uint16_t calibration_seq_ = 0;  // 校正が終わるたびに増やす (設定画面が新しい結果に気付く)
  uint8_t last_alarm_level_ = 0;
  SessionState last_state_ = SessionState::Idle;
  bool session_dirty_ = true;
  uint32_t last_qr_ms_ = 0;
  uint32_t last_tick_ms_ = 0;
  uint32_t last_activity_ms_ = 0;  // QR・開閉・タッチ (古い写真を消すアイドル判定)
  bool prune_pending_ = false;
  bool camera_stuck_reported_ = false;
  bool touching_ = false;  // 指が触れている (離した瞬間だけ UiController に知らせる。sim press を毎ループ消さない)
  bool press_swallowed_ = false;  // 減光・消灯中の最初のタッチで起こしただけ (その押し方の長押し・押し続けも使わない)
  PowerState power_state_ = PowerState::Active;
  uint8_t applied_brightness_ = 0xFF;  // 最後に反映した明るさ (変わったときだけ反映する)
  bool qr_duty_ = false;
  uint32_t sd_interval_ms_ = 0;
  int64_t last_photo_key_ = INT64_MIN;  // 同じ秒の写真に -2 を付けるため
  uint8_t photo_seq_ = 0;
  // ループの間隔 (PC が読んでいないときに USB シリアルの書き込みで止まるかを見る。info で出して戻す)
  static constexpr uint32_t kSlowLoopMs = 200;
  uint32_t last_loop_entry_ms_ = 0;
  uint32_t loop_max_ms_ = 0;
  uint32_t loop_slow_ = 0;
};

}  // namespace toolcheck
