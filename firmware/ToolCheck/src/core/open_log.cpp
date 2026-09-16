#include "open_log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace toolcheck {
namespace {

const char* const kLogNs = "openlog";
const char kLogPrefix = 'O';
const char* const kAlertNs = "alerts";
const char* const kAlertMetaKey = "meta";
const uint8_t kRecordVersion = 1;
const size_t kAlertMetaSize = 8;
const size_t kKeyLen = 16;

// 開閉ログ 1 件の並び: [版数][種類][印][持出][返却][持出に切替][記録できなかった行][予備][時刻 i64][利用者 ID 16][写真名 32]
const size_t kLogKindAt = 1;
const size_t kLogFlagsAt = 2;
const size_t kLogCheckoutsAt = 3;
const size_t kLogReturnsAt = 4;
const size_t kLogSwitchesAt = 5;
const size_t kLogFailuresAt = 6;
const size_t kLogEpochAt = 8;
const size_t kLogUserAt = 16;
const size_t kLogPhotoAt = 32;
static_assert(kLogPhotoAt + kPhotoFieldLen == kOpenLogRecordSize, "開閉ログ 1 件の並びと大きさが合わない");

const uint8_t kLogOpened = 1 << 0;
const uint8_t kLogGaveUp = 1 << 1;
const uint8_t kLogUnregisteredOpen = 1 << 2;
const uint8_t kLogUnknownCheckout = 1 << 3;
const uint8_t kLogCancelled = 1 << 4;  // 2026-09-15 追加。前からの記録 (bit4 = 0) は取りやめでない

// 赤帯 1 件の並び: [版数][印][予備 2][通し番号 u32][時刻 i64][写真名 32]
const size_t kAlertFlagsAt = 1;
const size_t kAlertSeqAt = 4;
const size_t kAlertEpochAt = 8;
const size_t kAlertPhotoAt = 16;
static_assert(kAlertPhotoAt + kPhotoFieldLen == kAlertRecordSize, "赤帯 1 件の並びと大きさが合わない");

const uint8_t kAlertUnregisteredOpen = 1 << 0;
const uint8_t kAlertUnknownCheckout = 1 << 1;

void putU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>(v >> 8);
}

uint16_t getU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

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

// 欄を保存用に書く (NUL の後ろは 0 で埋める。NUL が無ければ最後の 1 バイトを NUL にする)
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

// 呼び出し側の欄を写す (NUL が無くても最後の 1 バイトを NUL にして収める)
void copyField(char* dst, const char* src, size_t field_len) {
  std::memcpy(dst, src, field_len);
  dst[field_len - 1] = '\0';
}

bool validKind(uint8_t kind) {
  return kind >= static_cast<uint8_t>(OpenLogKind::Session) && kind <= static_cast<uint8_t>(OpenLogKind::PhotosCleared);
}

void encodeEntry(const OpenLogEntry& entry, uint8_t* out) {
  std::memset(out, 0, kOpenLogRecordSize);
  out[0] = kRecordVersion;
  out[kLogKindAt] = static_cast<uint8_t>(entry.kind);
  uint8_t flags = 0;
  if (entry.opened) flags |= kLogOpened;
  if (entry.gave_up) flags |= kLogGaveUp;
  if (entry.unregistered_open) flags |= kLogUnregisteredOpen;
  if (entry.unknown_checkout) flags |= kLogUnknownCheckout;
  if (entry.cancelled) flags |= kLogCancelled;
  out[kLogFlagsAt] = flags;
  out[kLogCheckoutsAt] = entry.checkouts;
  out[kLogReturnsAt] = entry.returns;
  out[kLogSwitchesAt] = entry.switches;
  out[kLogFailuresAt] = entry.failures;
  putI64(out + kLogEpochAt, entry.epoch);
  putField(out + kLogUserAt, entry.user, kCodeFieldLen);
  putField(out + kLogPhotoAt, entry.photo, kPhotoFieldLen);
}

