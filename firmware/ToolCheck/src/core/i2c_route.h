// Port A の経路を探す: 各ユニット (QR 0x21・ToF 0x29) が Unit PaHub v2.1 のどの ch の先にいるか、直結か (docs/設計.md「Unit PaHub v2.1」)。
// Arduino 非依存。I2C の読み書きは I2cBus を実装した呼び出し側 (hw/port_a_bus) が行う。
//
// PaHub v2.1 = PCA9548AP。制御は 1 バイトのレジスタだけ (書くとビット n で ch n を繋ぐ・読むと今の値)。
// 電源を入れた直後と抜き差しの後は 0 (全部切れている) なので、ch を選ばないと向こうのユニットは見えない (2026-09-15 実機で qr=0)。
//
// 探し方 (毎回すべて探し直す。探す間は ch が切れるが、メインループは 1 本でその間にユニットとやりとりしないので困らない):
//   1. 0x70〜0x77 のうち、書いた値 (0x00・0x01) を読み戻せる最初のものを PaHub とみなす (0x70 台にいる別の部品を取り違えない)。
//      アドレスは DIP スイッチで変わり、ON が H か L かも図だけでは決まらないので決め打ちしない
//   2. PaHub の ch を全部切ってから、ユニットを直に探す (直結・受動の Grove ハブ)
//   3. 見つからないユニットを ch0〜5 の順に探す (PaHub が出している ch は 6 つ)。同じアドレスが 2 つの ch にあれば若い方
//   4. 見つかったユニットの ch をまとめて繋いだままにする (QR と ToF はアドレスが違うので同時に繋げる。やりとりの度に切り替えない)
#pragma once

#include <cstddef>
#include <cstdint>

namespace toolcheck {

class I2cBus {
 public:
  virtual ~I2cBus() = default;
  // 0 バイトの書き込みに ACK が返るか
  virtual bool probe(uint8_t addr) = 0;
  virtual bool writeByte(uint8_t addr, uint8_t value) = 0;
  virtual bool readByte(uint8_t addr, uint8_t* value) = 0;
};

constexpr uint8_t kHubFirstAddr = 0x70;
constexpr uint8_t kHubLastAddr = 0x77;
constexpr uint8_t kHubChannels = 6;  // PCA9548AP は 8 ch だが PaHub v2.1 が出しているのは 0〜5

constexpr uint8_t kNoHub = 0x00;     // I2cRoute::hub_addr: PaHub が無い
constexpr uint8_t kDirect = 0xFE;    // I2cRoute::channelOf: PaHub を通さず直に見えた
constexpr uint8_t kNotFound = 0xFF;  // I2cRoute::channelOf: 見つからない (探していないアドレスも)

constexpr size_t kMaxRouteTargets = 4;

struct I2cRoute {
  uint8_t hub_addr = kNoHub;
  uint8_t mask = 0;   // PaHub で繋いでいる ch (ビット n = ch n)
  uint8_t count = 0;  // 探したユニットの数
  uint8_t addr[kMaxRouteTargets] = {0, 0, 0, 0};
  uint8_t channel[kMaxRouteTargets] = {kNotFound, kNotFound, kNotFound, kNotFound};

  // 0〜5 = PaHub の ch / kDirect / kNotFound
  uint8_t channelOf(uint8_t address) const;
  bool found(uint8_t address) const;
};

// targets (n 個まで。kMaxRouteTargets を超えた分は探さない) を探し、見つかった ch を繋いだ状態にして返す
I2cRoute findI2cRoute(I2cBus& bus, const uint8_t* targets, size_t n);

// 経路がそのまま使えるか: PaHub があればレジスタの読み戻しが mask と同じ (抜き差しで 0 に戻っていない)、
// かつ見つけたユニットが全部応答する。見つかっていないユニットは見ない (探し直すかは呼び出し側が決める)
bool i2cRouteIntact(I2cBus& bus, const I2cRoute& route);

}  // namespace toolcheck
