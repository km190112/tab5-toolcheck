/*
 * M3 カメラの go/no-go (Tab5 内蔵カメラ SC2356 = esp_cam_sensor の SC202CS、MIPI-CSI)
 *
 * 確かめること:
 *   - M5Unified の内部 I2C を共有してセンサーを見つけられるか (i2c_master_get_bus_handle で同じバスを ESP_Video に渡す)
 *   - MIPI の PHY 電源 (LDO ch3) は M5GFX の DSI が取っているので、CSI 側では LDO を初期化しない
 *   - RGB565 で取り込み、回転して半分の大きさで画面に出す / ハードウェア JPEG で microSD に保存して読み戻す
 *   - 取り込み開始から最初のフレーム・露出が落ち着くまでの時間
 *   - 1 回目: ISP がセンサーのゲインを SCCB で書けずに失敗し続けた。M5.update() (タッチ) を止めると 10 秒で 7 回 → 0 回
 *     → この版は既定で M5.update() を止める (`upd on` で戻す)
 *   - 2 回目: 紫・青っぽい (目視)。赤・緑・青・白の帯は画面でも JPEG でも正しかったので、中間の色で比べる (`pattern gray`)。
 *     同じフレームを画面と JPEG の両方に出して、画面の写しの色を比べる (`preview` → `jpgnow`)
 *   - ソフトの回転が 830ms かかったので、PPA (ハードウェアの回転) と比べる。出力はキャッシュ境界に揃えて確保する
 *   - (M3 の残り) 取り込み停止中・センサ電源 OFF の電流 (camseq。USB 電力チェッカーを読む)
 *   - (M3 の残り) 開放から撮影までの待ち時間: 明るさと AWB のゲインが落ち着くまで (settle = 停止からの再開 / cold = 電源 OFF からの再初期化)
 *
 * シリアルコマンド (改行終端):
 *   info / io / cam begin / cam start / cam stop / upd on|off / errs
 *   preview / jpgnow / ae [枚数] / live on|off / rot 0|90|180|270 / swap 0|1 / jrev 0|1 / hwrot on|off
 *   pattern / pattern jpg / pattern gray / pattern gray jpg / awb 0|1 / wb <赤ゲイン> <青ゲイン> / wbget
 *   shot [画質] / sd mount / sdview / screenshot / log / clear
 *   cam pwr on|off / cam end / settle [枚数] / cold [切る ms] [枚数] [入れてから待つ ms] / camseq [1 手順の秒数]
 */
#include <SD.h>  // M5GFX の drawJpgFile(SD, ...) を有効にするため、M5Unified より先に読む
#include <SPI.h>
#include <M5Unified.h>
#include <ESP_Video.h>
#include <cstdarg>
#include <cstring>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "driver/i2c_master.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "esp_video_isp_ioctl.h"

static const int      SD_CS     = 42;
static const int      SD_SCK    = 43;
static const int      SD_MOSI   = 44;
static const int      SD_MISO   = 39;
static const uint32_t SD_FREQ   = 20000000;
static const int      LOG_MAX   = 80;
static const int      PREVIEW_Y = 170;  // これより上は文字
static const uint8_t  IOE0_ADDR = 0x43;
static const uint8_t  IOE1_ADDR = 0x44;
static const size_t   JPEG_OUT_BYTES = 1024 * 1024;

ESPVideoClass video;
ESPVideoCaptureDevClass capture;

bool     camReady    = false;
bool     camStarted  = false;
uint32_t camStartAt  = 0;
uint32_t frameW      = 0;
uint32_t frameH      = 0;
uint8_t* frameBuf    = nullptr;  // 取り込んだ 1 枚 (RGB565)。JPEG の入力に使えるメモリで確保する
size_t   frameCap    = 0;
size_t   frameLen    = 0;
uint8_t* rotBuf      = nullptr;  // 回転した 1 枚 (PPA の出力・JPEG の入力)。キャッシュ境界に揃える
size_t   rotCap      = 0;
uint8_t* jpegBuf     = nullptr;
size_t   jpegCap     = 0;
uint16_t* previewBuf = nullptr;
size_t   previewCap  = 0;
jpeg_encoder_handle_t jpegEnc = nullptr;
ppa_client_handle_t   ppaSrm  = nullptr;
int      ispFd       = -1;

int    rotDeg      = 90;     // 1 回目の実機で 270 は上下逆、90 で正しい向き
bool   swapBytes   = false;
bool   jpegReverse = false;
bool   useHwRot    = true;
bool   liveView    = false;
bool   callUpdate  = false;  // M5.update() (タッチ) は SCCB とぶつかるので既定で止める
uint32_t lastLive  = 0;
uint32_t lastR = 0, lastG = 0, lastB = 0;  // 直近のフレームの RGB 平均 (0〜255)
bool   sdMounted   = false;
String lastPhoto   = "";
String lineBuf     = "";
String evLog[LOG_MAX];
int    evLogCount  = 0;

// ESP-IDF のログを数える (SCCB の失敗)
static vprintf_like_t origVprintf = nullptr;
volatile uint32_t sccbErrors = 0;

