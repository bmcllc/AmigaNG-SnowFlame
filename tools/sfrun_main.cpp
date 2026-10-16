// sf100_run: carrega uma imagem plana (ex.: gerada por port/build.sh) em 0x1000 e a executa até HALT; imprime o console e o código de saída (D0).
//   sf100_run prog.bin [--video] [--shot N:arquivo.png] [--base 0x1000] [--entry A] [--time segundos] [--dvd imagem.dvd] [--data caminho=arquivo]... [--cmdline "texto"]
//   --data monta um SnowFS em memória (vira o DVD); --cmdline grava um texto NUL-terminado em SRAM_BASE+0x200 (o ioq3 o usa como linha de comando).
#include "soc.h"
#include "sffs.h"
#include "util.h"
#include <cstdio>
#include <cstdlib>
#include "cfdis.h"
#include <fstream>
#include <map>
#include <algorithm>

using namespace sf;

int main(int argc, char** argv) {
  std::string in, dvd, cmdline; bool video = false; std::vector<std::pair<int, std::string>> shots; u32 wlo = 0, whi = 0; bool prof = false; std::vector<std::pair<std::string, std::string>> data; u32 base = abi::IPL_LOAD_ADDR, entry = 0; double secs = 20; bool have_entry = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i]; auto nx = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "faltou argumento de %s\n", a.c_str()); std::exit(2); } return argv[++i]; };
    if (a == "--base") base = (u32)std::strtoul(nx().c_str(), nullptr, 0); else if (a == "--entry") { entry = (u32)std::strtoul(nx().c_str(), nullptr, 0); have_entry = true; }
    else if (a == "--time") secs = std::atof(nx().c_str());
    else if (a == "--watch-code") { std::string v = nx(); size_t c = v.find(':'); wlo = (u32)std::strtoul(v.substr(0, c).c_str(), nullptr, 0); whi = (u32)std::strtoul(v.substr(c + 1).c_str(), nullptr, 0); }
    else if (a == "--profile") prof = true;
    else if (a == "--video") video = true;
    else if (a == "--shot") { std::string v = nx(); size_t c = v.find(':'); shots.push_back({std::atoi(v.substr(0, c).c_str()), v.substr(c + 1)}); video = true; }
    else if (a == "--dvd") dvd = nx(); else if (a == "--cmdline") cmdline = nx();
    else if (a == "--data") { std::string v = nx(); size_t e = v.find('='); if (e == std::string::npos) { std::fprintf(stderr, "esperava caminho=arquivo\n"); return 2; } data.push_back({v.substr(0, e), v.substr(e + 1)}); }
    else in = a;
  }
  if (in.empty()) { std::fprintf(stderr, "uso: sf100_run prog.bin [--base A] [--entry A] [--time s]\n"); return 2; }
  std::ifstream f(in, std::ios::binary); if (!f) { std::fprintf(stderr, "não abri %s\n", in.c_str()); return 1; }
  std::vector<u8> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  Soc s;
  if (!dvd.empty()) { std::ifstream d(dvd, std::ios::binary); std::vector<u8> im((std::istreambuf_iterator<char>(d)), std::istreambuf_iterator<char>()); s.dvd.insert(im); }
  else if (!data.empty()) { std::vector<SffsFile> fl; for (auto& kv : data) { std::ifstream d(kv.second, std::ios::binary); if (!d) { std::fprintf(stderr, "não abri %s\n", kv.second.c_str()); return 1; } fl.push_back({kv.first, std::vector<u8>((std::istreambuf_iterator<char>(d)), std::istreambuf_iterator<char>())}); } std::vector<u8> im; std::string err; if (!sffs_build(fl, im, &err)) { std::fprintf(stderr, "SnowFS: %s\n", err.c_str()); return 1; } s.dvd.insert(im); }
  if (!cmdline.empty()) { std::vector<u8> c(cmdline.begin(), cmdline.end()); c.push_back(0); s.bus.raw_write(abi::SRAM_BASE + 0x200, c.data(), (u32)c.size()); }
  s.console.on_char = [](char c) { std::putchar(c); };
  s.load(base, img); s.cpu.reset_regs(); s.cpu.a[7] = abi::IPL_STACK; s.cpu.set_pc(have_entry ? entry : base);
  if (video) { s.video.on_frame = [&](Video& v) { for (auto& sh : shots) if (sh.first == (int)v.frames) { write_png(sh.second, v.width(), v.height(), v.frame); std::printf("[sf100_run] quadro %u gravado em %s\n", v.frames, sh.second.c_str()); } }; s.video.start(); }
  u32 ring[256] = {0}; int rn = 0; std::map<u32, u64> hist;
  s.on_step = [&](u32 pc, int) {
    ring[rn++ & 255] = pc; if (prof) hist[pc >> 4]++;
    static const u32 brk = std::getenv("SF_BREAK") ? (u32)std::strtoul(std::getenv("SF_BREAK"), nullptr, 16) : 0;   // SF_BREAK=endereço_hex: mostra regs e pilha nas primeiras passagens
    static u64 nstep; static bool corr; if (!corr && std::getenv("SF_GUARD") && (++nstep & 1023) == 0) {   // SF_GUARD=endereço: avisa quando a memória do código muda (DMA/escrita fora do on_access)
      u32 g = (u32)std::strtoul(std::getenv("SF_GUARD"), nullptr, 16); for (int i = 0; i < 16; i++) if (s.bus.raw_r8(g + i) != img[g - base + i]) { corr = true; std::printf("[sf100_run] CÓDIGO ALTERADO em %08x (pc=%08x, instr=%llu); últimos PCs:", g + i, pc, (unsigned long long)s.cpu.instrs); for (int k = 0; k < 16; k++) std::printf(" %x", ring[(rn + 240 + k) & 255]); std::printf("\n"); break; }
    }
    static int tr = 0; if (tr > 0) { tr--; auto rd = [&](u32 a) -> u16 { u16 v = 0; s.bus.peek16(a, v); return v; }; CfInsn ci = cf_disasm(rd, pc); std::printf("  tr %06x %-28s d0=%x d1=%x a0=%x a6=%x a7=%x\n", pc, ci.text.c_str(), s.cpu.d[0], s.cpu.d[1], s.cpu.a[0], s.cpu.a[6], s.cpu.a[7]); }
    if (brk && pc == brk && std::getenv("SF_TRACE")) tr = 60;
    static int brk_n; if (brk && pc == brk && brk_n++ < 3) { std::printf("[sf100_run] SF_BREAK %08x: ", pc); for (int i = 0; i < 8; i++) std::printf("d%d=%x ", i, s.cpu.d[i]); for (int i = 0; i < 8; i++) std::printf("a%d=%x ", i, s.cpu.a[i]); std::printf("\n  pilha:"); for (int i = 0; i < 12; i++) std::printf(" %x", s.bus.raw_r32(s.cpu.a[7] + 4 * i)); std::printf("\n"); }
    static bool bad; if (!bad && (s.cpu.exc_vec == 2 || s.cpu.exc_vec == 3 || (pc >= abi::DDR_SIZE && pc < 0x10000000u))) {   // salto para fora da RAM: mostra o contexto da primeira ocorrência (antes do loop de exceções)
      bad = true; std::printf("[sf100_run] PRIMEIRA FALHA: pc=%08x vetor=%d endereço=%08x; últimos PCs:", pc, s.cpu.exc_vec, s.cpu.exc_addr); for (int i = 0; i < 16; i++) std::printf(" %x", ring[(rn + 240 + i) & 255]);
      std::printf("\n[sf100_run] ring64:"); for (int i = 0; i < 64; i++) std::printf(" %x", ring[(rn + 192 + i) & 255]);
      std::printf("\n[sf100_run] regs:"); for (int i = 0; i < 8; i++) std::printf(" d%d=%x", i, s.cpu.d[i]); for (int i = 0; i < 8; i++) std::printf(" a%d=%x", i, s.cpu.a[i]);
      std::printf("\n[sf100_run] pilha:"); for (int i = 0; i < 16; i++) std::printf(" %x", s.bus.raw_r32(s.cpu.a[7] + 4 * i)); std::printf("\n");
    }
  };
  if (whi) s.bus.on_access = [&](u32 addr, int size, bool w) { if (w && addr >= wlo && addr < whi && !s.cpu.halted) { std::printf("[sf100_run] ESCRITA NO CÓDIGO: %08x (%d bytes) pelo pc=%08x; últimos PCs:", addr, size, ring[(rn - 1) & 255]); for (int i = 0; i < 16; i++) std::printf(" %x", ring[(rn + 240 + i) & 255]); std::printf("\n"); s.cpu.halted = true; } };
  bool ok = s.run_until_halt((Ps)(secs * 1e12));
  s.bus.cache_flush_all(false); std::fflush(stdout);
  std::printf("[sf100_run] %s: pc=%08x d0=%d, %llu instruções, %.3f ms simulados, I-cache %llu/%llu\n", ok ? "HALT" : (s.cpu.halted ? "CPU parou (falha)" : "tempo esgotado"), s.cpu.pc, (int)s.cpu.d[0],
    (unsigned long long)s.cpu.instrs, s.sched.now / 1e9, (unsigned long long)s.bus.ihits, (unsigned long long)s.bus.imisses);
  if (prof) { std::vector<std::pair<u64, u32>> v; for (auto& kv : hist) v.push_back({kv.second, kv.first << 4}); std::sort(v.rbegin(), v.rend()); std::printf("[sf100_run] PCs mais quentes (blocos de 16 B):"); for (size_t i = 0; i < 12 && i < v.size(); i++) std::printf(" %x(%llu)", v[i].second, (unsigned long long)v[i].first); std::printf("\n"); }
  if (!ok && s.cpu.halted) { std::printf("[sf100_run] últimos PCs:"); for (int i = 0; i < 16; i++) std::printf(" %x", ring[(rn + 240 + i) & 255]); std::printf("\n"); }
  if (!ok && s.cpu.halted) { u32 a = s.cpu.exc_pc; std::printf("[sf100_run] memória em %08x:", a); for (int i = 0; i < 12; i++) std::printf(" %02x", s.bus.raw_r8(a + i)); std::printf("   imagem original:"); for (int i = 0; i < 12; i++) std::printf(" %02x", a - base + i < img.size() ? img[a - base + i] : 0); std::printf("\n"); }
  if (!ok && s.cpu.halted) {   // registradores e topo da pilha: ajuda a achar quem chamou um endereço inválido (use com o .map do link)
    std::printf("[sf100_run] regs:"); for (int i = 0; i < 8; i++) std::printf(" d%d=%x", i, s.cpu.d[i]); for (int i = 0; i < 8; i++) std::printf(" a%d=%x", i, s.cpu.a[i]); std::printf("\n[sf100_run] pilha:");
    for (int i = 0; i < 24; i++) std::printf(" %x", s.bus.raw_r32(s.cpu.a[7] + 4 * i)); std::printf("\n");
  }
  if (!ok && s.cpu.halted) std::printf("[sf100_run] motivo do halt: %d; última exceção: vetor %d em pc=%08x (endereço de falha %08x)\n", (int)s.cpu.reason, s.cpu.exc_vec, s.cpu.exc_pc, s.cpu.exc_addr);
  return ok ? (int)(s.cpu.d[0] & 0xFF) : 1;
}
