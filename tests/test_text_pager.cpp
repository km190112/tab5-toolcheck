// text_pager: 長い文を幅で行に折り、ページに分ける (設定画面「ライセンス全文」)
#include "core/text_pager.h"

#include <cstring>
#include <string>

#include "testing.h"

using toolcheck::TextLine;
using toolcheck::TextPager;

namespace {

// 偽物の幅: ASCII は 1、それ以外 (日本語など) は 2
int fakeWidth(const char* utf8, size_t len, void* ctx) {
  auto* calls = static_cast<int*>(ctx);
  if (calls != nullptr) ++*calls;
  (void)utf8;
  return len == 1 ? 1 : 2;
}

// 折った結果を "行|行|行" にする
std::string layoutJoined(const char* text, int max_width) {
  TextPager pager;
  if (!pager.layout(text, std::strlen(text), max_width, &fakeWidth, nullptr)) return "<failed>";
  std::string out;
  for (size_t i = 0; i < pager.lineCount(); ++i) {
    const TextLine l = pager.line(i);
    if (i > 0) out += "|";
    out.append(text + l.offset, l.length);
  }
  return out;
}

}  // namespace

TEST(pager_empty_text_has_no_lines) {
  CHECK_EQ(layoutJoined("", 10), std::string(""));
  TextPager pager;
  CHECK(pager.layout("", 0, 10, &fakeWidth, nullptr));
  CHECK_EQ(pager.lineCount(), size_t{0});
}

TEST(pager_short_text_is_one_line) {
  CHECK_EQ(layoutJoined("abc", 10), std::string("abc"));
  CHECK_EQ(layoutJoined("abcdefghij", 10), std::string("abcdefghij"));  // ちょうど幅いっぱい
}

TEST(pager_wraps_at_space_and_drops_the_space) {
  CHECK_EQ(layoutJoined("hello world foo", 11), std::string("hello world|foo"));
  CHECK_EQ(layoutJoined("hello world foo", 8), std::string("hello|world|foo"));
  // 折った所に空白が続いても次の行の頭には残さない
  CHECK_EQ(layoutJoined("aaaa    bbbb", 6), std::string("aaaa|bbbb"));
}

TEST(pager_long_word_is_split_by_character) {
  CHECK_EQ(layoutJoined("abcdefghijkl", 5), std::string("abcde|fghij|kl"));
  CHECK_EQ(layoutJoined("ab abcdefghijkl", 5), std::string("ab|abcde|fghij|kl"));
}

TEST(pager_keeps_explicit_newlines_and_blank_lines) {
  CHECK_EQ(layoutJoined("a\n\nb", 10), std::string("a||b"));
  CHECK_EQ(layoutJoined("a\r\nb", 10), std::string("a|b"));
  CHECK_EQ(layoutJoined("a\r\n\r\nb", 10), std::string("a||b"));
}

TEST(pager_last_newline_does_not_add_a_line) {
  CHECK_EQ(layoutJoined("a\n", 10), std::string("a"));
  CHECK_EQ(layoutJoined("a\n\n", 10), std::string("a|"));
}

TEST(pager_keeps_leading_indent_of_paragraph) {
  CHECK_EQ(layoutJoined("  ab cd ef", 6), std::string("  ab|cd ef"));
}

TEST(pager_never_splits_utf8_and_breaks_between_japanese) {
  // 1 文字 3 バイト・幅 2。幅 6 なら 3 文字ずつ
  CHECK_EQ(layoutJoined("あいうえおかきく", 6), std::string("あいう|えおか|きく"));
  CHECK_EQ(layoutJoined("あいうえおかきく", 7), std::string("あいう|えおか|きく"));
}

TEST(pager_breaks_between_japanese_and_ascii) {
  // 日本語の後ろの英字の前でも折れる (英字の語の途中より先に)
  CHECK_EQ(layoutJoined("日本語text", 8), std::string("日本語|text"));
  CHECK_EQ(layoutJoined("abc日本", 5), std::string("abc日|本"));
}

TEST(pager_glyph_wider_than_width_gets_its_own_line) {
  CHECK_EQ(layoutJoined("aあb", 1), std::string("a|あ|b"));
}

TEST(pager_broken_utf8_byte_counts_as_one_character) {
  const char text[] = {'a', static_cast<char>(0xE3), 'b', '\0'};  // 続きのバイトが無い 0xE3
  TextPager pager;
  CHECK(pager.layout(text, 3, 10, &fakeWidth, nullptr));
  CHECK_EQ(pager.lineCount(), size_t{1});
  CHECK_EQ(pager.line(0).length, uint32_t{3});
}