int countingVprintf(const char* fmt, va_list args) {
  char buf[200];
  va_list copy;
  va_copy(copy, args);
  vsnprintf(buf, sizeof(buf), fmt, copy);
  va_end(copy);
  if (strstr(buf, "sccb") != nullptr) sccbErrors++;
  return origVprintf ? origVprintf(fmt, args) : vprintf(fmt, args);
}

void pushLog(const String& entry) {
  if (evLogCount < LOG_MAX) {
    evLog[evLogCount++] = entry;
    return;
  }
  for (int i = 1; i < LOG_MAX; i++) evLog[i - 1] = evLog[i];
  evLog[LOG_MAX - 1] = entry;
}

void logf(const char* fmt, ...) {
  char buf[320];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
  pushLog(String((unsigned long)millis()) + " " + buf);
}

void drawHeader(const String& line1, const String& line2) {
  auto& d = M5.Display;
  d.startWrite();
  d.fillRect(0, 0, d.width(), PREVIEW_Y, TFT_BLACK);
  d.setTextSize(1);
  d.setFont(&fonts::lgfxJapanGothicP_32);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("M3 カメラ", 20, 10);
  d.setFont(&fonts::lgfxJapanGothicP_28);
  d.setTextColor(TFT_YELLOW, TFT_BLACK);
  d.drawString(line1, 20, 60);
  d.setTextColor(TFT_CYAN, TFT_BLACK);
  d.drawString(line2, 20, 110);
  d.endWrite();
}

// ---------------------------------------------------------------- カメラ

void camBegin() {
  if (camReady) {
    logf("#CAM begin already=1");
    return;
  }
  uint32_t t0 = millis();
  i2c_master_bus_handle_t bus = nullptr;
  i2c_port_num_t port = (i2c_port_num_t)M5.In_I2C.getPort();
  esp_err_t err = i2c_master_get_bus_handle(port, &bus);
  logf("#CAM i2c port=%d get_bus=%s", (int)port, esp_err_to_name(err));
  if (err != ESP_OK || bus == nullptr) return;

  ESPVideoCamConfigClass camCfg;
  camCfg.begin(bus);
  ESPVideoCSIConfigClass csiCfg;
  csiCfg.begin(camCfg, true);  // LDO ch3 は M5GFX の DSI が取っている
  if (!video.begin(csiCfg)) {
    logf("#CAM video_begin=0 ms=%lu", (unsigned long)(millis() - t0));
    return;
  }
  logf("#CAM video_begin=1 ms=%lu", (unsigned long)(millis() - t0));
  if (!capture.begin(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, 2)) {
    logf("#CAM open=0");
    return;
  }
  if (!capture.setFormat(ESP_VIDEO_FORMAT_RGB565)) {
    logf("#CAM format=0");
    return;
  }
  frameW = capture.getWidth();
  frameH = capture.getHeight();

  jpeg_encode_memory_alloc_cfg_t inCfg;
  inCfg.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER;
  jpeg_encode_memory_alloc_cfg_t outCfg;
  outCfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
  size_t need = (size_t)frameW * frameH * 2;
  frameBuf = (uint8_t*)jpeg_alloc_encoder_mem(need, &inCfg, &frameCap);
  jpegBuf = (uint8_t*)jpeg_alloc_encoder_mem(JPEG_OUT_BYTES, &outCfg, &jpegCap);

  // PPA の出力は、アドレスも大きさも PSRAM のキャッシュ境界に揃える必要がある (1 回目は ESP_ERR_INVALID_ARG)
  size_t align = 0;
  esp_err_t aerr = esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &align);
  if (aerr != ESP_OK || align == 0) align = 64;
  rotCap = (need + align - 1) / align * align;
  rotBuf = (uint8_t*)heap_caps_aligned_calloc(align, 1, rotCap, MALLOC_CAP_SPIRAM);

  jpeg_encode_engine_cfg_t engCfg;
  engCfg.intr_priority = 0;
  engCfg.timeout_ms = 3000;
  esp_err_t jerr = jpeg_new_encoder_engine(&engCfg, &jpegEnc);

  ppa_client_config_t ppaCfg = {};
  ppaCfg.oper_type = PPA_OPERATION_SRM;
  ppaCfg.max_pending_trans_num = 1;
  esp_err_t perr = ppa_register_client(&ppaCfg, &ppaSrm);

  camReady = frameBuf != nullptr && rotBuf != nullptr && jpegBuf != nullptr && jerr == ESP_OK;
  logf("#CAM begin=%d ms=%lu w=%lu h=%lu fmt=%s frame_cap=%u rot_cap=%u align=%u(%s) jpeg_cap=%u jpeg_enc=%s ppa=%s", (int)camReady,
       (unsigned long)(millis() - t0), (unsigned long)frameW, (unsigned long)frameH, capture.getFormatName(), (unsigned)frameCap,
       (unsigned)rotCap, (unsigned)align, esp_err_to_name(aerr), (unsigned)jpegCap, esp_err_to_name(jerr), esp_err_to_name(perr));
  drawHeader(camReady ? "カメラ準備 OK" : "カメラ準備 NG", String(frameW) + "x" + String(frameH));
}

void camStart() {
  if (!camReady) {
    logf("#CAM start ready=0");
    return;
  }
  if (camStarted) return;
  uint32_t t0 = millis();
  camStarted = capture.startCapture();
  camStartAt = millis();
  logf("#CAM start=%d ms=%lu", (int)camStarted, (unsigned long)(millis() - t0));
}

