// screen_state: どの画面を出すか (docs/設計.md「画面」、2026-09-13 決定: ページ送り / 名札の一覧は 10 秒 / 放置 2 分で貸出中)
#include <cstdint>
#include <string>

#include "core/screen_state.h"
#include "testing.h"

using toolcheck::kIdleReturnMs;
using toolcheck::kUserLoansShowMs;
using toolcheck::ScreenState;
using toolcheck::Tab;
using toolcheck::View;

namespace {

const uint32_t kT0 = 1000;

std::string str(const char* s) { return std::string(s); }

}  // namespace

// --- タブとページ ---

TEST(screen_starts_on_loans_first_page) {
  ScreenState s(kT0);
  CHECK_EQ(s.view(), View::Tab);
  CHECK_EQ(s.tab(), Tab::Loans);
  CHECK_EQ(s.page(), uint16_t{0});
  CHECK_EQ(s.pageCount(), uint16_t{1});
  CHECK_EQ(str(s.userLoansUser()), str(""));
  CHECK_EQ(str(s.photoName()), str(""));
}

TEST(screen_page_count_rounds_up_and_is_at_least_one) {
  ScreenState s(kT0);
  s.setListSize(0, 8);
  CHECK_EQ(s.pageCount(), uint16_t{1});
  s.setListSize(8, 8);
  CHECK_EQ(s.pageCount(), uint16_t{1});
  s.setListSize(9, 8);
  CHECK_EQ(s.pageCount(), uint16_t{2});
  s.setListSize(200, 8);
  CHECK_EQ(s.pageCount(), uint16_t{25});
}

TEST(screen_zero_rows_per_page_is_one_row) {
  ScreenState s(kT0);
  s.setListSize(3, 0);
  CHECK_EQ(s.pageCount(), uint16_t{3});
}

TEST(screen_next_and_prev_page_stop_at_ends) {
  ScreenState s(kT0);
  s.setListSize(20, 8);  // 3 ページ
  CHECK(!s.prevPage(kT0));
  CHECK(s.nextPage(kT0));
  CHECK(s.nextPage(kT0));
  CHECK_EQ(s.page(), uint16_t{2});
  CHECK(!s.nextPage(kT0));
  CHECK_EQ(s.page(), uint16_t{2});
  CHECK(s.prevPage(kT0));
  CHECK_EQ(s.page(), uint16_t{1});
}

TEST(screen_shrinking_list_moves_page_to_last) {
  ScreenState s(kT0);
  s.setListSize(20, 8);
  s.nextPage(kT0);
  s.nextPage(kT0);
  s.setListSize(9, 8);  // 2 ページに減った
  CHECK_EQ(s.page(), uint16_t{1});
  s.setListSize(0, 8);
  CHECK_EQ(s.page(), uint16_t{0});
  CHECK_EQ(s.pageCount(), uint16_t{1});
}

TEST(screen_select_tab_goes_to_first_page) {
  ScreenState s(kT0);
  s.setListSize(20, 8);
  s.nextPage(kT0);
  s.selectTab(Tab::History, kT0);
  CHECK_EQ(s.tab(), Tab::History);
  CHECK_EQ(s.page(), uint16_t{0});
  CHECK_EQ(s.view(), View::Tab);
}

TEST(screen_select_current_tab_returns_to_first_page) {
  ScreenState s(kT0);
  s.setListSize(20, 8);
  s.nextPage(kT0);
  s.selectTab(Tab::Loans, kT0);
  CHECK_EQ(s.page(), uint16_t{0});
}

// --- 名札だけの一覧 ---

TEST(screen_user_loans_shows_for_10_seconds) {
  ScreenState s(kT0);
  s.showUserLoans("1234567", kT0);
  CHECK_EQ(s.view(), View::UserLoans);
  CHECK_EQ(str(s.userLoansUser()), str("1234567"));
  s.tick(kT0 + kUserLoansShowMs - 1);
  CHECK_EQ(s.view(), View::UserLoans);
  s.tick(kT0 + kUserLoansShowMs);
  CHECK_EQ(s.view(), View::Tab);
  CHECK_EQ(str(s.userLoansUser()), str(""));
}

