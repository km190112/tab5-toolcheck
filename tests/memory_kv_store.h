// テスト用の KvStore (メモリ)。NVS と同じく名前空間名・キーは 15 文字まで。
// fail_after_puts で「この回数の書き込みが成功した後は、書き込み・削除がすべて失敗する」= 電源断のまねができる。
#pragma once

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/kv_store.h"

class MemoryKvStore : public toolcheck::KvStore {
 public:
  bool getBlob(const char* ns, const char* key, uint8_t* out, size_t len) override {
    auto n = data.find(ns);
    if (n == data.end()) return false;
    auto k = n->second.find(key);
    if (k == n->second.end() || k->second.size() != len) return false;
    if (len > 0) std::memcpy(out, k->second.data(), len);
    return true;
  }

  bool putBlob(const char* ns, const char* key, const uint8_t* src, size_t len) override {
    if (powerLost()) return false;
    if (std::strlen(ns) > 15 || std::strlen(key) > 15) return false;
    data[ns][key] = std::vector<uint8_t>(src, src + len);
    ++puts;
    return true;
  }

  bool remove(const char* ns, const char* key) override {
    if (powerLost() || fail_removes) return false;
    auto n = data.find(ns);
    if (n != data.end()) n->second.erase(key);
    ++removes;
    return true;
  }

  bool clearNamespace(const char* ns) override {
    if (powerLost()) return false;
    data.erase(ns);
    ++clears;
    return true;
  }

  bool forEachKey(const char* ns, KeyVisitor visit, void* ctx) override {
    if (fail_enumeration) return false;
    auto n = data.find(ns);
    if (n == data.end()) return true;
    for (const auto& kv : n->second) {
      if (!visit(kv.first.c_str(), ctx)) break;
    }
    return true;
  }

  bool hasKey(const char* ns, const char* key) const {
    auto n = data.find(ns);
    return n != data.end() && n->second.count(key) > 0;
  }

  // 書き込み・削除・名前空間の消去をした回数の合計 (「読み込みで書いていない」の確認用)
  int writes() const { return puts + removes + clears; }

  int fail_after_puts = -1;       // -1 = 失敗させない
  bool fail_removes = false;      // 削除だけ失敗させる (書き込みは通る)
  bool fail_enumeration = false;  // キーの列挙を失敗させる
  int puts = 0;                   // 成功した書き込みの回数
  int removes = 0;                // 成功した削除の回数
  int clears = 0;                 // 成功した名前空間の消去の回数
  std::map<std::string, std::map<std::string, std::vector<uint8_t>>> data;

 private:
  bool powerLost() const { return fail_after_puts >= 0 && puts >= fail_after_puts; }
};
