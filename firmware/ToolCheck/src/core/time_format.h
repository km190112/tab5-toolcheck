// 時刻まわりの表示: 経過時間 (「N分前」「N時間前」)、前日判定、現地時刻の "HH:MM"。
// Arduino 非依存。時刻は UTC の UNIX 秒で受け取り、時差 (分) は呼び出し側が設定から渡す。
#pragma once

#include <cstddef>
#include <cstdint>

namespace toolcheck {

// 2026-01-01T00:00:00Z。これより前は「時刻未設定」とみなす (RTC が初期値に戻っている)
constexpr int64_t kValidEpochMin = 1767225600;

bool isValidEpoch(int64_t epoch);

// 経過時間。60 分未満は「N分前」、それ以上は「N時間前」。
// どちらかの時刻が無効、または since が now より未来なら「--」。
// buf に収まらなければ false を返し、buf は空文字列にする
bool formatElapsed(int64_t now_epoch, int64_t since_epoch, char* buf, size_t buf_len);

// 時差を足した現地の日付で、since が now より前の日なら true。どちらかが無効なら false
bool isPreviousDay(int64_t now_epoch, int64_t since_epoch, int32_t tz_offset_min);

// 現地時刻の "HH:MM"。時刻が無効なら "--:--"。buf に収まらなければ false (buf は空文字列)
bool formatClock(int64_t epoch, int32_t tz_offset_min, char* buf, size_t buf_len);

struct LocalDateTime {
  int year    = 0;
  int month   = 0;  // 1〜12
  int day     = 0;  // 1〜31
  int hour    = 0;
  int minute  = 0;
  int second  = 0;
  int weekday = 0;  // 0 = 日曜 … 6 = 土曜
};

// 時差を足した現地の日時に分解する。時刻が無効なら false を返し、out は変えない
bool toLocalDateTime(int64_t epoch, int32_t tz_offset_min, LocalDateTime* out);

// 現地の日時 (weekday は見ない) から時差を引いて UTC の UNIX 秒にする。
// 年 1970〜9999・月・日 (うるう年を含む)・時・分・秒のどれかが範囲外なら false を返し、out は変えない。
// 2026 年より前でも変換する (RTC が初期値に戻ったことは、変換した後に isValidEpoch で見分ける)
bool fromLocalDateTime(const LocalDateTime& local, int32_t tz_offset_min, int64_t* out);

// "YYYY-MM-DDTHH:MM" か "YYYY-MM-DDTHH:MM:SS" (現地時刻、桁数は固定) を読み、UTC の UNIX 秒にする。
// 書式・範囲の誤りは false を返し、out は変えない
bool parseLocalDateTime(const char* text, int32_t tz_offset_min, int64_t* out);

}  // namespace toolcheck
