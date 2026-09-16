#include "sd_task.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp_heap_caps.h"
#include "../core/photo_store.h"

namespace toolcheck {
namespace {

constexpr int kSdCs = 42;
constexpr int kSdSck = 43;
constexpr int kSdMosi = 44;
constexpr int kSdMiso = 39;
constexpr uint32_t kSdFreq = 20000000;
const char* const kRootDir = "/ToolCheck";
const char* const kPhotoDir = "/ToolCheck/photos";
constexpr uint32_t kTaskStack = 6144;
constexpr UBaseType_t kTaskPriority = 1;
constexpr BaseType_t kTaskCore = 0;
// 写真の全削除で続けてこの回数消せなければ打ち切る (カードが抜けている間は 1 回に数秒かかる。M3 の実測)
constexpr uint8_t kDeleteGiveUpFailures = 3;

}  // namespace

struct SdTask::Job {
  enum class Kind : uint8_t { Save, Prune, Check, Read, DeleteAllPhotos };
  Kind kind;
  char name[kPhotoFieldLen];
  uint8_t* data;
  size_t len;
};

bool SdTask::begin(uint32_t check_interval_ms) {
  interval_ms_.store(check_interval_ms);
  jobs_ = xQueueCreate(4, sizeof(Job));
  events_ = xQueueCreate(8, sizeof(SdEvent));
  if (jobs_ == nullptr || events_ == nullptr) return false;
  return xTaskCreatePinnedToCore(&SdTask::taskMain, "sd", kTaskStack, this, kTaskPriority, nullptr, kTaskCore) == pdPASS;
}

bool SdTask::present() const { return present_.load(); }
uint16_t SdTask::photoCount() const { return photos_.load(); }
void SdTask::setCheckInterval(uint32_t ms) { interval_ms_.store(ms); }

bool SdTask::enqueueSave(const char* name, uint8_t* data, size_t len) {
  if (jobs_ == nullptr || data == nullptr) return false;
  Job job = {};
  job.kind = Job::Kind::Save;
  std::snprintf(job.name, sizeof(job.name), "%s", name != nullptr ? name : "");
  job.data = data;
  job.len = len;
  return xQueueSend(jobs_, &job, 0) == pdTRUE;
}

bool SdTask::requestPrune() {
  if (jobs_ == nullptr) return false;
  Job job = {};
  job.kind = Job::Kind::Prune;
  return xQueueSend(jobs_, &job, 0) == pdTRUE;
}

bool SdTask::requestCheck() {
  if (jobs_ == nullptr) return false;
  Job job = {};
  job.kind = Job::Kind::Check;
  return xQueueSend(jobs_, &job, 0) == pdTRUE;
}

bool SdTask::requestDeleteAllPhotos() {
  if (jobs_ == nullptr) return false;
  Job job = {};
  job.kind = Job::Kind::DeleteAllPhotos;
  return xQueueSend(jobs_, &job, 0) == pdTRUE;
}

bool SdTask::requestRead(const char* name) {
  if (jobs_ == nullptr || name == nullptr || name[0] == '\0') return false;
  Job job = {};
  job.kind = Job::Kind::Read;
  std::snprintf(job.name, sizeof(job.name), "%s", name);
  return xQueueSend(jobs_, &job, 0) == pdTRUE;
}

bool SdTask::pollEvent(SdEvent* out) { return events_ != nullptr && out != nullptr && xQueueReceive(events_, out, 0) == pdTRUE; }

void SdTask::taskMain(void* arg) { static_cast<SdTask*>(arg)->run(); }

void SdTask::run() {
  uint32_t next_check_ms = millis();
  for (;;) {
    const uint32_t now = millis();
    const int32_t wait = static_cast<int32_t>(next_check_ms - now);
    Job job;
    if (xQueueReceive(jobs_, &job, wait > 0 ? pdMS_TO_TICKS(wait) : 0) == pdTRUE) {
      switch (job.kind) {
        case Job::Kind::Save:
          save(job);
          break;
        case Job::Kind::Prune:
          prune();
          break;
        case Job::Kind::Check:
          check();
          next_check_ms = millis() + interval_ms_.load();
          break;
        case Job::Kind::Read:
          read(job);
          break;
        case Job::Kind::DeleteAllPhotos:
          deleteAllPhotos();
          break;
      }
    }
    if (static_cast<int32_t>(millis() - next_check_ms) >= 0) {
      check();
      next_check_ms = millis() + interval_ms_.load();
    }
  }
}

void SdTask::check() {
  const uint32_t t0 = millis();
  if (!mounted_) {
    SPI.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);
    if (!SD.begin(kSdCs, SPI, kSdFreq)) return;  // カードが無い (約 1 秒かかる)
    mounted_ = true;
    SD.mkdir(kRootDir);
    SD.mkdir(kPhotoDir);
    uint16_t count = 0;
    File dir = SD.open(kPhotoDir);
    if (dir && dir.isDirectory()) {
      File e = dir.openNextFile();
      while (e) {
        if (!e.isDirectory() && count < 65535) ++count;
        e.close();
        e = dir.openNextFile();
      }
    }
    if (dir) dir.close();
    photos_.store(count);
    present_.store(true);
    SdEvent ev;
    ev.kind = SdEventKind::Mounted;
    ev.ms = millis() - t0;
    ev.photos = count;
    publish(ev);
    return;
  }
  // SD.exists だけで確かめていたら、カードを約 10 秒抜いても true のままだった (2026-09-13 M5 3 段目)。
  // FatFs が読んだセクタを覚えていてカードに問い合わせないとみて、セクタ 0 を直接読む (書き込まないのでカードも減らない)
  uint8_t sector[512];
  if (!SD.readRAW(sector, 0)) {  // 抜けている間は 1〜3 秒かかる (M3)
    unmount();
    SdEvent ev;
    ev.kind = SdEventKind::Unmounted;
    ev.ms = millis() - t0;
    publish(ev);
  }
}