bool decodeEntry(const uint8_t* in, OpenLogEntry* out) {
  if (in[0] != kRecordVersion || !validKind(in[kLogKindAt])) return false;
  OpenLogEntry entry;
  entry.kind = static_cast<OpenLogKind>(in[kLogKindAt]);
  const uint8_t flags = in[kLogFlagsAt];
  entry.opened = (flags & kLogOpened) != 0;
  entry.gave_up = (flags & kLogGaveUp) != 0;
  entry.unregistered_open = (flags & kLogUnregisteredOpen) != 0;
  entry.unknown_checkout = (flags & kLogUnknownCheckout) != 0;
  entry.cancelled = (flags & kLogCancelled) != 0;
  entry.checkouts = in[kLogCheckoutsAt];
  entry.returns = in[kLogReturnsAt];
  entry.switches = in[kLogSwitchesAt];
  entry.failures = in[kLogFailuresAt];
  entry.epoch = getI64(in + kLogEpochAt);
  if (!getField(entry.user, kCodeFieldLen, in + kLogUserAt)) return false;
  if (!getField(entry.photo, kPhotoFieldLen, in + kLogPhotoAt)) return false;
  *out = entry;
  return true;
}

void encodeAlert(const Alert& alert, uint8_t* out) {
  std::memset(out, 0, kAlertRecordSize);
  out[0] = kRecordVersion;
  uint8_t flags = 0;
  if (alert.unregistered_open) flags |= kAlertUnregisteredOpen;
  if (alert.unknown_checkout) flags |= kAlertUnknownCheckout;
  out[kAlertFlagsAt] = flags;
  putU32(out + kAlertSeqAt, alert.seq);
  putI64(out + kAlertEpochAt, alert.epoch);
  putField(out + kAlertPhotoAt, alert.photo, kPhotoFieldLen);
}

bool decodeAlert(const uint8_t* in, Alert* out) {
  if (in[0] != kRecordVersion) return false;
  Alert alert;
  const uint8_t flags = in[kAlertFlagsAt];
  alert.unregistered_open = (flags & kAlertUnregisteredOpen) != 0;
  alert.unknown_checkout = (flags & kAlertUnknownCheckout) != 0;
  alert.seq = getU32(in + kAlertSeqAt);
  if (alert.seq == 0) return false;
  alert.epoch = getI64(in + kAlertEpochAt);
  if (!getField(alert.photo, kPhotoFieldLen, in + kAlertPhotoAt)) return false;
  *out = alert;
  return true;
}

// 赤帯のキー: "A" + 通し番号 (十進 10 桁) = 11 文字
void alertKey(uint32_t seq, char* buf) { std::snprintf(buf, kKeyLen, "A%010lu", static_cast<unsigned long>(seq)); }

struct KeyText {
  char text[kKeyLen] = {0};
};

bool collectAlertKey(const char* key, void* ctx) {
  if (std::strcmp(key, kAlertMetaKey) == 0) return true;
  KeyText k;
  std::snprintf(k.text, sizeof(k.text), "%s", key);
  static_cast<std::vector<KeyText>*>(ctx)->push_back(k);
  return true;
}

}  // namespace

OpenLog::OpenLog(KvStore& kv) : kv_(kv), log_(kv, kLogNs, kLogPrefix, kOpenLogCapacity, kOpenLogRecordSize) {}

