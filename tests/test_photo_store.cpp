// photo_store: 写真のファイル名・古い写真の選び方・SD 保存の実効状態 (docs/設計.md「写真」)
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/photo_store.h"
#include "testing.h"

using toolcheck::isPhotoCleanupIdle;
using toolcheck::makePhotoName;
using toolcheck::PhotoFile;
using toolcheck::selectPhotosToDelete;
using toolcheck::shouldSavePhotoToSd;

namespace {

const int64_t kSep12Utc = 1789171200;  // 2026-09-12T00:00:00Z
const int32_t kJst = 9 * 60;
const int64_t kAt104233Jst = kSep12Utc + 3600 + 42 * 60 + 33;  // JST 2026-09-12 10:42:33

std::string photoName(int64_t epoch, uint32_t uptime_s, uint8_t seq) {
  char buf[40];
  if (!makePhotoName(epoch, kJst, uptime_s, seq, buf, sizeof(buf))) return "<false>";
  return std::string(buf);
}

// "20260912-000000.jpg" から連番で n 枚分の名前
std::vector<std::string> numberedNames(int n) {
  std::vector<std::string> names;
  for (int i = 0; i < n; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "20260912-%06d.jpg", i);
    names.push_back(buf);
  }
  return names;
}

std::string deletedNames(const std::vector<std::string>& names, const std::vector<uint32_t>& sizes, uint16_t keep,
                         size_t out_cap) {
  std::vector<PhotoFile> files;
  for (size_t i = 0; i < names.size(); ++i) files.push_back(PhotoFile{names[i].c_str(), sizes[i]});
  std::vector<size_t> out(out_cap == 0 ? 1 : out_cap);
  size_t n = selectPhotosToDelete(files.data(), files.size(), keep, out.data(), out_cap);
  std::string joined;
  for (size_t i = 0; i < n; ++i) {
    if (!joined.empty()) joined += ",";
    joined += names[out[i]];
  }
  return joined;
}

}  // namespace

// --- ファイル名 ---

TEST(photo_name_uses_local_time) { CHECK_EQ(photoName(kAt104233Jst, 0, 1), std::string("20260912-104233.jpg")); }

TEST(photo_name_adds_sequence_within_same_second) {
  CHECK_EQ(photoName(kAt104233Jst, 0, 2), std::string("20260912-104233-2.jpg"));
}

TEST(photo_name_when_time_unset_uses_uptime) {
  CHECK_EQ(photoName(1000, 12345, 1), std::string("00000000-0000012345.jpg"));
  CHECK_EQ(photoName(1000, 12345, 3), std::string("00000000-0000012345-3.jpg"));
}

TEST(photo_name_time_unset_sorts_before_valid) {
  const std::string unset = photoName(1000, 4294967295u, 1);
  const std::string valid = photoName(kAt104233Jst, 0, 1);
  CHECK(unset < valid);
}

TEST(photo_name_small_buffer_fails) {
  char buf[10] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
  CHECK(!makePhotoName(kAt104233Jst, kJst, 0, 1, buf, sizeof(buf)));
  CHECK_EQ(std::string(buf), std::string(""));
}

// --- 古い写真の選び方 ---

TEST(photo_delete_none_at_keep_count) {
  auto names = numberedNames(100);
  std::vector<uint32_t> sizes(names.size(), 150000);
  CHECK_EQ(deletedNames(names, sizes, 100, names.size()), std::string(""));
}

TEST(photo_delete_oldest_beyond_keep_count) {
  auto names = numberedNames(105);
  std::reverse(names.begin(), names.end());  // 並び順に依存しないこと
  std::vector<uint32_t> sizes(names.size(), 150000);
  CHECK_EQ(deletedNames(names, sizes, 100, names.size()),
           std::string("20260912-000000.jpg,20260912-000001.jpg,20260912-000002.jpg,20260912-000003.jpg,"
                       "20260912-000004.jpg"));
}

TEST(photo_delete_zero_byte_always) {
  std::vector<std::string> names = {"20260912-000001.jpg", "20260912-000002.jpg", "20260912-000003.jpg"};
  std::vector<uint32_t> sizes = {150000, 0, 150000};
  CHECK_EQ(deletedNames(names, sizes, 100, names.size()), std::string("20260912-000002.jpg"));
}

TEST(photo_zero_byte_is_not_counted_in_keep) {
  auto names = numberedNames(102);
  std::vector<uint32_t> sizes(names.size(), 150000);
  sizes[101] = 0;  // いちばん新しい 1 枚が壊れている → 中身のある写真は 101 枚
  CHECK_EQ(deletedNames(names, sizes, 100, names.size()), std::string("20260912-000000.jpg,20260912-000101.jpg"));
}

TEST(photo_delete_respects_out_capacity) {
  auto names = numberedNames(105);
  std::vector<uint32_t> sizes(names.size(), 150000);
  CHECK_EQ(deletedNames(names, sizes, 100, 2), std::string("20260912-000000.jpg,20260912-000001.jpg"));
}

TEST(photo_keep_zero_deletes_all) {
  auto names = numberedNames(3);
  std::vector<uint32_t> sizes(names.size(), 150000);
  CHECK_EQ(deletedNames(names, sizes, 0, names.size()),
           std::string("20260912-000000.jpg,20260912-000001.jpg,20260912-000002.jpg"));
}

// --- SD 保存の実効状態・アイドル ---

TEST(photo_save_to_sd_needs_all_three) {
  CHECK(shouldSavePhotoToSd(true, true, true));
  CHECK(!shouldSavePhotoToSd(true, true, false));   // SD が無い → 自動で OFF
  CHECK(!shouldSavePhotoToSd(true, false, true));   // 設定が OFF
  CHECK(!shouldSavePhotoToSd(false, true, true));   // カメラを使わない
}

TEST(photo_cleanup_needs_idle) {
  CHECK(isPhotoCleanupIdle(false, false, 10000, 10000));
  CHECK(!isPhotoCleanupIdle(false, false, 9999, 10000));
  CHECK(!isPhotoCleanupIdle(true, false, 60000, 10000));  // セッション中
  CHECK(!isPhotoCleanupIdle(false, true, 60000, 10000));  // 引き出しが開いている
}
