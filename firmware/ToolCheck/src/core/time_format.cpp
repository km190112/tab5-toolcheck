#include "time_format.h"

#include <cstdio>

namespace toolcheck {
namespace {

const int64_t kSecondsPerDay = 86400;

// text を buf に写す。入りきらなければ buf を空文字列にして false
bool writeText(char* buf, size_t buf_len, const char* text) {
  if (buf == nullptr || buf_len == 0) return false;
  size_t n = 0;
  while (text[n] != '\0') ++n;
  if (n + 1 > buf_len) {
    buf[0] = '\0';
    return false;
  }
  for (size_t i = 0; i <= n; ++i) buf[i] = text[i];
  return true;
}

// 時差を足した現地時刻の「1970-01-01 からの日数」。負の時刻でも切り捨てで数える
int64_t localDay(int64_t epoch, int32_t tz_offset_min) {
  const int64_t local = epoch + static_cast<int64_t>(tz_offset_min) * 60;
  if (local >= 0) return local / kSecondsPerDay;
  return -((-local + kSecondsPerDay - 1) / kSecondsPerDay);
}

}  // namespace

bool isValidEpoch(int64_t epoch) { return epoch >= kValidEpochMin; }

bool formatElapsed(int64_t now_epoch, int64_t since_epoch, char* buf, size_t buf_len) {
  if (buf == nullptr || buf_len == 0) return false;
  if (!isValidEpoch(now_epoch) || !isValidEpoch(since_epoch) || since_epoch > now_epoch) {
    return writeText(buf, buf_len, "--");
  }
  const int64_t seconds = now_epoch - since_epoch;
  char text[32];
  if (seconds < 3600) {
    std::snprintf(text, sizeof(text), "%lld分前", static_cast<long long>(seconds / 60));
  } else {
    std::snprintf(text, sizeof(text), "%lld時間前", static_cast<long long>(seconds / 3600));
  }
  return writeText(buf, buf_len, text);
}

bool isPreviousDay(int64_t now_epoch, int64_t since_epoch, int32_t tz_offset_min) {
  if (!isValidEpoch(now_epoch) || !isValidEpoch(since_epoch)) return false;
  return localDay(since_epoch, tz_offset_min) < localDay(now_epoch, tz_offset_min);
}

bool formatClock(int64_t epoch, int32_t tz_offset_min, char* buf, size_t buf_len) {
  if (buf == nullptr || buf_len == 0) return false;
  if (!isValidEpoch(epoch)) return writeText(buf, buf_len, "--:--");
  const int64_t local = epoch + static_cast<int64_t>(tz_offset_min) * 60;
  const int64_t secondOfDay = local - localDay(epoch, tz_offset_min) * kSecondsPerDay;
  const int hour = static_cast<int>(secondOfDay / 3600);
  const int minute = static_cast<int>((secondOfDay % 3600) / 60);
  char text[8];
  std::snprintf(text, sizeof(text), "%02d:%02d", hour, minute);
  return writeText(buf, buf_len, text);
}

bool toLocalDateTime(int64_t epoch, int32_t tz_offset_min, LocalDateTime* out) {
  if (out == nullptr || !isValidEpoch(epoch)) return false;
  const int64_t days = localDay(epoch, tz_offset_min);
  const int64_t local = epoch + static_cast<int64_t>(tz_offset_min) * 60;
  const int64_t secondOfDay = local - days * kSecondsPerDay;

  // 1970-01-01 からの日数 → 年月日 (Howard Hinnant の civil_from_days)
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;                                   // 0〜146096
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // 0〜399
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);            // 0〜365
  const int64_t mp = (5 * doy + 2) / 153;                                 // 0〜11 (3 月始まり)
  const int64_t day = doy - (153 * mp + 2) / 5 + 1;
  const int64_t month = (mp < 10) ? mp + 3 : mp - 9;
  const int64_t year = yoe + era * 400 + ((month <= 2) ? 1 : 0);

  int64_t weekday = (days + 4) % 7;  // 1970-01-01 は木曜 (4)
  if (weekday < 0) weekday += 7;

  out->year = static_cast<int>(year);
  out->month = static_cast<int>(month);
  out->day = static_cast<int>(day);
  out->hour = static_cast<int>(secondOfDay / 3600);
  out->minute = static_cast<int>((secondOfDay % 3600) / 60);
  out->second = static_cast<int>(secondOfDay % 60);
  out->weekday = static_cast<int>(weekday);
  return true;
}

namespace {

bool isLeapYear(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

int daysInMonth(int year, int month) {
  static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return (month == 2 && isLeapYear(year)) ? 29 : kDays[month - 1];
}

// 年月日 → 1970-01-01 からの日数 (Howard Hinnant の days_from_civil)
int64_t daysFromCivil(int64_t year, int64_t month, int64_t day) {
  year -= (month <= 2) ? 1 : 0;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t yoe = year - era * 400;                                        // 0〜399
  const int64_t doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;  // 0〜365
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                   // 0〜146096
  return era * 146097 + doe - 719468;
}

// text の pos から count 文字がすべて数字なら、その値を out に入れて true
bool readDigits(const char* text, size_t pos, size_t count, int* out) {
  int value = 0;
  for (size_t i = 0; i < count; ++i) {
    const char c = text[pos + i];
    if (c < '0' || c > '9') return false;
    value = value * 10 + (c - '0');
  }
  *out = value;
  return true;
}

}  // namespace

bool fromLocalDateTime(const LocalDateTime& local, int32_t tz_offset_min, int64_t* out) {
  if (out == nullptr) return false;
  if (local.year < 1970 || local.year > 9999 || local.month < 1 || local.month > 12) return false;
  if (local.day < 1 || local.day > daysInMonth(local.year, local.month)) return false;
  if (local.hour < 0 || local.hour > 23 || local.minute < 0 || local.minute > 59 || local.second < 0 ||
      local.second > 59) {
    return false;
  }
  const int64_t seconds = daysFromCivil(local.year, local.month, local.day) * kSecondsPerDay +
                          static_cast<int64_t>(local.hour) * 3600 + static_cast<int64_t>(local.minute) * 60 +
                          local.second;
  *out = seconds - static_cast<int64_t>(tz_offset_min) * 60;
  return true;
}

bool parseLocalDateTime(const char* text, int32_t tz_offset_min, int64_t* out) {
  if (text == nullptr || out == nullptr) return false;
  // "YYYY-MM-DDTHH:MM" (16 文字) / "YYYY-MM-DDTHH:MM:SS" (19 文字)
  size_t len = 0;
  while (len < 20 && text[len] != '\0') ++len;
  if (len != 16 && len != 19) return false;
  if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':') return false;
  if (len == 19 && text[16] != ':') return false;
  LocalDateTime t;
  if (!readDigits(text, 0, 4, &t.year) || !readDigits(text, 5, 2, &t.month) || !readDigits(text, 8, 2, &t.day) ||
      !readDigits(text, 11, 2, &t.hour) || !readDigits(text, 14, 2, &t.minute)) {
    return false;
  }
  if (len == 19 && !readDigits(text, 17, 2, &t.second)) return false;
  return fromLocalDateTime(t, tz_offset_min, out);
}

}  // namespace toolcheck
