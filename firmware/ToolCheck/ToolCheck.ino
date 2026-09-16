/*
 * ToolCheck 本体ファーム (M5Stack Tab5)。仕様の正は docs/設計.md。
 * この .ino は setup / loop の配線だけにする。型と処理は src/ の .h / .cpp に置く (自動プロトタイプ生成の罠を避ける)。
 */
#include <M5Unified.h>

#include "src/app/app.h"

static toolcheck::App app;

void setup() {
  auto cfg = M5.config();
  cfg.internal_imu = false;  // 使わない (docs/設計.md「省電力」)
  cfg.internal_mic = false;
  cfg.external_display_value = 0;
  M5.begin(cfg);
  // 送信リングの既定は 256 バイト。USB が抜けたと一瞬見えるとき、一杯のリングから古いバイトが捨てられて行が欠けた (2026-09-14)。
  // 普段のログが一杯に当たりにくいよう広げる (begin の前でないと既定の 256 で作られる)
  Serial.setTxBufferSize(toolcheck::SerialPort::kTxBufferSize);
  // 書き込みで待たない。PC に繋いだままポートを閉じていると、一杯のリングへの書き込みが 1 回 2 秒止まる (理由は serial_port.h)
  Serial.setTxTimeoutMs(0);
  Serial.begin(115200);
  M5.Display.setTextWrap(false, false);  // M5.begin() の後でないと効かない
  app.begin();
}

void loop() {
  app.loop();  // M5.update() は App の中で内部 I2C を押さえてから呼ぶ (撮影タスクと取り合わないため)
  delay(2);
}
