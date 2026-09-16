#include "session.h"

#include <cstddef>
#include <cstring>

namespace toolcheck {
namespace {

// src (nullptr は空) を dst に写す。収まらない分は切り、残りは 0 で埋める
void copyText(char* dst, size_t dst_len, const char* src) {
  std::memset(dst, 0, dst_len);
  if (src == nullptr) return;
  const size_t n = std::strlen(src);
  std::memcpy(dst, src, (n < dst_len) ? n : dst_len - 1);
}

uint32_t secToMs(uint16_t sec) { return static_cast<uint32_t>(sec) * 1000u; }

}  // namespace

Session::Session(LoanBook& book, const Config& cfg) : book_(book), cfg_(cfg) {}

void Session::setConfig(const Config& cfg) { cfg_ = cfg; }

SessionOutput Session::onDrawerOpened(uint32_t now_ms, int64_t epoch, const char* photo_name) {
  SessionOutput out;
  expireTimers(now_ms, epoch, &out);
  out.capture_photo = true;

  if (state_ == SessionState::Collecting) {
    // 同じセッションの中での開け直し: 読み直しにする。写真は最初の開放のものを使う
    progress_ms_ = now_ms;
    if (!record_.opened) {
      record_.opened = true;
      copyText(record_.photo, kPhotoFieldLen, photo_name);
    }
    return out;
  }
  if (state_ == SessionState::TagWindow) endSession(SessionEnd::Confirmed, &out);  // 閉 → 開で次の開放

  if (paused_) {
    // 一時停止中はセッションを起こさず、開放を開閉ログに残すだけ
    SessionRecord record;
    record.end = SessionEnd::PausedOpen;
    record.start_epoch = epoch;
    record.opened = true;
    copyText(record.photo, kPhotoFieldLen, photo_name);
    addRecord(&out, record);
    return out;
  }
  startSession(now_ms, epoch, true, photo_name);
  return out;
}

SessionOutput Session::onCode(const ClassifiedCode& code, uint32_t now_ms, int64_t epoch, const char* photo_name) {
  SessionOutput out;
  expireTimers(now_ms, epoch, &out);
  if (code.kind == CodeKind::Invalid || isDuplicate(code.text, now_ms)) {
    out.ignored = true;
    return out;
  }

  if (code.kind == CodeKind::User) {
    if (state_ == SessionState::Collecting) {
      // 引き出しセンサを使わない設定では開放が無いので、名札で確定したときに撮る。
      // 行を記録する前に写真名を決める (名札先に読んだ物品も同じ写真名で記録する)
      if (!cfg_.drawer_sensor_enabled && photo_name != nullptr && record_.photo[0] == '\0') {
        copyText(record_.photo, kPhotoFieldLen, photo_name);
        out.capture_photo = true;
      }
      // 確定: 名札待ちの行 (持出・切替) をこの人で記録し、名札先の 20 秒に入る
      for (SessionRow& row : rows_) {
        if (row.status == RowStatus::Pending) commitRow(&row, code.text, epoch);
      }
      copyText(record_.user, kCodeFieldLen, code.text);
      copyText(tag_user_, kCodeFieldLen, code.text);
      state_ = SessionState::TagWindow;
      tag_ms_ = now_ms;
      return out;
    }
    if (state_ == SessionState::TagWindow) {
      if (std::strcmp(tag_user_, code.text) == 0) {
        tag_ms_ = now_ms;  // 同じ名札: そこから 20 秒を数え直す
        return out;
      }
      endSession(SessionEnd::Confirmed, &out);  // 別の人の名札: ここで区切る
    }
    // セッションの外の名札: 引き出しの開閉に関係なく、その人の貸出中一覧だけ出す (記録しない)
    out.show_user_loans = true;
    copyText(out.loans_user, kCodeFieldLen, code.text);
    return out;
  }

  // 物品
  if (rowIndexOf(code.text) >= 0) {
    // このセッションで扱い済み。持出と返却を入れ替えない (有効な QR なので警告のタイマは読み直す)
    out.ignored = true;
    if (state_ == SessionState::Collecting) progress_ms_ = now_ms;
    return out;
  }
  if (rows_.size() >= kMaxSessionRows) {
    out.ignored = true;
    out.rows_full = true;
    return out;
  }

  SessionRow row;
  copyText(row.item, kCodeFieldLen, code.text);
  Loan loan;
  if (book_.findLoan(code.text, &loan)) {
    row.kind = RowKind::Return;
    std::memcpy(row.borrower, loan.user, kCodeFieldLen);
  }

  if (state_ == SessionState::Idle) startSession(now_ms, epoch, false, nullptr);
  if (row.kind == RowKind::Return) {
    commitRow(&row, nullptr, epoch);  // 返却は読んだ時点で書く。名札は要らず、返した人は記録しない (2026-09-15)
  } else if (state_ == SessionState::TagWindow) {
    commitRow(&row, tag_user_, epoch);  // 名札先: その人で即確定 (20 秒は延ばさない)
  }
  if (state_ == SessionState::Collecting) progress_ms_ = now_ms;
  rows_.push_back(row);
  return out;
}

SessionOutput Session::tick(uint32_t now_ms, int64_t epoch) {
  SessionOutput out;
  expireTimers(now_ms, epoch, &out);
  return out;
}

bool Session::removeRow(size_t index) {
  if (state_ != SessionState::Collecting || index >= rows_.size() || rows_[index].status != RowStatus::Pending ||
      rows_[index].kind != RowKind::Checkout) {
    return false;
  }
  rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(index));
  // QR だけで始めたセッションで何も残らなければ、記録せずに終える (開放があれば名札が要る)
  if (rows_.empty() && !record_.opened) resetSession();
  return true;
}

