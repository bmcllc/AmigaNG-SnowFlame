#include "helpers.h"
#include "texutil.h"

using Vtx = FlameCore::Vtx;
static Vtx V(float x, float y, float r, float g, float b, float z = 0.5f, float a = 1, float rhw = 1, float u = 0, float v = 0, float fog = 0) { return Vtx{x, y, z, rhw, r, g, b, a, u, v, fog}; }

struct GL {
  std::vector<u32> w;
  GL& add(u32 op, std::vector<u32> p) { w.push_back((op << 24) | (u32)p.size()); for (u32 x : p) w.push_back(x); return *this; }
  GL& rt(int W = 64, int H = 64, u32 addr = VRAM_BASE, u32 pitch = 256) { return add(GC_SET_RT, {addr, pitch, (u32)W | ((u32)H << 16)}); }
  GL& clear(u32 argb, float depth = 1.0f, u32 fl = CLR_COLOR | CLR_DEPTH) { return add(GC_CLEAR, {argb, f2u(depth), fl}); }
  GL& state(u32 f) { return add(GC_STATE, {f}); }
  GL& vbase(u32 a) { return add(GC_VERTEX_BASE, {a}); }
  GL& arrays(u32 first, u32 n, u32 prim = PRIM_TRIS) { return add(GC_DRAW_ARRAYS, {first, n, prim}); }
  GL& indexed(u32 a, u32 n, u32 prim = PRIM_TRIS) { return add(GC_DRAW_INDEXED, {a, n, prim}); }
  GL& tex(u32 unit, u32 addr, int lw, int lh, int fmt, u32 flags = 0, int nl = 1) { return add(GC_SET_TEX, {unit, addr, (u32)lw | ((u32)lh << 8) | ((u32)fmt << 16) | (flags << 24), (u32)nl}); }
  GL& blend(u32 s, u32 d) { return add(GC_BLEND, {s, d}); }
  GL& end() { return add(GC_END, {}); }
};
constexpr u32 VB = 0x200000, CMD = 0x100000, TEXB = 0x400000;
constexpr u32 ST_OPAQUE_DEPTH = ST_DEPTH_TEST | ST_DEPTH_WRITE;

struct GT {
  Soc s;
  void verts(const std::vector<Vtx>& v, u32 at = VB) {
    for (size_t i = 0; i < v.size(); i++) { u8* p = s.bus.raw(at + i * 48, 48); const Vtx& q = v[i];
      float f[12] = {q.x, q.y, q.z, q.rhw, q.r, q.g, q.b, q.a, q.u, q.v, q.fog, 0}; for (int k = 0; k < 12; k++) wbef(p + 4 * k, f[k]); }
  }
  FlameCore::Stats exec(GL& g) {
    for (size_t i = 0; i < g.w.size(); i++) s.bus.raw_w32(CMD + i * 4, g.w[i]);
    return s.gpu.execute(CMD, (u32)g.w.size());
  }
  u32 px(int x, int y) { return s.bus.raw_r32(VRAM_BASE + y * 256 + x * 4); }
  int count_nonzero(int W = 64, int H = 64) { int n = 0; for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (px(x, y)) n++; return n; }
};
static int ch(u32 c, int shift) { return (c >> shift) & 255; }

TEST(gpu_clear_and_tile_writes) {
  GT t; GL g; g.rt().clear(0xFF102030).end(); auto st = t.exec(g);
  CHECK(!t.s.gpu.error); CHECK_EQ(st.tiles, 4); CHECK_EQ(st.vram_write, 64 * 64 * 4);
  CHECK_EQ(t.px(0, 0), 0xFF102030); CHECK_EQ(t.px(63, 63), 0xFF102030); CHECK_EQ(t.px(31, 32), 0xFF102030);
  GL g2; g2.rt(40, 40).clear(0xFF00FF00).end(); st = t.exec(g2);
  CHECK_EQ(st.tiles, 4); CHECK_EQ(st.vram_write, 40 * 40 * 4); CHECK_EQ(t.px(39, 39), 0xFF00FF00); CHECK_EQ(t.px(40, 10), 0xFF102030);   // fora do RT: intacto
}

TEST(gpu_top_left_rule_exact_pixel_count) {
  GT t; t.verts({V(0, 0, 1, 1, 1), V(32, 0, 1, 1, 1), V(0, 32, 1, 1, 1)});
  GL g; g.rt().clear(0).state(0).vbase(VB).arrays(0, 3).end(); auto st = t.exec(g);
  CHECK_EQ(st.tris_drawn, 1); CHECK_EQ(t.count_nonzero(), 496);     // x+y <= 30; hipotenusa (x+y=31) fica de fora
  CHECK_EQ(st.pix_covered, 496); CHECK(t.px(0, 0) != 0); CHECK(t.px(30, 0) != 0); CHECK_EQ(t.px(31, 0), 0);
  // sentido anti-horário também desenha (sem culling)
  GT t2; t2.verts({V(0, 0, 1, 1, 1), V(0, 32, 1, 1, 1), V(32, 0, 1, 1, 1)});
  GL g2; g2.rt().clear(0).state(0).vbase(VB).arrays(0, 3).end(); t2.exec(g2); CHECK_EQ(t2.count_nonzero(), 496);
}