OpenLogLoadReport OpenLog::load() {
  OpenLogLoadReport rep;

  // --- 開閉ログ ---
  rep.log_ok = log_.load();
  entries_.assign(log_.count(), CachedEntry{});
  uint8_t record[kOpenLogRecordSize];
  for (uint16_t i = 0; i < log_.count(); ++i) {
    CachedEntry& cached = entries_[i];
    cached.valid = log_.getNewest(i, record) && decodeEntry(record, &cached.entry);
  }

  // --- 赤帯 ---
  alerts_.clear();
  pending_removes_.clear();
  uint32_t metaNext = 1;
  uint16_t metaDropped = 0;
  uint8_t meta[kAlertMetaSize];
  if (kv_.getBlob(kAlertNs, kAlertMetaKey, meta, kAlertMetaSize)) {
    metaNext = getU32(meta);
    metaDropped = getU16(meta + 4);
  }

  std::vector<KeyText> keys;
  if (!kv_.forEachKey(kAlertNs, collectAlertKey, &keys)) rep.alerts_ok = false;
  std::vector<Alert> found;
  uint8_t blob[kAlertRecordSize];
  for (const KeyText& key : keys) {
    Alert alert;
    if (!kv_.getBlob(kAlertNs, key.text, blob, kAlertRecordSize) || !decodeAlert(blob, &alert)) {
      ++rep.skipped_alerts;  // 読めない赤帯は消さずに残す
      continue;
    }
    found.push_back(alert);
  }
  std::sort(found.begin(), found.end(), [](const Alert& a, const Alert& b) { return a.seq > b.seq; });

  const uint32_t maxSeq = found.empty() ? 0 : found.front().seq;
  next_alert_seq_ = std::max(metaNext, maxSeq + 1);
  size_t dropped = metaDropped;
  if (found.size() > kAlertCapacity) {
    // meta を書く前に落ちた赤帯 (通し番号が meta の「次」以上) で溢れたぶんは、まだ数えていない
    size_t unaccounted = 0;
    for (const Alert& alert : found) {
      if (alert.seq >= metaNext) ++unaccounted;
    }
    const size_t excess = found.size() - kAlertCapacity;
    const size_t before = found.size() - unaccounted;
    const size_t countedBefore = (before > kAlertCapacity) ? before - kAlertCapacity : 0;
    if (excess > countedBefore) dropped += excess - countedBefore;
    // 古い方は一覧から外し、キーは次の変更の直前に消す
    for (size_t i = kAlertCapacity; i < found.size(); ++i) pending_removes_.push_back(found[i].seq);
    found.resize(kAlertCapacity);
  }
  dropped_alerts_ = static_cast<uint16_t>(std::min<size_t>(dropped, 0xFFFF));
  alerts_ = found;
  return rep;
}

bool OpenLog::appendSession(const SessionRecord& record) {
  OpenLogEntry entry;
  entry.kind = (record.end == SessionEnd::PausedOpen) ? OpenLogKind::PausedOpen : OpenLogKind::Session;
  entry.epoch = record.start_epoch;
  entry.opened = record.opened;
  entry.gave_up = (record.end == SessionEnd::GaveUp);
  entry.cancelled = (record.end == SessionEnd::Cancelled);
  entry.unregistered_open = record.unregistered_open;
  entry.unknown_checkout = record.unknown_checkout;
  copyField(entry.user, record.user, kCodeFieldLen);
  copyField(entry.photo, record.photo, kPhotoFieldLen);
  entry.checkouts = record.checkouts;
  entry.returns = record.returns;
  entry.switches = record.switches;
  entry.failures = record.failures;

  // 見せることが目的なので、赤帯を開閉ログより先に書く
  if (entry.gave_up && (entry.unregistered_open || entry.unknown_checkout)) {
    if (!appendAlert(record)) return false;
  }
  return appendEntry(entry);
}

bool OpenLog::appendEvent(OpenLogKind kind, int64_t epoch) {
  if (kind != OpenLogKind::PauseStarted && kind != OpenLogKind::PauseEnded && kind != OpenLogKind::PhotosCleared) {
    return false;
  }
  OpenLogEntry entry;
  entry.kind = kind;
  entry.epoch = epoch;
  return appendEntry(entry);
}

uint16_t OpenLog::count() const { return log_.count(); }

bool OpenLog::get(uint16_t index, OpenLogEntry* out) const {
  if (out == nullptr || index >= entries_.size() || !entries_[index].valid) return false;
  *out = entries_[index].entry;
  return true;
}

uint16_t OpenLog::alertCount() const { return static_cast<uint16_t>(alerts_.size()); }

bool OpenLog::getAlert(uint16_t index, Alert* out) const {
  if (out == nullptr || index >= alerts_.size()) return false;
  *out = alerts_[index];
  return true;
}

uint16_t OpenLog::droppedAlerts() const { return dropped_alerts_; }

