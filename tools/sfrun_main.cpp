// sf100_run: carrega uma imagem plana (ex.: gerada por port/build.sh) em 0x1000 e a executa até HALT; imprime o console e o código de saída (D0).
//   sf100_run prog.bin [--base 0x1000] [--entry A] [--time segundos] [--trace-fault]
#include "soc.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace sf;

int main(int argc, char** argv) {
  std::string in; u32 base = abi::IPL_LOAD_ADDR, entry = 0; double secs = 20; bool have_entry = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i]; auto nx = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "faltou argumento de %s\n", a.c_str()); std::exit(2); } return argv[++i]; };
    if (a == "--base") base = (u32)std::strtoul(nx().c_str(), nullptr, 0); else if (a == "--entry") { entry = (u32)std::strtoul(nx().c_str(), nullptr, 0); have_entry = true; }
    else if (a == "--time") secs = std::atof(nx().c_str()); else in = a;
  }
  if (in.empty()) { std::fprintf(stderr, "uso: sf100_run prog.bin [--base A] [--entry A] [--time s]\n"); return 2; }
  std::ifstream f(in, std::ios::binary); if (!f) { std::fprintf(stderr, "não abri %s\n", in.c_str()); return 1; }
  std::vector<u8> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  Soc s; s.console.on_char = [](char c) { std::putchar(c); };
  s.load(base, img); s.cpu.reset_regs(); s.cpu.a[7] = abi::IPL_STACK; s.cpu.set_pc(have_entry ? entry : base);
  bool ok = s.run_until_halt((Ps)(secs * 1e12));
  s.bus.cache_flush_all(false); std::fflush(stdout);
  std::printf("[sf100_run] %s: pc=%08x d0=%d, %llu instruções, %.3f ms simulados, I-cache %llu/%llu\n", ok ? "HALT" : (s.cpu.halted ? "CPU parou (falha)" : "tempo esgotado"), s.cpu.pc, (int)s.cpu.d[0],
    (unsigned long long)s.cpu.instrs, s.sched.now / 1e9, (unsigned long long)s.bus.ihits, (unsigned long long)s.bus.imisses);
  if (!ok && s.cpu.halted) std::printf("[sf100_run] motivo do halt: %d\n", (int)s.cpu.reason);
  return ok ? (int)(s.cpu.d[0] & 0xFF) : 1;
}
