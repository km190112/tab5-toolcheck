// このソフトについて: 製品名・版数・リリース日・開発者・公開リポジトリ (設定画面「このソフトについて」とシリアル about)。
// 版数とリリース日は手で書く。版を上げるときに一緒に直す。
#pragma once

namespace toolcheck {
namespace about {

inline constexpr const char* kProductName = "ToolCheck";
inline constexpr const char* kFirmwareVersion = "0.6.4-m6.5";
inline constexpr const char* kReleaseDate = "2026-09-17";  // YYYY-MM-DD
inline constexpr const char* kDeveloper = "松岡 賢";
inline constexpr const char* kRepoUrl = "https://github.com/km190112/tab5-toolcheck";
inline constexpr const char* kLicenseName = "MIT";

}  // namespace about
}  // namespace toolcheck
