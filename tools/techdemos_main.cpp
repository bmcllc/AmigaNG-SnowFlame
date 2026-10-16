// sf100_techdemos: tech demos do SnowFlame SF-100. Cada demo exercita um subsistema pela ABI e grava PNG/WAV/texto.
//   sf100_techdemos [dir=out/techdemos] [flamecore|layers|classic|snowos|audio|all]
#include "classic.h"
#include "snowcc.h"
#include "kernels.h"
#include "texutil.h"
#include "sffs.h"
#include "util.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <array>
#include <algorithm>
#include <sys/stat.h>

using namespace sf;
using namespace sf::abi;

// ---------------------------------------------------------------- utilitários ----------------------------------------------------------------
static std::vector<u32> grab(Soc& s, u32 addr, int w, int h, int pitch_bytes) {
  std::vector<u32> v((size_t)w * h);
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) v[(size_t)y * w + x] = s.bus.raw_r32(addr + y * pitch_bytes + x * 4);
  return v;
}
static std::string OUT;
static std::string path(const std::string& f) { return OUT + "/" + f; }
static u32 rgb(int r, int g, int b) { return 0xFF000000u | ((u32)std::clamp(r, 0, 255) << 16) | ((u32)std::clamp(g, 0, 255) << 8) | (u32)std::clamp(b, 0, 255); }
static void put_f(Soc& s, u32 a, float f) { s.bus.raw_w32(a, f2u(f)); }

