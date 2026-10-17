// Recursos do disco do sistema (lado do host): tudo o que o SnowFlame Dashboard lê do DVD em sys/.
// Usado pelo sf100_mkdvd e pelos testes. Formatos (big-endian):
//   sys/icons.raw   5 ícones 64x64 ARGB4444 numa faixa 512x64
//   sys/ui.raw      texturas ARGB8888 da interface (offsets UI_TEX_* abaixo): cápsula "gel", brilho da cápsula, halo radial,
//                   floco de neve, retângulo arredondado (9 fatias) e sua borda
//   sys/hud.raw     sprites ARGB8888 do HUD (Blitter): 6 células 24x24 lado a lado (botões A/B/X/Y, ponto cinza, emblema)
//   sys/emblem.msh  'SFEM', nº de partes, (primeiro, contagem) por parte, vértices de 64 B (pos xyz1, normal xyz0, cor rgba, u v 0 0)
//                   partes: 0 chama (emissiva), 1 cristal de gelo (vidro), 2 anel
//   sys/snd.raw     nº de sons, (offset em bytes, amostras) por som, amostras PCM16; sons: tique, abrir, voltar, sino (C5), pad (laço de 2048)
//   sys/vx/*.vx     microprogramas SnowVector montados de os/vx/*.vsm
#pragma once
#include "sffs.h"
#include "texutil.h"
#include "vpu.h"
#include <cmath>
#include <fstream>
#include <functional>
#include <sstream>

