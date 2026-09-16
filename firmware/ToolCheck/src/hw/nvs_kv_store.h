// KvStore を ESP-IDF の NVS (パーティション tooldb) で実装する (docs/設計.md「永続化 (NVS)」)。
// 読むだけの操作は名前空間を読み取り専用で開く (読み書きで開くと、無い名前空間を作る書き込みになる。起動直後に NVS へ書かない)。
// 開けないパーティションを自動で消さない。消すのは eraseAll() を呼んだときだけ (確認は呼び出し側)。
#pragma once

#include <cstddef>

#include "esp_err.h"
#include "nvs.h"
#include "../core/kv_store.h"

namespace toolcheck {

class NvsKvStore : public KvStore {
 public:
  explicit NvsKvStore(const char* partition);

  // パーティションを開く。ESP_OK 以外なら ready() は false のまま (「記録領域を読めません」)
  esp_err_t begin();
  bool ready() const;
  const char* partition() const;

  // パーティションごと消して開き直す (全削除)
  esp_err_t eraseAll();

  bool stats(nvs_stats_t* out) const;

  bool getBlob(const char* ns, const char* key, uint8_t* out, size_t len) override;
  bool putBlob(const char* ns, const char* key, const uint8_t* data, size_t len) override;
  bool remove(const char* ns, const char* key) override;
  bool clearNamespace(const char* ns) override;
  bool forEachKey(const char* ns, KeyVisitor visit, void* ctx) override;

 private:
  const char* part_;
  bool ready_ = false;
};

}  // namespace toolcheck
