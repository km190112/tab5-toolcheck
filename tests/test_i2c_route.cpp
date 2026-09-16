// i2c_route: Port A の経路 (Unit PaHub v2.1 の ch か直結か) を探す (docs/設計.md「Unit PaHub v2.1」)
#include <cstddef>
#include <cstdint>
#include <set>

#include "core/i2c_route.h"
#include "testing.h"

using toolcheck::findI2cRoute;
using toolcheck::I2cBus;
using toolcheck::I2cRoute;
using toolcheck::i2cRouteIntact;
using toolcheck::kDirect;
using toolcheck::kNoHub;
using toolcheck::kNotFound;

namespace {

constexpr uint8_t kQr = 0x21;
constexpr uint8_t kTof = 0x29;
constexpr uint8_t kTargets[] = {kQr, kTof};

// Port A の偽物: PCA9548A (制御レジスタ 1 バイト) と、その ch の先・直結にいるユニット
class FakePortA : public I2cBus {
 public:
  uint8_t hub_addr = 0;          // 0 = PaHub なし
  uint8_t hub_reg = 0;           // 電源を入れた直後は全部切れている
  uint8_t hub_seen_bits = 0;     // PaHub に書いたことのあるビット
  std::set<uint8_t> direct;      // 直結 (受動の Grove ハブを含む)
  std::set<uint8_t> channel[8];  // PCA9548A の ch (PaHub v2.1 が出しているのは 0〜5)
  uint8_t odd_addr = 0;          // 0x70 台にいる PaHub でない部品 (ACK は返すが書いた値を読み戻せない)

  bool probe(uint8_t addr) override { return responds(addr); }

  bool writeByte(uint8_t addr, uint8_t value) override {
    if (hub_addr != 0 && addr == hub_addr) {
      hub_reg = value;
      hub_seen_bits |= value;
      return true;
    }
    return responds(addr);
  }

  bool readByte(uint8_t addr, uint8_t* value) override {
    if (hub_addr != 0 && addr == hub_addr) {
      *value = hub_reg;
      return true;
    }
    if (odd_addr != 0 && addr == odd_addr) {
      *value = 0x5A;
      return true;
    }
    if (!responds(addr)) return false;
    *value = 0;
    return true;
  }

  // PaHub を抜き差しした (レジスタが 0 に戻る)
  void powerCycleHub() { hub_reg = 0; }

 private:
  bool responds(uint8_t addr) const {
    if (hub_addr != 0 && addr == hub_addr) return true;
    if (odd_addr != 0 && addr == odd_addr) return true;
    if (direct.count(addr) > 0) return true;
    if (hub_addr == 0) return false;
    for (int ch = 0; ch < 8; ++ch) {
      if ((hub_reg & (1u << ch)) != 0 && channel[ch].count(addr) > 0) return true;
    }
    return false;
  }
};

I2cRoute find(FakePortA& bus) { return findI2cRoute(bus, kTargets, 2); }

// 2026-09-15 に繋いだ形: DIP 全部 H (0x77)、ch0 に QR、ch1 に ToF
FakePortA kenWiring() {
  FakePortA bus;
  bus.hub_addr = 0x77;
  bus.channel[0].insert(kQr);
  bus.channel[1].insert(kTof);
  return bus;
}

}  // namespace

TEST(i2c_route_finds_units_behind_hub) {
  FakePortA bus = kenWiring();
  const I2cRoute r = find(bus);
  CHECK_EQ(r.hub_addr, 0x77);
  CHECK_EQ(r.channelOf(kQr), 0);
  CHECK_EQ(r.channelOf(kTof), 1);
  CHECK(r.found(kQr));
  CHECK(r.found(kTof));
  CHECK_EQ(r.mask, 0x03);
  CHECK_EQ(bus.hub_reg, 0x03);  // 見つけた 2 つの ch を同時に繋いだまま
  CHECK(bus.probe(kQr));
  CHECK(bus.probe(kTof));
  CHECK(i2cRouteIntact(bus, r));
}

TEST(i2c_route_finds_hub_at_any_address) {
  for (uint8_t addr = 0x70; addr <= 0x77; ++addr) {
    FakePortA bus;
    bus.hub_addr = addr;
    bus.channel[2].insert(kQr);
    const I2cRoute r = find(bus);
    CHECK_EQ(r.hub_addr, addr);
    CHECK_EQ(r.channelOf(kQr), 2);
  }
}

TEST(i2c_route_follows_moved_channels) {
  FakePortA bus = kenWiring();
  I2cRoute r = find(bus);
  bus.channel[0].clear();
  bus.channel[1].clear();
  bus.channel[3].insert(kQr);
  bus.channel[5].insert(kTof);
  CHECK(!i2cRouteIntact(bus, r));
  r = find(bus);
  CHECK_EQ(r.channelOf(kQr), 3);
  CHECK_EQ(r.channelOf(kTof), 5);
  CHECK_EQ(r.mask, 0x28);
  CHECK_EQ(bus.hub_reg, 0x28);
  CHECK(i2cRouteIntact(bus, r));
}

TEST(i2c_route_follows_one_moved_unit) {
  FakePortA bus = kenWiring();
  I2cRoute r = find(bus);
  bus.channel[1].clear();
  bus.channel[4].insert(kTof);
  CHECK(!i2cRouteIntact(bus, r));  // ToF が応答しない (QR はそのまま)
  r = find(bus);
  CHECK_EQ(r.channelOf(kQr), 0);
  CHECK_EQ(r.channelOf(kTof), 4);
  CHECK_EQ(r.mask, 0x11);
  CHECK(i2cRouteIntact(bus, r));
}

