#include "screen_state.h"

#include <cstddef>
#include <cstdio>

namespace toolcheck {
namespace {

void copyText(char* dst, size_t cap, const char* src) { std::snprintf(dst, cap, "%s", src != nullptr ? src : ""); }

}  // namespace

ScreenState::ScreenState(uint32_t now_ms) : last_activity_ms_(now_ms) {}

View ScreenState::view() const {
  if (session_active_) return View::Session;
  if (overlay_ == Overlay::UserLoans) return View::UserLoans;
  if (overlay_ == Overlay::Photo) return View::Photo;
  if (overlay_ == Overlay::Settings) return View::Settings;
  return View::Tab;
}

Tab ScreenState::tab() const { return tab_; }
uint16_t ScreenState::page() const { return page_; }
uint16_t ScreenState::pageCount() const { return page_count_; }

const char* ScreenState::userLoansUser() const { return view() == View::UserLoans ? user_ : ""; }

const char* ScreenState::photoName() const { return view() == View::Photo ? photo_ : ""; }

uint32_t ScreenState::revision() const { return revision_; }

void ScreenState::onTouch(uint32_t now_ms) { touch(now_ms); }

void ScreenState::selectTab(Tab tab, uint32_t now_ms) {
  touch(now_ms);
  const bool changed = tab != tab_ || page_ != 0 || overlay_ != Overlay::None;
  if (tab != tab_) page_count_ = 1;  // 件数は描く側が setListSize で入れ直す
  tab_ = tab;
  page_ = 0;
  setOverlay(Overlay::None);
  if (changed) ++revision_;
}

bool ScreenState::nextPage(uint32_t now_ms) {
  touch(now_ms);
  if (page_ + 1 >= page_count_) return false;
  ++page_;
  ++revision_;
  return true;
}

bool ScreenState::prevPage(uint32_t now_ms) {
  touch(now_ms);
  if (page_ == 0) return false;
  --page_;
  ++revision_;
  return true;
}

void ScreenState::setListSize(uint16_t items, uint16_t rows_per_page) {
  const uint32_t rows = rows_per_page == 0 ? 1 : rows_per_page;
  uint32_t count = (static_cast<uint32_t>(items) + rows - 1) / rows;
  if (count == 0) count = 1;
  const uint16_t new_count = static_cast<uint16_t>(count);
  const uint16_t new_page = page_ < new_count ? page_ : static_cast<uint16_t>(new_count - 1);
  if (new_count != page_count_ || new_page != page_) ++revision_;
  page_count_ = new_count;
  page_ = new_page;
}

void ScreenState::showUserLoans(const char* user, uint32_t now_ms) {
  if (session_active_) return;
  touch(now_ms);
  setOverlay(Overlay::UserLoans);
  copyText(user_, sizeof(user_), user);
  user_shown_ms_ = now_ms;
  ++revision_;
}

void ScreenState::openPhoto(const char* name, uint32_t now_ms) {
  if (session_active_) return;
  touch(now_ms);
  setOverlay(Overlay::Photo);
  copyText(photo_, sizeof(photo_), name);
  ++revision_;
}

void ScreenState::dismiss(uint32_t now_ms) {
  touch(now_ms);
  if (overlay_ == Overlay::None || overlay_ == Overlay::Settings) return;  // 設定画面はタップでは閉じない
  setOverlay(Overlay::None);
  ++revision_;
}

void ScreenState::openSettings(uint32_t now_ms) {
  if (session_active_) return;
  touch(now_ms);
  if (overlay_ == Overlay::Settings) return;
  setOverlay(Overlay::Settings);
  ++revision_;
}

void ScreenState::closeSettings(uint32_t now_ms) {
  touch(now_ms);
  if (overlay_ != Overlay::Settings) return;
  setOverlay(Overlay::None);
  ++revision_;
}

void ScreenState::setSessionActive(bool active, uint32_t now_ms) {
  if (active == session_active_) return;
  touch(now_ms);
  session_active_ = active;
  if (active) setOverlay(Overlay::None);
  ++revision_;
}

void ScreenState::tick(uint32_t now_ms) {
  if (session_active_) return;
  if (overlay_ == Overlay::UserLoans && now_ms - user_shown_ms_ >= kUserLoansShowMs) {
    setOverlay(Overlay::None);
    ++revision_;
  }
  if (now_ms - last_activity_ms_ >= kIdleReturnMs &&
      (tab_ != Tab::Loans || page_ != 0 || overlay_ != Overlay::None)) {
    if (tab_ != Tab::Loans) page_count_ = 1;
    tab_ = Tab::Loans;
    page_ = 0;
    setOverlay(Overlay::None);
    ++revision_;
  }
}

void ScreenState::touch(uint32_t now_ms) { last_activity_ms_ = now_ms; }

void ScreenState::setOverlay(Overlay overlay) {
  overlay_ = overlay;
  if (overlay != Overlay::UserLoans) user_[0] = '\0';
  if (overlay != Overlay::Photo) photo_[0] = '\0';
}

}  // namespace toolcheck
