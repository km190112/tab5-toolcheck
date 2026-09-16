// 長さ固定のレコードを KvStore に回し書きする (返却履歴 100 件・開閉ログ 200 件)。
// Arduino 非依存。名前空間はこの入れ物だけで使う (clear は名前空間ごと消す)。
//
// 保存の形:
//   スロット  キー = 接頭字 + 容量に合わせた桁数の番号 ("H00"〜"H99" / "O000"〜"O199")
//             値   = [通し番号 u32][レコード]
//   meta      キー = "meta"、値 = [次に書くスロット u16][件数 u16][最後の通し番号 u32]
//
// 電源断: 追記は「スロットを書く → meta を書く」の順。meta の前で落ちたら、次の load() で
// 「次に書くスロットに最後の通し番号 + 1 のレコードがある」ことから書き終わっていたと判断して数に入れる
// (起動直後に NVS へ書かないため、load() は書き戻さない。meta は次の追記で直る)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "kv_store.h"

namespace toolcheck {

class RingStore {
 public:
  RingStore(KvStore& kv, const char* ns, char key_prefix, uint16_t capacity, size_t record_size);

  // 保存先から状態を読む。空なら 0 件で true。meta が壊れていたら 0 件にして false
  bool load();

  // レコードを 1 件足す (いっぱいなら最古を上書き)。書けなければ false (件数は変えない)
  bool append(const uint8_t* record);

  uint16_t count() const;
  uint16_t capacity() const;

  // 新しい方から index 番目 (0 = 最新) を読む。範囲外・壊れていたら false
  bool getNewest(uint16_t index, uint8_t* out) const;

  // 全部消す
  bool clear();

  // スロットのキー ("H07" / "O007")。桁数は capacity - 1 の桁数
  static bool makeKey(char prefix, uint16_t capacity, uint16_t slot, char* buf, size_t buf_len);

 private:
  KvStore& kv_;
  const char* ns_;
  char prefix_;
  uint16_t capacity_;
  size_t record_size_;
  uint16_t head_ = 0;
  uint16_t count_ = 0;
  uint32_t seq_ = 0;
};

}  // namespace toolcheck
