// ホストテスト用の最小限の仕組み (外部依存なし)。MSVC でビルドする。
#pragma once

#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

namespace tt {

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline int& failures() {
  static int f = 0;
  return f;
}

inline void fail(const char* file, int line, const std::string& msg) {
  std::printf("  FAIL %s:%d: %s\n", file, line, msg.c_str());
  ++failures();
}

template <typename T>
std::string show(const T& v) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(v));
  } else if constexpr (std::is_same_v<T, bool>) {
    return v ? "true" : "false";
  } else if constexpr (std::is_arithmetic_v<T>) {
    return std::to_string(v);
  } else {
    return "<?>";
  }
}
inline std::string show(const std::string& s) { return "\"" + s + "\""; }
inline std::string show(const char* s) { return s ? "\"" + std::string(s) + "\"" : "null"; }

}  // namespace tt

#define TEST(name)                                   \
  static void name();                                \
  static tt::Registrar name##_registrar(#name, &name); \
  static void name()

#define CHECK(expr)                                                  \
  do {                                                               \
    if (!(expr)) tt::fail(__FILE__, __LINE__, "CHECK(" #expr ")");   \
  } while (0)

#define CHECK_EQ(a, b)                                                                  \
  do {                                                                                  \
    const auto& tt_a_ = (a);                                                            \
    const auto& tt_b_ = (b);                                                            \
    if (!(tt_a_ == tt_b_)) {                                                            \
      tt::fail(__FILE__, __LINE__,                                                      \
               std::string("CHECK_EQ(" #a ", " #b ") : ") + tt::show(tt_a_) + " != " + \
                   tt::show(tt_b_));                                                    \
    }                                                                                   \
  } while (0)
