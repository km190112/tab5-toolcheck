// 貸出中 (loans) と返却履歴 (hist、100 件) の記録 (docs/設計.md「永続化 (NVS)」)。
// Arduino 非依存。読み出しは RAM に持った写しから返し、変更は KvStore に書けたものだけ写しに反映する。
//
// 保存の形 (数値はリトルエンディアン、文字列は NUL 終端で残りを 0 で埋める。先頭 1 バイトは版数):
//   loans  キー = 物品番号 (1〜15 文字)、値 = 貸出 1 件 (kLoanRecordSize バイト)
//   hist   RingStore (接頭字 'H'、100 件)、1 件 = 返却 1 件 (kReturnRecordSize バイト)
//
// 電源断の整合: 貸出を閉じる操作 (返却・付け替え・取消) は「①返却履歴に足す → ②貸出のキーを消す (付け替えは上書き)」の順。
// ②の前で落ちたら、次の load() で「最新の返却履歴が閉じた貸出がまだ loans に残っている」ことから閉じ終わっていたと判断し、
// 一覧から外す。起動直後に NVS へ書かないため、キーを実際に消すのは次の変更の直前 (付け替えで失われた新しい貸出は戻らない)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "kv_store.h"
#include "ring_store.h"

namespace toolcheck {

constexpr uint16_t kMaxLoans = 100;               // 同時に貸出中にできる数
constexpr uint16_t kReturnHistoryCapacity = 100;  // 返却履歴に残す数
constexpr size_t kCodeFieldLen = 16;              // 物品番号・利用者 ID の欄 (15 文字 + NUL)
constexpr size_t kPhotoFieldLen = 32;             // 写真のファイル名の欄 (31 文字 + NUL)
constexpr size_t kLoanRecordSize = 78;            // 保存する貸出 1 件のバイト数 (版数 1)
constexpr size_t kReturnRecordSize = 130;         // 保存する返却 1 件のバイト数 (版数 1)

// 貸出の印
constexpr uint8_t kLoanUserUnknown = 1 << 0;  // 利用者「不明」のまま貸出中にした (打ち切り)
constexpr uint8_t kLoanSwitched = 1 << 1;     // 「持出に切替」で付け替えた

// 返却の印
constexpr uint8_t kReturnProxy = 1 << 0;            // 借りた人と返した人が違う (代理)
constexpr uint8_t kReturnReturnerUnknown = 1 << 1;  // 返した人が分からない (打ち切り・返却漏れ)
constexpr uint8_t kReturnMissed = 1 << 2;           // 付け替えで閉じた (返却漏れ)
constexpr uint8_t kReturnCancelled = 1 << 3;        // 誤登録として取り消した

struct Loan {
  char item[kCodeFieldLen] = {0};
  char user[kCodeFieldLen] = {0};             // 空 = 不明
  int64_t checkout_epoch = 0;                 // RTC の値そのまま (時刻未設定でも入れる)
  char checkout_photo[kPhotoFieldLen] = {0};  // 空 = 写真なし
  uint32_t order = 0;                         // 貸し出した順 (貸出中の中で大きいほど新しい)
  uint8_t flags = 0;
};

struct ReturnEntry {
  char item[kCodeFieldLen] = {0};
  char borrower[kCodeFieldLen] = {0};  // 空 = 不明
  char returner[kCodeFieldLen] = {0};  // 空 = 不明
  int64_t checkout_epoch = 0;
  int64_t return_epoch = 0;
  char checkout_photo[kPhotoFieldLen] = {0};
  char return_photo[kPhotoFieldLen] = {0};
  uint8_t flags = 0;
};

enum class BookResult : uint8_t {
  Ok,
  NotLent,       // 貸出中ではない
  AlreadyLent,   // もう貸出中 (返却か付け替えにする)
  Full,          // 貸出中が上限
  InvalidArg,    // 物品番号が空か 15 文字超、利用者 ID が 15 文字超、写真名が 31 文字超
  StorageError,  // 書けなかった。記録は変わっていない (付け替えだけは、元の貸出を閉じたところで止まることがある)
};

struct LoadReport {
  bool loans_ok = true;         // 貸出中のキーを列挙できた
  bool history_ok = true;       // 返却履歴の管理情報が読めた (false = 壊れていて 0 件として扱う)
  uint16_t skipped_loans = 0;   // 読めなかった貸出の数 (消さずに残す)
  bool repair_pending = false;  // 閉じる途中で電源が落ちた貸出を一覧から外した (キーは次の変更の直前に消す)
};

struct ItemSearchHit {
  char item[kCodeFieldLen] = {0};
  bool lent = false;        // 貸出中なら loan が有効
  Loan loan;
  bool has_return = false;  // 返却履歴にあれば last_return (その物品の最新の 1 件) が有効
  ReturnEntry last_return;
};

class LoanBook {
 public:
  explicit LoanBook(KvStore& kv);