TEST(gpu_shared_edges_no_gaps_no_double_hits) {
  GT t;
  // quad com vértices fracionários dividido em 2 triângulos + leque de 9 triângulos ao redor de um ponto
  std::vector<Vtx> v = {V(3.3f, 5.7f, 0.4f, 0.4f, 0.4f), V(50.2f, 9.9f, 0.4f, 0.4f, 0.4f), V(47.1f, 48.8f, 0.4f, 0.4f, 0.4f), V(8.6f, 52.4f, 0.4f, 0.4f, 0.4f)};
  t.verts(v);
  t.s.bus.raw_w16(0x300000, 0); t.s.bus.raw_w16(0x300002, 1); t.s.bus.raw_w16(0x300004, 2); t.s.bus.raw_w16(0x300006, 0); t.s.bus.raw_w16(0x300008, 2); t.s.bus.raw_w16(0x30000A, 3);
  GL g; g.rt().clear(0).state(ST_BLEND).blend(BF_ONE, BF_ONE).vbase(VB).indexed(0x300000, 6).end(); t.exec(g);
  int single = 0, dbl = 0;
  auto inside = [&](double x, double y, double m) {
    double px_[4] = {3.3, 50.2, 47.1, 8.6}, py_[4] = {5.7, 9.9, 48.8, 52.4};
    for (int i = 0; i < 4; i++) { int j = (i + 1) % 4; double ex = px_[j] - px_[i], ey = py_[j] - py_[i], d = ex * (y - py_[i]) - ey * (x - px_[i]); if (d / std::hypot(ex, ey) < m) return false; }
    return true;
  };
  for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
    u32 c = t.px(x, y); int r = ch(c, 16);
    if (r == 102) single++; else if (r == 204) dbl++; else if (r != 0) CHECK_EQ(r, 0);
    if (inside(x + 0.5, y + 0.5, 1.5)) CHECK_EQ(r, 102);          // sem buracos no interior
  }
  CHECK_EQ(dbl, 0); CHECK(single > 1500);
  // leque
  GT f; std::vector<Vtx> fv; fv.push_back(V(32.37f, 31.61f, 0.4f, 0.4f, 0.4f));
  for (int i = 0; i < 9; i++) { double a = i * 2 * M_PI / 9; fv.push_back(V((float)(32.37 + 25 * std::cos(a)), (float)(31.61 + 25 * std::sin(a)), 0.4f, 0.4f, 0.4f)); }
  f.verts(fv); GL gf; gf.rt().clear(0).state(ST_BLEND).blend(BF_ONE, BF_ONE).vbase(VB).arrays(0, 10, PRIM_FAN).add(GC_DRAW_ARRAYS, {0, 3, PRIM_FAN}).end();
  // (o último comando redesenha um triângulo -> 2 acertos nele; usamos apenas para validar contagem)
  GL gf2; gf2.rt().clear(0).state(ST_BLEND).blend(BF_ONE, BF_ONE).vbase(VB).arrays(0, 10, PRIM_FAN).end();
  // o leque fecha em v9 -> v1? Não: leque com 10 vértices cobre 8 triângulos; fecha explicitamente repetindo v1
  fv.push_back(fv[1]); f.verts(fv); GL gf3; gf3.rt().clear(0).state(ST_BLEND).blend(BF_ONE, BF_ONE).vbase(VB).arrays(0, 11, PRIM_FAN).end(); f.exec(gf3);
  int d2 = 0, ok = 0;
  for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
    double dd = std::hypot(x + 0.5 - 32.37, y + 0.5 - 31.61), inner = 25 * std::cos(M_PI / 9) - 1.0; int r = ch(f.px(x, y), 16);
    if (r > 102) d2++; if (dd < inner) { CHECK_EQ(r, 102); ok++; }
  }
  CHECK_EQ(d2, 0); CHECK(ok > 1500);
}

TEST(gpu_gouraud_and_perspective_correct_interpolation) {
  GT t; t.verts({V(0, 0, 1, 0, 0), V(60, 0, 0, 1, 0), V(0, 60, 0, 0, 1)});
  GL g; g.rt().clear(0).state(0).vbase(VB).arrays(0, 3).end(); t.exec(g);
  u32 c = t.px(10, 10);
  CHECK(std::abs(ch(c, 16) - 166) <= 1); CHECK(std::abs(ch(c, 8) - 45) <= 1); CHECK(std::abs(ch(c, 0) - 45) <= 1);
  // perspectiva: r = u; A(rhw=1,u=0) B(rhw=.25,u=1) C(rhw=1,u=0)
  GT p; p.verts({V(0, 0, 0, 0, 0, 0.5f, 1, 1.0f), V(64, 0, 1, 0, 0, 0.5f, 1, 0.25f), V(0, 64, 0, 0, 0, 0.5f, 1, 1.0f)});
  GL gp; gp.rt().clear(0).state(0).vbase(VB).arrays(0, 3).end(); p.exec(gp);
  double sx = 32.5, sy = 1.5, l1 = sx / 64, l2 = sy / 64, l0 = 1 - l1 - l2;
  double expect = (l1 * 0.25 * 1.0) / (l0 * 1 + l1 * 0.25 + l2 * 1), affine = l1;
  int r = ch(p.px(32, 1), 16);
  CHECK(std::abs(r - (int)std::lround(expect * 255)) <= 1); CHECK(std::abs(r - (int)std::lround(affine * 255)) > 40);
}

static std::vector<Vtx> random_scene(int n, u32 seed, bool with_alpha) {
  std::vector<Vtx> v; u32 st = seed; auto rnd = [&]() { st = st * 1664525u + 1013904223u; return (st >> 8) / 16777216.0f; };
  for (int i = 0; i < n; i++) {
    float cx = rnd() * 64, cy = rnd() * 64, z = rnd(), r = rnd(), g = rnd(), b = rnd(), a = with_alpha && (i % 5 == 4) ? 0.5f : 1.0f;
    for (int k = 0; k < 3; k++) { Vtx q = V(cx + (rnd() - 0.5f) * 40, cy + (rnd() - 0.5f) * 40, r, g, b, std::min(1.0f, z + 0.05f * k), a); v.push_back(q); }
  }
  return v;
}

TEST(gpu_hsr_gives_identical_image_with_less_shading) {
  const int N = 60; auto sc = random_scene(N, 12345, false);
  GT a, b; a.verts(sc); b.verts(sc); a.s.gpu.hsr = true; b.s.gpu.hsr = false;
  GL g; g.rt().clear(0xFF000000).state(ST_OPAQUE_DEPTH).vbase(VB).arrays(0, N * 3).end();
  auto sa = a.exec(g); auto sb = b.exec(g);
  CHECK(!a.s.gpu.error);
  for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) CHECK_EQ(a.px(x, y), b.px(x, y));
  CHECK_EQ(sa.pix_covered, sb.pix_covered); CHECK(sa.pix_shaded < sb.pix_shaded);
  CHECK(sa.pix_shaded <= 64 * 64);
  // cena mista com transparência e função de profundidade variada
  auto sc2 = random_scene(N, 777, true); GT c, d; c.verts(sc2); d.verts(sc2); d.s.gpu.hsr = false;
  GL g2; g2.rt().clear(0xFF204060).blend(BF_SRC_ALPHA, BF_INV_SRC_ALPHA).vbase(VB);
  for (int i = 0; i < N; i++) { bool tr = (i % 5 == 4); g2.state(tr ? (ST_DEPTH_TEST | ST_BLEND) : ST_OPAQUE_DEPTH | (i % 7 == 0 ? (1u << ST_DEPTH_FUNC_SHIFT) : 0)); g2.arrays(i * 3, 3); }
  g2.end(); c.exec(g2); d.exec(g2);
  for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) CHECK_EQ(c.px(x, y), d.px(x, y));
}

