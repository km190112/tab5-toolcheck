// 画面に出すライセンスの全文 (設定画面「このソフトについて」→「ライセンス全文」)。
// 中身は license_text.cpp にあり、tools/gen_license_text.ps1 が LICENSE と licenses/ から生成する。手で直さない。
// 元のファイルと一致することは tests/test_license_text.cpp が確かめる。
#pragma once

#include <cstddef>

namespace toolcheck {

struct LicenseSection {
  const char* title;   // 画面の節の名前
  const char* source;  // 元のファイル (リポジトリの根からの相対パス)
  const char* text;    // 全文 (UTF-8、改行は LF、BOM なし)
};

extern const LicenseSection kLicenseSections[];
extern const size_t kLicenseSectionCount;

// 全文を載せないもの (Arduino esp32 コアの LGPL-2.1 など) を 1 行で示す
extern const char* const kLicenseNoteLines[];
extern const size_t kLicenseNoteLineCount;

}  // namespace toolcheck