// ---------------------------------------------------------------- 1) FlameCore + SnowVector ----------------------------------------------------------------
// Toro e piso com fog, texturas DXT1 com mipmaps e filtragem bilinear: a geometria é transformada/iluminada pela VPU-A (microcódigo de demo/kernels.h),
// o FlameCore rasteriza por tiles com eliminação de superfícies ocultas.
static void demo_flamecore() {
  std::puts("== 1) FlameCore + SnowVector-A: reflexo por stencil, blend alfa/aditivo, multitextura, fog, DXT1 mipmapped, HSR ==");
  using namespace sf::kern;
  Soc s; const int W = 480, H = 360; const u32 RT = VRAM_BASE, TEX_CHECK = VRAM_BASE + 0x400000, TEX_VEIN = VRAM_BASE + 0x410000, TEX_SPOT = VRAM_BASE + 0x420000,
                                             TEX_GLASS = VRAM_BASE + 0x430000, TEX_GLOW = VRAM_BASE + 0x440000, VERT = 0x00300000, IDX = 0x00500000, LIST = 0x00110000;
  std::vector<u32> pg; std::string err; if (!vpu_assemble(geometry_src(), pg, err)) { std::printf("geometria: %s\n", err.c_str()); return; }
  s.vpuA.load_program(pg);
  // ---- texturas ----
  std::vector<u32> img(64 * 64);
  for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { bool c = ((x / 8) + (y / 8)) & 1; img[y * 64 + x] = c ? pack_argb(255, 250, 220, 120) : pack_argb(255, 220, 60, 30); }
  int nl = 0; auto t0 = build_texture(img, 64, 64, TF_DXT1, true, &nl); s.bus.raw_write(TEX_CHECK, t0.data(), (u32)t0.size());
  for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {                                         // veios de mármore (TEX1 do toro): periódico em 64
    double a = 2 * M_PI * x / 64, b = 2 * M_PI * y / 64, v = std::sin(a * 2 + 2.2 * std::sin(b * 1) + 1.3 * std::sin(a + b)) * 0.5 + 0.5; v = std::pow(v, 0.7);
    img[y * 64 + x] = pack_argb(255, (int)(255 * (0.3 + 0.7 * v)), (int)(255 * (0.4 + 0.6 * v)), (int)(255 * (0.6 + 0.4 * v)));
  }
  auto t1 = build_texture(img, 64, 64, TF_RGB565, false); s.bus.raw_write(TEX_VEIN, t1.data(), (u32)t1.size());
  for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {                                         // "poça" de luz: rgb = brilho, alfa = quanto o piso reflete (menos perto do objeto)
    double d = std::hypot(x - 31.5, y - 31.5) / 32.0, l = std::clamp(1.05 - 0.8 * d, 0.2, 1.0), al = std::clamp(0.3 + 0.6 * d, 0.3, 0.92);
    img[y * 64 + x] = pack_argb((int)(al * 255), (int)(255 * l), (int)(245 * l), (int)(225 * l));
  }
  auto t2 = build_texture(img, 64, 64, TF_ARGB4444, false); s.bus.raw_write(TEX_SPOT, t2.data(), (u32)t2.size());
  for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { double band = 0.5 + 0.5 * std::sin(2 * M_PI * y / 64.0 * 8), al = 0.22 + 0.3 * band * band; img[y * 64 + x] = pack_argb((int)(al * 255), 210, 240, 255); }
  auto t3 = build_texture(img, 64, 64, TF_ARGB4444, false); s.bus.raw_write(TEX_GLASS, t3.data(), (u32)t3.size());
  for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { double d = std::hypot(x - 31.5, y - 31.5) / 32.0, g = std::exp(-d * d * 3.2) * std::clamp((1.0 - d) * 2.5, 0.0, 1.0); img[y * 64 + x] = pack_argb(255, (int)(255 * g), (int)(150 * g), (int)(55 * g)); }
  auto t4 = build_texture(img, 64, 64, TF_ARGB8888, false); s.bus.raw_write(TEX_GLOW, t4.data(), (u32)t4.size());

  // ---- malhas ----
  struct V { float p[3], n[3], uv[2], fog; };
  struct Mesh { std::vector<V> v; std::vector<u16> idx; u32 vaddr = 0, iaddr = 0; };
  Mesh torus, floor_m, sphere, halo; const int NS = 32, NR = 16; const double R = 1.0, r = 0.42;
  for (int i = 0; i <= NR; i++) for (int j = 0; j <= NS; j++) {
    double u = 2 * M_PI * j / NS, v = 2 * M_PI * i / NR; double cx = std::cos(u), sx = std::sin(u), cv = std::cos(v), sv = std::sin(v);
    V t{}; t.p[0] = (float)((R + r * cv) * cx); t.p[1] = (float)(r * sv); t.p[2] = (float)((R + r * cv) * sx);
    t.n[0] = (float)(cv * cx); t.n[1] = (float)sv; t.n[2] = (float)(cv * sx); t.uv[0] = (float)j / NS * 4; t.uv[1] = (float)i / NR * 2; torus.v.push_back(t);
  }
  for (int i = 0; i < NR; i++) for (int j = 0; j < NS; j++) { u16 a = (u16)(i * (NS + 1) + j), b = (u16)(a + 1), c = (u16)(a + NS + 1), d = (u16)(c + 1); for (u16 k : {a, c, b, b, c, d}) torus.idx.push_back(k); }
  const int FN = 16;
  for (int i = 0; i <= FN; i++) for (int j = 0; j <= FN; j++) { V t{}; t.p[0] = (float)((j - FN / 2.0) * 1.2); t.p[1] = -1.0f; t.p[2] = (float)((i - FN / 2.0) * 1.2); t.n[1] = 1; t.uv[0] = (float)j * 0.5f; t.uv[1] = (float)i * 0.5f; floor_m.v.push_back(t); }
  for (int i = 0; i < FN; i++) for (int j = 0; j < FN; j++) { u16 a = (u16)(i * (FN + 1) + j), b = (u16)(a + 1), c = (u16)(a + FN + 1), d = (u16)(c + 1); for (u16 k : {a, c, b, b, c, d}) floor_m.idx.push_back(k); }
  const int SS = 20, SR = 12; const double SRAD = 0.5;
  for (int i = 0; i <= SR; i++) for (int j = 0; j <= SS; j++) {
    double th = M_PI * i / SR, ph = 2 * M_PI * j / SS; V t{}; t.n[0] = (float)(std::sin(th) * std::cos(ph)); t.n[1] = (float)std::cos(th); t.n[2] = (float)(std::sin(th) * std::sin(ph));
    for (int k = 0; k < 3; k++) t.p[k] = (float)(SRAD * t.n[k]); t.uv[0] = (float)j / SS; t.uv[1] = (float)i / SR; sphere.v.push_back(t);
  }
  for (int i = 0; i < SR; i++) for (int j = 0; j < SS; j++) { u16 a = (u16)(i * (SS + 1) + j), b = (u16)(a + 1), c = (u16)(a + SS + 1), d = (u16)(c + 1); for (u16 k : {a, c, b, b, c, d}) sphere.idx.push_back(k); }
  for (int c = 0; c < 4; c++) { V t{}; t.p[0] = (c & 1 ? 1.5f : -1.5f); t.p[1] = (c & 2 ? 1.5f : -1.5f); t.p[2] = 0; t.n[2] = 1; t.uv[0] = c & 1 ? 1.0f : 0.0f; t.uv[1] = c & 2 ? 0.0f : 1.0f; halo.v.push_back(t); }
  halo.idx = {0, 1, 3, 0, 3, 2};
  u32 va = VERT, ia = IDX;
  for (Mesh* m : {&torus, &floor_m, &sphere, &halo}) { m->vaddr = va; va += (u32)m->v.size() * 48; m->iaddr = ia; for (size_t i = 0; i < m->idx.size(); i++) s.bus.raw_w16(ia + (u32)i * 2, m->idx[i]); ia += (u32)((m->idx.size() * 2 + 15) & ~15u); }
  Mesh mirror = torus; mirror.vaddr = va; mirror.iaddr = torus.iaddr;                                   // reflexo: mesmos índices, vértices refletidos

  // ---- transformação na VPU-A ----
  auto transform = [&](Mesh& m, std::vector<V>& vs, const Mat4& model, double amb) {
    Mat4 view = translate(0, -0.1, -4.6), proj = perspective(55.0 * M_PI / 180.0, (double)W / H, 0.1, 100.0), mvp = mul(proj, mul(view, model));
    const u32 CST = 0, IN = 0x200; u32 OUTD = IN + (u32)vs.size() * 48;
    auto pv = [&](u32 a, float x, float y, float z, float w) { put_f(s, VPUA_DMEM + a, x); put_f(s, VPUA_DMEM + a + 4, y); put_f(s, VPUA_DMEM + a + 8, z); put_f(s, VPUA_DMEM + a + 12, w); };
    for (int c = 0; c < 4; c++) pv(CST + c * 16, (float)mvp.m[0][c], (float)mvp.m[1][c], (float)mvp.m[2][c], (float)mvp.m[3][c]);
    for (int c = 0; c < 3; c++) pv(CST + (4 + c) * 16, (float)model.m[0][c], (float)model.m[1][c], (float)model.m[2][c], 0);
    double L[3] = {-0.5, 0.8, 0.45}, ln = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    pv(CST + 7 * 16, (float)(L[0] / ln), (float)(L[1] / ln), (float)(L[2] / ln), 0); pv(CST + 8 * 16, W / 2.0f, -H / 2.0f, 0.5f, 0); pv(CST + 9 * 16, W / 2.0f, H / 2.0f, 0.5f, 0);
    pv(CST + 10 * 16, 0, 0, 0, 1); pv(CST + 11 * 16, 0.85f, 0.8f, 0.7f, 0); pv(CST + 12 * 16, 0, 0, 0, 0); pv(CST + 13 * 16, (float)amb, (float)amb, (float)(amb + 0.05), 1); pv(CST + 14 * 16, 1, 1, 1, 1);
    for (size_t i = 0; i < vs.size(); i++) {
      double x = vs[i].p[0], y = vs[i].p[1], z = vs[i].p[2]; double ex = model.m[0][0] * x + model.m[0][1] * y + model.m[0][2] * z + model.m[0][3];
      double ez = model.m[2][0] * x + model.m[2][1] * y + model.m[2][2] * z + model.m[2][3] - 4.6;
      vs[i].fog = (float)std::clamp((std::sqrt(ex * ex + ez * ez) - 4.4) / 6.5, 0.0, 1.0);
      u32 a = IN + (u32)i * 48; pv(a, vs[i].p[0], vs[i].p[1], vs[i].p[2], 1); pv(a + 16, vs[i].n[0], vs[i].n[1], vs[i].n[2], 0); pv(a + 32, vs[i].uv[0], vs[i].uv[1], vs[i].fog, 0);
    }
    s.vpuA.write(VPU_PC, 0); s.vpuA.write(VPU_S0, IN); s.vpuA.write(VPU_S0 + 4, OUTD); s.vpuA.write(VPU_S0 + 8, IN + (u32)vs.size() * 48); s.vpuA.write(VPU_S0 + 12, CST);
    s.vpuA.write(VPU_CTRL, VPUC_START);
    s.wait_until([&] { return !(s.vpuA.read(VPU_STATUS) & VPUS_BUSY); });
    for (size_t i = 0; i < vs.size() * 12; i++) s.bus.raw_w32(m.vaddr + (u32)i * 4, s.bus.raw_r32(VPUA_DMEM + OUTD + (u32)i * 4));
  };
  auto cmd = [&](std::vector<u32>& w, u32 op, std::vector<u32> p) { w.push_back((op << 24) | (u32)p.size()); for (u32 x : p) w.push_back(x); };
  auto settex = [&](std::vector<u32>& w, u32 unit, u32 addr, u32 fmt, u32 flags, u32 levels, float scale) {
    cmd(w, GC_SET_TEX, {unit, addr, 6u | (6u << 8) | (fmt << 16) | (flags << 24), levels}); cmd(w, GC_TEXENV, {unit, TE_MODULATE, f2u(scale)});
  };
  auto draw = [&](std::vector<u32>& w, const Mesh& m) { cmd(w, GC_VERTEX_BASE, {m.vaddr}); cmd(w, GC_DRAW_INDEXED, {m.iaddr, (u32)m.idx.size(), PRIM_TRIS}); };
  const u32 FOG = 0xFF10142A, ALL = 0xFF;
  u64 tris = 0, covered = 0, shaded = 0, cyc = 0, texf = 0;
  for (int f = 0; f < 4; f++) {
    double t = f * 0.55; double ty = 0.35 + 0.1 * std::sin(t * 2);
    Mat4 tmodel = mul(translate(0, ty, 0), mul(rotate_y(t * 0.8), rotate_x(1.05 + 0.25 * std::sin(t))));
    Mat4 reflect = mul(translate(0, -1, 0), mul(Mat4{{{1, 0, 0, 0}, {0, -1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}}, translate(0, 1, 0)));   // espelho no plano y = -1
    transform(torus, torus.v, tmodel, 0.18); mirror.v = torus.v; transform(mirror, mirror.v, mul(reflect, tmodel), 0.18);
    transform(floor_m, floor_m.v, identity(), 0.35);
    double sa = t * 1.3 + 0.6; Mat4 smodel = translate(2.1 * std::cos(sa), 0.2 + 0.25 * std::sin(sa * 1.7), 1.3 * std::sin(sa)); transform(sphere, sphere.v, smodel, 0.35);
    transform(halo, halo.v, translate(0, ty + 0.1, -1.1), 0.6);

    std::vector<u32> w;
    cmd(w, GC_SET_RT, {RT, (u32)W * 4, (u32)W | ((u32)H << 16)});
    cmd(w, GC_CLEAR, {FOG, f2u(1.0f), CLR_COLOR | CLR_DEPTH});
    cmd(w, GC_FOG, {FOG, f2u(0.0f), f2u(1.0f)});
    const u32 TEXS = ST_TEX0 | ST_TEX1 | ST_BILINEAR | ST_FOG;
    // A) piso: marca o stencil (=1) onde existe piso; sem escrever Z, para o reflexo (que fica "abaixo" do piso) poder desenhar
    settex(w, 0, TEX_CHECK, TF_DXT1, TFLAG_MIPS, (u32)nl, 1.0f); settex(w, 1, TEX_SPOT, TF_ARGB4444, TFLAG_CLAMP, 1, 0.125f);
    cmd(w, GC_STENCIL, {6, 1, ALL, SOP_KEEP, SOP_KEEP, SOP_REPLACE}); cmd(w, GC_STATE, {ST_DEPTH_TEST | ST_STENCIL | TEXS}); draw(w, floor_m);
    // B) toro refletido, recortado pelo stencil (só aparece onde há piso)
    settex(w, 0, TEX_CHECK, TF_DXT1, TFLAG_MIPS, (u32)nl, 1.0f); settex(w, 1, TEX_VEIN, TF_RGB565, 0, 1, 1.0f);
    cmd(w, GC_STENCIL, {4, 1, ALL, SOP_KEEP, SOP_KEEP, SOP_KEEP}); cmd(w, GC_STATE, {ST_DEPTH_TEST | ST_DEPTH_WRITE | ST_STENCIL | TEXS}); draw(w, mirror);
    // C) toro real: multitextura (xadrez DXT1 × veios de mármore, cada unidade com sua escala de UV)
    cmd(w, GC_TEXENV, {1, TE_MODULATE, f2u(0.5f)}); cmd(w, GC_STATE, {ST_DEPTH_TEST | ST_DEPTH_WRITE | TEXS}); draw(w, torus);
    // D) piso de novo com blend: o alfa da "poça de luz" (TEX1) decide quanto reflexo aparece
    settex(w, 0, TEX_CHECK, TF_DXT1, TFLAG_MIPS, (u32)nl, 1.0f); settex(w, 1, TEX_SPOT, TF_ARGB4444, TFLAG_CLAMP, 1, 0.125f);
    cmd(w, GC_BLEND, {BF_SRC_ALPHA, BF_INV_SRC_ALPHA}); cmd(w, GC_STATE, {ST_DEPTH_TEST | ST_BLEND | (1u << ST_DEPTH_FUNC_SHIFT) | TEXS}); draw(w, floor_m);
    // E) halo aditivo atrás do toro (BF_ONE, BF_ONE), sem Z-write
    cmd(w, GC_SET_TEX, {0, TEX_GLOW, 6u | (6u << 8) | ((u32)TF_ARGB8888 << 16) | (TFLAG_CLAMP << 24), 1}); cmd(w, GC_TEXENV, {0, TE_REPLACE, f2u(1.0f)});
    cmd(w, GC_BLEND, {BF_ONE, BF_ONE}); cmd(w, GC_STATE, {ST_DEPTH_TEST | ST_BLEND | ST_TEX0 | ST_BILINEAR}); draw(w, halo);
    // F) esfera de vidro translúcida (alfa da textura × iluminação), por último
    cmd(w, GC_SET_TEX, {0, TEX_GLASS, 6u | (6u << 8) | ((u32)TF_ARGB4444 << 16) | (0u << 24), 1}); cmd(w, GC_TEXENV, {0, TE_MODULATE, f2u(1.0f)});
    cmd(w, GC_BLEND, {BF_SRC_ALPHA, BF_INV_SRC_ALPHA}); cmd(w, GC_STATE, {ST_DEPTH_TEST | ST_BLEND | ST_TEX0 | ST_BILINEAR | ST_FOG}); draw(w, sphere);
    cmd(w, GC_END, {});
    for (size_t i = 0; i < w.size(); i++) s.bus.raw_w32(LIST + (u32)i * 4, w[i]);
    s.gpu.write(GPU_CMD_ADDR, LIST); s.gpu.write(GPU_CMD_LEN, (u32)w.size()); s.gpu.write(GPU_CTRL, 1);
    s.wait_until([&] { return !(s.gpu.read(GPU_STATUS) & GPUS_BUSY); });
    if (s.gpu.read(GPU_STATUS) & GPUS_ERROR) std::puts("   AVISO: a GPU reportou erro na lista de comandos");
    tris += s.gpu.last.tris_drawn; covered += s.gpu.last.pix_covered; shaded += s.gpu.last.pix_shaded; cyc += s.gpu.last.cycles; texf += s.gpu.last.texfetch;
    char name[64]; std::snprintf(name, sizeof name, "flamecore_%d.png", f); write_png(path(name), W, H, grab(s, RT, W, H, W * 4));
  }
  std::printf("   4 quadros %dx%d, 6 passes por quadro (piso+stencil, reflexo, toro multitextura, piso com blend, halo aditivo, vidro): %llu triângulos, %.1f%% dos pixels cobertos sombreados,\n", W, H, (unsigned long long)tris, 100.0 * shaded / std::max<u64>(covered, 1));
  std::printf("   GPU %.2f Mciclos (%.2f ms/quadro @200 MHz), %.1f M buscas de textura; VPU-A: %llu ciclos no total (%zu vértices/quadro)\n", cyc / 1e6, cyc / 4 / 200e3, texf / 1e6,
    (unsigned long long)s.vpuA.total_cycles, torus.v.size() * 2 + floor_m.v.size() + sphere.v.size() + halo.v.size());
}

// ---------------------------------------------------------------- 2) Camadas de vídeo + Copper ----------------------------------------------------------------
// 4 camadas ARGB com rolagem em paralaxe; o Copper reescreve scroll e alfa por scanline (ondulação da água, desvanecimento do reflexo).
static void demo_layers() {
  std::puts("== 2) Vídeo de 4 camadas + SnowCopper: paralaxe, ondulação e alfa por scanline ==");
  Soc s; const int LW = 512, LH = 240; const u32 PITCH = LW * 4, COP = 0x00150000;
  auto base = [&](int i) { return VRAM_BASE + (u32)i * 0x80000; };
  auto px = [&](int layer, int x, int y, u32 c) { s.bus.raw_w32(base(layer) + y * PITCH + x * 4, c); };
  for (int y = 0; y < LH; y++) for (int x = 0; x < LW; x++) {                                         // L0: céu em degradê com estrelas e lua
    double t = y / 180.0; int r = (int)(12 + 150 * t * t), g = (int)(18 + 80 * t), b = (int)(60 + 140 * t); u32 c = rgb(r, g, b);
    u32 h = (u32)(x * 73856093u ^ y * 19349663u); if (y < 100 && (h % 600) == 0) c = rgb(255, 255, 230);
    double mx = x - 400, my = y - 50; if (mx * mx + my * my < 22 * 22) c = rgb(250, 245, 215); else if ((mx + 7) * (mx + 7) + (my + 3) * (my + 3) < 21 * 21 && mx * mx + my * my < 28 * 28) c = rgb((r + 250) / 2, (g + 245) / 2, (b + 215) / 2);
    px(0, x, y, c);
  }
  for (int x = 0; x < LW; x++) {                                                                      // L1: montanhas distantes (periódicas em 512)
    double a = 2 * M_PI * x / LW; double hgt = 105 + 28 * std::sin(a * 3 + 1) + 16 * std::sin(a * 7 + 2) + 8 * std::sin(a * 15);
    for (int y = 0; y < LH; y++) {
      u32 c = 0; if (y >= hgt) { double k = std::min(1.0, (y - hgt) / 70.0); c = rgb((int)(70 - 30 * k), (int)(80 - 25 * k), (int)(130 - 30 * k)); if (y < hgt + 6 && hgt < 100) c = rgb(235, 240, 255); }
      px(1, x, y, c);
    }
  }
  for (int x = 0; x < LW; x++) {                                                                      // L2: colinas com pinheiros
    double a = 2 * M_PI * x / LW; double hgt = 150 + 14 * std::sin(a * 4) + 7 * std::sin(a * 9 + 1);
    for (int y = 0; y < LH; y++) { u32 c = 0; if (y >= hgt) c = rgb((int)(18 + 10 * std::sin(x * 0.3)), (int)(52 + 14 * (y - hgt) / 90), 32); px(2, x, y, c); }
    if (x % 29 == 0) { int top = (int)hgt - 26; for (int y = 0; y < 26; y++) for (int dx = -(y / 3 + 1); dx <= y / 3 + 1; dx++) px(2, (x + dx + LW) % LW, top + y, rgb(14, 70 - y, 30)); }
  }
  for (int y = 0; y < LH; y++) for (int x = 0; x < LW; x++) {                                         // L3: lago (parte de baixo) com brilho da lua
    u32 c = 0; if (y >= 186) { double k = (y - 186) / 54.0; int r = (int)(20 + 30 * k), g = (int)(60 + 40 * k), b = (int)(120 + 40 * k);
      double sp = std::exp(-std::pow((x - 400 + 20 * std::sin(y * 0.5)) / (6 + (y - 186) * 0.4), 2)); r += (int)(200 * sp); g += (int)(190 * sp); b += (int)(120 * sp); c = rgb(r, g, b); }
    px(3, x, y, c);
  }
  s.video.write(VID_MODE, VMODE_240P); s.video.write(VID_BG, 0xFF000000);
  for (int i = 0; i < 4; i++) { u32 L = VID_LAYER0 + (u32)i * VID_LAYER_STRIDE; s.video.write(L + LR_ADDR, base(i)); s.video.write(L + LR_PITCH, PITCH); s.video.write(L + LR_FLAGS, LF_ENABLE | LF_WRAP); s.video.write(L + LR_SIZE, (u32)LW | ((u32)LH << 16)); s.video.write(L + LR_ALPHA, 255); }
  s.video.write(VID_ORDER, 0x3210); s.cop.write(COP_LIST, COP); s.cop.write(COP_CTRL, 1);
  const int frames = 120; u64 ops0 = 0;
  for (int f = 0; f < frames; f++) {
    u32 i = 0; auto put = [&](u32 a, u32 b) { s.bus.raw_w32(COP + i * 8, a); s.bus.raw_w32(COP + i * 8 + 4, b); i++; };
    auto set = [&](int layer, u32 reg, u32 v) { put(cop_move(VIDEO_BASE + VID_LAYER0 + (u32)layer * VID_LAYER_STRIDE + reg), v); };
    put(cop_wait(0), 0); set(0, LR_SCX, (u32)(f / 3)); set(1, LR_SCX, (u32)(f * 4 / 5)); set(2, LR_SCX, (u32)(f * 2)); set(3, LR_SCX, 0);
    for (int y = 150; y < 186; y += 6) { put(cop_wait((u32)y), 0); set(2, LR_SCX, (u32)(f * 2 + (y - 150) / 6 * 3 + 12 * (y / 6 % 2))); }   // calor sobre as colinas
    for (int y = 186; y < 240; y++) {                                                                  // água: ondulação horizontal e alfa decrescente
      put(cop_wait((u32)y), 0); double k = (y - 186) / 54.0;
      set(3, LR_SCX, (u32)(int)(std::sin(y * 0.55 + f * 0.25) * (2 + 6 * k) + 512 * 4)); set(3, LR_ALPHA, (u32)(150 + 90 * k));
    }
    put(0, 0);
    s.run_frames(1);
    if (f == 0 || f == 40 || f == 80 || f == 119) { char n[64]; std::snprintf(n, sizeof n, "layers_%03d.png", f); write_png(path(n), s.video.width(), s.video.height(), s.video.frame); }
  }
  (void)ops0;
  std::printf("   %d quadros 320x240; Copper executou %llu instruções (%.0f por quadro)\n", frames, (unsigned long long)s.cop.ops_executed, (double)s.cop.ops_executed / frames);
}

// ---------------------------------------------------------------- 3) SnowOS Classic (OCS/AGA) ----------------------------------------------------------------
static void W(ClassicSystem& c, u32 reg, u16 v) { c.write16(0xDFF000 + reg, v); }
static void demo_classic() {
  std::puts("== 3) SnowOS Classic: 5 bitplanes + barras do Copper + color cycling; HAM6 ==");
  {
    Soc s; ClassicSystem c(s); const u32 BM = 0x20000, COP = 0x10000, PLANE = 0x2000;
    W(c, 0x08E, 0x2C81); W(c, 0x090, 0xF4C1); W(c, 0x092, 0x38); W(c, 0x094, 0xD0); W(c, 0x100, (u16)((5 << 12) | 0x200)); W(c, 0x108, 0); W(c, 0x10A, 0);
    for (int n = 0; n < 5; n++) { W(c, 0x0E0 + 4 * n, (u16)((BM + n * PLANE) >> 16)); W(c, 0x0E2 + 4 * n, (u16)(BM + n * PLANE)); }
    for (int y = 0; y < 200; y++) for (int x = 0; x < 320; x++) {
      double dx = (x - 160) / 150.0, dy = (y - 100) / 82.0; int idx = 0;
      if (dx * dx + dy * dy < 1.0) { double v = std::sin(x * 0.07) + std::sin(y * 0.09) + std::sin((x + y) * 0.05) + std::sin(std::hypot(x - 160, y - 100) * 0.11); idx = 1 + ((int)((v + 4) * 3.4)) % 31; }
      for (int n = 0; n < 5; n++) if ((idx >> n) & 1) { u32 a = BM + n * PLANE + y * 40 + x / 8; s.bus.raw_write(a, (u8[]){(u8)(s.bus.raw_r8(a) | (0x80 >> (x & 7)))}, 1); }
    }
    auto hsv = [](double h) { h = std::fmod(h, 1.0); double r = std::clamp(std::fabs(h * 6 - 3) - 1, 0.0, 1.0), g = std::clamp(2 - std::fabs(h * 6 - 2), 0.0, 1.0), b = std::clamp(2 - std::fabs(h * 6 - 4), 0.0, 1.0); return std::array<int, 3>{(int)(r * 15), (int)(g * 15), (int)(b * 15)}; };
    W(c, 0x080, (u16)(COP >> 16)); W(c, 0x082, (u16)COP); W(c, 0x096, 0x8000 | 0x200 | 0x100 | 0x80);
    for (int f = 0; f < 3; f++) {
      for (int n = 0; n < 5; n++) { W(c, 0x0E0 + 4 * n, (u16)((BM + n * PLANE) >> 16)); W(c, 0x0E2 + 4 * n, (u16)(BM + n * PLANE)); }   // BPLxPT são contadores vivos: reaponta a cada quadro
      for (int i = 1; i < 32; i++) { auto k = hsv((i - 1 + f * 7) / 31.0); W(c, 0x180 + 2 * i, (u16)((k[0] << 8) | (k[1] << 4) | k[2])); }      // color cycling
      u32 a = COP; auto w16 = [&](u16 v) { s.bus.raw_w16(a, v); a += 2; };
      for (int y = 0; y < 200; y++) {                                                                      // barras de raster em COLOR00
        double ph = y * 0.09 + f * 0.9; int r = (int)(7 + 7 * std::sin(ph)), g = (int)(7 + 7 * std::sin(ph + 2.1)), b = (int)(7 + 7 * std::sin(ph + 4.2));
        w16((u16)(((0x2C + y) << 8) | 0x01)); w16(0xFFFE); w16(0x0180); w16((u16)((r << 8) | (g << 4) | b));
      }
      w16(0xFFFF); w16(0xFFFE);
      c.chip.render_frame(); char n[64]; std::snprintf(n, sizeof n, "classic_bars_%d.png", f); write_png(path(n), CLASSIC_FB_W, CLASSIC_FB_H, grab(s, CLASSIC_FB_ADDR, CLASSIC_FB_W, CLASSIC_FB_H, CLASSIC_FB_PITCH));
    }
  }
  {   // HAM6: 4096 cores com 6 bitplanes; codificador guloso (palavra = controle de 2 bits + valor de 4 bits)
    Soc s; ClassicSystem c(s); const u32 BM = 0x20000, PLANE = 0x2000;
    W(c, 0x08E, 0x2C81); W(c, 0x090, 0xF4C1); W(c, 0x092, 0x38); W(c, 0x094, 0xD0); W(c, 0x100, (u16)((6 << 12) | 0x200 | 0x800)); W(c, 0x108, 0); W(c, 0x10A, 0); W(c, 0x096, 0x8000 | 0x200 | 0x100);
    int pal[16][3]; for (int i = 0; i < 16; i++) { int v = i; pal[i][0] = (v * 2 + 1) / 3 % 16; pal[i][1] = v; pal[i][2] = std::min(15, v + 3); W(c, 0x180 + 2 * i, (u16)((pal[i][0] << 8) | (pal[i][1] << 4) | pal[i][2])); }
    for (int n = 0; n < 6; n++) { W(c, 0x0E0 + 4 * n, (u16)((BM + n * PLANE) >> 16)); W(c, 0x0E2 + 4 * n, (u16)(BM + n * PLANE)); }
    auto src = [&](int x, int y) {                                                                       // pôr do sol sobre o mar
      double h = y / 200.0, r, g, b;
      if (h < 0.62) { r = 0.1 + 0.95 * h * h * 2.2; g = 0.05 + 0.5 * h * h * 2.2; b = 0.35 + 0.15 * h; double d = std::hypot(x - 160, y - 105) / 38.0; if (d < 1) { r = 1; g = 0.85 - 0.3 * d; b = 0.5 - 0.3 * d; } else { double k = std::exp(-(d - 1) * 1.4) * 0.45; r += k; g += k * 0.6; b += k * 0.2; } }
      else { double k = (h - 0.62) / 0.38; r = 0.45 - 0.35 * k; g = 0.2 + 0.1 * (1 - k); b = 0.45 - 0.2 * k; double sp = std::exp(-std::pow((x - 160) / (4 + k * 30.0), 2)) * (0.6 + 0.4 * std::sin(y * 1.3)); r += sp * 0.9; g += sp * 0.6; b += sp * 0.2; }
      return std::array<int, 3>{(int)std::clamp(r * 15 + 0.5, 0.0, 15.0), (int)std::clamp(g * 15 + 0.5, 0.0, 15.0), (int)std::clamp(b * 15 + 0.5, 0.0, 15.0)};
    };
    for (int y = 0; y < 200; y++) {
      int cur[3] = {0, 0, 0}; for (int k = 0; k < 3; k++) cur[k] = pal[0][k];
      for (int x = 0; x < 320; x++) {
        auto t = src(x, y); int best = 1 << 30, code = 0, nr[3] = {cur[0], cur[1], cur[2]};
        auto err = [&](int r, int g, int b) { return (r - t[0]) * (r - t[0]) + (g - t[1]) * (g - t[1]) + (b - t[2]) * (b - t[2]); };
        for (int i = 0; i < 16; i++) { int e = err(pal[i][0], pal[i][1], pal[i][2]); if (e < best) { best = e; code = i; nr[0] = pal[i][0]; nr[1] = pal[i][1]; nr[2] = pal[i][2]; } }
        { int e = err(cur[0], cur[1], t[2]); if (e < best) { best = e; code = 0x10 | t[2]; nr[0] = cur[0]; nr[1] = cur[1]; nr[2] = t[2]; } }
        { int e = err(t[0], cur[1], cur[2]); if (e < best) { best = e; code = 0x20 | t[0]; nr[0] = t[0]; nr[1] = cur[1]; nr[2] = cur[2]; } }
        { int e = err(cur[0], t[1], cur[2]); if (e < best) { best = e; code = 0x30 | t[1]; nr[0] = cur[0]; nr[1] = t[1]; nr[2] = cur[2]; } }
        for (int k = 0; k < 3; k++) cur[k] = nr[k];
        for (int n = 0; n < 6; n++) if ((code >> n) & 1) { u32 a = BM + n * PLANE + y * 40 + x / 8; s.bus.raw_write(a, (u8[]){(u8)(s.bus.raw_r8(a) | (0x80 >> (x & 7)))}, 1); }
      }
    }
    c.chip.render_frame(); write_png(path("classic_ham6.png"), CLASSIC_FB_W, CLASSIC_FB_H, grab(s, CLASSIC_FB_ADDR, CLASSIC_FB_W, CLASSIC_FB_H, CLASSIC_FB_PITCH));
  }
  std::puts("   3 quadros de color cycling com barras do Copper + 1 imagem HAM6 de 4096 cores");
}

// ---------------------------------------------------------------- 4) SnowOS Native ----------------------------------------------------------------
// Aplicação em C compilada pelo snowcc: SnowFS no DVD, dos.library (Open/Read/Write), SnowGX (GPU), portas de mensagem exec, MMU e teclado (SnowIn).
static const char* SNOWOS_APP = R"APP(
#include "snowos_all.c"
unsigned cmd[160], verts[96]; unsigned char readme[400];
struct MsgPort *srv_port; volatile int served;
struct Req { struct Message m; int n; int result; };
static struct Req reqs[3];
static void rogue(void) { volatile int *p = (volatile int *)0; os_printf("[rogue ] vou escrever em NULL...\n"); *p = 1; os_printf("[rogue ] nao deveria chegar aqui\n"); }
static void server(void) {
  struct MsgPort *p = CreateMsgPort(); struct Req *r; int i;
  srv_port = p;
  for (i = 0; i < 3; i++) { WaitPort(p); r = (struct Req *)GetMsg(p); r->result = r->n * r->n; served++; ReplyMsg(&r->m); }
  DeleteMsgPort(p);
}
void app_main(void) {
  GxList l; BPTR fh; int i, n, id; unsigned t0;
  struct MsgPort *rp;
  os_printf("=== SnowOS Native no SF-100 ===\n");
  fs_mount(); os_printf("SnowFS: %d arquivos no DVD\n", fs_count());
  for (i = 0; i < fs_count(); i++) os_printf("  %s  (%d bytes)\n", fs_name(i), FileSize(fs_name(i)));
  fh = Open("readme.txt", MODE_OLDFILE); n = Read(fh, readme, 399); readme[n] = 0; Close(fh);
  os_printf("--- readme.txt ---\n"); Write(Output(), readme, n); os_printf("------------------\n");
  n = fs_load("gfx/flame.raw", (void *)0x01100000, 0x10000); os_printf("textura carregada pelo dos: %d bytes\n", n);
  gx_init();
  gx_put_vertex(&verts[0],   16 << 16,  16 << 16, 0x8000, 0xFFFFFFFF, 0, 0);
  gx_put_vertex(&verts[12], 240 << 16,  16 << 16, 0x8000, 0xFFFFFFFF, 65536, 0);
  gx_put_vertex(&verts[24], 240 << 16, 240 << 16, 0x8000, 0xFFFFFFFF, 65536, 65536);
  gx_put_vertex(&verts[36],  16 << 16,  16 << 16, 0x8000, 0xFFFFFFFF, 0, 0);
  gx_put_vertex(&verts[48], 240 << 16, 240 << 16, 0x8000, 0xFFFFFFFF, 65536, 65536);
  gx_put_vertex(&verts[60],  16 << 16, 240 << 16, 0x8000, 0xFFFFFFFF, 0, 65536);
  os_cache_flush((unsigned)verts, sizeof verts);
  gx_begin(&l, cmd, 160);
  gx_set_rt(&l, VRAM_BASE, 1024, 256, 256); gx_clear(&l, 0xFF20103A, 0x3F800000, CLR_COLOR | CLR_DEPTH);
  gx_set_tex(&l, 0, 0x01100000, 7, 7, TF_RGB565, TFLAG_CLAMP, 1); gx_texenv(&l, 0, TE_REPLACE);
  gx_vertex_base(&l, (unsigned)verts); gx_state(&l, ST_TEX0 | ST_BILINEAR); gx_draw_arrays(&l, 0, 6, PRIM_TRIS); gx_end(&l);
  os_printf("FlameCore: %s\n", gx_submit(&l) == 0 ? "quad texturizado desenhado" : "erro");
  id = task_create(rogue, 2048, 6); task_sleep(5);
  os_printf("MMU: tarefa rogue esta %s\n", task_state(id) == TASK_DEAD ? "MORTA (acesso a NULL barrado)" : "viva?!");
  rp = CreateMsgPort(); task_create(server, 4096, 6); while (!srv_port) task_yield();
  for (i = 0; i < 3; i++) { reqs[i].m.mn_ReplyPort = rp; reqs[i].n = i + 3; PutMsg(srv_port, &reqs[i].m); WaitPort(rp); GetMsg(rp); os_printf("exec: servidor devolveu %d^2 = %d\n", reqs[i].n, reqs[i].result); }
  os_printf("teclado: aguardando 3 teclas...\n");
  for (i = 0; i < 3; i++) { int ev; do { ev = in_key_wait(); } while (in_key_ascii(ev) == 0); os_printf("  tecla '%c'\n", in_key_ascii(ev)); }
  t0 = os_ticks(); Delay(25); os_printf("Delay(25 ticks de 1/50 s) = %d ms\n", os_ticks() - t0);
  os_printf("heap livre: %d bytes\n", AvailMem(0));
  os_exit(0);
}
)APP";
static void demo_snowos() {
  std::puts("== 4) SnowOS Native: kernel + SnowFS + dos/exec + MMU + SnowGX + SnowIn ==");
  std::vector<u32> flame(128 * 128);
  for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++) {
    double dx = (x - 64) / 44.0, dy = (y - 80) / 60.0, d = std::sqrt(dx * dx * (1.0 + 0.9 * std::max(0.0, -dy)) + dy * dy * 0.7) + 0.12 * std::sin(y * 0.4 + x * 0.17) * (y / 128.0);
    double t = std::clamp(1.2 - d, 0.0, 1.0); int r = (int)(255 * std::min(1.0, t * 1.9)), g = (int)(255 * std::clamp(t * 1.9 - 0.7, 0.0, 1.0)), b = (int)(255 * std::clamp(t * 2.4 - 1.5, 0.0, 1.0));
    flame[y * 128 + x] = t <= 0 ? pack_argb(255, 8, 4, 18) : pack_argb(255, r, g, b);
  }
  auto tex = build_texture(flame, 128, 128, TF_RGB565, false);
  std::string readme = "Amiga SnowFlame SF-100\nDVD de demonstracao do SnowOS Native.\nArquivos lidos pelo SnowFS (somente leitura).\n";
  std::vector<SffsFile> files = {{"readme.txt", std::vector<u8>(readme.begin(), readme.end())}, {"gfx/flame.raw", tex}};
  std::vector<u8> dvd; sffs_build(files, dvd);
  CcOptions opt; opt.include_dirs = {"os", "os/include"};
  CcResult r = cc_compile(SNOWOS_APP, "app.c", opt); if (!r.ok) { std::printf("   erro de compilação: %s\n", r.error.c_str()); return; }
  Soc s; s.dvd.insert(dvd); s.load(r.base, r.image); s.cpu.reset_regs(); s.cpu.a[7] = 0x00F00000; s.cpu.set_pc(r.entry);
  for (u8 k : {0x16, 0x12, 0x12}) { s.hid.key_event(k, true); s.hid.key_event(k, false); }          // 'S', 'O', 'O' (USB usages 0x16, 0x12)
  bool ok = s.run_until_halt(6000000000000ull);
  { std::ofstream f(path("snowos_console.txt")); f << s.console.out; }
  std::printf("   %s; console salvo em snowos_console.txt (%zu bytes), TLB: %llu faltas atendidas\n", ok ? "app terminou (HALT)" : "app NÃO terminou", s.console.out.size(), (unsigned long long)s.bus.tlb_misses);
  s.bus.cache_flush_all(false);
  write_png(path("snowos_quad.png"), 256, 256, grab(s, VRAM_BASE, 256, 256, 1024));
  std::printf("%s", [&] { std::string o; std::istringstream is(s.console.out); std::string ln; while (std::getline(is, ln)) o += "   | " + ln + "\n"; return o; }().c_str());
}

