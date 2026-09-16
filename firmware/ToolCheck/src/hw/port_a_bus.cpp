#include "port_a_bus.h"

#include <Arduino.h>
#include <Wire.h>

#include <cstdio>

namespace toolcheck {
namespace {

constexpr int kSda = 53;
constexpr int kScl = 54;
constexpr uint32_t kI2cHz = 100000;
constexpr uint32_t kCheckMs = 5000;  // 抜き差し・ch の差し替えに気付くまで最長この間隔
constexpr uint8_t kTargets[] = {kQrI2cAddr, kTofI2cAddr};

// 応答の無いアドレスは NACK ですぐ返り、IDF のエラーログも出ない (2026-09-14 実機で 0x29 を 5 秒ごとに叩いて確認)
class WireBus : public I2cBus {
 public:
  bool probe(uint8_t addr) override {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
  }

  bool writeByte(uint8_t addr, uint8_t value) override {
    Wire.beginTransmission(addr);
    Wire.write(value);
    return Wire.endTransmission() == 0;
  }

  bool readByte(uint8_t addr, uint8_t* value) override {
    if (Wire.requestFrom(addr, static_cast<size_t>(1), true) != 1) {
      while (Wire.available() > 0) Wire.read();
      return false;
    }
    *value = static_cast<uint8_t>(Wire.read());
    return true;
  }
};

WireBus g_wire_bus;

bool reached(uint32_t now_ms, uint32_t at_ms) { return static_cast<int32_t>(now_ms - at_ms) >= 0; }

bool sameRoute(const I2cRoute& a, const I2cRoute& b) {
  if (a.hub_addr != b.hub_addr || a.mask != b.mask || a.count != b.count) return false;
  for (uint8_t i = 0; i < a.count; ++i) {
    if (a.addr[i] != b.addr[i] || a.channel[i] != b.channel[i]) return false;
  }
  return true;
}

void formatChannel(uint8_t ch, bool human, char* out, size_t cap) {
  if (ch == kNotFound) {
    std::snprintf(out, cap, "%s", human ? "なし" : "none");
  } else if (ch == kDirect) {
    std::snprintf(out, cap, "%s", human ? "直結" : "direct");
  } else {
    std::snprintf(out, cap, "ch%u", static_cast<unsigned>(ch));
  }
}

}  // namespace

void PortABus::begin(bool tof_wanted, uint32_t now_ms) {
  Wire.begin(kSda, kScl, kI2cHz);  // M5UnitQRCode に Wire の初期化を任せない (160Hz になる。CLAUDE.md)
  tof_wanted_ = tof_wanted;
  search();
  next_check_ms_ = now_ms + kCheckMs;
}

void PortABus::setTofWanted(bool wanted) { tof_wanted_ = wanted; }

bool PortABus::update(uint32_t now_ms) {
  if (!reached(now_ms, next_check_ms_)) return false;
  next_check_ms_ = now_ms + kCheckMs;
  if (i2cRouteIntact(g_wire_bus, route_) && !needsSearch()) return false;
  return rescan(now_ms);
}

bool PortABus::rescan(uint32_t now_ms) {
  const I2cRoute before = route_;
  search();
  next_check_ms_ = now_ms + kCheckMs;
  return !sameRoute(before, route_);
}

const I2cRoute& PortABus::route() const { return route_; }

uint32_t PortABus::searchCount() const { return searches_; }

uint32_t PortABus::lastSearchMs() const { return last_search_ms_; }

void PortABus::formatMachine(char* out, size_t cap) const {
  char hub[8];
  char qr[8];
  char tof[8];
  if (route_.hub_addr == kNoHub) {
    std::snprintf(hub, sizeof(hub), "none");
  } else {
    std::snprintf(hub, sizeof(hub), "0x%02x", static_cast<unsigned>(route_.hub_addr));
  }
  formatChannel(route_.channelOf(kQrI2cAddr), false, qr, sizeof(qr));
  formatChannel(route_.channelOf(kTofI2cAddr), false, tof, sizeof(tof));
  std::snprintf(out, cap, "hub=%s qr=%s tof=%s", hub, qr, tof);
}

void PortABus::formatHuman(char* out, size_t cap) const {
  char qr[16];
  char tof[16];
  formatChannel(route_.channelOf(kQrI2cAddr), true, qr, sizeof(qr));
  formatChannel(route_.channelOf(kTofI2cAddr), true, tof, sizeof(tof));
  if (route_.hub_addr == kNoHub) {
    std::snprintf(out, cap, "PaHub なし (QR %s・ToF %s)", qr, tof);
  } else {
    std::snprintf(out, cap, "PaHub 0x%02X (QR %s・ToF %s)", static_cast<unsigned>(route_.hub_addr), qr, tof);
  }
}

void PortABus::search() {
  const uint32_t start = millis();
  route_ = findI2cRoute(g_wire_bus, kTargets, sizeof(kTargets));
  last_search_ms_ = millis() - start;
  ++searches_;
}

bool PortABus::needsSearch() const {
  return !route_.found(kQrI2cAddr) || (tof_wanted_ && !route_.found(kTofI2cAddr));
}

}  // namespace toolcheck
