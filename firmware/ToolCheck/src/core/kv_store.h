// キーと値の保存先の抽象 (実機 = NVS の tooldb パーティション / テスト = メモリ)。
// 名前空間名とキーは NVS と同じく 15 文字まで。値は長さ固定の blob として扱う。
#pragma once

#include <cstddef>
#include <cstdint>

namespace toolcheck {

class KvStore {
 public:
  virtual ~KvStore() = default;

  // 長さがちょうど len の blob を読む。無い・長さが違う・読めないなら false
  virtual bool getBlob(const char* ns, const char* key, uint8_t* out, size_t len) = 0;

  // blob を書く。書けなければ false (電源断や容量不足)
  virtual bool putBlob(const char* ns, const char* key, const uint8_t* data, size_t len) = 0;

  // キーを消す。呼んだ後にキーが無ければ true (もともと無かったときも true)。消せなければ false
  virtual bool remove(const char* ns, const char* key) = 0;

  // 名前空間の中身を全部消す
  virtual bool clearNamespace(const char* ns) = 0;

  // 名前空間のキーを順に visit に渡す。visit が false を返したら止める。列挙できなければ false
  using KeyVisitor = bool (*)(const char* key, void* ctx);
  virtual bool forEachKey(const char* ns, KeyVisitor visit, void* ctx) = 0;
};

}  // namespace toolcheck
