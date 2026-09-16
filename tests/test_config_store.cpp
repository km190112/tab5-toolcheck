// config_store: 設定値の保存と、USB シリアルから設定値を名前で読み書きする (docs/設計.md「永続化 (NVS)」「設定画面」)
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "core/config_store.h"
#include "memory_kv_store.h"
#include "testing.h"

using toolcheck::Config;
using toolcheck::ConfigError;
using toolcheck::ConfigFieldResult;
using toolcheck::configFieldCount;
using toolcheck::configFieldName;
using toolcheck::ConfigLoadResult;
using toolcheck::ConfigLoadStatus;
using toolcheck::formatConfigField;
using toolcheck::kConfigRecordSize;
using toolcheck::loadConfig;
using toolcheck::saveConfig;
using toolcheck::setConfigField;

namespace {

// 既定値と全部違う、制約を守った設定 (欄の取り違えを見つける)
Config nonDefault() {
  Config c;
  c.brightness_pct = 80;
  c.dim_after_min = 10;
  c.dim_brightness_pct = 20;
  c.off_after_min = 30;
  c.volume_operation_pct = 30;  // 既定値 (0) と違う値
  c.volume_warning1_pct = 45;
  c.volume_warning2_pct = 90;
  c.warning1_sec = 50;
  c.warning2_sec = 70;
  c.giveup_sec = 300;
  c.user_id_digits = 8;
  c.user_id_alnum = true;
  c.tz_offset_min = -300;
  std::memcpy(c.pin, "4821", 5);
  c.camera_enabled = true;  // 既定値 (2026-09-14 夕から OFF) と違う値
  c.save_photos_to_sd = false;
  c.capture_delay_ms = 1500;
  c.tof_baseline_mm = 812;
  c.open_delta_mm = 150;
  c.close_delta_mm = 60;
  c.drawer_sensor_enabled = false;
  c.dev_sim = true;
  c.serial_log = true;
  return c;
}

void checkSame(const Config& a, const Config& b) {
  CHECK_EQ(a.brightness_pct, b.brightness_pct);
  CHECK_EQ(a.dim_after_min, b.dim_after_min);
  CHECK_EQ(a.dim_brightness_pct, b.dim_brightness_pct);
  CHECK_EQ(a.off_after_min, b.off_after_min);
  CHECK_EQ(a.volume_operation_pct, b.volume_operation_pct);
  CHECK_EQ(a.volume_warning1_pct, b.volume_warning1_pct);
  CHECK_EQ(a.volume_warning2_pct, b.volume_warning2_pct);
  CHECK_EQ(a.warning1_sec, b.warning1_sec);
  CHECK_EQ(a.warning2_sec, b.warning2_sec);
  CHECK_EQ(a.giveup_sec, b.giveup_sec);
  CHECK_EQ(a.user_id_digits, b.user_id_digits);
  CHECK_EQ(a.user_id_alnum, b.user_id_alnum);
  CHECK_EQ(a.tz_offset_min, b.tz_offset_min);
  CHECK_EQ(std::string(a.pin), std::string(b.pin));
  CHECK_EQ(a.camera_enabled, b.camera_enabled);
  CHECK_EQ(a.save_photos_to_sd, b.save_photos_to_sd);
  CHECK_EQ(a.capture_delay_ms, b.capture_delay_ms);
  CHECK_EQ(a.tof_baseline_mm, b.tof_baseline_mm);
  CHECK_EQ(a.open_delta_mm, b.open_delta_mm);
  CHECK_EQ(a.close_delta_mm, b.close_delta_mm);
  CHECK_EQ(a.drawer_sensor_enabled, b.drawer_sensor_enabled);
  CHECK_EQ(a.dev_sim, b.dev_sim);
  CHECK_EQ(a.serial_log, b.serial_log);
}

std::vector<uint8_t>& storedBlob(MemoryKvStore& kv) { return kv.data["cfg"]["config"]; }

std::string get(const Config& c, const char* name) {
  char buf[32];
  if (!formatConfigField(c, name, buf, sizeof(buf))) return "<false>";
  return std::string(buf);
}

}  // namespace

// --- 保存と読み込み ---