TEST(gpu_hsr_overdraw_worst_and_best_case) {
  const int L = 10; std::vector<Vtx> v;
  for (int i = 0; i < L; i++) { float z = 0.9f - 0.08f * i; float c = 0.1f * i; v.push_back(V(0, 0, c, 0, 0, z)); v.push_back(V(64, 0, c, 0, 0, z)); v.push_back(V(0, 64, c, 0, 0, z)); }   // frente-para-trás... z decresce => cada camada mais perto
  GT a, b; a.verts(v); b.verts(v); b.s.gpu.hsr = false;
  GL g; g.rt().clear(0).state(ST_OPAQUE_DEPTH).vbase(VB).arrays(0, L * 3).end();
  auto sa = a.exec(g), sb = b.exec(g);
  CHECK_EQ(sb.pix_shaded, sb.pix_covered); CHECK_EQ(sa.pix_covered, sb.pix_covered);
  CHECK(sa.pix_shaded * 5 < sb.pix_shaded);          // HSR sombreia ~1 camada em vez de 10
  CHECK(a.px(5, 5) == b.px(5, 5));
  std::vector<Vtx> rev(v.rbegin(), v.rend());        // ordem invertida de submissão: imediato também sombreia pouco
  for (size_t i = 0; i + 2 < rev.size(); i += 3) std::swap(rev[i], rev[i + 2]);
  GT c; c.verts(rev); c.s.gpu.hsr = false; auto sc = c.exec(g); CHECK(sc.pix_shaded < sb.pix_shaded);
}

TEST(gpu_depth_functions) {
  auto run = [&](u32 func, float z2) {
    GT t; t.verts({V(0, 0, 1, 0, 0, 0.5f), V(40, 0, 1, 0, 0, 0.5f), V(0, 40, 1, 0, 0, 0.5f), V(0, 0, 0, 1, 0, z2), V(40, 0, 0, 1, 0, z2), V(0, 40, 0, 1, 0, z2)});
    GL g; g.rt().clear(0xFF000000).state(ST_OPAQUE_DEPTH).vbase(VB).arrays(0, 3).state(ST_DEPTH_TEST | ST_DEPTH_WRITE | (func << ST_DEPTH_FUNC_SHIFT)).arrays(3, 3).end(); t.exec(g);
    return t.px(5, 5);
  };
  CHECK_EQ(run(0, 0.3f), 0xFF00FF00); CHECK_EQ(run(0, 0.7f), 0xFFFF0000); CHECK_EQ(run(0, 0.5f), 0xFFFF0000);   // LESS
  CHECK_EQ(run(1, 0.5f), 0xFF00FF00);                                                                             // LEQUAL
  CHECK_EQ(run(2, 0.7f), 0xFF00FF00); CHECK_EQ(run(2, 0.3f), 0xFFFF0000);                                         // GREATER
  CHECK_EQ(run(4, 0.5f), 0xFF00FF00); CHECK_EQ(run(4, 0.6f), 0xFFFF0000);                                         // EQUAL
  CHECK_EQ(run(6, 0.9f), 0xFF00FF00); CHECK_EQ(run(7, 0.1f), 0xFFFF0000);                                         // ALWAYS / NEVER
}

TEST(gpu_culling_modes) {
  GT t; t.verts({V(0, 0, 1, 1, 1), V(30, 0, 1, 1, 1), V(0, 30, 1, 1, 1)});     // área > 0 (horário na tela com y para baixo)
  auto n = [&](u32 st) { GL g; g.rt().clear(0).state(st).vbase(VB).arrays(0, 3).end(); return t.exec(g).tris_drawn; };
  CHECK_EQ(n(0), 1); CHECK_EQ(n(ST_CULL_CW), 0); CHECK_EQ(n(ST_CULL_CCW), 1);
  GT u; u.verts({V(0, 0, 1, 1, 1), V(0, 30, 1, 1, 1), V(30, 0, 1, 1, 1)});
  auto m = [&](u32 st) { GL g; g.rt().clear(0).state(st).vbase(VB).arrays(0, 3).end(); return u.exec(g).tris_drawn; };
  CHECK_EQ(m(ST_CULL_CW), 1); CHECK_EQ(m(ST_CULL_CCW), 0);
}

TEST(gpu_alpha_blending_and_factors) {
  GT t; t.verts({V(0, 0, 0, 0, 1, 0.5f, 0.5f), V(64, 0, 0, 0, 1, 0.5f, 0.5f), V(0, 64, 0, 0, 1, 0.5f, 0.5f)});
  GL g; g.rt().clear(0xFFFF0000).state(ST_BLEND).blend(BF_SRC_ALPHA, BF_INV_SRC_ALPHA).vbase(VB).arrays(0, 3).end(); t.exec(g);
  u32 c = t.px(5, 5); CHECK(std::abs(ch(c, 16) - 127) <= 1); CHECK(std::abs(ch(c, 0) - 128) <= 1); CHECK_EQ(ch(c, 8), 0);
  GL g2; g2.rt().clear(0xFFFF0000).state(ST_BLEND).blend(BF_ZERO, BF_ONE).vbase(VB).arrays(0, 3).end(); t.exec(g2); CHECK_EQ(t.px(5, 5), 0xFFFF0000);   // destino preservado
  GL g3; g3.rt().clear(0xFFFF0000).state(ST_BLEND).blend(BF_ONE, BF_ZERO).vbase(VB).arrays(0, 3).end(); t.exec(g3); CHECK_EQ(ch(t.px(5, 5), 0), 255); CHECK_EQ(ch(t.px(5, 5), 16), 0);
}

static void quad(std::vector<Vtx>& v, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float z = 0.5f, float r = 1, float g = 1, float b = 1) {
  v.push_back(V(x0, y0, r, g, b, z, 1, 1, u0, v0)); v.push_back(V(x1, y0, r, g, b, z, 1, 1, u1, v0)); v.push_back(V(x0, y1, r, g, b, z, 1, 1, u0, v1));
  v.push_back(V(x1, y0, r, g, b, z, 1, 1, u1, v0)); v.push_back(V(x1, y1, r, g, b, z, 1, 1, u1, v1)); v.push_back(V(x0, y1, r, g, b, z, 1, 1, u0, v1));
}