TEST(screen_user_loans_closes_on_dismiss) {
  ScreenState s(kT0);
  s.showUserLoans("1234567", kT0);
  s.dismiss(kT0 + 100);
  CHECK_EQ(s.view(), View::Tab);
}

TEST(screen_user_loans_again_restarts_timer_and_replaces_user) {
  ScreenState s(kT0);
  s.showUserLoans("1234567", kT0);
  s.showUserLoans("7654321", kT0 + 8000);
  CHECK_EQ(str(s.userLoansUser()), str("7654321"));
  s.tick(kT0 + 8000 + kUserLoansShowMs - 1);
  CHECK_EQ(s.view(), View::UserLoans);
  s.tick(kT0 + 8000 + kUserLoansShowMs);
  CHECK_EQ(s.view(), View::Tab);
}

TEST(screen_long_user_and_photo_names_are_cut_safely) {
  ScreenState s(kT0);
  s.showUserLoans("1234567890123456789", kT0);
  CHECK_EQ(str(s.userLoansUser()), str("123456789012345"));  // 15 文字
  s.openPhoto("20260913-101500-2.jpg.....................long", kT0);
  CHECK_EQ(str(s.photoName()).size(), size_t{31});
}

// --- 写真 ---

TEST(screen_photo_opens_and_dismisses) {
  ScreenState s(kT0);
  s.openPhoto("20260913-101500.jpg", kT0);
  CHECK_EQ(s.view(), View::Photo);
  CHECK_EQ(str(s.photoName()), str("20260913-101500.jpg"));
  s.dismiss(kT0 + 100);
  CHECK_EQ(s.view(), View::Tab);
  CHECK_EQ(str(s.photoName()), str(""));
}

TEST(screen_photo_replaces_user_loans) {
  ScreenState s(kT0);
  s.showUserLoans("1234567", kT0);
  s.openPhoto("a.jpg", kT0 + 10);
  CHECK_EQ(s.view(), View::Photo);
  s.tick(kT0 + kUserLoansShowMs + 10);  // 名札の一覧の時間切れで写真は閉じない
  CHECK_EQ(s.view(), View::Photo);
}

TEST(screen_select_tab_closes_overlays) {
  ScreenState s(kT0);
  s.openPhoto("a.jpg", kT0);
  s.selectTab(Tab::OpenLog, kT0 + 10);
  CHECK_EQ(s.view(), View::Tab);
  CHECK_EQ(s.tab(), Tab::OpenLog);
  s.showUserLoans("1234567", kT0 + 20);
  s.selectTab(Tab::Loans, kT0 + 30);
  CHECK_EQ(s.view(), View::Tab);
}

// --- セッション ---

TEST(screen_session_start_closes_user_loans_and_photo) {
  ScreenState s(kT0);
  s.showUserLoans("1234567", kT0);
  s.setSessionActive(true, kT0 + 10);
  CHECK_EQ(s.view(), View::Session);
  s.setSessionActive(false, kT0 + 20);
  CHECK_EQ(s.view(), View::Tab);  // 名札の一覧には戻らない

  s.openPhoto("a.jpg", kT0 + 30);
  s.setSessionActive(true, kT0 + 40);
  s.setSessionActive(false, kT0 + 50);
  CHECK_EQ(s.view(), View::Tab);
}

TEST(screen_session_end_keeps_tab_and_page) {
  ScreenState s(kT0);
  s.selectTab(Tab::History, kT0);
  s.setListSize(20, 8);
  s.nextPage(kT0);
  s.setSessionActive(true, kT0 + 10);
  s.setSessionActive(false, kT0 + 20);
  CHECK_EQ(s.tab(), Tab::History);
  CHECK_EQ(s.page(), uint16_t{1});
}

TEST(screen_user_loans_and_photo_ignored_during_session) {
  ScreenState s(kT0);
  s.setSessionActive(true, kT0);
  s.showUserLoans("1234567", kT0 + 10);
  s.openPhoto("a.jpg", kT0 + 20);
  CHECK_EQ(s.view(), View::Session);
  s.setSessionActive(false, kT0 + 30);
  CHECK_EQ(s.view(), View::Tab);
}