bool Session::canToggleSwitch(size_t index) const {
  if (index >= rows_.size()) return false;
  const SessionRow& row = rows_[index];
  const bool done_return = row.kind == RowKind::Return && row.status == RowStatus::Done;
  if (state_ == SessionState::Collecting) {
    return done_return || (row.kind == RowKind::Switch && row.status == RowStatus::Pending);
  }
  return state_ == SessionState::TagWindow && done_return;  // 名札先で書いた切替は戻せない
}

bool Session::toggleSwitch(size_t index, int64_t epoch) {
  if (!canToggleSwitch(index)) return false;
  SessionRow& row = rows_[index];
  if (row.kind == RowKind::Return) {
    // 返却 (記録済み) → 持出に切替。開閉ログの数は行の見た目に合わせる
    row.kind = RowKind::Switch;
    row.status = RowStatus::Pending;
    row.result = BookResult::Ok;
    if (record_.returns > 0) --record_.returns;
    if (state_ == SessionState::TagWindow) commitRow(&row, tag_user_, epoch);  // 名札先: その人にすぐ貸し出す
    return true;
  }
  // 名札待ちの切替 → 返却に戻す。返却はもう書いてあるので本は書き直さない
  row.kind = RowKind::Return;
  row.status = RowStatus::Done;
  ++record_.returns;
  return true;
}

SessionOutput Session::cancel(uint32_t now_ms, int64_t epoch) {
  SessionOutput out;
  expireTimers(now_ms, epoch, &out);
  if (!canCancel()) return out;
  // 名札待ちの持出は消し、名札待ちの切替は返却 (記録済み) に戻す。済んだ行はそのまま
  std::vector<SessionRow> kept;
  for (SessionRow& row : rows_) {
    if (row.status == RowStatus::Pending) {
      if (row.kind != RowKind::Switch) continue;
      row.kind = RowKind::Return;
      row.status = RowStatus::Done;
      ++record_.returns;
    }
    kept.push_back(row);
  }
  rows_.swap(kept);
  out.cancelled = true;
  // 開けた後か済んだ行があれば開閉ログに残す (赤帯にはしない)。QR だけで何も済んでいなければ何も残さない
  if (record_.opened || !rows_.empty()) {
    endSession(SessionEnd::Cancelled, &out);
  } else {
    resetSession();
  }
  return out;
}

bool Session::canCancel() const { return isCounting(); }