void SdTask::save(const Job& job) {
  SdEvent ev;
  std::snprintf(ev.name, sizeof(ev.name), "%s", job.name);
  ev.bytes = static_cast<uint32_t>(job.len);
  if (!mounted_) {
    heap_caps_free(job.data);
    ev.kind = SdEventKind::SaveSkipped;
    publish(ev);
    return;
  }
  char path[64];
  std::snprintf(path, sizeof(path), "%s/%s", kPhotoDir, job.name);
  const uint32_t t0 = millis();
  File f = SD.open(path, FILE_WRITE);
  const size_t written = f ? f.write(job.data, job.len) : 0;
  if (f) f.close();
  heap_caps_free(job.data);
  ev.ms = millis() - t0;
  if (written != job.len) {
    unmount();
    ev.kind = SdEventKind::SaveFailed;
    publish(ev);
    return;
  }
  const uint16_t count = photos_.load();
  if (count < 65535) photos_.store(static_cast<uint16_t>(count + 1));
  ev.kind = SdEventKind::Saved;
  ev.photos = photos_.load();
  publish(ev);
}

void SdTask::read(const Job& job) {
  SdEvent ev;
  ev.kind = SdEventKind::ReadFailed;
  std::snprintf(ev.name, sizeof(ev.name), "%s", job.name);
  if (!mounted_) {
    publish(ev);
    return;
  }
  char path[64];
  std::snprintf(path, sizeof(path), "%s/%s", kPhotoDir, job.name);
  const uint32_t t0 = millis();
  File f = SD.open(path, FILE_READ);
  const size_t size = f ? f.size() : 0;
  if (f && size > 0 && size <= kPhotoReadMaxBytes) {
    uint8_t* data = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (data != nullptr) {
      if (f.read(data, size) == size) {
        ev.kind = SdEventKind::ReadDone;
        ev.data = data;
      } else {
        heap_caps_free(data);
      }
    }
  }
  if (f) f.close();
  ev.ms = millis() - t0;
  ev.bytes = static_cast<uint32_t>(size);
  // 出来事のキューが一杯で渡せなかったら、ここで解放する
  if (xQueueSend(events_, &ev, 0) != pdTRUE && ev.data != nullptr) heap_caps_free(ev.data);
}

