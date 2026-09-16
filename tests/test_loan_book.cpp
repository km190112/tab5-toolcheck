// loan_book: 貸出中と返却履歴、電源断の整合、検索 (docs/設計.md「セッション」「永続化 (NVS)」「画面」の検索)
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/loan_book.h"
#include "memory_kv_store.h"
#include "testing.h"

using toolcheck::BookResult;
using toolcheck::ItemSearchHit;
using toolcheck::LoadReport;
using toolcheck::Loan;
using toolcheck::LoanBook;
using toolcheck::ReturnEntry;

namespace {

const int64_t kT0 = 1789174800;  // 2026-09-12T01:00:00Z (10:00 JST)
const char* const kUserA = "1234567";
const char* const kUserB = "7654321";
const char* const kUserC = "1111111";

std::string str(const char* s) { return std::string(s); }

std::string at(const std::vector<uint8_t>& blob, size_t offset) {
  if (offset >= blob.size()) return "<out>";
  return std::string(reinterpret_cast<const char*>(&blob[offset]));
}

// 貸出中の物品番号を一覧の順に "A,B,C" でつなぐ
std::string loanItems(const LoanBook& book) {
  std::vector<Loan> out(toolcheck::kMaxLoans + 1);
  const size_t n = book.listLoans(out.data(), out.size());
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    if (i > 0) s += ",";
    s += out[i].item;
  }
  return s;
}

}  // namespace

TEST(loan_empty_store_loads_empty) {
  MemoryKvStore kv;
  LoanBook book(kv);
  const LoadReport r = book.load();
  CHECK(r.loans_ok);
  CHECK(r.history_ok);
  CHECK_EQ(r.skipped_loans, uint16_t{0});
  CHECK(!r.repair_pending);
  CHECK_EQ(book.loanCount(), uint16_t{0});
  CHECK_EQ(book.returnCount(), uint16_t{0});
  Loan l;
  CHECK(!book.findLoan("TW-025", &l));
  CHECK_EQ(kv.writes(), 0);
}

// --- 持出 ---

TEST(loan_checkout_records_fields) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  CHECK_EQ(book.checkout("TW-025", kUserA, kT0, "20260912-100000.jpg"), BookResult::Ok);
  CHECK_EQ(book.loanCount(), uint16_t{1});
  Loan l;
  CHECK(book.findLoan("TW-025", &l));
  CHECK_EQ(str(l.item), str("TW-025"));
  CHECK_EQ(str(l.user), str(kUserA));
  CHECK_EQ(l.checkout_epoch, kT0);
  CHECK_EQ(str(l.checkout_photo), str("20260912-100000.jpg"));
  CHECK_EQ(l.flags, uint8_t{0});
  CHECK(kv.hasKey("loans", "TW-025"));
}

TEST(loan_checkout_persists_across_instances) {
  MemoryKvStore kv;
  {
    LoanBook book(kv);
    book.load();
    book.checkout("TW-025", kUserA, kT0, "20260912-100000.jpg");
    book.checkout("MM-100", nullptr, kT0 + 60, nullptr);
  }
  LoanBook again(kv);
  const LoadReport r = again.load();
  CHECK_EQ(r.skipped_loans, uint16_t{0});
  CHECK_EQ(again.loanCount(), uint16_t{2});
  Loan l;
  CHECK(again.findLoan("TW-025", &l));
  CHECK_EQ(str(l.user), str(kUserA));
  CHECK_EQ(l.checkout_epoch, kT0);
  CHECK_EQ(str(l.checkout_photo), str("20260912-100000.jpg"));
  CHECK(again.findLoan("MM-100", &l));
  CHECK_EQ(str(l.user), str(""));
  CHECK_EQ(l.flags, toolcheck::kLoanUserUnknown);
  CHECK_EQ(loanItems(again), str("TW-025,MM-100"));
}

TEST(loan_checkout_empty_user_is_unknown) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  CHECK_EQ(book.checkout("MM-100", "", kT0, ""), BookResult::Ok);
  Loan l;
  CHECK(book.findLoan("MM-100", &l));
  CHECK_EQ(str(l.user), str(""));
  CHECK_EQ(str(l.checkout_photo), str(""));
  CHECK_EQ(l.flags, toolcheck::kLoanUserUnknown);
}

// 2026-09-15: 返却は読んだ時点で書くので、「持出に切替」は返却の後の持出として書き、貸出に切替の印を付ける
TEST(loan_checkout_marked_as_switched) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  CHECK_EQ(book.checkout("SW-1", kUserA, kT0, "p.jpg", true), BookResult::Ok);
  CHECK_EQ(book.checkout("SW-2", nullptr, kT0 + 60, nullptr, true), BookResult::Ok);
  LoanBook again(kv);
  again.load();
  Loan l;
  CHECK(again.findLoan("SW-1", &l));
  CHECK_EQ(l.flags, toolcheck::kLoanSwitched);
  CHECK(again.findLoan("SW-2", &l));
  CHECK_EQ(l.flags, static_cast<uint8_t>(toolcheck::kLoanSwitched | toolcheck::kLoanUserUnknown));
}