SessionOutput Session::completeReturn(uint32_t now_ms, int64_t epoch) {
  SessionOutput out;
  expireTimers(now_ms, epoch, &out);
  if (!canCompleteReturn()) return out;
  endSession(SessionEnd::Confirmed, &out);  // 返却だけを 20 秒待たずに終える (20 秒で終えるのと同じ 1 件)
  return out;
}

// 集め中で警告を数えていない = 行があって、名札待ちの行が無い (返却だけ)
bool Session::canCompleteReturn() const { return state_ == SessionState::Collecting && !isCounting(); }

SessionOutput Session::completeTagWindow(uint32_t now_ms, int64_t epoch) {
  SessionOutput out;
  expireTimers(now_ms, epoch, &out);  // ちょうど 20 秒に達していれば、ここで既に終わっている
  if (state_ != SessionState::TagWindow) return out;
  endSession(SessionEnd::Confirmed, &out);  // 名札先の 20 秒を待たずに終える (20 秒で終えるのと同じ 1 件)
  return out;
}

bool Session::canCompleteTagWindow() const { return state_ == SessionState::TagWindow; }

bool Session::isCounting() const {
  if (state_ != SessionState::Collecting) return false;
  if (rows_.empty()) return true;  // 開けて何も読んでいない
  for (const SessionRow& row : rows_) {
    if (row.status == RowStatus::Pending) return true;
  }
  return false;  // 返却だけ
}

SessionOutput Session::pause(uint32_t duration_ms, uint32_t now_ms, int64_t epoch) {
  SessionOutput out;
  expireTimers(now_ms, epoch, &out);
  paused_ = true;
  pause_start_ms_ = now_ms;
  pause_duration_ms_ = duration_ms;
  out.pause_started = true;
  return out;
}

SessionOutput Session::resume(uint32_t now_ms, int64_t epoch) {
  SessionOutput out;
  expireTimers(now_ms, epoch, &out);
  if (paused_) endPause(now_ms, &out);
  return out;
}

SessionState Session::state() const { return state_; }

uint8_t Session::alarmLevel(uint32_t now_ms) const {
  if (!isCounting() || paused_) return 0;
  const uint32_t elapsed = now_ms - progress_ms_;
  if (elapsed >= secToMs(cfg_.warning2_sec)) return 2;
  if (elapsed >= secToMs(cfg_.warning1_sec)) return 1;
  return 0;
}

uint32_t Session::sinceProgressMs(uint32_t now_ms) const {
  return (state_ == SessionState::Collecting) ? now_ms - progress_ms_ : 0;
}

bool Session::hasOpened() const { return record_.opened; }

const char* Session::tagUser() const { return (state_ == SessionState::TagWindow) ? tag_user_ : ""; }

size_t Session::rowCount() const { return rows_.size(); }

bool Session::getRow(size_t index, SessionRow* out) const {
  if (out == nullptr || index >= rows_.size()) return false;
  *out = rows_[index];
  return true;
}

bool Session::isPaused() const { return paused_; }

uint32_t Session::pauseRemainingMs(uint32_t now_ms) const {
  if (!paused_) return 0;
  const uint32_t elapsed = now_ms - pause_start_ms_;
  return (elapsed >= pause_duration_ms_) ? 0 : pause_duration_ms_ - elapsed;
}

bool Session::isDuplicate(const char* text, uint32_t now_ms) {
  for (RecentCode& recent : recent_) {
    if (recent.used && std::strcmp(recent.text, text) == 0) {
      const bool duplicate = (now_ms - recent.last_seen_ms) < kDuplicateIgnoreMs;
      recent.last_seen_ms = now_ms;  // 読み続けている間は読み捨てを延ばす
      return duplicate;
    }
  }
  // 初めてのコード: 空き枠、無ければ最も長く読んでいない枠に入れる
  RecentCode* slot = nullptr;
  for (RecentCode& recent : recent_) {
    if (!recent.used) {
      slot = &recent;
      break;
    }
    if (slot == nullptr || (now_ms - recent.last_seen_ms) > (now_ms - slot->last_seen_ms)) slot = &recent;
  }
  copyText(slot->text, kCodeFieldLen, text);
  slot->last_seen_ms = now_ms;
  slot->used = true;
  return false;
}

