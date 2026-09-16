// 1 回のセッション (1 人ぶんの持出・返却の登録) の流れと、警告・打ち切り (docs/設計.md「セッション」)。
// Arduino 非依存。貸出と返却は LoanBook に書き、開閉ログと赤帯に書く 1 件 (SessionRecord) は呼び出し側に返す。
// 時刻は millis() 相当 (32bit で一周する) と、記録に使う RTC の UNIX 秒の両方を受け取る。
//
//   Idle       : セッション無し。物品の QR か引き出しの開放で Collecting へ。
//                名札だけなら、引き出しの開閉に関係なくその人の貸出中一覧を出す (記録しない)
//   Collecting : 物品を集めて名札を待つ。貸出中の物品は読んだ時点で返却する (名札は要らず、返した人は記録しない。2026-09-15)。
//                名札待ちの行 (持出・切替) があるか、開けて何も読んでいない間は、最後に進んだ時点 (開放・有効な QR) から
//                警告 1 → 警告 2 → 打ち切り。名札待ちの行が無ければ (返却だけ) 警告せず、最後に進んでから 20 秒で終える
//                (「返却完了」で 20 秒を待たずに終える。2026-09-16)。
//                「取りやめ」で名札待ちの行を消して終える (開けた後か済んだ行があれば開閉ログに残す。赤帯にはしない)
//   TagWindow  : 名札で確定した後、名札を読んでから 20 秒。読んだ物品はその人で即確定する (物品を読んでも延びない)。
//                「完了」で 20 秒を待たずに終える (2026-09-16: すぐ次の人を登録したい)
//
// 警告の一時停止中: 開放してもセッションを起こさず、開閉ログの 1 件だけ返す。QR の登録はできるが、
// 音と打ち切りは止め、停止が明けた時点から警告を数え直す。ToF 校正中は呼び出し側が開放を渡さない。
//
// 引き出しセンサ (Unit ToF) を使わない設定 (2026-09-14): 呼び出し側は開放を渡さない。セッションは物品の QR で始まり、
// 写真は名札で確定したときに撮る (onCode に渡した写真名で記録する)。無登録の開放は起きない。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "code_classifier.h"
#include "config.h"
#include "loan_book.h"

namespace toolcheck {

constexpr uint32_t kDuplicateIgnoreMs = 3000;  // 同じコードを読み捨てる時間 (読み続けている間は延びる)
constexpr uint32_t kTagWindowMs = 20000;       // 名札を読んでから、読んだ物品をその人で即確定する時間
constexpr uint32_t kReturnOnlyEndMs = 20000;   // 名札待ちの行が無い (返却だけの) セッションを、最後に進んでから終えるまで
constexpr uint8_t kMaxSessionRows = 20;        // 1 回のセッションで扱う物品の数

enum class SessionState : uint8_t { Idle, Collecting, TagWindow };

enum class RowKind : uint8_t {
  Checkout,  // 持出
  Return,    // 返却 (読んだ時点で貸出中だった。その場で記録する)
  Switch,    // 持出に切替 (返却の行で押した。名札の人に切替の印で貸し出す)
};

enum class RowStatus : uint8_t {
  Pending,  // 名札待ち
  Done,     // 記録した
  Failed,   // 記録できなかった (result に理由)
};

struct SessionRow {
  char item[kCodeFieldLen] = {0};
  char borrower[kCodeFieldLen] = {0};  // 返却・切替の行で、今借りている人 (空 = 不明)
  RowKind kind = RowKind::Checkout;
  RowStatus status = RowStatus::Pending;
  BookResult result = BookResult::Ok;
};

enum class SessionEnd : uint8_t {
  Confirmed,   // 名札で確定した
  GaveUp,      // 打ち切った
  PausedOpen,  // 警告の一時停止中の開放 (セッションは起こしていない)
  Cancelled,   // 取りやめた (名札待ちの持出を消して終えた。2026-09-15)
};

// 開閉ログに書く 1 件
struct SessionRecord {
  SessionEnd end = SessionEnd::Confirmed;
  int64_t start_epoch = 0;             // 開放、または最初の QR の時刻
  bool opened = false;                 // 引き出しの開放があった
  char user[kCodeFieldLen] = {0};      // 空 = 不明
  char photo[kPhotoFieldLen] = {0};    // 最初の開放の写真名 (空 = なし)
  uint8_t checkouts = 0;               // 記録できた持出 (切替は含まない)
  uint8_t returns = 0;                 // 記録できた返却
  uint8_t switches = 0;                // 記録できた持出に切替
  uint8_t failures = 0;                // 記録できなかった行
  bool unregistered_open = false;      // 打ち切りで、1 件も読まなかった開放 (赤帯)
  bool unknown_checkout = false;       // 打ち切りで、利用者不明の持出・切替がある (赤帯)
};

struct SessionOutput {
  bool capture_photo = false;             // 開放を検知した (センサを使わない設定では名札で確定した)。写真を撮る
  bool show_user_loans = false;           // セッションの外で名札を読んだ。loans_user の貸出中一覧を出す
  char loans_user[kCodeFieldLen] = {0};
  bool ignored = false;                   // 読み捨てた (同じコードの連続・このセッションで扱い済み・不正・行がいっぱい)
  bool rows_full = false;                 // 行がいっぱいで読み捨てた
  bool cancelled = false;                 // 取りやめた (記録するものが無ければ record_count は 0)
  bool pause_started = false;
  bool pause_ended = false;
  uint8_t record_count = 0;               // 終わったセッションの数 (多くて 2)
  SessionRecord records[2];
};

class Session {
 public:
  Session(LoanBook& book, const Config& cfg);