TEST(loan_checkout_already_lent_is_rejected) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  CHECK_EQ(book.checkout("TW-025", kUserA, kT0, nullptr), BookResult::Ok);
  const int before = kv.writes();
  CHECK_EQ(book.checkout("TW-025", kUserB, kT0 + 60, nullptr), BookResult::AlreadyLent);
  CHECK_EQ(kv.writes(), before);
  Loan l;
  CHECK(book.findLoan("TW-025", &l));
  CHECK_EQ(str(l.user), str(kUserA));
  CHECK_EQ(l.checkout_epoch, kT0);
}

TEST(loan_checkout_rejects_invalid_arguments) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  const std::string photo32(32, 'p');
  CHECK_EQ(book.checkout(nullptr, kUserA, kT0, nullptr), BookResult::InvalidArg);
  CHECK_EQ(book.checkout("", kUserA, kT0, nullptr), BookResult::InvalidArg);
  CHECK_EQ(book.checkout("ABCDEFGHIJKLMNOP", kUserA, kT0, nullptr), BookResult::InvalidArg);  // 16 文字
  CHECK_EQ(book.checkout("TW-025", "1234567890123456", kT0, nullptr), BookResult::InvalidArg);  // 16 桁
  CHECK_EQ(book.checkout("TW-025", kUserA, kT0, photo32.c_str()), BookResult::InvalidArg);
  CHECK_EQ(book.loanCount(), uint16_t{0});
  CHECK_EQ(kv.writes(), 0);

  // 端は通り、読み直しても欠けない
  const std::string photo31(31, 'p');
  CHECK_EQ(book.checkout("ABCDEFGHIJKLMNO", "123456789012345", kT0, photo31.c_str()), BookResult::Ok);
  LoanBook again(kv);
  again.load();
  Loan l;
  CHECK(again.findLoan("ABCDEFGHIJKLMNO", &l));
  CHECK_EQ(str(l.user), str("123456789012345"));
  CHECK_EQ(str(l.checkout_photo), photo31);
}

TEST(loan_checkout_full_at_max) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  char item[8];
  for (int i = 0; i < toolcheck::kMaxLoans; ++i) {
    std::snprintf(item, sizeof(item), "IT%03d", i);
    CHECK_EQ(book.checkout(item, kUserA, kT0 + i, nullptr), BookResult::Ok);
  }
  CHECK_EQ(book.loanCount(), toolcheck::kMaxLoans);
  CHECK_EQ(book.checkout("IT999", kUserA, kT0, nullptr), BookResult::Full);
  CHECK_EQ(book.checkout("IT000", kUserB, kT0, nullptr), BookResult::AlreadyLent);
}

// --- 一覧 ---

TEST(loan_list_is_checkout_order) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  book.checkout("C-3", kUserA, kT0, nullptr);
  book.checkout("A-1", kUserB, kT0 + 60, nullptr);
  book.checkout("B-2", kUserA, kT0 + 120, nullptr);
  CHECK_EQ(loanItems(book), str("C-3,A-1,B-2"));

  Loan two[2];
  CHECK_EQ(book.listLoans(two, 2), size_t{2});
  CHECK_EQ(str(two[0].item), str("C-3"));
  CHECK_EQ(str(two[1].item), str("A-1"));

  CHECK_EQ(book.returnItem("A-1", kUserB, kT0 + 180, nullptr), BookResult::Ok);
  CHECK_EQ(book.checkout("D-4", kUserB, kT0 + 240, nullptr), BookResult::Ok);
  CHECK_EQ(loanItems(book), str("C-3,B-2,D-4"));

  LoanBook again(kv);
  again.load();
  CHECK_EQ(loanItems(again), str("C-3,B-2,D-4"));
  // 返してから同じ物をまた借りると一番新しくなる
  CHECK_EQ(again.returnItem("C-3", kUserA, kT0 + 300, nullptr), BookResult::Ok);
  CHECK_EQ(again.checkout("C-3", kUserA, kT0 + 360, nullptr), BookResult::Ok);
  CHECK_EQ(loanItems(again), str("B-2,D-4,C-3"));
}

TEST(loan_list_order_ignores_clock) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  book.checkout("X-1", kUserA, 2462227200, nullptr);  // 時刻未設定の RTC が返す未来の値
  book.checkout("Y-2", kUserA, kT0, nullptr);
  CHECK_EQ(loanItems(book), str("X-1,Y-2"));
}

