// session: 物品 → 名札の確定、名札先の 20 秒、警告と打ち切り、一時停止 (docs/設計.md「セッション」)
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/code_classifier.h"
#include "core/config.h"
#include "core/loan_book.h"
#include "core/session.h"
#include "memory_kv_store.h"
#include "testing.h"

using toolcheck::BookResult;
using toolcheck::Config;
using toolcheck::Loan;
using toolcheck::LoanBook;
using toolcheck::ReturnEntry;
using toolcheck::RowKind;
using toolcheck::RowStatus;
using toolcheck::Session;
using toolcheck::SessionEnd;
using toolcheck::SessionOutput;
using toolcheck::SessionRecord;
using toolcheck::SessionRow;
using toolcheck::SessionState;

namespace {

const int64_t kE0 = 1789174800;  // 2026-09-12T01:00:00Z
const uint32_t kS = 1000;        // 1 秒
const uint32_t kT = 10 * kS;     // テストの開始時刻 (millis)
const char* const kUserA = "1234567";
const char* const kUserB = "7654321";

std::string str(const char* s) { return std::string(s); }

toolcheck::ClassifiedCode qr(const char* text) {
  return toolcheck::classifyCode(text, std::strlen(text), toolcheck::ClassifierConfig{});
}

struct Rig {
  MemoryKvStore kv;
  LoanBook book;
  Config cfg;
  Session session;
  Rig() : book(kv), cfg(), session(book, cfg) { book.load(); }
};

SessionRow row(const Session& s, size_t index) {
  SessionRow r;
  s.getRow(index, &r);
  return r;
}

}  // namespace

TEST(session_starts_idle) {
  Rig rig;
  CHECK_EQ(rig.session.state(), SessionState::Idle);
  CHECK_EQ(rig.session.alarmLevel(kT), uint8_t{0});
  CHECK_EQ(rig.session.rowCount(), size_t{0});
  CHECK(!rig.session.isPaused());
}

// --- 物品 → 名札 ---

TEST(session_open_starts_collecting_and_requests_photo) {
  Rig rig;
  const SessionOutput out = rig.session.onDrawerOpened(kT, kE0, "p1.jpg");
  CHECK(out.capture_photo);
  CHECK_EQ(out.record_count, uint8_t{0});
  CHECK_EQ(rig.session.state(), SessionState::Collecting);
  CHECK(rig.session.hasOpened());
}

TEST(session_item_then_tag_checks_out) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, "p1.jpg");
  SessionOutput out = rig.session.onCode(qr("TW-025"), kT + 2 * kS, kE0 + 2);
  CHECK(!out.ignored);
  CHECK_EQ(rig.session.rowCount(), size_t{1});
  const SessionRow pending = row(rig.session, 0);
  CHECK_EQ(str(pending.item), str("TW-025"));
  CHECK_EQ(pending.kind, RowKind::Checkout);
  CHECK_EQ(pending.status, RowStatus::Pending);
  Loan l;
  CHECK(!rig.book.findLoan("TW-025", &l));  // 名札を読むまでは記録しない

  out = rig.session.onCode(qr(kUserA), kT + 5 * kS, kE0 + 5);
  CHECK_EQ(out.record_count, uint8_t{0});
  CHECK_EQ(rig.session.state(), SessionState::TagWindow);
  CHECK_EQ(str(rig.session.tagUser()), str(kUserA));
  const SessionRow done = row(rig.session, 0);
  CHECK_EQ(done.status, RowStatus::Done);
  CHECK(rig.book.findLoan("TW-025", &l));
  CHECK_EQ(str(l.user), str(kUserA));
  CHECK_EQ(l.checkout_epoch, kE0 + 5);
  CHECK_EQ(str(l.checkout_photo), str("p1.jpg"));

  // 名札先の 20 秒が終わると、開閉ログの 1 件が出る
  out = rig.session.tick(kT + 5 * kS + toolcheck::kTagWindowMs, kE0 + 25);
  CHECK_EQ(out.record_count, uint8_t{1});
  const SessionRecord rec = out.records[0];
  CHECK_EQ(rec.end, SessionEnd::Confirmed);
  CHECK_EQ(rec.start_epoch, kE0);
  CHECK(rec.opened);
  CHECK_EQ(str(rec.user), str(kUserA));
  CHECK_EQ(str(rec.photo), str("p1.jpg"));
  CHECK_EQ(rec.checkouts, uint8_t{1});
  CHECK_EQ(rec.returns, uint8_t{0});
  CHECK_EQ(rec.switches, uint8_t{0});
  CHECK_EQ(rec.failures, uint8_t{0});
  CHECK(!rec.unregistered_open);
  CHECK(!rec.unknown_checkout);
  CHECK_EQ(rig.session.state(), SessionState::Idle);
  CHECK_EQ(rig.session.rowCount(), size_t{0});
}

// 2026-09-15: 返却は名札を読まない。貸出中の物品を読んだ時点で返却し、返した人は記録しない
// (前は返却の行を名札で確定し、返した人と代理の印を書いていた)
TEST(session_lent_item_is_returned_when_read) {
  Rig rig;
  CHECK_EQ(rig.book.checkout("TW-025", kUserA, kE0 - 3600, "old.jpg"), BookResult::Ok);
  rig.session.onDrawerOpened(kT, kE0, "p1.jpg");
  const SessionOutput read = rig.session.onCode(qr("TW-025"), kT + kS, kE0 + 1);
  CHECK(!read.ignored);
  const SessionRow r = row(rig.session, 0);
  CHECK_EQ(r.kind, RowKind::Return);
  CHECK_EQ(r.status, RowStatus::Done);
  CHECK_EQ(str(r.borrower), str(kUserA));
  Loan l;
  CHECK(!rig.book.findLoan("TW-025", &l));
  ReturnEntry e;
  CHECK(rig.book.getReturn(0, &e));
  CHECK_EQ(str(e.borrower), str(kUserA));
  CHECK_EQ(str(e.returner), str(""));
  CHECK_EQ(e.return_epoch, kE0 + 1);
  CHECK_EQ(str(e.return_photo), str("p1.jpg"));
  CHECK(!rig.session.isCounting());
  CHECK_EQ(rig.session.alarmLevel(kT + 100 * kS), uint8_t{0});  // 開けて返却だけなら警告しない

  // 名札を読まなくても、最後に読んでから 20 秒で 1 件になる
  SessionOutput out = rig.session.tick(kT + kS + toolcheck::kReturnOnlyEndMs - 1, kE0 + 20);
  CHECK_EQ(out.record_count, uint8_t{0});
  out = rig.session.tick(kT + kS + toolcheck::kReturnOnlyEndMs, kE0 + 21);
  CHECK_EQ(out.record_count, uint8_t{1});
  const SessionRecord rec = out.records[0];
  CHECK_EQ(rec.end, SessionEnd::Confirmed);
  CHECK(rec.opened);
  CHECK_EQ(str(rec.user), str(""));
  CHECK_EQ(str(rec.photo), str("p1.jpg"));
  CHECK_EQ(rec.returns, uint8_t{1});
  CHECK_EQ(rec.checkouts, uint8_t{0});
  CHECK(!rec.unregistered_open);
  CHECK(!rec.unknown_checkout);
  CHECK_EQ(rig.session.state(), SessionState::Idle);
}

TEST(session_return_only_without_open_needs_no_tag) {
  Rig rig;
  rig.book.checkout("R-1", kUserA, kE0 - 600, nullptr);
  rig.book.checkout("R-2", kUserB, kE0 - 600, nullptr);
  rig.session.onCode(qr("R-1"), kT, kE0);
  CHECK_EQ(rig.session.state(), SessionState::Collecting);
  CHECK(!rig.session.isCounting());
  rig.session.onCode(qr("R-2"), kT + 10 * kS, kE0 + 10);  // 続けて返すと、20 秒はそこから数える
  CHECK_EQ(rig.session.alarmLevel(kT + 200 * kS), uint8_t{0});
  SessionOutput out = rig.session.tick(kT + 10 * kS + toolcheck::kReturnOnlyEndMs - 1, kE0 + 29);
  CHECK_EQ(out.record_count, uint8_t{0});
  out = rig.session.tick(kT + 10 * kS + toolcheck::kReturnOnlyEndMs, kE0 + 30);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK(!out.records[0].opened);
  CHECK_EQ(out.records[0].returns, uint8_t{2});
  CHECK_EQ(str(out.records[0].user), str(""));
  CHECK_EQ(rig.book.loanCount(), uint16_t{0});
}