namespace sf { namespace sysassets {

constexpr u32 UI_TEX_PILL = 0x00000, UI_TEX_GLOSS = 0x10000, UI_TEX_GLOW = 0x20000, UI_TEX_FLAKE = 0x24000, UI_TEX_PANEL = 0x28000, UI_TEX_BORDER = 0x2C000, UI_TEX_SIZE = 0x30000;
constexpr int HUD_CELL = 24, HUD_CELLS = 6;

inline std::vector<u8> slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary); if (!f) throw std::runtime_error("não abri " + p);
  return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---- ícones do menu: 5 de 64x64 numa faixa ARGB4444 de 512x64 (supersampling 4x4) ----
struct Px { float r, g, b, a; };
inline float sdBox(float x, float y, float cx, float cy, float hw, float hh, float rad) {
  float dx = std::fabs(x - cx) - hw + rad, dy = std::fabs(y - cy) - hh + rad;
  return std::hypot(std::max(dx, 0.f), std::max(dy, 0.f)) + std::min(std::max(dx, dy), 0.f) - rad;
}
inline float sdCirc(float x, float y, float cx, float cy, float r) { return std::hypot(x - cx, y - cy) - r; }
inline float cov(float d) { return std::clamp(0.5f - d, 0.f, 1.f); }          // d em "pixels de 64 unidades"; 0,5 = meio pixel
inline Px icon_px(int id, float x, float y) {                                  // x,y em [0,64)
  float a = 0, r = 1, g = 1, b = 1; float t = y / 64.f;
  auto over = [&](float cvg, float rr, float gg, float bb) { r = r * (1 - cvg) + rr * cvg; g = g * (1 - cvg) + gg * cvg; b = b * (1 - cvg) + bb * cvg; a = std::max(a, cvg); };
  float br = 0.75f + 0.25f * (1 - t);                                              // brilho vertical
  switch (id) {
    case 0: {                                                                      // controle
      float d = std::min({sdBox(x, y, 32, 34, 22, 11, 8), sdCirc(x, y, 15, 42, 11), sdCirc(x, y, 49, 42, 11)});
      float c = cov(d); r = g = b = br; a = c;
      float cross = std::min(sdBox(x, y, 19, 33, 7, 2.5f, 1), sdBox(x, y, 19, 33, 2.5f, 7, 1)); over(cov(cross), 0.1f, 0.2f, 0.5f);
      over(cov(sdCirc(x, y, 43, 30, 3.2f)), 0.9f, 0.3f, 0.3f); over(cov(sdCirc(x, y, 50, 35, 3.2f)), 0.3f, 0.8f, 0.4f);
      over(cov(sdBox(x, y, 32, 31, 4, 1.5f, 0.8f)), 0.1f, 0.2f, 0.5f); a = c; break;
    }
    case 1: {                                                                      // cartão de memória
      float d = sdBox(x, y, 32, 32, 19, 25, 4);
      float c = cov(d); if (x + (64 - y) > 62 + 13 && y < 17) c = 0; r = 0.6f * br + 0.2f; g = 0.8f * br + 0.1f; b = 1.0f; a = c;
      over(cov(sdBox(x, y, 32, 36, 9, 8, 2)), 0.1f, 0.15f, 0.4f); over(cov(sdBox(x, y, 32, 36, 6.5f, 5.5f, 1)), 0.9f, 0.8f, 0.3f);
      for (int k = 0; k < 4; k++) over(cov(sdBox(x, y, 23 + k * 6, 14, 2, 4, 0.5f)), 0.95f, 0.8f, 0.3f);
      a = c; break;
    }
    case 2: {                                                                      // chip / sistema
      float c = cov(sdBox(x, y, 32, 32, 15, 15, 3)); r = 0.55f * br; g = 0.85f * br; b = 1.0f; a = c;
      for (int k = 0; k < 5; k++) { float p = 22 + k * 5; c = std::max(c, cov(sdBox(x, y, p, 12, 1.4f, 5, 0.3f))); c = std::max(c, cov(sdBox(x, y, p, 52, 1.4f, 5, 0.3f))); c = std::max(c, cov(sdBox(x, y, 12, p, 5, 1.4f, 0.3f))); c = std::max(c, cov(sdBox(x, y, 52, p, 5, 1.4f, 0.3f))); }
      a = std::max(a, c); over(cov(sdBox(x, y, 32, 32, 9, 9, 2)), 0.05f, 0.1f, 0.35f); over(cov(sdBox(x, y, 32, 32, 5, 5, 1)), 0.6f, 0.9f, 1.f); break;
    }
    case 3: {                                                                      // disco
      float R = std::hypot(x - 32, y - 32); float c = cov(R - 24) * (1 - cov(R - 6)); float ang = std::atan2(y - 32, x - 32);
      float sh = 0.5f + 0.5f * std::sin(ang * 2.f + 0.6f); r = 0.5f + 0.5f * sh; g = 0.7f + 0.3f * sh; b = 1.0f; a = c;
      over(cov(std::fabs(R - 16) - 0.7f) * 0.5f, 1, 1, 1); over(cov(R - 9) * (1 - cov(R - 8)) * 0.7f, 0.2f, 0.3f, 0.6f); a = std::max(a, c); break;
    }
    default: {                                                                     // opções: três sliders
      float c = 0; for (int k = 0; k < 3; k++) { float yy = 18 + k * 14; c = std::max(c, cov(sdBox(x, y, 32, yy, 22, 2.2f, 2.2f))); }
      r = 0.7f; g = 0.85f; b = 1.0f; a = c;
      for (int k = 0; k < 3; k++) { float yy = 18 + k * 14, kx = 20 + (k * 17 + 11) % 28; over(cov(sdCirc(x, y, kx, yy, 6.5f)), 1.f, 0.85f - 0.2f * k, 0.3f + 0.3f * k); over(cov(sdCirc(x, y, kx, yy, 2.5f)), 0.15f, 0.2f, 0.45f); a = std::max(a, cov(sdCirc(x, y, kx, yy, 6.5f))); }
    }
  }
  return {r, g, b, a};
}
inline std::vector<u8> make_icons() {
  const int W = 512, H = 64; std::vector<u32> img((size_t)W * H);
  for (int id = 0; id < 5; id++) for (int py = 0; py < 64; py++) for (int px = 0; px < 64; px++) {
    Px acc{0, 0, 0, 0}; for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
      Px p = icon_px(id, px + (sx + 0.5f) / 4, py + (sy + 0.5f) / 4); acc.r += p.r * p.a; acc.g += p.g * p.a; acc.b += p.b * p.a; acc.a += p.a; }
    float al = acc.a / 16; float inv = acc.a > 0 ? 1.f / acc.a : 0;
    auto q = [](float v) { return (int)std::clamp(v * 255.f + 0.5f, 0.f, 255.f); };
    img[(size_t)py * W + id * 64 + px] = pack_argb(q(al), q(acc.r * inv), q(acc.g * inv), q(acc.b * inv));
  }
  return build_texture(img, W, H, abi::TF_ARGB4444, false);
}


