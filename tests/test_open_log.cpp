// open_log: 開閉ログ (200 件) と、確認するまで残す赤帯 (20 件) (docs/設計.md「永続化 (NVS)」「セッション」)
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/open_log.h"
#include "memory_kv_store.h"
#include "testing.h"

using toolcheck::Alert;
using toolcheck::kAlertCapacity;
using toolcheck::kAlertRecordSize;
using toolcheck::kOpenLogCapacity;
using toolcheck::kOpenLogRecordSize;
using toolcheck::OpenLog;
using toolcheck::OpenLogEntry;
using toolcheck::OpenLogKind;
using toolcheck::OpenLogLoadReport;
using toolcheck::SessionEnd;
using toolcheck::SessionRecord;

namespace {

const int64_t kT0 = 1789300000;  // 2026-09-13 11:46 UTC

template <size_t N>
void setText(char(&dst)[N], const char* src) {
  std::snprintf(dst, N, "%s", src);
}

std::string str(const char* s) { return std::string(s); }

// 名札で確定したセッション (数はわざと全部違う値にして、欄の取り違えを見つける)
SessionRecord confirmed(int64_t epoch, const char* user) {
  SessionRecord r;
  r.end = SessionEnd::Confirmed;
  r.start_epoch = epoch;
  r.opened = true;
  setText(r.user, user);
  setText(r.photo, "20260913-101500.jpg");
  r.checkouts = 2;
  r.returns = 1;
  r.switches = 3;
  r.failures = 4;
  return r;
}

// 打ち切り: 1 件も読まなかった開放
SessionRecord gaveUpUnregistered(int64_t epoch, const char* photo) {
  SessionRecord r;
  r.end = SessionEnd::GaveUp;
  r.start_epoch = epoch;
  r.opened = true;
  setText(r.photo, photo);
  r.unregistered_open = true;
  return r;
}

// 打ち切り: 利用者不明の持出がある
SessionRecord gaveUpUnknownCheckout(int64_t epoch) {
  SessionRecord r;
  r.end = SessionEnd::GaveUp;
  r.start_epoch = epoch;
  r.opened = true;
  setText(r.photo, "20260913-090000.jpg");
  r.checkouts = 1;
  r.unknown_checkout = true;
  return r;
}

// 赤帯の名前空間にある、meta 以外のキーの数
size_t alertKeys(const MemoryKvStore& kv) {
  auto n = kv.data.find("alerts");
  if (n == kv.data.end()) return 0;
  size_t keys = 0;
  for (const auto& kv_pair : n->second) {
    if (kv_pair.first != "meta") ++keys;
  }
  return keys;
}

int64_t readI64(const uint8_t* p) {
  uint64_t u = 0;
  for (int i = 7; i >= 0; --i) u = (u << 8) | p[i];
  return static_cast<int64_t>(u);
}

uint32_t readU32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

}  // namespace

// --- 開閉ログ ---

TEST(openlog_empty_store_loads_empty) {
  MemoryKvStore kv;
  OpenLog log(kv);
  OpenLogLoadReport rep = log.load();
  CHECK(rep.log_ok);
  CHECK(rep.alerts_ok);
  CHECK_EQ(rep.skipped_alerts, uint16_t{0});
  CHECK_EQ(log.count(), uint16_t{0});
  CHECK_EQ(log.alertCount(), uint16_t{0});
  CHECK_EQ(log.droppedAlerts(), uint16_t{0});
  OpenLogEntry e;
  CHECK(!log.get(0, &e));
  CHECK_EQ(kv.writes(), 0);
}

TEST(openlog_session_fields_round_trip) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  CHECK(log.appendSession(confirmed(kT0, "1234567")));
  CHECK_EQ(log.count(), uint16_t{1});
  OpenLogEntry e;
  CHECK(log.get(0, &e));
  CHECK_EQ(e.kind, OpenLogKind::Session);
  CHECK_EQ(e.epoch, kT0);
  CHECK(e.opened);
  CHECK(!e.gave_up);
  CHECK(!e.unregistered_open);
  CHECK(!e.unknown_checkout);
  CHECK(!e.cancelled);
  CHECK_EQ(str(e.user), std::string("1234567"));
  CHECK_EQ(str(e.photo), std::string("20260913-101500.jpg"));
  CHECK_EQ(e.checkouts, uint8_t{2});
  CHECK_EQ(e.returns, uint8_t{1});
  CHECK_EQ(e.switches, uint8_t{3});
  CHECK_EQ(e.failures, uint8_t{4});
  CHECK_EQ(log.alertCount(), uint16_t{0});  // 名札で確定したセッションに赤帯は付けない
}

