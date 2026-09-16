// time_format: 経過時間・前日判定・現地時刻 (docs/設計.md「画面」「時刻」)
#include <string>

#include "core/time_format.h"
#include "testing.h"

using toolcheck::formatClock;
using toolcheck::formatElapsed;
using toolcheck::isPreviousDay;
using toolcheck::isValidEpoch;
using toolcheck::kValidEpochMin;

namespace {

const int64_t kSep12Utc = 1789171200;  // 2026-09-12T00:00:00Z
const int32_t kJst = 9 * 60;

std::string elapsed(int64_t now, int64_t since) {
  char buf[32];
  if (!formatElapsed(now, since, buf, sizeof(buf))) return "<false>";
  return std::string(buf);
}

std::string clock(int64_t epoch, int32_t tz) {
  char buf[16];
  if (!formatClock(epoch, tz, buf, sizeof(buf))) return "<false>";
  return std::string(buf);
}

}  // namespace

// --- 時刻の有効性 ---

TEST(time_valid_epoch_boundary) {
  CHECK(isValidEpoch(kValidEpochMin));
  CHECK(!isValidEpoch(kValidEpochMin - 1));
  CHECK(!isValidEpoch(0));
  CHECK(!isValidEpoch(-1));
  CHECK(isValidEpoch(kSep12Utc));
}

// --- 経過時間 ---

TEST(time_elapsed_zero_is_zero_minutes) { CHECK_EQ(elapsed(kSep12Utc, kSep12Utc), std::string("0分前")); }

TEST(time_elapsed_59_seconds_is_zero_minutes) {
  CHECK_EQ(elapsed(kSep12Utc + 59, kSep12Utc), std::string("0分前"));
}

TEST(time_elapsed_60_seconds_is_one_minute) {
  CHECK_EQ(elapsed(kSep12Utc + 60, kSep12Utc), std::string("1分前"));
}

TEST(time_elapsed_59_minutes_59_seconds) {
  CHECK_EQ(elapsed(kSep12Utc + 3599, kSep12Utc), std::string("59分前"));
}

TEST(time_elapsed_60_minutes_is_one_hour) {
  CHECK_EQ(elapsed(kSep12Utc + 3600, kSep12Utc), std::string("1時間前"));
}

TEST(time_elapsed_23_hours_59_minutes) {
  CHECK_EQ(elapsed(kSep12Utc + 86399, kSep12Utc), std::string("23時間前"));
}

TEST(time_elapsed_48_hours) { CHECK_EQ(elapsed(kSep12Utc + 172800, kSep12Utc), std::string("48時間前")); }

TEST(time_elapsed_future_since_is_dash) { CHECK_EQ(elapsed(kSep12Utc, kSep12Utc + 1), std::string("--")); }

TEST(time_elapsed_invalid_since_is_dash) { CHECK_EQ(elapsed(kSep12Utc, 1000), std::string("--")); }

TEST(time_elapsed_invalid_now_is_dash) { CHECK_EQ(elapsed(1000, kSep12Utc), std::string("--")); }

TEST(time_elapsed_small_buffer_fails_with_empty_string) {
  char buf[4] = {'x', 'x', 'x', 'x'};
  CHECK(!formatElapsed(kSep12Utc + 3600 * 12, kSep12Utc, buf, sizeof(buf)));
  CHECK_EQ(std::string(buf), std::string(""));
}

// --- 前日判定 (時差を足した現地の日付で見る) ---

TEST(time_previous_day_across_local_midnight) {
  const int64_t since = kSep12Utc + 14 * 3600 + 59 * 60;  // JST 9/12 23:59
  const int64_t now = kSep12Utc + 15 * 3600 + 1 * 60;     // JST 9/13 00:01
  CHECK(isPreviousDay(now, since, kJst));
  CHECK(!isPreviousDay(now, since, 0));  // UTC ではどちらも 9/12
}

TEST(time_same_local_day_across_utc_midnight) {
  const int64_t since = kSep12Utc - 9 * 3600;             // JST 9/12 00:00 (UTC 9/11 15:00)
  const int64_t now = kSep12Utc + 14 * 3600 + 59 * 60;    // JST 9/12 23:59
  CHECK(!isPreviousDay(now, since, kJst));
  CHECK(isPreviousDay(now, since, 0));  // UTC では 9/11 と 9/12
}

TEST(time_previous_day_false_when_invalid) {
  CHECK(!isPreviousDay(kSep12Utc + 86400 * 2, 1000, kJst));
  CHECK(!isPreviousDay(1000, kSep12Utc, kJst));
}