// ---- texturas da interface: amostragem 4x4 de uma função de cobertura/cor ----
inline std::vector<u32> raster(int W, int H, const std::function<Px(float, float)>& fn) {
  std::vector<u32> img((size_t)W * H);
  auto q = [](float v) { return (int)std::clamp(v * 255.f + 0.5f, 0.f, 255.f); };
  for (int py = 0; py < H; py++) for (int px = 0; px < W; px++) {
    Px acc{0, 0, 0, 0}; for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
      Px p = fn(px + (sx + 0.5f) / 4, py + (sy + 0.5f) / 4); acc.r += p.r * p.a; acc.g += p.g * p.a; acc.b += p.b * p.a; acc.a += p.a; }
    float inv = acc.a > 0 ? 1.f / acc.a : 0;
    img[(size_t)py * W + px] = pack_argb(q(acc.a / 16), q(acc.r * inv), q(acc.g * inv), q(acc.b * inv));
  }
  return img;
}
inline void put_tex(std::vector<u8>& out, u32 off, const std::vector<u32>& img, int w, int h) {
  auto t = build_texture(img, w, h, abi::TF_ARGB8888, false); std::copy(t.begin(), t.end(), out.begin() + off);
}
inline float sdFlake(float x, float y, float s) {                                 // floco de 6 braços com ramos (centro 0,0; raio s)
  float d = 1e9f;
  for (int k = 0; k < 6; k++) {
    float a = k * 3.14159265f / 3, c = std::cos(a), sn = std::sin(a), u = x * c + y * sn, v = -x * sn + y * c;
    d = std::min(d, sdBox(u, v, s * 0.5f, 0, s * 0.5f, s * 0.06f, s * 0.06f));
    for (int sd = -1; sd <= 1; sd += 2) {
      float bu = u - s * 0.6f, bv = v, ca = std::cos(0.9f), sa = std::sin(0.9f) * sd, ru = bu * ca + bv * sa, rv = -bu * sa + bv * ca;
      d = std::min(d, sdBox(ru, rv, s * 0.14f, 0, s * 0.14f, s * 0.045f, s * 0.04f));
    }
  }
  return std::min(d, sdCirc(x, y, 0, 0, s * 0.14f));
}
inline std::vector<u8> make_ui_textures() {
  std::vector<u8> out(UI_TEX_SIZE, 0);
  put_tex(out, UI_TEX_PILL, raster(256, 64, [](float x, float y) {              // cápsula: corpo com sombreamento vertical (tingida pela cor do vértice)
    float c = cov(sdBox(x, y, 128, 32, 124, 27, 27)), t = y / 64;
    float edge = cov(sdBox(x, y, 128, 32, 124, 27, 27) + 2.5f);                    // borda interna mais clara
    float l = 0.62f + 0.38f * (1 - t) * (1 - t); l = l * edge + std::min(1.f, l + 0.35f) * (1 - edge);
    return Px{l, l, l, c * 0.94f}; }), 256, 64);
  put_tex(out, UI_TEX_GLOSS, raster(256, 64, [](float x, float y) {             // reflexo especular "gel" na metade de cima
    float c = cov(sdBox(x, y, 128, 19, 112, 12, 12)); float g = std::clamp(1.f - (y - 7) / 26.f, 0.f, 1.f);
    return Px{1, 1, 1, c * (0.15f + 0.6f * g * g)}; }), 256, 64);
  put_tex(out, UI_TEX_GLOW, raster(64, 64, [](float x, float y) {
    float r = std::hypot(x - 32, y - 32) / 32, a = std::max(0.f, 1 - r); return Px{1, 1, 1, a * a}; }), 64, 64);
  put_tex(out, UI_TEX_FLAKE, raster(64, 64, [](float x, float y) {
    float d = sdFlake(x - 32, y - 32, 30), r = std::hypot(x - 32, y - 32) / 32, halo = std::max(0.f, 1 - r);
    return Px{1, 1, 1, std::max(cov(d * 1.2f), halo * halo * 0.25f)}; }), 64, 64);
  put_tex(out, UI_TEX_PANEL, raster(64, 64, [](float x, float y) { return Px{1, 1, 1, cov(sdBox(x, y, 32, 32, 31, 31, 14))}; }), 64, 64);
  put_tex(out, UI_TEX_BORDER, raster(64, 64, [](float x, float y) {
    float d = sdBox(x, y, 32, 32, 31, 31, 14); return Px{1, 1, 1, cov(d) * (1 - cov(d + 1.6f))}; }), 64, 64);
  return out;
}
inline std::vector<u8> make_hud_sprites() {                                     // botões coloridos "gel" e o emblema pequeno, ARGB8888
  const int W = HUD_CELL * HUD_CELLS, H = HUD_CELL;
  const float col[5][3] = {{0.25f, 0.85f, 0.35f}, {0.95f, 0.3f, 0.25f}, {0.3f, 0.6f, 1.0f}, {1.0f, 0.8f, 0.2f}, {0.45f, 0.5f, 0.6f}};
  auto img = raster(W, H, [&](float x, float y) {
    int cell = std::min(HUD_CELLS - 1, (int)(x / HUD_CELL)); float cx = cell * HUD_CELL + 12, cy = 12;
    if (cell < 5) {
      float d = sdCirc(x, y, cx, cy, 10), t = (y - 2) / 20, l = 0.75f + 0.25f * (1 - t);
      Px p{col[cell][0] * l, col[cell][1] * l, col[cell][2] * l, cov(d)};
      float gl = cov(sdBox(x, y, cx, cy - 4.5f, 6, 3.5f, 3.5f)) * 0.55f; p.r += (1 - p.r) * gl; p.g += (1 - p.g) * gl; p.b += (1 - p.b) * gl;
      float ring = cov(d) * (1 - cov(d + 1.2f)); p.r += (1 - p.r) * ring * 0.5f; p.g += (1 - p.g) * ring * 0.5f; p.b += (1 - p.b) * ring * 0.5f;
      return p;
    }
    float d = sdFlake(x - cx, y - cy, 11), r = std::hypot(x - cx, y - cy) / 11;
    float hot = std::max(0.f, 1 - r * 2.2f);                                       // núcleo de chama no centro do floco
    return Px{0.7f + 0.3f * hot, 0.9f - 0.35f * hot, 1.0f - 0.85f * hot, std::max(cov(d), hot)};
  });
  return build_texture(img, W, H, abi::TF_ARGB8888, false);
}