TEST(openlog_session_without_open_or_photo) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  SessionRecord r = confirmed(kT0, "7654321");
  r.opened = false;  // QR だけで始めたセッション
  r.photo[0] = '\0';
  CHECK(log.appendSession(r));
  OpenLogEntry e;
  CHECK(log.get(0, &e));
  CHECK(!e.opened);
  CHECK_EQ(str(e.photo), std::string(""));
}

// 2026-09-15: 持出の取りやめ。開けた後なら開閉ログに「取りやめ」で残し、赤帯にはしない。印は bit4
TEST(openlog_cancelled_session_round_trip_without_alert) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  SessionRecord r;
  r.end = SessionEnd::Cancelled;
  r.start_epoch = kT0;
  r.opened = true;
  setText(r.photo, "20260915-101500.jpg");
  r.returns = 1;
  CHECK(log.appendSession(r));
  CHECK_EQ(log.alertCount(), uint16_t{0});
  OpenLog again(kv);
  again.load();
  OpenLogEntry e;
  CHECK(again.get(0, &e));
  CHECK_EQ(e.kind, OpenLogKind::Session);
  CHECK(e.cancelled);
  CHECK(!e.gave_up);
  CHECK(e.opened);
  CHECK_EQ(e.returns, uint8_t{1});
  CHECK_EQ(str(e.photo), std::string("20260915-101500.jpg"));
  const std::vector<uint8_t>& slot = kv.data["openlog"]["O000"];
  CHECK_EQ(slot.size(), size_t{4 + kOpenLogRecordSize});
  if (slot.size() != 4 + kOpenLogRecordSize) return;
  CHECK_EQ(slot[4 + 2], uint8_t{0x11});  // 印: bit0 開放 / bit4 取りやめ
}

TEST(openlog_paused_open_is_its_own_kind) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  SessionRecord r;
  r.end = SessionEnd::PausedOpen;
  r.start_epoch = kT0;
  r.opened = true;
  setText(r.photo, "20260913-120000.jpg");
  CHECK(log.appendSession(r));
  OpenLogEntry e;
  CHECK(log.get(0, &e));
  CHECK_EQ(e.kind, OpenLogKind::PausedOpen);
  CHECK(e.opened);
  CHECK_EQ(str(e.photo), std::string("20260913-120000.jpg"));
  CHECK_EQ(log.alertCount(), uint16_t{0});
}

TEST(openlog_pause_events_are_recorded) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  CHECK(log.appendEvent(OpenLogKind::PauseStarted, kT0));
  CHECK(log.appendEvent(OpenLogKind::PauseEnded, kT0 + 600));
  CHECK_EQ(log.count(), uint16_t{2});
  OpenLogEntry e;
  CHECK(log.get(0, &e));
  CHECK_EQ(e.kind, OpenLogKind::PauseEnded);
  CHECK_EQ(e.epoch, kT0 + 600);
  CHECK(log.get(1, &e));
  CHECK_EQ(e.kind, OpenLogKind::PauseStarted);
  CHECK_EQ(e.epoch, kT0);
  CHECK(!e.opened);
}

TEST(openlog_event_rejects_other_kinds) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  CHECK(!log.appendEvent(OpenLogKind::Session, kT0));
  CHECK(!log.appendEvent(OpenLogKind::PausedOpen, kT0));
  CHECK(!log.appendEvent(OpenLogKind::RecordsCleared, kT0));  // 記録の削除は clear() だけが書く
  CHECK_EQ(log.count(), uint16_t{0});
  CHECK_EQ(kv.writes(), 0);
}