TEST(loan_list_of_user) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  book.checkout("A-1", kUserA, kT0, nullptr);
  book.checkout("B-2", kUserB, kT0 + 60, nullptr);
  book.checkout("C-3", kUserA, kT0 + 120, nullptr);
  book.checkout("D-4", nullptr, kT0 + 180, nullptr);
  Loan out[8];
  CHECK_EQ(book.listLoansOf(kUserA, out, 8), size_t{2});
  CHECK_EQ(str(out[0].item), str("A-1"));
  CHECK_EQ(str(out[1].item), str("C-3"));
  CHECK_EQ(book.listLoansOf(kUserA, out, 1), size_t{1});
  CHECK_EQ(book.listLoansOf("9999999", out, 8), size_t{0});
  // 「不明」の貸出は誰の名札でも出さない
  CHECK_EQ(book.listLoansOf("", out, 8), size_t{0});
  CHECK_EQ(book.listLoansOf(nullptr, out, 8), size_t{0});
}

// --- 返却 ---

TEST(loan_return_moves_loan_to_history) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  book.checkout("TW-025", kUserA, kT0, "20260912-100000.jpg");
  CHECK_EQ(book.returnItem("TW-025", kUserA, kT0 + 3600, "20260912-110000.jpg"), BookResult::Ok);
  CHECK_EQ(book.loanCount(), uint16_t{0});
  CHECK(!kv.hasKey("loans", "TW-025"));
  CHECK_EQ(book.returnCount(), uint16_t{1});
  ReturnEntry e;
  CHECK(book.getReturn(0, &e));
  CHECK_EQ(str(e.item), str("TW-025"));
  CHECK_EQ(str(e.borrower), str(kUserA));
  CHECK_EQ(str(e.returner), str(kUserA));
  CHECK_EQ(e.checkout_epoch, kT0);
  CHECK_EQ(e.return_epoch, kT0 + 3600);
  CHECK_EQ(str(e.checkout_photo), str("20260912-100000.jpg"));
  CHECK_EQ(str(e.return_photo), str("20260912-110000.jpg"));
  CHECK_EQ(e.flags, uint8_t{0});
  CHECK(!book.getReturn(1, &e));

  LoanBook again(kv);
  const LoadReport r = again.load();
  CHECK(!r.repair_pending);
  CHECK_EQ(again.loanCount(), uint16_t{0});
  CHECK_EQ(again.returnCount(), uint16_t{1});
  ReturnEntry e2;
  CHECK(again.getReturn(0, &e2));
  CHECK_EQ(str(e2.item), str("TW-025"));
  CHECK_EQ(str(e2.returner), str(kUserA));
  CHECK_EQ(str(e2.return_photo), str("20260912-110000.jpg"));
}

TEST(loan_return_by_other_user_is_proxy) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  book.checkout("TW-025", kUserA, kT0, nullptr);
  CHECK_EQ(book.returnItem("TW-025", kUserB, kT0 + 60, nullptr), BookResult::Ok);
  ReturnEntry e;
  CHECK(book.getReturn(0, &e));
  CHECK_EQ(str(e.borrower), str(kUserA));
  CHECK_EQ(str(e.returner), str(kUserB));
  CHECK_EQ(e.flags, toolcheck::kReturnProxy);
}

TEST(loan_return_without_returner_is_unknown) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  book.checkout("TW-025", kUserA, kT0, nullptr);
  CHECK_EQ(book.returnItem("TW-025", nullptr, kT0 + 60, nullptr), BookResult::Ok);
  ReturnEntry e;
  CHECK(book.getReturn(0, &e));
  CHECK_EQ(str(e.returner), str(""));
  CHECK_EQ(e.flags, toolcheck::kReturnReturnerUnknown);
}

TEST(loan_return_of_unknown_borrower_is_not_proxy) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  book.checkout("TW-025", nullptr, kT0, nullptr);
  CHECK_EQ(book.returnItem("TW-025", kUserB, kT0 + 60, nullptr), BookResult::Ok);
  ReturnEntry e;
  CHECK(book.getReturn(0, &e));
  CHECK_EQ(str(e.borrower), str(""));
  CHECK_EQ(str(e.returner), str(kUserB));
  CHECK_EQ(e.flags, uint8_t{0});
}

TEST(loan_return_not_lent_changes_nothing) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  CHECK_EQ(book.returnItem("TW-025", kUserA, kT0, nullptr), BookResult::NotLent);
  CHECK_EQ(kv.writes(), 0);
  CHECK_EQ(book.returnCount(), uint16_t{0});
  book.checkout("TW-025", kUserA, kT0, nullptr);
  CHECK_EQ(book.returnItem("TW-025", kUserA, kT0 + 60, nullptr), BookResult::Ok);
  CHECK_EQ(book.returnItem("TW-025", kUserA, kT0 + 120, nullptr), BookResult::NotLent);
  CHECK_EQ(book.returnCount(), uint16_t{1});
}

