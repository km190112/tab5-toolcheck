// 写真のファイル名・古い写真の選び方・SD 保存の実効状態 (docs/設計.md「写真」)。
// Arduino 非依存。SD の読み書きそのものは hw 層 (sd_card) が行う。
#pragma once

#include <cstddef>
#include <cstdint>

namespace toolcheck {

// SD に残す写真の枚数
constexpr uint16_t kPhotoKeepCount = 100;

// 現地時刻で "YYYYMMDD-hhmmss.jpg"。同じ秒の 2 枚目以降は seq を付けて "YYYYMMDD-hhmmss-2.jpg"。
// 時刻が無効なら "00000000-<起動からの秒を 10 桁>.jpg" (名前順で最古になり、先に消える)。
// seq は 1 始まりで、1 なら付けない。buf に収まらなければ false を返し、buf は空文字列にする
bool makePhotoName(int64_t epoch, int32_t tz_offset_min, uint32_t uptime_s, uint8_t seq, char* buf, size_t buf_len);

struct PhotoFile {
  const char* name;  // ファイル名 (ディレクトリを含まない)
  uint32_t size;     // バイト数
};

// 消す写真を選ぶ。名前の昇順を古い順とみなし、中身のある写真は新しい keep 枚を残す。
// 0 バイトの写真 (書込途中の電源断の残骸) は枚数に関係なく消す。
// 消す写真の files の添字を名前の昇順で out に入れ、入れた数を返す (out_cap を超える分は入れない)
size_t selectPhotosToDelete(const PhotoFile* files, size_t count, uint16_t keep, size_t* out, size_t out_cap);

// SD に保存するか (実効状態): カメラを使う かつ 設定が ON かつ SD がある
bool shouldSavePhotoToSd(bool camera_enabled, bool save_setting, bool sd_present);

// 古い写真を消してよいアイドル状態か: セッション無し かつ 引き出しが閉 かつ 無操作が idle_hold_ms 以上
bool isPhotoCleanupIdle(bool session_active, bool drawer_open, uint32_t idle_ms, uint32_t idle_hold_ms);

}  // namespace toolcheck