// SD の写真の全削除 (2026-09-14: 設定画面の「データ削除」から。消したことを後から追えるように開閉ログに残す)
TEST(openlog_event_records_photos_cleared) {
  MemoryKvStore kv;
  {
    OpenLog a(kv);
    a.load();
    CHECK(a.appendEvent(OpenLogKind::PhotosCleared, kT0 + 30));
    CHECK_EQ(a.count(), uint16_t{1});
    CHECK_EQ(a.alertCount(), uint16_t{0});
  }
  OpenLog b(kv);
  b.load();  // 読み直しても、知らない種類として捨てない
  CHECK_EQ(b.count(), uint16_t{1});
  OpenLogEntry e;
  CHECK(b.get(0, &e));
  CHECK_EQ(e.kind, OpenLogKind::PhotosCleared);
  CHECK_EQ(e.epoch, kT0 + 30);
  CHECK(!e.opened);
  CHECK_EQ(str(e.user), std::string(""));
  CHECK_EQ(str(e.photo), std::string(""));
}

TEST(openlog_unknown_kind_is_not_read) {
  MemoryKvStore kv;
  {
    OpenLog a(kv);
    a.load();
    CHECK(a.appendEvent(OpenLogKind::PauseStarted, kT0));
  }
  std::vector<uint8_t>& slot = kv.data["openlog"]["O000"];
  CHECK_EQ(slot.size(), size_t{4 + kOpenLogRecordSize});
  if (slot.size() != 4 + kOpenLogRecordSize) return;
  slot[4 + 1] = 7;  // 知らない種類
  OpenLog b(kv);
  b.load();
  OpenLogEntry e;
  CHECK(!b.get(0, &e));
}

TEST(openlog_keeps_newest_200) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  bool all = true;
  for (int i = 0; i < 205; ++i) {
    all = log.appendEvent((i % 2) ? OpenLogKind::PauseEnded : OpenLogKind::PauseStarted, kT0 + i) && all;
  }
  CHECK(all);
  CHECK_EQ(log.count(), kOpenLogCapacity);
  OpenLogEntry e;
  CHECK(log.get(0, &e));
  CHECK_EQ(e.epoch, kT0 + 204);
  CHECK(log.get(199, &e));
  CHECK_EQ(e.epoch, kT0 + 5);
  CHECK(!log.get(200, &e));
}

TEST(openlog_persists_across_instances) {
  MemoryKvStore kv;
  {
    OpenLog a(kv);
    a.load();
    CHECK(a.appendSession(confirmed(kT0, "1234567")));
    CHECK(a.appendEvent(OpenLogKind::PauseStarted, kT0 + 10));
  }
  OpenLog b(kv);
  b.load();
  CHECK_EQ(b.count(), uint16_t{2});
  OpenLogEntry e;
  CHECK(b.get(0, &e));
  CHECK_EQ(e.kind, OpenLogKind::PauseStarted);
  CHECK(b.get(1, &e));
  CHECK_EQ(e.kind, OpenLogKind::Session);
  CHECK_EQ(str(e.user), std::string("1234567"));
  CHECK_EQ(e.checkouts, uint8_t{2});
}

TEST(openlog_load_does_not_write) {
  MemoryKvStore kv;
  {
    OpenLog a(kv);
    a.load();
    CHECK(a.appendSession(gaveUpUnregistered(kT0, "20260913-101500.jpg")));
  }
  const int before = kv.writes();
  OpenLog b(kv);
  b.load();
  CHECK_EQ(kv.writes(), before);
  CHECK_EQ(b.count(), uint16_t{1});
  CHECK_EQ(b.alertCount(), uint16_t{1});
}

// --- 赤帯 ---

TEST(alert_added_for_unregistered_open) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  CHECK(log.appendSession(gaveUpUnregistered(kT0, "20260913-101500.jpg")));
  CHECK_EQ(log.alertCount(), uint16_t{1});
  Alert a;
  CHECK(log.getAlert(0, &a));
  CHECK(a.unregistered_open);
  CHECK(!a.unknown_checkout);
  CHECK_EQ(a.epoch, kT0);
  CHECK_EQ(str(a.photo), std::string("20260913-101500.jpg"));
  // 開閉ログにも打ち切りとして残る
  CHECK_EQ(log.count(), uint16_t{1});
  OpenLogEntry e;
  CHECK(log.get(0, &e));
  CHECK_EQ(e.kind, OpenLogKind::Session);
  CHECK(e.gave_up);
  CHECK(e.unregistered_open);
}