// ---- emblema 3D: cristal de neve de 6 braços, chama no núcleo e anel de órbita ----
struct MeshV { float p[3], n[3], c[4], uv[2]; };
struct Mesh {
  std::vector<MeshV> v;
  static void norm(float* n) { float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); if (l > 0) for (int i = 0; i < 3; i++) n[i] /= l; }
  // Triângulo de face plana; a ordem é corrigida para ficar anti-horária vista de fora (normal apontando para longe de 'ctr').
  void tri(const float* a, const float* b, const float* c, const float* ca, const float* cb, const float* cc, const float* ctr) {
    float u[3], w[3], n[3]; for (int i = 0; i < 3; i++) { u[i] = b[i] - a[i]; w[i] = c[i] - a[i]; }
    n[0] = u[1] * w[2] - u[2] * w[1]; n[1] = u[2] * w[0] - u[0] * w[2]; n[2] = u[0] * w[1] - u[1] * w[0]; norm(n);
    float m[3]; for (int i = 0; i < 3; i++) m[i] = (a[i] + b[i] + c[i]) / 3 - ctr[i];
    if (n[0] * m[0] + n[1] * m[1] + n[2] * m[2] < 0) { std::swap(b, c); std::swap(cb, cc); for (float& x : n) x = -x; }
    for (auto [p, col] : {std::pair{a, ca}, {b, cb}, {c, cc}}) { MeshV q{}; std::copy(p, p + 3, q.p); std::copy(n, n + 3, q.n); std::copy(col, col + 4, q.c); v.push_back(q); }
  }
  // Losango facetado (bipirâmide) do ponto o na direção ang: base em r0, ponta em r1, maior largura w em rm, espessura t.
  void diamond(float ox, float oy, float ang, float r0, float r1, float rm, float w, float t, const float* cb, const float* ct) {
    float dx = std::cos(ang), dy = std::sin(ang), px = -dy, py = dx;
    float tip[3] = {ox + dx * r1, oy + dy * r1, 0}, base[3] = {ox + dx * r0, oy + dy * r0, 0};
    float L[3] = {ox + dx * rm + px * w, oy + dy * rm + py * w, 0}, R[3] = {ox + dx * rm - px * w, oy + dy * rm - py * w, 0};
    float F[3] = {ox + dx * rm, oy + dy * rm, t}, B[3] = {ox + dx * rm, oy + dy * rm, -t}, ctr[3] = {ox + dx * rm, oy + dy * rm, 0};
    for (const float* A : {F, B}) { tri(A, tip, L, cb, ct, cb, ctr); tri(A, L, base, cb, cb, cb, ctr); tri(A, base, R, cb, cb, cb, ctr); tri(A, R, tip, cb, cb, ct, ctr); }
  }
};
inline std::vector<u8> make_emblem() {
  Mesh flame, crystal, ring;
  {                                                                                // chama: gota de 8 lados, branca-amarela embaixo, vermelha transparente na ponta
    const float base[4] = {1.0f, 0.95f, 0.7f, 1}, mid[4] = {1.0f, 0.5f, 0.1f, 1}, tip[4] = {1.0f, 0.15f, 0.03f, 0}, ctr[3] = {0, 0.05f, 0};
    float top[3] = {0, 0.46f, 0}, bot[3] = {0, -0.2f, 0};
    for (int k = 0; k < 8; k++) {
      float a0 = k * 6.2831853f / 8, a1 = (k + 1) * 6.2831853f / 8;
      float p0[3] = {0.12f * std::cos(a0), -0.02f, 0.12f * std::sin(a0)}, p1[3] = {0.12f * std::cos(a1), -0.02f, 0.12f * std::sin(a1)};
      flame.tri(top, p0, p1, tip, mid, mid, ctr); flame.tri(bot, p0, p1, base, mid, mid, ctr);
    }
  }
  {                                                                                // cristal: 6 braços com ramos + gema hexagonal central
    const float ice[4] = {0.55f, 0.82f, 1.0f, 0.42f}, tipc[4] = {0.95f, 0.98f, 1.0f, 0.62f}, gem[4] = {0.7f, 0.9f, 1.0f, 0.3f};
    for (int k = 0; k < 6; k++) {
      float a = 1.5707963f + k * 1.0471976f, dx = std::cos(a), dy = std::sin(a);
      crystal.diamond(0, 0, a, 0.16f, 1.0f, 0.5f, 0.075f, 0.07f, ice, tipc);
      for (int sd = -1; sd <= 1; sd += 2) {
        crystal.diamond(dx * 0.52f, dy * 0.52f, a + sd * 0.95f, 0, 0.3f, 0.13f, 0.045f, 0.045f, ice, tipc);
        crystal.diamond(dx * 0.78f, dy * 0.78f, a + sd * 0.95f, 0, 0.16f, 0.07f, 0.03f, 0.03f, ice, tipc);
      }
    }
    float F[3] = {0, 0, 0.15f}, B[3] = {0, 0, -0.15f}, ctr[3] = {0, 0, 0};
    for (int k = 0; k < 6; k++) {
      float a0 = k * 1.0471976f, a1 = (k + 1) * 1.0471976f;
      float p0[3] = {0.23f * std::cos(a0), 0.23f * std::sin(a0), 0}, p1[3] = {0.23f * std::cos(a1), 0.23f * std::sin(a1), 0};
      crystal.tri(F, p0, p1, gem, gem, gem, ctr); crystal.tri(B, p0, p1, gem, gem, gem, ctr);
    }
  }
  {                                                                                // anel: toro fino no plano XY com normais suaves
    const int SEG = 32, SIDE = 4; const float R = 1.0f, r = 0.02f;
    auto vert = [&](int i, int j) { float u = i * 6.2831853f / SEG, w = j * 6.2831853f / SIDE; MeshV q{};
      float nx = std::cos(w) * std::cos(u), ny = std::cos(w) * std::sin(u), nz = std::sin(w);
      q.p[0] = R * std::cos(u) + r * nx; q.p[1] = R * std::sin(u) + r * ny; q.p[2] = r * nz; q.n[0] = nx; q.n[1] = ny; q.n[2] = nz;
      q.c[0] = q.c[1] = q.c[2] = q.c[3] = 1; return q; };
    for (int i = 0; i < SEG; i++) for (int j = 0; j < SIDE; j++) {
      MeshV a = vert(i, j), b = vert(i + 1, j), c = vert(i + 1, j + 1), d = vert(i, j + 1);
      for (const MeshV& q : {a, b, c, a, c, d}) ring.v.push_back(q);
    }
  }
  std::vector<u8> out; auto p32 = [&](u32 x) { for (int k = 3; k >= 0; k--) out.push_back((u8)(x >> (8 * k))); };
  p32(0x5346454D); p32(3); u32 first = 0;
  for (Mesh* m : {&flame, &crystal, &ring}) { p32(first); p32((u32)m->v.size()); first += (u32)m->v.size(); }
  for (Mesh* m : {&flame, &crystal, &ring}) for (auto& q : m->v) {
    float f[16] = {q.p[0], q.p[1], q.p[2], 1, q.n[0], q.n[1], q.n[2], 0, q.c[0], q.c[1], q.c[2], q.c[3], q.uv[0], q.uv[1], 0, 0};
    for (float x : f) p32(f2u(x));
  }
  return out;
}

