// sf100_boot: liga um SF-100 sem janela: o IPL carrega boot/menu.bin do DVD e o sistema roda quadro a quadro.
//   sf100_boot disco.dvd [--frames N] [--out dir] [--shot F]... [--key F:nome]... [--pad F:botoes]... [--mc arquivo] [--log]
// Teclas: right left up down enter esc space (injeta evento HID no quadro F). --pad F:mask (BTN_*). --shot F grava PNG do quadro F.
#include "soc.h"
#include "sffs.h"
#include "util.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>

using namespace sf;
using namespace sf::abi;

int main(int argc, char** argv) {
  std::string dvd, outdir = "out/boot", mcfile; int frames = 120; bool log = false;
  std::vector<int> shots; std::vector<std::pair<int, u8>> keys; std::vector<std::pair<int, u32>> pads;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i]; auto nx = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "faltou argumento de %s\n", a.c_str()); std::exit(2); } return argv[++i]; };
    if (a == "--frames") frames = std::atoi(nx().c_str()); else if (a == "--out") outdir = nx(); else if (a == "--shot") shots.push_back(std::atoi(nx().c_str()));
    else if (a == "--mc") mcfile = nx(); else if (a == "--log") log = true;
    else if (a == "--key") { std::string s = nx(); size_t c = s.find(':'); int f = std::atoi(s.substr(0, c).c_str()); std::string n = s.substr(c + 1);
      u8 code = n == "right" ? 0x4F : n == "left" ? 0x50 : n == "up" ? 0x52 : n == "down" ? 0x51 : n == "enter" ? 0x28 : n == "esc" ? 0x29 : n == "space" ? 0x2C : 0; keys.push_back({f, code}); }
    else if (a == "--pad") { std::string s = nx(); size_t c = s.find(':'); pads.push_back({std::atoi(s.substr(0, c).c_str()), (u32)std::strtoul(s.substr(c + 1).c_str(), nullptr, 0)}); }
    else dvd = a;
  }
  if (dvd.empty()) { std::fprintf(stderr, "uso: sf100_boot disco.dvd [--frames N] [--out dir] [--shot F]... [--key F:nome]...\n"); return 2; }
  std::ifstream f(dvd, std::ios::binary); if (!f) { std::fprintf(stderr, "não abri %s\n", dvd.c_str()); return 1; }
  std::vector<u8> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (std::system(("mkdir -p '" + outdir + "'").c_str()) != 0) return 1;
  Soc s; s.dvd.insert(img); s.pads.pad[0].connected = true;
  if (!mcfile.empty()) { std::ifstream m(mcfile, std::ios::binary); if (m) { std::vector<u8> d((std::istreambuf_iterator<char>(m)), std::istreambuf_iterator<char>()); if (d.size() == s.mcard.data.size()) s.mcard.data = d; } }
  s.console.on_char = [&](char c) { if (log) { std::putchar(c); std::fflush(stdout); } };
  if (!s.boot_dvd()) { std::fprintf(stderr, "o disco não tem boot/menu.bin\n"); return 1; }
  s.video.start(); u32 held = 0;
  for (int fr = 0; fr < frames; fr++) {
    for (auto& k : keys) if (k.first == fr && k.second) { s.hid.key_event(k.second, true); s.hid.key_event(k.second, false); }
    for (auto& p : pads) if (p.first == fr) { held = p.second; s.pads.pad[0].buttons = held; }
    s.run_frames(1);
    if (s.cpu.halted && s.sched.empty()) { std::printf("CPU parou no quadro %d\n", fr); }
    for (int sh : shots) if (sh == fr) { char n[256]; std::snprintf(n, sizeof n, "%s/frame_%04d.png", outdir.c_str(), fr); write_png(n, s.video.width(), s.video.height(), s.video.frame); }
  }
  if (!mcfile.empty()) { std::ofstream m(mcfile, std::ios::binary); m.write((const char*)s.mcard.data.data(), (std::streamsize)s.mcard.data.size()); }
  std::printf("CPU: %llu instruções (%.1f M/quadro), %llu ciclos\n", (unsigned long long)s.cpu.instrs, s.cpu.instrs / 1e6 / frames, (unsigned long long)s.cpu.cycles);
  std::printf("%d quadros, %.2f s simulados, %u boots (último: %s), TLB: %llu faltas atendidas\n", frames, s.sched.now / 1e12, s.boots, s.last_boot.c_str(), (unsigned long long)s.bus.tlb_misses);
  if (!log) std::printf("%s", s.console.out.c_str());
  return 0;
}