TEST(session_checkout_with_return_still_needs_tag) {
  Rig rig;
  rig.book.checkout("R-1", kUserB, kE0 - 600, nullptr);
  rig.session.onCode(qr("R-1"), kT, kE0);
  rig.session.onCode(qr("X-1"), kT + kS, kE0 + 1);
  CHECK(rig.session.isCounting());
  CHECK_EQ(rig.session.alarmLevel(kT + 41 * kS), uint8_t{1});
  SessionOutput out = rig.session.tick(kT + kS + toolcheck::kReturnOnlyEndMs, kE0 + 21);
  CHECK_EQ(out.record_count, uint8_t{0});  // 名札待ちの持出があるので 20 秒では終えない
  rig.session.onCode(qr(kUserA), kT + 45 * kS, kE0 + 45);
  Loan l;
  CHECK(rig.book.findLoan("X-1", &l));
  CHECK_EQ(str(l.user), str(kUserA));
  out = rig.session.tick(kT + 45 * kS + toolcheck::kTagWindowMs, kE0 + 65);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(out.records[0].returns, uint8_t{1});
  CHECK_EQ(out.records[0].checkouts, uint8_t{1});
  CHECK_EQ(str(out.records[0].user), str(kUserA));
}

TEST(session_failed_return_marks_row_failed) {
  Rig rig;
  rig.book.checkout("R-1", kUserA, kE0 - 600, nullptr);
  rig.session.onDrawerOpened(kT, kE0, nullptr);
  rig.kv.fail_after_puts = rig.kv.puts;
  rig.session.onCode(qr("R-1"), kT + kS, kE0 + 1);
  rig.kv.fail_after_puts = -1;
  const SessionRow r = row(rig.session, 0);
  CHECK_EQ(r.kind, RowKind::Return);
  CHECK_EQ(r.status, RowStatus::Failed);
  CHECK_EQ(r.result, BookResult::StorageError);
  CHECK(!rig.session.canToggleSwitch(0));
  Loan l;
  CHECK(rig.book.findLoan("R-1", &l));  // 返せていないので貸出中のまま
  const SessionOutput out = rig.session.tick(kT + kS + toolcheck::kReturnOnlyEndMs, kE0 + 21);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(out.records[0].failures, uint8_t{1});
  CHECK_EQ(out.records[0].returns, uint8_t{0});
}

TEST(session_return_row_cannot_be_removed) {
  Rig rig;
  rig.book.checkout("R-1", kUserA, kE0 - 600, nullptr);
  rig.session.onCode(qr("R-1"), kT, kE0);
  CHECK(!rig.session.removeRow(0));  // もう返却を記録している
  CHECK_EQ(rig.session.rowCount(), size_t{1});
}

TEST(session_qr_first_starts_session_without_open) {
  Rig rig;
  SessionOutput out = rig.session.onCode(qr("TW-025"), kT, kE0);
  CHECK(!out.capture_photo);
  CHECK_EQ(rig.session.state(), SessionState::Collecting);
  CHECK(!rig.session.hasOpened());
  rig.session.onCode(qr(kUserA), kT + kS, kE0 + 1);
  out = rig.session.tick(kT + kS + toolcheck::kTagWindowMs, kE0 + 21);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK(!out.records[0].opened);
  CHECK_EQ(out.records[0].start_epoch, kE0);
  CHECK_EQ(out.records[0].checkouts, uint8_t{1});
  CHECK_EQ(str(out.records[0].photo), str(""));
}

TEST(session_open_after_qr_start_attaches_photo) {
  Rig rig;
  rig.session.onCode(qr("X-1"), kT, kE0);
  const SessionOutput opened = rig.session.onDrawerOpened(kT + 2 * kS, kE0 + 2, "p1.jpg");
  CHECK(opened.capture_photo);
  CHECK(rig.session.hasOpened());
  rig.session.onCode(qr(kUserA), kT + 5 * kS, kE0 + 5);
  Loan l;
  CHECK(rig.book.findLoan("X-1", &l));
  CHECK_EQ(str(l.checkout_photo), str("p1.jpg"));
  const SessionOutput out = rig.session.tick(kT + 5 * kS + toolcheck::kTagWindowMs, kE0 + 25);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK(out.records[0].opened);
  CHECK_EQ(out.records[0].start_epoch, kE0);
}

TEST(session_tag_only_after_open_records_user) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, "p1.jpg");
  rig.session.onCode(qr(kUserA), kT + 3 * kS, kE0 + 3);
  CHECK_EQ(rig.session.state(), SessionState::TagWindow);
  CHECK_EQ(rig.session.alarmLevel(kT + 100 * kS), uint8_t{0});
  const SessionOutput out = rig.session.tick(kT + 3 * kS + toolcheck::kTagWindowMs, kE0 + 23);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(out.records[0].end, SessionEnd::Confirmed);
  CHECK_EQ(str(out.records[0].user), str(kUserA));
  CHECK(out.records[0].opened);
  CHECK_EQ(out.records[0].checkouts, uint8_t{0});
}

TEST(session_failed_write_marks_row_failed) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, nullptr);
  rig.session.onCode(qr("X-1"), kT + kS, kE0 + 1);
  rig.kv.fail_after_puts = rig.kv.puts;
  rig.session.onCode(qr(kUserA), kT + 2 * kS, kE0 + 2);
  const SessionRow r = row(rig.session, 0);
  CHECK_EQ(r.status, RowStatus::Failed);
  CHECK_EQ(r.result, BookResult::StorageError);
  rig.kv.fail_after_puts = -1;
  const SessionOutput out = rig.session.tick(kT + 2 * kS + toolcheck::kTagWindowMs, kE0 + 22);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(out.records[0].failures, uint8_t{1});
  CHECK_EQ(out.records[0].checkouts, uint8_t{0});
}

// --- 名札先の 20 秒 ---

TEST(session_tag_window_confirms_items_immediately) {
  Rig rig;
  rig.book.checkout("MM-100", kUserB, kE0 - 60, nullptr);
  rig.session.onDrawerOpened(kT, kE0, "p1.jpg");
  rig.session.onCode(qr(kUserA), kT + kS, kE0 + 1);
  rig.session.onCode(qr("TW-025"), kT + 5 * kS, kE0 + 5);
  Loan l;
  CHECK(rig.book.findLoan("TW-025", &l));
  CHECK_EQ(str(l.user), str(kUserA));
  CHECK_EQ(l.checkout_epoch, kE0 + 5);
  CHECK_EQ(str(l.checkout_photo), str("p1.jpg"));

  rig.session.onCode(qr("MM-100"), kT + 8 * kS, kE0 + 8);
  CHECK(!rig.book.findLoan("MM-100", &l));
  ReturnEntry e;
  CHECK(rig.book.getReturn(0, &e));
  CHECK_EQ(str(e.item), str("MM-100"));
  CHECK_EQ(str(e.returner), str(""));  // 2026-09-15: 返した人は記録しない (前は名札の kUserA)
  CHECK_EQ(rig.session.rowCount(), size_t{2});
  const SessionRow r = row(rig.session, 1);
  CHECK_EQ(r.kind, RowKind::Return);
  CHECK_EQ(r.status, RowStatus::Done);

  const SessionOutput out = rig.session.tick(kT + kS + toolcheck::kTagWindowMs, kE0 + 21);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(out.records[0].checkouts, uint8_t{1});
  CHECK_EQ(out.records[0].returns, uint8_t{1});
}