TEST(alert_added_for_unknown_checkout) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  CHECK(log.appendSession(gaveUpUnknownCheckout(kT0)));
  CHECK_EQ(log.alertCount(), uint16_t{1});
  Alert a;
  CHECK(log.getAlert(0, &a));
  CHECK(!a.unregistered_open);
  CHECK(a.unknown_checkout);
  OpenLogEntry e;
  CHECK(log.get(0, &e));
  CHECK(e.gave_up);
  CHECK(e.unknown_checkout);
  CHECK_EQ(e.checkouts, uint8_t{1});
}

TEST(alert_not_added_when_gave_up_without_marks) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  SessionRecord r;
  r.end = SessionEnd::GaveUp;  // 返却だけを読んで立ち去った (返却者不明で確定。赤帯は付けない)
  r.start_epoch = kT0;
  r.opened = true;
  r.returns = 1;
  CHECK(log.appendSession(r));
  CHECK_EQ(log.count(), uint16_t{1});
  CHECK_EQ(log.alertCount(), uint16_t{0});
}

TEST(alerts_are_newest_first) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  CHECK(log.appendSession(gaveUpUnregistered(kT0, "a.jpg")));
  CHECK(log.appendSession(gaveUpUnknownCheckout(kT0 + 1)));
  CHECK(log.appendSession(gaveUpUnregistered(kT0 + 2, "c.jpg")));
  CHECK_EQ(log.alertCount(), uint16_t{3});
  Alert a0;
  Alert a1;
  Alert a2;
  CHECK(log.getAlert(0, &a0));
  CHECK(log.getAlert(1, &a1));
  CHECK(log.getAlert(2, &a2));
  CHECK_EQ(a0.epoch, kT0 + 2);
  CHECK_EQ(a1.epoch, kT0 + 1);
  CHECK_EQ(a2.epoch, kT0);
  CHECK(a0.seq > a1.seq);
  CHECK(a1.seq > a2.seq);
  CHECK(!log.getAlert(3, &a0));
}

TEST(alerts_keep_newest_20_and_count_dropped) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  bool all = true;
  for (int i = 0; i < 23; ++i) all = log.appendSession(gaveUpUnregistered(kT0 + i, "x.jpg")) && all;
  CHECK(all);
  CHECK_EQ(log.alertCount(), kAlertCapacity);
  CHECK_EQ(log.droppedAlerts(), uint16_t{3});
  Alert a;
  CHECK(log.getAlert(0, &a));
  CHECK_EQ(a.epoch, kT0 + 22);
  CHECK(log.getAlert(19, &a));
  CHECK_EQ(a.epoch, kT0 + 3);
  CHECK_EQ(alertKeys(kv), size_t{20});  // 溢れた赤帯のキーは消えている
  CHECK_EQ(log.count(), uint16_t{23});  // 開閉ログには全部残る
}

TEST(alerts_persist_across_instances_with_dropped) {
  MemoryKvStore kv;
  {
    OpenLog a(kv);
    a.load();
    for (int i = 0; i < 21; ++i) CHECK(a.appendSession(gaveUpUnregistered(kT0 + i, "x.jpg")));
  }
  OpenLog b(kv);
  b.load();
  CHECK_EQ(b.alertCount(), kAlertCapacity);
  CHECK_EQ(b.droppedAlerts(), uint16_t{1});
  Alert a;
  CHECK(b.getAlert(0, &a));
  CHECK_EQ(a.epoch, kT0 + 20);
  CHECK(b.getAlert(19, &a));
  CHECK_EQ(a.epoch, kT0 + 1);
}