// --- 現地時刻 ---

TEST(time_clock_applies_timezone) {
  const int64_t epoch = kSep12Utc + 22 * 3600 + 42 * 60;  // UTC 22:42 → JST 07:42
  CHECK_EQ(clock(epoch, kJst), std::string("07:42"));
  CHECK_EQ(clock(epoch, 0), std::string("22:42"));
}

TEST(time_clock_invalid_is_dashes) { CHECK_EQ(clock(1000, kJst), std::string("--:--")); }

TEST(time_clock_small_buffer_fails) {
  char buf[5] = {'x', 'x', 'x', 'x', 'x'};
  CHECK(!formatClock(kSep12Utc, kJst, buf, sizeof(buf)));  // "09:00" + NUL は 6 バイト要る
  CHECK_EQ(std::string(buf), std::string(""));
}

// --- 現地の日時への分解 ---

TEST(time_local_datetime_jst) {
  toolcheck::LocalDateTime t;
  CHECK(toolcheck::toLocalDateTime(kSep12Utc + 3600 + 42 * 60 + 33, kJst, &t));  // JST 2026-09-12 10:42:33
  CHECK_EQ(t.year, 2026);
  CHECK_EQ(t.month, 9);
  CHECK_EQ(t.day, 12);
  CHECK_EQ(t.hour, 10);
  CHECK_EQ(t.minute, 42);
  CHECK_EQ(t.second, 33);
  CHECK_EQ(t.weekday, 6);  // 土曜
}

TEST(time_local_datetime_leap_day) {
  toolcheck::LocalDateTime t;
  CHECK(toolcheck::toLocalDateTime(1835438400, 0, &t));  // 2028-02-29T12:00:00Z
  CHECK_EQ(t.year, 2028);
  CHECK_EQ(t.month, 2);
  CHECK_EQ(t.day, 29);
  CHECK_EQ(t.hour, 12);
}

TEST(time_local_datetime_new_year_in_jst) {
  toolcheck::LocalDateTime t;
  CHECK(toolcheck::toLocalDateTime(1798729200, kJst, &t));  // UTC 2026-12-31 15:00 → JST 2027-01-01 00:00
  CHECK_EQ(t.year, 2027);
  CHECK_EQ(t.month, 1);
  CHECK_EQ(t.day, 1);
  CHECK_EQ(t.hour, 0);
  CHECK_EQ(t.weekday, 5);  // 金曜
}

TEST(time_local_datetime_invalid_keeps_out) {
  toolcheck::LocalDateTime t;
  t.year = 1234;
  CHECK(!toolcheck::toLocalDateTime(1000, kJst, &t));
  CHECK_EQ(t.year, 1234);
}

// --- 現地の日時から UNIX 秒へ (時刻の設定・RTC の読み出し) ---

namespace {

toolcheck::LocalDateTime dt(int year, int month, int day, int hour, int minute, int second) {
  toolcheck::LocalDateTime t;
  t.year = year;
  t.month = month;
  t.day = day;
  t.hour = hour;
  t.minute = minute;
  t.second = second;
  return t;
}

// 変換できなければ -1
int64_t fromLocal(const toolcheck::LocalDateTime& t, int32_t tz) {
  int64_t e = -1;
  if (!toolcheck::fromLocalDateTime(t, tz, &e)) return -1;
  return e;
}

int64_t parse(const char* text, int32_t tz) {
  int64_t e = -1;
  if (!toolcheck::parseLocalDateTime(text, tz, &e)) return -1;
  return e;
}

}  // namespace

TEST(time_from_local_datetime_subtracts_timezone) {
  CHECK_EQ(fromLocal(dt(2026, 9, 12, 10, 0, 0), kJst), kSep12Utc + 3600);  // JST 10:00 = UTC 01:00
  CHECK_EQ(fromLocal(dt(2026, 9, 12, 0, 0, 0), 0), kSep12Utc);
  CHECK_EQ(fromLocal(dt(2027, 1, 1, 0, 0, 0), kJst), int64_t{1798729200});  // UTC 2026-12-31 15:00
}

TEST(time_from_local_datetime_round_trips_with_to_local) {
  const int64_t samples[] = {kSep12Utc + 12345, 1798729200, 1835438400, kValidEpochMin, 4102444799};
  for (int64_t e : samples) {
    toolcheck::LocalDateTime t;
    CHECK(toolcheck::toLocalDateTime(e, kJst, &t));
    CHECK_EQ(fromLocal(t, kJst), e);
    CHECK(toolcheck::toLocalDateTime(e, -300, &t));
    CHECK_EQ(fromLocal(t, -300), e);
  }
}