void Session::expireTimers(uint32_t now_ms, int64_t epoch, SessionOutput* out) {
  if (paused_ && (now_ms - pause_start_ms_) >= pause_duration_ms_) endPause(now_ms, out);
  if (state_ == SessionState::TagWindow && (now_ms - tag_ms_) >= kTagWindowMs) endSession(SessionEnd::Confirmed, out);
  if (state_ != SessionState::Collecting) return;
  if (!isCounting()) {
    // 返却だけ: 名札は要らない。最後に進んでから 20 秒で終える (一時停止中も同じ)
    if ((now_ms - progress_ms_) >= kReturnOnlyEndMs) endSession(SessionEnd::Confirmed, out);
  } else if (!paused_ && (now_ms - progress_ms_) >= secToMs(cfg_.warning2_sec) + secToMs(cfg_.giveup_sec)) {
    giveUp(epoch, out);
  }
}

void Session::startSession(uint32_t now_ms, int64_t epoch, bool opened, const char* photo) {
  resetSession();
  state_ = SessionState::Collecting;
  progress_ms_ = now_ms;
  record_.start_epoch = epoch;
  record_.opened = opened;
  copyText(record_.photo, kPhotoFieldLen, photo);
}

void Session::endSession(SessionEnd end, SessionOutput* out) {
  record_.end = end;
  addRecord(out, record_);
  resetSession();
}

void Session::resetSession() {
  state_ = SessionState::Idle;
  rows_.clear();
  record_ = SessionRecord{};
  tag_user_[0] = '\0';
}

void Session::endPause(uint32_t now_ms, SessionOutput* out) {
  paused_ = false;
  out->pause_ended = true;
  if (state_ == SessionState::Collecting) progress_ms_ = now_ms;  // 停止が明けた時点から警告を数え直す
}

void Session::commitRow(SessionRow* row, const char* user, int64_t epoch) {
  BookResult result = BookResult::Ok;
  switch (row->kind) {
    case RowKind::Checkout:
      result = book_.checkout(row->item, user, epoch, record_.photo);
      break;
    case RowKind::Return:
      result = book_.returnItem(row->item, nullptr, epoch, record_.photo);  // 返した人は記録しない
      break;
    case RowKind::Switch:
      result = book_.checkout(row->item, user, epoch, record_.photo, true);  // 返却は読んだ時点で書いてある
      break;
  }
  row->result = result;
  if (result != BookResult::Ok) {
    row->status = RowStatus::Failed;
    ++record_.failures;
    return;
  }
  row->status = RowStatus::Done;
  switch (row->kind) {
    case RowKind::Checkout:
      ++record_.checkouts;
      break;
    case RowKind::Return:
      ++record_.returns;
      break;
    case RowKind::Switch:
      ++record_.switches;
      break;
  }
}

void Session::giveUp(int64_t epoch, SessionOutput* out) {
  // 打ち切り: 名札待ちの持出・切替を利用者「不明」で記録する (返却は読んだ時点で記録済み)
  bool unknown_checkout = false;
  for (SessionRow& row : rows_) {
    if (row.status != RowStatus::Pending) continue;
    unknown_checkout = true;
    commitRow(&row, nullptr, epoch);
  }
  record_.unregistered_open = record_.opened && rows_.empty();
  record_.unknown_checkout = unknown_checkout;
  endSession(SessionEnd::GaveUp, out);
}

int Session::rowIndexOf(const char* item) const {
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (std::strcmp(rows_[i].item, item) == 0) return static_cast<int>(i);
  }
  return -1;
}

void Session::addRecord(SessionOutput* out, const SessionRecord& record) {
  const size_t capacity = sizeof(out->records) / sizeof(out->records[0]);
  if (out->record_count < capacity) {
    out->records[out->record_count] = record;
    ++out->record_count;
  }
}

}  // namespace toolcheck