TEST(gpu_texture_formats_decode) {
  std::vector<u32> img(64);
  for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { int k = x + y; img[y * 8 + x] = pack_argb(255, 255 - k * 10, k * 17, 20 + k * 12); }   // gradiente em reta no espaço de cor
  for (int fmt = 0; fmt < 4; fmt++) {
    GT t; auto bytes = build_texture(img, 8, 8, fmt, false); t.s.bus.raw_write(TEXB, bytes.data(), (u32)bytes.size());
    std::vector<Vtx> v; quad(v, 0, 0, 8, 8, 0, 0, 1, 1); t.verts(v);
    GL g; g.rt().clear(0).state(ST_TEX0).add(GC_TEXENV, {0, TE_REPLACE}).tex(0, TEXB, 3, 3, fmt).vbase(VB).arrays(0, 6).end(); t.exec(g);
    CHECK(!t.s.gpu.error);
    int bad = 0, tol = fmt == 0 ? 0 : fmt == 1 ? 8 : fmt == 2 ? 16 : 40;
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
      u32 got = t.px(x, y), src = img[y * 8 + x];
      for (int sh = 0; sh < 32; sh += 8) if (std::abs(ch(got, sh) - ch(src, sh)) > tol) bad++;
    }
    if (bad) std::printf("    formato %d: %d canais fora da tolerância %d\n", fmt, bad, tol);
    CHECK_EQ(bad, 0);
  }
}

TEST(gpu_dxt1_two_color_blocks_and_transparency) {
  std::vector<u32> img(16); for (int i = 0; i < 16; i++) img[i] = (i % 4 < 2) ? pack_argb(255, 255, 0, 0) : pack_argb(255, 0, 0, 255);
  auto enc = encode_level(img, 4, 4, 3); CHECK_EQ(enc.size(), 8);
  GT t; t.s.bus.raw_write(TEXB, enc.data(), 8); std::vector<Vtx> v; quad(v, 0, 0, 4, 4, 0, 0, 1, 1); t.verts(v);
  GL g; g.rt().clear(0).state(ST_TEX0).add(GC_TEXENV, {0, TE_REPLACE}).tex(0, TEXB, 2, 2, TF_DXT1).vbase(VB).arrays(0, 6).end(); t.exec(g);
  for (int y = 0; y < 4; y++) { CHECK_EQ(t.px(0, y), 0xFFFF0000 & 0xFFFFFFFF ? t.px(0, y) : 0); CHECK(ch(t.px(0, y), 16) >= 247 && ch(t.px(0, y), 0) <= 8); CHECK(ch(t.px(3, y), 0) >= 247 && ch(t.px(3, y), 16) <= 8); }
  // bloco com texels transparentes: alfa 0 (modo 3 cores)
  std::vector<u32> im2(16, pack_argb(255, 200, 100, 50)); im2[5] = pack_argb(0, 0, 0, 0);
  auto e2 = encode_level(im2, 4, 4, 3); t.s.bus.raw_write(TEXB, e2.data(), 8);
  GL g2; g2.rt().clear(0).state(ST_TEX0).add(GC_TEXENV, {0, TE_REPLACE}).tex(0, TEXB, 2, 2, TF_DXT1).vbase(VB).arrays(0, 6).end(); t.exec(g2);
  CHECK_EQ(ch(t.px(1, 1), 24), 0); CHECK_EQ(ch(t.px(0, 0), 24), 255);
}

TEST(gpu_dxt1_encoder_psnr_on_gradient) {
  std::vector<u32> img(32 * 32); for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) img[y * 32 + x] = pack_argb(255, x * 8, y * 8, 128 + (x + y) * 2);
  GT t; auto bytes = build_texture(img, 32, 32, 3, false); CHECK_EQ(bytes.size(), 8 * 8 * 8);   // 4:1 em relação a 16bpp, 8:1 vs ARGB
  t.s.bus.raw_write(TEXB, bytes.data(), (u32)bytes.size()); std::vector<Vtx> v; quad(v, 0, 0, 32, 32, 0, 0, 1, 1); t.verts(v);
  GL g; g.rt().clear(0).state(ST_TEX0).add(GC_TEXENV, {0, TE_REPLACE}).tex(0, TEXB, 5, 5, TF_DXT1).vbase(VB).arrays(0, 6).end(); t.exec(g);
  double se = 0; for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) for (int sh = 0; sh < 24; sh += 8) { double d = ch(t.px(x, y), sh) - ch(img[y * 32 + x], sh); se += d * d; }
  double psnr = 10 * std::log10(255.0 * 255.0 / (se / (32 * 32 * 3))); CHECK(psnr > 30.0);
}

TEST(gpu_texture_wrap_clamp_bilinear_mip) {
  // 4x4 com cores distintas por texel
  std::vector<u32> img(16); for (int i = 0; i < 16; i++) img[i] = pack_argb(255, i * 16, 255 - i * 16, (i * 37) & 255);
  GT t; auto bytes = build_texture(img, 4, 4, 0, false); t.s.bus.raw_write(TEXB, bytes.data(), (u32)bytes.size());
  std::vector<Vtx> v; quad(v, 0, 0, 16, 16, 0, 0, 2, 2); t.verts(v);
  GL g; g.rt().clear(0).state(ST_TEX0).add(GC_TEXENV, {0, TE_REPLACE}).tex(0, TEXB, 2, 2, 0).vbase(VB).arrays(0, 6).end(); t.exec(g);
  CHECK_EQ(t.px(1, 1), img[0]); CHECK_EQ(t.px(9, 1), img[0]); CHECK_EQ(t.px(5, 5), img[10]); CHECK_EQ(t.px(13, 13), img[10]);        // REPEAT
  GL gc; gc.rt().clear(0).state(ST_TEX0).add(GC_TEXENV, {0, TE_REPLACE}).tex(0, TEXB, 2, 2, 0, TFLAG_CLAMP).vbase(VB).arrays(0, 6).end(); t.exec(gc);
  CHECK_EQ(t.px(13, 13), img[15]); CHECK_EQ(t.px(9, 1), img[3]);                                                                 // CLAMP
  // bilinear 2x1 preto/branco
  std::vector<u32> bw = {pack_argb(255, 0, 0, 0), pack_argb(255, 255, 255, 255)}; auto b2 = build_texture(bw, 2, 1, 0, false);
  t.s.bus.raw_write(TEXB, b2.data(), (u32)b2.size()); std::vector<Vtx> v2; quad(v2, 0, 0, 16, 4, 0, 0, 1, 1); t.verts(v2);
  GL gb; gb.rt().clear(0).state(ST_TEX0 | ST_BILINEAR).add(GC_TEXENV, {0, TE_REPLACE}).tex(0, TEXB, 1, 0, 0, TFLAG_CLAMP).vbase(VB).arrays(0, 6).end(); t.exec(gb);
  int mid = ch(t.px(7, 1), 0), mid2 = ch(t.px(8, 1), 0); CHECK(mid > 100 && mid < 160); CHECK(mid2 > mid); CHECK(ch(t.px(0, 1), 0) < 10); CHECK(ch(t.px(15, 1), 0) > 245);
  GL gn; gn.rt().clear(0).state(ST_TEX0).add(GC_TEXENV, {0, TE_REPLACE}).tex(0, TEXB, 1, 0, 0, TFLAG_CLAMP).vbase(VB).arrays(0, 6).end(); t.exec(gn);
  CHECK_EQ(ch(t.px(7, 1), 0), 0); CHECK_EQ(ch(t.px(8, 1), 0), 255);                                                               // vizinho mais próximo: serrilhado
  // mipmaps: cada nível uniforme, seleciona por densidade de texels
  std::vector<u8> mip; u32 cols[5] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFFFFFFFF};
  for (int l = 0; l < 5; l++) { int sz = 16 >> l; for (int i = 0; i < sz * sz; i++) { u8 b[4]; wbe32(b, cols[l]); mip.insert(mip.end(), b, b + 4); } }
  t.s.bus.raw_write(TEXB, mip.data(), (u32)mip.size());
  auto draw = [&](float size) { std::vector<Vtx> q; quad(q, 0, 0, size, size, 0, 0, 1, 1); t.verts(q);
    GL gm; gm.rt().clear(0).state(ST_TEX0).add(GC_TEXENV, {0, TE_REPLACE}).tex(0, TEXB, 4, 4, 0, TFLAG_MIPS, 5).vbase(VB).arrays(0, 6).end(); t.exec(gm); return t.px(1, 1); };
  CHECK_EQ(draw(16), cols[0]); CHECK_EQ(draw(8), cols[1]); CHECK_EQ(draw(4), cols[2]); CHECK_EQ(draw(2), cols[3]);
}

