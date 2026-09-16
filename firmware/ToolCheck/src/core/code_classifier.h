// QR / バーコードの中身を「利用者 ID」「物品番号」「不正」に分ける。
// Arduino 非依存 (ホストの MSVC と ESP32 の gcc の両方で通す)。
#pragma once

#include <cstddef>
#include <cstdint>

namespace toolcheck {

enum class CodeKind : uint8_t {
  User,     // 利用者 ID (名札)
  Item,     // 物品番号
  Invalid,  // どちらでもない
};

enum class InvalidReason : uint8_t {
  None,           // 有効
  Empty,          // 前後の空白を除くと何も残らない
  TooShort,       // 物品番号として短すぎる
  TooLong,        // 物品番号として長すぎる
  ForbiddenChar,  // 印字可能 ASCII 以外・空白・カンマを含む
};

// NVS のキーの上限。物品番号はそのままキーにするので、設定でもこれを超えられない
constexpr uint8_t kItemMaxLenLimit = 15;

struct ClassifierConfig {
  uint8_t user_id_digits = 7;                 // 利用者 ID の桁数 (英字・記号も使うときは文字数)。0 か 15 超なら利用者 ID は無し
  bool    user_id_alnum  = false;             // false = 数字だけ / true = 物品番号と同じ文字も使う (その文字数の物品番号は利用者 ID になる)
  uint8_t item_min_len   = 2;                 // 物品番号の最短
  uint8_t item_max_len   = kItemMaxLenLimit;  // 物品番号の最長 (15 を超える値は 15 とみなす)
};

struct ClassifiedCode {
  CodeKind      kind   = CodeKind::Invalid;
  InvalidReason reason = InvalidReason::Empty;
  size_t        length = 0;                        // 前後の空白を除いた長さ
  char          text[kItemMaxLenLimit + 1] = {0};  // 前後の空白を除いた中身 (15 文字で切る。NUL 終端)
};

// raw は QR ユニットから読んだバイト列。NUL 終端でなくてよい (raw_len だけ見る)
ClassifiedCode classifyCode(const char* raw, size_t raw_len, const ClassifierConfig& cfg);

}  // namespace toolcheck
