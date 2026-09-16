#include "nvs_kv_store.h"

#include <cstring>
#include <vector>

#include "nvs_flash.h"

namespace toolcheck {

NvsKvStore::NvsKvStore(const char* partition) : part_(partition) {}

esp_err_t NvsKvStore::begin() {
  const esp_err_t e = nvs_flash_init_partition(part_);
  ready_ = (e == ESP_OK);
  return e;
}

bool NvsKvStore::ready() const { return ready_; }

const char* NvsKvStore::partition() const { return part_; }

esp_err_t NvsKvStore::eraseAll() {
  nvs_flash_deinit_partition(part_);
  ready_ = false;
  const esp_err_t e = nvs_flash_erase_partition(part_);
  if (e != ESP_OK) return e;
  return begin();
}

bool NvsKvStore::stats(nvs_stats_t* out) const {
  if (out == nullptr) return false;
  std::memset(out, 0, sizeof(*out));
  return nvs_get_stats(part_, out) == ESP_OK;
}

bool NvsKvStore::getBlob(const char* ns, const char* key, uint8_t* out, size_t len) {
  if (!ready_) return false;
  nvs_handle_t h;
  if (nvs_open_from_partition(part_, ns, NVS_READONLY, &h) != ESP_OK) return false;
  size_t size = 0;
  bool ok = nvs_get_blob(h, key, nullptr, &size) == ESP_OK && size == len;
  if (ok) {
    size_t n = len;
    ok = nvs_get_blob(h, key, out, &n) == ESP_OK && n == len;
  }
  nvs_close(h);
  return ok;
}

bool NvsKvStore::putBlob(const char* ns, const char* key, const uint8_t* data, size_t len) {
  if (!ready_) return false;
  nvs_handle_t h;
  if (nvs_open_from_partition(part_, ns, NVS_READWRITE, &h) != ESP_OK) return false;
  const bool ok = nvs_set_blob(h, key, data, len) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

bool NvsKvStore::remove(const char* ns, const char* key) {
  if (!ready_) return false;
  nvs_handle_t h;
  if (nvs_open_from_partition(part_, ns, NVS_READWRITE, &h) != ESP_OK) return false;
  const esp_err_t e = nvs_erase_key(h, key);
  const bool ok = (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

bool NvsKvStore::clearNamespace(const char* ns) {
  if (!ready_) return false;
  nvs_handle_t h;
  if (nvs_open_from_partition(part_, ns, NVS_READWRITE, &h) != ESP_OK) return false;
  const bool ok = nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

bool NvsKvStore::forEachKey(const char* ns, KeyVisitor visit, void* ctx) {
  if (!ready_) return false;
  // 列挙しながら visit を呼ぶと、visit の中で読み書きしたときに列挙が崩れうるので、先にキーを集める
  struct Key {
    char text[NVS_KEY_NAME_MAX_SIZE];
  };
  std::vector<Key> keys;
  nvs_iterator_t it = nullptr;
  esp_err_t e = nvs_entry_find(part_, ns, NVS_TYPE_ANY, &it);
  while (e == ESP_OK) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    Key k;
    std::memcpy(k.text, info.key, sizeof(k.text));
    k.text[sizeof(k.text) - 1] = '\0';
    keys.push_back(k);
    e = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
  if (e != ESP_ERR_NVS_NOT_FOUND) return false;  // 最後まで列挙できると NOT_FOUND で終わる
  for (const Key& k : keys) {
    if (!visit(k.text, ctx)) break;
  }
  return true;
}

}  // namespace toolcheck
