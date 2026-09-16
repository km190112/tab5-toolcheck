// license_text: 画面に出すライセンスの全文が、リポジトリの LICENSE と licenses/ の元ファイルと一致する
// (tools/gen_license_text.ps1 で生成し直し忘れたら落ちる)。about の値の形も確かめる
#include "core/license_text.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "core/about.h"
#include "testing.h"

using toolcheck::kLicenseNoteLineCount;
using toolcheck::kLicenseSectionCount;
using toolcheck::kLicenseSections;

namespace {

// このテストのソース (run.ps1 が絶対パスで渡す) から、リポジトリの根を求める
std::string repoRoot() {
  std::string here = __FILE__;
  for (int up = 0; up < 2; ++up) {
    const size_t cut = here.find_last_of("/\\");
    if (cut == std::string::npos) return "";
    here.resize(cut);
  }
  return here;
}

// BOM を落とし、CRLF を LF にそろえて読む
bool readNormalized(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  const std::string raw_read((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::string raw = raw_read;
  if (raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0) raw.erase(0, 3);
  out->clear();
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') continue;
    out->push_back(raw[i]);
  }
  return true;
}

}  // namespace

TEST(license_sections_are_the_five_expected) {
  CHECK_EQ(kLicenseSectionCount, size_t{5});
  if (kLicenseSectionCount != 5) return;
  CHECK_EQ(std::string(kLicenseSections[0].source), std::string("LICENSE"));
  CHECK_EQ(std::string(kLicenseSections[1].source), std::string("licenses/M5Unified-LICENSE.txt"));
  CHECK_EQ(std::string(kLicenseSections[2].source), std::string("licenses/M5GFX-LICENSE.txt"));
  CHECK_EQ(std::string(kLicenseSections[3].source), std::string("licenses/VL53L1X-LICENSE.txt"));
  CHECK_EQ(std::string(kLicenseSections[4].source), std::string("licenses/IPA_Font_License_Agreement_v1.0.txt"));
}

TEST(license_texts_match_repository_files) {
  const std::string root = repoRoot();
  CHECK(!root.empty());
  for (size_t i = 0; i < kLicenseSectionCount; ++i) {
    std::string expected;
    const std::string path = root + "/" + kLicenseSections[i].source;
    const bool read = readNormalized(path, &expected);
    if (!read) tt::fail(__FILE__, __LINE__, "読めない: " + path);
    if (read && expected != kLicenseSections[i].text) {
      tt::fail(__FILE__, __LINE__,
               std::string("中身が違う (tools/gen_license_text.ps1 で生成し直す): ") + kLicenseSections[i].source);
    }
    CHECK(std::strlen(kLicenseSections[i].title) > 0);
  }
}

TEST(license_own_section_names_the_developer_and_mit) {
  if (kLicenseSectionCount == 0) {
    tt::fail(__FILE__, __LINE__, "節が無い");
    return;
  }
  const std::string own = kLicenseSections[0].text;
  CHECK(own.find("MIT License") != std::string::npos);
  CHECK(own.find(toolcheck::about::kDeveloper) != std::string::npos);
}

TEST(license_notes_mention_arduino_core_lgpl) {
  CHECK(kLicenseNoteLineCount >= 1);
  bool found = false;
  for (size_t i = 0; i < kLicenseNoteLineCount; ++i) {
    if (std::strstr(toolcheck::kLicenseNoteLines[i], "LGPL") != nullptr) found = true;
  }
  CHECK(found);
}

TEST(about_values_have_expected_shape) {
  const std::string date = toolcheck::about::kReleaseDate;
  CHECK_EQ(date.size(), size_t{10});
  if (date.size() == 10) {
    CHECK(date[4] == '-' && date[7] == '-');
    for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) CHECK(date[i] >= '0' && date[i] <= '9');
  }
  CHECK_EQ(std::string(toolcheck::about::kRepoUrl).rfind("https://github.com/", 0), size_t{0});
  CHECK(std::strlen(toolcheck::about::kFirmwareVersion) > 0);
}