  void setConfig(const Config& cfg);

  // 引き出しが開いた。photo_name はこの開放で撮る写真の名前 (カメラを使わないなら nullptr)
  SessionOutput onDrawerOpened(uint32_t now_ms, int64_t epoch, const char* photo_name);

  // QR を読んだ。photo_name は名札で確定したときに撮る写真の名前 (カメラを使えないなら nullptr)
  SessionOutput onCode(const ClassifiedCode& code, uint32_t now_ms, int64_t epoch, const char* photo_name = nullptr);

  // 周期処理 (一時停止の終わり・名札先の終わり・打ち切り)
  SessionOutput tick(uint32_t now_ms, int64_t epoch);

  // 名札待ちの持出の行の ✕。確定した行・返却・切替の行・範囲外は false。開放の無いセッションで行が無くなったらセッションを終える (記録しない)
  bool removeRow(size_t index);

  // 返却 (記録済み) の行で「持出に切替」→ 名札待ちの切替 (名札先の 20 秒ならその場で名札の人に貸し出す)。
  // 名札待ちの切替でもう一度押すと返却に戻す (本は書き直さない)。それ以外は false
  bool toggleSwitch(size_t index, int64_t epoch);
  bool canToggleSwitch(size_t index) const;

  // 取りやめ (警告を数えている間だけ)。名札待ちの持出を消し、名札待ちの切替は返却に戻して終える。
  // 開けた後か済んだ行があれば SessionEnd::Cancelled の 1 件を返す。何もしなければ cancelled は false
  SessionOutput cancel(uint32_t now_ms, int64_t epoch);
  bool canCancel() const;

  // 返却完了 (返却だけのセッションを 20 秒待たずに終える。2026-09-16)。20 秒で終えるのと同じ「確定」の 1 件を返す
  SessionOutput completeReturn(uint32_t now_ms, int64_t epoch);
  bool canCompleteReturn() const;

  // 完了 (名札先の 20 秒を待たずに終える。2026-09-16: すぐ次の人を登録したい)。
  // 20 秒たって次の名札・開放・別の名札で区切られたときと同じ「確定」の 1 件を返す
  SessionOutput completeTagWindow(uint32_t now_ms, int64_t epoch);
  bool canCompleteTagWindow() const;

  // 警告を数えている (集め中で、名札待ちの行があるか、行が 0 件)
  bool isCounting() const;

  // 警告の一時停止 (停止中にもう一度呼ぶと、その時点から duration_ms に置き換える)
  SessionOutput pause(uint32_t duration_ms, uint32_t now_ms, int64_t epoch);
  SessionOutput resume(uint32_t now_ms, int64_t epoch);

  SessionState state() const;
  uint8_t alarmLevel(uint32_t now_ms) const;        // 0 = 鳴らさない / 1 = 警告 1 / 2 = 警告 2
  uint32_t sinceProgressMs(uint32_t now_ms) const;  // 名札待ちで、最後に進んでからの時間 (それ以外は 0)
  bool hasOpened() const;                           // このセッションで引き出しが開いた (案内文の出し分け)
  const char* tagUser() const;                      // 名札先の人 (TagWindow 以外は空文字列)
  size_t rowCount() const;
  bool getRow(size_t index, SessionRow* out) const;
  bool isPaused() const;
  uint32_t pauseRemainingMs(uint32_t now_ms) const;

 private:
  struct RecentCode {
    char text[kCodeFieldLen] = {0};
    uint32_t last_seen_ms = 0;
    bool used = false;
  };

  bool isDuplicate(const char* text, uint32_t now_ms);
  void expireTimers(uint32_t now_ms, int64_t epoch, SessionOutput* out);
  void startSession(uint32_t now_ms, int64_t epoch, bool opened, const char* photo);
  void endSession(SessionEnd end, SessionOutput* out);
  void resetSession();
  void endPause(uint32_t now_ms, SessionOutput* out);
  void commitRow(SessionRow* row, const char* user, int64_t epoch);
  void giveUp(int64_t epoch, SessionOutput* out);
  int rowIndexOf(const char* item) const;
  static void addRecord(SessionOutput* out, const SessionRecord& record);

  LoanBook& book_;
  Config cfg_;
  SessionState state_ = SessionState::Idle;
  std::vector<SessionRow> rows_;
  SessionRecord record_;  // 進行中のセッションの記録
  uint32_t progress_ms_ = 0;
  uint32_t tag_ms_ = 0;
  char tag_user_[kCodeFieldLen] = {0};
  bool paused_ = false;
  uint32_t pause_start_ms_ = 0;
  uint32_t pause_duration_ms_ = 0;
  RecentCode recent_[8];
};

}  // namespace toolcheck
