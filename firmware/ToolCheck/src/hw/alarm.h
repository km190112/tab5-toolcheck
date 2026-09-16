// 警告音と操作音 (docs/設計.md「セッション」「画面の明るさと音量」)。鳴らし方は M2 で聞き分けたもの (spikes/README.md):
//   操作音 = 2400Hz 70ms / 警告 1 = 2000Hz を 300ms 鳴らして 300ms 休む (断続) / 警告 2 = 2800Hz と 2000Hz を 250ms ずつ交互 (連続)
// 音量は百分率 × 255 / 100 で M5.Speaker に渡す。
#pragma once

#include <cstdint>

namespace toolcheck {

struct ToneStep {
  uint16_t hz;  // 0 = 休み
  uint16_t ms;
};

class Alarm {
 public:
  // level: 0 = 止める / 1 = 警告 1 / 2 = 警告 2。段が変わったときだけ鳴らし方を切り替え、音量は変わったら反映する
  void setLevel(uint8_t level, uint8_t volume_pct, uint32_t now_ms);
  // 操作音。警告を鳴らしている間と音量 0 のときは鳴らさない
  void beep(uint8_t volume_pct);
  void update(uint32_t now_ms);
  uint8_t level() const;
  // 開発中の消音 (sim mute。保存しない。2026-09-14: 夜中の試験で音を鳴らさない)。消音中も level() は段を返す
  void setMuted(bool muted);
  bool muted() const;
  // 試し鳴らし (設定画面): level の鳴らし方を duration_ms だけ鳴らして止める。本当の警告を鳴らしている間は鳴らさない
  void preview(uint8_t level, uint8_t volume_pct, uint32_t now_ms, uint32_t duration_ms);
  // 試し鳴らしの間は、呼び出し側がセッションの警告の段で setLevel しない
  bool previewing() const;

 private:
  void startStep(uint32_t now_ms);

  uint8_t level_ = 0;
  uint8_t volume_ = 0;
  const ToneStep* steps_ = nullptr;
  uint8_t step_count_ = 0;
  uint8_t index_ = 0;
  uint32_t step_at_ = 0;
  bool muted_ = false;
  bool previewing_ = false;
  uint32_t preview_until_ms_ = 0;
};

}  // namespace toolcheck