TEST(loan_return_rejects_invalid_arguments) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  book.checkout("TW-025", kUserA, kT0, nullptr);
  const int before = kv.writes();
  const std::string photo32(32, 'p');
  CHECK_EQ(book.returnItem("TW-025", "1234567890123456", kT0, nullptr), BookResult::InvalidArg);
  CHECK_EQ(book.returnItem("TW-025", kUserA, kT0, photo32.c_str()), BookResult::InvalidArg);
  CHECK_EQ(book.returnItem("ABCDEFGHIJKLMNOP", kUserA, kT0, nullptr), BookResult::InvalidArg);
  CHECK_EQ(book.returnItem(nullptr, kUserA, kT0, nullptr), BookResult::InvalidArg);
  CHECK_EQ(kv.writes(), before);
  CHECK_EQ(book.loanCount(), uint16_t{1});
}

// --- 持出に切替・取消 ---

TEST(loan_switch_closes_as_missed_and_lends_to_new_user) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  book.checkout("TW-025", kUserA, kT0, "a.jpg");
  CHECK_EQ(book.switchLoan("TW-025", kUserB, kT0 + 7200, "b.jpg"), BookResult::Ok);
  CHECK_EQ(book.loanCount(), uint16_t{1});
  Loan l;
  CHECK(book.findLoan("TW-025", &l));
  CHECK_EQ(str(l.user), str(kUserB));
  CHECK_EQ(l.checkout_epoch, kT0 + 7200);
  CHECK_EQ(str(l.checkout_photo), str("b.jpg"));
  CHECK_EQ(l.flags, toolcheck::kLoanSwitched);

  const uint8_t missed = static_cast<uint8_t>(toolcheck::kReturnMissed | toolcheck::kReturnReturnerUnknown);
  ReturnEntry e;
  CHECK(book.getReturn(0, &e));
  CHECK_EQ(str(e.borrower), str(kUserA));
  CHECK_EQ(str(e.returner), str(""));
  CHECK_EQ(e.checkout_epoch, kT0);
  CHECK_EQ(e.return_epoch, kT0 + 7200);
  CHECK_EQ(str(e.checkout_photo), str("a.jpg"));
  CHECK_EQ(str(e.return_photo), str("b.jpg"));
  CHECK_EQ(e.flags, missed);

  LoanBook again(kv);
  const LoadReport r = again.load();
  CHECK(!r.repair_pending);
  Loan l2;
  CHECK(again.findLoan("TW-025", &l2));
  CHECK_EQ(str(l2.user), str(kUserB));
  CHECK_EQ(l2.flags, toolcheck::kLoanSwitched);
  CHECK_EQ(again.returnCount(), uint16_t{1});
}

TEST(loan_switch_to_unknown_user) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  book.checkout("TW-025", kUserA, kT0, nullptr);
  CHECK_EQ(book.switchLoan("TW-025", nullptr, kT0 + 60, nullptr), BookResult::Ok);
  Loan l;
  CHECK(book.findLoan("TW-025", &l));
  CHECK_EQ(str(l.user), str(""));
  CHECK_EQ(l.flags, static_cast<uint8_t>(toolcheck::kLoanSwitched | toolcheck::kLoanUserUnknown));
}

TEST(loan_switch_moves_loan_to_newest) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  book.checkout("A-1", kUserA, kT0, nullptr);
  book.checkout("B-2", kUserA, kT0 + 60, nullptr);
  CHECK_EQ(book.switchLoan("A-1", kUserB, kT0 + 120, nullptr), BookResult::Ok);
  CHECK_EQ(loanItems(book), str("B-2,A-1"));
}

TEST(loan_switch_not_lent_changes_nothing) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  CHECK_EQ(book.switchLoan("TW-025", kUserB, kT0, nullptr), BookResult::NotLent);
  CHECK_EQ(kv.writes(), 0);
  CHECK_EQ(book.loanCount(), uint16_t{0});
}

TEST(loan_cancel_removes_loan_and_logs_cancel) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  book.checkout("TW-025", kUserA, kT0, "a.jpg");
  CHECK_EQ(book.cancelLoan("TW-025", kT0 + 60), BookResult::Ok);
  CHECK_EQ(book.loanCount(), uint16_t{0});
  CHECK(!kv.hasKey("loans", "TW-025"));
  ReturnEntry e;
  CHECK(book.getReturn(0, &e));
  CHECK_EQ(str(e.item), str("TW-025"));
  CHECK_EQ(str(e.borrower), str(kUserA));
  CHECK_EQ(str(e.returner), str(""));
  CHECK_EQ(e.return_epoch, kT0 + 60);
  CHECK_EQ(str(e.checkout_photo), str("a.jpg"));
  CHECK_EQ(str(e.return_photo), str(""));
  CHECK_EQ(e.flags, toolcheck::kReturnCancelled);
  CHECK_EQ(book.cancelLoan("TW-025", kT0 + 120), BookResult::NotLent);
}