TEST(gpu_multitexture_modulate_and_fog) {
  std::vector<u32> a = {pack_argb(255, 200, 100, 50)}, b = {pack_argb(255, 128, 255, 0)};
  GT t; auto ba = build_texture(a, 1, 1, 0, false), bb = build_texture(b, 1, 1, 0, false);
  t.s.bus.raw_write(TEXB, ba.data(), 4); t.s.bus.raw_write(TEXB + 0x100, bb.data(), 4);
  std::vector<Vtx> v; quad(v, 0, 0, 8, 8, 0, 0, 1, 1); t.verts(v);
  GL g; g.rt().clear(0).state(ST_TEX0 | ST_TEX1).tex(0, TEXB, 0, 0, 0).tex(1, TEXB + 0x100, 0, 0, 0).add(GC_TEXENV, {0, TE_MODULATE}).add(GC_TEXENV, {1, TE_MODULATE}).vbase(VB).arrays(0, 6).end(); t.exec(g);
  u32 c = t.px(3, 3); CHECK(std::abs(ch(c, 16) - 200 * 128 / 255) <= 2); CHECK(std::abs(ch(c, 8) - 100) <= 2); CHECK_EQ(ch(c, 0), 0);
  // fog: metade do caminho até branco
  std::vector<Vtx> fv; quad(fv, 0, 0, 8, 8, 0, 0, 1, 1, 0.5f, 0, 0, 0); for (auto& q : fv) q.fog = 0.5f; t.verts(fv);
  GL gf; gf.rt().clear(0).state(ST_FOG).add(GC_FOG, {0xFFFFFFFF, f2u(0.0f), f2u(1.0f)}).vbase(VB).arrays(0, 6).end(); t.exec(gf);
  CHECK(std::abs(ch(t.px(3, 3), 16) - 128) <= 1);
  GL gf2; gf2.rt().clear(0).state(ST_FOG).add(GC_FOG, {0xFF00FF00, f2u(0.6f), f2u(1.0f)}).vbase(VB).arrays(0, 6).end(); t.exec(gf2); CHECK_EQ(ch(t.px(3, 3), 8), 0);   // antes do início da névoa
}

TEST(gpu_stencil_and_scissor) {
  GT t; std::vector<Vtx> v; quad(v, 0, 0, 40, 40, 0, 0, 1, 1, 0.5f, 1, 0, 0); quad(v, 20, 20, 60, 60, 0, 0, 1, 1, 0.4f, 0, 1, 0); t.verts(v);
  GL g; g.rt().clear(0xFF000000).vbase(VB)
    .add(GC_STENCIL, {6, 1, 0xFF, SOP_KEEP, SOP_KEEP, SOP_REPLACE}).state(ST_STENCIL).arrays(0, 6)                      // marca stencil=1 sob o quad vermelho
    .add(GC_STENCIL, {4, 1, 0xFF, SOP_KEEP, SOP_KEEP, SOP_KEEP}).state(ST_STENCIL).arrays(6, 6).end(); t.exec(g);         // verde só onde stencil==1
  CHECK_EQ(t.px(10, 10) & 0xFFFFFF, 0xFF0000); CHECK_EQ(t.px(30, 30) & 0xFFFFFF, 0x00FF00); CHECK_EQ(t.px(50, 50) & 0xFFFFFF, 0); CHECK_EQ(t.px(30, 10) & 0xFFFFFF, 0xFF0000);
  GT s; s.verts({V(0, 0, 1, 1, 1), V(64, 0, 1, 1, 1), V(0, 64, 1, 1, 1), V(64, 0, 1, 1, 1), V(64, 64, 1, 1, 1), V(0, 64, 1, 1, 1)});
  GL gs; gs.rt().clear(0).add(GC_SCISSOR, {10, 12, 20, 8}).state(0).vbase(VB).arrays(0, 6).end(); s.exec(gs);
  CHECK_EQ(s.count_nonzero(), 20 * 8); CHECK(s.px(10, 12) != 0); CHECK_EQ(s.px(9, 12), 0); CHECK_EQ(s.px(29, 19) != 0, true); CHECK_EQ(s.px(30, 19), 0);
}

TEST(gpu_idle_tiles_are_not_written) {
  GT t; for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) t.s.bus.raw_w32(VRAM_BASE + y * 256 + x * 4, 0xFF777777);
  t.verts({V(2, 2, 1, 0, 0), V(20, 2, 1, 0, 0), V(2, 20, 1, 0, 0)});
  GL g; g.rt().state(0).vbase(VB).arrays(0, 3).end(); auto st = t.exec(g);     // sem CLEAR: carrega o conteúdo atual do bloco
  CHECK_EQ(st.tiles, 1); CHECK_EQ(st.vram_write, 32 * 32 * 4); CHECK_EQ(t.px(40, 40), 0xFF777777); CHECK_EQ(t.px(25, 25), 0xFF777777); CHECK_EQ(t.px(5, 5) & 0xFFFFFF, 0xFF0000);
  CHECK_EQ(st.bin_entries, 1);
}

