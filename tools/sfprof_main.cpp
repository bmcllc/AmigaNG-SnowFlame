// sfprof: executa um programa (C ou binário) e imprime o relatório do profiler.  sfprof prog.c [-n top] [--base A]
#include "snowcc.h"
#include "profiler.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace sf;

int main(int argc, char** argv) {
  std::string in; u32 base = 0x1000; int top = 12; CcOptions opt;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-n" && i + 1 < argc) top = std::atoi(argv[++i]); else if (a == "--base" && i + 1 < argc) base = (u32)std::strtoul(argv[++i], nullptr, 0);
    else if (a == "-I" && i + 1 < argc) opt.include_dirs.push_back(argv[++i]); else in = a;
  }
  if (in.empty()) { std::fprintf(stderr, "uso: sfprof prog.c [-n top] [--base A] [-I dir]\n"); return 2; }
  opt.base = base; CcResult r = cc_compile_file(in, opt); if (!r.ok) { std::fprintf(stderr, "%s\n", r.error.c_str()); return 1; }
  Soc s; s.load(r.base, r.image); s.cpu.reset_regs(); s.cpu.a[7] = opt.stack_top; s.cpu.set_pc(r.entry);
  Profiler p(s); for (auto& kv : r.symbols) if (kv.first[0] != '.' && kv.first != "_end") p.symbols[kv.first] = kv.second;
  p.start(); bool ok = s.run_until_halt(60ull * 1000000000000ull); p.stop();
  if (!s.console.out.empty()) std::printf("[console] %s\n", s.console.out.c_str());
  std::fputs(p.report(top).c_str(), stdout); std::printf("\nfim: %s, d0 = %d\n", ok ? "HALT" : "não terminou", (int)s.cpu.d[0]);
  return ok ? 0 : 3;
}