TEST(session_tag_window_boundary) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, nullptr);
  rig.session.onCode(qr(kUserA), kT + kS, kE0 + 1);
  rig.session.onCode(qr("X-1"), kT + kS + toolcheck::kTagWindowMs - 1, kE0 + 20);
  Loan l;
  CHECK(rig.book.findLoan("X-1", &l));
}

TEST(session_tag_window_is_counted_from_tag) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, nullptr);
  rig.session.onCode(qr(kUserA), kT + kS, kE0 + 1);
  rig.session.onCode(qr("X-1"), kT + 11 * kS, kE0 + 11);  // 名札から 10 秒: 即確定
  Loan l;
  CHECK(rig.book.findLoan("X-1", &l));

  // 物品を読んでも延びない。名札から 20 秒を過ぎたら、物品 → 名札の順に戻る
  const SessionOutput out = rig.session.onCode(qr("Y-2"), kT + kS + toolcheck::kTagWindowMs, kE0 + 21);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(out.records[0].checkouts, uint8_t{1});
  CHECK(!rig.book.findLoan("Y-2", &l));
  CHECK_EQ(rig.session.state(), SessionState::Collecting);
  CHECK_EQ(rig.session.rowCount(), size_t{1});
  const SessionRow r = row(rig.session, 0);
  CHECK_EQ(str(r.item), str("Y-2"));
  CHECK_EQ(r.status, RowStatus::Pending);
}

TEST(session_same_tag_again_restarts_window) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, nullptr);
  rig.session.onCode(qr(kUserA), kT + kS, kE0 + 1);
  const SessionOutput again = rig.session.onCode(qr(kUserA), kT + 16 * kS, kE0 + 16);
  CHECK_EQ(again.record_count, uint8_t{0});
  CHECK(!again.show_user_loans);
  rig.session.onCode(qr("X-1"), kT + 30 * kS, kE0 + 30);  // 2 回目の名札から 14 秒
  Loan l;
  CHECK(rig.book.findLoan("X-1", &l));
  CHECK_EQ(str(l.user), str(kUserA));
}

TEST(session_other_tag_in_window_ends_session_and_shows_loans) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, nullptr);
  rig.session.onCode(qr(kUserA), kT + kS, kE0 + 1);
  const SessionOutput out = rig.session.onCode(qr(kUserB), kT + 5 * kS, kE0 + 5);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(str(out.records[0].user), str(kUserA));
  CHECK(out.show_user_loans);
  CHECK_EQ(str(out.loans_user), str(kUserB));
  CHECK_EQ(rig.session.state(), SessionState::Idle);
}

TEST(session_open_during_tag_window_starts_next_session) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, "p1.jpg");
  rig.session.onCode(qr(kUserA), kT + kS, kE0 + 1);
  const SessionOutput out = rig.session.onDrawerOpened(kT + 5 * kS, kE0 + 5, "p2.jpg");
  CHECK(out.capture_photo);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(str(out.records[0].user), str(kUserA));
  CHECK_EQ(str(out.records[0].photo), str("p1.jpg"));
  CHECK_EQ(rig.session.state(), SessionState::Collecting);
  CHECK_EQ(str(rig.session.tagUser()), str(""));
  CHECK_EQ(rig.session.alarmLevel(kT + 45 * kS), uint8_t{1});
}

// --- 名札だけ・読み捨て ---

TEST(session_tag_without_session_shows_loans_only) {
  Rig rig;
  const int before = rig.kv.writes();
  const SessionOutput out = rig.session.onCode(qr(kUserA), kT, kE0);
  CHECK(out.show_user_loans);
  CHECK_EQ(str(out.loans_user), str(kUserA));
  CHECK_EQ(out.record_count, uint8_t{0});
  CHECK_EQ(rig.session.state(), SessionState::Idle);
  CHECK_EQ(rig.kv.writes(), before);
}

TEST(session_same_code_is_ignored_while_seen_within_3s) {
  Rig rig;
  SessionOutput out = rig.session.onCode(qr(kUserA), kT, kE0);
  CHECK(out.show_user_loans);
  out = rig.session.onCode(qr(kUserA), kT + 2 * kS, kE0 + 2);
  CHECK(out.ignored);
  CHECK(!out.show_user_loans);
  out = rig.session.onCode(qr(kUserA), kT + 4 * kS, kE0 + 4);  // 読み続けている間は延びる
  CHECK(out.ignored);
  out = rig.session.onCode(qr(kUserA), kT + 4 * kS + toolcheck::kDuplicateIgnoreMs, kE0 + 7);
  CHECK(!out.ignored);
  CHECK(out.show_user_loans);
}

TEST(session_duplicate_check_is_per_code) {
  Rig rig;
  rig.session.onCode(qr(kUserA), kT, kE0);            // 一覧
  rig.session.onCode(qr("X-1"), kT + kS, kE0 + 1);    // セッション開始
  const SessionOutput out = rig.session.onCode(qr(kUserA), kT + 2 * kS, kE0 + 2);  // 間に別のコードを挟んでも読み捨て
  CHECK(out.ignored);
  CHECK_EQ(rig.session.state(), SessionState::Collecting);
  const SessionRow r = row(rig.session, 0);
  CHECK_EQ(r.status, RowStatus::Pending);
}

TEST(session_item_handled_in_session_is_not_toggled) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, nullptr);
  rig.session.onCode(qr("X-1"), kT + kS, kE0 + 1);
  rig.session.onCode(qr(kUserA), kT + 2 * kS, kE0 + 2);
  const SessionOutput out = rig.session.onCode(qr("X-1"), kT + 10 * kS, kE0 + 10);  // 3 秒を過ぎてからもう一度
  CHECK(out.ignored);
  Loan l;
  CHECK(rig.book.findLoan("X-1", &l));  // 返却にならない
  CHECK_EQ(rig.session.rowCount(), size_t{1});
}

TEST(session_rows_are_capped) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, nullptr);
  char item[8];
  for (int i = 0; i < toolcheck::kMaxSessionRows; ++i) {
    std::snprintf(item, sizeof(item), "IT%02d", i);
    const SessionOutput added = rig.session.onCode(qr(item), kT + kS + i, kE0 + 1);
    CHECK(!added.ignored);
  }
  const SessionOutput out = rig.session.onCode(qr("IT99"), kT + 2 * kS, kE0 + 2);
  CHECK(out.ignored);
  CHECK(out.rows_full);
  CHECK_EQ(rig.session.rowCount(), size_t{toolcheck::kMaxSessionRows});
}

TEST(session_invalid_code_is_ignored_without_progress) {
  Rig rig;
  SessionOutput out = rig.session.onCode(qr("A B"), kT, kE0);
  CHECK(out.ignored);
  CHECK_EQ(rig.session.state(), SessionState::Idle);

  rig.session.onDrawerOpened(kT, kE0, nullptr);
  out = rig.session.onCode(qr("A B"), kT + 30 * kS, kE0 + 30);
  CHECK(out.ignored);
  CHECK_EQ(rig.session.rowCount(), size_t{0});
  CHECK_EQ(rig.session.alarmLevel(kT + 40 * kS), uint8_t{1});  // 読み直しにならない
}

// --- 行の操作 ---

TEST(session_remove_row_before_tag) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, nullptr);
  rig.session.onCode(qr("X-1"), kT + kS, kE0 + 1);
  rig.session.onCode(qr("Y-2"), kT + 2 * kS, kE0 + 2);
  CHECK(rig.session.removeRow(0));
  CHECK_EQ(rig.session.rowCount(), size_t{1});
  CHECK(!rig.session.removeRow(5));
  rig.session.onCode(qr(kUserA), kT + 3 * kS, kE0 + 3);
  Loan l;
  CHECK(!rig.book.findLoan("X-1", &l));
  CHECK(rig.book.findLoan("Y-2", &l));
  CHECK(!rig.session.removeRow(0));  // 確定した行は消せない
}

