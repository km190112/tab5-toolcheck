// 内蔵カメラ (SC202CS、MIPI-CSI) の撮影を別のタスクで行う (docs/設計.md「写真」。2026-09-13 決定: 撮影もタスクにする)。
// 起動時に 1 回初期化し、待機中はセンサの電源を入れたまま取り込みを止めておく (2026-09-13 決定)。
// カメラを使わない設定なら、センサの電源 (IO エキスパンダ 0x43 bit6) を切って初期化しない。
// 撮影を頼まれたら、内部 I2C を押さえて取り込みを始め、delay_ms を過ぎた最初のフレームを取り、止めて押さえを離す。
// 回転 (PPA、時計回り 90°) とハードウェア JPEG は押さえを離してから行う。できた JPEG は直近の 1 枚として PSRAM に持ち、
// save_to_sd なら SdTask に渡す。タスクからはシリアルに出さず、結果はキューで返す (App が出す)。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "../core/loan_book.h"

namespace toolcheck {

class SdTask;

struct CaptureResult {
  char name[kPhotoFieldLen] = {0};
  bool ok = false;
  bool test = false;             // テスト撮影 (記録に使わない)
  bool sd_queued = false;        // SD の保存に回した
  uint32_t first_frame_ms = 0;   // 取り込みを始めてから最初のフレームまで
  uint32_t used_frame_ms = 0;    // 取り込みを始めてから使ったフレームまで
  uint32_t frames = 0;
  uint32_t encode_ms = 0;        // 回転 + JPEG
  uint32_t jpeg_bytes = 0;
  const char* error = "";        // 失敗の理由 (静的な文字列)
};

class CameraTask {
 public:
  // M5.begin() の後、ループを始める前に呼ぶ (内部 I2C を使う)。enabled = false なら電源を切って false を返す
  bool begin(bool enabled, SdTask* sd);

  bool ready() const;
  bool enabled() const;
  const char* initError() const;
  uint32_t initMs() const;

  // 撮影を頼む。キューが一杯なら false
  bool request(const char* name, uint16_t delay_ms, bool save_to_sd, bool test);

  // 取り込み中 (内部 I2C を押さえている) なら、始めてからの時間。取り込み中でなければ 0
  uint32_t busyMs(uint32_t now_ms) const;

  bool pollResult(CaptureResult* out);

  // 直近の写真 (JPEG) を out に写して大きさを返す。無い・入らなければ 0
  size_t copyLatest(uint8_t* out, size_t cap, char* name, size_t name_cap);
  size_t latestSize() const;
  // 直近の写真の名前だけを写す (無ければ空文字列)
  void latestName(char* out, size_t cap);

 private:
  static void taskMain(void* arg);
  void run();

  SdTask* sd_ = nullptr;
  bool enabled_ = false;
  bool ready_ = false;
  const char* init_error_ = "";
  uint32_t init_ms_ = 0;
  QueueHandle_t requests_ = nullptr;
  QueueHandle_t results_ = nullptr;
  SemaphoreHandle_t latest_mutex_ = nullptr;
  uint8_t* latest_ = nullptr;
  size_t latest_cap_ = 0;
  std::atomic<size_t> latest_len_{0};
  char latest_name_[kPhotoFieldLen] = {0};
  std::atomic<bool> busy_{false};
  std::atomic<uint32_t> busy_since_ms_{0};
};

}  // namespace toolcheck
