// m68kxlat: traduz código 68000 para ColdFire.  m68kxlat in.bin -o out.bin [--base68 0x1000] [--base 0x1000] [--data ini-fim]... [--map arquivo]
#include "m68k2cf.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace sf;

int main(int argc, char** argv) {
  XlatOptions opt; std::string in, out, mapf;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "faltou o argumento de %s\n", a.c_str()); std::exit(2); } return argv[++i]; };
    if (a == "-o") out = next(); else if (a == "--base68") opt.base68 = (u32)std::strtoul(next().c_str(), nullptr, 0); else if (a == "--base") opt.base_cf = (u32)std::strtoul(next().c_str(), nullptr, 0);
    else if (a == "--map") mapf = next();
    else if (a == "--data") { std::string r = next(); size_t d = r.find('-'); if (d == std::string::npos) { std::fprintf(stderr, "--data ini-fim\n"); return 2; } opt.data_ranges.push_back({(u32)std::strtoul(r.substr(0, d).c_str(), nullptr, 0), (u32)std::strtoul(r.substr(d + 1).c_str(), nullptr, 0)}); }
    else if (a == "-h" || a == "--help") { std::puts("uso: m68kxlat in.bin -o out.bin [--base68 A] [--base A] [--data ini-fim]... [--map arquivo]"); return 0; }
    else in = a;
  }
  if (in.empty()) { std::fprintf(stderr, "uso: m68kxlat in.bin [opções]\n"); return 2; }
  std::ifstream f(in, std::ios::binary); if (!f) { std::fprintf(stderr, "não abri %s\n", in.c_str()); return 1; }
  std::vector<u8> code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  XlatResult r = translate_68k(code, opt);
  if (!r.ok) { std::fprintf(stderr, "erro: %s\n", r.error.c_str()); return 1; }
  if (!out.empty()) { std::ofstream o(out, std::ios::binary); o.write((const char*)r.code.data(), (std::streamsize)r.code.size()); }
  if (!mapf.empty()) { std::ofstream m(mapf); for (auto& kv : r.map) { char b[64]; std::snprintf(b, sizeof b, "%08x %08x\n", kv.first, kv.second); m << b; } }
  std::printf("%s: %zu bytes 68k -> %zu bytes ColdFire; %d instruções, %d expandidas\n", in.c_str(), code.size(), r.code.size(), r.source_insns, r.expanded);
  for (auto& n : r.notes) std::printf("aviso: %s\n", n.c_str());
  return 0;
}