TEST(session_removing_last_row_of_qr_session_ends_it) {
  Rig rig;
  rig.session.onCode(qr("X-1"), kT, kE0);
  CHECK(rig.session.removeRow(0));
  CHECK_EQ(rig.session.state(), SessionState::Idle);
  const SessionOutput out = rig.session.tick(kT + 300 * kS, kE0 + 300);
  CHECK_EQ(out.record_count, uint8_t{0});
}

// 2026-09-15: 返し忘れの物品を次の人が持ち出すときは、読んだ時点で返却になった行の「持出に切替」で持出にする
TEST(session_switch_toggle_only_on_return_rows) {
  Rig rig;
  rig.book.checkout("Y-2", kUserA, kE0 - 600, nullptr);
  rig.session.onDrawerOpened(kT, kE0, nullptr);
  rig.session.onCode(qr("X-1"), kT + kS, kE0 + 1);
  rig.session.onCode(qr("Y-2"), kT + 2 * kS, kE0 + 2);  // 読んだ時点で返却済み
  CHECK(!rig.session.canToggleSwitch(0));
  CHECK(!rig.session.toggleSwitch(0, kE0 + 3));
  CHECK(rig.session.canToggleSwitch(1));
  CHECK(rig.session.toggleSwitch(1, kE0 + 3));
  const SessionRow switched = row(rig.session, 1);
  CHECK_EQ(switched.kind, RowKind::Switch);
  CHECK_EQ(switched.status, RowStatus::Pending);
  CHECK(!rig.session.removeRow(1));  // 切替の行は × で消さない (「返却に戻す」で戻す)
  CHECK(rig.session.toggleSwitch(1, kE0 + 4));
  const SessionRow back = row(rig.session, 1);
  CHECK_EQ(back.kind, RowKind::Return);
  CHECK_EQ(back.status, RowStatus::Done);
  CHECK(!rig.session.toggleSwitch(9, kE0 + 4));
  Loan l;
  CHECK(!rig.book.findLoan("Y-2", &l));  // 切り替えても戻しても、本は書き直さない
  CHECK_EQ(rig.book.returnCount(), uint16_t{1});
}

// 前は名札で元の貸出を「返却漏れ」の印で閉じて付け替えていた。今は読んだ時点の返却の後に、切替の印で貸し出す
TEST(session_switch_row_moves_loan_to_tag_user) {
  Rig rig;
  rig.book.checkout("Y-2", kUserA, kE0 - 600, "old.jpg");
  rig.session.onDrawerOpened(kT, kE0, "p1.jpg");
  rig.session.onCode(qr("Y-2"), kT + kS, kE0 + 1);
  CHECK(rig.session.toggleSwitch(0, kE0 + 1));
  CHECK(rig.session.isCounting());  // 切替は名札待ち
  rig.session.onCode(qr(kUserB), kT + 2 * kS, kE0 + 2);
  Loan l;
  CHECK(rig.book.findLoan("Y-2", &l));
  CHECK_EQ(str(l.user), str(kUserB));
  CHECK_EQ(l.flags, toolcheck::kLoanSwitched);
  CHECK_EQ(l.checkout_epoch, kE0 + 2);
  CHECK_EQ(str(l.checkout_photo), str("p1.jpg"));
  ReturnEntry e;
  CHECK(rig.book.getReturn(0, &e));
  CHECK_EQ(str(e.borrower), str(kUserA));
  CHECK_EQ(e.return_epoch, kE0 + 1);
  CHECK_EQ(rig.book.returnCount(), uint16_t{1});
  const SessionRow r = row(rig.session, 0);
  CHECK_EQ(r.kind, RowKind::Switch);
  CHECK_EQ(r.status, RowStatus::Done);
  const SessionOutput out = rig.session.tick(kT + 2 * kS + toolcheck::kTagWindowMs, kE0 + 22);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(out.records[0].switches, uint8_t{1});
  CHECK_EQ(out.records[0].returns, uint8_t{0});  // 開閉ログの数は行の見た目に合わせる
}

TEST(session_switch_in_tag_window_lends_immediately) {
  Rig rig;
  rig.book.checkout("Y-2", kUserA, kE0 - 600, nullptr);
  rig.session.onDrawerOpened(kT, kE0, "p1.jpg");
  rig.session.onCode(qr(kUserB), kT + kS, kE0 + 1);     // 名札が先
  rig.session.onCode(qr("Y-2"), kT + 3 * kS, kE0 + 3);  // 返却になる
  Loan l;
  CHECK(!rig.book.findLoan("Y-2", &l));
  CHECK(rig.session.canToggleSwitch(0));
  CHECK(rig.session.toggleSwitch(0, kE0 + 5));
  CHECK(rig.book.findLoan("Y-2", &l));
  CHECK_EQ(str(l.user), str(kUserB));
  CHECK_EQ(l.flags, toolcheck::kLoanSwitched);
  CHECK_EQ(l.checkout_epoch, kE0 + 5);
  CHECK_EQ(str(l.checkout_photo), str("p1.jpg"));
  const SessionRow r = row(rig.session, 0);
  CHECK_EQ(r.kind, RowKind::Switch);
  CHECK_EQ(r.status, RowStatus::Done);
  CHECK(!rig.session.canToggleSwitch(0));  // 名札先で書いた切替は戻せない
  CHECK(!rig.session.toggleSwitch(0, kE0 + 6));
  const SessionOutput out = rig.session.tick(kT + kS + toolcheck::kTagWindowMs, kE0 + 21);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(out.records[0].switches, uint8_t{1});
  CHECK_EQ(out.records[0].returns, uint8_t{0});
}

// --- 警告と打ち切り ---

TEST(session_warning_levels_follow_config) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, nullptr);
  CHECK_EQ(rig.session.alarmLevel(kT + 40 * kS - 1), uint8_t{0});
  CHECK_EQ(rig.session.alarmLevel(kT + 40 * kS), uint8_t{1});
  CHECK_EQ(rig.session.alarmLevel(kT + 60 * kS - 1), uint8_t{1});
  CHECK_EQ(rig.session.alarmLevel(kT + 60 * kS), uint8_t{2});
  CHECK_EQ(rig.session.sinceProgressMs(kT + 50 * kS), 50 * kS);

  Config cfg;
  cfg.warning1_sec = 10;
  cfg.warning2_sec = 20;
  cfg.giveup_sec = 30;
  rig.session.setConfig(cfg);
  CHECK_EQ(rig.session.alarmLevel(kT + 10 * kS), uint8_t{1});
  CHECK_EQ(rig.session.alarmLevel(kT + 20 * kS), uint8_t{2});
  const SessionOutput before = rig.session.tick(kT + 50 * kS - 1, kE0 + 49);
  CHECK_EQ(before.record_count, uint8_t{0});
  const SessionOutput after = rig.session.tick(kT + 50 * kS, kE0 + 50);
  CHECK_EQ(after.record_count, uint8_t{1});
}

TEST(session_valid_code_resets_warning) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, nullptr);
  CHECK_EQ(rig.session.alarmLevel(kT + 45 * kS), uint8_t{1});
  rig.session.onCode(qr("X-1"), kT + 45 * kS, kE0 + 45);
  CHECK_EQ(rig.session.alarmLevel(kT + 45 * kS), uint8_t{0});
  CHECK_EQ(rig.session.alarmLevel(kT + 85 * kS - 1), uint8_t{0});
  CHECK_EQ(rig.session.alarmLevel(kT + 85 * kS), uint8_t{1});
  // 同じ物品をもう一度 (3 秒を過ぎて) 読んでも行は増えないが、読み直しにはなる
  rig.session.onCode(qr("X-1"), kT + 90 * kS, kE0 + 90);
  CHECK_EQ(rig.session.rowCount(), size_t{1});
  CHECK_EQ(rig.session.alarmLevel(kT + 90 * kS), uint8_t{0});
}