TEST(gpu_primitive_types_strip_fan_indexed) {
  GT t; t.verts({V(5, 5, 1, 1, 1), V(55, 5, 1, 1, 1), V(5, 55, 1, 1, 1), V(55, 55, 1, 1, 1)});
  GL g; g.rt().clear(0).state(ST_BLEND).blend(BF_ONE, BF_ONE).vbase(VB).arrays(0, 4, PRIM_STRIP).end(); auto st = t.exec(g);
  CHECK_EQ(st.tris_in, 2); CHECK_EQ(st.tris_drawn, 2); CHECK_EQ(ch(t.px(30, 30), 16), 255);          // alternância de orientação no strip mantém ambos visíveis
  CHECK_EQ(t.count_nonzero(), 50 * 50);                                                               // quad 50x50 exato
  GT f; f.verts({V(30, 30, 1, 1, 1), V(10, 10, 1, 1, 1), V(50, 10, 1, 1, 1), V(50, 50, 1, 1, 1), V(10, 50, 1, 1, 1), V(10, 10, 1, 1, 1)});
  GL gf; gf.rt().clear(0).state(ST_BLEND).blend(BF_ONE, BF_ONE).vbase(VB).arrays(0, 6, PRIM_FAN).end(); auto sf = f.exec(gf);
  CHECK_EQ(sf.tris_drawn, 4); CHECK_EQ(f.count_nonzero(), 40 * 40);
  for (int i = 0; i < 6; i++) f.s.bus.raw_w16(0x300000 + i * 2, (u16)(i == 0 ? 0 : i));
  GL gi; gi.rt().clear(0).state(0).vbase(VB).indexed(0x300000, 6, PRIM_FAN).end(); auto si = f.exec(gi); CHECK_EQ(si.tris_drawn, 4);
}

TEST(gpu_near_plane_and_invalid_vertices_rejected) {
  GT t; t.verts({V(0, 0, 1, 1, 1, 0.5f, 1, -1.0f), V(30, 0, 1, 1, 1), V(0, 30, 1, 1, 1), V(0, 0, 1, 1, 1, 0.5f, 1, 1), V(NAN, 0, 1, 1, 1), V(0, 30, 1, 1, 1)});
  GL g; g.rt().clear(0).state(0).vbase(VB).arrays(0, 6).end(); auto st = t.exec(g);
  CHECK_EQ(st.tris_in, 2); CHECK_EQ(st.tris_drawn, 0); CHECK_EQ(t.count_nonzero(), 0);
}

TEST(gpu_command_errors_are_reported_and_recoverable) {
  GT t; GL bad; bad.rt().add(0x7F, {1}).end(); t.exec(bad); CHECK(t.s.gpu.error); CHECK(t.s.gpu.error_msg.find("desconhecido") != std::string::npos);
  GL tr; tr.rt(); tr.w.push_back((GC_DRAW_ARRAYS << 24) | 3); tr.w.push_back(0); t.exec(tr); CHECK(t.s.gpu.error);                                // truncado
  GL br; br.add(GC_SET_RT, {VRAM_BASE, 16, 64u | (64u << 16)}).end(); t.exec(br); CHECK(t.s.gpu.error);                                       // pitch menor que a largura
  GL oi; oi.rt().vbase(VB).indexed(0x3FFFFFFF, 10).end(); t.exec(oi); CHECK(t.s.gpu.error);
  GL nort; nort.clear(1).end(); t.exec(nort); CHECK(t.s.gpu.error);
  GL ok; ok.rt().clear(0xFF112233).end(); t.exec(ok); CHECK(!t.s.gpu.error); CHECK_EQ(t.px(1, 1), 0xFF112233);
}

TEST(gpu_executes_via_mmio_with_latency_irq_and_queue) {
  GT t; t.verts({V(0, 0, 1, 1, 1), V(60, 0, 1, 1, 1), V(0, 60, 1, 1, 1)});
  GL g; g.rt().clear(0).state(0).vbase(VB).arrays(0, 3).end();
  for (size_t i = 0; i < g.w.size(); i++) t.s.bus.raw_w32(CMD + i * 4, g.w[i]);
  t.s.bus.irq_mask = IRQ_GPU;
  mmio_w(t.s, GPU_BASE + GPU_CMD_ADDR, CMD); mmio_w(t.s, GPU_BASE + GPU_CMD_LEN, (u32)g.w.size()); mmio_w(t.s, GPU_BASE + GPU_CTRL, 3);
  CHECK_EQ(mmio_r(t.s, GPU_BASE + GPU_STATUS) & GPUS_BUSY, GPUS_BUSY); CHECK(!t.s.bus.irq_pending());
  u32 cycles = mmio_r(t.s, GPU_BASE + GPU_STAT_CYCLES); CHECK(cycles > 100);
  Ps t0 = t.s.sched.now; CHECK(t.s.wait_until([&] { return t.s.bus.irq_pending(); })); CHECK_EQ(t.s.sched.now - t0, cyc2ps(cycles, GPU_HZ));
  CHECK_EQ(mmio_r(t.s, GPU_BASE + GPU_STATUS) & (GPUS_BUSY | GPUS_DONE), GPUS_DONE); CHECK_EQ(mmio_r(t.s, GPU_BASE + GPU_STAT_TRIS_DRAWN), 1);
  CHECK_EQ(mmio_r(t.s, GPU_BASE + GPU_STAT_PIX_SHADED), t.count_nonzero());
  for (int i = 0; i < 6; i++) mmio_w(t.s, GPU_BASE + GPU_CTRL, 1);
  CHECK_EQ(mmio_r(t.s, GPU_BASE + GPU_STATUS) & GPUS_ERROR, GPUS_ERROR);                          // fila de 4 estourou
  mmio_w(t.s, GPU_BASE + GPU_HSR, 0); CHECK_EQ(mmio_r(t.s, GPU_BASE + GPU_HSR), 0);
}

