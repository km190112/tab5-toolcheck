// code_classifier: QR / バーコードの中身の判別 (docs/設計.md「QR の判別」)
#include <string>

#include "core/code_classifier.h"
#include "testing.h"

using toolcheck::ClassifiedCode;
using toolcheck::ClassifierConfig;
using toolcheck::classifyCode;
using toolcheck::CodeKind;
using toolcheck::InvalidReason;

static ClassifiedCode classify(const std::string& s, const ClassifierConfig& cfg) {
  return classifyCode(s.data(), s.size(), cfg);
}

static ClassifiedCode classify(const std::string& s) { return classify(s, ClassifierConfig{}); }

// --- 利用者 ID ---

TEST(classifier_seven_digits_is_user) {
  auto c = classify("1234567");
  CHECK_EQ(c.kind, CodeKind::User);
  CHECK_EQ(c.reason, InvalidReason::None);
  CHECK_EQ(std::string(c.text), std::string("1234567"));
  CHECK_EQ(c.length, size_t{7});
}

TEST(classifier_seven_chars_with_letter_is_item) {
  CHECK_EQ(classify("123456A").kind, CodeKind::Item);
  CHECK_EQ(classify("AB12345").kind, CodeKind::Item);
}

TEST(classifier_eight_digits_is_item) { CHECK_EQ(classify("12345678").kind, CodeKind::Item); }

TEST(classifier_six_digits_is_item) { CHECK_EQ(classify("123456").kind, CodeKind::Item); }

TEST(classifier_user_digits_follow_config) {
  ClassifierConfig cfg;
  cfg.user_id_digits = 6;
  CHECK_EQ(classify("123456", cfg).kind, CodeKind::User);
  CHECK_EQ(classify("1234567", cfg).kind, CodeKind::Item);
}

TEST(classifier_user_digits_zero_means_no_user) {
  ClassifierConfig cfg;
  cfg.user_id_digits = 0;
  CHECK_EQ(classify("1234567", cfg).kind, CodeKind::Item);
}

// --- 英字・記号も使う利用者 ID (2026-09-15: 名前を英字にして記号で桁埋めし、マスタ無しで誰か分かるようにする) ---

static ClassifierConfig alnum(uint8_t len) {
  ClassifierConfig cfg;
  cfg.user_id_digits = len;
  cfg.user_id_alnum = true;
  return cfg;
}

TEST(classifier_alnum_user_id_of_configured_length) {
  auto c = classify("TANAKA_", alnum(7));
  CHECK_EQ(c.kind, CodeKind::User);
  CHECK_EQ(c.reason, InvalidReason::None);
  CHECK_EQ(std::string(c.text), std::string("TANAKA_"));
  CHECK_EQ(classify("sato-k_", alnum(7)).kind, CodeKind::User);  // 小文字・ハイフン・アンダーバー
  CHECK_EQ(classify("1234567", alnum(7)).kind, CodeKind::User);  // 数字だけの名札もそのまま使える
  CHECK_EQ(classify("A.B/C#1", alnum(7)).kind, CodeKind::User);  // 物品番号と同じ文字を使える
}

TEST(classifier_alnum_other_lengths_are_items) {
  CHECK_EQ(classify("TANAKA", alnum(7)).kind, CodeKind::Item);
  CHECK_EQ(classify("TANAKA__", alnum(7)).kind, CodeKind::Item);
  CHECK_EQ(classify("ABCDEFGHIJKLMNO", alnum(15)).kind, CodeKind::User);
  CHECK_EQ(classify("ABCDEFGHIJKLMN", alnum(15)).kind, CodeKind::Item);
}

TEST(classifier_alnum_item_of_same_length_becomes_user) {
  // 英字・記号も使う設定では、その文字数の物品番号も利用者 ID として読む (数字だけの設定では物品番号のまま)
  CHECK_EQ(classify("TW-0251", alnum(7)).kind, CodeKind::User);
  CHECK_EQ(classify("TW-0251").kind, CodeKind::Item);
}

TEST(classifier_alnum_forbidden_chars_are_invalid) {
  CHECK_EQ(classify("TANA,A_", alnum(7)).reason, InvalidReason::ForbiddenChar);
  CHECK_EQ(classify("TANA A_", alnum(7)).reason, InvalidReason::ForbiddenChar);
  CHECK_EQ(classify("TANAKA\xE3", alnum(7)).reason, InvalidReason::ForbiddenChar);
}

TEST(classifier_alnum_trims_surrounding_whitespace) {
  auto c = classify(" TANAKA_\r\n", alnum(7));
  CHECK_EQ(c.kind, CodeKind::User);
  CHECK_EQ(std::string(c.text), std::string("TANAKA_"));
  CHECK_EQ(c.length, size_t{7});
}

TEST(classifier_digits_only_setting_keeps_letters_as_items) {
  const ClassifierConfig cfg;  // 既定は数字だけ
  CHECK(!cfg.user_id_alnum);
  CHECK_EQ(classify("TANAKA_", cfg).kind, CodeKind::Item);
}

