// 引き出しの開閉判定。ToF の距離 (mm) を 1 サンプルずつ受け取り、開いた / 閉じた / センサ異常を決める。
// Arduino 非依存。時刻は millis() 相当の単調増加 (32bit で一周する) を呼び出し側が渡す。
//
// 考え方 (docs/設計.md「引き出し検知」):
//   基準距離 = 全段を閉めて測った床までの距離 (校正で決める)。引き出しを引くと手前に物が来て距離が縮む。
//   開: 基準 - 距離 >= open_delta が open_samples 回続いたら
//   閉: 基準 - 距離 <= close_delta が close_hold_ms 続いたら (その間の値はヒステリシスとしてどちらにも倒さない)
//   異常: 無効なサンプル (タイムアウト・範囲外) が fault_samples 回続いたら
//   押さえ (2026-09-16): 登録後に holdOpenUntilQuiet() を呼ぶと、閉の範囲に rearm_quiet_ms 続けて入るまで「開」にしない
//         (開けたまま・作業者が前にいて反応したままのとき、次の開放を作らない。開のままなら「閉」は今までどおり出す)
#pragma once

#include <cstdint>

namespace toolcheck {

struct DrawerDetectorConfig {
  uint16_t open_delta_mm  = 100;   // これ以上近づいたら「開」の候補
  uint16_t close_delta_mm = 50;    // これ以下の縮みなら「閉」の候補
  uint8_t  open_samples   = 3;     // 「開」と決めるまでに続けて要る回数
  uint32_t close_hold_ms  = 1000;  // 「閉」と決めるまでに続けて要る時間
  uint8_t  fault_samples  = 10;    // 無効なサンプルがこの回数続いたら異常
  uint32_t rearm_quiet_ms = 2000;  // 押さえた後、閉の範囲にこれだけ続けて入ったら「開」を受け付け直す (2026-09-16)
};

enum class DrawerState : uint8_t {
  Uncalibrated,  // 基準距離が無い
  Closed,
  Open,
};

struct DrawerEvent {
  bool opened        = false;  // このサンプルで「開」になった
  bool closed        = false;  // このサンプルで「閉」になった
  bool fault_changed = false;  // 異常の有無がこのサンプルで変わった (今の値は isFault())
  bool rearmed       = false;  // このサンプルで押さえを解いた (次の「開」を受け付ける)
};

class DrawerDetector {
 public:
  explicit DrawerDetector(const DrawerDetectorConfig& cfg);

  // 基準距離を決める (校正)。全段を閉めた状態で測る前提なので、状態は「閉」に戻す。0 は校正前に戻す
  void setBaseline(uint16_t baseline_mm);

  // 1 サンプル分進める。valid=false はタイムアウト・範囲外 (distance_mm は見ない)
  DrawerEvent update(uint16_t distance_mm, bool valid, uint32_t now_ms);

  // 登録した後 (名札で確定・返却完了・取りやめ・セッションの終わり・判定の再開) に呼ぶ。
  // 閉の範囲に rearm_quiet_ms 続けて入るまで「開」にしない (作業者が前にいる・開けたままのとき。2026-09-16)
  void holdOpenUntilQuiet();
  bool openHeld() const;

  DrawerState state() const;
  bool isFault() const;

 private:
  DrawerDetectorConfig cfg_;
  uint16_t baseline_mm_ = 0;
  DrawerState state_ = DrawerState::Uncalibrated;
  bool fault_ = false;
  uint8_t open_count_ = 0;
  uint8_t invalid_count_ = 0;
  bool close_timing_ = false;
  uint32_t close_since_ms_ = 0;
  bool hold_ = false;           // 登録後の押さえ (閉の範囲に rearm_quiet_ms 続けて入るまで「開」にしない)
  bool quiet_ = false;          // 閉の範囲に続けて入っている
  uint32_t quiet_since_ms_ = 0;
};

}  // namespace toolcheck
