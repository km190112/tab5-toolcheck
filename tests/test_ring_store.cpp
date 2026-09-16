// ring_store: 長さ固定のレコードの回し書きと電源断の整合 (docs/設計.md「永続化 (NVS)」)
#include <cstring>
#include <string>
#include <vector>

#include "core/ring_store.h"
#include "memory_kv_store.h"
#include "testing.h"

using toolcheck::RingStore;

namespace {

const size_t kRecSize = 12;

std::vector<uint8_t> rec(const char* text) {
  std::vector<uint8_t> r(kRecSize, 0);
  for (size_t i = 0; i + 1 < kRecSize && text[i] != '\0'; ++i) r[i] = static_cast<uint8_t>(text[i]);
  return r;
}

std::string newest(const RingStore& rs, uint16_t index) {
  std::vector<uint8_t> out(kRecSize, 0);
  if (!rs.getNewest(index, out.data())) return "<none>";
  return std::string(reinterpret_cast<char*>(out.data()));
}

bool add(RingStore& rs, const char* text) {
  auto r = rec(text);
  return rs.append(r.data());
}

}  // namespace

TEST(ring_empty_store_loads_empty) {
  MemoryKvStore kv;
  RingStore rs(kv, "hist", 'H', 100, kRecSize);
  CHECK(rs.load());
  CHECK_EQ(rs.count(), uint16_t{0});
  CHECK_EQ(newest(rs, 0), std::string("<none>"));
}

TEST(ring_reads_newest_first) {
  MemoryKvStore kv;
  RingStore rs(kv, "hist", 'H', 100, kRecSize);
  rs.load();
  CHECK(add(rs, "a"));
  CHECK(add(rs, "b"));
  CHECK(add(rs, "c"));
  CHECK_EQ(rs.count(), uint16_t{3});
  CHECK_EQ(newest(rs, 0), std::string("c"));
  CHECK_EQ(newest(rs, 1), std::string("b"));
  CHECK_EQ(newest(rs, 2), std::string("a"));
  CHECK_EQ(newest(rs, 3), std::string("<none>"));
}

TEST(ring_wraps_at_capacity) {
  MemoryKvStore kv;
  RingStore rs(kv, "hist", 'H', 5, kRecSize);
  rs.load();
  const char* names[] = {"r1", "r2", "r3", "r4", "r5", "r6", "r7"};
  for (const char* n : names) CHECK(add(rs, n));
  CHECK_EQ(rs.count(), uint16_t{5});
  CHECK_EQ(newest(rs, 0), std::string("r7"));
  CHECK_EQ(newest(rs, 4), std::string("r3"));
  CHECK_EQ(newest(rs, 5), std::string("<none>"));
}

TEST(ring_persists_across_instances) {
  MemoryKvStore kv;
  {
    RingStore rs(kv, "hist", 'H', 100, kRecSize);
    rs.load();
    add(rs, "a");
    add(rs, "b");
    add(rs, "c");
  }
  RingStore again(kv, "hist", 'H', 100, kRecSize);
  CHECK(again.load());
  CHECK_EQ(again.count(), uint16_t{3});
  CHECK_EQ(newest(again, 0), std::string("c"));
  CHECK_EQ(newest(again, 2), std::string("a"));
}

TEST(ring_wrapped_state_persists) {
  MemoryKvStore kv;
  {
    RingStore rs(kv, "hist", 'H', 3, kRecSize);
    rs.load();
    for (const char* n : {"r1", "r2", "r3", "r4"}) add(rs, n);
  }
  RingStore again(kv, "hist", 'H', 3, kRecSize);
  CHECK(again.load());
  CHECK_EQ(again.count(), uint16_t{3});
  CHECK_EQ(newest(again, 0), std::string("r4"));
  CHECK_EQ(newest(again, 2), std::string("r2"));
}

TEST(ring_key_names_follow_capacity) {
  char buf[8];
  CHECK(RingStore::makeKey('H', 100, 7, buf, sizeof(buf)));
  CHECK_EQ(std::string(buf), std::string("H07"));
  CHECK(RingStore::makeKey('H', 100, 99, buf, sizeof(buf)));
  CHECK_EQ(std::string(buf), std::string("H99"));
  CHECK(RingStore::makeKey('O', 200, 7, buf, sizeof(buf)));
  CHECK_EQ(std::string(buf), std::string("O007"));
  CHECK(!RingStore::makeKey('H', 100, 100, buf, sizeof(buf)));  // 容量を超えるスロット
}

