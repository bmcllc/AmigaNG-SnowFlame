// snowcc: compilador C -> ColdFire (SF-100).  snowcc prog.c [-o prog.bin] [-S prog.lst] [--map prog.map] [-I dir] [-Dnome[=valor]] [--base 0x1000] [--run]
#include "snowcc.h"
#include "soc.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace sf;

int main(int argc, char** argv) {
  CcOptions opt; std::string in, out, lst, map; bool run = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "faltou o argumento de %s\n", a.c_str()); std::exit(2); } return argv[++i]; };
    if (a == "-o") out = next(); else if (a == "-S") lst = next(); else if (a == "--map") map = next();
    else if (a == "--base") opt.base = (u32)std::strtoul(next().c_str(), nullptr, 0); else if (a == "--stack") opt.stack_top = (u32)std::strtoul(next().c_str(), nullptr, 0);
    else if (a == "-I") opt.include_dirs.push_back(next()); else if (a.rfind("-I", 0) == 0) opt.include_dirs.push_back(a.substr(2));
    else if (a.rfind("-D", 0) == 0) { std::string d = a.substr(2); size_t e = d.find('='); opt.defines[d.substr(0, e)] = e == std::string::npos ? "1" : d.substr(e + 1); }
    else if (a == "--run") run = true; else if (a == "--no-crt0") opt.crt0 = false;
    else if (a == "-h" || a == "--help") { std::puts("uso: snowcc prog.c [-o prog.bin] [-S prog.lst] [--map prog.map] [-I dir] [-Dnome[=valor]] [--base 0x1000] [--stack 0xF00000] [--run]"); return 0; }
    else if (in.empty()) in = a; else { std::fprintf(stderr, "argumento inesperado: %s\n", a.c_str()); return 2; }
  }
  if (in.empty()) { std::fprintf(stderr, "uso: snowcc prog.c [opções]\n"); return 2; }
  CcResult r = cc_compile_file(in, opt);
  if (!r.ok) { std::fprintf(stderr, "%s\n", r.error.c_str()); return 1; }
  if (!out.empty()) { std::ofstream f(out, std::ios::binary); f.write((const char*)r.image.data(), (std::streamsize)r.image.size()); }
  if (!lst.empty()) { std::ofstream f(lst); f << r.listing; }
  if (!map.empty()) { std::ofstream f(map); for (auto& kv : r.symbols) { char b[96]; std::snprintf(b, sizeof b, "%08x %s\n", kv.second, kv.first.c_str()); f << b; } }
  std::printf("%s: %zu bytes (código %u, dados %zu), entrada 0x%08x\n", in.c_str(), r.image.size(), r.code_end - r.base, r.image.size() - (r.code_end - r.base), r.entry);
  if (run) {
    Soc s; s.load(r.base, r.image); s.cpu.reset_regs(); s.cpu.a[7] = opt.stack_top; s.cpu.set_pc(r.entry);
    bool ok = s.run_until_halt(60ull * 1000000000000ull); s.bus.cache_flush_all(false);
    if (!s.console.out.empty()) std::printf("[console] %s%s", s.console.out.c_str(), s.console.out.back() == '\n' ? "" : "\n");
    std::printf("executou %llu instruções em %.3f ms simulados: %s; d0 = %d (0x%08x)\n", (unsigned long long)s.cpu.instrs, s.sched.now / 1e9, ok ? "HALT" : "não terminou", (int)s.cpu.d[0], s.cpu.d[0]);
    return ok ? 0 : 3;
  }
  return 0;
}