TEST(config_store_not_found_uses_defaults_without_writing) {
  MemoryKvStore kv;
  Config c = nonDefault();
  const ConfigLoadResult r = loadConfig(kv, &c);
  CHECK_EQ(r.status, ConfigLoadStatus::NotFound);
  CHECK_EQ(r.error, ConfigError::None);
  checkSame(c, Config{});
  CHECK_EQ(kv.writes(), 0);
}

TEST(config_store_round_trip_all_fields) {
  MemoryKvStore kv;
  ConfigError e = ConfigError::PinNotFourDigits;
  CHECK(saveConfig(kv, nonDefault(), &e));
  CHECK_EQ(e, ConfigError::None);
  CHECK_EQ(storedBlob(kv).size(), kConfigRecordSize);

  Config loaded;
  const int before = kv.writes();
  const ConfigLoadResult r = loadConfig(kv, &loaded);
  CHECK_EQ(r.status, ConfigLoadStatus::Loaded);
  CHECK_EQ(r.error, ConfigError::None);
  checkSame(loaded, nonDefault());
  CHECK_EQ(kv.writes(), before);
}

TEST(config_store_save_accepts_null_error) {
  MemoryKvStore kv;
  CHECK(saveConfig(kv, Config{}, nullptr));
  CHECK(kv.hasKey("cfg", "config"));
}

TEST(config_store_save_rejects_invalid_without_writing) {
  MemoryKvStore kv;
  Config c = nonDefault();
  c.brightness_pct = 9;
  ConfigError e = ConfigError::None;
  CHECK(!saveConfig(kv, c, &e));
  CHECK_EQ(e, ConfigError::BrightnessOutOfRange);
  CHECK_EQ(kv.writes(), 0);
  CHECK(!kv.hasKey("cfg", "config"));
}

TEST(config_store_save_reports_storage_failure) {
  MemoryKvStore kv;
  kv.fail_after_puts = 0;
  ConfigError e = ConfigError::PinNotFourDigits;
  CHECK(!saveConfig(kv, nonDefault(), &e));
  CHECK_EQ(e, ConfigError::None);
  CHECK(!kv.hasKey("cfg", "config"));
}

TEST(config_store_overwrites_previous_value) {
  MemoryKvStore kv;
  CHECK(saveConfig(kv, nonDefault(), nullptr));
  Config second = nonDefault();
  second.brightness_pct = 100;
  CHECK(saveConfig(kv, second, nullptr));
  Config loaded;
  CHECK_EQ(loadConfig(kv, &loaded).status, ConfigLoadStatus::Loaded);
  CHECK_EQ(loaded.brightness_pct, uint8_t{100});
}

TEST(config_store_wrong_length_is_unreadable_and_kept) {
  MemoryKvStore kv;
  storedBlob(kv) = std::vector<uint8_t>(32, 0x11);  // spike が残した 32 バイトの blob のような値
  Config c = nonDefault();
  const ConfigLoadResult r = loadConfig(kv, &c);
  CHECK_EQ(r.status, ConfigLoadStatus::Unreadable);
  checkSame(c, Config{});
  CHECK_EQ(kv.writes(), 0);
  CHECK_EQ(storedBlob(kv).size(), size_t{32});
}

TEST(config_store_enumeration_failure_is_unreadable) {
  MemoryKvStore kv;
  kv.fail_enumeration = true;
  Config c = nonDefault();
  CHECK_EQ(loadConfig(kv, &c).status, ConfigLoadStatus::Unreadable);
  checkSame(c, Config{});
}

TEST(config_store_unknown_version_uses_defaults_and_keeps_value) {
  MemoryKvStore kv;
  CHECK(saveConfig(kv, nonDefault(), nullptr));
  CHECK_EQ(storedBlob(kv).size(), kConfigRecordSize);
  if (storedBlob(kv).size() != kConfigRecordSize) return;
  storedBlob(kv)[0] = 2;
  const int before = kv.writes();
  Config c = nonDefault();
  const ConfigLoadResult r = loadConfig(kv, &c);
  CHECK_EQ(r.status, ConfigLoadStatus::UnknownVersion);
  checkSame(c, Config{});
  CHECK_EQ(kv.writes(), before);
  CHECK_EQ(storedBlob(kv)[0], uint8_t{2});
}

