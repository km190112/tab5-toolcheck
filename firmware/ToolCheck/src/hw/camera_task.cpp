#include "camera_task.h"

#include <M5Unified.h>
#include <ESP_Video.h>

#include <cstdio>
#include <cstring>

#include "driver/i2c_master.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "internal_i2c.h"
#include "sd_task.h"

namespace toolcheck {
namespace {

constexpr uint8_t kIoe0Addr = 0x43;
constexpr uint8_t kIoe0OutReg = 0x05;
constexpr uint8_t kCameraPowerBit = 0x40;  // esp-bsp の配線で 0x43 bit6 (本当にセンサの電源かは電流でも確かめられていない)
constexpr uint32_t kIoeFreq = 400000;
constexpr size_t kJpegOutBytes = 1024 * 1024;
constexpr uint32_t kJpegQuality = 80;
constexpr uint32_t kMaxFrames = 80;        // capture_delay_ms の上限 3000ms を約 65ms ごとのフレームで超える数
constexpr uint32_t kI2cLockTimeoutMs = 2000;
constexpr uint32_t kTaskStack = 6144;
constexpr UBaseType_t kTaskPriority = 2;
constexpr BaseType_t kTaskCore = 0;

ESPVideoClass g_video;
ESPVideoCaptureDevClass g_capture;
jpeg_encoder_handle_t g_jpeg = nullptr;
ppa_client_handle_t g_ppa = nullptr;
uint8_t* g_frame = nullptr;  // 取り込んだ 1 枚 (RGB565)
size_t g_frame_cap = 0;
uint8_t* g_rot = nullptr;    // 回した 1 枚 (PPA の出力・JPEG の入力。キャッシュ境界に揃える)
size_t g_rot_cap = 0;
uint8_t* g_jpeg_buf = nullptr;
size_t g_jpeg_cap = 0;
uint32_t g_w = 0;
uint32_t g_h = 0;

struct Request {
  char name[kPhotoFieldLen];
  uint16_t delay_ms;
  bool save_to_sd;
  bool test;
};

// 時計回りに 90° 回す (PPA は反時計回りなので 270°。M3 の実機で 90° が正しい向き)。出力は g_h × g_w
bool rotateFrame() {
  ppa_srm_oper_config_t op = {};
  op.in.buffer = g_frame;
  op.in.pic_w = g_w;
  op.in.pic_h = g_h;
  op.in.block_w = g_w;
  op.in.block_h = g_h;
  op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  op.out.buffer = g_rot;
  op.out.buffer_size = static_cast<uint32_t>(g_rot_cap);
  op.out.pic_w = g_h;
  op.out.pic_h = g_w;
  op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  op.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
  op.scale_x = 1.0f;
  op.scale_y = 1.0f;
  op.mode = PPA_TRANS_MODE_BLOCKING;
  return ppa_do_scale_rotate_mirror(g_ppa, &op) == ESP_OK;
}

bool encodeJpeg(uint32_t* out_len) {
  jpeg_encode_cfg_t cfg = {};
  cfg.width = g_h;
  cfg.height = g_w;
  cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
  cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
  cfg.image_quality = kJpegQuality;
  return jpeg_encoder_process(g_jpeg, &cfg, g_rot, g_w * g_h * 2, g_jpeg_buf, static_cast<uint32_t>(g_jpeg_cap),
                              out_len) == ESP_OK;
}

}  // namespace

bool CameraTask::begin(bool enabled, SdTask* sd) {
  sd_ = sd;
  enabled_ = enabled;
  if (!enabled) {
    M5.In_I2C.bitOff(kIoe0Addr, kIoe0OutReg, kCameraPowerBit, kIoeFreq);
    init_error_ = "カメラを使わない設定";
    return false;
  }
  const uint32_t t0 = millis();
  M5.In_I2C.bitOn(kIoe0Addr, kIoe0OutReg, kCameraPowerBit, kIoeFreq);
  i2c_master_bus_handle_t bus = nullptr;
  const i2c_port_num_t port = static_cast<i2c_port_num_t>(M5.In_I2C.getPort());
  if (i2c_master_get_bus_handle(port, &bus) != ESP_OK || bus == nullptr) {
    init_error_ = "内部 I2C のハンドルを取れません";
    return false;
  }
  ESPVideoCamConfigClass cam_cfg;
  cam_cfg.begin(bus);  // M5Unified の内部 I2C を共有する
  ESPVideoCSIConfigClass csi_cfg;
  csi_cfg.begin(cam_cfg, true);  // MIPI の PHY 電源 (LDO ch3) は M5GFX の DSI が取っている
  if (!g_video.begin(csi_cfg)) {
    init_error_ = "カメラを見つけられません";
    return false;
  }
  if (!g_capture.begin(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, 2) || !g_capture.setFormat(ESP_VIDEO_FORMAT_RGB565)) {
    init_error_ = "取り込みのデバイスを開けません";
    return false;
  }
  g_w = g_capture.getWidth();
  g_h = g_capture.getHeight();
  const size_t need = static_cast<size_t>(g_w) * g_h * 2;

  jpeg_encode_memory_alloc_cfg_t in_cfg = {};
  in_cfg.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER;
  jpeg_encode_memory_alloc_cfg_t out_cfg = {};
  out_cfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
  g_frame = static_cast<uint8_t*>(jpeg_alloc_encoder_mem(need, &in_cfg, &g_frame_cap));
  g_jpeg_buf = static_cast<uint8_t*>(jpeg_alloc_encoder_mem(kJpegOutBytes, &out_cfg, &g_jpeg_cap));

  // PPA の出力は、アドレスも大きさも PSRAM のキャッシュ境界に揃える (M3: 揃えないと ESP_ERR_INVALID_ARG)
  size_t align = 0;
  if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &align) != ESP_OK || align == 0) align = 64;
  g_rot_cap = (need + align - 1) / align * align;
  g_rot = static_cast<uint8_t*>(heap_caps_aligned_calloc(align, 1, g_rot_cap, MALLOC_CAP_SPIRAM));

  jpeg_encode_engine_cfg_t eng_cfg = {};
  eng_cfg.intr_priority = 0;
  eng_cfg.timeout_ms = 3000;
  const esp_err_t jerr = jpeg_new_encoder_engine(&eng_cfg, &g_jpeg);
  ppa_client_config_t ppa_cfg = {};
  ppa_cfg.oper_type = PPA_OPERATION_SRM;
  ppa_cfg.max_pending_trans_num = 1;
  const esp_err_t perr = ppa_register_client(&ppa_cfg, &g_ppa);

  latest_cap_ = g_jpeg_cap;
  latest_ = static_cast<uint8_t*>(heap_caps_malloc(latest_cap_, MALLOC_CAP_SPIRAM));
  requests_ = xQueueCreate(2, sizeof(Request));
  results_ = xQueueCreate(4, sizeof(CaptureResult));
  latest_mutex_ = xSemaphoreCreateMutex();
  if (g_frame == nullptr || g_rot == nullptr || g_jpeg_buf == nullptr || latest_ == nullptr || jerr != ESP_OK ||
      perr != ESP_OK || requests_ == nullptr || results_ == nullptr || latest_mutex_ == nullptr) {
    init_error_ = "取り込み・回転・JPEG のメモリか部品を用意できません";
    return false;
  }
  if (xTaskCreatePinnedToCore(&CameraTask::taskMain, "camera", kTaskStack, this, kTaskPriority, nullptr, kTaskCore) !=
      pdPASS) {
    init_error_ = "撮影のタスクを作れません";
    return false;
  }
  init_ms_ = millis() - t0;
  ready_ = true;
  return true;
}

