// どの画面を出すかの状態 (docs/設計.md「画面」)。Arduino 非依存。描くのは ui/、ここは「何を出すか」だけを決める。
// 2026-09-13 決定: 長い一覧はページ送りのボタン / 名札だけの一覧は 10 秒 (タップでも) 閉じる / 放置したタブは 2 分で貸出中に戻る。
//
//   Tab       : 下のタブ (貸出中 / 返却履歴 / 開閉ログ / 検索) のどれかを出している
//   Session   : セッション中の全面表示。いちばん強く、始まったら名札の一覧と写真を閉じる
//   UserLoans : セッションの外で名札を読んだ人の貸出中一覧 (10 秒かタップで閉じる)
//   Photo     : 写真の全面表示 (タップで閉じる)
//   Settings  : 設定画面 (⚙ の長押しと PIN の後)。セッションが始まったら閉じる (未保存の変更は捨てる。計画「設定画面にいる間も監視は止めない」)
// 無操作 (タッチ・QR・開閉) が 2 分続いたら、貸出中の 1 ページ目に戻す (名札の一覧・写真・設定画面も閉じる)。セッション中は数えない。
#pragma once

#include <cstdint>

#include "loan_book.h"

namespace toolcheck {

enum class Tab : uint8_t { Loans, History, OpenLog, Search };
enum class View : uint8_t { Tab, Session, UserLoans, Photo, Settings };

constexpr uint32_t kUserLoansShowMs = 10000;  // 名札だけの一覧を出しておく時間
constexpr uint32_t kIdleReturnMs = 120000;    // 無操作でこれだけたったら貸出中の 1 ページ目に戻す

class ScreenState {
 public:
  explicit ScreenState(uint32_t now_ms);

  View view() const;
  Tab tab() const;
  uint16_t page() const;       // 0 始まり
  uint16_t pageCount() const;  // 1 以上
  const char* userLoansUser() const;  // UserLoans を出しているときの利用者 ID (それ以外は空文字列)
  const char* photoName() const;      // Photo を出しているときの写真名 (それ以外は空文字列)
  // 見た目が変わるたびに増える (描き直しの合図)。変わらない呼び出しでは増えない
  uint32_t revision() const;

  // どこかに触れた (無操作を数え直す)
  void onTouch(uint32_t now_ms);
  // タブを押した。そのタブの 1 ページ目にし、名札の一覧と写真を閉じる (いまのタブを押しても 1 ページ目に戻る)
  void selectTab(Tab tab, uint32_t now_ms);
  // ページ送り。端なら何もせず false
  bool nextPage(uint32_t now_ms);
  bool prevPage(uint32_t now_ms);
  // いまのタブの件数と 1 ページの行数 (0 は 1 とみなす)。ページが範囲外になったら最後のページに寄せる
  void setListSize(uint16_t items, uint16_t rows_per_page);

  // セッションの外で名札を読んだ。セッション中は無視する。別の人・同じ人でも、そこから 10 秒出す
  void showUserLoans(const char* user, uint32_t now_ms);
  // 写真を全面に出す。セッション中は無視する
  void openPhoto(const char* name, uint32_t now_ms);
  // 名札の一覧・写真をタップで閉じる (設定画面は閉じない。closeSettings で閉じる)
  void dismiss(uint32_t now_ms);

  // 設定画面を開く (名札の一覧と写真は閉じる)。セッション中は無視する
  void openSettings(uint32_t now_ms);
  // 設定画面を閉じてタブに戻る (タブとページはそのまま)
  void closeSettings(uint32_t now_ms);

  // セッションが始まった / 終わった (開始・終了の時点も操作として数える)
  void setSessionActive(bool active, uint32_t now_ms);

  // 名札の一覧の時間切れと、無操作での戻りを見る
  void tick(uint32_t now_ms);

 private:
  enum class Overlay : uint8_t { None, UserLoans, Photo, Settings };

  void touch(uint32_t now_ms);
  void setOverlay(Overlay overlay);

  Tab tab_ = Tab::Loans;
  uint16_t page_ = 0;
  uint16_t page_count_ = 1;
  Overlay overlay_ = Overlay::None;
  bool session_active_ = false;
  uint32_t last_activity_ms_ = 0;
  uint32_t user_shown_ms_ = 0;
  char user_[kCodeFieldLen] = {0};
  char photo_[kPhotoFieldLen] = {0};
  uint32_t revision_ = 0;
};

}  // namespace toolcheck
