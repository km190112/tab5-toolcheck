#include "loan_book.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace toolcheck {
namespace {

const char* const kLoansNs = "loans";
const char* const kHistNs = "hist";
const char kHistPrefix = 'H';
const uint8_t kRecordVersion = 1;

// 貸出 1 件の並び: [版数][印][物品番号 16][利用者 ID 16][持出時刻 i64][貸し出した順 u32][持出時の写真名 32]
const size_t kLoanItemAt = 2;
const size_t kLoanUserAt = 18;
const size_t kLoanEpochAt = 34;
const size_t kLoanOrderAt = 42;
const size_t kLoanPhotoAt = 46;
static_assert(kLoanPhotoAt + kPhotoFieldLen == kLoanRecordSize, "貸出 1 件の並びと大きさが合わない");

// 返却 1 件の並び: [版数][印][物品番号 16][借りた人 16][返した人 16][持出時刻 i64][返却時刻 i64][持出時の写真名 32][返却時の写真名 32]
const size_t kRetItemAt = 2;
const size_t kRetBorrowerAt = 18;
const size_t kRetReturnerAt = 34;
const size_t kRetCheckoutEpochAt = 50;
const size_t kRetReturnEpochAt = 58;
const size_t kRetCheckoutPhotoAt = 66;
const size_t kRetReturnPhotoAt = 98;
static_assert(kRetReturnPhotoAt + kPhotoFieldLen == kReturnRecordSize, "返却 1 件の並びと大きさが合わない");

void putU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint32_t getU32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

void putI64(uint8_t* p, int64_t v) {
  const uint64_t u = static_cast<uint64_t>(v);
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(u >> (8 * i));
}

int64_t getI64(const uint8_t* p) {
  uint64_t u = 0;
  for (int i = 7; i >= 0; --i) u = (u << 8) | p[i];
  return static_cast<int64_t>(u);
}

// src (nullptr は空) を欄に写し、残りを 0 で埋める。NUL まで欄に収まらなければ false (欄は変えない)
bool setField(char* field, size_t field_len, const char* src) {
  const size_t n = (src == nullptr) ? 0 : std::strlen(src);
  if (n >= field_len) return false;
  std::memset(field, 0, field_len);
  if (n > 0) std::memcpy(field, src, n);
  return true;
}

// 欄を保存用に書く (NUL の後ろは 0 で埋める)
void putField(uint8_t* p, const char* field, size_t field_len) {
  std::memset(p, 0, field_len);
  const void* end = std::memchr(field, 0, field_len);
  const size_t n = (end != nullptr) ? static_cast<size_t>(static_cast<const char*>(end) - field) : field_len - 1;
  std::memcpy(p, field, n);
}

// 保存された欄を読む。欄の中に NUL が無ければ壊れているので false
bool getField(char* field, size_t field_len, const uint8_t* p) {
  if (std::memchr(p, 0, field_len) == nullptr) return false;
  std::memcpy(field, p, field_len);
  return true;
}

void encodeLoan(const Loan& loan, uint8_t* out) {
  std::memset(out, 0, kLoanRecordSize);
  out[0] = kRecordVersion;
  out[1] = loan.flags;
  putField(out + kLoanItemAt, loan.item, kCodeFieldLen);
  putField(out + kLoanUserAt, loan.user, kCodeFieldLen);
  putI64(out + kLoanEpochAt, loan.checkout_epoch);
  putU32(out + kLoanOrderAt, loan.order);
  putField(out + kLoanPhotoAt, loan.checkout_photo, kPhotoFieldLen);
}

bool decodeLoan(const uint8_t* in, Loan* out) {
  if (in[0] != kRecordVersion) return false;
  Loan loan;
  loan.flags = in[1];
  if (!getField(loan.item, kCodeFieldLen, in + kLoanItemAt) || loan.item[0] == '\0') return false;
  if (!getField(loan.user, kCodeFieldLen, in + kLoanUserAt)) return false;
  if (!getField(loan.checkout_photo, kPhotoFieldLen, in + kLoanPhotoAt)) return false;
  loan.checkout_epoch = getI64(in + kLoanEpochAt);
  loan.order = getU32(in + kLoanOrderAt);
  *out = loan;
  return true;
}

void encodeReturn(const ReturnEntry& entry, uint8_t* out) {
  std::memset(out, 0, kReturnRecordSize);
  out[0] = kRecordVersion;
  out[1] = entry.flags;
  putField(out + kRetItemAt, entry.item, kCodeFieldLen);
  putField(out + kRetBorrowerAt, entry.borrower, kCodeFieldLen);
  putField(out + kRetReturnerAt, entry.returner, kCodeFieldLen);
  putI64(out + kRetCheckoutEpochAt, entry.checkout_epoch);
  putI64(out + kRetReturnEpochAt, entry.return_epoch);
  putField(out + kRetCheckoutPhotoAt, entry.checkout_photo, kPhotoFieldLen);
  putField(out + kRetReturnPhotoAt, entry.return_photo, kPhotoFieldLen);
}

bool decodeReturn(const uint8_t* in, ReturnEntry* out) {
  if (in[0] != kRecordVersion) return false;
  ReturnEntry entry;
  entry.flags = in[1];
  if (!getField(entry.item, kCodeFieldLen, in + kRetItemAt) || entry.item[0] == '\0') return false;
  if (!getField(entry.borrower, kCodeFieldLen, in + kRetBorrowerAt)) return false;
  if (!getField(entry.returner, kCodeFieldLen, in + kRetReturnerAt)) return false;
  if (!getField(entry.checkout_photo, kPhotoFieldLen, in + kRetCheckoutPhotoAt)) return false;
  if (!getField(entry.return_photo, kPhotoFieldLen, in + kRetReturnPhotoAt)) return false;
  entry.checkout_epoch = getI64(in + kRetCheckoutEpochAt);
  entry.return_epoch = getI64(in + kRetReturnEpochAt);
  *out = entry;
  return true;
}

bool validItem(const char* item) {
  if (item == nullptr) return false;
  const size_t n = std::strlen(item);
  return n >= 1 && n < kCodeFieldLen;
}

// 貸出を閉じる返却 1 件の、貸出から写す部分だけを作る
ReturnEntry closingEntry(const Loan& loan) {
  ReturnEntry entry;
  std::memcpy(entry.item, loan.item, kCodeFieldLen);
  std::memcpy(entry.borrower, loan.user, kCodeFieldLen);
  std::memcpy(entry.checkout_photo, loan.checkout_photo, kPhotoFieldLen);
  entry.checkout_epoch = loan.checkout_epoch;
  return entry;
}

// entry がこの貸出を閉じた返却か (同じ物品を後でまた貸した分とは、借りた人・持出時刻・写真で見分ける)
bool closesLoan(const ReturnEntry& entry, const Loan& loan) {
  return std::strcmp(entry.item, loan.item) == 0 && std::strcmp(entry.borrower, loan.user) == 0 &&
         entry.checkout_epoch == loan.checkout_epoch && std::strcmp(entry.checkout_photo, loan.checkout_photo) == 0;
}

struct KeyName {
  char text[kCodeFieldLen];
};

struct KeyCollector {
  std::vector<KeyName> keys;
  uint16_t too_long = 0;
};

bool collectKey(const char* key, void* ctx) {
  auto* collector = static_cast<KeyCollector*>(ctx);
  KeyName name{};
  if (setField(name.text, kCodeFieldLen, key)) {
    collector->keys.push_back(name);
  } else {
    ++collector->too_long;
  }
  return true;
}

bool checkoutOrder(const Loan* a, const Loan* b) {
  if (a->order != b->order) return a->order < b->order;
  return std::strcmp(a->item, b->item) < 0;
}

// user が nullptr なら全員分。貸し出した順に並べて cap まで写す
size_t copyInCheckoutOrder(const std::vector<Loan>& loans, const char* user, Loan* out, size_t cap) {
  std::vector<const Loan*> sorted;
  for (const Loan& loan : loans) {
    if (user == nullptr || std::strcmp(loan.user, user) == 0) sorted.push_back(&loan);
  }
  std::sort(sorted.begin(), sorted.end(), checkoutOrder);
  const size_t n = std::min(sorted.size(), cap);
  for (size_t i = 0; i < n; ++i) out[i] = *sorted[i];
  return n;
}

}  // namespace

