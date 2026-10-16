#include "helpers.h"
#include <cstdlib>
#include <fstream>

// ======================= Port de software C real: GCC cross (ColdFire V4e) + runtime freestanding =======================
// Só roda se houver um m68k-linux-gnu-gcc-14 (PATH ou $SF_M68K_CROSS, veja docs/PORT.md); sem ele o teste é ignorado.
static bool have_cross() {
  if (const char* r = std::getenv("SF_M68K_CROSS")) return std::system((std::string("test -x '") + r + "/usr/bin/m68k-linux-gnu-gcc-14'").c_str()) == 0;
  return std::system("command -v m68k-linux-gnu-gcc-14 >/dev/null 2>&1") == 0;
}

TEST(gcc_coldfire_hello_runs_with_correct_fpu_int64_and_printf) {
  if (!have_cross()) { std::printf("    (ignorado: sem m68k-linux-gnu-gcc-14)\n"); return; }
  const char* out = "/tmp/sf100_port_hello.bin";
  CHECK_EQ(std::system((std::string("bash port/build.sh ") + out + " port/hello/hello.c >/dev/null 2>&1").c_str()), 0);
  std::ifstream f(out, std::ios::binary); CHECK(f.good());
  std::vector<u8> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  Soc s; s.load(IPL_LOAD_ADDR, img); s.cpu.reset_regs(); s.cpu.a[7] = IPL_STACK; s.cpu.set_pc(IPL_LOAD_ADDR);
  CHECK(s.run_until_halt(20ull * 1000000000000ull)); CHECK_EQ(s.cpu.d[0], 42u);
  const char* expect =
    "Ola, SnowFlame SF-100! (GCC, ColdFire V4e)\n"
    "sqrt(2)=1.414213562373  sin(1)=0.841470984808  cos(1)=0.540302305868  atan2(1,2)=0.463647609001\n"
    "exp(1.5)=4.4816890703  log(10)=2.3025850930  pow(2,0.5)=1.4142135624  pow(3,5)=243.0  floor(-2.5)=-3.0\n"
    "big=998244359987710471  big/12345=80862240582236  big%1000003=614682  ub/3=6148914691236517205  ub>>33=2147483647\n"
    "dot=3.000  float->int 3  0.0001234 1.23457e+08 1.234568e+04\n"
    "qsort: 0 13 16 19 22 35 38 41 54 57 60 76 79 82 95 98 \n"
    "malloc ok: 2 1\n"
    "   42|42   |00042|ff|0XFF|Z|     3.142|fim\n";
  CHECK(s.console.out == expect);
}