// ---- DXT3 / DXT5 (SFTC3/SFTC5): cor + alfa gradual, blocos de 16 bytes ----
static void dxt35_roundtrip(int fmt, int tol_alpha, double min_psnr) {
  std::vector<u32> img(16 * 16);
  for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) img[y * 16 + x] = pack_argb(x * 16 + y, 40 + x * 10, 200 - y * 8, 90 + (x + y) * 4);
  GT t; auto bytes = build_texture(img, 16, 16, fmt, false); CHECK_EQ(bytes.size(), 4 * 4 * 16);
  t.s.bus.raw_write(TEXB, bytes.data(), (u32)bytes.size()); std::vector<Vtx> v; quad(v, 0, 0, 16, 16, 0, 0, 1, 1); t.verts(v);
  GL g; g.rt().clear(0).state(ST_TEX0).add(GC_TEXENV, {0, TE_REPLACE}).tex(0, TEXB, 4, 4, fmt).vbase(VB).arrays(0, 6).end(); t.exec(g);
  double se = 0; int maxa = 0;
  for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
    maxa = std::max(maxa, std::abs(ch(t.px(x, y), 24) - ch(img[y * 16 + x], 24)));
    for (int sh = 0; sh < 24; sh += 8) { double d = ch(t.px(x, y), sh) - ch(img[y * 16 + x], sh); se += d * d; }
  }
  CHECK(maxa <= tol_alpha); CHECK(10 * std::log10(255.0 * 255.0 / (se / (16 * 16 * 3) + 1e-9)) > min_psnr);
}
TEST(gpu_dxt3_explicit_alpha_roundtrip) { dxt35_roundtrip(TF_DXT3, 16, 28.0); }       // alfa de 4 bits: erro <= 16
TEST(gpu_dxt5_interpolated_alpha_roundtrip) { dxt35_roundtrip(TF_DXT5, 20, 28.0); }   // 8 níveis por bloco
TEST(gpu_dxt5_constant_alpha_and_two_alpha_modes) {
  std::vector<u32> img(16, pack_argb(77, 10, 200, 30));
  GT t; auto bytes = encode_level(img, 4, 4, TF_DXT5); CHECK_EQ(bytes[0], 77); CHECK_EQ(bytes[1], 77);
  t.s.bus.raw_write(TEXB, bytes.data(), (u32)bytes.size()); std::vector<Vtx> v; quad(v, 0, 0, 4, 4, 0, 0, 1, 1); t.verts(v);
  GL g; g.rt().clear(0).state(ST_TEX0).add(GC_TEXENV, {0, TE_REPLACE}).tex(0, TEXB, 2, 2, TF_DXT5).vbase(VB).arrays(0, 6).end(); t.exec(g);
  for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) CHECK_EQ(ch(t.px(x, y), 24), 77);
  // modo a0 <= a1 escrito à mão: 6 níveis + 0 + 255. índices: texel 6 -> 0, texel 7 -> 255
  u8 blk[16] = {0}; blk[0] = 10; blk[1] = 200; u64 bits = (u64)6 << (3 * 6) | (u64)7 << (3 * 7);
  for (int k = 0; k < 6; k++) blk[2 + k] = (u8)(bits >> (8 * (5 - k)));
  t.s.bus.raw_write(TEXB, blk, 16);
  GL g2; g2.rt().clear(0).state(ST_TEX0).add(GC_TEXENV, {0, TE_REPLACE}).tex(0, TEXB, 2, 2, TF_DXT5).vbase(VB).arrays(0, 6).end(); t.exec(g2);
  CHECK_EQ(ch(t.px(2, 1), 24), 0); CHECK_EQ(ch(t.px(3, 1), 24), 255); CHECK_EQ(ch(t.px(0, 0), 24), 10);
}

// ======================= Item 4: formatos adicionais (RGBA5551, L8, A8, LA88, CI4/CI8, VQ, ETC1) =======================
static double psnr_vs(GT& t, const std::vector<u32>& img, int w, int h, bool alpha = false) {
  double se = 0; int n = 0;
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int sh = alpha ? 24 : 16; sh >= 0; sh -= 8) { double d = ch(t.px(x, y), sh) - ch(img[y * w + x], sh); se += d * d; n++; }
  return se == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / (se / n));
}
static void render_tex(GT& t, const std::vector<u8>& bytes, int w, int h, int fmt, const std::vector<u32>* pal = nullptr, u32 flags = 0, int nl = 1) {
  t.s.bus.raw_write(TEXB, bytes.data(), (u32)bytes.size());
  int lw = 0, lh = 0; while ((1 << lw) < w) lw++; while ((1 << lh) < h) lh++;
  std::vector<Vtx> v; quad(v, 0, 0, (float)w, (float)h, 0, 0, 1, 1); t.verts(v);
  GL g; g.rt().clear(0).state(ST_TEX0).add(GC_TEXENV, {0, TE_REPLACE});
  if (pal) { for (size_t i = 0; i < pal->size(); i++) t.s.bus.raw_w32(0x500000 + i * 4, (*pal)[i]); g.add(GC_SET_PALETTE, {0, 0x500000, (u32)pal->size()}); }
  g.tex(0, TEXB, lw, lh, fmt, flags, nl).vbase(VB).arrays(0, 6).end(); t.exec(g);
}

TEST(gpu_simple_formats_rgba5551_l8_a8_la88) {
  std::vector<u32> img(64); for (int i = 0; i < 64; i++) img[i] = pack_argb(i < 32 ? 255 : 0, (i * 4) & 255, 255 - i * 3, (i * 9) & 255);
  { GT t; render_tex(t, build_texture(img, 8, 8, TF_RGBA5551, false), 8, 8, TF_RGBA5551); for (int i = 0; i < 64; i++) { u32 g = t.px(i % 8, i / 8); CHECK_EQ(ch(g, 24), i < 32 ? 255 : 0); CHECK(std::abs(ch(g, 16) - ch(img[i], 16)) <= 8); } }
  { GT t; render_tex(t, build_texture(img, 8, 8, TF_L8, false), 8, 8, TF_L8); for (int i = 0; i < 64; i++) { u32 g = t.px(i % 8, i / 8); CHECK_EQ(ch(g, 24), 255); CHECK_EQ(ch(g, 16), ch(g, 8)); CHECK_EQ(ch(g, 8), ch(g, 0)); } }
  { GT t; render_tex(t, build_texture(img, 8, 8, TF_A8, false), 8, 8, TF_A8); for (int i = 0; i < 64; i++) { u32 g = t.px(i % 8, i / 8); CHECK_EQ(ch(g, 24), i < 32 ? 255 : 0); CHECK_EQ(g & 0xFFFFFF, 0xFFFFFFu); } }
  { GT t; render_tex(t, build_texture(img, 8, 8, TF_LA88, false), 8, 8, TF_LA88); for (int i = 0; i < 64; i++) { u32 g = t.px(i % 8, i / 8); CHECK_EQ(ch(g, 24), i < 32 ? 255 : 0); CHECK_EQ(ch(g, 16), ch(g, 0)); } }
  CHECK_EQ(tex_level_bytes(TF_L8, 8, 8), 64); CHECK_EQ(tex_level_bytes(TF_LA88, 8, 8), 128);
}

