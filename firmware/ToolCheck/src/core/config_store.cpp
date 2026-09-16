#include "config_store.h"

#include <cstdio>
#include <cstring>

namespace toolcheck {
namespace {

const char* const kCfgNs = "cfg";
const char* const kCfgKey = "config";
constexpr uint8_t kCfgRecordVersion = 1;

constexpr uint8_t kFlagCamera = 1 << 0;
constexpr uint8_t kFlagSaveToSd = 1 << 1;
constexpr uint8_t kFlagDevSim = 1 << 2;
constexpr uint8_t kFlagSerialLog = 1 << 3;
// 2026-09-14 追加。「使わない」で持つので、前から保存されている値 (bit4 = 0) は既定どおり「使う」で読める
constexpr uint8_t kFlagNoDrawerSensor = 1 << 4;
// 2026-09-15 追加。前から保存されている値 (bit5 = 0) は既定どおり「数字だけ」で読める
constexpr uint8_t kFlagUserIdAlnum = 1 << 5;

void putU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>(v >> 8);
}

uint16_t getU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

void encode(const Config& c, uint8_t* b) {
  std::memset(b, 0, kConfigRecordSize);
  b[0] = kCfgRecordVersion;
  b[1] = c.brightness_pct;
  b[2] = c.dim_after_min;
  b[3] = c.dim_brightness_pct;
  b[4] = c.off_after_min;
  b[5] = c.volume_operation_pct;
  b[6] = c.volume_warning1_pct;
  b[7] = c.volume_warning2_pct;
  putU16(b + 8, c.warning1_sec);
  putU16(b + 10, c.warning2_sec);
  putU16(b + 12, c.giveup_sec);
  b[14] = c.user_id_digits;
  uint8_t flags = 0;
  if (c.camera_enabled) flags |= kFlagCamera;
  if (c.save_photos_to_sd) flags |= kFlagSaveToSd;
  if (c.dev_sim) flags |= kFlagDevSim;
  if (c.serial_log) flags |= kFlagSerialLog;
  if (!c.drawer_sensor_enabled) flags |= kFlagNoDrawerSensor;
  if (c.user_id_alnum) flags |= kFlagUserIdAlnum;
  b[15] = flags;
  putU16(b + 16, static_cast<uint16_t>(c.tz_offset_min));
  std::memcpy(b + 18, c.pin, 4);
  putU16(b + 22, c.capture_delay_ms);
  putU16(b + 24, c.tof_baseline_mm);
  putU16(b + 26, c.open_delta_mm);
  putU16(b + 28, c.close_delta_mm);
}

void decode(const uint8_t* b, Config* c) {
  c->brightness_pct = b[1];
  c->dim_after_min = b[2];
  c->dim_brightness_pct = b[3];
  c->off_after_min = b[4];
  c->volume_operation_pct = b[5];
  c->volume_warning1_pct = b[6];
  c->volume_warning2_pct = b[7];
  c->warning1_sec = getU16(b + 8);
  c->warning2_sec = getU16(b + 10);
  c->giveup_sec = getU16(b + 12);
  c->user_id_digits = b[14];
  c->camera_enabled = (b[15] & kFlagCamera) != 0;
  c->save_photos_to_sd = (b[15] & kFlagSaveToSd) != 0;
  c->dev_sim = (b[15] & kFlagDevSim) != 0;
  c->serial_log = (b[15] & kFlagSerialLog) != 0;
  c->drawer_sensor_enabled = (b[15] & kFlagNoDrawerSensor) == 0;
  c->user_id_alnum = (b[15] & kFlagUserIdAlnum) != 0;
  c->tz_offset_min = static_cast<int16_t>(getU16(b + 16));
  std::memcpy(c->pin, b + 18, 4);
  c->pin[4] = '\0';
  c->capture_delay_ms = getU16(b + 22);
  c->tof_baseline_mm = getU16(b + 24);
  c->open_delta_mm = getU16(b + 26);
  c->close_delta_mm = getU16(b + 28);
}

struct KeyFinder {
  bool found = false;
};

bool findCfgKey(const char* key, void* ctx) {
  if (std::strcmp(key, kCfgKey) != 0) return true;
  static_cast<KeyFinder*>(ctx)->found = true;
  return false;
}

// 名前で読み書きする設定。型ごとにどれか 1 つのメンバを指す (PIN はどれも指さない)
struct FieldDef {
  const char* name;
  uint8_t Config::*u8;
  uint16_t Config::*u16;
  int16_t Config::*i16;
  bool Config::*flag;
};

constexpr FieldDef kFields[] = {
    {"brightness_pct", &Config::brightness_pct, nullptr, nullptr, nullptr},
    {"dim_after_min", &Config::dim_after_min, nullptr, nullptr, nullptr},
    {"dim_brightness_pct", &Config::dim_brightness_pct, nullptr, nullptr, nullptr},
    {"off_after_min", &Config::off_after_min, nullptr, nullptr, nullptr},
    {"volume_operation_pct", &Config::volume_operation_pct, nullptr, nullptr, nullptr},
    {"volume_warning1_pct", &Config::volume_warning1_pct, nullptr, nullptr, nullptr},
    {"volume_warning2_pct", &Config::volume_warning2_pct, nullptr, nullptr, nullptr},
    {"warning1_sec", nullptr, &Config::warning1_sec, nullptr, nullptr},
    {"warning2_sec", nullptr, &Config::warning2_sec, nullptr, nullptr},
    {"giveup_sec", nullptr, &Config::giveup_sec, nullptr, nullptr},
    {"user_id_digits", &Config::user_id_digits, nullptr, nullptr, nullptr},
    {"tz_offset_min", nullptr, nullptr, &Config::tz_offset_min, nullptr},
    {"pin", nullptr, nullptr, nullptr, nullptr},
    {"camera_enabled", nullptr, nullptr, nullptr, &Config::camera_enabled},
    {"save_photos_to_sd", nullptr, nullptr, nullptr, &Config::save_photos_to_sd},
    {"capture_delay_ms", nullptr, &Config::capture_delay_ms, nullptr, nullptr},
    {"tof_baseline_mm", nullptr, &Config::tof_baseline_mm, nullptr, nullptr},
    {"open_delta_mm", nullptr, &Config::open_delta_mm, nullptr, nullptr},
    {"close_delta_mm", nullptr, &Config::close_delta_mm, nullptr, nullptr},
    {"drawer_sensor_enabled", nullptr, nullptr, nullptr, &Config::drawer_sensor_enabled},
    {"dev_sim", nullptr, nullptr, nullptr, &Config::dev_sim},
    {"serial_log", nullptr, nullptr, nullptr, &Config::serial_log},
    {"user_id_alnum", nullptr, nullptr, nullptr, &Config::user_id_alnum},
};

constexpr size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);