TEST(alert_confirm_removes_it_and_persists) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  CHECK(log.appendSession(gaveUpUnregistered(kT0, "a.jpg")));
  CHECK(log.appendSession(gaveUpUnregistered(kT0 + 1, "b.jpg")));
  CHECK(log.appendSession(gaveUpUnregistered(kT0 + 2, "c.jpg")));
  CHECK(log.confirmAlert(1));  // 真ん中 (kT0 + 1)
  CHECK_EQ(log.alertCount(), uint16_t{2});
  Alert a;
  CHECK(log.getAlert(0, &a));
  CHECK_EQ(a.epoch, kT0 + 2);
  CHECK(log.getAlert(1, &a));
  CHECK_EQ(a.epoch, kT0);
  CHECK_EQ(log.count(), uint16_t{3});  // 開閉ログは消えない

  OpenLog b(kv);
  b.load();
  CHECK_EQ(b.alertCount(), uint16_t{2});
  CHECK(b.getAlert(0, &a));
  CHECK_EQ(a.epoch, kT0 + 2);
  CHECK(b.getAlert(1, &a));
  CHECK_EQ(a.epoch, kT0);
}

TEST(alert_confirm_out_of_range_changes_nothing) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  CHECK(!log.confirmAlert(0));
  CHECK(log.appendSession(gaveUpUnregistered(kT0, "a.jpg")));
  const int before = kv.writes();
  CHECK(!log.confirmAlert(1));
  CHECK_EQ(kv.writes(), before);
  CHECK_EQ(log.alertCount(), uint16_t{1});
}

TEST(alert_confirming_all_resets_dropped) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  for (int i = 0; i < 21; ++i) CHECK(log.appendSession(gaveUpUnregistered(kT0 + i, "x.jpg")));
  CHECK_EQ(log.droppedAlerts(), uint16_t{1});
  bool all = true;
  for (int i = 0; i < 20; ++i) all = log.confirmAlert(0) && all;
  CHECK(all);
  CHECK_EQ(log.alertCount(), uint16_t{0});
  CHECK_EQ(log.droppedAlerts(), uint16_t{0});

  OpenLog b(kv);
  b.load();
  CHECK_EQ(b.alertCount(), uint16_t{0});
  CHECK_EQ(b.droppedAlerts(), uint16_t{0});
}

// --- 電源断 ---

TEST(alert_power_loss_before_alert_changes_nothing) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  kv.fail_after_puts = 0;
  CHECK(!log.appendSession(gaveUpUnregistered(kT0, "a.jpg")));
  CHECK_EQ(log.alertCount(), uint16_t{0});
  CHECK_EQ(log.count(), uint16_t{0});
  kv.fail_after_puts = -1;
  OpenLog b(kv);
  b.load();
  CHECK_EQ(b.alertCount(), uint16_t{0});
  CHECK_EQ(b.count(), uint16_t{0});
}

TEST(alert_power_loss_before_log_keeps_alert) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  kv.fail_after_puts = 2;  // 赤帯と赤帯の meta は書けて、開閉ログで落ちる
  CHECK(!log.appendSession(gaveUpUnregistered(kT0, "a.jpg")));
  CHECK_EQ(log.alertCount(), uint16_t{1});  // 見せることが目的なので赤帯は残す
  CHECK_EQ(log.count(), uint16_t{0});
  kv.fail_after_puts = -1;
  OpenLog b(kv);
  b.load();
  CHECK_EQ(b.alertCount(), uint16_t{1});
  CHECK_EQ(b.count(), uint16_t{0});
}

TEST(alert_power_loss_before_meta_is_counted_on_load) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  kv.fail_after_puts = 1;  // 赤帯だけ書けて meta で落ちる
  CHECK(!log.appendSession(gaveUpUnregistered(kT0, "a.jpg")));
  kv.fail_after_puts = -1;
  OpenLog b(kv);
  b.load();
  CHECK_EQ(b.alertCount(), uint16_t{1});
  // 次の赤帯は通し番号がぶつからず、新しい方に並ぶ
  CHECK(b.appendSession(gaveUpUnregistered(kT0 + 1, "b.jpg")));
  CHECK_EQ(b.alertCount(), uint16_t{2});
  Alert x;
  Alert y;
  CHECK(b.getAlert(0, &x));
  CHECK(b.getAlert(1, &y));
  CHECK_EQ(x.epoch, kT0 + 1);
  CHECK_EQ(y.epoch, kT0);
  CHECK(x.seq > y.seq);
}

