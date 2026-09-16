#include "status_marks.h"

namespace toolcheck {
namespace {

StatusMark mark(bool shown, MarkLevel level) {
  StatusMark m;
  m.shown = shown;
  m.level = shown ? level : MarkLevel::Ok;
  return m;
}

}  // namespace

StatusMarks statusMarks(const StatusMarkInputs& in) {
  StatusMarks m;
  m.camera = mark(in.camera_enabled, in.camera_ready ? MarkLevel::Ok : MarkLevel::Alert);
  m.sd = mark(in.camera_enabled && in.save_photos_to_sd, in.sd_present ? MarkLevel::Ok : MarkLevel::Alert);
  m.qr = mark(true, (in.qr_present && !in.qr_failed) ? MarkLevel::Ok : MarkLevel::Alert);
  const MarkLevel tof = (!in.drawer_present || in.drawer_fault) ? MarkLevel::Alert
                        : !in.drawer_calibrated                 ? MarkLevel::Warn
                                                                : MarkLevel::Ok;
  m.tof = mark(in.drawer_enabled, tof);
  return m;
}

}  // namespace toolcheck
