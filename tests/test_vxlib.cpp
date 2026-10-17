// Microprogramas padrão da SnowVX (os/vx/*.vsm): geometria na VPU-A e partículas na VPU-B, com DMA local e SIGNAL.
#include "helpers.h"
#include <cmath>
#include <fstream>
#include <sstream>

static std::vector<u32> vsm(const char* path) {
  std::ifstream f(path); std::stringstream ss; ss << f.rdbuf();
  std::vector<u32> p; std::string err; if (!vpu_assemble(ss.str(), p, err)) report_fail(__FILE__, __LINE__, std::string(path) + ": " + err);
  return p;
}

TEST(vxlib_geometry_transforms_lights_and_signals) {
  Soc s; auto prog = vsm("os/vx/geometry.vsm"); CHECK(prog.size() * 4 <= VPU_IMEM_SIZE); s.vpuA.load_program(prog);
  const u32 IN = 0x100000, OUT = 0x110000, K = 0x120000; const int N = 5;
  float L[3] = {0, 0, 1}, H[3] = {0, 0, 1};
  float k[18][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 2}, {0, 0, -1, 0},      // MVP (colunas): clip = (x, y, z - 1, 2z)
                    {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {L[0], L[1], L[2], 0}, {100, -100, 0.5f, 0}, {320, 240, 0.5f, 0}, {0, 0, 0, 1},
                    {0.6f, 0.6f, 0.6f, 0}, {0, 0, 0, 0}, {0.2f, 0.2f, 0.2f, 1}, {1, 1, 1, 1}, {H[0], H[1], H[2], 0}, {0.5f, 0.5f, 0.5f, 0}, {0, 0, 1, 0.5f}};
  for (int i = 0; i < 18; i++) put_vec(s, K + i * 16, k[i][0], k[i][1], k[i][2], k[i][3]);
  for (int i = 0; i < N; i++) {
    float a = i * 0.7f;
    put_vec(s, IN + i * 64, 0.5f * i - 1, 0.25f * i, 2 + i, 1); put_vec(s, IN + i * 64 + 16, std::sin(a) * 3, 0, std::cos(a) * 3, 0);
    put_vec(s, IN + i * 64 + 32, 1, 0.5f, 0.25f, 0.8f); put_vec(s, IN + i * 64 + 48, 0.1f * i, 0.2f, 0, 0);
  }
  u32 sr[8] = {IN, OUT, N * 64, K, N * 48, 0, 0, 0};
  CHECK(s.vpuA.submit(0, sr)); s.wait_until([&] { return !s.vpuA.busy(); });
  CHECK(!(s.vpuA.status() & VPUS_FAULT)); CHECK_EQ(s.bus.sem[0], 1); CHECK_EQ(s.vpuA.dma_bytes, 288 + N * 64 + N * 48);
  for (int i = 0; i < N; i++) {
    float x = 0.5f * i - 1, y = 0.25f * i, z = 2 + i, cz = z - 1, w = 2 * z, iw = 1 / w;      // clip = (x, y, z - 1, 2z)
    float sx = x * iw * 100 + 320, sy = y * iw * -100 + 240, sz = cz * iw * 0.5f + 0.5f;
    float a = i * 0.7f, nz = std::cos(a), ndl = std::max(0.0f, nz), sp = std::pow(ndl, 16.0f), rim = (1 - std::fabs(nz)) * (1 - std::fabs(nz));
    float lit = 0.6f * ndl + 0.2f;
    float er = std::min(1.0f, lit * 1 + sp * 0.5f), eb = std::min(1.0f, lit * 0.25f + sp * 0.5f + rim), ea = std::min(1.0f, 0.8f + rim * 0.5f);
    u32 o = OUT + i * 48;
    CHECK_NEAR(s.bus.raw_rf(o), sx, 1e-3); CHECK_NEAR(s.bus.raw_rf(o + 4), sy, 1e-3); CHECK_NEAR(s.bus.raw_rf(o + 8), sz, 1e-5); CHECK_NEAR(s.bus.raw_rf(o + 12), iw, 1e-6);
    CHECK_NEAR(s.bus.raw_rf(o + 16), er, 1e-4); CHECK_NEAR(s.bus.raw_rf(o + 24), eb, 1e-4); CHECK_NEAR(s.bus.raw_rf(o + 28), ea, 1e-4);
    CHECK_NEAR(s.bus.raw_rf(o + 32), 0.1f * i, 1e-7);
  }
}