void camStop() {
  if (camStarted) {
    capture.stopCapture();
    camStarted = false;
  }
  logf("#CAM stop");
}

uint16_t pixelAt(const uint8_t* p) {
  return swapBytes ? (uint16_t)((p[0] << 8) | p[1]) : (uint16_t)(p[0] | (p[1] << 8));
}

// frameBuf の RGB 平均 (0〜255) を lastR/G/B に入れ、明るさの目安 (0〜255) を返す
uint32_t measureFrame() {
  uint64_t sr = 0;
  uint64_t sg = 0;
  uint64_t sb = 0;
  uint32_t count = 0;
  for (size_t i = 0; i + 1 < frameLen; i += 64) {
    uint16_t px = pixelAt(frameBuf + i);
    sr += (px >> 11) & 0x1F;
    sg += (px >> 5) & 0x3F;
    sb += px & 0x1F;
    count++;
  }
  if (count == 0) return 0;
  lastR = (uint32_t)(sr * 255 / (31ULL * count));
  lastG = (uint32_t)(sg * 255 / (63ULL * count));
  lastB = (uint32_t)(sb * 255 / (31ULL * count));
  return (lastR * 299 + lastG * 587 + lastB * 114) / 1000;
}

// 1 枚読んで frameBuf に写す
bool grabFrame(uint32_t* captureMs, uint32_t* luma) {
  uint32_t t0 = millis();
  ESPVideoBufferClass buf = capture.captureBuffer();
  *captureMs = millis() - t0;
  if (!buf.valid()) return false;
  size_t n = buf.size();
  if (n > frameCap) n = frameCap;
  memcpy(frameBuf, buf.data(), n);
  frameLen = n;
  *luma = measureFrame();
  return true;
}

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r * 31 / 255) << 11) | ((g * 63 / 255) << 5) | (b * 31 / 255));
}