// ---------------------------------------------------------------- 5) SnowPaula ----------------------------------------------------------------
static void demo_audio() {
  std::puts("== 5) SnowPaula: 16 vozes com posicionamento 3D, filtro, reverb e sequenciador de eventos ==");
  Soc s; auto& ar = s.bus.aram;
  auto wave = [&](u32 at, int kind) {                                                                    // ciclo de 64 amostras (750 Hz @ passo 1,0)
    for (int i = 0; i < 64; i++) {
      double p = i / 64.0, v = 0;
      if (kind == 0) { for (int h = 1; h <= 9; h++) v += std::sin(2 * M_PI * h * p) / h; v *= 0.5; }              // dente de serra aditivo
      else if (kind == 1) { v = std::sin(2 * M_PI * p) + 0.35 * std::sin(4 * M_PI * p) + 0.15 * std::sin(6 * M_PI * p); }  // "pluck"
      else v = std::sin(2 * M_PI * p);                                                                      // seno (baixo)
      wbe16(&ar[at + i * 2], (u16)(i16)std::lround(v * 9000));
    }
  };
  wave(0x0000, 0); wave(0x1000, 1); wave(0x2000, 2);
  std::vector<i16> noise(2400); u32 st = 7; for (auto& n : noise) { st = st * 1664525u + 1013904223u; n = (i16)((int)((st >> 16) & 0x7FFF) - 0x4000); }
  for (size_t i = 0; i < noise.size(); i++) wbe16(&ar[0x3000 + i * 2], (u16)(noise[i] * (int)(noise.size() - i) / (int)noise.size()));        // ruído com decaimento (chimbal)
  struct Ev { u32 t, v, reg, val; }; std::vector<Ev> ev;
  auto F = [](double hz) { return (u32)std::lround(hz / 750.0 * 65536.0); };
  auto note = [&](double t, int v, u32 addr, u32 len, double hz, int vol, double x, double z, int rev, int lp, double dur, bool loop = true) {
    u32 T = (u32)(t * 48000); u32 ctrl = 1 | (loop ? 2 : 0) | (1 << 2) | 0x10 | (lp ? 0x20 : 0);
    ev.push_back({T, (u32)v, PV_CTRL, 0}); ev.push_back({T, (u32)v, PV_ADDR, addr}); ev.push_back({T, (u32)v, PV_LEN, len}); ev.push_back({T, (u32)v, PV_LOOP_START, 0}); ev.push_back({T, (u32)v, PV_LOOP_LEN, len});
    ev.push_back({T, (u32)v, PV_STEP, F(hz)}); ev.push_back({T, (u32)v, PV_X, f2u((float)x)}); ev.push_back({T, (u32)v, PV_Z, f2u((float)z)}); ev.push_back({T, (u32)v, PV_REV_SEND, (u32)rev});
    ev.push_back({T, (u32)v, PV_LP_COEF, (u32)lp}); ev.push_back({T, (u32)v, PV_VOL_L, (u32)vol}); ev.push_back({T, (u32)v, PV_VOL_R, (u32)vol}); ev.push_back({T, (u32)v, PV_CTRL, ctrl});
    for (int k = 1; k <= 3; k++) { u32 tv = T + (u32)(dur * 48000 * k / 4.0); u32 vv = (u32)(vol * (4 - k) / 4); ev.push_back({tv, (u32)v, PV_VOL_L, vv}); ev.push_back({tv, (u32)v, PV_VOL_R, vv}); }
    ev.push_back({T + (u32)(dur * 48000), (u32)v, PV_CTRL, 0});
  };
  const double prog[4][3] = {{220.00, 261.63, 329.63}, {174.61, 220.00, 261.63}, {261.63, 329.63, 392.00}, {196.00, 246.94, 293.66}};   // Am F C G
  const double bass[4] = {55.0, 43.65, 65.41, 49.0}; int pv = 0;
  for (int bar = 0; bar < 8; bar++) {
    double t0 = bar * 1.6; const double* ch = prog[bar % 4];
    for (int k = 0; k < 3; k++) note(t0, k, 0x0000, 64, ch[k] / 2, 90, -3.0 + 3.0 * k, -2.0, 120, 70, 1.5);                       // pad: dente de serra filtrado
    note(t0, 3, 0x2000, 64, bass[bar % 4], 200, 0.0, -1.0, 20, 0, 1.4);                                                          // baixo
    for (int n = 0; n < 8; n++) {                                                                                                // arpejo "pluck" girando em torno do ouvinte
      double hz = ch[n % 3] * (n >= 6 ? 4 : n >= 3 ? 2 : 1) * (n % 2 ? 1 : 1); double ang = (bar * 8 + n) * 0.45; pv = 4 + (n % 8);
      note(t0 + n * 0.2, pv, 0x1000, 64, hz, 150, 4.0 * std::sin(ang), -1.5 - 2.0 * (1 + std::cos(ang)) / 2, 150, 90, 0.5);
    }
    for (int n = 0; n < 4; n++) note(t0 + n * 0.4 + 0.2, 12 + (n % 3), 0x3000, 2400, 750.0, 70, (n % 2) ? 3.0 : -3.0, -2.5, 60, 0, 0.05, false);   // chimbal
  }
  std::stable_sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
  u32 a = 0x00170000; for (auto& e : ev) { s.bus.raw_w32(a, e.t); s.bus.raw_w32(a + 4, e.v); s.bus.raw_w32(a + 8, e.reg); s.bus.raw_w32(a + 12, e.val); a += 16; }
  s.paula.master = 230; s.paula.rev_wet = 110;
  s.paula.write(PA_SEQ_ADDR, 0x00170000); s.paula.write(PA_SEQ_LEN, (u32)ev.size()); s.paula.write(PA_SEQ_CTRL, 1);
  s.sched.advance_to(s.sched.now + (Ps)(13.6e12)); s.sync_audio();
  int peak = 0; for (i16 x : s.paula.pcm) peak = std::max(peak, std::abs((int)x));
  if (peak > 0) for (i16& x : s.paula.pcm) x = (i16)std::lround((double)x * 28000.0 / peak);          // normaliza para o arquivo
  write_wav(path("snowpaula_demo.wav"), s.paula.pcm);
  std::printf("   %zu eventos do sequenciador, %.1f s de áudio estéreo 48 kHz, pico %d/32767; DSP: pico %.1f%% do orçamento, %llu vozes descartadas\n", ev.size(), s.paula.pcm.size() / 96000.0, peak,
    s.paula.dsp_peak / 10.0, (unsigned long long)s.paula.dsp_drops);
}

int main(int argc, char** argv) {
  OUT = argc > 1 ? argv[1] : "out/techdemos"; std::string which = argc > 2 ? argv[2] : "all";
  std::string cmd = "mkdir -p '" + OUT + "'"; if (std::system(cmd.c_str()) != 0) return 1;
  std::printf("SnowFlame SF-100 - tech demos (saída em %s)\n", OUT.c_str());
  auto on = [&](const char* n) { return which == "all" || which == n; };
  if (on("flamecore")) demo_flamecore();
  if (on("layers")) demo_layers();
  if (on("classic")) demo_classic();
  if (on("snowos")) demo_snowos();
  if (on("audio")) demo_audio();
  return 0;
}