bool CameraTask::ready() const { return ready_; }
bool CameraTask::enabled() const { return enabled_; }
const char* CameraTask::initError() const { return init_error_; }
uint32_t CameraTask::initMs() const { return init_ms_; }

bool CameraTask::request(const char* name, uint16_t delay_ms, bool save_to_sd, bool test) {
  if (!ready_) return false;
  Request req = {};
  std::snprintf(req.name, sizeof(req.name), "%s", name != nullptr ? name : "");
  req.delay_ms = delay_ms;
  req.save_to_sd = save_to_sd;
  req.test = test;
  return xQueueSend(requests_, &req, 0) == pdTRUE;
}

uint32_t CameraTask::busyMs(uint32_t now_ms) const {
  if (!busy_.load()) return 0;
  const uint32_t ms = now_ms - busy_since_ms_.load();
  return ms == 0 ? 1 : ms;
}

bool CameraTask::pollResult(CaptureResult* out) {
  return ready_ && out != nullptr && xQueueReceive(results_, out, 0) == pdTRUE;
}

size_t CameraTask::copyLatest(uint8_t* out, size_t cap, char* name, size_t name_cap) {
  if (!ready_ || out == nullptr) return 0;
  if (xSemaphoreTake(latest_mutex_, pdMS_TO_TICKS(200)) != pdTRUE) return 0;
  const size_t n = latest_len_.load();
  size_t copied = 0;
  if (n > 0 && n <= cap) {
    std::memcpy(out, latest_, n);
    copied = n;
    if (name != nullptr && name_cap > 0) std::snprintf(name, name_cap, "%s", latest_name_);
  }
  xSemaphoreGive(latest_mutex_);
  return copied;
}

