// USB シリアル出力用の CSV (docs/設計.md「設定画面」USB シリアル出力)。
// 1 行ぶんを固定長のバッファに組み立てる。セクションは "#BEGIN <名前>" と "#END <名前> rows=<件数>" で挟む。
// Arduino 非依存。
//
// 既知の穴: 先頭が = + - @ の値は Excel で数式として解釈されうるが、物品番号を書き換えないために手を入れていない。
#pragma once

#include <cstddef>
#include <cstdint>

namespace toolcheck {

class CsvLine {
 public:
  CsvLine(char* buf, size_t buf_len);

  // 文字列。カンマ・ダブルクォート・CR・LF を含むなら "" で囲み、" は "" に重ねる (RFC 4180)。nullptr は空欄
  CsvLine& addText(const char* text);
  // 整数
  CsvLine& addInt(int64_t value);
  // 現地の日時 "YYYY-MM-DD hh:mm:ss"。時刻が無効なら空欄
  CsvLine& addDateTime(int64_t epoch, int32_t tz_offset_min);

  // ここまでの全部がバッファに収まったか。一度溢れたら以後の add は何もせず、中身は空文字列になる
  bool ok() const;
  const char* c_str() const;
  size_t length() const;

 private:
  bool appendRaw(const char* text, size_t n);
  bool beginField();

  char* buf_;
  size_t cap_;
  size_t len_ = 0;
  size_t fields_ = 0;
  bool ok_ = true;
};

// "#BEGIN <name>"。buf に収まらなければ false (buf は空文字列)
bool formatSectionBegin(const char* name, char* buf, size_t buf_len);

// "#END <name> rows=<rows>"。buf に収まらなければ false (buf は空文字列)
bool formatSectionEnd(const char* name, size_t rows, char* buf, size_t buf_len);

}  // namespace toolcheck