TEST(session_reopen_resets_warning_and_keeps_first_photo) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, "p1.jpg");
  const SessionOutput out = rig.session.onDrawerOpened(kT + 50 * kS, kE0 + 50, "p2.jpg");
  CHECK(out.capture_photo);
  CHECK_EQ(out.record_count, uint8_t{0});
  CHECK_EQ(rig.session.alarmLevel(kT + 50 * kS), uint8_t{0});
  rig.session.onCode(qr(kUserA), kT + 55 * kS, kE0 + 55);
  const SessionOutput end = rig.session.tick(kT + 55 * kS + toolcheck::kTagWindowMs, kE0 + 75);
  CHECK_EQ(end.record_count, uint8_t{1});
  CHECK_EQ(str(end.records[0].photo), str("p1.jpg"));
  CHECK_EQ(end.records[0].start_epoch, kE0);
}

TEST(session_giveup_records_unregistered_open) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, "p1.jpg");
  const uint32_t giveup = kT + (60 + 120) * kS;
  SessionOutput out = rig.session.tick(giveup - 1, kE0 + 179);
  CHECK_EQ(out.record_count, uint8_t{0});
  CHECK_EQ(rig.session.alarmLevel(giveup - 1), uint8_t{2});
  out = rig.session.tick(giveup, kE0 + 180);
  CHECK_EQ(out.record_count, uint8_t{1});
  const SessionRecord rec = out.records[0];
  CHECK_EQ(rec.end, SessionEnd::GaveUp);
  CHECK(rec.opened);
  CHECK_EQ(str(rec.user), str(""));
  CHECK_EQ(str(rec.photo), str("p1.jpg"));
  CHECK(rec.unregistered_open);
  CHECK(!rec.unknown_checkout);
  CHECK_EQ(rig.session.state(), SessionState::Idle);
  CHECK_EQ(rig.session.alarmLevel(giveup), uint8_t{0});
}

TEST(session_giveup_records_unknown_user_for_pending_rows) {
  Rig rig;
  rig.book.checkout("RET-1", kUserB, kE0 - 600, nullptr);
  rig.book.checkout("SW-1", kUserB, kE0 - 600, nullptr);
  rig.session.onDrawerOpened(kT, kE0, "p1.jpg");
  rig.session.onCode(qr("OUT-1"), kT + kS, kE0 + 1);
  rig.session.onCode(qr("RET-1"), kT + 2 * kS, kE0 + 2);
  rig.session.onCode(qr("SW-1"), kT + 3 * kS, kE0 + 3);
  CHECK(rig.session.toggleSwitch(2, kE0 + 3));

  const SessionOutput out = rig.session.tick(kT + 3 * kS + 180 * kS, kE0 + 183);
  CHECK_EQ(out.record_count, uint8_t{1});
  const SessionRecord rec = out.records[0];
  CHECK_EQ(rec.end, SessionEnd::GaveUp);
  CHECK_EQ(rec.checkouts, uint8_t{1});
  CHECK_EQ(rec.returns, uint8_t{1});
  CHECK_EQ(rec.switches, uint8_t{1});
  CHECK(rec.unknown_checkout);
  CHECK(!rec.unregistered_open);

  Loan l;
  CHECK(rig.book.findLoan("OUT-1", &l));
  CHECK_EQ(str(l.user), str(""));
  CHECK_EQ(l.flags, toolcheck::kLoanUserUnknown);
  CHECK_EQ(str(l.checkout_photo), str("p1.jpg"));
  CHECK(!rig.book.findLoan("RET-1", &l));
  ReturnEntry e;
  CHECK(rig.book.findLastReturn("RET-1", &e));
  CHECK_EQ(e.flags, toolcheck::kReturnReturnerUnknown);
  CHECK(rig.book.findLoan("SW-1", &l));
  CHECK_EQ(l.flags, static_cast<uint8_t>(toolcheck::kLoanSwitched | toolcheck::kLoanUserUnknown));
}

TEST(session_giveup_with_only_returns_is_not_an_alert) {
  Rig rig;
  rig.book.checkout("RET-1", kUserB, kE0 - 600, nullptr);
  rig.session.onDrawerOpened(kT, kE0, nullptr);
  rig.session.onCode(qr("RET-1"), kT + kS, kE0 + 1);
  const SessionOutput out = rig.session.tick(kT + kS + 180 * kS, kE0 + 181);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(out.records[0].returns, uint8_t{1});
  CHECK(!out.records[0].unknown_checkout);
  CHECK(!out.records[0].unregistered_open);
}

TEST(session_giveup_after_removing_all_rows_is_unregistered) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, nullptr);
  rig.session.onCode(qr("X-1"), kT + kS, kE0 + 1);
  CHECK(rig.session.removeRow(0));
  CHECK_EQ(rig.session.state(), SessionState::Collecting);  // 開けたので名札が要る
  const SessionOutput out = rig.session.tick(kT + kS + 180 * kS, kE0 + 181);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK(out.records[0].unregistered_open);
  Loan l;
  CHECK(!rig.book.findLoan("X-1", &l));
}

TEST(session_timers_survive_millis_wraparound) {
  Rig rig;
  uint32_t start = 0xFFFFFFFFu - 10 * kS;
  rig.session.onDrawerOpened(start, kE0, nullptr);
  uint32_t later = start + 40 * kS;  // 一周して小さい値になる
  CHECK_EQ(rig.session.alarmLevel(later), uint8_t{1});
  later = start + 180 * kS;
  const SessionOutput out = rig.session.tick(later, kE0 + 180);
  CHECK_EQ(out.record_count, uint8_t{1});
}

// --- 警告の一時停止 ---

TEST(session_open_while_paused_logs_without_session) {
  Rig rig;
  SessionOutput out = rig.session.pause(10 * 60 * kS, kT, kE0);
  CHECK(out.pause_started);
  CHECK(rig.session.isPaused());
  out = rig.session.onDrawerOpened(kT + kS, kE0 + 1, "p1.jpg");
  CHECK(out.capture_photo);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(out.records[0].end, SessionEnd::PausedOpen);
  CHECK(out.records[0].opened);
  CHECK_EQ(out.records[0].start_epoch, kE0 + 1);
  CHECK_EQ(str(out.records[0].photo), str("p1.jpg"));
  CHECK_EQ(rig.session.state(), SessionState::Idle);
  CHECK_EQ(rig.session.alarmLevel(kT + 100 * kS), uint8_t{0});
}

TEST(session_open_while_paused_ends_tag_window_first) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, nullptr);
  rig.session.onCode(qr(kUserA), kT + kS, kE0 + 1);
  rig.session.pause(10 * 60 * kS, kT + 2 * kS, kE0 + 2);
  const SessionOutput out = rig.session.onDrawerOpened(kT + 5 * kS, kE0 + 5, "p2.jpg");
  CHECK_EQ(out.record_count, uint8_t{2});
  CHECK_EQ(out.records[0].end, SessionEnd::Confirmed);
  CHECK_EQ(str(out.records[0].user), str(kUserA));
  CHECK_EQ(out.records[1].end, SessionEnd::PausedOpen);
  CHECK_EQ(str(out.records[1].photo), str("p2.jpg"));
  CHECK_EQ(rig.session.state(), SessionState::Idle);
}

TEST(session_pause_ends_by_time_or_resume) {
  Rig rig;
  rig.session.pause(60 * kS, kT, kE0);
  CHECK_EQ(rig.session.pauseRemainingMs(kT + 20 * kS), 40 * kS);
  SessionOutput out = rig.session.tick(kT + 60 * kS - 1, kE0 + 59);
  CHECK(!out.pause_ended);
  CHECK(rig.session.isPaused());
  out = rig.session.tick(kT + 60 * kS, kE0 + 60);
  CHECK(out.pause_ended);
  CHECK(!rig.session.isPaused());
  CHECK_EQ(rig.session.pauseRemainingMs(kT + 61 * kS), uint32_t{0});

  rig.session.pause(60 * kS, kT + 100 * kS, kE0 + 100);
  out = rig.session.resume(kT + 110 * kS, kE0 + 110);
  CHECK(out.pause_ended);
  CHECK(!rig.session.isPaused());
  out = rig.session.resume(kT + 120 * kS, kE0 + 120);  // 停止していなければ何もしない
  CHECK(!out.pause_ended);
}

