// 内部 I2C (M5Unified の In_I2C: タッチ・RTC・IO エキスパンダ・カメラの SCCB) を、撮影タスクとメインループで取り合わないための押さえ。
// M3 の実測: カメラが取り込んでいる間に M5.update() (タッチ) で内部 I2C を使うと、ISP のゲイン設定 (SCCB) が失敗した。
// メインループは tryLock() で押さえられたときだけ内部 I2C を使い、撮影タスクは取り込みの間 lock() で押さえ続ける。
#pragma once

#include <cstdint>

namespace toolcheck {
namespace internal_i2c {

// タスクを作る前に 1 回呼ぶ
void begin();

// すぐに押さえられたら true (メインループ用。押さえられなければ、その回は内部 I2C を使わない)
bool tryLock();

// timeout_ms まで待って押さえる
bool lock(uint32_t timeout_ms);

void unlock();

}  // namespace internal_i2c
}  // namespace toolcheck