TEST(ring_writes_slot_and_meta_keys) {
  MemoryKvStore kv;
  RingStore rs(kv, "hist", 'H', 100, kRecSize);
  rs.load();
  add(rs, "a");
  CHECK(kv.hasKey("hist", "H00"));
  CHECK(kv.hasKey("hist", "meta"));
}

// --- 電源断 ---

TEST(ring_power_loss_after_slot_counts_the_record) {
  MemoryKvStore kv;
  RingStore rs(kv, "hist", 'H', 100, kRecSize);
  rs.load();
  kv.fail_after_puts = 1;  // スロットは書けて、meta の前で落ちる
  CHECK(!add(rs, "a"));
  RingStore again(kv, "hist", 'H', 100, kRecSize);
  CHECK(again.load());
  CHECK_EQ(again.count(), uint16_t{1});
  CHECK_EQ(newest(again, 0), std::string("a"));
}

TEST(ring_power_loss_before_slot_keeps_old_state) {
  MemoryKvStore kv;
  RingStore rs(kv, "hist", 'H', 100, kRecSize);
  rs.load();
  add(rs, "a");
  kv.fail_after_puts = kv.puts;  // 次の書き込みから全部落ちる
  CHECK(!add(rs, "b"));
  CHECK_EQ(rs.count(), uint16_t{1});
  RingStore again(kv, "hist", 'H', 100, kRecSize);
  CHECK(again.load());
  CHECK_EQ(again.count(), uint16_t{1});
  CHECK_EQ(newest(again, 0), std::string("a"));
}

TEST(ring_power_loss_when_full_rolls_forward) {
  MemoryKvStore kv;
  RingStore rs(kv, "hist", 'H', 3, kRecSize);
  rs.load();
  for (const char* n : {"r1", "r2", "r3"}) add(rs, n);
  kv.fail_after_puts = kv.puts + 1;  // r4 のスロットは書けて (r1 を上書き)、meta の前で落ちる
  CHECK(!add(rs, "r4"));
  RingStore again(kv, "hist", 'H', 3, kRecSize);
  CHECK(again.load());
  CHECK_EQ(again.count(), uint16_t{3});
  CHECK_EQ(newest(again, 0), std::string("r4"));
  CHECK_EQ(newest(again, 2), std::string("r2"));
}

TEST(ring_append_after_roll_forward_continues) {
  MemoryKvStore kv;
  RingStore rs(kv, "hist", 'H', 100, kRecSize);
  rs.load();
  kv.fail_after_puts = 1;
  add(rs, "a");
  kv.fail_after_puts = -1;
  RingStore again(kv, "hist", 'H', 100, kRecSize);
  again.load();
  CHECK(add(again, "b"));
  RingStore third(kv, "hist", 'H', 100, kRecSize);
  CHECK(third.load());
  CHECK_EQ(third.count(), uint16_t{2});
  CHECK_EQ(newest(third, 0), std::string("b"));
  CHECK_EQ(newest(third, 1), std::string("a"));
}

// --- 消去・壊れたデータ ---

TEST(ring_clear_removes_everything) {
  MemoryKvStore kv;
  RingStore rs(kv, "hist", 'H', 100, kRecSize);
  rs.load();
  add(rs, "a");
  add(rs, "b");
  CHECK(rs.clear());
  CHECK_EQ(rs.count(), uint16_t{0});
  CHECK(!kv.hasKey("hist", "H00"));
  CHECK(!kv.hasKey("hist", "meta"));
  RingStore again(kv, "hist", 'H', 100, kRecSize);
  CHECK(again.load());
  CHECK_EQ(again.count(), uint16_t{0});
}

TEST(ring_corrupt_slot_is_not_returned) {
  MemoryKvStore kv;
  RingStore rs(kv, "hist", 'H', 100, kRecSize);
  rs.load();
  add(rs, "a");
  kv.data["hist"]["H00"].resize(3);  // 長さが合わない
  CHECK_EQ(newest(rs, 0), std::string("<none>"));
}

TEST(ring_corrupt_meta_loads_as_failed_empty) {
  MemoryKvStore kv;
  RingStore rs(kv, "hist", 'H', 5, kRecSize);
  rs.load();
  add(rs, "a");
  auto& meta = kv.data["hist"]["meta"];
  CHECK_EQ(meta.size(), size_t{8});
  if (meta.size() >= 2) {
    meta[0] = 9;  // 次に書くスロット = 9 (容量 5 を超える)
    meta[1] = 0;
  }
  RingStore again(kv, "hist", 'H', 5, kRecSize);
  CHECK(!again.load());
  CHECK_EQ(again.count(), uint16_t{0});
}
