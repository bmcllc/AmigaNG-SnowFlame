#include "framework.h"
#include <cstring>

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0, failed = 0;
  for (auto& t : registry()) {
    if (filter && !std::strstr(t.name, filter)) continue;
    failures_in_test() = 0; std::printf("[ RUN  ] %s\n", t.name); std::fflush(stdout);
    try { t.fn(); } catch (const std::exception& e) { report_fail("(exceção)", 0, e.what()); }
    ran++;
    if (failures_in_test()) { failed++; std::printf("[ FAIL ] %s\n", t.name); } else std::printf("[  OK  ] %s\n", t.name);
  }
  std::printf("\n%d/%d testes aprovados\n", ran - failed, ran);
  return failed ? 1 : 0;
}
