#include "csv_export.h"

#include <cstdio>
#include <cstring>

#include "time_format.h"

namespace toolcheck {

CsvLine::CsvLine(char* buf, size_t buf_len) : buf_(buf), cap_(buf_len) {
  if (buf_ == nullptr || cap_ == 0) {
    ok_ = false;
    return;
  }
  buf_[0] = '\0';
}

bool CsvLine::appendRaw(const char* text, size_t n) {
  if (!ok_) return false;
  if (len_ + n + 1 > cap_) {
    // 途中まで書いた行を残すと、受け手が列のずれた行を読んでしまう。空にして失敗を知らせる
    ok_ = false;
    len_ = 0;
    buf_[0] = '\0';
    return false;
  }
  std::memcpy(buf_ + len_, text, n);
  len_ += n;
  buf_[len_] = '\0';
  return true;
}

bool CsvLine::beginField() {
  if (!ok_) return false;
  if (fields_ > 0 && !appendRaw(",", 1)) return false;
  ++fields_;
  return true;
}

CsvLine& CsvLine::addText(const char* text) {
  if (!beginField() || text == nullptr) return *this;
  const size_t n = std::strlen(text);
  bool needQuote = false;
  for (size_t i = 0; i < n; ++i) {
    const char c = text[i];
    if (c == ',' || c == '"' || c == '\r' || c == '\n') {
      needQuote = true;
      break;
    }
  }
  if (!needQuote) {
    appendRaw(text, n);
    return *this;
  }
  if (!appendRaw("\"", 1)) return *this;
  for (size_t i = 0; i < n; ++i) {
    const bool ok = (text[i] == '"') ? appendRaw("\"\"", 2) : appendRaw(&text[i], 1);
    if (!ok) return *this;
  }
  appendRaw("\"", 1);
  return *this;
}

CsvLine& CsvLine::addInt(int64_t value) {
  if (!beginField()) return *this;
  char text[24];
  const int n = std::snprintf(text, sizeof(text), "%lld", static_cast<long long>(value));
  if (n > 0) appendRaw(text, static_cast<size_t>(n));
  return *this;
}

CsvLine& CsvLine::addDateTime(int64_t epoch, int32_t tz_offset_min) {
  if (!beginField()) return *this;
  LocalDateTime t;
  if (!toLocalDateTime(epoch, tz_offset_min, &t)) return *this;  // 時刻が無効なら空欄
  char text[32];
  const int n = std::snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day, t.hour,
                              t.minute, t.second);
  if (n > 0) appendRaw(text, static_cast<size_t>(n));
  return *this;
}

bool CsvLine::ok() const { return ok_; }

const char* CsvLine::c_str() const { return (buf_ != nullptr && cap_ > 0) ? buf_ : ""; }

size_t CsvLine::length() const { return len_; }

bool formatSectionBegin(const char* name, char* buf, size_t buf_len) {
  if (buf == nullptr || buf_len == 0) return false;
  const int n = std::snprintf(buf, buf_len, "#BEGIN %s", (name != nullptr) ? name : "");
  if (n < 0 || static_cast<size_t>(n) >= buf_len) {
    buf[0] = '\0';
    return false;
  }
  return true;
}

bool formatSectionEnd(const char* name, size_t rows, char* buf, size_t buf_len) {
  if (buf == nullptr || buf_len == 0) return false;
  const int n = std::snprintf(buf, buf_len, "#END %s rows=%lu", (name != nullptr) ? name : "",
                              static_cast<unsigned long>(rows));
  if (n < 0 || static_cast<size_t>(n) >= buf_len) {
    buf[0] = '\0';
    return false;
  }
  return true;
}

}  // namespace toolcheck