TEST(i2c_route_without_hub_uses_direct) {
  FakePortA bus;
  bus.direct.insert(kQr);
  bus.direct.insert(kTof);
  const I2cRoute r = find(bus);
  CHECK_EQ(r.hub_addr, kNoHub);
  CHECK_EQ(r.channelOf(kQr), kDirect);
  CHECK_EQ(r.channelOf(kTof), kDirect);
  CHECK_EQ(r.mask, 0);
  CHECK(i2cRouteIntact(bus, r));
}

TEST(i2c_route_reconnects_after_hub_power_cycle) {
  FakePortA bus = kenWiring();
  I2cRoute r = find(bus);
  bus.powerCycleHub();
  CHECK(!i2cRouteIntact(bus, r));  // レジスタが 0 に戻り、どちらのユニットも見えない
  r = find(bus);
  CHECK_EQ(bus.hub_reg, 0x03);
  CHECK(i2cRouteIntact(bus, r));
}

TEST(i2c_route_hub_register_change_breaks_route) {
  FakePortA bus = kenWiring();
  const I2cRoute r = find(bus);
  bus.hub_reg = 0x07;  // ユニットは応答するが、読み戻しが違う
  CHECK(!i2cRouteIntact(bus, r));
}

TEST(i2c_route_hub_removed_and_unit_plugged_direct) {
  FakePortA bus = kenWiring();
  I2cRoute r = find(bus);
  bus.hub_addr = 0;
  bus.channel[0].clear();
  bus.channel[1].clear();
  bus.direct.insert(kQr);
  CHECK(!i2cRouteIntact(bus, r));  // PaHub が応答しない
  r = find(bus);
  CHECK_EQ(r.hub_addr, kNoHub);
  CHECK_EQ(r.channelOf(kQr), kDirect);
  CHECK_EQ(r.channelOf(kTof), kNotFound);
  CHECK(i2cRouteIntact(bus, r));
}

TEST(i2c_route_ignores_non_hub_in_hub_range) {
  FakePortA bus;
  bus.odd_addr = 0x70;
  bus.hub_addr = 0x74;
  bus.channel[2].insert(kQr);
  const I2cRoute r = find(bus);
  CHECK_EQ(r.hub_addr, 0x74);
  CHECK_EQ(r.channelOf(kQr), 2);
}

TEST(i2c_route_non_hub_alone_is_not_hub) {
  FakePortA bus;
  bus.odd_addr = 0x70;
  bus.direct.insert(kQr);
  const I2cRoute r = find(bus);
  CHECK_EQ(r.hub_addr, kNoHub);
  CHECK_EQ(r.channelOf(kQr), kDirect);
  CHECK(i2cRouteIntact(bus, r));
}

TEST(i2c_route_duplicate_address_uses_lowest_channel) {
  FakePortA bus;
  bus.hub_addr = 0x70;
  bus.channel[2].insert(kQr);
  bus.channel[4].insert(kQr);
  const I2cRoute r = find(bus);
  CHECK_EQ(r.channelOf(kQr), 2);
  CHECK_EQ(r.mask, 0x04);  // 同じアドレスが 2 つ繋がらないよう ch4 は繋がない
}

TEST(i2c_route_missing_unit_does_not_break_route) {
  FakePortA bus;
  bus.hub_addr = 0x77;
  bus.channel[0].insert(kQr);
  const I2cRoute r = find(bus);
  CHECK_EQ(r.channelOf(kQr), 0);
  CHECK_EQ(r.channelOf(kTof), kNotFound);
  CHECK(!r.found(kTof));
  CHECK_EQ(r.mask, 0x01);
  CHECK(i2cRouteIntact(bus, r));
}

TEST(i2c_route_direct_and_hub_mixed) {
  FakePortA bus;
  bus.hub_addr = 0x77;
  bus.direct.insert(kQr);
  bus.channel[1].insert(kTof);
  const I2cRoute r = find(bus);
  CHECK_EQ(r.channelOf(kQr), kDirect);
  CHECK_EQ(r.channelOf(kTof), 1);
  CHECK_EQ(r.mask, 0x02);
  CHECK_EQ(bus.hub_reg, 0x02);
  CHECK(i2cRouteIntact(bus, r));
}

TEST(i2c_route_searches_only_six_channels) {
  FakePortA bus;
  bus.hub_addr = 0x77;
  bus.channel[6].insert(kTof);
  const I2cRoute r = find(bus);
  CHECK_EQ(r.channelOf(kTof), kNotFound);
  CHECK_EQ(bus.hub_seen_bits & 0xC0, 0);  // ch6・ch7 は選ばない
}

TEST(i2c_route_nothing_connected) {
  FakePortA bus;
  const I2cRoute r = find(bus);
  CHECK_EQ(r.hub_addr, kNoHub);
  CHECK_EQ(r.count, 2);
  CHECK_EQ(r.channelOf(kQr), kNotFound);
  CHECK_EQ(r.channelOf(kTof), kNotFound);
  CHECK_EQ(r.channelOf(0x55), kNotFound);  // 探していないアドレス
  CHECK(i2cRouteIntact(bus, r));           // 見つけたものが無いので壊れてもいない
}
