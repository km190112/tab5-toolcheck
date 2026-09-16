// 長い文 (ライセンスの全文) を画面の幅で行に折り、ページに分ける (設定画面「ライセンス全文」)。
// Arduino 非依存。1 文字の幅は関数で受け取る (実機は M5GFX で測り、テストは偽物を渡す)。
//   - 明示の改行 (LF、CRLF) で段落を分ける。空の段落は空行 1 行。文の最後の改行は行を増やさない
//   - UTF-8 の途中では切らない。壊れたバイトは 1 バイトを 1 文字として扱う
//   - 空白の後か、ASCII 以外の文字の前後で折る。折れる所が無い長い語は文字の境目で折る
//   - 折った行の末尾の空白は行に含めず、次の行の頭の空白も飛ばす。段落の頭の字下げは残す
//   - 1 文字だけで幅を超えるときは、その文字だけを 1 行にする
//   - join_hard_wraps のとき、決まった桁で改行してあるだけの行 (英文のライセンスは 80 桁ほど) を前の行につなぐ:
//     前の行が kHardWrapMinBytes 以上で、次の行が空でなく、空白・全角空白・数字・'(' '-' '*' で始まらないとき。
//     つないだ改行は行の中に残る (描くときに空白として描く)。文字は変えない
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace toolcheck {

struct TextLine {
  uint32_t offset = 0;  // 文の先頭からのバイト位置
  uint32_t length = 0;  // バイト数 (改行は含まない)
};

// utf8 から len バイトの 1 文字を描いたときの幅 (ピクセル)
using GlyphWidthFn = int (*)(const char* utf8, size_t len, void* ctx);

constexpr size_t kHardWrapMinBytes = 60;

class TextPager {
 public:
  // 折り直す。前の結果は捨てる。text が null (len > 0) か幅の関数が無ければ false (行は空のまま)
  bool layout(const char* text, size_t len, int max_width, GlyphWidthFn width, void* ctx, bool join_hard_wraps = false);

  size_t lineCount() const { return lines_.size(); }
  TextLine line(size_t index) const;  // 範囲外は空の行

  // ページ数 (行が 0 でも 1)。lines_per_page が 0 なら 1 とみなす
  static size_t pageCount(size_t lines, size_t lines_per_page);
  // そのページの最初の行
  static size_t firstLineOfPage(size_t page, size_t lines_per_page);

 private:
  std::vector<TextLine> lines_;
};

}  // namespace toolcheck
