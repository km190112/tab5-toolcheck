// 設定値の保存 (docs/設計.md「永続化 (NVS)」) と、USB シリアルの cfg get / set 用に設定値を名前で読み書きする。
// Arduino 非依存。値の範囲と相互の制約は config.h の validateConfig が決める。
//
// 保存の形 (版数 1、kConfigRecordSize バイト、数値はリトルエンディアン):
//   名前空間 "cfg"、キー "config"
//   [0] 版数            [1] brightness_pct       [2] dim_after_min        [3] dim_brightness_pct
//   [4] off_after_min   [5] volume_operation_pct [6] volume_warning1_pct  [7] volume_warning2_pct
//   [8] warning1_sec u16  [10] warning2_sec u16  [12] giveup_sec u16      [14] user_id_digits
//   [15] 入切 (bit0 camera_enabled / bit1 save_photos_to_sd / bit2 dev_sim / bit3 serial_log /
//        bit4 引き出しセンサを「使わない」= !drawer_sensor_enabled。2026-09-14 に予備から使い始めた。前からの値は 0 =「使う」/
//        bit5 利用者 ID に英字・記号も使う = user_id_alnum。2026-09-15 に予備から使い始めた。前からの値は 0 =「数字だけ」)
//   [16] tz_offset_min i16  [18] pin 4 文字 (NUL なし)  [22] capture_delay_ms u16
//   [24] tof_baseline_mm u16  [26] open_delta_mm u16  [28] close_delta_mm u16  [30〜39] 予備 (0)
//
// 読み込みは書かない。読めない・版数を知らない・値が制約に反するときは既定値を使って理由を返し、
// 保存されている値は消さない (次に保存したときに上書きされる)。記録の削除では消さない。
#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"
#include "kv_store.h"

namespace toolcheck {

constexpr size_t kConfigRecordSize = 40;  // 版数 1

enum class ConfigLoadStatus : uint8_t {
  Loaded,          // 保存されていた値を使う
  NotFound,        // 保存されていない (既定値を使う)
  Unreadable,      // キーはあるが読めない・長さが違う・列挙できない (既定値を使う)
  UnknownVersion,  // 知らない版数 (既定値を使う)
  Invalid,         // 値が制約に反する (既定値を使う。error に最初の違反)
};

struct ConfigLoadResult {
  ConfigLoadStatus status = ConfigLoadStatus::NotFound;
  ConfigError error = ConfigError::None;
};

// 保存先から読む。Loaded 以外なら out を既定値にする。KvStore へは書かない
ConfigLoadResult loadConfig(KvStore& kv, Config* out);

// 制約を確かめてから書く。違反なら書かずに false (error に理由)。書けなければ false (error は None)
bool saveConfig(KvStore& kv, const Config& cfg, ConfigError* error);

// --- 名前での読み書き (USB シリアル) ---

enum class ConfigFieldResult : uint8_t {
  Ok,
  UnknownField,  // その名前の設定は無い
  BadValue,      // 型に収まらない・数字以外が混ざる・入切が 0 / 1 以外・PIN が 4 文字でない
};

// 設定の数と、index 番目の名前 (範囲外は nullptr)。名前は Config のメンバ名と同じ
size_t configFieldCount();
const char* configFieldName(size_t index);

// 値を文字列にする (数値は 10 進、入切は 0 / 1、PIN は 4 文字)。名前が無い・buf に収まらなければ false (buf は空文字列)
bool formatConfigField(const Config& cfg, const char* name, char* buf, size_t buf_len);

// 文字列を読んで cfg に入れる。範囲や相互の制約は見ない (applyConfig で確かめる)。Ok 以外なら cfg は変えない
ConfigFieldResult setConfigField(Config* cfg, const char* name, const char* value);

}  // namespace toolcheck
