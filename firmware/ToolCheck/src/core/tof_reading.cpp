#include "tof_reading.h"

#include <algorithm>
#include <vector>

namespace toolcheck {

bool tofDistanceValid(uint16_t mm) { return mm > 0 && mm < kTofOutOfRangeMm; }

bool tofRangeStatusUsable(uint8_t status) {
  // Pololu VL53L1X の RangeValid / RangeValidMinRangeClipped / RangeValidNoWrapCheckFail
  return status == 0 || status == 3 || status == 6;
}

TofCalibration calibrateTofBaseline(const uint16_t* mm, size_t n) {
  TofCalibration c;
  std::vector<uint16_t> v;
  if (mm != nullptr) {
    v.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      if (tofDistanceValid(mm[i])) v.push_back(mm[i]);
    }
  }
  c.valid_samples = static_cast<uint16_t>(v.size() > 0xFFFF ? 0xFFFF : v.size());
  if (v.empty()) return c;

  std::sort(v.begin(), v.end());
  c.min_mm = v.front();
  c.max_mm = v.back();
  const size_t mid = v.size() / 2;
  c.baseline_mm = (v.size() % 2 == 1)
                      ? v[mid]
                      : static_cast<uint16_t>((static_cast<uint32_t>(v[mid - 1]) + v[mid]) / 2);

  if (v.size() < kTofCalibrationMinSamples) {
    c.result = TofCalibrationResult::TooFewSamples;
    return c;
  }
  if (c.baseline_mm < kTofBaselineMinMm || c.baseline_mm > kTofBaselineMaxMm) {
    c.result = TofCalibrationResult::OutOfRange;
    return c;
  }
  size_t stable = 0;
  for (uint16_t x : v) {
    const int32_t diff = static_cast<int32_t>(x) - static_cast<int32_t>(c.baseline_mm);
    if (diff >= -static_cast<int32_t>(kTofStableMm) && diff <= static_cast<int32_t>(kTofStableMm)) ++stable;
  }
  if (stable * 100 < v.size() * kTofStablePercent) {
    c.result = TofCalibrationResult::Unstable;
    return c;
  }
  c.result = TofCalibrationResult::Ok;
  return c;
}

DrawerDetectorConfig drawerDetectorConfigFrom(const Config& cfg) {
  DrawerDetectorConfig d;
  d.open_delta_mm = cfg.open_delta_mm;
  d.close_delta_mm = cfg.close_delta_mm;
  return d;
}

}  // namespace toolcheck