  // 保存先から読み直す。KvStore へは書かない
  LoadReport load();

  // --- 貸出中 ---
  uint16_t loanCount() const;
  // 貸出中なら out に写して true
  bool findLoan(const char* item, Loan* out) const;
  // 貸し出した順 (古い順 = 経過の長い順) に out へ入れ、入れた数を返す
  size_t listLoans(Loan* out, size_t cap) const;
  // user の貸出だけを貸し出した順に out へ入れ、入れた数を返す
  size_t listLoansOf(const char* user, Loan* out, size_t cap) const;

  // --- 返却履歴 ---
  // 読めなかった分も含む件数
  uint16_t returnCount() const;
  // 新しい方から index 番目 (0 = 最新)。範囲外・壊れていたら false
  bool getReturn(uint16_t index, ReturnEntry* out) const;
  // item の最新の返却
  bool findLastReturn(const char* item, ReturnEntry* out) const;

  // --- 検索 ---
  // 物品番号が prefix で始まる物品 (貸出中と返却履歴から。大文字・小文字は区別する) を物品番号の昇順に out へ入れる。
  // 見つかった物品の数を返す (cap を超えた分は入れない)
  size_t searchItems(const char* prefix, ItemSearchHit* out, size_t cap) const;

  // --- 変更 ---
  // 持出。user が nullptr か空なら「不明」(kLoanUserUnknown)。photo が nullptr か空なら写真なし。
  // switched: 「持出に切替」の持出 (kLoanSwitched)。2026-09-15 から返却は読んだ時点で書くので、切替は返却の後の持出として書く
  BookResult checkout(const char* item, const char* user, int64_t epoch, const char* photo, bool switched = false);
  // 返却。returner が nullptr か空なら「不明」
  BookResult returnItem(const char* item, const char* returner, int64_t epoch, const char* photo);
  // 持出に切替: 今の貸出を返却漏れとして閉じ、new_user に貸し出す (new_user が nullptr か空なら「不明」)
  BookResult switchLoan(const char* item, const char* new_user, int64_t epoch, const char* photo);
  // 誤登録の貸出を取り消す (返却履歴に「取消」で残す)
  BookResult cancelLoan(const char* item, int64_t epoch);

  // 貸出中と返却履歴を全部消し (記録の削除)、読み直す。どちらも消せたら true
  bool clear();

 private:
  struct CachedReturn {
    bool valid = false;
    ReturnEntry entry;
  };

  int findLoanIndex(const char* item) const;
  bool appendReturn(const ReturnEntry& entry);
  bool putLoan(const Loan& loan);
  void dropLoanAt(size_t index);  // RAM から外してキーを消す (消せなければ次の変更の直前に消す)
  bool flushPendingRemove();

  KvStore& kv_;
  RingStore hist_;
  std::vector<Loan> loans_;            // 並びは不定 (一覧を返すときに order で並べる)
  std::vector<CachedReturn> returns_;  // returns_[i] = 新しい方から i 番目
  uint32_t next_order_ = 1;
  char pending_remove_[kCodeFieldLen] = {0};  // 閉じ終わったのにキーが残っている物品番号 (空 = なし)
};

}  // namespace toolcheck
