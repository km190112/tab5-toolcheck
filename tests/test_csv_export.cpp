// csv_export: USB シリアル出力用の CSV 1 行とセクションの区切り (docs/設計.md「設定画面」)
#include <string>

#include "core/csv_export.h"
#include "testing.h"

using toolcheck::CsvLine;
using toolcheck::formatSectionBegin;
using toolcheck::formatSectionEnd;

namespace {
const int64_t kSep12Utc = 1789171200;  // 2026-09-12T00:00:00Z
const int32_t kJst = 9 * 60;
}  // namespace

TEST(csv_plain_fields_are_joined_with_commas) {
  char buf[64];
  CsvLine line(buf, sizeof(buf));
  line.addText("TW-025").addText("1234567").addInt(3);
  CHECK(line.ok());
  CHECK_EQ(std::string(line.c_str()), std::string("TW-025,1234567,3"));
  CHECK_EQ(line.length(), size_t{16});
}

TEST(csv_empty_and_null_text_are_empty_fields) {
  char buf[64];
  CsvLine line(buf, sizeof(buf));
  line.addText("A").addText("").addText(nullptr).addText("B");
  CHECK_EQ(std::string(line.c_str()), std::string("A,,,B"));
}

TEST(csv_single_empty_field_is_empty_line) {
  char buf[8];
  CsvLine line(buf, sizeof(buf));
  line.addText("");
  CHECK(line.ok());
  CHECK_EQ(std::string(line.c_str()), std::string(""));
}

TEST(csv_negative_and_large_integers) {
  char buf[64];
  CsvLine line(buf, sizeof(buf));
  line.addInt(-5).addInt(9007199254740993LL);
  CHECK_EQ(std::string(line.c_str()), std::string("-5,9007199254740993"));
}

TEST(csv_quotes_comma) {
  char buf[64];
  CsvLine line(buf, sizeof(buf));
  line.addText("a,b").addText("c");
  CHECK_EQ(std::string(line.c_str()), std::string("\"a,b\",c"));
}

TEST(csv_doubles_quote_characters) {
  char buf[64];
  CsvLine line(buf, sizeof(buf));
  line.addText("say \"hi\"");
  CHECK_EQ(std::string(line.c_str()), std::string("\"say \"\"hi\"\"\""));
}

TEST(csv_quotes_line_breaks) {
  char buf[64];
  CsvLine line(buf, sizeof(buf));
  line.addText("a\nb").addText("c\rd");
  CHECK_EQ(std::string(line.c_str()), std::string("\"a\nb\",\"c\rd\""));
}

TEST(csv_utf8_text_is_kept_as_is) {
  char buf[64];
  CsvLine line(buf, sizeof(buf));
  line.addText("代理").addText("返却漏れ");
  CHECK_EQ(std::string(line.c_str()), std::string("代理,返却漏れ"));
}

TEST(csv_datetime_in_local_time) {
  char buf[64];
  CsvLine line(buf, sizeof(buf));
  line.addDateTime(kSep12Utc + 3600 + 42 * 60 + 33, kJst).addText("x");
  CHECK_EQ(std::string(line.c_str()), std::string("2026-09-12 10:42:33,x"));
}

TEST(csv_invalid_datetime_is_empty_field) {
  char buf[64];
  CsvLine line(buf, sizeof(buf));
  line.addText("a").addDateTime(1000, kJst).addText("b");
  CHECK_EQ(std::string(line.c_str()), std::string("a,,b"));
}

TEST(csv_overflow_empties_line_and_stays_failed) {
  char buf[8];
  CsvLine line(buf, sizeof(buf));
  line.addText("ABC").addText("DEFGHIJ");  // "ABC,DEFGHIJ" は 11 文字 + NUL
  CHECK(!line.ok());
  CHECK_EQ(std::string(line.c_str()), std::string(""));
  line.addText("X");
  CHECK(!line.ok());
  CHECK_EQ(std::string(line.c_str()), std::string(""));
}

TEST(csv_exact_fit_is_ok) {
  char buf[6];
  CsvLine line(buf, sizeof(buf));
  line.addText("AB").addText("CD");  // "AB,CD" は 5 文字 + NUL
  CHECK(line.ok());
  CHECK_EQ(std::string(line.c_str()), std::string("AB,CD"));
}

TEST(csv_section_markers) {
  char buf[32];
  CHECK(formatSectionBegin("hist", buf, sizeof(buf)));
  CHECK_EQ(std::string(buf), std::string("#BEGIN hist"));
  CHECK(formatSectionEnd("hist", 100, buf, sizeof(buf)));
  CHECK_EQ(std::string(buf), std::string("#END hist rows=100"));
}

TEST(csv_section_marker_small_buffer_fails) {
  char buf[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
  CHECK(!formatSectionEnd("openlog", 200, buf, sizeof(buf)));
  CHECK_EQ(std::string(buf), std::string(""));
}