size_t CameraTask::latestSize() const { return latest_len_.load(); }

void CameraTask::latestName(char* out, size_t cap) {
  if (out == nullptr || cap == 0) return;
  out[0] = '\0';
  if (!ready_ || xSemaphoreTake(latest_mutex_, pdMS_TO_TICKS(200)) != pdTRUE) return;
  if (latest_len_.load() > 0) std::snprintf(out, cap, "%s", latest_name_);
  xSemaphoreGive(latest_mutex_);
}

void CameraTask::taskMain(void* arg) { static_cast<CameraTask*>(arg)->run(); }

void CameraTask::run() {
  Request req;
  for (;;) {
    if (xQueueReceive(requests_, &req, portMAX_DELAY) != pdTRUE) continue;
    CaptureResult r;
    std::snprintf(r.name, sizeof(r.name), "%s", req.name);
    r.test = req.test;

    // 取り込みの間だけ内部 I2C を押さえる (メインループのタッチ・RTC は、その間は読まない)
    if (!internal_i2c::lock(kI2cLockTimeoutMs)) {
      r.error = "内部 I2C を押さえられません";
      xQueueSend(results_, &r, 0);
      continue;
    }
    busy_since_ms_.store(millis());
    busy_.store(true);
    const uint32_t t0 = millis();
    size_t frame_len = 0;
    if (!g_capture.startCapture()) {
      r.error = "取り込みを始められません";
    } else {
      for (uint32_t i = 0; i < kMaxFrames; ++i) {
        ESPVideoBufferClass buf = g_capture.captureBuffer();
        if (!buf.valid()) {
          r.error = "フレームを受け取れません";
          break;
        }
        const uint32_t elapsed = millis() - t0;
        if (++r.frames == 1) r.first_frame_ms = elapsed;
        if (elapsed >= req.delay_ms) {
          frame_len = buf.size() < g_frame_cap ? buf.size() : g_frame_cap;
          std::memcpy(g_frame, buf.data(), frame_len);
          r.used_frame_ms = elapsed;
          break;
        }
      }
      g_capture.stopCapture();
      if (frame_len == 0 && r.error[0] == '\0') r.error = "待ち時間の間にフレームが足りません";
    }
    busy_.store(false);
    internal_i2c::unlock();

    if (frame_len > 0) {
      const uint32_t t1 = millis();
      uint32_t jpeg_len = 0;
      if (!rotateFrame()) {
        r.error = "回転 (PPA) に失敗しました";
      } else if (!encodeJpeg(&jpeg_len) || jpeg_len == 0) {
        r.error = "JPEG にできません";
      } else {
        r.encode_ms = millis() - t1;
        r.jpeg_bytes = jpeg_len;
        r.ok = true;
        if (xSemaphoreTake(latest_mutex_, pdMS_TO_TICKS(1000)) == pdTRUE) {
          const size_t n = jpeg_len <= latest_cap_ ? jpeg_len : 0;
          if (n > 0) std::memcpy(latest_, g_jpeg_buf, n);
          std::snprintf(latest_name_, sizeof(latest_name_), "%s", req.name);
          latest_len_.store(n);
          xSemaphoreGive(latest_mutex_);
        }
        if (req.save_to_sd && sd_ != nullptr) {
          uint8_t* copy = static_cast<uint8_t*>(heap_caps_malloc(jpeg_len, MALLOC_CAP_SPIRAM));
          if (copy != nullptr) {
            std::memcpy(copy, g_jpeg_buf, jpeg_len);
            r.sd_queued = sd_->enqueueSave(req.name, copy, jpeg_len);
            if (!r.sd_queued) heap_caps_free(copy);
          }
        }
      }
    }
    xQueueSend(results_, &r, 0);
  }
}

}  // namespace toolcheck