TEST(time_from_local_datetime_converts_before_2026) {
  CHECK_EQ(fromLocal(dt(2000, 1, 1, 0, 0, 0), 0), int64_t{946684800});  // RTC が初期値に戻ったとき
  CHECK_EQ(fromLocal(dt(1970, 1, 1, 0, 0, 0), 0), int64_t{0});
}

TEST(time_from_local_datetime_leap_days) {
  CHECK_EQ(fromLocal(dt(2028, 2, 29, 12, 0, 0), 0), int64_t{1835438400});
  CHECK_EQ(fromLocal(dt(2027, 2, 29, 0, 0, 0), 0), int64_t{-1});
  CHECK_EQ(fromLocal(dt(2100, 2, 29, 0, 0, 0), 0), int64_t{-1});  // 100 で割り切れて 400 で割り切れない
  CHECK(fromLocal(dt(2000, 2, 29, 0, 0, 0), 0) > 0);              // 400 で割り切れる
}

TEST(time_from_local_datetime_rejects_out_of_range) {
  CHECK_EQ(fromLocal(dt(2026, 0, 1, 0, 0, 0), kJst), int64_t{-1});
  CHECK_EQ(fromLocal(dt(2026, 13, 1, 0, 0, 0), kJst), int64_t{-1});
  CHECK_EQ(fromLocal(dt(2026, 9, 0, 0, 0, 0), kJst), int64_t{-1});
  CHECK_EQ(fromLocal(dt(2026, 4, 31, 0, 0, 0), kJst), int64_t{-1});
  CHECK_EQ(fromLocal(dt(2026, 9, 12, 24, 0, 0), kJst), int64_t{-1});
  CHECK_EQ(fromLocal(dt(2026, 9, 12, -1, 0, 0), kJst), int64_t{-1});
  CHECK_EQ(fromLocal(dt(2026, 9, 12, 0, 60, 0), kJst), int64_t{-1});
  CHECK_EQ(fromLocal(dt(2026, 9, 12, 0, 0, 60), kJst), int64_t{-1});
  CHECK_EQ(fromLocal(dt(1969, 12, 31, 0, 0, 0), 0), int64_t{-1});
  CHECK_EQ(fromLocal(dt(10000, 1, 1, 0, 0, 0), 0), int64_t{-1});
  CHECK_EQ(fromLocal(dt(2026, 12, 31, 23, 59, 59), kJst), int64_t{1798729199});
}

TEST(time_from_local_datetime_failure_keeps_out) {
  int64_t e = 777;
  CHECK(!toolcheck::fromLocalDateTime(dt(2026, 2, 30, 0, 0, 0), kJst, &e));
  CHECK_EQ(e, int64_t{777});
  CHECK(!toolcheck::fromLocalDateTime(dt(2026, 9, 12, 0, 0, 0), kJst, nullptr));
}

TEST(time_parse_local_datetime_minutes) { CHECK_EQ(parse("2026-09-12T10:00", kJst), kSep12Utc + 3600); }

TEST(time_parse_local_datetime_seconds) { CHECK_EQ(parse("2026-09-12T10:00:30", kJst), kSep12Utc + 3630); }

TEST(time_parse_local_datetime_rejects_bad_text) {
  CHECK_EQ(parse(nullptr, kJst), int64_t{-1});
  CHECK_EQ(parse("", kJst), int64_t{-1});
  CHECK_EQ(parse("2026-9-12T10:00", kJst), int64_t{-1});
  CHECK_EQ(parse("2026-09-12 10:00", kJst), int64_t{-1});
  CHECK_EQ(parse("2026-09-12T10", kJst), int64_t{-1});
  CHECK_EQ(parse("2026-09-12T10:00:", kJst), int64_t{-1});
  CHECK_EQ(parse("2026-09-12T10:00x", kJst), int64_t{-1});
  CHECK_EQ(parse("2026-09-12T10:00:30x", kJst), int64_t{-1});
  CHECK_EQ(parse("+026-09-12T10:00", kJst), int64_t{-1});
  CHECK_EQ(parse("2026-09-12T1a:00", kJst), int64_t{-1});
  CHECK_EQ(parse("2026-13-01T00:00", kJst), int64_t{-1});
  CHECK_EQ(parse("2026-02-29T00:00", kJst), int64_t{-1});
  int64_t e = 777;
  CHECK(!toolcheck::parseLocalDateTime("2026-09-12", kJst, &e));
  CHECK_EQ(e, int64_t{777});
}
