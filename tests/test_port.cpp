#include "helpers.h"
#include "sffs.h"
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

static std::string ioq3_dir() {
  if (const char* r = std::getenv("IOQ3")) return r;
  return "../Games-To-Port/ioq3";
}
static bool have_ioq3() { return std::system(("test -f '" + ioq3_dir() + "/code/qcommon/q_shared.c'").c_str()) == 0; }

// Código REAL do ioquake3 (q_shared.c + q_math.c) compilado pelo GCC para ColdFire roda no emulador e imprime exatamente o que o host x86 imprime.
TEST(ioq3_real_code_on_coldfire_matches_x86_host_output) {
  if (!have_cross() || !have_ioq3()) { std::printf("    (ignorado: sem toolchain m68k ou sem Games-To-Port/ioq3)\n"); return; }
  const char* out = "/tmp/sf100_q3smoke.bin";
  CHECK_EQ(std::system(("IOQ3='" + ioq3_dir() + "' bash port/ioq3/build_smoke.sh " + out + " >/dev/null 2>&1").c_str()), 0);
  std::ifstream f(out, std::ios::binary); CHECK(f.good());
  std::vector<u8> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  Soc s; s.load(IPL_LOAD_ADDR, img); s.cpu.reset_regs(); s.cpu.a[7] = IPL_STACK; s.cpu.set_pc(IPL_LOAD_ADDR);
  CHECK(s.run_until_halt(20ull * 1000000000000ull)); CHECK_EQ(s.cpu.d[0], 0u);
  std::ifstream e("port/ioq3/q3smoke.expected"); std::string expect((std::istreambuf_iterator<char>(e)), std::istreambuf_iterator<char>());
  CHECK(!expect.empty()); CHECK(s.console.out == expect);
}

// Compila os 186 fontes (qcommon, server, botlib, client, game, cgame, ui, zlib) para ColdFire. Lento (~2 min): só com SF_PORT_SLOW=1.
TEST(ioq3_all_modules_compile_for_coldfire) {
  if (!have_cross() || !have_ioq3() || !std::getenv("SF_PORT_SLOW")) { std::printf("    (ignorado: defina SF_PORT_SLOW=1 com toolchain e ioq3)\n"); return; }
  CHECK_EQ(std::system(("IOQ3='" + ioq3_dir() + "' bash port/ioq3/check.sh 2>&1 | tail -1 | grep -q 'com erro: 0'").c_str()), 0);
}

// libc ampliada (sscanf, strtod, setjmp, time, stdio em RAM e sobre o SnowFS do DVD, math): mesma saída do glibc no host, mais a leitura de arquivos do disco.
TEST(gcc_coldfire_libc_matches_host_and_reads_dvd_files) {
  if (!have_cross()) { std::printf("    (ignorado: sem m68k-linux-gnu-gcc-14)\n"); return; }
  const char* out = "/tmp/sf100_port_libc.bin";
  CHECK_EQ(std::system((std::string("bash port/build.sh ") + out + " port/hello/libc_test.c >/dev/null 2>&1").c_str()), 0);
  std::ifstream f(out, std::ios::binary); CHECK(f.good());
  std::vector<u8> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::string hello = "Ola do DVD SnowFS!\n"; std::vector<u8> big(6000); for (size_t i = 0; i < big.size(); i++) big[i] = (u8)(i * 29 + (i >> 8) * 3);
  std::vector<u8> disc; CHECK(sffs_build({{"sys/hello.txt", std::vector<u8>(hello.begin(), hello.end())}, {"data/big.bin", big}}, disc));
  Soc s; s.dvd.insert(disc); s.load(IPL_LOAD_ADDR, img); s.cpu.reset_regs(); s.cpu.a[7] = IPL_STACK; s.cpu.set_pc(IPL_LOAD_ADDR);
  CHECK(s.run_until_halt(30ull * 1000000000000ull)); CHECK_EQ(s.cpu.d[0], 0u);
  std::ifstream e("port/hello/libc_test.expected"); std::string expect((std::istreambuf_iterator<char>(e)), std::istreambuf_iterator<char>()); CHECK(!expect.empty());
  char line[160]; std::snprintf(line, sizeof line, "dvd: %zu bytes, size via fseek=%zu: %s", hello.size(), hello.size(), hello.c_str()); expect += line;
  unsigned sum = 0; for (int i = 0; i < 5000; i++) sum += big[7 + i] * (unsigned)(i + 1);
  std::snprintf(line, sizeof line, "dvd big: 5000 bytes sum=%u\n", sum); expect += line;
  CHECK(s.console.out == expect);
  if (s.console.out != expect) std::printf("--- obtido ---\n%s", s.console.out.c_str());
}