TEST(classifier_alnum_zero_length_means_no_user) {
  CHECK_EQ(classify("TANAKA_", alnum(0)).kind, CodeKind::Item);
}

// --- 物品番号 ---

TEST(classifier_jan13_is_item) {
  auto c = classify("4901234567894");
  CHECK_EQ(c.kind, CodeKind::Item);
  CHECK_EQ(c.length, size_t{13});
}

TEST(classifier_two_chars_is_item) { CHECK_EQ(classify("AB").kind, CodeKind::Item); }

TEST(classifier_fifteen_chars_is_item) {
  auto c = classify("ABCDEFGHIJKLMNO");
  CHECK_EQ(c.kind, CodeKind::Item);
  CHECK_EQ(std::string(c.text), std::string("ABCDEFGHIJKLMNO"));
}

TEST(classifier_symbols_are_allowed) {
  auto c = classify("MM-100/A_1#");
  CHECK_EQ(c.kind, CodeKind::Item);
  CHECK_EQ(std::string(c.text), std::string("MM-100/A_1#"));
}

TEST(classifier_keeps_lowercase) {
  auto c = classify("tw-025");
  CHECK_EQ(c.kind, CodeKind::Item);
  CHECK_EQ(std::string(c.text), std::string("tw-025"));
}

TEST(classifier_trims_surrounding_whitespace_and_crlf) {
  auto c = classify(" \tTW-025\r\n");
  CHECK_EQ(c.kind, CodeKind::Item);
  CHECK_EQ(std::string(c.text), std::string("TW-025"));
  CHECK_EQ(c.length, size_t{6});
}

TEST(classifier_reads_only_raw_len) {
  const char buf[] = "1234567XYZ";
  auto c = classifyCode(buf, 7, ClassifierConfig{});
  CHECK_EQ(c.kind, CodeKind::User);
}

// --- 長さ ---

TEST(classifier_one_char_is_too_short) {
  auto c = classify("X");
  CHECK_EQ(c.kind, CodeKind::Invalid);
  CHECK_EQ(c.reason, InvalidReason::TooShort);
}

TEST(classifier_sixteen_chars_is_too_long) {
  auto c = classify("ABCDEFGHIJKLMNOP");
  CHECK_EQ(c.kind, CodeKind::Invalid);
  CHECK_EQ(c.reason, InvalidReason::TooLong);
  CHECK_EQ(c.length, size_t{16});
  CHECK_EQ(std::string(c.text), std::string("ABCDEFGHIJKLMNO"));  // 表示用に 15 文字で切る
}

TEST(classifier_item_max_len_above_limit_is_clamped) {
  ClassifierConfig cfg;
  cfg.item_max_len = 20;
  CHECK_EQ(classify("ABCDEFGHIJKLMNOP", cfg).reason, InvalidReason::TooLong);
}

TEST(classifier_item_max_len_follows_shorter_config) {
  ClassifierConfig cfg;
  cfg.item_max_len = 10;
  CHECK_EQ(classify("ABCDEFGHIJ", cfg).kind, CodeKind::Item);
  CHECK_EQ(classify("ABCDEFGHIJK", cfg).reason, InvalidReason::TooLong);
}

// --- 不正な文字 ---

TEST(classifier_comma_is_forbidden) {
  auto c = classify("T,01");
  CHECK_EQ(c.kind, CodeKind::Invalid);
  CHECK_EQ(c.reason, InvalidReason::ForbiddenChar);
}

TEST(classifier_inner_space_is_forbidden) {
  CHECK_EQ(classify("A B").reason, InvalidReason::ForbiddenChar);
}

TEST(classifier_inner_tab_is_forbidden) {
  CHECK_EQ(classify("AB\tCD").reason, InvalidReason::ForbiddenChar);
}

TEST(classifier_non_ascii_is_forbidden) {
  CHECK_EQ(classify("AB\xE3\x81\x82").reason, InvalidReason::ForbiddenChar);
}

TEST(classifier_del_is_forbidden) { CHECK_EQ(classify("AB\x7F").reason, InvalidReason::ForbiddenChar); }

TEST(classifier_embedded_nul_is_forbidden) {
  CHECK_EQ(classify(std::string("AB\0CD", 5)).reason, InvalidReason::ForbiddenChar);
}

// --- 空 ---

TEST(classifier_empty_is_invalid) {
  auto c = classify("");
  CHECK_EQ(c.kind, CodeKind::Invalid);
  CHECK_EQ(c.reason, InvalidReason::Empty);
}

TEST(classifier_whitespace_only_is_empty) { CHECK_EQ(classify("  \r\n").reason, InvalidReason::Empty); }

TEST(classifier_null_pointer_is_empty) {
  auto c = classifyCode(nullptr, 0, ClassifierConfig{});
  CHECK_EQ(c.kind, CodeKind::Invalid);
  CHECK_EQ(c.reason, InvalidReason::Empty);
}