// --- 無操作で貸出中に戻る ---

TEST(screen_idle_two_minutes_returns_to_loans_first_page) {
  ScreenState s(kT0);
  s.selectTab(Tab::OpenLog, kT0);
  s.setListSize(200, 8);
  s.nextPage(kT0);
  s.tick(kT0 + kIdleReturnMs - 1);
  CHECK_EQ(s.tab(), Tab::OpenLog);
  s.tick(kT0 + kIdleReturnMs);
  CHECK_EQ(s.tab(), Tab::Loans);
  CHECK_EQ(s.page(), uint16_t{0});
  CHECK_EQ(s.view(), View::Tab);
}

TEST(screen_idle_returns_loans_tab_to_first_page) {
  ScreenState s(kT0);
  s.setListSize(20, 8);
  s.nextPage(kT0);
  s.tick(kT0 + kIdleReturnMs);
  CHECK_EQ(s.page(), uint16_t{0});
}

TEST(screen_idle_closes_photo) {
  ScreenState s(kT0);
  s.openPhoto("a.jpg", kT0);
  s.tick(kT0 + kIdleReturnMs);
  CHECK_EQ(s.view(), View::Tab);
}

TEST(screen_touch_and_page_restart_idle) {
  ScreenState s(kT0);
  s.selectTab(Tab::History, kT0);
  s.onTouch(kT0 + 60000);
  s.tick(kT0 + kIdleReturnMs);
  CHECK_EQ(s.tab(), Tab::History);
  s.setListSize(20, 8);
  s.nextPage(kT0 + 150000);
  s.tick(kT0 + 60000 + kIdleReturnMs);
  CHECK_EQ(s.tab(), Tab::History);
  s.tick(kT0 + 150000 + kIdleReturnMs);
  CHECK_EQ(s.tab(), Tab::Loans);
}

TEST(screen_no_idle_return_during_session_and_counts_from_session_end) {
  ScreenState s(kT0);
  s.selectTab(Tab::History, kT0);
  s.setSessionActive(true, kT0 + 1000);
  s.tick(kT0 + 1000 + kIdleReturnMs * 2);
  CHECK_EQ(s.view(), View::Session);
  const uint32_t end = kT0 + 1000 + kIdleReturnMs * 2;
  s.setSessionActive(false, end);
  s.tick(end + kIdleReturnMs - 1);
  CHECK_EQ(s.tab(), Tab::History);
  s.tick(end + kIdleReturnMs);
  CHECK_EQ(s.tab(), Tab::Loans);
}

TEST(screen_user_loans_counts_as_activity) {
  ScreenState s(kT0);
  s.selectTab(Tab::History, kT0);
  s.showUserLoans("1234567", kT0 + 100000);
  s.tick(kT0 + kIdleReturnMs);
  CHECK_EQ(s.tab(), Tab::History);
}

TEST(screen_timers_work_across_millis_wrap) {
  const uint32_t start = 0xFFFFF000u;
  ScreenState s(start);
  s.showUserLoans("1234567", start);
  s.tick(start + kUserLoansShowMs - 1);  // 一周している
  CHECK_EQ(s.view(), View::UserLoans);
  s.tick(start + kUserLoansShowMs);
  CHECK_EQ(s.view(), View::Tab);
  s.selectTab(Tab::Search, start);
  s.tick(start + kIdleReturnMs - 1);
  CHECK_EQ(s.tab(), Tab::Search);
  s.tick(start + kIdleReturnMs);
  CHECK_EQ(s.tab(), Tab::Loans);
}

// --- 設定画面 ---

TEST(screen_settings_open_and_close_keeps_tab) {
  ScreenState s(kT0);
  s.selectTab(Tab::History, kT0);
  s.setListSize(20, 8);
  s.nextPage(kT0);
  s.openSettings(kT0 + 10);
  CHECK_EQ(s.view(), View::Settings);
  s.closeSettings(kT0 + 20);
  CHECK_EQ(s.view(), View::Tab);
  CHECK_EQ(s.tab(), Tab::History);
  CHECK_EQ(s.page(), uint16_t{1});
}

