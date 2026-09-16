#include "ring_store.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace toolcheck {
namespace {

const char* const kMetaKey = "meta";
const size_t kSeqSize = 4;
const size_t kMetaSize = 8;

void putU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void putU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint16_t getU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t getU32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

// capacity - 1 を十進で書いたときの桁数 (100 → 2 桁、200 → 3 桁)
int digitsFor(uint16_t capacity) {
  unsigned n = (capacity > 0) ? static_cast<unsigned>(capacity - 1) : 0u;
  int digits = 1;
  while (n >= 10) {
    n /= 10;
    ++digits;
  }
  return digits;
}

}  // namespace

RingStore::RingStore(KvStore& kv, const char* ns, char key_prefix, uint16_t capacity, size_t record_size)
    : kv_(kv), ns_(ns), prefix_(key_prefix), capacity_(capacity), record_size_(record_size) {}

bool RingStore::makeKey(char prefix, uint16_t capacity, uint16_t slot, char* buf, size_t buf_len) {
  if (buf == nullptr || buf_len == 0) return false;
  buf[0] = '\0';
  if (capacity == 0 || slot >= capacity) return false;
  const int n = std::snprintf(buf, buf_len, "%c%0*u", prefix, digitsFor(capacity), static_cast<unsigned>(slot));
  if (n < 0 || static_cast<size_t>(n) >= buf_len) {
    buf[0] = '\0';
    return false;
  }
  return true;
}

bool RingStore::load() {
  head_ = 0;
  count_ = 0;
  seq_ = 0;
  if (capacity_ == 0) return false;

  uint8_t meta[kMetaSize];
  if (kv_.getBlob(ns_, kMetaKey, meta, kMetaSize)) {
    const uint16_t head = getU16(meta);
    const uint16_t count = getU16(meta + 2);
    if (head >= capacity_ || count > capacity_) return false;  // 壊れている。0 件として扱う
    head_ = head;
    count_ = count;
    seq_ = getU32(meta + 4);
  }

  // meta を書く前に電源が落ちた追記を数に入れる: 次に書くスロットに「最後の通し番号 + 1」があれば書き終わっていた
  std::vector<uint8_t> slot(kSeqSize + record_size_);
  char key[16];
  for (uint16_t guard = 0; guard < capacity_; ++guard) {
    if (!makeKey(prefix_, capacity_, head_, key, sizeof(key))) break;
    if (!kv_.getBlob(ns_, key, slot.data(), slot.size())) break;
    if (getU32(slot.data()) != seq_ + 1) break;
    head_ = static_cast<uint16_t>((head_ + 1) % capacity_);
    if (count_ < capacity_) ++count_;
    ++seq_;
  }
  return true;
}

bool RingStore::append(const uint8_t* record) {
  if (capacity_ == 0 || record == nullptr) return false;
  char key[16];
  if (!makeKey(prefix_, capacity_, head_, key, sizeof(key))) return false;

  // ① スロットを書く
  const uint32_t nextSeq = seq_ + 1;
  std::vector<uint8_t> slot(kSeqSize + record_size_);
  putU32(slot.data(), nextSeq);
  std::memcpy(slot.data() + kSeqSize, record, record_size_);
  if (!kv_.putBlob(ns_, key, slot.data(), slot.size())) return false;

  // ② meta を書く (ここで落ちても、次の load() が①から書き終わっていたと判断する)
  const uint16_t nextHead = static_cast<uint16_t>((head_ + 1) % capacity_);
  const uint16_t nextCount = (count_ < capacity_) ? static_cast<uint16_t>(count_ + 1) : count_;
  uint8_t meta[kMetaSize];
  putU16(meta, nextHead);
  putU16(meta + 2, nextCount);
  putU32(meta + 4, nextSeq);
  if (!kv_.putBlob(ns_, kMetaKey, meta, kMetaSize)) return false;

  head_ = nextHead;
  count_ = nextCount;
  seq_ = nextSeq;
  return true;
}

uint16_t RingStore::count() const { return count_; }

uint16_t RingStore::capacity() const { return capacity_; }

bool RingStore::getNewest(uint16_t index, uint8_t* out) const {
  if (out == nullptr || capacity_ == 0 || index >= count_) return false;
  const uint16_t slotIndex = static_cast<uint16_t>((head_ + capacity_ - 1 - index) % capacity_);
  char key[16];
  if (!makeKey(prefix_, capacity_, slotIndex, key, sizeof(key))) return false;
  std::vector<uint8_t> slot(kSeqSize + record_size_);
  if (!kv_.getBlob(ns_, key, slot.data(), slot.size())) return false;
  std::memcpy(out, slot.data() + kSeqSize, record_size_);
  return true;
}

bool RingStore::clear() {
  const bool ok = kv_.clearNamespace(ns_);
  head_ = 0;
  count_ = 0;
  seq_ = 0;
  return ok;
}

}  // namespace toolcheck