// --- 返却履歴 ---

TEST(loan_history_keeps_newest_100) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  char item[8];
  for (int i = 0; i < 105; ++i) {
    std::snprintf(item, sizeof(item), "IT%03d", i);
    CHECK_EQ(book.checkout(item, kUserA, kT0 + i * 60, nullptr), BookResult::Ok);
    CHECK_EQ(book.returnItem(item, kUserA, kT0 + i * 60 + 30, nullptr), BookResult::Ok);
  }
  CHECK_EQ(book.returnCount(), toolcheck::kReturnHistoryCapacity);
  ReturnEntry e;
  CHECK(book.getReturn(0, &e));
  CHECK_EQ(str(e.item), str("IT104"));
  CHECK(book.getReturn(99, &e));
  CHECK_EQ(str(e.item), str("IT005"));
  CHECK(!book.getReturn(100, &e));

  LoanBook again(kv);
  again.load();
  CHECK_EQ(again.returnCount(), toolcheck::kReturnHistoryCapacity);
  ReturnEntry e2;
  CHECK(again.getReturn(0, &e2));
  CHECK_EQ(str(e2.item), str("IT104"));
  CHECK(again.getReturn(99, &e2));
  CHECK_EQ(str(e2.item), str("IT005"));
}

TEST(loan_find_last_return_of_item) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  book.checkout("X-1", kUserA, kT0, nullptr);
  book.returnItem("X-1", kUserA, kT0 + 60, nullptr);
  book.checkout("Y-2", kUserB, kT0 + 120, nullptr);
  book.returnItem("Y-2", kUserB, kT0 + 180, nullptr);
  book.checkout("X-1", kUserB, kT0 + 240, nullptr);
  book.returnItem("X-1", kUserC, kT0 + 300, nullptr);
  ReturnEntry e;
  CHECK(book.findLastReturn("X-1", &e));
  CHECK_EQ(str(e.returner), str(kUserC));
  CHECK_EQ(e.return_epoch, kT0 + 300);
  CHECK(!book.findLastReturn("Z-9", &e));
}

// --- 検索 ---

TEST(loan_search_by_prefix) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  book.checkout("TW-002", kUserA, kT0, nullptr);
  book.returnItem("TW-002", kUserB, kT0 + 60, nullptr);
  book.checkout("AB-1", kUserA, kT0 + 120, nullptr);
  book.returnItem("AB-1", kUserA, kT0 + 180, nullptr);
  book.checkout("TW-001", kUserB, kT0 + 240, nullptr);
  book.checkout("TW-010", kUserA, kT0 + 300, nullptr);
  book.returnItem("TW-010", kUserA, kT0 + 360, nullptr);
  book.checkout("TW-010", kUserB, kT0 + 420, nullptr);
  book.checkout("TW-002", kUserC, kT0 + 480, nullptr);
  book.returnItem("TW-002", kUserC, kT0 + 540, nullptr);

  ItemSearchHit hits[8];
  CHECK_EQ(book.searchItems("TW", hits, 8), size_t{3});
  CHECK_EQ(str(hits[0].item), str("TW-001"));
  CHECK(hits[0].lent);
  CHECK_EQ(str(hits[0].loan.user), str(kUserB));
  CHECK(!hits[0].has_return);
  CHECK_EQ(str(hits[1].item), str("TW-002"));  // 返却履歴に 2 件あっても 1 件にまとめ、最新を返す
  CHECK(!hits[1].lent);
  CHECK(hits[1].has_return);
  CHECK_EQ(str(hits[1].last_return.returner), str(kUserC));
  CHECK_EQ(hits[1].last_return.return_epoch, kT0 + 540);
  CHECK_EQ(str(hits[2].item), str("TW-010"));  // 貸出中で、返却履歴にもある
  CHECK(hits[2].lent);
  CHECK_EQ(str(hits[2].loan.user), str(kUserB));
  CHECK(hits[2].has_return);
  CHECK_EQ(str(hits[2].last_return.returner), str(kUserA));

  CHECK_EQ(book.searchItems("tw", hits, 8), size_t{0});  // 大文字・小文字は区別する
  CHECK_EQ(book.searchItems("ZZ", hits, 8), size_t{0});
  CHECK_EQ(book.searchItems("TW-00", hits, 8), size_t{2});
  CHECK_EQ(book.searchItems("", hits, 8), size_t{4});
  CHECK_EQ(str(hits[0].item), str("AB-1"));
  CHECK_EQ(book.searchItems(nullptr, hits, 8), size_t{4});

  ItemSearchHit one[1];
  CHECK_EQ(book.searchItems("TW", one, 1), size_t{3});  // 入れるのは cap まで、数は全部
  CHECK_EQ(str(one[0].item), str("TW-001"));
}

// --- 電源断 ---

