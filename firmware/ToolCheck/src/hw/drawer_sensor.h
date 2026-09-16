// 引き出しセンサ (Unit ToF4M、VL53L1X、I2C 0x29) (docs/設計.md「引き出し検知」)。
// 2026-09-14: 設定で使う / 使わないを切り替える (既定は使う)。使う設定なのに見つからなければ、ヘッダの ToF● を赤にする。
// M6 (2026-09-15): 距離を連続で測り、core/drawer_detector で開閉を決める。
//   - 届いたのは計画の Unit ToF (VL53L0X) ではなく Unit ToF4M (VL53L1X、型番 0x010F = 0xEACC) だった。
//     ドライバは Pololu VL53L1X 1.3.1 (M5 公式の M5Unit-ToF4M も中でこれを使う)。VL53L0X には対応していない (初期化に失敗したら型番をログに出す)
//   - タイムアウトの既定は 0 = 無限に待つので、必ず setTimeout する
//   - read() の既定は結果が出るまで待つので、dataReady() を見てから read(false) で読む (メインループを止めない)
//   - I2C の失敗が続く・結果が来なくなったら見失ったとみて、探し直す (抜き差しで ToF の設定が消えるので、見つけたら初期化し直す)
//   - 測定の間隔は電力状態で変える (PowerOutputs::tof_period_ms: 操作中 50 / 待機 100 / 減光・消灯 200ms)
// Port A の Wire は PortABus::begin が開き、PaHub の ch もそこで繋ぐ。メインループからだけ呼ぶ (Wire をタスクの間で取り合わない)。
#pragma once

#include <cstdint>

#include "../core/config.h"
#include "../core/drawer_detector.h"

namespace toolcheck {

struct DrawerSensorUpdate {
  bool status_changed = false;  // 見つけた・見失った・異常の有無が変わった (ヘッダを描き直す)
  bool sample = false;          // このループで距離を 1 つ読んだ
  uint16_t mm = 0;
  uint8_t range_status = 255;   // VL53L1X の読み取りの状態 (0 = 正常)
  bool valid = false;
  bool opened = false;          // 開閉の判定 (判定しない間は出ない)
  bool closed = false;
  bool rearmed = false;         // 登録後の押さえを解いた (閉の範囲に 2 秒続けて戻った)
};

class DrawerSensor {
 public:
  // PortABus::begin の後に呼ぶ。使わない設定なら ToF に触らない
  void begin(bool enabled, const Config& cfg, uint16_t period_ms, uint32_t now_ms);
  void setEnabled(bool enabled, uint32_t now_ms);

  // 基準距離・開閉の閾値を反映する。変わったときだけ判定を作り直して true (「閉」から数え直す)
  bool setConfig(const Config& cfg);

  // 測定の間隔 (ms)。測っている最中なら測り直しを始める
  void setPeriod(uint16_t period_ms);

  // false の間は距離を読むだけで開閉を判定しない (設定画面を開いている間・校正中。2026-09-16 から設定画面の全部)。
  // true に戻すと「閉」から数え直す
  void setJudging(bool judging);

  // 登録後に呼ぶ。閉の範囲に 2 秒続けて戻るまで「開」にしない (core/drawer_detector。設定の反映で判定を作り直すと解ける)
  void holdOpenUntilQuiet();
  bool openHeld() const;

  DrawerSensorUpdate update(uint32_t now_ms);

  bool enabled() const;
  bool present() const;     // ToF を見つけて測っている
  bool fault() const;       // 使えない値 (範囲外・信号が弱いなど) が続いている
  bool calibrated() const;  // 基準距離がある
  bool judging() const;
  DrawerState state() const;
  bool hasSample() const;
  uint16_t lastMm() const;
  uint8_t lastRangeStatus() const;
  bool lastValid() const;
  uint16_t periodMs() const;
  uint32_t initMs() const;     // 最後に初期化にかかった時間
  uint32_t sampleCount() const;
  uint32_t invalidCount() const;
  uint32_t startCount() const;  // 見つけて測り始めた回数 (2 回目からは見失った後の探し直し)

 private:
  bool start(uint32_t now_ms);
  void lose(uint32_t now_ms, const char* why);
  void noteIoError(uint32_t now_ms, DrawerSensorUpdate* u);

  bool enabled_ = false;
  bool present_ = false;
  bool judging_ = true;
  uint16_t baseline_mm_ = 0;
  uint16_t open_delta_mm_ = 0;
  uint16_t close_delta_mm_ = 0;
  DrawerDetector detector_{DrawerDetectorConfig{}};
  uint16_t period_ms_ = 100;
  uint32_t next_probe_ms_ = 0;
  uint32_t next_poll_ms_ = 0;
  uint32_t last_result_ms_ = 0;
  uint8_t io_errors_ = 0;
  bool has_sample_ = false;
  uint16_t last_mm_ = 0;
  uint8_t last_range_status_ = 255;
  bool last_valid_ = false;
  uint32_t init_ms_ = 0;
  uint32_t samples_ = 0;
  uint32_t invalid_ = 0;
  uint32_t starts_ = 0;
  uint32_t init_failures_ = 0;  // 応答はあるのに初期化できなかった回数
};

}  // namespace toolcheck