// 回転前の画像を横に 4 等分して塗る (rot 90 なら画面の上からこの順になる)
//   通常: 赤・緑・青・白 / gray: 25%・50%・75% の灰色と、直近のカメラの平均色
void fillPattern(bool gray) {
  uint16_t colors[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
  if (gray) {
    colors[0] = rgb565(64, 64, 64);
    colors[1] = rgb565(128, 128, 128);
    colors[2] = rgb565(192, 192, 192);
    colors[3] = rgb565((uint8_t)lastR, (uint8_t)lastG, (uint8_t)lastB);
  }
  logf("#PATTERN colors=%04X,%04X,%04X,%04X", colors[0], colors[1], colors[2], colors[3]);
  for (uint32_t y = 0; y < frameH; y++) {
    uint8_t* row = frameBuf + y * frameW * 2;
    for (uint32_t x = 0; x < frameW; x++) {
      uint16_t c = colors[(x * 4) / frameW];
      row[x * 2] = (uint8_t)(c & 0xFF);
      row[x * 2 + 1] = (uint8_t)(c >> 8);
    }
  }
  frameLen = (size_t)frameW * frameH * 2;
}

void rotatedSize(uint32_t* rw, uint32_t* rh) {
  bool swapWH = (rotDeg == 90 || rotDeg == 270);
  *rw = swapWH ? frameH : frameW;
  *rh = swapWH ? frameW : frameH;
}

void rotateSoft(uint32_t rw, uint32_t rh) {
  const uint32_t w = frameW;
  const uint32_t h = frameH;
  const uint8_t* src = frameBuf;
  uint8_t* dst = rotBuf;
  switch (rotDeg) {
    case 90:  // 回転後の (x, y) = 回転前の (y, h - 1 - x)
      for (uint32_t y = 0; y < w; y++) {
        const uint8_t* s = src + ((h - 1) * w + y) * 2;
        for (uint32_t x = 0; x < h; x++) {
          *dst++ = s[0];
          *dst++ = s[1];
          s -= w * 2;
        }
      }
      break;
    case 270:  // 回転後の (x, y) = 回転前の (w - 1 - y, x)
      for (uint32_t y = 0; y < w; y++) {
        const uint8_t* s = src + (w - 1 - y) * 2;
        for (uint32_t x = 0; x < h; x++) {
          *dst++ = s[0];
          *dst++ = s[1];
          s += w * 2;
        }
      }
      break;
    case 180: {
      const uint8_t* s = src + (w * h - 1) * 2;
      for (uint32_t i = 0; i < w * h; i++) {
        *dst++ = s[0];
        *dst++ = s[1];
        s -= 2;
      }
      break;
    }
    default:
      memcpy(dst, src, (size_t)w * h * 2);
      break;
  }
  if (swapBytes) {
    for (size_t i = 0; i + 1 < (size_t)rw * rh * 2; i += 2) {
      uint8_t t = rotBuf[i];
      rotBuf[i] = rotBuf[i + 1];
      rotBuf[i + 1] = t;
    }
  }
}

// frameBuf を rotDeg だけ時計回りに回して rotBuf に入れる。PPA が使えればハードウェアで回す
void rotateFrame(uint32_t* rw, uint32_t* rh) {
  rotatedSize(rw, rh);
  if (useHwRot && ppaSrm != nullptr) {
    ppa_srm_oper_config_t op = {};
    op.in.buffer = frameBuf;
    op.in.pic_w = frameW;
    op.in.pic_h = frameH;
    op.in.block_w = frameW;
    op.in.block_h = frameH;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = rotBuf;
    op.out.buffer_size = (uint32_t)rotCap;
    op.out.pic_w = *rw;
    op.out.pic_h = *rh;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    // PPA は反時計回り。時計回り 90 = 反時計回り 270
    switch (rotDeg) {
      case 90:
        op.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
        break;
      case 180:
        op.rotation_angle = PPA_SRM_ROTATION_ANGLE_180;
        break;
      case 270:
        op.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
        break;
      default:
        op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
        break;
    }
    op.scale_x = 1.0f;
    op.scale_y = 1.0f;
    op.byte_swap = swapBytes;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    esp_err_t err = ppa_do_scale_rotate_mirror(ppaSrm, &op);
    if (err == ESP_OK) return;
    logf("#ROT hw=%s (ソフトで回す)", esp_err_to_name(err));
  }
  rotateSoft(*rw, *rh);
}

// 回転してから半分の大きさで画面に出す
void drawPreview() {
  uint32_t rw = 0;
  uint32_t rh = 0;
  uint32_t t0 = millis();
  rotateFrame(&rw, &rh);
  uint32_t rotMs = millis() - t0;
  uint32_t dw = rw / 2;
  uint32_t dh = rh / 2;
  if (previewBuf == nullptr || previewCap < dw * dh) {
    free(previewBuf);
    previewBuf = (uint16_t*)ps_malloc(dw * dh * sizeof(uint16_t));
    previewCap = previewBuf ? dw * dh : 0;
    if (previewBuf == nullptr) return;
  }
  for (uint32_t y = 0; y < dh; y++) {
    const uint8_t* s = rotBuf + (y * 2) * rw * 2;
    uint16_t* d = previewBuf + y * dw;
    for (uint32_t x = 0; x < dw; x++) {
      d[x] = (uint16_t)(s[0] | (s[1] << 8));
      s += 4;
    }
  }
  auto& d = M5.Display;
  int px = ((int)d.width() - (int)dw) / 2;
  d.startWrite();
  d.fillRect(0, PREVIEW_Y, d.width(), d.height() - PREVIEW_Y, TFT_BLACK);
  d.pushImage(px, PREVIEW_Y, (int32_t)dw, (int32_t)dh, (const lgfx::rgb565_t*)previewBuf);
  d.endWrite();
  logf("#DRAW rot_ms=%lu hwrot=%d total_ms=%lu", (unsigned long)rotMs, (int)useHwRot, (unsigned long)(millis() - t0));
}

void previewOnce() {
  if (!camStarted) camStart();
  if (!camStarted) return;
  uint32_t capMs = 0;
  uint32_t luma = 0;
  if (!grabFrame(&capMs, &luma)) {
    logf("#PREVIEW grab=0");
    return;
  }
  drawPreview();
  drawHeader("カメラの 1 枚 (画面に直接)", "rgb=" + String(lastR) + "," + String(lastG) + "," + String(lastB));
  logf("#PREVIEW cap_ms=%lu luma=%lu rgb=%lu,%lu,%lu", (unsigned long)capMs, (unsigned long)luma, (unsigned long)lastR,
       (unsigned long)lastG, (unsigned long)lastB);
}

// 取り込みを始め直して、最初のフレームまでと明るさ・色が落ち着くまでを見る
void aeCheck(int frames) {
  if (!camReady) {
    logf("#AE ready=0");
    return;
  }
  camStop();
  camStart();
  if (!camStarted) return;
  for (int i = 0; i < frames; i++) {
    uint32_t capMs = 0;
    uint32_t luma = 0;
    if (!grabFrame(&capMs, &luma)) {
      logf("#AE i=%d grab=0", i);
      continue;
    }
    logf("#AE i=%d since_start_ms=%lu cap_ms=%lu luma=%lu rgb=%lu,%lu,%lu", i, (unsigned long)(millis() - camStartAt),
         (unsigned long)capMs, (unsigned long)luma, (unsigned long)lastR, (unsigned long)lastG, (unsigned long)lastB);
  }
  drawPreview();
}

// ---------------------------------------------------------------- ISP のホワイトバランス

bool ispOpen() {
  if (ispFd >= 0) return true;
  ispFd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDONLY);
  logf("#ISP open fd=%d errno=%d", ispFd, ispFd < 0 ? errno : 0);
  return ispFd >= 0;
}

void setAwb(bool on) {
  if (!ispOpen()) return;
  struct v4l2_ext_control c = {};
  struct v4l2_ext_controls cs = {};
  cs.ctrl_class = V4L2_CTRL_CLASS_USER;
  cs.count = 1;
  cs.controls = &c;
  c.id = V4L2_CID_AUTO_WHITE_BALANCE;
  c.value = on ? 1 : 0;
  int r = ioctl(ispFd, VIDIOC_S_EXT_CTRLS, &cs);
  logf("#ISP awb=%d ret=%d errno=%d", (int)on, r, r < 0 ? errno : 0);
}

