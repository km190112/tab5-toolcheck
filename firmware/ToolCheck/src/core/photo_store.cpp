#include "photo_store.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "time_format.h"

namespace toolcheck {
namespace {

// text を buf に写す。入りきらなければ buf を空文字列にして false
bool writeOrClear(char* buf, size_t buf_len, const char* text) {
  if (buf == nullptr || buf_len == 0) return false;
  const size_t n = std::strlen(text);
  if (n + 1 > buf_len) {
    buf[0] = '\0';
    return false;
  }
  std::memcpy(buf, text, n + 1);
  return true;
}

}  // namespace

bool makePhotoName(int64_t epoch, int32_t tz_offset_min, uint32_t uptime_s, uint8_t seq, char* buf, size_t buf_len) {
  if (buf == nullptr || buf_len == 0) return false;
  char suffix[8] = "";
  if (seq > 1) std::snprintf(suffix, sizeof(suffix), "-%u", static_cast<unsigned>(seq));

  char text[64];
  LocalDateTime t;
  if (toLocalDateTime(epoch, tz_offset_min, &t)) {
    std::snprintf(text, sizeof(text), "%04d%02d%02d-%02d%02d%02d%s.jpg", t.year, t.month, t.day, t.hour, t.minute,
                  t.second, suffix);
  } else {
    // 時刻が無効: 起動からの秒を桁を揃えて入れる (名前順で有効な時刻の写真より前に並び、先に消える)
    std::snprintf(text, sizeof(text), "00000000-%010lu%s.jpg", static_cast<unsigned long>(uptime_s), suffix);
  }
  return writeOrClear(buf, buf_len, text);
}

size_t selectPhotosToDelete(const PhotoFile* files, size_t count, uint16_t keep, size_t* out, size_t out_cap) {
  if (files == nullptr || count == 0 || out == nullptr || out_cap == 0) return 0;

  std::vector<size_t> order(count);
  for (size_t i = 0; i < count; ++i) order[i] = i;
  std::sort(order.begin(), order.end(),
            [files](size_t a, size_t b) { return std::strcmp(files[a].name, files[b].name) < 0; });

  size_t nonEmpty = 0;
  for (size_t i : order) {
    if (files[i].size > 0) ++nonEmpty;
  }
  size_t removeNonEmpty = (nonEmpty > keep) ? nonEmpty - keep : 0;

  size_t n = 0;
  for (size_t i : order) {
    if (n >= out_cap) break;
    if (files[i].size == 0) {  // 書込途中の電源断の残骸は枚数に関係なく消す
      out[n++] = i;
    } else if (removeNonEmpty > 0) {  // 古い順に、新しい keep 枚を残す分だけ消す
      out[n++] = i;
      --removeNonEmpty;
    }
  }
  return n;
}

bool shouldSavePhotoToSd(bool camera_enabled, bool save_setting, bool sd_present) {
  return camera_enabled && save_setting && sd_present;
}

bool isPhotoCleanupIdle(bool session_active, bool drawer_open, uint32_t idle_ms, uint32_t idle_hold_ms) {
  return !session_active && !drawer_open && idle_ms >= idle_hold_ms;
}

}  // namespace toolcheck