TEST(alert_power_loss_before_meta_when_full_counts_dropped_on_load) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  for (int i = 0; i < 20; ++i) CHECK(log.appendSession(gaveUpUnregistered(kT0 + i, "x.jpg")));
  kv.fail_after_puts = kv.puts + 1;  // 21 件目の赤帯だけ書けて meta で落ちる
  CHECK(!log.appendSession(gaveUpUnregistered(kT0 + 20, "n.jpg")));
  kv.fail_after_puts = -1;
  OpenLog b(kv);
  b.load();
  CHECK_EQ(b.alertCount(), kAlertCapacity);
  CHECK_EQ(b.droppedAlerts(), uint16_t{1});
  Alert a;
  CHECK(b.getAlert(0, &a));
  CHECK_EQ(a.epoch, kT0 + 20);
  CHECK(b.getAlert(19, &a));
  CHECK_EQ(a.epoch, kT0 + 1);
}

TEST(alert_failed_overflow_removal_hides_oldest_until_next_write) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  for (int i = 0; i < 20; ++i) CHECK(log.appendSession(gaveUpUnregistered(kT0 + i, "x.jpg")));
  kv.fail_removes = true;
  CHECK(log.appendSession(gaveUpUnregistered(kT0 + 20, "n.jpg")));
  CHECK_EQ(log.alertCount(), kAlertCapacity);
  CHECK_EQ(log.droppedAlerts(), uint16_t{1});
  Alert a;
  CHECK(log.getAlert(19, &a));
  CHECK_EQ(a.epoch, kT0 + 1);         // 最古 (kT0) は一覧から外す
  CHECK_EQ(alertKeys(kv), size_t{21});  // キーはまだ残っている

  // 読み直しても最古は出さず、数え直さない
  OpenLog b(kv);
  b.load();
  CHECK_EQ(b.alertCount(), kAlertCapacity);
  CHECK_EQ(b.droppedAlerts(), uint16_t{1});
  CHECK(b.getAlert(19, &a));
  CHECK_EQ(a.epoch, kT0 + 1);

  // 次の変更の直前に消す
  kv.fail_removes = false;
  CHECK(b.confirmAlert(0));
  CHECK_EQ(b.alertCount(), uint16_t{19});
  CHECK_EQ(alertKeys(kv), size_t{19});
}

// --- 壊れた記録 ---

TEST(alert_unreadable_is_skipped_not_deleted) {
  MemoryKvStore kv;
  {
    OpenLog a(kv);
    a.load();
    CHECK(a.appendSession(gaveUpUnregistered(kT0, "a.jpg")));
    CHECK(a.appendSession(gaveUpUnregistered(kT0 + 1, "b.jpg")));
  }
  std::string broken;
  for (auto& kv_pair : kv.data["alerts"]) {
    if (kv_pair.first != "meta") {
      kv_pair.second[0] = 99;  // 版数を壊す
      broken = kv_pair.first;
      break;
    }
  }
  OpenLog b(kv);
  OpenLogLoadReport rep = b.load();
  CHECK(rep.alerts_ok);
  CHECK_EQ(rep.skipped_alerts, uint16_t{1});
  CHECK_EQ(b.alertCount(), uint16_t{1});
  CHECK(kv.hasKey("alerts", broken.c_str()));  // 自動で消さない
}

TEST(openlog_broken_meta_is_reported) {
  MemoryKvStore kv;
  {
    OpenLog a(kv);
    a.load();
    CHECK(a.appendEvent(OpenLogKind::PauseStarted, kT0));
  }
  kv.data["openlog"]["meta"] = std::vector<uint8_t>(8, 0xFF);
  OpenLog b(kv);
  OpenLogLoadReport rep = b.load();
  CHECK(!rep.log_ok);
  CHECK_EQ(b.count(), uint16_t{0});
}

TEST(alert_enumeration_failure_is_reported) {
  MemoryKvStore kv;
  kv.fail_enumeration = true;
  OpenLog log(kv);
  OpenLogLoadReport rep = log.load();
  CHECK(!rep.alerts_ok);
  CHECK_EQ(log.alertCount(), uint16_t{0});
}

// --- 記録の削除 ---

