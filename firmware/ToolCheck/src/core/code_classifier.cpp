#include "code_classifier.h"

namespace toolcheck {
namespace {

// 前後から取り除く空白 (QR ユニットは末尾に CR/LF を付けることがある)
bool isTrimSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// 使ってよい文字: 印字可能 ASCII (0x21〜0x7E) のうち、カンマ以外。
// 空白・制御文字・DEL・非 ASCII は不可。カンマは CSV 出力で列がずれるので不可
bool isAllowedChar(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return u >= 0x21 && u <= 0x7E && c != ',';
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

ClassifiedCode invalid(ClassifiedCode out, InvalidReason reason) {
  out.kind = CodeKind::Invalid;
  out.reason = reason;
  return out;
}

}  // namespace

ClassifiedCode classifyCode(const char* raw, size_t raw_len, const ClassifierConfig& cfg) {
  ClassifiedCode out;
  if (raw == nullptr) raw_len = 0;

  size_t begin = 0;
  size_t end = raw_len;
  while (begin < end && isTrimSpace(raw[begin])) ++begin;
  while (end > begin && isTrimSpace(raw[end - 1])) --end;

  const size_t len = end - begin;
  const size_t limit = static_cast<size_t>(kItemMaxLenLimit);
  const size_t copy = (len < limit) ? len : limit;
  for (size_t i = 0; i < copy; ++i) out.text[i] = raw[begin + i];
  out.text[copy] = '\0';
  out.length = len;

  if (len == 0) return invalid(out, InvalidReason::Empty);

  bool allDigits = true;
  for (size_t i = begin; i < end; ++i) {
    if (!isAllowedChar(raw[i])) return invalid(out, InvalidReason::ForbiddenChar);
    if (!isDigit(raw[i])) allDigits = false;
  }

  // 利用者 ID: 設定した桁数ちょうどの数字だけ (英字・記号も使う設定なら、使える文字でその文字数ちょうど)。
  // 桁数が 0 か 15 超なら利用者 ID は無い
  const size_t userDigits = static_cast<size_t>(cfg.user_id_digits);
  if ((allDigits || cfg.user_id_alnum) && userDigits >= 1 && userDigits <= limit && len == userDigits) {
    out.kind = CodeKind::User;
    out.reason = InvalidReason::None;
    return out;
  }

  // 物品番号: 最長は設定値と NVS キーの上限 (15) の小さい方
  const size_t minLen = static_cast<size_t>(cfg.item_min_len);
  const size_t cfgMax = static_cast<size_t>(cfg.item_max_len);
  const size_t maxLen = (cfgMax < limit) ? cfgMax : limit;
  if (len < minLen) return invalid(out, InvalidReason::TooShort);
  if (len > maxLen) return invalid(out, InvalidReason::TooLong);

  out.kind = CodeKind::Item;
  out.reason = InvalidReason::None;
  return out;
}

}  // namespace toolcheck