TEST(session_pause_silences_running_session) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, nullptr);
  CHECK_EQ(rig.session.alarmLevel(kT + 50 * kS), uint8_t{1});
  rig.session.pause(10 * 60 * kS, kT + 50 * kS, kE0 + 50);
  CHECK_EQ(rig.session.alarmLevel(kT + 70 * kS), uint8_t{0});
}

TEST(session_paused_registration_works_without_alarm_or_giveup) {
  Rig rig;
  rig.session.pause(10 * 60 * kS, kT, kE0);
  rig.session.onCode(qr("X-1"), kT + kS, kE0 + 1);
  CHECK_EQ(rig.session.state(), SessionState::Collecting);
  CHECK_EQ(rig.session.alarmLevel(kT + 100 * kS), uint8_t{0});
  const SessionOutput mid = rig.session.tick(kT + 5 * 60 * kS, kE0 + 300);  // 打ち切りの時間を過ぎても
  CHECK_EQ(mid.record_count, uint8_t{0});
  CHECK_EQ(rig.session.state(), SessionState::Collecting);
  rig.session.onCode(qr(kUserA), kT + 6 * 60 * kS, kE0 + 360);
  Loan l;
  CHECK(rig.book.findLoan("X-1", &l));
  CHECK_EQ(str(l.user), str(kUserA));
}

TEST(session_pending_rows_are_warned_after_resume) {
  Rig rig;
  rig.session.pause(5 * 60 * kS, kT, kE0);
  rig.session.onCode(qr("X-1"), kT + kS, kE0 + 1);
  const uint32_t resumed = kT + 4 * 60 * kS;
  SessionOutput out = rig.session.resume(resumed, kE0 + 240);
  CHECK(out.pause_ended);
  CHECK_EQ(rig.session.alarmLevel(resumed + 40 * kS - 1), uint8_t{0});
  CHECK_EQ(rig.session.alarmLevel(resumed + 40 * kS), uint8_t{1});
  out = rig.session.tick(resumed + 180 * kS - 1, kE0 + 419);
  CHECK_EQ(out.record_count, uint8_t{0});
  out = rig.session.tick(resumed + 180 * kS, kE0 + 420);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(out.records[0].end, SessionEnd::GaveUp);
  CHECK(out.records[0].unknown_checkout);
}

TEST(session_pause_timeout_restarts_warning_timer) {
  Rig rig;
  rig.session.pause(60 * kS, kT, kE0);
  rig.session.onCode(qr("X-1"), kT + kS, kE0 + 1);
  const SessionOutput out = rig.session.tick(kT + 60 * kS, kE0 + 60);
  CHECK(out.pause_ended);
  CHECK_EQ(rig.session.alarmLevel(kT + 100 * kS - 1), uint8_t{0});
  CHECK_EQ(rig.session.alarmLevel(kT + 100 * kS), uint8_t{1});
}

// --- 引き出しセンサ (Unit ToF) を使わない設定 ---
// 2026-09-14: Unit ToF が無くても貸出管理ができるようにする。写真は名札で確定したときに撮る。
// 使う設定では今までどおり開放で撮り、名札では撮らない。

namespace {

Config withoutDrawerSensor() {
  Config c;
  c.drawer_sensor_enabled = false;
  return c;
}

}  // namespace

TEST(session_without_drawer_sensor_takes_photo_when_tag_confirms) {
  Rig rig;
  rig.session.setConfig(withoutDrawerSensor());
  SessionOutput out = rig.session.onCode(qr("TW-025"), kT, kE0, "item.jpg");
  CHECK(!out.capture_photo);  // 物品の QR では撮らない
  CHECK_EQ(rig.session.state(), SessionState::Collecting);
  CHECK(!rig.session.hasOpened());

  out = rig.session.onCode(qr(kUserA), kT + 3 * kS, kE0 + 3, "20260914-100003.jpg");
  CHECK(out.capture_photo);
  CHECK_EQ(rig.session.state(), SessionState::TagWindow);
  Loan l;
  CHECK(rig.book.findLoan("TW-025", &l));
  CHECK_EQ(str(l.user), str(kUserA));
  CHECK_EQ(str(l.checkout_photo), str("20260914-100003.jpg"));

  out = rig.session.tick(kT + 3 * kS + toolcheck::kTagWindowMs, kE0 + 23);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(out.records[0].end, SessionEnd::Confirmed);
  CHECK(!out.records[0].opened);
  CHECK_EQ(str(out.records[0].user), str(kUserA));
  CHECK_EQ(str(out.records[0].photo), str("20260914-100003.jpg"));
}

// 2026-09-15 から返却は読んだ時点で書くので、返却に名札の写真は付かない (前は名札で確定したときの写真で返却を書いた)
TEST(session_without_drawer_sensor_uses_tag_photo_for_tag_window) {
  Rig rig;
  rig.session.setConfig(withoutDrawerSensor());
  CHECK_EQ(rig.book.checkout("R-1", kUserB, kE0 - 3600, "old.jpg"), BookResult::Ok);
  rig.session.onCode(qr("R-1"), kT, kE0, nullptr);
  SessionOutput out = rig.session.onCode(qr(kUserA), kT + 2 * kS, kE0 + 2, "tag.jpg");
  CHECK(out.capture_photo);
  ReturnEntry e;
  CHECK(rig.book.getReturn(0, &e));
  CHECK_EQ(str(e.returner), str(""));
  CHECK_EQ(str(e.checkout_photo), str("old.jpg"));
  CHECK_EQ(str(e.return_photo), str(""));

  // 名札先の 20 秒に読んだ物品も、名札で撮った写真で記録する (もう一度は撮らない)
  out = rig.session.onCode(qr("X-2"), kT + 5 * kS, kE0 + 5, "again.jpg");
  CHECK(!out.capture_photo);
  Loan l;
  CHECK(rig.book.findLoan("X-2", &l));
  CHECK_EQ(str(l.checkout_photo), str("tag.jpg"));
  out = rig.session.onCode(qr(kUserA), kT + 9 * kS, kE0 + 9, "again2.jpg");  // 同じ名札をもう一度 (3 秒より後)
  CHECK(!out.capture_photo);
  out = rig.session.tick(kT + 9 * kS + toolcheck::kTagWindowMs, kE0 + 29);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(str(out.records[0].photo), str("tag.jpg"));
  CHECK_EQ(out.records[0].returns, uint8_t{1});
  CHECK_EQ(out.records[0].checkouts, uint8_t{1});
}

TEST(session_without_drawer_sensor_no_photo_without_camera_name) {
  Rig rig;
  rig.session.setConfig(withoutDrawerSensor());
  rig.session.onCode(qr("TW-025"), kT, kE0);
  const SessionOutput out = rig.session.onCode(qr(kUserA), kT + 2 * kS, kE0 + 2, nullptr);  // カメラを使わない・使えない
  CHECK(!out.capture_photo);
  Loan l;
  CHECK(rig.book.findLoan("TW-025", &l));
  CHECK_EQ(str(l.checkout_photo), str(""));
}

TEST(session_without_drawer_sensor_tag_outside_session_takes_no_photo) {
  Rig rig;
  rig.session.setConfig(withoutDrawerSensor());
  const SessionOutput out = rig.session.onCode(qr(kUserA), kT, kE0, "tag.jpg");
  CHECK(out.show_user_loans);
  CHECK(!out.capture_photo);
  CHECK_EQ(rig.session.state(), SessionState::Idle);
}

TEST(session_without_drawer_sensor_giveup_has_no_photo) {
  Rig rig;
  rig.session.setConfig(withoutDrawerSensor());
  rig.session.onCode(qr("X-1"), kT, kE0, nullptr);
  const SessionOutput out = rig.session.tick(kT + 180 * kS, kE0 + 180);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(out.records[0].end, SessionEnd::GaveUp);
  CHECK(out.records[0].unknown_checkout);
  CHECK(!out.records[0].unregistered_open);
  CHECK_EQ(str(out.records[0].photo), str(""));
  CHECK(!out.capture_photo);
}