LoanBook::LoanBook(KvStore& kv)
    : kv_(kv), hist_(kv, kHistNs, kHistPrefix, kReturnHistoryCapacity, kReturnRecordSize) {}

LoadReport LoanBook::load() {
  LoadReport report;
  loans_.clear();
  returns_.clear();
  next_order_ = 1;
  pending_remove_[0] = '\0';

  // 返却履歴 (新しい順)。読めないスロットも位置を保つために入れておく
  report.history_ok = hist_.load();
  returns_.reserve(hist_.count());
  uint8_t record[kReturnRecordSize];
  for (uint16_t i = 0; i < hist_.count(); ++i) {
    CachedReturn cached;
    cached.valid = hist_.getNewest(i, record) && decodeReturn(record, &cached.entry);
    returns_.push_back(cached);
  }

  // 貸出中。列挙しながら読まず、キーを集めてから読む
  KeyCollector collector;
  report.loans_ok = kv_.forEachKey(kLoansNs, collectKey, &collector);
  report.skipped_loans = collector.too_long;
  uint8_t blob[kLoanRecordSize];
  for (const KeyName& key : collector.keys) {
    Loan loan;
    if (!kv_.getBlob(kLoansNs, key.text, blob, sizeof(blob)) || !decodeLoan(blob, &loan) ||
        std::strcmp(loan.item, key.text) != 0) {
      ++report.skipped_loans;
      continue;
    }
    loans_.push_back(loan);
    if (loan.order >= next_order_) next_order_ = loan.order + 1;
  }

  // 閉じる途中で電源が落ちた貸出: 最新の返却履歴が閉じた貸出がまだ残っていれば一覧から外す (キーは次の変更の直前に消す)
  if (!returns_.empty() && returns_[0].valid) {
    const int found = findLoanIndex(returns_[0].entry.item);
    if (found >= 0 && closesLoan(returns_[0].entry, loans_[static_cast<size_t>(found)])) {
      std::memcpy(pending_remove_, returns_[0].entry.item, kCodeFieldLen);
      loans_.erase(loans_.begin() + static_cast<std::ptrdiff_t>(found));
      report.repair_pending = true;
    }
  }
  return report;
}

