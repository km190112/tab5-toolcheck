#include "text_pager.h"

namespace toolcheck {

namespace {

// p から始まる 1 文字のバイト数。壊れた並びは 1
size_t charLength(const char* p, size_t remain) {
  const auto b = static_cast<unsigned char>(p[0]);
  size_t n = 1;
  if (b >= 0xF0 && b <= 0xF4) {
    n = 4;
  } else if (b >= 0xE0) {
    n = 3;
  } else if (b >= 0xC2 && b <= 0xDF) {
    n = 2;
  }
  if (n == 1 || n > remain) return 1;
  for (size_t i = 1; i < n; ++i) {
    if ((static_cast<unsigned char>(p[i]) & 0xC0) != 0x80) return 1;
  }
  return n;
}

// 決まった桁で改行してあるだけの行の続きか (空行・字下げ・全角空白・番号・'(' '-' '*' で始まる行は新しい段落)
bool continuesParagraph(const char* p, size_t remain) {
  const auto b = static_cast<unsigned char>(p[0]);
  if (b == '\n' || b == '\r' || b == ' ' || b == '\t' || b == '(' || b == '-' || b == '*') return false;
  if (b >= '0' && b <= '9') return false;
  if (remain >= 3 && b == 0xE3 && static_cast<unsigned char>(p[1]) == 0x80 && static_cast<unsigned char>(p[2]) == 0x80) {
    return false;  // 全角空白
  }
  return true;
}

// 折れる所: 行の中身を end で終え、次の行を next から始める。next までの幅が width_at_next
struct BreakPoint {
  bool valid = false;
  size_t end = 0;
  size_t next = 0;
  int width_at_next = 0;
};

}  // namespace

bool TextPager::layout(const char* text, size_t len, int max_width, GlyphWidthFn width, void* ctx,
                       bool join_hard_wraps) {
  lines_.clear();
  if ((text == nullptr && len > 0) || width == nullptr) return false;

  const auto push = [this](size_t start, size_t end) {
    lines_.push_back(TextLine{static_cast<uint32_t>(start), static_cast<uint32_t>(end - start)});
  };

  size_t para = 0;
  while (para < len) {
    // 段落の終わり (LF の手前。CR は含めない)。決まった桁で改行してあるだけの行は、次の行もこの段落に入れる
    size_t para_end = para;
    size_t row_start = para;
    for (;;) {
      size_t row_end = row_start;
      while (row_end < len && text[row_end] != '\n') ++row_end;
      para_end = row_end;
      if (!join_hard_wraps || row_end >= len) break;
      size_t row_content = row_end;
      if (row_content > row_start && text[row_content - 1] == '\r') --row_content;
      const size_t next = row_end + 1;
      if (row_content - row_start < kHardWrapMinBytes || next >= len || !continuesParagraph(text + next, len - next)) {
        break;
      }
      row_start = next;
    }
    size_t content_end = para_end;
    if (content_end > para && text[content_end - 1] == '\r') --content_end;

    size_t line_start = para;
    int cur = 0;
    BreakPoint bp;
    size_t space_run_start = 0;  // 続いている空白の頭 (空白の中にいないときは 0 以外でも使わない)
    bool in_space = false;
    bool prev_ascii = true;
    bool has_prev = false;

    size_t pos = para;
    while (pos < content_end) {
      const size_t n = charLength(text + pos, content_end - pos);
      // つないだ改行 (と、その手前の CR) は空白として扱う
      const bool joined_break = n == 1 && (text[pos] == '\n' || text[pos] == '\r');
      const bool is_space = n == 1 && (text[pos] == ' ' || joined_break);
      const bool is_ascii = n == 1 && static_cast<unsigned char>(text[pos]) < 0x80;
      const int w = joined_break ? (text[pos] == '\r' ? 0 : width(" ", 1, ctx)) : width(text + pos, n, ctx);

      if (is_space) {
        // 段落の頭の字下げは折る所にしない。空白で幅を超えても折らない (次の文字で折る)
        if (!in_space) {
          space_run_start = pos;
          in_space = true;
        }
        cur += w;
        pos += n;
        has_prev = true;
        prev_ascii = true;
        continue;
      }

      // この文字の手前が折れる所か
      if (in_space && space_run_start > line_start) {
        bp = BreakPoint{true, space_run_start, pos, cur};
      } else if (has_prev && pos > line_start && !(prev_ascii && is_ascii) && !in_space) {
        bp = BreakPoint{true, pos, pos, cur};
      }
      in_space = false;

      if (cur + w > max_width && pos > line_start) {
        if (bp.valid && bp.next > line_start) {
          push(line_start, bp.end);
          line_start = bp.next;
          cur -= bp.width_at_next;
          bp.valid = false;
        }
        if (cur + w > max_width && pos > line_start) {
          push(line_start, pos);
          line_start = pos;
          cur = 0;
          bp.valid = false;
        }
      }
      cur += w;
      pos += n;
      has_prev = true;
      prev_ascii = is_ascii;
    }
    push(line_start, content_end < line_start ? line_start : content_end);

    if (para_end >= len) break;
    para = para_end + 1;  // 最後の改行の後ろには行を足さない (ループが終わる)
  }
  return true;
}

TextLine TextPager::line(size_t index) const { return index < lines_.size() ? lines_[index] : TextLine{}; }

size_t TextPager::pageCount(size_t lines, size_t lines_per_page) {
  const size_t per = lines_per_page == 0 ? 1 : lines_per_page;
  const size_t pages = (lines + per - 1) / per;
  return pages == 0 ? 1 : pages;
}

size_t TextPager::firstLineOfPage(size_t page, size_t lines_per_page) {
  return page * (lines_per_page == 0 ? 1 : lines_per_page);
}

}  // namespace toolcheck