// ---- sons da interface (PCM16 big-endian, 48 kHz) ----
inline std::vector<u8> make_sounds() {
  const float SR = 48000, PI2 = 6.2831853f; std::vector<std::vector<float>> s(5);
  { int n = 2400; s[0].resize(n); for (int i = 0; i < n; i++) { float t = i / SR, e = std::exp(-t / 0.012f);   // tique cristalino
      s[0][i] = e * (0.6f * std::sin(PI2 * 2350 * t) + 0.3f * std::sin(PI2 * 3520 * t + 1) + 0.15f * std::sin(PI2 * 5870 * t)); } }
  { int n = 13440; s[1].resize(n); float ph = 0; for (int i = 0; i < n; i++) { float t = i / SR, k = t / 0.28f, f = 520 * std::pow(3.2f, k);  // abrir: varredura ascendente
      ph += PI2 * f / SR; float e = std::min(1.f, t / 0.01f) * (1 - k) * (1 - k);
      s[1][i] = e * (0.55f * std::sin(ph) + 0.25f * std::sin(ph * 2.01f) + 0.12f * std::sin(ph * 3.98f)); } }
  { int n = 8640; s[2].resize(n); float ph = 0; for (int i = 0; i < n; i++) { float t = i / SR, k = t / 0.18f, f = 1300 * std::pow(0.4f, k);  // voltar: descendente
      ph += PI2 * f / SR; float e = std::min(1.f, t / 0.006f) * (1 - k); s[2][i] = e * (0.55f * std::sin(ph) + 0.2f * std::sin(ph * 2.5f)); } }
  { int n = 76800; s[3].resize(n); const float pr[5] = {1, 2.0f, 2.76f, 4.07f, 5.4f}, am[5] = {1, 0.45f, 0.35f, 0.18f, 0.1f}, dc[5] = {0.9f, 0.5f, 0.32f, 0.2f, 0.12f};
    for (int i = 0; i < n; i++) { float t = i / SR, v = 0; for (int k = 0; k < 5; k++) v += am[k] * std::exp(-t / dc[k]) * std::sin(PI2 * 523.25f * pr[k] * t);   // sino em C5
      s[3][i] = 0.45f * v * std::min(1.f, t / 0.002f); } }
  { int n = 2048; s[4].resize(n); for (int i = 0; i < n; i++) { float x = PI2 * 4 * i / n, v = 0;          // pad: 4 períodos (93,75 Hz), harmônicos suaves
      for (int k = 1; k <= 8; k++) v += std::sin(k * x + k * 0.7f) / std::pow((float)k, 1.6f); s[4][i] = 0.42f * v; } }
  std::vector<u8> out; auto p32 = [&](u32 x) { for (int k = 3; k >= 0; k--) out.push_back((u8)(x >> (8 * k))); };
  p32((u32)s.size()); u32 off = 0; for (auto& v : s) { p32(off); p32((u32)v.size()); off += (u32)v.size() * 2; }
  for (auto& v : s) for (float x : v) { int q = (int)std::lround(std::clamp(x, -1.f, 1.f) * 32767); out.push_back((u8)(q >> 8)); out.push_back((u8)q); }
  return out;
}

