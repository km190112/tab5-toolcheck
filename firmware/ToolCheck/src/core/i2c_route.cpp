#include "i2c_route.h"

namespace toolcheck {
namespace {

bool writeVerified(I2cBus& bus, uint8_t addr, uint8_t value) {
  uint8_t back = 0;
  return bus.writeByte(addr, value) && bus.readByte(addr, &back) && back == value;
}

// PCA9548A の制御レジスタなら、書いた値をそのまま読み戻せる。確かめた後は全部切った状態 (0) で返す
bool isHub(I2cBus& bus, uint8_t addr) {
  return bus.probe(addr) && writeVerified(bus, addr, 0x01) && writeVerified(bus, addr, 0x00);
}

}  // namespace

uint8_t I2cRoute::channelOf(uint8_t address) const {
  for (uint8_t i = 0; i < count; ++i) {
    if (addr[i] == address) return channel[i];
  }
  return kNotFound;
}

bool I2cRoute::found(uint8_t address) const { return channelOf(address) != kNotFound; }

I2cRoute findI2cRoute(I2cBus& bus, const uint8_t* targets, size_t n) {
  I2cRoute r;
  r.count = static_cast<uint8_t>(n < kMaxRouteTargets ? n : kMaxRouteTargets);
  for (uint8_t i = 0; i < r.count; ++i) r.addr[i] = targets[i];

  for (uint8_t a = kHubFirstAddr; a <= kHubLastAddr; ++a) {
    if (isHub(bus, a)) {
      r.hub_addr = a;
      break;
    }
  }
  // PaHub があれば全部切れている (isHub の後) ので、ここで応答するのは直結のユニット
  for (uint8_t i = 0; i < r.count; ++i) {
    if (bus.probe(r.addr[i])) r.channel[i] = kDirect;
  }
  if (r.hub_addr == kNoHub) return r;

  for (uint8_t ch = 0; ch < kHubChannels; ++ch) {
    const uint8_t bit = static_cast<uint8_t>(1u << ch);
    if (!bus.writeByte(r.hub_addr, bit)) break;  // 探している間に PaHub が抜けた (次の i2cRouteIntact で分かる)
    for (uint8_t i = 0; i < r.count; ++i) {
      if (r.channel[i] != kNotFound || !bus.probe(r.addr[i])) continue;  // 見つかっているものは若い ch のまま
      r.channel[i] = ch;
      r.mask = static_cast<uint8_t>(r.mask | bit);
    }
  }
  bus.writeByte(r.hub_addr, r.mask);  // 書けなければ次の i2cRouteIntact で読み戻しが合わない
  return r;
}

bool i2cRouteIntact(I2cBus& bus, const I2cRoute& route) {
  if (route.hub_addr != kNoHub) {
    uint8_t reg = 0;
    if (!bus.readByte(route.hub_addr, &reg) || reg != route.mask) return false;
  }
  for (uint8_t i = 0; i < route.count; ++i) {
    if (route.channel[i] != kNotFound && !bus.probe(route.addr[i])) return false;
  }
  return true;
}

}  // namespace toolcheck