TEST(loan_power_loss_before_history_keeps_loan) {
  MemoryKvStore kv;
  {
    LoanBook book(kv);
    book.load();
    book.checkout("TW-025", kUserA, kT0, nullptr);
    kv.fail_after_puts = kv.puts;  // 返却の最初の書き込みから落ちる
    CHECK_EQ(book.returnItem("TW-025", kUserA, kT0 + 60, nullptr), BookResult::StorageError);
    CHECK_EQ(book.loanCount(), uint16_t{1});
    CHECK_EQ(book.returnCount(), uint16_t{0});
  }
  kv.fail_after_puts = -1;
  LoanBook again(kv);
  const LoadReport r = again.load();
  CHECK(!r.repair_pending);
  CHECK_EQ(again.loanCount(), uint16_t{1});
  CHECK_EQ(again.returnCount(), uint16_t{0});
}

TEST(loan_power_loss_after_history_slot_counts_return) {
  MemoryKvStore kv;
  {
    LoanBook book(kv);
    book.load();
    book.checkout("TW-025", kUserA, kT0, nullptr);
    kv.fail_after_puts = kv.puts + 1;  // 返却履歴のスロットは書けて、その管理情報の前で落ちる
    CHECK_EQ(book.returnItem("TW-025", kUserA, kT0 + 60, nullptr), BookResult::StorageError);
  }
  kv.fail_after_puts = -1;
  LoanBook again(kv);
  const int before = kv.writes();
  const LoadReport r = again.load();
  CHECK(r.repair_pending);
  CHECK_EQ(kv.writes(), before);  // 読み込みでは書かない
  CHECK_EQ(again.loanCount(), uint16_t{0});
  CHECK_EQ(again.returnCount(), uint16_t{1});
  CHECK(kv.hasKey("loans", "TW-025"));  // キーはまだ残っている
}

TEST(loan_power_loss_before_loan_removal_is_repaired_on_next_write) {
  MemoryKvStore kv;
  {
    LoanBook book(kv);
    book.load();
    book.checkout("TW-025", kUserA, kT0, "a.jpg");
    kv.fail_after_puts = kv.puts + 2;  // 返却履歴 (スロット + 管理情報) は書けて、貸出のキーを消す前に落ちる
    book.returnItem("TW-025", kUserA, kT0 + 60, "b.jpg");  // 実機では戻ってこないので結果は見ない
  }
  kv.fail_after_puts = -1;
  CHECK(kv.hasKey("loans", "TW-025"));

  LoanBook again(kv);
  const int before = kv.writes();
  const LoadReport r = again.load();
  CHECK(r.repair_pending);
  CHECK_EQ(kv.writes(), before);
  CHECK_EQ(again.loanCount(), uint16_t{0});
  CHECK_EQ(again.returnCount(), uint16_t{1});
  ReturnEntry e;
  CHECK(again.getReturn(0, &e));
  CHECK_EQ(str(e.returner), str(kUserA));
  CHECK_EQ(str(e.return_photo), str("b.jpg"));

  // 次の変更の直前に残ったキーを消す
  CHECK_EQ(again.checkout("MM-100", kUserB, kT0 + 120, nullptr), BookResult::Ok);
  CHECK(!kv.hasKey("loans", "TW-025"));
  LoanBook third(kv);
  const LoadReport r3 = third.load();
  CHECK(!r3.repair_pending);
  CHECK_EQ(loanItems(third), str("MM-100"));
}

TEST(loan_power_loss_in_switch_loses_only_new_loan) {
  MemoryKvStore kv;
  {
    LoanBook book(kv);
    book.load();
    book.checkout("TW-025", kUserA, kT0, nullptr);
    kv.fail_after_puts = kv.puts + 2;  // 返却漏れの履歴は書けて、新しい貸出の上書きの前で落ちる
    CHECK_EQ(book.switchLoan("TW-025", kUserB, kT0 + 60, nullptr), BookResult::StorageError);
    Loan l;
    CHECK(!book.findLoan("TW-025", &l));  // 元の貸出は閉じ終わっている
  }
  kv.fail_after_puts = -1;
  LoanBook again(kv);
  const LoadReport r = again.load();
  CHECK(r.repair_pending);
  Loan l;
  CHECK(!again.findLoan("TW-025", &l));
  CHECK_EQ(again.returnCount(), uint16_t{1});
  ReturnEntry e;
  CHECK(again.getReturn(0, &e));
  CHECK_EQ(e.flags, static_cast<uint8_t>(toolcheck::kReturnMissed | toolcheck::kReturnReturnerUnknown));

  // 同じ物品をもう一度持出できる
  CHECK_EQ(again.checkout("TW-025", kUserB, kT0 + 120, nullptr), BookResult::Ok);
  LoanBook third(kv);
  const LoadReport r3 = third.load();
  CHECK(!r3.repair_pending);
  Loan l3;
  CHECK(third.findLoan("TW-025", &l3));
  CHECK_EQ(str(l3.user), str(kUserB));
}