void setWb(float red, float blue) {
  if (!ispOpen()) return;
  esp_video_isp_wb_t wb = {};
  wb.enable = true;
  wb.red_gain = red;
  wb.blue_gain = blue;
  struct v4l2_ext_control c = {};
  struct v4l2_ext_controls cs = {};
  cs.ctrl_class = V4L2_CTRL_CLASS_USER;
  cs.count = 1;
  cs.controls = &c;
  c.id = V4L2_CID_USER_ESP_ISP_WB;
  c.size = sizeof(wb);
  c.ptr = &wb;
  int r = ioctl(ispFd, VIDIOC_S_EXT_CTRLS, &cs);
  logf("#ISP wb red=%.2f blue=%.2f ret=%d errno=%d", red, blue, r, r < 0 ? errno : 0);
}

void getWb() {
  if (!ispOpen()) return;
  esp_video_isp_wb_t wb = {};
  struct v4l2_ext_control c = {};
  struct v4l2_ext_controls cs = {};
  cs.ctrl_class = V4L2_CTRL_CLASS_USER;
  cs.count = 1;
  cs.controls = &c;
  c.id = V4L2_CID_USER_ESP_ISP_WB;
  c.size = sizeof(wb);
  c.ptr = &wb;
  int r = ioctl(ispFd, VIDIOC_G_EXT_CTRLS, &cs);
  logf("#ISP wbget ret=%d errno=%d enable=%d red=%.3f blue=%.3f", r, r < 0 ? errno : 0, (int)wb.enable, wb.red_gain, wb.blue_gain);
}

// ---------------------------------------------------------------- JPEG と microSD

void sdMount() {
  if (sdMounted) return;
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdMounted = SD.begin(SD_CS, SPI, SD_FREQ);
  logf("#SD mount=%d", (int)sdMounted);
}

// frameBuf を回して JPEG にし、microSD に保存する
void encodeAndSave(int quality, const char* prefix) {
  uint32_t t0 = millis();
  uint32_t rw = 0;
  uint32_t rh = 0;
  rotateFrame(&rw, &rh);
  uint32_t rotMs = millis() - t0;

  jpeg_encode_cfg_t cfg;
  cfg.height = rh;
  cfg.width = rw;
  cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
  cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
  cfg.image_quality = (uint32_t)quality;
  cfg.pixel_reverse = jpegReverse;
  uint32_t outLen = 0;
  t0 = millis();
  esp_err_t err = jpeg_encoder_process(jpegEnc, &cfg, rotBuf, rw * rh * 2, jpegBuf, (uint32_t)jpegCap, &outLen);
  uint32_t encMs = millis() - t0;
  if (err != ESP_OK) {
    logf("#JPEG err=%s", esp_err_to_name(err));
    return;
  }

  sdMount();
  if (!sdMounted) {
    logf("#JPEG sd=0 bytes=%lu", (unsigned long)outLen);
    return;
  }
  SD.mkdir("/ToolCheck");
  SD.mkdir("/ToolCheck/photos");
  char path[64];
  snprintf(path, sizeof(path), "/ToolCheck/photos/%s-%lu.jpg", prefix, (unsigned long)millis());
  t0 = millis();
  File f = SD.open(path, FILE_WRITE);
  size_t written = f ? f.write(jpegBuf, outLen) : 0;
  if (f) f.close();
  uint32_t sdMs = millis() - t0;
  lastPhoto = path;
  logf("#JPEG path=%s w=%lu h=%lu q=%d rot_ms=%lu hwrot=%d jpeg_ms=%lu bytes=%lu sd_ms=%lu written=%u jrev=%d", path,
       (unsigned long)rw, (unsigned long)rh, quality, (unsigned long)rotMs, (int)useHwRot, (unsigned long)encMs,
       (unsigned long)outLen, (unsigned long)sdMs, (unsigned)written, (int)jpegReverse);
}

void shot(int quality) {
  if (!camStarted) camStart();
  if (!camStarted) return;
  uint32_t capMs = 0;
  uint32_t luma = 0;
  if (!grabFrame(&capMs, &luma)) {
    logf("#SHOT grab=0");
    return;
  }
  encodeAndSave(quality, "test");
}

void sdView() {
  sdMount();
  if (!sdMounted || lastPhoto.length() == 0) {
    logf("#SDVIEW photo=none");
    return;
  }
  auto& d = M5.Display;
  uint32_t t0 = millis();
  d.fillRect(0, PREVIEW_Y, d.width(), d.height() - PREVIEW_Y, TFT_BLACK);
  bool ok = d.drawJpgFile(SD, lastPhoto.c_str(), 180, PREVIEW_Y, 360, 640, 0, 0, 0.5f);
  logf("#SDVIEW ok=%d ms=%lu path=%s", (int)ok, (unsigned long)(millis() - t0), lastPhoto.c_str());
}

// ---------------------------------------------------------------- 画面の転送・その他