bool OpenLog::confirmAlert(uint16_t index) {
  if (index >= alerts_.size()) return false;
  flushPendingAlertRemoves();  // 消せなくても確認は進める (残りは次の変更の直前に消す)
  char key[kKeyLen];
  alertKey(alerts_[index].seq, key);
  if (!kv_.remove(kAlertNs, key)) return false;
  alerts_.erase(alerts_.begin() + index);
  if (alerts_.empty() && dropped_alerts_ != 0 && putAlertMeta(next_alert_seq_, 0)) dropped_alerts_ = 0;
  return true;
}

bool OpenLog::clear(int64_t epoch) {
  const bool logCleared = log_.clear();
  entries_.clear();
  const bool alertsCleared = kv_.clearNamespace(kAlertNs);
  if (alertsCleared) {
    alerts_.clear();
    pending_removes_.clear();
    next_alert_seq_ = 1;
    dropped_alerts_ = 0;
  }
  if (!logCleared || !alertsCleared) return false;
  OpenLogEntry entry;
  entry.kind = OpenLogKind::RecordsCleared;
  entry.epoch = epoch;
  return appendEntry(entry);
}

bool OpenLog::appendAlert(const SessionRecord& record) {
  flushPendingAlertRemoves();  // 消せなくても書くのは止めない (新しい赤帯を見せる方を優先)

  Alert alert;
  alert.seq = next_alert_seq_;
  alert.epoch = record.start_epoch;
  alert.unregistered_open = record.unregistered_open;
  alert.unknown_checkout = record.unknown_checkout;
  copyField(alert.photo, record.photo, kPhotoFieldLen);

  // ① 赤帯
  uint8_t blob[kAlertRecordSize];
  encodeAlert(alert, blob);
  char key[kKeyLen];
  alertKey(alert.seq, key);
  if (!kv_.putBlob(kAlertNs, key, blob, kAlertRecordSize)) return false;

  // ② 赤帯の meta (ここで落ちても、次の load() が通し番号から数に入れる)
  const bool overflow = alerts_.size() >= kAlertCapacity;
  const uint16_t dropped = (overflow && dropped_alerts_ < 0xFFFF) ? static_cast<uint16_t>(dropped_alerts_ + 1) : dropped_alerts_;
  if (!putAlertMeta(alert.seq + 1, dropped)) return false;
  next_alert_seq_ = alert.seq + 1;
  dropped_alerts_ = dropped;
  alerts_.insert(alerts_.begin(), alert);

  // ③ 溢れた最古のキーを消す (消せなければ一覧からだけ外し、次の変更の直前に消す)
  if (alerts_.size() > kAlertCapacity) {
    const uint32_t oldestSeq = alerts_.back().seq;
    alerts_.pop_back();
    char oldKey[kKeyLen];
    alertKey(oldestSeq, oldKey);
    if (!kv_.remove(kAlertNs, oldKey)) pending_removes_.push_back(oldestSeq);
  }
  return true;
}

bool OpenLog::appendEntry(const OpenLogEntry& entry) {
  uint8_t record[kOpenLogRecordSize];
  encodeEntry(entry, record);
  if (!log_.append(record)) return false;
  CachedEntry cached;
  cached.valid = true;
  cached.entry = entry;
  entries_.insert(entries_.begin(), cached);
  if (entries_.size() > log_.count()) entries_.resize(log_.count());
  return true;
}

bool OpenLog::putAlertMeta(uint32_t next_seq, uint16_t dropped) {
  uint8_t meta[kAlertMetaSize];
  std::memset(meta, 0, sizeof(meta));
  putU32(meta, next_seq);
  putU16(meta + 4, dropped);
  return kv_.putBlob(kAlertNs, kAlertMetaKey, meta, kAlertMetaSize);
}

bool OpenLog::flushPendingAlertRemoves() {
  while (!pending_removes_.empty()) {
    char key[kKeyLen];
    alertKey(pending_removes_.back(), key);
    if (!kv_.remove(kAlertNs, key)) return false;
    pending_removes_.pop_back();
  }
  return true;
}

}  // namespace toolcheck