TEST(loan_repair_ignores_newer_loan_of_same_item) {
  MemoryKvStore kv;
  {
    LoanBook book(kv);
    book.load();
    book.checkout("TW-025", kUserA, kT0, nullptr);
    book.returnItem("TW-025", kUserA, kT0 + 60, nullptr);
    book.checkout("TW-025", kUserB, kT0 + 120, nullptr);
  }
  LoanBook again(kv);
  const LoadReport r = again.load();
  CHECK(!r.repair_pending);
  Loan l;
  CHECK(again.findLoan("TW-025", &l));
  CHECK_EQ(str(l.user), str(kUserB));
}

TEST(loan_failed_key_removal_blocks_next_write) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  book.checkout("TW-025", kUserA, kT0, nullptr);
  kv.fail_removes = true;
  CHECK_EQ(book.returnItem("TW-025", kUserA, kT0 + 60, nullptr), BookResult::Ok);  // 返却履歴には残った
  Loan l;
  CHECK(!book.findLoan("TW-025", &l));
  CHECK(kv.hasKey("loans", "TW-025"));

  // 残ったキーを消せないうちは次の変更を書かない
  const int puts = kv.puts;
  CHECK_EQ(book.checkout("MM-100", kUserB, kT0 + 120, nullptr), BookResult::StorageError);
  CHECK_EQ(kv.puts, puts);
  CHECK(!book.findLoan("MM-100", &l));

  kv.fail_removes = false;
  CHECK_EQ(book.checkout("MM-100", kUserB, kT0 + 120, nullptr), BookResult::Ok);
  CHECK(!kv.hasKey("loans", "TW-025"));
  LoanBook again(kv);
  again.load();
  CHECK(!again.findLoan("TW-025", &l));
  CHECK(again.findLoan("MM-100", &l));
}

// --- 壊れたデータ ---

TEST(loan_unreadable_loans_are_skipped_not_deleted) {
  MemoryKvStore kv;
  {
    LoanBook book(kv);
    book.load();
    book.checkout("TW-025", kUserA, kT0, nullptr);
    book.checkout("MM-100", kUserA, kT0 + 60, nullptr);
  }
  auto& loans = kv.data["loans"];
  loans["BAD"] = std::vector<uint8_t>(5, 0xAB);  // 長さが違う
  loans["OTHER"] = loans["TW-025"];              // 中身の物品番号とキーが違う
  std::vector<uint8_t> nonul = loans["TW-025"];  // 物品番号の欄に NUL が無い
  for (size_t i = 2; i < 18 && i < nonul.size(); ++i) nonul[i] = 'X';
  loans["XXXXXXXXXXXXXXX"] = nonul;
  if (!loans["MM-100"].empty()) loans["MM-100"][0] = 99;  // 知らない版数

  LoanBook again(kv);
  const int before = kv.writes();
  const LoadReport r = again.load();
  CHECK(r.loans_ok);
  CHECK_EQ(r.skipped_loans, uint16_t{4});
  CHECK_EQ(again.loanCount(), uint16_t{1});
  Loan l;
  CHECK(again.findLoan("TW-025", &l));
  CHECK_EQ(kv.writes(), before);
  CHECK(kv.hasKey("loans", "BAD"));
  CHECK(kv.hasKey("loans", "OTHER"));
  CHECK(kv.hasKey("loans", "MM-100"));
}

TEST(loan_unreadable_return_is_skipped) {
  MemoryKvStore kv;
  {
    LoanBook book(kv);
    book.load();
    book.checkout("X-1", kUserA, kT0, nullptr);
    book.returnItem("X-1", kUserA, kT0 + 60, nullptr);
    book.checkout("Y-2", kUserA, kT0 + 120, nullptr);
    book.returnItem("Y-2", kUserA, kT0 + 180, nullptr);
  }
  auto& slot = kv.data["hist"]["H01"];  // 最新 (Y-2)。[通し番号 4 バイト][返却 1 件]
  CHECK(slot.size() > 4);
  if (slot.size() > 4) slot[4] = 99;  // 知らない版数

  LoanBook again(kv);
  const LoadReport r = again.load();
  CHECK(r.history_ok);
  CHECK(!r.repair_pending);
  CHECK_EQ(again.returnCount(), uint16_t{2});
  ReturnEntry e;
  CHECK(!again.getReturn(0, &e));
  CHECK(again.getReturn(1, &e));
  CHECK_EQ(str(e.item), str("X-1"));
  CHECK(!again.findLastReturn("Y-2", &e));
  ItemSearchHit hits[4];
  CHECK_EQ(again.searchItems("", hits, 4), size_t{1});
}