void sendScreenshot() {
  size_t len = 0;
  uint8_t* png = (uint8_t*)M5.Display.createPng(&len, 0, 0, M5.Display.width(), M5.Display.height());
  if (png == nullptr || len == 0) {
    Serial.println("#ERR screenshot createPng failed");
    return;
  }
  Serial.printf("#BEGIN png %u\n", (unsigned)len);
  static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char out[80];
  for (size_t i = 0; i < len; i += 57) {
    size_t n = (len - i < 57) ? (len - i) : 57;
    size_t o = 0;
    for (size_t j = 0; j < n; j += 3) {
      uint32_t v = (uint32_t)png[i + j] << 16;
      if (j + 1 < n) v |= (uint32_t)png[i + j + 1] << 8;
      if (j + 2 < n) v |= (uint32_t)png[i + j + 2];
      out[o++] = B64[(v >> 18) & 63];
      out[o++] = B64[(v >> 12) & 63];
      out[o++] = (j + 1 < n) ? B64[(v >> 6) & 63] : '=';
      out[o++] = (j + 2 < n) ? B64[v & 63] : '=';
    }
    out[o] = 0;
    Serial.println(out);
  }
  Serial.println("#END png");
  free(png);
}

void printIo() {
  uint8_t dir0 = M5.In_I2C.readRegister8(IOE0_ADDR, 0x03, 400000);
  uint8_t out0 = M5.In_I2C.readRegister8(IOE0_ADDR, 0x05, 400000);
  uint8_t dir1 = M5.In_I2C.readRegister8(IOE1_ADDR, 0x03, 400000);
  uint8_t out1 = M5.In_I2C.readRegister8(IOE1_ADDR, 0x05, 400000);
  logf("#IO 0x43 dir=0x%02X out=0x%02X / 0x44 dir=0x%02X out=0x%02X", dir0, out0, dir1, out1);
}

void printInfo() {
  logf("#INFO chip=%s rev=%d cpu=%lu heap=%lu psram=%lu cam_ready=%d started=%d w=%lu h=%lu rot=%d swap=%d jrev=%d hwrot=%d upd=%d sd=%d",
       ESP.getChipModel(), (int)ESP.getChipRevision(), (unsigned long)getCpuFrequencyMhz(), (unsigned long)ESP.getFreeHeap(),
       (unsigned long)ESP.getFreePsram(), (int)camReady, (int)camStarted, (unsigned long)frameW, (unsigned long)frameH, rotDeg,
       (int)swapBytes, (int)jpegReverse, (int)useHwRot, (int)callUpdate, (int)sdMounted);
}

// ---------------------------------------------------------------- M3 の残り: センサの電源・電流・撮影までの待ち時間

bool     streamGrab    = false;  // 取り込み中の電流を測る間、フレームを読み捨て続ける
uint32_t pwrOnAt       = 0;      // 最後にセンサの電源を入れた時刻 (0 = このスケッチでは入れ直していない)
int      camSeqStep    = 0;      // 電流の手順 (0 = していない)
uint32_t camSeqUntil   = 0;
uint32_t camSeqStepMs  = 30000;
int      camSeqLastSec = -1;

static const int CAMSEQ_LAST = 6;
static const char* CAMSEQ_LABELS[CAMSEQ_LAST + 1] = {
  "",
  "カメラ未初期化 (センサ電源 ON)",
  "初期化して取り込み停止",
  "取り込み中 (フレームを読み続ける)",
  "取り込みを止める",
  "カメラを閉じてセンサ電源 OFF",
  "センサ電源 ON に戻す",
};

// 0x43 の bit6 = カメラの電源 (esp-bsp の m5stack_tab5)。M5Unified がアンプ (bit1) を入切するのと同じ書き方
void camPower(bool on) {
  if (on) {
    M5.In_I2C.bitOn(IOE0_ADDR, 0x05, 0b01000000, 400000);
    pwrOnAt = millis();
  } else {
    M5.In_I2C.bitOff(IOE0_ADDR, 0x05, 0b01000000, 400000);
  }
  logf("#CAM pwr=%d", (int)on);
  printIo();
}

// 取り込みを止めてカメラを閉じる。電源を切った後は camBegin からやり直す
void camEnd() {
  streamGrab = false;
  liveView = false;
  camStop();
  // ISP のデバイスを開いたまま video.end() を呼ぶと「video20 is opened / Failed to deinitialize ISP video device」で後始末に失敗する
  if (ispFd >= 0) {
    close(ispFd);
    ispFd = -1;
  }
  if (capture.isOpened()) capture.end();
  if (video.isActive()) video.end();
  if (jpegEnc != nullptr) {
    jpeg_del_encoder_engine(jpegEnc);
    jpegEnc = nullptr;
  }
  if (ppaSrm != nullptr) {
    ppa_unregister_client(ppaSrm);
    ppaSrm = nullptr;
  }
  free(frameBuf);
  frameBuf = nullptr;
  frameCap = 0;
  frameLen = 0;
  free(rotBuf);
  rotBuf = nullptr;
  rotCap = 0;
  free(jpegBuf);
  jpegBuf = nullptr;
  jpegCap = 0;
  camReady = false;
  logf("#CAM end");
}

bool readWbGains(float* red, float* blue) {
  if (!ispOpen()) return false;
  esp_video_isp_wb_t wb = {};
  struct v4l2_ext_control c = {};
  struct v4l2_ext_controls cs = {};
  cs.ctrl_class = V4L2_CTRL_CLASS_USER;
  cs.count = 1;
  cs.controls = &c;
  c.id = V4L2_CID_USER_ESP_ISP_WB;
  c.size = sizeof(wb);
  c.ptr = &wb;
  if (ioctl(ispFd, VIDIOC_G_EXT_CTRLS, &cs) < 0) return false;
  *red = wb.red_gain;
  *blue = wb.blue_gain;
  return true;
}