const FieldDef* findField(const char* name) {
  if (name == nullptr) return nullptr;
  for (const FieldDef& f : kFields) {
    if (std::strcmp(f.name, name) == 0) return &f;
  }
  return nullptr;
}

// 10 進の整数を読む (先頭の '-' は lo が負のときだけ)。空・数字以外が混ざる・lo〜hi の外なら false
bool parseInt(const char* text, int32_t lo, int32_t hi, int32_t* out) {
  const char* p = text;
  bool negative = false;
  if (*p == '-') {
    if (lo >= 0) return false;
    negative = true;
    ++p;
  }
  if (*p == '\0') return false;
  int64_t value = 0;
  for (; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') return false;
    value = value * 10 + (*p - '0');
    if (value > 1000000) return false;  // どの型にも収まらない (桁あふれさせない)
  }
  if (negative) value = -value;
  if (value < lo || value > hi) return false;
  *out = static_cast<int32_t>(value);
  return true;
}

}  // namespace

ConfigLoadResult loadConfig(KvStore& kv, Config* out) {
  ConfigLoadResult result;
  Config loaded;
  uint8_t blob[kConfigRecordSize];
  if (!kv.getBlob(kCfgNs, kCfgKey, blob, sizeof(blob))) {
    KeyFinder finder;
    const bool listed = kv.forEachKey(kCfgNs, findCfgKey, &finder);
    result.status = (listed && !finder.found) ? ConfigLoadStatus::NotFound : ConfigLoadStatus::Unreadable;
  } else if (blob[0] != kCfgRecordVersion) {
    result.status = ConfigLoadStatus::UnknownVersion;
  } else {
    decode(blob, &loaded);
    result.error = validateConfig(loaded);
    result.status = (result.error == ConfigError::None) ? ConfigLoadStatus::Loaded : ConfigLoadStatus::Invalid;
  }
  if (out != nullptr) *out = (result.status == ConfigLoadStatus::Loaded) ? loaded : Config{};
  return result;
}