TEST(session_with_drawer_sensor_tag_takes_no_photo) {
  Rig rig;  // 既定は引き出しセンサを使う
  rig.session.onCode(qr("Q-1"), kT, kE0, nullptr);  // QR だけのセッション
  SessionOutput out = rig.session.onCode(qr(kUserA), kT + 2 * kS, kE0 + 2, "tag.jpg");
  CHECK(!out.capture_photo);
  Loan l;
  CHECK(rig.book.findLoan("Q-1", &l));
  CHECK_EQ(str(l.checkout_photo), str(""));

  rig.session.onDrawerOpened(kT + 30 * kS, kE0 + 30, "open.jpg");  // 名札先の後の開放で次のセッション
  rig.session.onCode(qr("Q-2"), kT + 31 * kS, kE0 + 31, nullptr);
  out = rig.session.onCode(qr(kUserB), kT + 32 * kS, kE0 + 32, "tag2.jpg");
  CHECK(!out.capture_photo);
  CHECK(rig.book.findLoan("Q-2", &l));
  CHECK_EQ(str(l.checkout_photo), str("open.jpg"));
}

// --- 取りやめ ---
// 2026-09-15: QR を読んで持って行くのをやめるときは、セッションごと取りやめる (1 回押し)。
// 名札待ちの持出・切替を消して終える。読んだ時点で済んだ返却は残す。開けた後なら開閉ログに「取りやめ」で残し、赤帯にはしない

TEST(session_cancel_qr_only_checkout_leaves_nothing) {
  Rig rig;
  rig.session.onCode(qr("X-1"), kT, kE0);
  rig.session.onCode(qr("Y-2"), kT + kS, kE0 + 1);
  CHECK(rig.session.canCancel());
  const int before = rig.kv.writes();
  const SessionOutput out = rig.session.cancel(kT + 50 * kS, kE0 + 50);
  CHECK(out.cancelled);
  CHECK_EQ(out.record_count, uint8_t{0});
  CHECK_EQ(rig.session.state(), SessionState::Idle);
  CHECK_EQ(rig.session.rowCount(), size_t{0});
  CHECK_EQ(rig.session.alarmLevel(kT + 50 * kS), uint8_t{0});
  CHECK_EQ(rig.book.loanCount(), uint16_t{0});
  CHECK_EQ(rig.kv.writes(), before);
}

TEST(session_cancel_after_open_records_cancelled) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, "p1.jpg");
  rig.session.onCode(qr("X-1"), kT + kS, kE0 + 1);
  const SessionOutput out = rig.session.cancel(kT + 45 * kS, kE0 + 45);
  CHECK(out.cancelled);
  CHECK_EQ(out.record_count, uint8_t{1});
  const SessionRecord rec = out.records[0];
  CHECK_EQ(rec.end, SessionEnd::Cancelled);
  CHECK(rec.opened);
  CHECK_EQ(rec.start_epoch, kE0);
  CHECK_EQ(str(rec.photo), str("p1.jpg"));
  CHECK_EQ(str(rec.user), str(""));
  CHECK_EQ(rec.checkouts, uint8_t{0});
  CHECK(!rec.unregistered_open);
  CHECK(!rec.unknown_checkout);
  Loan l;
  CHECK(!rig.book.findLoan("X-1", &l));
  CHECK_EQ(rig.session.state(), SessionState::Idle);
  CHECK_EQ(rig.session.alarmLevel(kT + 45 * kS), uint8_t{0});
}

TEST(session_cancel_open_without_reads_is_not_unregistered) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, "p1.jpg");
  CHECK(rig.session.canCancel());
  const SessionOutput out = rig.session.cancel(kT + 70 * kS, kE0 + 70);
  CHECK(out.cancelled);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(out.records[0].end, SessionEnd::Cancelled);
  CHECK(!out.records[0].unregistered_open);
  CHECK_EQ(rig.session.state(), SessionState::Idle);
}

TEST(session_cancel_keeps_returns_done) {
  Rig rig;
  rig.book.checkout("R-1", kUserB, kE0 - 600, nullptr);
  rig.session.onCode(qr("R-1"), kT, kE0);
  rig.session.onCode(qr("X-1"), kT + kS, kE0 + 1);
  const SessionOutput out = rig.session.cancel(kT + 5 * kS, kE0 + 5);
  CHECK(out.cancelled);
  CHECK_EQ(out.record_count, uint8_t{1});  // 開けていなくても、済んだ返却があるので 1 件にする
  CHECK_EQ(out.records[0].end, SessionEnd::Cancelled);
  CHECK(!out.records[0].opened);
  CHECK_EQ(out.records[0].returns, uint8_t{1});
  CHECK_EQ(out.records[0].checkouts, uint8_t{0});
  Loan l;
  CHECK(!rig.book.findLoan("R-1", &l));
  CHECK(!rig.book.findLoan("X-1", &l));
}

TEST(session_cancel_reverts_pending_switch_to_return) {
  Rig rig;
  rig.book.checkout("SW-1", kUserB, kE0 - 600, nullptr);
  rig.session.onDrawerOpened(kT, kE0, nullptr);
  rig.session.onCode(qr("SW-1"), kT + kS, kE0 + 1);
  CHECK(rig.session.toggleSwitch(0, kE0 + 2));
  const SessionOutput out = rig.session.cancel(kT + 5 * kS, kE0 + 5);
  CHECK(out.cancelled);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(out.records[0].returns, uint8_t{1});
  CHECK_EQ(out.records[0].switches, uint8_t{0});
  Loan l;
  CHECK(!rig.book.findLoan("SW-1", &l));  // 貸出は作らない (返却のまま)
}

TEST(session_cancel_does_nothing_outside_collecting) {
  Rig rig;
  CHECK(!rig.session.canCancel());
  SessionOutput out = rig.session.cancel(kT, kE0);  // セッション無し
  CHECK(!out.cancelled);
  CHECK_EQ(out.record_count, uint8_t{0});

  rig.book.checkout("R-1", kUserB, kE0 - 600, nullptr);
  rig.session.onCode(qr("R-1"), kT + kS, kE0 + 1);  // 返却だけ (取りやめる物が無い)
  CHECK(!rig.session.canCancel());
  out = rig.session.cancel(kT + 2 * kS, kE0 + 2);
  CHECK(!out.cancelled);
  CHECK_EQ(rig.session.state(), SessionState::Collecting);

  rig.session.onCode(qr(kUserA), kT + 3 * kS, kE0 + 3);  // 名札先
  CHECK_EQ(rig.session.state(), SessionState::TagWindow);
  CHECK(!rig.session.canCancel());
  out = rig.session.cancel(kT + 4 * kS, kE0 + 4);
  CHECK(!out.cancelled);
  CHECK_EQ(rig.session.state(), SessionState::TagWindow);
}

TEST(session_cancel_while_paused) {
  Rig rig;
  rig.session.pause(10 * 60 * kS, kT, kE0);
  rig.session.onCode(qr("X-1"), kT + kS, kE0 + 1);
  CHECK(rig.session.canCancel());
  const SessionOutput out = rig.session.cancel(kT + 2 * kS, kE0 + 2);
  CHECK(out.cancelled);
  CHECK_EQ(out.record_count, uint8_t{0});
  CHECK_EQ(rig.session.state(), SessionState::Idle);
  CHECK(rig.session.isPaused());  // 一時停止はそのまま
}

// --- 返却完了 ---
// 2026-09-16: 返却だけのセッションは 20 秒で閉じるが、すぐ貸し出したいことがあるので「返却完了」ですぐ終える