// 取り込みを始めてから、明るさと AWB のゲインが落ち着くまでをフレームごとに出す
void settleCheck(int frames) {
  if (!camReady) {
    logf("#SETTLE ready=0");
    return;
  }
  camStop();
  camStart();
  if (!camStarted) return;
  for (int i = 0; i < frames; i++) {
    uint32_t capMs = 0;
    uint32_t luma = 0;
    if (!grabFrame(&capMs, &luma)) {
      logf("#SETTLE i=%d grab=0", i);
      continue;
    }
    float red = 0;
    float blue = 0;
    bool wb = readWbGains(&red, &blue);
    logf("#SETTLE i=%d since_start_ms=%lu since_pwr_ms=%lu cap_ms=%lu luma=%lu rgb=%lu,%lu,%lu wb=%d red=%.2f blue=%.2f", i,
         (unsigned long)(millis() - camStartAt), (unsigned long)(pwrOnAt ? millis() - pwrOnAt : 0), (unsigned long)capMs,
         (unsigned long)luma, (unsigned long)lastR, (unsigned long)lastG, (unsigned long)lastB, (int)wb, red, blue);
  }
  camStop();
  drawPreview();
}

// センサの電源を offMs 切ってから入れ直し、waitMs 待って初期化し、落ち着くまでを見る
void coldCheck(int offMs, int frames, int waitMs) {
  camEnd();
  camPower(false);
  delay((uint32_t)offMs);
  camPower(true);
  delay((uint32_t)waitMs);
  camBegin();
  logf("#COLD off_ms=%d wait_ms=%d ready=%d since_pwr_ms=%lu", offMs, waitMs, (int)camReady, (unsigned long)(millis() - pwrOnAt));
  if (camReady) settleCheck(frames);
}

void camSeqBegin(int step) {
  switch (step) {
    case 1:
      camEnd();
      camPower(true);
      break;
    case 2:
      camBegin();
      break;
    case 3:
      camStart();
      streamGrab = true;
      break;
    case 4:
      streamGrab = false;
      camStop();
      break;
    case 5:
      camEnd();
      camPower(false);
      break;
    case 6:
      camPower(true);
      break;
  }
  camSeqUntil = millis() + camSeqStepMs;
  camSeqLastSec = -1;
  logf("#CAMSEQ step=%d label=%s", step, CAMSEQ_LABELS[step]);
}

void camSeqUpdate() {
  if (camSeqStep == 0) return;
  int32_t left = (int32_t)(camSeqUntil - millis());
  if (left > 0) {
    int sec = (int)((left + 999) / 1000);
    if (sec != camSeqLastSec) {
      camSeqLastSec = sec;
      char title[64];
      snprintf(title, sizeof(title), "手順 %d / %d  残り %d 秒", camSeqStep, CAMSEQ_LAST, sec);
      drawHeader(title, CAMSEQ_LABELS[camSeqStep]);
    }
    return;
  }
  if (camSeqStep >= CAMSEQ_LAST) {
    camSeqStep = 0;
    drawHeader("電流 終わり", "手順ごとの値を教えてください");
    logf("#CAMSEQ done");
    return;
  }
  camSeqStep++;
  camSeqBegin(camSeqStep);
}