TEST(vxlib_dynamics_particles_wrap_and_emit_quads) {
  Soc s; auto prog = vsm("os/vx/dynamics.vsm"); s.vpuB.load_program(prog);
  const u32 OUT = 0x200000, K = 0x210000; const int N = 3;
  float c[16][4] = {{1, 1, 1, 0}, {-0.1f, 0.1f, 0, 0}, {0.02f, 0, 0, 0}, {640, 480, 1, 0}, {0, 0, 0, 1}, {1, 1, 1, 1}, {0, 0, 0, 0},
                    {-1, -1, 0, 0}, {1, -1, 0, 0}, {1, 1, 0, 0}, {-1, 1, 0, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}, {1, 1, 0, 0}, {0, 1, 0, 0}, {4, 4, 4, 4}};
  for (int i = 0; i < 16; i++) put_vec(s, K + i * 16, c[i][0], c[i][1], c[i][2], c[i][3]);
  auto st = [&](int i, int slot, float x, float y, float z, float w) { u8* p = s.vpuB.dmem + 0x100 + i * 64 + slot * 16; wbef(p, x); wbef(p + 4, y); wbef(p + 8, z); wbef(p + 12, w); };
  for (int i = 0; i < N; i++) { st(i, 0, 0.5f, 0.9f + 0.05f * i, 0.5f, 0); st(i, 1, 0, 0.125f, 0, 4.0f + i); st(i, 2, 1, 0, 0, 0); st(i, 3, 1, 0.5f, 0.25f, 1); }
  u32 sr[8] = {0x100, 0, 0x100 + N * 64, OUT, N * 288, K, 0, 0};
  for (int f = 0; f < 2; f++) { CHECK(s.vpuB.submit(0, sr)); s.wait_until([&] { return !s.vpuB.busy(); }); CHECK(!(s.vpuB.status() & VPUS_FAULT)); }
  CHECK_EQ(s.bus.sem[1], 2);
  for (int i = 0; i < N; i++) {
    float y = std::fmod(0.9f + 0.05f * i + 0.25f, 1.0f);                       // passou da borda: deu a volta
    CHECK_NEAR(rbef(s.vpuB.dmem + 0x100 + i * 64 + 4), y, 1e-5);
    float osc_c = rbef(s.vpuB.dmem + 0x100 + i * 64 + 32), osc_s = rbef(s.vpuB.dmem + 0x100 + i * 64 + 36);
    CHECK_NEAR(osc_c * osc_c + osc_s * osc_s, 1.0, 1e-5); CHECK(osc_s > 0.15f);   // girou duas vezes, raio 1
    float cx = (0.5f + 0.02f * osc_s) * 640, cy = y * 480, sz = 4.0f + i, fade = std::min(1.0f, 4 * (1 - std::fabs(2 * y - 1)));
    u32 o = OUT + i * 288;
    CHECK_NEAR(s.bus.raw_rf(o), cx - sz, 1e-3); CHECK_NEAR(s.bus.raw_rf(o + 4), cy - sz, 1e-3); CHECK_NEAR(s.bus.raw_rf(o + 12), 1.0, 0);
    CHECK_NEAR(s.bus.raw_rf(o + 48 * 2), cx + sz, 1e-3); CHECK_NEAR(s.bus.raw_rf(o + 48 * 2 + 4), cy + sz, 1e-3);   // canto oposto
    CHECK_NEAR(s.bus.raw_rf(o + 48 * 5), cx - sz, 1e-3); CHECK_NEAR(s.bus.raw_rf(o + 48 * 5 + 4), cy + sz, 1e-3);
    CHECK_NEAR(s.bus.raw_rf(o + 16 + 4), 0.5f * fade, 1e-5); CHECK_NEAR(s.bus.raw_rf(o + 48 * 2 + 32), 1.0, 0);    // cor com fade; UV do canto
  }
}