TEST(screen_settings_closes_user_loans_and_photo) {
  ScreenState s(kT0);
  s.showUserLoans("1234567", kT0);
  s.openSettings(kT0 + 10);
  CHECK_EQ(s.view(), View::Settings);
  CHECK_EQ(str(s.userLoansUser()), str(""));
  s.closeSettings(kT0 + 20);
  CHECK_EQ(s.view(), View::Tab);  // 名札の一覧には戻らない
}

TEST(screen_settings_not_dismissed_by_tap) {
  ScreenState s(kT0);
  s.openSettings(kT0);
  s.dismiss(kT0 + 10);
  CHECK_EQ(s.view(), View::Settings);
}

TEST(screen_settings_ignored_during_session) {
  ScreenState s(kT0);
  s.setSessionActive(true, kT0);
  s.openSettings(kT0 + 10);
  CHECK_EQ(s.view(), View::Session);
  s.setSessionActive(false, kT0 + 20);
  CHECK_EQ(s.view(), View::Tab);
}

TEST(screen_session_start_closes_settings) {
  ScreenState s(kT0);
  s.openSettings(kT0);
  s.setSessionActive(true, kT0 + 10);
  CHECK_EQ(s.view(), View::Session);
  s.setSessionActive(false, kT0 + 20);
  CHECK_EQ(s.view(), View::Tab);  // 設定画面には戻らない (未保存の変更は捨てる)
}

TEST(screen_idle_two_minutes_closes_settings) {
  ScreenState s(kT0);
  s.selectTab(Tab::OpenLog, kT0);
  s.openSettings(kT0 + 1000);
  s.tick(kT0 + 1000 + kIdleReturnMs - 1);
  CHECK_EQ(s.view(), View::Settings);
  s.tick(kT0 + 1000 + kIdleReturnMs);
  CHECK_EQ(s.view(), View::Tab);
  CHECK_EQ(s.tab(), Tab::Loans);
}

TEST(screen_settings_touch_restarts_idle) {
  ScreenState s(kT0);
  s.openSettings(kT0);
  s.onTouch(kT0 + 100000);
  s.tick(kT0 + kIdleReturnMs);
  CHECK_EQ(s.view(), View::Settings);
}

TEST(screen_settings_revision) {
  ScreenState s(kT0);
  uint32_t r = s.revision();
  s.openSettings(kT0);
  CHECK(s.revision() != r);
  r = s.revision();
  s.openSettings(kT0 + 10);  // もう開いている
  CHECK_EQ(s.revision(), r);
  s.closeSettings(kT0 + 20);
  CHECK(s.revision() != r);
  r = s.revision();
  s.closeSettings(kT0 + 30);  // もう閉じている
  CHECK_EQ(s.revision(), r);
}

// --- 描き直しの合図 ---

TEST(screen_revision_changes_only_when_something_visible_changes) {
  ScreenState s(kT0);
  uint32_t r = s.revision();
  s.tick(kT0 + 10);
  s.onTouch(kT0 + 20);
  s.setListSize(0, 8);
  CHECK(!s.prevPage(kT0 + 30));
  CHECK_EQ(s.revision(), r);

  s.setListSize(20, 8);  // ページ数が変わった
  CHECK(s.revision() != r);
  r = s.revision();
  s.setListSize(20, 8);
  CHECK_EQ(s.revision(), r);

  s.nextPage(kT0 + 40);
  CHECK(s.revision() != r);
  r = s.revision();
  s.showUserLoans("1234567", kT0 + 50);
  CHECK(s.revision() != r);
  r = s.revision();
  s.tick(kT0 + 50 + kUserLoansShowMs);
  CHECK(s.revision() != r);
  r = s.revision();
  s.tick(kT0 + 50 + kUserLoansShowMs + 10);
  CHECK_EQ(s.revision(), r);
  s.setSessionActive(true, kT0 + 20000);
  CHECK(s.revision() != r);
  r = s.revision();
  s.setSessionActive(true, kT0 + 20010);
  CHECK_EQ(s.revision(), r);
}
