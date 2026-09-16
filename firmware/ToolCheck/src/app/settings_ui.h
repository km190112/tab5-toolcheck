// 設定画面 (docs/設計.md「設定画面」)。「設定」の長押し → PIN → 13 項目の一覧 → 各項目の画面。
// 2026-09-17: 13 項目目「このソフトについて」(版数・リリース日・開発者・GitHub の URL と QR・ライセンス全文)。
// ここは描くのとタッチだけを受け持ち、記録や設定を変える操作は SettingsActions (App) に頼む。読むだけの貸出と赤帯は直接見る。
// M5 5 段目:
//   5b-1 PIN・一覧・数値の編集 (明るさ / 音量 / 警告の秒数 / 写真 / 利用者 ID の桁数)・警告の一時停止・時刻
//   5b-2 赤帯・貸出の取消 / USB シリアル出力 / NVS データ削除 (件数を見せてから 3 秒押し続ける) / 機器情報 / PIN の変更
//   動作ログの入切は 2026-09-14 に取りやめ
// M6 (2026-09-15): 「引き出しセンサ」に開の閾値・今の距離・基準を取る (2 秒の中央値を下書きに入れ、保存で反映)。開いている間は開閉を判定しない
// 2026-09-14: 「ToF 校正」の項目を「引き出しセンサ」(使う / 使わない) に、「NVS データ削除」を「データ削除」にして SD の写真の全削除を足した
// 設定を変えても「保存」を押すまでは反映しない (明るさだけはその場で試し、戻ると元に戻す)。
#pragma once

#include <cstddef>
#include <cstdint>

#include "../core/config.h"
#include "../core/loan_book.h"
#include "../core/open_log.h"
#include "../core/pin_lock.h"
#include "../core/text_pager.h"
#include "../core/time_format.h"
#include "../core/tof_reading.h"

namespace toolcheck {

constexpr size_t kInfoLineLen = 80;

// 引き出しセンサの今の状態 (設定画面の「引き出しセンサ」で距離と校正を見せる。M6)
struct TofStatus {
  bool enabled = false;
  bool present = false;  // Unit ToF を見つけて測っている
  bool fault = false;
  bool has_sample = false;
  uint16_t mm = 0;
  bool valid = false;
  bool calibrating = false;       // 基準を取っている (2 秒)
  uint16_t calibration_seq = 0;   // 校正が終わるたびに増える
  TofCalibration calibration;     // 直近の校正の結果
  char route[64] = {0};           // "PaHub 0x70 (QR ch0・ToF ch1)"
};
constexpr size_t kInfoMaxLines = 14;
constexpr uint32_t kEraseHoldMs = 3000;  // データ削除 (記録・全削除・SD の写真) は 3 秒押し続ける (計画の二段確認の 2 段目)

class SettingsActions {
 public:
  virtual ~SettingsActions() = default;

  virtual const Config& currentConfig() const = 0;
  // 制約を確かめて保存し、すぐ反映する。違反なら false で error に理由、保存できなければ false で error は None
  virtual bool applySettings(const Config& cfg, ConfigError* error) = 0;
  // 保存の前に明るさだけ試す
  virtual void previewBrightness(uint8_t pct) = 0;
  // 試し鳴らし: kind 0 = 操作音 / 1 = 警告 1 / 2 = 警告 2
  virtual void testSound(uint8_t kind, uint8_t volume_pct) = 0;
  virtual void takeTestPhoto() = 0;

  virtual int64_t nowEpoch() const = 0;  // UTC
  virtual bool timeLost() const = 0;
  virtual bool setClock(int64_t epoch) = 0;

  virtual bool warningsPaused(uint32_t* remaining_ms) const = 0;
  virtual void pauseWarnings(uint32_t minutes) = 0;
  virtual void resumeWarnings() = 0;

  virtual bool sdPresent() const = 0;
  virtual uint16_t sdPhotoCount() const = 0;
  // SD の写真を全部消すよう頼む。結果は後で SettingsUi::onPhotosErased で知らせる。SD が無い・頼めなければ false
  virtual bool eraseSdPhotos() = 0;
  // 引き出しセンサ (Unit ToF) が応答している
  virtual bool drawerSensorPresent() const = 0;
  virtual void tofStatus(TofStatus* out) const = 0;
  // 基準距離を取り始める (2 秒)。保存はしない (結果は tofStatus の calibration_seq が進んだら読み、下書きに入れる)。
  // 測っていない・取っている最中なら false
  virtual bool startTofCalibration() = 0;

  // 赤帯を確認して消す (新しい方から index 番目)
  virtual bool confirmAlert(uint16_t index) = 0;
  // 誤登録の貸出を取り消す (返却履歴に「取消」で残る)
  virtual bool cancelLoan(const char* item) = 0;
  // USB シリアルに CSV を出す (loans / hist / openlog / alerts / cfg / all)
  virtual void exportRecords(const char* what) = 0;
  // 記録の削除 (設定は残す)
  virtual bool eraseRecords() = 0;
  // 全削除 (記録領域ごと消して再起動する)
  virtual void eraseAll() = 0;
  // 機器情報を 1 行ずつ入れ、入れた行数を返す
  virtual size_t deviceInfo(char lines[][kInfoLineLen], size_t max_lines) = 0;
};

enum class SettingsResult : uint8_t {
  None,     // 何もしなかった
  Handled,  // 操作した
  Close,    // 設定画面を閉じる
};

class SettingsUi {
 public:
  SettingsUi(SettingsActions& actions, const LoanBook& book, const OpenLog& log);

