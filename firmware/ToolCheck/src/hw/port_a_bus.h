// Port A の I2C (Wire、G53 SDA / G54 SCL、100kHz) の持ち主 (docs/設計.md「Unit PaHub v2.1」)。
// Unit PaHub v2.1 のどの ch に QR (0x21) と ToF (0x29) が挿さっているかを探して繋ぎ、QrReader と DrawerSensor が Wire を直に使えるようにする。
// 2026-09-15: 挿すところが変わっても自動で検知できるのが理想 → 一定の間隔で経路を確かめ、壊れたり見つからないものがあれば探し直す。
// 経路の探し方は core/i2c_route (ホストテスト済み)。メインループからだけ呼ぶ (Wire をタスクの間で取り合わない)。
#pragma once

#include <cstddef>
#include <cstdint>

#include "../core/i2c_route.h"

namespace toolcheck {

constexpr uint8_t kQrI2cAddr = 0x21;
constexpr uint8_t kTofI2cAddr = 0x29;

class PortABus {
 public:
  // M5.begin() の後、QrReader::begin の前に呼ぶ。Wire を開いて経路を探す
  void begin(bool tof_wanted, uint32_t now_ms);

  // 引き出しセンサを使わない設定なら、ToF が見つからなくても探し直さない
  void setTofWanted(bool wanted);

  // 一定の間隔で経路を確かめ、壊れた・要るユニットが見つからないなら探し直す。経路が変わったら true
  bool update(uint32_t now_ms);

  // 次の update を待たずに探し直す (コマンド i2c rescan)。経路が変わったら true
  bool rescan(uint32_t now_ms);

  const I2cRoute& route() const;
  uint32_t searchCount() const;
  uint32_t lastSearchMs() const;  // 最後に探すのにかかった時間

  // 機械向け: "hub=0x77 qr=ch0 tof=ch1" (直結は direct、無ければ none / hub=none)
  void formatMachine(char* out, size_t cap) const;
  // 人向け: "PaHub 0x77 (QR ch0・ToF ch1)" / "直結 (QR あり・ToF なし)"
  void formatHuman(char* out, size_t cap) const;

 private:
  void search();
  bool needsSearch() const;

  I2cRoute route_;
  bool tof_wanted_ = true;
  uint32_t next_check_ms_ = 0;
  uint32_t searches_ = 0;
  uint32_t last_search_ms_ = 0;
};

}  // namespace toolcheck
