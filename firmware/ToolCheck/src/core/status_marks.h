// ヘッダの右端に並べる ● (カメラ・SD・QR・ToF) を出すかと色 (docs/設計.md「画面」)。
// Arduino 非依存。描くのは app/ui_controller。
// 2026-09-15: 使わない設定の ● は灰色で残さず出さない (要らない情報を減らし、利用者の認知負荷を下げる)。
//   - カメラ●: カメラを使う設定のときだけ (準備 OK 緑 / 使えない 赤)
//   - SD●    : カメラを使い、写真を SD に保存する設定のときだけ (SD は写真にしか使わない。あり 緑 / 無い 赤)
//   - QR●    : いつも (使える 緑 / 見つからない・読めない 赤)
//   - ToF●   : 引き出しセンサを使う設定のときだけ (測っている 緑 / 未校正 黄 / 見つからない・使えない値が続く 赤)
#pragma once

#include <cstdint>

namespace toolcheck {

enum class MarkLevel : uint8_t {
  Ok,     // 緑
  Warn,   // 黄
  Alert,  // 赤
};

struct StatusMark {
  bool shown = false;
  MarkLevel level = MarkLevel::Ok;
};

struct StatusMarkInputs {
  bool camera_enabled = false;     // カメラを使う (入切は再起動で反映するので、動いている方の値を渡す)
  bool camera_ready = false;
  bool save_photos_to_sd = false;  // 設定「写真を SD に保存」
  bool sd_present = false;
  bool qr_present = false;
  bool qr_failed = false;          // 作り直しても読めない
  bool drawer_enabled = false;     // 引き出しセンサを使う
  bool drawer_present = false;     // ToF を見つけて測っている
  bool drawer_fault = false;       // 使えない値が続いている
  bool drawer_calibrated = false;  // 基準距離がある
};

struct StatusMarks {
  StatusMark camera;
  StatusMark sd;
  StatusMark qr;
  StatusMark tof;
};

StatusMarks statusMarks(const StatusMarkInputs& in);

}  // namespace toolcheck