bool saveConfig(KvStore& kv, const Config& cfg, ConfigError* error) {
  const ConfigError e = validateConfig(cfg);
  if (error != nullptr) *error = e;
  if (e != ConfigError::None) return false;
  uint8_t blob[kConfigRecordSize];
  encode(cfg, blob);
  return kv.putBlob(kCfgNs, kCfgKey, blob, sizeof(blob));
}

size_t configFieldCount() { return kFieldCount; }

const char* configFieldName(size_t index) { return (index < kFieldCount) ? kFields[index].name : nullptr; }

bool formatConfigField(const Config& cfg, const char* name, char* buf, size_t buf_len) {
  if (buf == nullptr || buf_len == 0) return false;
  buf[0] = '\0';
  const FieldDef* f = findField(name);
  if (f == nullptr) return false;
  char text[16];
  if (f->u8 != nullptr) {
    std::snprintf(text, sizeof(text), "%u", static_cast<unsigned>(cfg.*(f->u8)));
  } else if (f->u16 != nullptr) {
    std::snprintf(text, sizeof(text), "%u", static_cast<unsigned>(cfg.*(f->u16)));
  } else if (f->i16 != nullptr) {
    std::snprintf(text, sizeof(text), "%d", static_cast<int>(cfg.*(f->i16)));
  } else if (f->flag != nullptr) {
    std::snprintf(text, sizeof(text), "%s", (cfg.*(f->flag)) ? "1" : "0");
  } else {
    std::snprintf(text, sizeof(text), "%.4s", cfg.pin);
  }
  const size_t n = std::strlen(text);
  if (n + 1 > buf_len) return false;
  std::memcpy(buf, text, n + 1);
  return true;
}

ConfigFieldResult setConfigField(Config* cfg, const char* name, const char* value) {
  const FieldDef* f = findField(name);
  if (f == nullptr) return ConfigFieldResult::UnknownField;
  if (cfg == nullptr || value == nullptr) return ConfigFieldResult::BadValue;
  int32_t v = 0;
  if (f->u8 != nullptr) {
    if (!parseInt(value, 0, 255, &v)) return ConfigFieldResult::BadValue;
    cfg->*(f->u8) = static_cast<uint8_t>(v);
  } else if (f->u16 != nullptr) {
    if (!parseInt(value, 0, 65535, &v)) return ConfigFieldResult::BadValue;
    cfg->*(f->u16) = static_cast<uint16_t>(v);
  } else if (f->i16 != nullptr) {
    if (!parseInt(value, -32768, 32767, &v)) return ConfigFieldResult::BadValue;
    cfg->*(f->i16) = static_cast<int16_t>(v);
  } else if (f->flag != nullptr) {
    if (std::strcmp(value, "1") == 0) {
      cfg->*(f->flag) = true;
    } else if (std::strcmp(value, "0") == 0) {
      cfg->*(f->flag) = false;
    } else {
      return ConfigFieldResult::BadValue;
    }
  } else {
    if (std::strlen(value) != 4) return ConfigFieldResult::BadValue;
    std::memcpy(cfg->pin, value, 4);
    cfg->pin[4] = '\0';
  }
  return ConfigFieldResult::Ok;
}

}  // namespace toolcheck