uint16_t LoanBook::loanCount() const { return static_cast<uint16_t>(loans_.size()); }

bool LoanBook::findLoan(const char* item, Loan* out) const {
  const int found = findLoanIndex(item);
  if (found < 0) return false;
  if (out != nullptr) *out = loans_[static_cast<size_t>(found)];
  return true;
}

size_t LoanBook::listLoans(Loan* out, size_t cap) const {
  if (out == nullptr) return 0;
  return copyInCheckoutOrder(loans_, nullptr, out, cap);
}

size_t LoanBook::listLoansOf(const char* user, Loan* out, size_t cap) const {
  // 「不明」の貸出を誰かの一覧に出さない
  if (out == nullptr || user == nullptr || user[0] == '\0') return 0;
  return copyInCheckoutOrder(loans_, user, out, cap);
}

uint16_t LoanBook::returnCount() const { return static_cast<uint16_t>(returns_.size()); }

bool LoanBook::getReturn(uint16_t index, ReturnEntry* out) const {
  if (index >= returns_.size() || !returns_[index].valid) return false;
  if (out != nullptr) *out = returns_[index].entry;
  return true;
}

bool LoanBook::findLastReturn(const char* item, ReturnEntry* out) const {
  if (item == nullptr) return false;
  for (const CachedReturn& cached : returns_) {
    if (cached.valid && std::strcmp(cached.entry.item, item) == 0) {
      if (out != nullptr) *out = cached.entry;
      return true;
    }
  }
  return false;
}