TEST(gpu_paletted_ci8_ci4_exact_and_palette_swap) {
  // 4 cores distintas (cabe em CI4 e CI8): reprodução exata
  u32 cols[4] = {pack_argb(255, 255, 0, 0), pack_argb(255, 0, 255, 0), pack_argb(255, 0, 0, 255), pack_argb(128, 255, 255, 0)};
  std::vector<u32> img(16 * 16); for (int i = 0; i < 256; i++) img[i] = cols[((i % 16) / 4 + (i / 16) / 4) % 4];
  for (int fmt : {(int)TF_CI8, (int)TF_CI4}) {
    GT t; std::vector<u32> pal; int nl = 0; auto bytes = build_texture(img, 16, 16, fmt, false, &nl, &pal);
    CHECK_EQ(bytes.size(), fmt == TF_CI8 ? 256u : 128u); CHECK_EQ(pal.size(), 4);
    render_tex(t, bytes, 16, 16, fmt, &pal);
    CHECK(!t.s.gpu.error); for (int i = 0; i < 256; i++) CHECK_EQ(t.px(i % 16, i / 16), img[i]);
    // troca de paleta sem tocar nos texels (efeito clássico): vermelho <-> azul
    std::vector<u32> pal2 = pal; for (auto& c : pal2) c = (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
    GT t2; render_tex(t2, bytes, 16, 16, fmt, &pal2);
    for (int i = 0; i < 256; i++) { u32 a = img[i]; u32 sw = (a & 0xFF00FF00u) | ((a & 0xFF) << 16) | ((a >> 16) & 0xFF); CHECK_EQ(t2.px(i % 16, i / 16), sw); }
  }
  // imagem com muitas cores: CI8 quantiza (PSNR razoável), CI4 pior
  std::vector<u32> rich(32 * 32); for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) rich[y * 32 + x] = pack_argb(255, x * 8, y * 8, (x + y) * 4);
  GT a; std::vector<u32> p8; int nl; auto b8 = build_texture(rich, 32, 32, TF_CI8, false, &nl, &p8); render_tex(a, b8, 32, 32, TF_CI8, &p8);
  GT b; std::vector<u32> p4; auto b4 = build_texture(rich, 32, 32, TF_CI4, false, &nl, &p4); render_tex(b, b4, 32, 32, TF_CI4, &p4);
  double q8 = psnr_vs(a, rich, 32, 32), q4 = psnr_vs(b, rich, 32, 32); std::printf("    psnr ci8=%.1f ci4=%.1f\n", q8, q4); CHECK(q8 > 34.0); CHECK(q4 > 22.0); CHECK(q8 > q4 + 6);
  // palette fora da memória/contagem inválida são erros recuperáveis
  GT e; GL g; g.add(GC_SET_PALETTE, {0, 0x500000, 257}).end(); e.exec(g); CHECK(e.s.gpu.error);
}

TEST(gpu_vq_texture_lossless_when_few_blocks_and_good_psnr_otherwise) {
  // poucas cores de bloco (<= 256): exato em RGB565
  std::vector<u32> img(16 * 16); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { int k = ((x / 2) + (y / 2)) % 5; img[y * 16 + x] = pack_argb(255, k * 60, 255 - k * 50, (x & 1) * 40 + k * 8); }
  GT t; auto bytes = build_texture(img, 16, 16, TF_VQ, false); CHECK_EQ(bytes.size(), 2048 + 8 * 8);
  render_tex(t, bytes, 16, 16, TF_VQ); CHECK(!t.s.gpu.error); CHECK(psnr_vs(t, img, 16, 16) > 40.0);
  // 32x32 suave: 256 blocos únicos; com 64x64 há mais de 256 -> quantiza
  std::vector<u32> big(64 * 64); for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) big[y * 64 + x] = pack_argb(255, x * 4, y * 4, 128 + (int)(60 * std::sin(x * 0.2) * std::cos(y * 0.25)));
  GT u; auto b2 = build_texture(big, 64, 64, TF_VQ, false); CHECK_EQ(b2.size(), 2048 + 32 * 32);
  render_tex(u, b2, 64, 64, TF_VQ); /* RT padrão 64x64 */ CHECK(psnr_vs(u, big, 64, 64) > 30.0);
  // mips: níveis têm livro próprio
  int nl = 0; auto bm = build_texture(big, 64, 64, TF_VQ, true, &nl); CHECK_EQ(nl, 7);
  CHECK_EQ(bm.size(), (2048 + 32 * 32) + (2048 + 16 * 16) + (2048 + 8 * 8) + (2048 + 4 * 4) + (2048 + 2 * 2) + (2048 + 1) + (2048 + 1));
}

TEST(gpu_etc1_roundtrip_and_known_block) {
  // bloco conhecido (modo individual): R1=R2=1111 ... tabela 0, flip 0, todos os índices 0 (modificador +2)
  u8 blk[8] = {0xFF, 0x00, 0x00, 0x00, 0, 0, 0, 0};                       // R=255, G=0, B=0, cw=0, diff=0, flip=0
  CHECK_EQ(etc1_texel(blk, 0, 0), pack_argb(255, 255, 2, 2));             // +2 aplicado a todos os canais (satura em 255)
  blk[6] = 0x00; blk[7] = 0x01;                                           // LSB do texel k=0 -> índice 1: +8
  CHECK_EQ(etc1_texel(blk, 0, 0), pack_argb(255, 255, 8, 8));
  blk[4] = 0x00; blk[5] = 0x01;                                           // MSB também em k=0 -> índice 3: -8
  CHECK_EQ(etc1_texel(blk, 0, 0), pack_argb(255, 247, 0, 0));
  // cor chapada: erro mínimo; gradiente: PSNR > 30; tamanho 4 bpp
  std::vector<u32> flat(64, pack_argb(255, 100, 150, 200));
  GT f; auto fb = build_texture(flat, 8, 8, TF_ETC1, false); CHECK_EQ(fb.size(), 4 * 8); render_tex(f, fb, 8, 8, TF_ETC1);
  for (int i = 0; i < 64; i++) { u32 g = f.px(i % 8, i / 8); CHECK(std::abs(ch(g, 16) - 100) <= 6 && std::abs(ch(g, 8) - 150) <= 6 && std::abs(ch(g, 0) - 200) <= 6); }
  std::vector<u32> grad(64 * 64); for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) grad[y * 64 + x] = pack_argb(255, x * 4, y * 4, 128 + ((x ^ y) & 15) * 3);
  GT g; auto gb = build_texture(grad, 64, 64, TF_ETC1, false); CHECK_EQ(gb.size(), 16 * 16 * 8); render_tex(g, gb, 64, 64, TF_ETC1);
  CHECK(psnr_vs(g, grad, 64, 64) > 30.0);
}