void handleCommand(const String& cmd) {
  Serial.printf("#CMD %s\n", cmd.c_str());
  if (cmd == "info") {
    printInfo();
  } else if (cmd == "cam pwr on" || cmd == "cam pwr off") {
    if (cmd == "cam pwr off") camEnd();  // 開いたまま電源を切らない
    camPower(cmd == "cam pwr on");
  } else if (cmd == "cam end") {
    camEnd();
  } else if (cmd == "settle" || cmd.startsWith("settle ")) {
    int n = (cmd.length() > 7) ? cmd.substring(7).toInt() : 30;
    settleCheck(n < 1 ? 1 : n);
  } else if (cmd == "cold" || cmd.startsWith("cold ")) {
    int offMs = 1000;
    int frames = 30;
    int waitMs = 0;
    sscanf(cmd.c_str() + 4, "%d %d %d", &offMs, &frames, &waitMs);
    coldCheck(constrain(offMs, 0, 10000), constrain(frames, 1, 100), constrain(waitMs, 0, 5000));
  } else if (cmd == "camseq" || cmd.startsWith("camseq ")) {
    int sec = 30;
    sscanf(cmd.c_str() + 6, "%d", &sec);
    camSeqStepMs = (uint32_t)constrain(sec, 5, 120) * 1000;
    camSeqStep = 1;
    camSeqBegin(camSeqStep);
  } else if (cmd == "io") {
    printIo();
  } else if (cmd == "cam begin") {
    camBegin();
  } else if (cmd == "cam start") {
    camStart();
  } else if (cmd == "cam stop") {
    camStop();
  } else if (cmd == "upd on" || cmd == "upd off") {
    callUpdate = (cmd == "upd on");
    logf("#OK upd=%d", (int)callUpdate);
  } else if (cmd == "errs") {
    logf("#ERRS sccb=%lu", (unsigned long)sccbErrors);
    sccbErrors = 0;
  } else if (cmd == "preview") {
    previewOnce();
  } else if (cmd == "jpgnow") {
    // 直近のフレーム (preview で画面に出したもの) をそのまま JPEG にして読み戻す
    if (!camReady || frameLen == 0) {
      logf("#JPGNOW frame=none");
      return;
    }
    encodeAndSave(80, "frame");
    sdView();
    drawHeader("同じ 1 枚 (JPEG を読み戻し)", "rgb=" + String(lastR) + "," + String(lastG) + "," + String(lastB));
  } else if (cmd == "ae" || cmd.startsWith("ae ")) {
    int n = (cmd.length() > 3) ? cmd.substring(3).toInt() : 40;
    aeCheck(n < 1 ? 1 : n);
  } else if (cmd == "live on" || cmd == "live off") {
    liveView = (cmd == "live on");
    if (liveView) camStart();
    logf("#OK live=%d", (int)liveView);
  } else if (cmd.startsWith("rot ")) {
    int r = cmd.substring(4).toInt();
    if (r == 0 || r == 90 || r == 180 || r == 270) rotDeg = r;
    logf("#OK rot=%d", rotDeg);
  } else if (cmd.startsWith("swap ")) {
    swapBytes = cmd.substring(5).toInt() != 0;
    logf("#OK swap=%d", (int)swapBytes);
  } else if (cmd.startsWith("jrev ")) {
    jpegReverse = cmd.substring(5).toInt() != 0;
    logf("#OK jrev=%d", (int)jpegReverse);
  } else if (cmd == "hwrot on" || cmd == "hwrot off") {
    useHwRot = (cmd == "hwrot on");
    logf("#OK hwrot=%d", (int)useHwRot);
  } else if (cmd == "pattern" || cmd == "pattern gray" || cmd == "pattern jpg" || cmd == "pattern gray jpg") {
    if (!camReady) {
      logf("#PATTERN ready=0");
      return;
    }
    bool gray = cmd.indexOf("gray") >= 0;
    bool viaJpeg = cmd.endsWith("jpg");
    liveView = false;
    fillPattern(gray);
    if (viaJpeg) {
      encodeAndSave(80, gray ? "gray" : "pattern");
      sdView();
    } else {
      drawPreview();
    }
    drawHeader(String(gray ? "灰色の帯" : "色の帯") + (viaJpeg ? " (JPEG を読み戻し)" : " (画面に直接)"),
               gray ? "上 3 本が灰色、4 本目はカメラの平均色" : "上から 赤・緑・青・白 なら正しい");
  } else if (cmd.startsWith("awb ")) {
    setAwb(cmd.substring(4).toInt() != 0);
  } else if (cmd == "wbget") {
    getWb();
  } else if (cmd.startsWith("wb ")) {
    float red = 1.0f;
    float blue = 1.0f;
    sscanf(cmd.c_str() + 3, "%f %f", &red, &blue);
    setWb(red, blue);
  } else if (cmd == "shot" || cmd.startsWith("shot ")) {
    int q = (cmd.length() > 5) ? cmd.substring(5).toInt() : 80;
    if (q < 1 || q > 100) q = 80;
    shot(q);
  } else if (cmd == "sd mount") {
    sdMount();
  } else if (cmd == "sdview") {
    sdView();
  } else if (cmd == "screenshot") {
    sendScreenshot();
  } else if (cmd == "log") {
    Serial.printf("#LOG begin count=%d\n", evLogCount);
    for (int i = 0; i < evLogCount; i++) Serial.printf("#LOG %s\n", evLog[i].c_str());
    Serial.println("#LOG end");
  } else if (cmd == "clear") {
    evLogCount = 0;
    Serial.println("#OK clear");
  } else {
    Serial.println("#ERR unknown command");
  }
}

void pollSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String cmd = lineBuf;
      lineBuf = "";
      cmd.trim();
      if (cmd.length()) handleCommand(cmd);
    } else if (lineBuf.length() < 120) {
      lineBuf += c;
    }
  }
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_imu = false;
  cfg.internal_mic = false;
  cfg.external_display_value = 0;
  M5.begin(cfg);
  Serial.begin(115200);
  origVprintf = esp_log_set_vprintf(countingVprintf);
  M5.Display.setTextWrap(false, false);  // M5.begin() の後でないと効かない
  M5.Display.setBrightness(153);
  M5.getIOExpander(1).digitalWrite(0, false);  // 無線モジュール C6 は使わない (M2 で -0.12A)
  M5.Display.fillScreen(TFT_BLACK);
  drawHeader("待機中", "cam begin → preview");
  printInfo();
  printIo();
  logf("#READY m3_camera");
}

void loop() {
  if (callUpdate) M5.update();
  pollSerial();
  if (streamGrab && camStarted) {
    uint32_t capMs = 0;
    uint32_t luma = 0;
    grabFrame(&capMs, &luma);
  }
  camSeqUpdate();
  if (liveView && camStarted && millis() - lastLive >= 300) {
    lastLive = millis();
    uint32_t capMs = 0;
    uint32_t luma = 0;
    if (grabFrame(&capMs, &luma)) drawPreview();
  }
  delay(2);
}