  // PIN の入力から始める
  void open(uint32_t now_ms);
  // ヘッダの下を全部描く
  void draw(uint32_t now_ms);
  // 描き直しが要る (触った・残り秒が変わった・押し続けている)
  bool needsRedraw(uint32_t now_ms) const;
  // 一部だけ描き直す (「引き出しセンサ」の距離)。needsRedraw が false のときに呼ぶ
  bool needsLiveRedraw(uint32_t now_ms) const;
  void drawLive(uint32_t now_ms);
  SettingsResult onTouch(int x, int y, uint32_t now_ms);
  // 押し続けている。held_ms は押し始めてからの時間 (NVS データ削除の 3 秒)
  void onPressing(int x, int y, uint32_t held_ms);
  void onReleased();
  // SD の写真の全削除が終わった (left = 消せずに残った枚数)
  void onPhotosErased(bool ok, uint16_t removed, uint16_t left);
  // 「引き出しセンサ」(校正の画面) を開いている。開いている間は開閉を判定しない (App が毎ループ見る)
  bool tofScreenOpen() const;

 private:
  enum class Page : uint8_t {
    Pin, Menu, Editor, Pause, Clock, Cancel, Export, Erase, Info, PinChange, NotYet,
    About, Licenses, LicenseText  // このソフトについて → ライセンスの節の一覧 → 全文 (2026-09-17)
  };

  void drawPin(uint32_t now_ms);
  void drawMenu();
  void drawEditor();
  void drawPause();
  void drawClock();
  void drawCancel();
  void drawExport();
  void drawErase();
  void drawInfo();
  void drawPinChange();
  void drawNotYet();
  void drawAbout();
  void drawLicenses();
  void drawLicenseText();
  void drawDrawerLive(int extra_y);
  void pollTofCalibration();
  void drawTitle(const char* title, const char* back_label);
  void drawMessage(int y);
  void drawPad();

  SettingsResult touchPin(int x, int y, uint32_t now_ms);
  SettingsResult touchMenu(int x, int y);
  SettingsResult touchEditor(int x, int y);
  SettingsResult touchPause(int x, int y);
  SettingsResult touchClock(int x, int y);
  SettingsResult touchCancel(int x, int y);
  SettingsResult touchExport(int x, int y);
  SettingsResult touchPinChange(int x, int y);
  SettingsResult touchAbout(int x, int y);
  SettingsResult touchLicenses(int x, int y);
  SettingsResult touchLicenseText(int x, int y);
  int padKey(int x, int y) const;  // テンキーの何番目か (-1 = 外)
  void openLicense(uint8_t section);

  void openItem(uint8_t item);
  void openEditor(uint8_t editor);
  void startClockDraft();
  void setMessage(const char* text, bool error);
  bool backPressed(int x, int y) const;

  SettingsActions& actions_;
  const LoanBook& book_;
  const OpenLog& log_;
  PinLock pin_;
  Page page_ = Page::Pin;
  uint8_t item_ = 0;
  uint8_t editor_ = 0;
  Config draft_;
  LocalDateTime clock_draft_;
  int16_t tz_draft_ = 540;
  char message_[128] = {0};
  bool message_error_ = false;
  bool dirty_ = true;
  uint32_t drawn_second_ = 0xFFFFFFFFu;
  // 引き出しセンサ (M6)
  uint32_t drawn_live_tick_ = 0xFFFFFFFFu;
  uint16_t seen_calibration_seq_ = 0;
  char calib_text_[128] = {0};
  bool calib_error_ = false;
  // 赤帯・貸出の取消
  bool cancel_alerts_ = true;  // true = 赤帯 / false = 貸出中
  uint16_t cancel_page_ = 0;
  char pending_cancel_[kCodeFieldLen] = {0};  // もう一度押すと取り消す物品
  // NVS データ削除の押し続け
  uint8_t hold_target_ = 0;  // 0 = なし / 1 = 記録の削除 / 2 = 全削除 / 3 = SD の写真
  uint32_t hold_ms_ = 0;
  bool hold_fired_ = false;
  uint32_t drawn_hold_step_ = 0;
  // PIN の変更
  uint8_t pin_step_ = 0;  // 0 = 新しい PIN / 1 = もう一度
  char first_pin_[kPinLength + 1] = {0};
  char new_pin_[kPinLength + 1] = {0};
  uint8_t new_len_ = 0;
  // ライセンス全文 (開いた節を 1 回だけ折り返し、ページで見せる)
  uint8_t license_section_ = 0;
  uint16_t license_page_ = 0;
  TextPager license_pager_;
};

}  // namespace toolcheck