TEST(config_store_out_of_range_value_is_invalid) {
  MemoryKvStore kv;
  CHECK(saveConfig(kv, nonDefault(), nullptr));
  CHECK_EQ(storedBlob(kv).size(), kConfigRecordSize);
  if (storedBlob(kv).size() != kConfigRecordSize) return;
  storedBlob(kv)[1] = 5;  // 通常の明るさ 5%
  Config c = nonDefault();
  const ConfigLoadResult r = loadConfig(kv, &c);
  CHECK_EQ(r.status, ConfigLoadStatus::Invalid);
  CHECK_EQ(r.error, ConfigError::BrightnessOutOfRange);
  checkSame(c, Config{});
}

TEST(config_store_broken_pin_is_invalid) {
  MemoryKvStore kv;
  CHECK(saveConfig(kv, nonDefault(), nullptr));
  CHECK_EQ(storedBlob(kv).size(), kConfigRecordSize);
  if (storedBlob(kv).size() != kConfigRecordSize) return;
  storedBlob(kv)[18] = 'x';
  Config c;
  const ConfigLoadResult r = loadConfig(kv, &c);
  CHECK_EQ(r.status, ConfigLoadStatus::Invalid);
  CHECK_EQ(r.error, ConfigError::PinNotFourDigits);
  checkSame(c, Config{});
}

TEST(config_store_layout_version1) {
  MemoryKvStore kv;
  CHECK(saveConfig(kv, nonDefault(), nullptr));
  const std::vector<uint8_t> expected = {
      1,                       // [0] 版数
      80, 10, 20, 30,          // [1] 明るさ・減光まで・減光時・消灯まで
      30, 45, 90,              // [5] 操作音・警告 1・警告 2
      50, 0, 70, 0, 0x2C, 1,   // [8] 警告 1・警告 2・打ち切り (u16)
      8,                       // [14] 利用者 ID の桁数
      0x3D,                    // [15] 入切: カメラ ON・SD 保存 OFF・sim ON・動作ログ ON・引き出しセンサを使わない (bit4)・利用者 ID に英字・記号も (bit5)
      0xD4, 0xFE,              // [16] 時差 -300 (i16)
      '4', '8', '2', '1',      // [18] PIN
      0xDC, 0x05,              // [22] 撮影までの時間 1500
      0x2C, 0x03,              // [24] ToF の基準 812
      0x96, 0x00, 0x3C, 0x00,  // [26] 開の閾値 150・閉の閾値 60
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // [30] 予備
  };
  CHECK_EQ(expected.size(), kConfigRecordSize);
  const std::vector<uint8_t>& blob = storedBlob(kv);
  CHECK_EQ(blob.size(), expected.size());
  for (size_t i = 0; i < blob.size() && i < expected.size(); ++i) {
    if (blob[i] != expected[i]) {
      tt::fail(__FILE__, __LINE__, "byte " + std::to_string(i) + " : " + std::to_string(blob[i]) +
                                       " != " + std::to_string(expected[i]));
    }
  }
}

// 引き出しセンサの入切 (2026-09-14 追加) は予備だった [15] の bit4 に「使わない」で持つ。
// 前から保存されている値 (bit4 = 0) は、既定どおり「使う」で読む (版数は上げない)
TEST(config_store_record_without_drawer_flag_uses_drawer_sensor) {
  MemoryKvStore kv;
  CHECK(saveConfig(kv, nonDefault(), nullptr));
  CHECK_EQ(storedBlob(kv).size(), kConfigRecordSize);
  if (storedBlob(kv).size() != kConfigRecordSize) return;
  storedBlob(kv)[15] = static_cast<uint8_t>(storedBlob(kv)[15] & ~0x10);
  Config c;
  const ConfigLoadResult r = loadConfig(kv, &c);
  CHECK_EQ(r.status, ConfigLoadStatus::Loaded);
  CHECK(c.drawer_sensor_enabled);
  CHECK(c.camera_enabled);  // ほかの入切はそのまま
  CHECK(c.dev_sim);
}