void SdTask::prune() {
  if (!mounted_) return;
  const uint32_t t0 = millis();
  File dir = SD.open(kPhotoDir);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    SdEvent ev;
    ev.kind = SdEventKind::PruneFailed;
    ev.ms = millis() - t0;
    publish(ev);
    return;
  }
  std::vector<std::string> names;
  std::vector<uint32_t> sizes;
  File e = dir.openNextFile();
  while (e) {
    if (!e.isDirectory()) {
      names.emplace_back(e.name());
      sizes.push_back(static_cast<uint32_t>(e.size()));
    }
    e.close();
    e = dir.openNextFile();
  }
  dir.close();
  std::vector<PhotoFile> files(names.size());
  for (size_t i = 0; i < names.size(); ++i) files[i] = PhotoFile{names[i].c_str(), sizes[i]};
  std::vector<size_t> doomed(names.size());
  const size_t n = selectPhotosToDelete(files.data(), files.size(), kPhotoKeepCount, doomed.data(), doomed.size());
  uint16_t removed = 0;
  char path[64];
  for (size_t i = 0; i < n; ++i) {
    std::snprintf(path, sizeof(path), "%s/%s", kPhotoDir, names[doomed[i]].c_str());
    if (SD.remove(path)) ++removed;
  }
  const size_t left = names.size() - removed;
  photos_.store(static_cast<uint16_t>(left < 65535 ? left : 65535));
  SdEvent ev;
  ev.kind = SdEventKind::Pruned;
  ev.ms = millis() - t0;
  ev.removed = removed;
  ev.photos = photos_.load();
  publish(ev);
}

// 写真のフォルダの中のファイルを全部消す (フォルダ自体と、ほかのフォルダには触らない)。1 枚あたり数十 ms (M3 の実測)
void SdTask::deleteAllPhotos() {
  const uint32_t t0 = millis();
  SdEvent ev;
  ev.kind = SdEventKind::PhotosDeleteFailed;
  if (!mounted_) {
    publish(ev);
    return;
  }
  File dir = SD.open(kPhotoDir);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    ev.ms = millis() - t0;
    ev.photos = photos_.load();
    publish(ev);
    return;
  }
  std::vector<std::string> names;
  File e = dir.openNextFile();
  while (e) {
    if (!e.isDirectory()) names.emplace_back(e.name());
    e.close();
    e = dir.openNextFile();
  }
  dir.close();
  size_t removed = 0;
  uint8_t failures = 0;
  char path[64];
  for (const std::string& name : names) {
    std::snprintf(path, sizeof(path), "%s/%s", kPhotoDir, name.c_str());
    if (SD.remove(path)) {
      ++removed;
      failures = 0;
    } else if (++failures >= kDeleteGiveUpFailures) {
      break;  // 抜けたとみて打ち切る (残りは消せなかった枚数に入る。抜けは次の確認で「SD なし」になる)
    }
  }
  const size_t left = names.size() - removed;
  photos_.store(static_cast<uint16_t>(left < 65535 ? left : 65535));
  ev.kind = SdEventKind::PhotosDeleted;
  ev.ms = millis() - t0;
  ev.removed = static_cast<uint16_t>(removed < 65535 ? removed : 65535);
  ev.photos = photos_.load();
  publish(ev);
}

void SdTask::unmount() {
  SD.end();
  mounted_ = false;
  present_.store(false);
}

void SdTask::publish(const SdEvent& ev) { xQueueSend(events_, &ev, 0); }

}  // namespace toolcheck