inline std::vector<u8> assemble_vx(const std::string& path) {
  auto src = slurp(path); std::vector<u32> w; std::string err;
  if (!vpu_assemble(std::string(src.begin(), src.end()), w, err)) throw std::runtime_error(path + ": " + err);
  std::vector<u8> out; for (u32 x : w) for (int k = 3; k >= 0; k--) out.push_back((u8)(x >> (8 * k)));
  return out;
}

// Todos os arquivos sys/ do disco do sistema (fonte e logotipo vêm de assets/, gerados por tools/mkassets.py).
inline std::vector<SffsFile> system_files() {
  std::vector<SffsFile> f;
  for (const char* n : {"font.raw", "font.adv", "logo.raw"}) f.push_back({std::string("sys/") + n, slurp(std::string("assets/") + n)});
  f.push_back({"sys/icons.raw", make_icons()});
  f.push_back({"sys/ui.raw", make_ui_textures()});
  f.push_back({"sys/hud.raw", make_hud_sprites()});
  f.push_back({"sys/emblem.msh", make_emblem()});
  f.push_back({"sys/snd.raw", make_sounds()});
  f.push_back({"sys/vx/geometry.vx", assemble_vx("os/vx/geometry.vsm")});
  f.push_back({"sys/vx/dynamics.vx", assemble_vx("os/vx/dynamics.vsm")});
  return f;
}

}}  // namespace sf::sysassets