size_t LoanBook::searchItems(const char* prefix, ItemSearchHit* out, size_t cap) const {
  const char* p = (prefix == nullptr) ? "" : prefix;
  const size_t len = std::strlen(p);
  std::vector<const char*> names;
  for (const Loan& loan : loans_) {
    if (std::strncmp(loan.item, p, len) == 0) names.push_back(loan.item);
  }
  for (const CachedReturn& cached : returns_) {
    if (cached.valid && std::strncmp(cached.entry.item, p, len) == 0) names.push_back(cached.entry.item);
  }
  std::sort(names.begin(), names.end(), [](const char* a, const char* b) { return std::strcmp(a, b) < 0; });
  names.erase(std::unique(names.begin(), names.end(), [](const char* a, const char* b) { return std::strcmp(a, b) == 0; }),
              names.end());

  if (out != nullptr) {
    const size_t n = std::min(names.size(), cap);
    for (size_t i = 0; i < n; ++i) {
      ItemSearchHit hit;
      std::memcpy(hit.item, names[i], kCodeFieldLen);
      hit.lent = findLoan(hit.item, &hit.loan);
      hit.has_return = findLastReturn(hit.item, &hit.last_return);
      out[i] = hit;
    }
  }
  return names.size();
}

BookResult LoanBook::checkout(const char* item, const char* user, int64_t epoch, const char* photo, bool switched) {
  Loan loan;
  if (!validItem(item) || !setField(loan.item, kCodeFieldLen, item) || !setField(loan.user, kCodeFieldLen, user) ||
      !setField(loan.checkout_photo, kPhotoFieldLen, photo)) {
    return BookResult::InvalidArg;
  }
  if (findLoanIndex(item) >= 0) return BookResult::AlreadyLent;
  if (loans_.size() >= kMaxLoans) return BookResult::Full;
  if (!flushPendingRemove()) return BookResult::StorageError;

  loan.checkout_epoch = epoch;
  loan.order = next_order_;
  loan.flags = static_cast<uint8_t>(((loan.user[0] == '\0') ? kLoanUserUnknown : 0) | (switched ? kLoanSwitched : 0));
  if (!putLoan(loan)) return BookResult::StorageError;
  loans_.push_back(loan);
  ++next_order_;
  return BookResult::Ok;
}

BookResult LoanBook::returnItem(const char* item, const char* returner, int64_t epoch, const char* photo) {
  ReturnEntry args;
  if (!validItem(item) || !setField(args.returner, kCodeFieldLen, returner) ||
      !setField(args.return_photo, kPhotoFieldLen, photo)) {
    return BookResult::InvalidArg;
  }
  const int found = findLoanIndex(item);
  if (found < 0) return BookResult::NotLent;
  if (!flushPendingRemove()) return BookResult::StorageError;
  const size_t index = static_cast<size_t>(found);

  // ① 返却履歴に足す
  ReturnEntry closing = closingEntry(loans_[index]);
  std::memcpy(closing.returner, args.returner, kCodeFieldLen);
  std::memcpy(closing.return_photo, args.return_photo, kPhotoFieldLen);
  closing.return_epoch = epoch;
  if (closing.returner[0] == '\0') {
    closing.flags = kReturnReturnerUnknown;
  } else if (closing.borrower[0] != '\0' && std::strcmp(closing.borrower, closing.returner) != 0) {
    closing.flags = kReturnProxy;
  }
  if (!appendReturn(closing)) return BookResult::StorageError;

  // ② 貸出のキーを消す
  dropLoanAt(index);
  return BookResult::Ok;
}