TEST(loan_broken_history_meta_is_reported) {
  MemoryKvStore kv;
  {
    LoanBook book(kv);
    book.load();
    book.checkout("X-1", kUserA, kT0, nullptr);
    book.returnItem("X-1", kUserA, kT0 + 60, nullptr);
  }
  auto& meta = kv.data["hist"]["meta"];
  CHECK_EQ(meta.size(), size_t{8});
  if (meta.size() >= 2) {
    meta[0] = 200;  // 次に書くスロット = 200 (容量 100 を超える)
    meta[1] = 0;
  }
  LoanBook again(kv);
  const LoadReport r = again.load();
  CHECK(!r.history_ok);
  CHECK_EQ(again.returnCount(), uint16_t{0});
}

TEST(loan_enumeration_failure_is_reported) {
  MemoryKvStore kv;
  {
    LoanBook book(kv);
    book.load();
    book.checkout("X-1", kUserA, kT0, nullptr);
  }
  kv.fail_enumeration = true;
  LoanBook again(kv);
  const LoadReport r = again.load();
  CHECK(!r.loans_ok);
  CHECK_EQ(again.loanCount(), uint16_t{0});
}

// --- 記録の削除 ---

TEST(loan_clear_erases_loans_and_history_only) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  book.checkout("X-1", kUserA, kT0, nullptr);
  book.returnItem("X-1", kUserA, kT0 + 60, nullptr);
  book.checkout("Y-2", kUserB, kT0 + 120, nullptr);
  kv.data["cfg"]["v"] = std::vector<uint8_t>{1};

  CHECK(book.clear());
  CHECK_EQ(book.loanCount(), uint16_t{0});
  CHECK_EQ(book.returnCount(), uint16_t{0});
  CHECK(!kv.hasKey("loans", "Y-2"));
  CHECK(!kv.hasKey("hist", "meta"));
  CHECK(kv.hasKey("cfg", "v"));

  LoanBook again(kv);
  again.load();
  CHECK_EQ(again.loanCount(), uint16_t{0});
  CHECK_EQ(again.returnCount(), uint16_t{0});

  // 消した後もそのまま使える
  CHECK_EQ(book.checkout("Z-3", kUserA, kT0 + 180, nullptr), BookResult::Ok);
  CHECK_EQ(loanItems(book), str("Z-3"));
}

// --- 保存の形 (変えると現場の記録が読めなくなる。変えるなら版数を上げて移行を書く) ---

TEST(loan_record_layout_is_version_1) {
  MemoryKvStore kv;
  LoanBook book(kv);
  book.load();
  const int64_t checkout = int64_t{0x0102030405060708};
  const int64_t returned = int64_t{0x1112131415161718};
  CHECK_EQ(book.checkout("TW-025", kUserA, checkout, "p.jpg"), BookResult::Ok);

  const std::vector<uint8_t> loan = kv.data["loans"]["TW-025"];
  CHECK_EQ(loan.size(), toolcheck::kLoanRecordSize);
  if (loan.size() == toolcheck::kLoanRecordSize) {
    CHECK_EQ(loan[0], uint8_t{1});  // 版数
    CHECK_EQ(loan[1], uint8_t{0});  // 印
    CHECK_EQ(at(loan, 2), str("TW-025"));
    CHECK_EQ(at(loan, 18), str(kUserA));
    CHECK_EQ(loan[34], uint8_t{0x08});  // 持出時刻 (リトルエンディアン)
    CHECK_EQ(loan[41], uint8_t{0x01});
    CHECK_EQ(loan[42], uint8_t{1});  // 貸し出した順
    CHECK_EQ(at(loan, 46), str("p.jpg"));
    CHECK_EQ(loan[77], uint8_t{0});
  }

  CHECK_EQ(book.returnItem("TW-025", kUserB, returned, "q.jpg"), BookResult::Ok);
  const std::vector<uint8_t> slot = kv.data["hist"]["H00"];
  CHECK_EQ(slot.size(), size_t{4} + toolcheck::kReturnRecordSize);  // [通し番号][返却 1 件]
  if (slot.size() == 4 + toolcheck::kReturnRecordSize) {
    CHECK_EQ(slot[4 + 0], uint8_t{1});
    CHECK_EQ(slot[4 + 1], toolcheck::kReturnProxy);
    CHECK_EQ(at(slot, 4 + 2), str("TW-025"));
    CHECK_EQ(at(slot, 4 + 18), str(kUserA));
    CHECK_EQ(at(slot, 4 + 34), str(kUserB));
    CHECK_EQ(slot[4 + 50], uint8_t{0x08});  // 持出時刻
    CHECK_EQ(slot[4 + 58], uint8_t{0x18});  // 返却時刻
    CHECK_EQ(slot[4 + 65], uint8_t{0x11});
    CHECK_EQ(at(slot, 4 + 66), str("p.jpg"));
    CHECK_EQ(at(slot, 4 + 98), str("q.jpg"));
    CHECK_EQ(slot[4 + 129], uint8_t{0});
  }
}
