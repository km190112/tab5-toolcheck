// status_marks: ヘッダの ● を出すかと色 (docs/設計.md「画面」)。2026-09-15: 使わない設定の ● は出さない
#include "core/status_marks.h"
#include "testing.h"

using toolcheck::MarkLevel;
using toolcheck::StatusMarkInputs;
using toolcheck::StatusMarks;
using toolcheck::statusMarks;

namespace {

// 全部そろって使える状態
StatusMarkInputs allWorking() {
  StatusMarkInputs in;
  in.camera_enabled = true;
  in.camera_ready = true;
  in.save_photos_to_sd = true;
  in.sd_present = true;
  in.qr_present = true;
  in.drawer_enabled = true;
  in.drawer_present = true;
  in.drawer_calibrated = true;
  return in;
}

}  // namespace

TEST(marks_all_working_are_green) {
  const StatusMarks m = statusMarks(allWorking());
  CHECK(m.camera.shown);
  CHECK_EQ(m.camera.level, MarkLevel::Ok);
  CHECK(m.sd.shown);
  CHECK_EQ(m.sd.level, MarkLevel::Ok);
  CHECK(m.qr.shown);
  CHECK_EQ(m.qr.level, MarkLevel::Ok);
  CHECK(m.tof.shown);
  CHECK_EQ(m.tof.level, MarkLevel::Ok);
}

TEST(marks_camera_hidden_when_not_used) {
  StatusMarkInputs in = allWorking();
  in.camera_enabled = false;
  in.camera_ready = false;
  const StatusMarks m = statusMarks(in);
  CHECK(!m.camera.shown);
  CHECK(!m.sd.shown);  // SD は写真にしか使わないので、カメラを使わなければ出さない
  CHECK(m.qr.shown);
  CHECK(m.tof.shown);
}

TEST(marks_camera_not_ready_is_red) {
  StatusMarkInputs in = allWorking();
  in.camera_ready = false;
  const StatusMarks m = statusMarks(in);
  CHECK(m.camera.shown);
  CHECK_EQ(m.camera.level, MarkLevel::Alert);
}

TEST(marks_sd_hidden_when_photos_are_not_saved) {
  StatusMarkInputs in = allWorking();
  in.save_photos_to_sd = false;
  const StatusMarks m = statusMarks(in);
  CHECK(!m.sd.shown);
  CHECK(m.camera.shown);
  in.sd_present = false;
  CHECK(!statusMarks(in).sd.shown);  // 保存しない設定なら、SD が無くても出さない
}

TEST(marks_sd_missing_when_used_is_red) {
  StatusMarkInputs in = allWorking();
  in.sd_present = false;
  const StatusMarks m = statusMarks(in);
  CHECK(m.sd.shown);
  CHECK_EQ(m.sd.level, MarkLevel::Alert);
}

TEST(marks_qr_is_always_shown) {
  StatusMarkInputs in;  // 何も使わない・何も見つからない
  StatusMarks m = statusMarks(in);
  CHECK(m.qr.shown);
  CHECK_EQ(m.qr.level, MarkLevel::Alert);
  CHECK(!m.camera.shown);
  CHECK(!m.sd.shown);
  CHECK(!m.tof.shown);

  in.qr_present = true;
  m = statusMarks(in);
  CHECK_EQ(m.qr.level, MarkLevel::Ok);
  in.qr_failed = true;
  m = statusMarks(in);
  CHECK_EQ(m.qr.level, MarkLevel::Alert);
}

TEST(marks_tof_hidden_when_drawer_sensor_not_used) {
  StatusMarkInputs in = allWorking();
  in.drawer_enabled = false;
  in.drawer_present = false;
  const StatusMarks m = statusMarks(in);
  CHECK(!m.tof.shown);
  CHECK(m.camera.shown);
}

TEST(marks_tof_levels) {
  StatusMarkInputs in = allWorking();
  in.drawer_calibrated = false;
  StatusMarks m = statusMarks(in);
  CHECK(m.tof.shown);
  CHECK_EQ(m.tof.level, MarkLevel::Warn);  // 未校正 (開閉を判定しない)

  in.drawer_fault = true;
  m = statusMarks(in);
  CHECK_EQ(m.tof.level, MarkLevel::Alert);  // 使えない値が続くのは未校正より強い

  in = allWorking();
  in.drawer_present = false;
  m = statusMarks(in);
  CHECK(m.tof.shown);
  CHECK_EQ(m.tof.level, MarkLevel::Alert);  // 使う設定なのに見つからない
}