TEST(openlog_clear_erases_log_and_alerts_and_writes_cleared_event) {
  MemoryKvStore kv;
  kv.data["loans"]["TW-025"] = std::vector<uint8_t>(1, 1);
  OpenLog log(kv);
  log.load();
  CHECK(log.appendSession(confirmed(kT0, "1234567")));
  for (int i = 0; i < 21; ++i) CHECK(log.appendSession(gaveUpUnregistered(kT0 + 1 + i, "x.jpg")));
  CHECK(log.clear(kT0 + 100));
  CHECK_EQ(log.count(), uint16_t{1});
  OpenLogEntry e;
  CHECK(log.get(0, &e));
  CHECK_EQ(e.kind, OpenLogKind::RecordsCleared);
  CHECK_EQ(e.epoch, kT0 + 100);
  CHECK_EQ(log.alertCount(), uint16_t{0});
  CHECK_EQ(log.droppedAlerts(), uint16_t{0});
  CHECK_EQ(alertKeys(kv), size_t{0});
  CHECK(kv.hasKey("loans", "TW-025"));  // 貸出は消さない (LoanBook::clear が消す)

  OpenLog b(kv);
  b.load();
  CHECK_EQ(b.count(), uint16_t{1});
  CHECK_EQ(b.alertCount(), uint16_t{0});
  // 削除の後も赤帯の通し番号は 1 から振り直してよい
  CHECK(b.appendSession(gaveUpUnregistered(kT0 + 200, "y.jpg")));
  CHECK_EQ(b.alertCount(), uint16_t{1});
}

// --- 保存の形 ---

TEST(openlog_record_layout_is_version_1) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  CHECK(log.appendSession(confirmed(kT0, "1234567")));
  CHECK(kv.hasKey("openlog", "O000"));
  const std::vector<uint8_t>& slot = kv.data["openlog"]["O000"];
  CHECK_EQ(slot.size(), size_t{4 + kOpenLogRecordSize});  // RingStore の通し番号 + 1 件
  if (slot.size() != 4 + kOpenLogRecordSize) return;
  const uint8_t* rec = slot.data() + 4;
  CHECK_EQ(rec[0], uint8_t{1});     // 版数
  CHECK_EQ(rec[1], uint8_t{1});     // 種類 = Session
  CHECK_EQ(rec[2], uint8_t{0x01});  // 印: bit0 開放 / bit1 打ち切り / bit2 無登録開放 / bit3 利用者不明の持出 / bit4 取りやめ
  CHECK_EQ(rec[3], uint8_t{2});     // 持出
  CHECK_EQ(rec[4], uint8_t{1});     // 返却
  CHECK_EQ(rec[5], uint8_t{3});     // 持出に切替
  CHECK_EQ(rec[6], uint8_t{4});     // 記録できなかった行
  CHECK_EQ(readI64(rec + 8), kT0);
  CHECK_EQ(str(reinterpret_cast<const char*>(rec + 16)), std::string("1234567"));
  CHECK_EQ(str(reinterpret_cast<const char*>(rec + 32)), std::string("20260913-101500.jpg"));
}

TEST(alert_record_layout_is_version_1) {
  MemoryKvStore kv;
  OpenLog log(kv);
  log.load();
  CHECK(log.appendSession(gaveUpUnregistered(kT0, "20260913-101500.jpg")));
  CHECK(kv.hasKey("alerts", "A0000000001"));
  const std::vector<uint8_t>& blob = kv.data["alerts"]["A0000000001"];
  CHECK_EQ(blob.size(), kAlertRecordSize);
  if (blob.size() != kAlertRecordSize) return;
  CHECK_EQ(blob[0], uint8_t{1});     // 版数
  CHECK_EQ(blob[1], uint8_t{0x01});  // 印: bit0 無登録開放 / bit1 利用者不明の持出
  CHECK_EQ(readU32(blob.data() + 4), uint32_t{1});
  CHECK_EQ(readI64(blob.data() + 8), kT0);
  CHECK_EQ(str(reinterpret_cast<const char*>(blob.data() + 16)), std::string("20260913-101500.jpg"));
  const std::vector<uint8_t>& meta = kv.data["alerts"]["meta"];
  CHECK_EQ(meta.size(), size_t{8});
  if (meta.size() != 8) return;
  CHECK_EQ(readU32(meta.data()), uint32_t{2});                             // 次の通し番号
  CHECK_EQ(static_cast<uint16_t>(meta[4] | (meta[5] << 8)), uint16_t{0});  // 溢れて消えた数
}