TEST(pager_measures_each_character_once) {
  int calls = 0;
  TextPager pager;
  const char* text = "hello world foo bar baz";
  CHECK(pager.layout(text, std::strlen(text), 7, &fakeWidth, &calls));
  CHECK(calls <= static_cast<int>(std::strlen(text)));
}

TEST(pager_layout_replaces_previous_result) {
  TextPager pager;
  CHECK(pager.layout("a\nb\nc", 5, 10, &fakeWidth, nullptr));
  CHECK_EQ(pager.lineCount(), size_t{3});
  CHECK(pager.layout("x", 1, 10, &fakeWidth, nullptr));
  CHECK_EQ(pager.lineCount(), size_t{1});
}

TEST(pager_out_of_range_line_is_empty) {
  TextPager pager;
  CHECK(pager.layout("abc", 3, 10, &fakeWidth, nullptr));
  CHECK_EQ(pager.line(5).length, uint32_t{0});
}

namespace {

// 折り返しの改行をつなぐ指定で折り、つないだ改行は "~" に置き換えて "行|行" にする
std::string layoutJoinedReflow(const std::string& text, int max_width) {
  TextPager pager;
  if (!pager.layout(text.c_str(), text.size(), max_width, &fakeWidth, nullptr, true)) return "<failed>";
  std::string out;
  for (size_t i = 0; i < pager.lineCount(); ++i) {
    const TextLine l = pager.line(i);
    if (i > 0) out += "|";
    std::string s = text.substr(l.offset, l.length);
    for (char& c : s) {
      if (c == '\n') c = '~';
    }
    out += s;
  }
  return out;
}

// 60 バイトの行 (英文のライセンスは 80 桁ほどで改行してある)
const std::string kLong60 = std::string(59, 'a') + "b";

}  // namespace

TEST(pager_reflow_joins_hard_wrapped_lines) {
  // 前の行が 60 バイト以上で、次の行が普通の文で始まるなら、改行を空白としてつなぐ
  CHECK_EQ(layoutJoinedReflow(kLong60 + "\ncc", 100), kLong60 + "~cc");
  // つないだ後も幅で折る (改行の所で折れる)
  CHECK_EQ(layoutJoinedReflow(kLong60 + "\ncc", 60), kLong60 + "|cc");
}

TEST(pager_reflow_keeps_short_lines_blank_lines_and_list_items) {
  CHECK_EQ(layoutJoinedReflow("short\ncc", 100), std::string("short|cc"));             // 前の行が短い
  CHECK_EQ(layoutJoinedReflow(kLong60 + "\n\ncc", 100), kLong60 + "||cc");            // 空行
  CHECK_EQ(layoutJoinedReflow(kLong60 + "\n2. item", 100), kLong60 + "|2. item");     // 番号
  CHECK_EQ(layoutJoinedReflow(kLong60 + "\n(1) item", 100), kLong60 + "|(1) item");   // (1)
  CHECK_EQ(layoutJoinedReflow(kLong60 + "\n- item", 100), kLong60 + "|- item");       // 箇条書き
  CHECK_EQ(layoutJoinedReflow(kLong60 + "\n  indent", 100), kLong60 + "|  indent");   // 字下げ
  CHECK_EQ(layoutJoinedReflow(kLong60 + "\n\xE3\x80\x80(1)", 100), kLong60 + "|\xE3\x80\x80(1)");  // 全角空白
  CHECK_EQ(layoutJoinedReflow(kLong60 + "\n", 100), kLong60);                          // 最後の改行
}

TEST(pager_reflow_is_off_by_default) {
  CHECK_EQ(layoutJoined((kLong60 + "\ncc").c_str(), 100), kLong60 + "|cc");
}

TEST(pager_page_count_and_first_line) {
  CHECK_EQ(TextPager::pageCount(0, 24), size_t{1});
  CHECK_EQ(TextPager::pageCount(1, 24), size_t{1});
  CHECK_EQ(TextPager::pageCount(24, 24), size_t{1});
  CHECK_EQ(TextPager::pageCount(25, 24), size_t{2});
  CHECK_EQ(TextPager::pageCount(10, 0), size_t{10});  // 0 は 1 行ずつとみなす
  CHECK_EQ(TextPager::firstLineOfPage(0, 24), size_t{0});
  CHECK_EQ(TextPager::firstLineOfPage(2, 24), size_t{48});
  CHECK_EQ(TextPager::firstLineOfPage(3, 0), size_t{3});
}
