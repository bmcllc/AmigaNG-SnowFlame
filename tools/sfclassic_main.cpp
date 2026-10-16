// sfclassic: roda um programa 68000 no SnowOS Classic (CPU 68k + chipset Amiga) e grava os quadros em PPM.
//   sfclassic prog.bin [--base 0x1000] [--entry A] [--sp A] [--frames N] [--out dir] [--ntsc] [--68020] [--joy botoes]
#include "classic.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace sf;

int main(int argc, char** argv) {
  std::string in, outdir = "."; u32 base = 0x1000, entry = 0, sp = 0x70000; int frames = 5; bool have_entry = false, ntsc = false, m020 = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "faltou o argumento de %s\n", a.c_str()); std::exit(2); } return argv[++i]; };
    if (a == "--base") base = (u32)std::strtoul(next().c_str(), nullptr, 0); else if (a == "--entry") { entry = (u32)std::strtoul(next().c_str(), nullptr, 0); have_entry = true; }
    else if (a == "--sp") sp = (u32)std::strtoul(next().c_str(), nullptr, 0); else if (a == "--frames") frames = std::atoi(next().c_str()); else if (a == "--out") outdir = next();
    else if (a == "--ntsc") ntsc = true; else if (a == "--68020") m020 = true;
    else if (a == "-h" || a == "--help") { std::puts("uso: sfclassic prog.bin [--base A] [--entry A] [--sp A] [--frames N] [--out dir] [--ntsc] [--68020]"); return 0; }
    else in = a;
  }
  if (in.empty()) { std::fprintf(stderr, "uso: sfclassic prog.bin [opções]\n"); return 2; }
  std::ifstream f(in, std::ios::binary); if (!f) { std::fprintf(stderr, "não abri %s\n", in.c_str()); return 1; }
  std::vector<u8> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  Soc soc; ClassicSystem cs(soc); cs.chip.pal = !ntsc; cs.cpu.m68020 = m020; cs.load(base, img); cs.boot(have_entry ? entry : base, sp); cs.chip.start();
  int written = 0;
  cs.chip.on_frame = [&]() {
    char name[256]; std::snprintf(name, sizeof name, "%s/classic_%03d.ppm", outdir.c_str(), written++);
    std::ofstream o(name, std::ios::binary); o << "P6\n" << CLASSIC_FB_W << " " << CLASSIC_FB_H << "\n255\n";
    for (u32 y = 0; y < CLASSIC_FB_H; y++) for (u32 x = 0; x < CLASSIC_FB_W; x++) { u32 p = soc.bus.raw_r32(CLASSIC_FB_ADDR + y * CLASSIC_FB_PITCH + x * 4); u8 rgb[3] = {(u8)(p >> 16), (u8)(p >> 8), (u8)p}; o.write((const char*)rgb, 3); }
  };
  u64 n = cs.run((Ps)frames * cs.chip.frame_ps() + 1000000);
  std::printf("%llu instruções 68k em %.2f ms simulados; %d quadros gravados em %s\n", (unsigned long long)n, soc.sched.now / 1e9, written, outdir.c_str());
  return 0;
}
