// sfdbg: depurador interativo do SF-100.  sfdbg prog.c|prog.bin [--base 0x1000] [--map prog.map] [-x script]   (comandos: help)
#include "snowcc.h"
#include "debugger.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace sf;

int main(int argc, char** argv) {
  std::string in, script, mapf; u32 base = 0x1000, entry = 0; bool have_entry = false; CcOptions opt; std::vector<std::string> cmds;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "faltou o argumento de %s\n", a.c_str()); std::exit(2); } return argv[++i]; };
    if (a == "--base") base = (u32)std::strtoul(next().c_str(), nullptr, 0); else if (a == "--entry") { entry = (u32)std::strtoul(next().c_str(), nullptr, 0); have_entry = true; }
    else if (a == "--map") mapf = next(); else if (a == "-x") script = next(); else if (a == "-I") opt.include_dirs.push_back(next()); else if (a == "-c") cmds.push_back(next());
    else if (a == "-h" || a == "--help") { std::puts("uso: sfdbg prog.c|prog.bin [--base A] [--entry A] [--map arquivo] [-x script] [-c 'comando']..."); return 0; }
    else in = a;
  }
  if (in.empty()) { std::fprintf(stderr, "uso: sfdbg prog.c|prog.bin [opções]\n"); return 2; }
  Soc soc; Debugger dbg(soc); std::vector<u8> image;
  if (in.size() > 2 && in.substr(in.size() - 2) == ".c") {
    opt.base = base; CcResult r = cc_compile_file(in, opt); if (!r.ok) { std::fprintf(stderr, "%s\n", r.error.c_str()); return 1; }
    image = r.image; dbg.add_symbols(r.symbols); entry = r.entry; have_entry = true; base = r.base;
  } else { std::ifstream f(in, std::ios::binary); if (!f) { std::fprintf(stderr, "não abri %s\n", in.c_str()); return 1; } image.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
  if (!mapf.empty()) { std::ifstream m(mapf); std::string l; while (std::getline(m, l)) { std::istringstream ls(l); std::string a, n; if (ls >> a >> n) dbg.symbols[n] = (u32)std::strtoul(a.c_str(), nullptr, 16); } }
  soc.load(base, image); soc.cpu.reset_regs(); soc.cpu.a[7] = 0x00F00000; soc.cpu.set_pc(have_entry ? entry : base);
  std::printf("sfdbg: %zu bytes em 0x%08x, entrada 0x%08x (help para ajuda)\n", image.size(), base, soc.cpu.pc);
  size_t shown = 0;
  auto run = [&](const std::string& l) {
    std::string o = dbg.exec(l); std::fputs(o.c_str(), stdout);
    if (soc.console.out.size() > shown) { std::printf("[console] %s%s", soc.console.out.substr(shown).c_str(), soc.console.out.back() == '\n' ? "" : "\n"); shown = soc.console.out.size(); }
  };
  for (auto& c : cmds) { std::printf("(sfdbg) %s\n", c.c_str()); run(c); }
  std::istream* is = &std::cin; std::ifstream sf; if (!script.empty()) { sf.open(script); if (!sf) { std::fprintf(stderr, "não abri %s\n", script.c_str()); return 1; } is = &sf; }
  std::string line;
  while (true) {
    if (script.empty() && cmds.empty()) { std::printf("(sfdbg) "); std::fflush(stdout); }
    if (!std::getline(*is, line)) break;
    if (line == "quit" || line == "q") break; if (!script.empty()) std::printf("(sfdbg) %s\n", line.c_str());
    run(line);
  }
  return 0;
}
