// ホストテストの入口。引数を渡すと、名前にその文字列を含むテストだけを実行する。
#include <cstring>

#include "testing.h"

int main(int argc, char** argv) {
  // 出力をバッファしない。テストが異常終了しても、どこまで進んだかが残る
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const char* filter = (argc > 1) ? argv[1] : "";
  int run = 0;
  for (const auto& t : tt::registry()) {
    if (filter[0] != '\0' && std::strstr(t.name, filter) == nullptr) continue;
    int before = tt::failures();
    t.fn();
    std::printf("%s %s\n", (tt::failures() == before) ? "PASS" : "FAIL", t.name);
    ++run;
  }
  std::printf("\n%d tests, %d failures\n", run, tt::failures());
  if (run == 0) {
    std::printf("no tests matched\n");
    return 2;
  }
  return tt::failures() == 0 ? 0 : 1;
}
