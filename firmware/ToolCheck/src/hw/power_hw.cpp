#include "power_hw.h"

#include <M5Unified.h>

namespace toolcheck {
namespace power_hw {

void begin() {
  // ESP32-C6 (無線) の電源: IO エキスパンダ 0x44 bit0 = WLAN_PWR_EN。M2 の実測で −0.12A
  M5.getIOExpander(1).digitalWrite(0, false);
  // USB-A の 5V: M5Unified の setExtOutput では IO エキスパンダ 1 の pin3。Port A (QR・ToF の 5V) は IO エキスパンダ 0 の pin2 で別系統。
  // M2 の実測で電流の差は無かったが、使わないので切る
  M5.Power.setExtOutput(false, m5::ext_port_mask_t::ext_USB);
}

}  // namespace power_hw
}  // namespace toolcheck