// 利用者 ID の形 (2026-09-15 追加) は [15] の bit5 に「英字・記号も使う」で持つ。
// 前から保存されている値 (bit5 = 0) は、既定どおり数字だけで読む (版数は上げない)
TEST(config_store_record_without_alnum_flag_uses_digits_only) {
  MemoryKvStore kv;
  CHECK(saveConfig(kv, nonDefault(), nullptr));
  CHECK_EQ(storedBlob(kv).size(), kConfigRecordSize);
  if (storedBlob(kv).size() != kConfigRecordSize) return;
  CHECK_EQ(storedBlob(kv)[15] & 0x20, 0x20);
  storedBlob(kv)[15] = static_cast<uint8_t>(storedBlob(kv)[15] & ~0x20);
  Config c;
  const ConfigLoadResult r = loadConfig(kv, &c);
  CHECK_EQ(r.status, ConfigLoadStatus::Loaded);
  CHECK(!c.user_id_alnum);
  CHECK_EQ(c.user_id_digits, uint8_t{8});
  CHECK(!c.drawer_sensor_enabled);  // ほかの入切はそのまま
}

TEST(config_store_default_flags_keep_drawer_bit_clear) {
  MemoryKvStore kv;
  CHECK(saveConfig(kv, Config{}, nullptr));
  CHECK_EQ(storedBlob(kv).size(), kConfigRecordSize);
  if (storedBlob(kv).size() != kConfigRecordSize) return;
  CHECK_EQ(storedBlob(kv)[15], uint8_t{0x02});  // カメラ OFF・SD 保存 ON。既定 (センサを使う) では bit4 は 0 のまま
}

// --- 名前での読み書き ---

TEST(config_field_names_match_config_members) {
  const std::vector<std::string> expected = {
      "brightness_pct",    "dim_after_min",     "dim_brightness_pct", "off_after_min",
      "volume_operation_pct", "volume_warning1_pct", "volume_warning2_pct", "warning1_sec",
      "warning2_sec",      "giveup_sec",        "user_id_digits",     "tz_offset_min",
      "pin",               "camera_enabled",    "save_photos_to_sd",  "capture_delay_ms",
      "tof_baseline_mm",   "open_delta_mm",     "close_delta_mm",     "dev_sim",
      "serial_log",        "drawer_sensor_enabled", "user_id_alnum",
  };
  CHECK_EQ(configFieldCount(), expected.size());
  std::set<std::string> names;
  for (size_t i = 0; i < configFieldCount(); ++i) {
    const char* name = configFieldName(i);
    CHECK(name != nullptr);
    if (name != nullptr) names.insert(name);
  }
  CHECK_EQ(names.size(), expected.size());
  for (const std::string& n : expected) CHECK(names.count(n) == 1);
  CHECK(configFieldName(configFieldCount()) == nullptr);
}

TEST(config_field_format_defaults) {
  const Config c;
  CHECK_EQ(get(c, "brightness_pct"), std::string("60"));
  CHECK_EQ(get(c, "giveup_sec"), std::string("120"));
  CHECK_EQ(get(c, "tz_offset_min"), std::string("540"));
  CHECK_EQ(get(c, "pin"), std::string("0000"));
  CHECK_EQ(get(c, "camera_enabled"), std::string("0"));  // 2026-09-14 夕: カメラの既定はオフ
  CHECK_EQ(get(c, "dev_sim"), std::string("0"));
  CHECK_EQ(get(c, "tof_baseline_mm"), std::string("0"));
  CHECK_EQ(get(c, "drawer_sensor_enabled"), std::string("1"));
  CHECK_EQ(get(c, "user_id_alnum"), std::string("0"));
}

TEST(config_field_format_negative_timezone) { CHECK_EQ(get(nonDefault(), "tz_offset_min"), std::string("-300")); }

TEST(config_field_round_trip_every_field_by_name) {
  const Config source = nonDefault();
  Config target;
  for (size_t i = 0; i < configFieldCount(); ++i) {
    const char* name = configFieldName(i);
    const std::string value = get(source, name);
    CHECK_EQ(setConfigField(&target, name, value.c_str()), ConfigFieldResult::Ok);
  }
  CHECK(configFieldCount() > 0);
  checkSame(target, source);
}

TEST(config_field_set_numbers) {
  Config c;
  CHECK_EQ(setConfigField(&c, "brightness_pct", "80"), ConfigFieldResult::Ok);
  CHECK_EQ(c.brightness_pct, uint8_t{80});
  CHECK_EQ(setConfigField(&c, "warning2_sec", "900"), ConfigFieldResult::Ok);
  CHECK_EQ(c.warning2_sec, uint16_t{900});
  CHECK_EQ(setConfigField(&c, "tz_offset_min", "-720"), ConfigFieldResult::Ok);
  CHECK_EQ(c.tz_offset_min, int16_t{-720});
}

