// 開閉ログ (openlog、200 件) と、設定画面で確認するまで残す赤帯 (alerts、20 件) の記録
// (docs/設計.md「永続化 (NVS)」「写真」「セッション」)。
// Arduino 非依存。読み出しは RAM に持った写しから返し、変更は KvStore に書けたものだけ写しに反映する。
//
// 保存の形 (数値はリトルエンディアン、文字列は NUL 終端で残りを 0 で埋める。先頭 1 バイトは版数):
//   openlog RingStore (接頭字 'O'、200 件)、1 件 = kOpenLogRecordSize バイト
//   alerts  キー = "A" + 通し番号 (十進 10 桁)、値 = 赤帯 1 件 (kAlertRecordSize バイト)
//           キー "meta" = [次の通し番号 u32][溢れて消えた数 u16][予備 u16]
//
// 赤帯が付くセッション (打ち切りで無登録開放・利用者不明の持出があった) は
// 「①赤帯 → ②赤帯の meta → ③溢れた最古の赤帯のキーを消す → ④開閉ログ」の順に書く。
// 見せることが目的なので赤帯を先に書く (④の前で落ちたら、赤帯は残って開閉ログの 1 件が欠ける)。
// ②の前で落ちた赤帯は、読み込みで通し番号から数に入れる。③の前で落ちて 21 件以上あるときは、
// 古い方を一覧から外し、キーは次の変更の直前に消す。起動直後に NVS へ書かないため、load() は書かない。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "kv_store.h"
#include "ring_store.h"
#include "session.h"

namespace toolcheck {

constexpr uint16_t kOpenLogCapacity = 200;  // 開閉ログに残す数
constexpr uint16_t kAlertCapacity = 20;     // 赤帯に残す数 (溢れたら古い方を消す。2026-09-13 決定)
constexpr size_t kOpenLogRecordSize = 64;   // 保存する開閉ログ 1 件のバイト数 (版数 1)
constexpr size_t kAlertRecordSize = 48;     // 保存する赤帯 1 件のバイト数 (版数 1)

enum class OpenLogKind : uint8_t {
  Session = 1,         // セッションの終わり (名札で確定・打ち切り)
  PausedOpen = 2,      // 警告の一時停止中の開放 (セッションは起こしていない)
  PauseStarted = 3,    // 警告の一時停止を始めた
  PauseEnded = 4,      // 警告の一時停止が明けた (時間切れ・今すぐ再開)
  RecordsCleared = 5,  // 記録の削除 (削除した後の 1 件目)
  PhotosCleared = 6,   // SD の写真を全部消した (2026-09-14)
};

struct OpenLogEntry {
  OpenLogKind kind = OpenLogKind::Session;
  int64_t epoch = 0;                 // セッションの始まり (開放または最初の QR) / 出来事の時刻
  bool opened = false;               // 引き出しの開放があった
  bool gave_up = false;              // 打ち切った
  bool unregistered_open = false;    // 打ち切りで、1 件も読まなかった開放
  bool unknown_checkout = false;     // 打ち切りで、利用者不明の持出・切替がある
  bool cancelled = false;            // 取りやめた (2026-09-15)
  char user[kCodeFieldLen] = {0};    // 空 = 不明
  char photo[kPhotoFieldLen] = {0};  // 最初の開放の写真名 (空 = なし)
  uint8_t checkouts = 0;
  uint8_t returns = 0;
  uint8_t switches = 0;
  uint8_t failures = 0;
};

struct Alert {
  uint32_t seq = 0;                  // 足した順 (大きいほど新しい)
  int64_t epoch = 0;                 // セッションの始まり
  bool unregistered_open = false;
  bool unknown_checkout = false;
  char photo[kPhotoFieldLen] = {0};  // 空 = なし
};

struct OpenLogLoadReport {
  bool log_ok = true;           // 開閉ログの管理情報が読めた (false = 壊れていて 0 件として扱う)
  bool alerts_ok = true;        // 赤帯のキーを列挙できた
  uint16_t skipped_alerts = 0;  // 読めなかった赤帯の数 (消さずに残す)
};

class OpenLog {
 public:
  explicit OpenLog(KvStore& kv);

  // 保存先から読み直す。KvStore へは書かない
  OpenLogLoadReport load();

  // --- 開閉ログ ---
  // セッションの終わり・停止中の開放を書く。打ち切りで赤帯の印があれば、先に赤帯を足す。全部書けたら true
  bool appendSession(const SessionRecord& record);
  // 一時停止の開始・終了と、SD の写真の全削除を書く (それ以外の種類は書かずに false)
  bool appendEvent(OpenLogKind kind, int64_t epoch);
  uint16_t count() const;
  // 新しい方から index 番目 (0 = 最新)。範囲外・壊れていたら false
  bool get(uint16_t index, OpenLogEntry* out) const;

  // --- 赤帯 ---
  uint16_t alertCount() const;
  // 新しい方から index 番目 (0 = 最新)。範囲外なら false
  bool getAlert(uint16_t index, Alert* out) const;
  // 20 件を超えて消えた数 (赤帯を全部確認し終えたら 0 に戻る)
  uint16_t droppedAlerts() const;
  // 新しい方から index 番目を確認して消す。消せたら true
  bool confirmAlert(uint16_t index);

  // 開閉ログと赤帯を全部消し、開閉ログの 1 件目に「記録削除」を書く。全部できたら true
  bool clear(int64_t epoch);

 private:
  struct CachedEntry {
    bool valid = false;
    OpenLogEntry entry;
  };

  bool appendAlert(const SessionRecord& record);
  bool appendEntry(const OpenLogEntry& entry);
  bool putAlertMeta(uint32_t next_seq, uint16_t dropped);
  bool flushPendingAlertRemoves();

  KvStore& kv_;
  RingStore log_;
  std::vector<CachedEntry> entries_;       // entries_[i] = 新しい方から i 番目
  std::vector<Alert> alerts_;              // 新しい順 (最大 kAlertCapacity)
  std::vector<uint32_t> pending_removes_;  // 一覧から外したのにキーが残っている赤帯の通し番号
  uint32_t next_alert_seq_ = 1;
  uint16_t dropped_alerts_ = 0;
};

}  // namespace toolcheck