BookResult LoanBook::switchLoan(const char* item, const char* new_user, int64_t epoch, const char* photo) {
  Loan next;
  if (!validItem(item) || !setField(next.item, kCodeFieldLen, item) || !setField(next.user, kCodeFieldLen, new_user) ||
      !setField(next.checkout_photo, kPhotoFieldLen, photo)) {
    return BookResult::InvalidArg;
  }
  const int found = findLoanIndex(item);
  if (found < 0) return BookResult::NotLent;
  if (!flushPendingRemove()) return BookResult::StorageError;
  const size_t index = static_cast<size_t>(found);

  // ① 今の貸出を返却漏れとして閉じる
  ReturnEntry closing = closingEntry(loans_[index]);
  std::memcpy(closing.return_photo, next.checkout_photo, kPhotoFieldLen);
  closing.return_epoch = epoch;
  closing.flags = static_cast<uint8_t>(kReturnMissed | kReturnReturnerUnknown);
  if (!appendReturn(closing)) return BookResult::StorageError;

  // ② 同じキーに新しい貸出を上書きする (古い貸出が消えるのと新しい貸出が載るのが 1 回の書き込み)
  next.checkout_epoch = epoch;
  next.order = next_order_;
  next.flags = static_cast<uint8_t>(kLoanSwitched | ((next.user[0] == '\0') ? kLoanUserUnknown : 0));
  if (!putLoan(next)) {
    dropLoanAt(index);  // 元の貸出は閉じ終わっている。新しい貸出は書けなかった
    return BookResult::StorageError;
  }
  loans_[index] = next;
  ++next_order_;
  return BookResult::Ok;
}

BookResult LoanBook::cancelLoan(const char* item, int64_t epoch) {
  if (!validItem(item)) return BookResult::InvalidArg;
  const int found = findLoanIndex(item);
  if (found < 0) return BookResult::NotLent;
  if (!flushPendingRemove()) return BookResult::StorageError;
  const size_t index = static_cast<size_t>(found);

  ReturnEntry closing = closingEntry(loans_[index]);
  closing.return_epoch = epoch;
  closing.flags = kReturnCancelled;
  if (!appendReturn(closing)) return BookResult::StorageError;
  dropLoanAt(index);
  return BookResult::Ok;
}

bool LoanBook::clear() {
  const bool loans_cleared = kv_.clearNamespace(kLoansNs);
  const bool hist_cleared = hist_.clear();
  load();
  return loans_cleared && hist_cleared;
}

int LoanBook::findLoanIndex(const char* item) const {
  if (item == nullptr) return -1;
  for (size_t i = 0; i < loans_.size(); ++i) {
    if (std::strcmp(loans_[i].item, item) == 0) return static_cast<int>(i);
  }
  return -1;
}

bool LoanBook::appendReturn(const ReturnEntry& entry) {
  uint8_t record[kReturnRecordSize];
  encodeReturn(entry, record);
  if (!hist_.append(record)) return false;
  CachedReturn cached;
  cached.valid = true;
  cached.entry = entry;
  returns_.insert(returns_.begin(), cached);
  while (returns_.size() > hist_.count()) returns_.pop_back();
  return true;
}

bool LoanBook::putLoan(const Loan& loan) {
  uint8_t blob[kLoanRecordSize];
  encodeLoan(loan, blob);
  return kv_.putBlob(kLoansNs, loan.item, blob, sizeof(blob));
}

void LoanBook::dropLoanAt(size_t index) {
  std::memcpy(pending_remove_, loans_[index].item, kCodeFieldLen);
  loans_.erase(loans_.begin() + static_cast<std::ptrdiff_t>(index));
  // 消せなければ pending_remove_ に残り、次の変更の直前にもう一度消す
  (void)flushPendingRemove();
}

bool LoanBook::flushPendingRemove() {
  if (pending_remove_[0] == '\0') return true;
  if (!kv_.remove(kLoansNs, pending_remove_)) return false;
  pending_remove_[0] = '\0';
  return true;
}

}  // namespace toolcheck