TEST(config_field_set_does_not_check_constraints) {
  Config c;
  CHECK_EQ(setConfigField(&c, "brightness_pct", "5"), ConfigFieldResult::Ok);  // 制約は applyConfig で見る
  CHECK_EQ(c.brightness_pct, uint8_t{5});
}

TEST(config_field_set_bools) {
  Config c;
  CHECK_EQ(setConfigField(&c, "dev_sim", "1"), ConfigFieldResult::Ok);
  CHECK(c.dev_sim);
  CHECK_EQ(setConfigField(&c, "dev_sim", "0"), ConfigFieldResult::Ok);
  CHECK(!c.dev_sim);
  CHECK_EQ(setConfigField(&c, "camera_enabled", "2"), ConfigFieldResult::BadValue);
  CHECK_EQ(setConfigField(&c, "camera_enabled", "true"), ConfigFieldResult::BadValue);
  CHECK(!c.camera_enabled);  // 読めない値では既定 (OFF) のまま
  CHECK_EQ(setConfigField(&c, "drawer_sensor_enabled", "0"), ConfigFieldResult::Ok);
  CHECK(!c.drawer_sensor_enabled);
  CHECK_EQ(setConfigField(&c, "user_id_alnum", "1"), ConfigFieldResult::Ok);
  CHECK(c.user_id_alnum);
}

TEST(config_field_set_pin_needs_four_characters) {
  Config c;
  CHECK_EQ(setConfigField(&c, "pin", "1234"), ConfigFieldResult::Ok);
  CHECK_EQ(std::string(c.pin), std::string("1234"));
  CHECK_EQ(setConfigField(&c, "pin", "123"), ConfigFieldResult::BadValue);
  CHECK_EQ(setConfigField(&c, "pin", "12345"), ConfigFieldResult::BadValue);
  CHECK_EQ(std::string(c.pin), std::string("1234"));
}

TEST(config_field_set_rejects_values_that_do_not_fit) {
  const Config before;
  Config c;
  CHECK_EQ(setConfigField(&c, "brightness_pct", ""), ConfigFieldResult::BadValue);
  CHECK_EQ(setConfigField(&c, "brightness_pct", "12a"), ConfigFieldResult::BadValue);
  CHECK_EQ(setConfigField(&c, "brightness_pct", " 12"), ConfigFieldResult::BadValue);
  CHECK_EQ(setConfigField(&c, "brightness_pct", "+12"), ConfigFieldResult::BadValue);
  CHECK_EQ(setConfigField(&c, "brightness_pct", "-1"), ConfigFieldResult::BadValue);
  CHECK_EQ(setConfigField(&c, "brightness_pct", "256"), ConfigFieldResult::BadValue);
  CHECK_EQ(setConfigField(&c, "giveup_sec", "65536"), ConfigFieldResult::BadValue);
  CHECK_EQ(setConfigField(&c, "giveup_sec", "99999999999999999999"), ConfigFieldResult::BadValue);
  CHECK_EQ(setConfigField(&c, "tz_offset_min", "32768"), ConfigFieldResult::BadValue);
  CHECK_EQ(setConfigField(&c, "tz_offset_min", "-32769"), ConfigFieldResult::BadValue);
  CHECK_EQ(setConfigField(&c, "tz_offset_min", "-"), ConfigFieldResult::BadValue);
  CHECK_EQ(setConfigField(&c, "brightness_pct", nullptr), ConfigFieldResult::BadValue);
  checkSame(c, before);
}

TEST(config_field_unknown_name) {
  Config c;
  CHECK_EQ(setConfigField(&c, "light_sleep", "1"), ConfigFieldResult::UnknownField);
  CHECK_EQ(setConfigField(&c, nullptr, "1"), ConfigFieldResult::UnknownField);
  char buf[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
  CHECK(!formatConfigField(c, "light_sleep", buf, sizeof(buf)));
  CHECK_EQ(std::string(buf), std::string(""));
  checkSame(c, Config{});
}

TEST(config_field_format_small_buffer_fails) {
  char buf[3] = {'x', 'x', 'x'};
  CHECK(!formatConfigField(Config{}, "tz_offset_min", buf, sizeof(buf)));  // "540" + NUL は 4 バイト要る
  CHECK_EQ(std::string(buf), std::string(""));
}
