// microSD (SPI: CS 42 / SCK 43 / MOSI 44 / MISO 39、20MHz) を専用のタスクで扱う (docs/設計.md「写真」。2026-09-13 決定)。
// SD に触るのはこのタスクだけ。M3 の実測で、抜けている間の確認は 1 回約 3 秒、カードが無いときのマウントは 921ms 止まるので、メインループでは回さない。
//   - 有無の確認: マウントしていなければ SD.begin を試す / マウント中は "/ToolCheck" の exists。失敗したら SD.end() して「SD なし」
//   - 保存: /ToolCheck/photos/<写真名>。書けなければ「SD なし」に落とす (写真は撮影タスクの直近の 1 枚に残る)
//   - 古い写真の削除: 名前順で新しい 100 枚を残し、0 バイトも消す (core/photo_store の selectPhotosToDelete)。いつ頼むかは App が決める (アイドル時)
//   - 写真の全削除: /ToolCheck/photos の中のファイルを全部消す (設定画面の「データ削除」。2026-09-14)。ほかのフォルダには触らない
// タスクからはシリアルに出さず、出来事はキューで返す (App が出す)。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "../core/loan_book.h"

namespace toolcheck {

enum class SdEventKind : uint8_t {
  Mounted,      // 挿さっていてマウントした (photos = 写真の枚数)
  Unmounted,    // 抜けた (確認か書込の失敗)
  Saved,        // 写真を保存した
  SaveFailed,   // 書けなかった (SD なしに落とした)
  SaveSkipped,  // SD が無いので保存しなかった
  Pruned,       // 古い写真を消した (removed = 消した枚数、photos = 残り)
  PruneFailed,  // 写真のフォルダを開けなかった
  ReadDone,     // 写真を読んだ (data は heap_caps_malloc の PSRAM。受け取った側が heap_caps_free する)
  ReadFailed,   // 写真が無い・読めない・大きすぎる
  PhotosDeleted,       // 写真を全部消した (removed = 消した枚数、photos = 消せずに残った枚数)
  PhotosDeleteFailed,  // SD が無い・写真のフォルダを開けなかった
};

struct SdEvent {
  SdEventKind kind = SdEventKind::Mounted;
  char name[kPhotoFieldLen] = {0};
  uint32_t ms = 0;
  uint32_t bytes = 0;
  uint16_t photos = 0;
  uint16_t removed = 0;
  uint8_t* data = nullptr;  // ReadDone のときだけ
};

constexpr size_t kPhotoReadMaxBytes = 1024 * 1024;  // 読む写真の上限

class SdTask {
 public:
  // タスクを作る。最初の確認はすぐに行う
  bool begin(uint32_t check_interval_ms);

  bool present() const;
  uint16_t photoCount() const;  // 最後に数えた枚数 (マウント・保存・削除で更新)
  void setCheckInterval(uint32_t ms);

  // data は heap_caps_malloc で確保したもの。受け取ったらこのタスクが解放する。キューが一杯なら false (呼び出し側が解放する)
  bool enqueueSave(const char* name, uint8_t* data, size_t len);
  bool requestPrune();
  bool requestCheck();
  // 写真を全部消す。結果は PhotosDeleted / PhotosDeleteFailed の出来事で返す
  bool requestDeleteAllPhotos();
  // /ToolCheck/photos/<name> を PSRAM に読む。結果は ReadDone / ReadFailed の出来事で返す
  bool requestRead(const char* name);

  // ReadDone の data は受け取った側が解放する
  bool pollEvent(SdEvent* out);

 private:
  struct Job;
  static void taskMain(void* arg);
  void run();
  void check();
  void save(const Job& job);
  void read(const Job& job);
  void prune();
  void deleteAllPhotos();
  void unmount();
  void publish(const SdEvent& ev);

  QueueHandle_t jobs_ = nullptr;
  QueueHandle_t events_ = nullptr;
  bool mounted_ = false;  // タスクの中だけで使う
  std::atomic<bool> present_{false};
  std::atomic<uint16_t> photos_{0};
  std::atomic<uint32_t> interval_ms_{5000};
};

}  // namespace toolcheck