TEST(session_complete_return_ends_immediately) {
  Rig rig;
  rig.book.checkout("R-1", kUserA, kE0 - 600, nullptr);
  rig.session.onDrawerOpened(kT, kE0, "p1.jpg");
  rig.session.onCode(qr("R-1"), kT + kS, kE0 + 1);
  CHECK(rig.session.canCompleteReturn());
  const SessionOutput out = rig.session.completeReturn(kT + 2 * kS, kE0 + 2);
  CHECK_EQ(out.record_count, uint8_t{1});
  const SessionRecord rec = out.records[0];
  CHECK_EQ(rec.end, SessionEnd::Confirmed);
  CHECK(rec.opened);
  CHECK_EQ(rec.start_epoch, kE0);
  CHECK_EQ(str(rec.photo), str("p1.jpg"));
  CHECK_EQ(str(rec.user), str(""));
  CHECK_EQ(rec.returns, uint8_t{1});
  CHECK(!rec.unregistered_open);
  CHECK_EQ(rig.session.state(), SessionState::Idle);
  CHECK(!rig.session.canCompleteReturn());
}

TEST(session_complete_return_then_checkout_same_item) {
  Rig rig;
  rig.book.checkout("R-1", kUserA, kE0 - 600, nullptr);
  rig.session.onCode(qr("R-1"), kT, kE0);
  CHECK_EQ(rig.session.completeReturn(kT + kS, kE0 + 1).record_count, uint8_t{1});
  rig.session.onCode(qr("R-1"), kT + 4 * kS, kE0 + 4);  // 3 秒より後にもう一度
  CHECK_EQ(rig.session.state(), SessionState::Collecting);
  const SessionRow r = row(rig.session, 0);
  CHECK_EQ(r.kind, RowKind::Checkout);
  CHECK_EQ(r.status, RowStatus::Pending);
  rig.session.onCode(qr(kUserB), kT + 5 * kS, kE0 + 5);
  Loan l;
  CHECK(rig.book.findLoan("R-1", &l));
  CHECK_EQ(str(l.user), str(kUserB));
}

TEST(session_complete_return_not_available_otherwise) {
  Rig rig;
  CHECK(!rig.session.canCompleteReturn());
  SessionOutput out = rig.session.completeReturn(kT, kE0);  // セッション無し
  CHECK_EQ(out.record_count, uint8_t{0});

  rig.session.onDrawerOpened(kT, kE0, nullptr);  // 開けて何も読んでいない
  CHECK(!rig.session.canCompleteReturn());
  out = rig.session.completeReturn(kT + kS, kE0 + 1);
  CHECK_EQ(out.record_count, uint8_t{0});
  CHECK_EQ(rig.session.state(), SessionState::Collecting);

  rig.book.checkout("R-1", kUserA, kE0 - 600, nullptr);
  rig.session.onCode(qr("R-1"), kT + 2 * kS, kE0 + 2);
  rig.session.onCode(qr("X-1"), kT + 3 * kS, kE0 + 3);  // 名札待ちの持出がある
  CHECK(!rig.session.canCompleteReturn());
  out = rig.session.completeReturn(kT + 4 * kS, kE0 + 4);
  CHECK_EQ(out.record_count, uint8_t{0});
  CHECK_EQ(rig.session.state(), SessionState::Collecting);

  rig.session.onCode(qr(kUserB), kT + 5 * kS, kE0 + 5);  // 名札先
  CHECK_EQ(rig.session.state(), SessionState::TagWindow);
  CHECK(!rig.session.canCompleteReturn());
  out = rig.session.completeReturn(kT + 6 * kS, kE0 + 6);
  CHECK_EQ(out.record_count, uint8_t{0});
  CHECK_EQ(rig.session.state(), SessionState::TagWindow);
}

TEST(session_complete_return_with_only_failed_return) {
  Rig rig;
  rig.book.checkout("R-1", kUserA, kE0 - 600, nullptr);
  rig.kv.fail_after_puts = rig.kv.puts;
  rig.session.onCode(qr("R-1"), kT, kE0);
  rig.kv.fail_after_puts = -1;
  CHECK(rig.session.canCompleteReturn());
  const SessionOutput out = rig.session.completeReturn(kT + kS, kE0 + 1);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(out.records[0].failures, uint8_t{1});
  CHECK_EQ(rig.session.state(), SessionState::Idle);
}

// --- 完了 (名札先) ---
// 2026-09-16:「続けて物品を読めます」の画面 (名札先の 20 秒) でも、すぐ次の人を登録したいので「完了」で終える

TEST(session_complete_tag_window_ends_immediately) {
  Rig rig;
  rig.session.onDrawerOpened(kT, kE0, "p1.jpg");
  rig.session.onCode(qr("X-1"), kT + kS, kE0 + 1);
  rig.session.onCode(qr(kUserA), kT + 2 * kS, kE0 + 2);
  CHECK_EQ(rig.session.state(), SessionState::TagWindow);
  CHECK(rig.session.canCompleteTagWindow());
  const SessionOutput out = rig.session.completeTagWindow(kT + 3 * kS, kE0 + 3);
  CHECK_EQ(out.record_count, uint8_t{1});
  const SessionRecord rec = out.records[0];
  CHECK_EQ(rec.end, SessionEnd::Confirmed);
  CHECK(rec.opened);
  CHECK_EQ(rec.start_epoch, kE0);
  CHECK_EQ(str(rec.photo), str("p1.jpg"));
  CHECK_EQ(str(rec.user), str(kUserA));
  CHECK_EQ(rec.checkouts, uint8_t{1});
  CHECK_EQ(rig.session.state(), SessionState::Idle);
  CHECK(!rig.session.canCompleteTagWindow());
}

TEST(session_complete_tag_window_with_multiple_rows_and_return) {
  Rig rig;
  rig.book.checkout("R-1", kUserB, kE0 - 600, nullptr);
  rig.session.onCode(qr("R-1"), kT, kE0);        // 読んだ時点で返却済み
  rig.session.onCode(qr("X-1"), kT + kS, kE0 + 1);
  rig.session.onCode(qr(kUserA), kT + 2 * kS, kE0 + 2);
  const SessionOutput out = rig.session.completeTagWindow(kT + 3 * kS, kE0 + 3);
  CHECK_EQ(out.record_count, uint8_t{1});
  CHECK_EQ(out.records[0].checkouts, uint8_t{1});
  CHECK_EQ(out.records[0].returns, uint8_t{1});
}

TEST(session_complete_tag_window_not_available_otherwise) {
  Rig rig;
  CHECK(!rig.session.canCompleteTagWindow());
  SessionOutput out = rig.session.completeTagWindow(kT, kE0);  // セッション無し
  CHECK_EQ(out.record_count, uint8_t{0});

  rig.session.onCode(qr("X-1"), kT + kS, kE0 + 1);  // 名札待ち (集め中。まだ TagWindow でない)
  CHECK(!rig.session.canCompleteTagWindow());
  out = rig.session.completeTagWindow(kT + 2 * kS, kE0 + 2);
  CHECK_EQ(out.record_count, uint8_t{0});
  CHECK_EQ(rig.session.state(), SessionState::Collecting);

  rig.book.checkout("R-1", kUserA, kE0 - 600, nullptr);
  rig.session.onCode(qr("R-1"), kT + 3 * kS, kE0 + 3);  // 返却だけ
  CHECK(!rig.session.canCompleteTagWindow());
  out = rig.session.completeTagWindow(kT + 4 * kS, kE0 + 4);
  CHECK_EQ(out.record_count, uint8_t{0});
}

TEST(session_complete_tag_window_allows_immediate_next_registration) {
  Rig rig;
  rig.session.onCode(qr("X-1"), kT, kE0);
  rig.session.onCode(qr(kUserA), kT + kS, kE0 + 1);
  CHECK_EQ(rig.session.completeTagWindow(kT + 2 * kS, kE0 + 2).record_count, uint8_t{1});
  const SessionOutput next = rig.session.onCode(qr("Y-2"), kT + 3 * kS, kE0 + 3);
  CHECK(!next.ignored);
  CHECK_EQ(rig.session.state(), SessionState::Collecting);
  CHECK_EQ(rig.session.rowCount(), size_t{1});
}
