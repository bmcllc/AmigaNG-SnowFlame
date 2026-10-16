// Demo integrada do SnowFlame SF-100: um "jogo" completo rodando num programa ColdFire que orquestra
// DMA, 2 VPUs, GPU, Blitter, Copper, vídeo e áudio exclusivamente pela ABI (MMIO).
#pragma once
#include "soc.h"
#include "cfasm.h"
#include "kernels.h"
#include "texutil.h"
#include "adpcm.h"
#include "util.h"

namespace sf { namespace demo {
using A = CfAsm;
using namespace abi;

// ---- mapa de memória da demo ----
constexpr u32 CODE = 0x00001000;
constexpr u32 GPU_LIST = 0x00110000, CONST_DDR = 0x00120000, PCONST_DDR = 0x00130000, PBUF_OUT = 0x00132000,
              SPRITE = 0x00140000, COPPER_LIST = 0x00150000, SPRITE_TAB = 0x00153000, SPRITE_LIST = 0x00154000, VERT_DDR = 0x00160000,
              GEO_OUT = 0x00300000, IDX_CUBE = 0x00310000, IDX_SPHERE = 0x00311000, OVERLAY = 0x00200000;
constexpr u32 RT = VRAM_BASE, RT_PITCH = 1280, TEX_CUBE = VRAM_BASE + 0x200000, TEX_SPHERE = VRAM_BASE + 0x220000;
constexpr int W = 320, H = 240, NPART = 64;
constexpr int CUBE_V = 24, SPH_SEG = 12, SPH_RING = 8, SPH_V = (SPH_SEG + 1) * (SPH_RING + 1);
constexpr u32 DM_CUBE_IN = 0x200, DM_SPH_IN = DM_CUBE_IN + CUBE_V * 48, DM_OUT = 0x2000, DM_SPH_OUT = DM_OUT + CUBE_V * 48;
constexpr u32 VERT_BYTES = (CUBE_V + SPH_V) * 48;

struct FrameStats { Ps cpu_ps = 0; u64 vpuA_cycles = 0, vpuB_cycles = 0, gpu_cycles = 0, tris = 0, shaded = 0, covered = 0, tiles = 0, blits = 0; };

class SnowDemo {
 public:
  Soc soc; std::vector<u16> cube_idx, sph_idx; u32 gpu_words = 0; u32 boot_entry = 0, frame_entry = 0;
  std::vector<u32> checker;

  SnowDemo() { build_assets(); build_gpu_list(); build_copper(); build_audio(); build_program(); init_video(); }

  void boot() {
    soc.cpu.reset_regs(); soc.cpu.a[7] = 0x00F00000; soc.cpu.set_pc(boot_entry); soc.run_until_halt(1000000000000ull);
    soc.bus.cache_flush_all(false);
  }
  FrameStats frame(int f) {
    update_constants(f);
    FrameStats fs; u64 a0 = soc.vpuA.total_cycles, b0 = soc.vpuB.total_cycles, g0 = soc.gpu.total.cycles, bl0 = soc.blt.jobs;
    soc.cpu.reset_regs(); soc.cpu.a[7] = 0x00F00000; soc.cpu.set_pc(frame_entry);
    Ps t0 = soc.sched.now; bool ok = soc.run_until_halt(FRAME_PS * 4); fs.cpu_ps = soc.sched.now - t0;
    if (!ok) throw std::runtime_error("programa de quadro não terminou");
    fs.vpuA_cycles = soc.vpuA.total_cycles - a0; fs.vpuB_cycles = soc.vpuB.total_cycles - b0; fs.gpu_cycles = soc.gpu.total.cycles - g0;
    fs.tris = soc.gpu.last.tris_drawn; fs.shaded = soc.gpu.last.pix_shaded; fs.covered = soc.gpu.last.pix_covered; fs.tiles = soc.gpu.last.tiles;
    soc.wait_until([&] { return soc.blt.jobs >= bl0 + 1 && !soc.blt.busy(); }); fs.blits = soc.blt.jobs - bl0;
    u32 target = soc.video.frames + 1; soc.wait_until([&] { return soc.video.frames >= target; }, FRAME_PS * 3);
    return fs;
  }

 private:
  Bus& bus() { return soc.bus; }
  void put_f(u32 a, float f) { bus().raw_w32(a, f2u(f)); }
  void put_v(u32 a, float x, float y, float z, float w) { put_f(a, x); put_f(a + 4, y); put_f(a + 8, z); put_f(a + 12, w); }
  void put_vertex(u32 a, float px, float py, float pz, float nx, float ny, float nz, float u, float v) { put_v(a, px, py, pz, 1); put_v(a + 16, nx, ny, nz, 0); put_v(a + 32, u, v, 0, 0); }

  void build_assets() {
    // cubo: 6 faces x 4 vértices
    const float S = 0.8f; int vi = 0; u32 a = VERT_DDR;
    float N[6][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    float U[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}};
    float Vv[6][3] = {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    for (int f = 0; f < 6; f++) {
      int su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1}; float uu[4] = {0, 1, 1, 0}, vv[4] = {1, 1, 0, 0};
      for (int c = 0; c < 4; c++) {
        float p[3]; for (int k = 0; k < 3; k++) p[k] = S * (N[f][k] + su[c] * U[f][k] + sv[c] * Vv[f][k]);
        put_vertex(a + vi * 48, p[0], p[1], p[2], N[f][0], N[f][1], N[f][2], uu[c], vv[c]); vi++;
      }
      u16 b = (u16)(f * 4); for (u16 i : {0, 1, 2, 0, 2, 3}) cube_idx.push_back(b + i);
    }
    // esfera
    const float R = 0.9f;
    for (int r = 0; r <= SPH_RING; r++) for (int s = 0; s <= SPH_SEG; s++) {
      double th = M_PI * r / SPH_RING, ph = 2 * M_PI * s / SPH_SEG; float nx = (float)(std::sin(th) * std::cos(ph)), ny = (float)std::cos(th), nz = (float)(std::sin(th) * std::sin(ph));
      put_vertex(a + vi * 48, R * nx, R * ny, R * nz, nx, ny, nz, (float)s / SPH_SEG, (float)r / SPH_RING); vi++;
    }
    for (int r = 0; r < SPH_RING; r++) for (int s = 0; s < SPH_SEG; s++) {
      u16 i0 = (u16)(CUBE_V + r * (SPH_SEG + 1) + s), i1 = i0 + 1, i2 = (u16)(i0 + SPH_SEG + 1), i3 = i2 + 1;
      for (u16 i : {i0, i2, i1, i1, i2, i3}) sph_idx.push_back(i);
    }
    for (size_t i = 0; i < cube_idx.size(); i++) bus().raw_w16(IDX_CUBE + i * 2, cube_idx[i]);
    for (size_t i = 0; i < sph_idx.size(); i++) bus().raw_w16(IDX_SPHERE + i * 2, sph_idx[i]);
    // texturas: xadrez DXT1 com mipmaps (cubo) e listras RGB565 (esfera)
    std::vector<u32> img(64 * 64);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { bool c = ((x / 8) + (y / 8)) & 1; img[y * 64 + x] = c ? pack_argb(255, 240, 240, 230) : pack_argb(255, 200, 60, 40); }
    int nl = 0; auto t1 = build_texture(img, 64, 64, TF_DXT1, true, &nl); bus().raw_write(TEX_CUBE, t1.data(), (u32)t1.size()); cube_levels = nl;
    std::vector<u32> sp(128 * 64);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 128; x++) { float t = 0.5f + 0.5f * std::sin(x * 0.35f + y * 0.1f); sp[y * 128 + x] = pack_argb(255, (int)(40 + 200 * t), (int)(90 + 100 * (1 - t)), (int)(200 - 120 * t)); }
    auto t2 = build_texture(sp, 128, 64, TF_RGB565, false); bus().raw_write(TEX_SPHERE, t2.data(), (u32)t2.size());
    // sprite 8x8 (brilho circular com alfa)
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
      float d = std::hypot(x - 3.5f, y - 3.5f) / 4.0f; float al = std::max(0.0f, 1.0f - d); bus().raw_w32(SPRITE + y * 32 + x * 4, pack_argb((int)(255 * al), 255, (int)(200 + 55 * al), 80));
    }
    // partículas: constantes + estado inicial
    u32 st = 2024; auto rnd = [&]() { st = st * 1664525u + 1013904223u; return (st >> 8) / 16777216.0f; };
    put_v(PCONST_DDR + 0, 1.0f / 60, 1.0f / 60, 1.0f / 60, 1.0f / 60); put_v(PCONST_DDR + 16, 0, -9.8f, 0, 0); put_v(PCONST_DDR + 32, 0, 0, 0, 0);
    put_v(PCONST_DDR + 48, 0, 1, 0, 0); put_v(PCONST_DDR + 64, 0, -2, 0, 0); put_v(PCONST_DDR + 80, 1000, 1000, 1000, 1000);
    put_v(PCONST_DDR + 96, -1, -1, -1, -1); put_v(PCONST_DDR + 112, 0.65f, 0.65f, 0.65f, 0.65f);
    for (int i = 0; i < NPART; i++) {
      put_v(PCONST_DDR + 0x100 + i * 32, (rnd() - 0.5f) * 3.0f, 0.2f + rnd() * 2.3f, 0, 1);
      put_v(PCONST_DDR + 0x100 + i * 32 + 16, (rnd() - 0.5f) * 1.2f, rnd() * 4.0f, 0, 0);
    }
    put_f(SPRITE_TAB + 0, 40.0f); put_f(SPRITE_TAB + 4, 160.0f - 4); put_f(SPRITE_TAB + 8, -50.0f); put_f(SPRITE_TAB + 12, 205.0f - 4);
    // programas das VPUs
    std::vector<u32> pg, pp; std::string err;
    if (!vpu_assemble(kern::geometry_src(), pg, err)) throw std::runtime_error("geometria: " + err);
    if (!vpu_assemble(kern::particles_src(), pp, err)) throw std::runtime_error("partículas: " + err);
    soc.vpuA.load_program(pg); soc.vpuB.load_program(pp);
  }
  int cube_levels = 1;

  void cmd(std::vector<u32>& w, u32 op, std::vector<u32> p) { w.push_back((op << 24) | (u32)p.size()); for (u32 x : p) w.push_back(x); }
  void build_gpu_list() {
    std::vector<u32> w;
    cmd(w, GC_SET_RT, {RT, RT_PITCH, (u32)W | ((u32)H << 16)});
    cmd(w, GC_CLEAR, {0x00000000, f2u(1.0f), CLR_COLOR | CLR_DEPTH});
    cmd(w, GC_VERTEX_BASE, {GEO_OUT});
    cmd(w, GC_SET_TEX, {0, TEX_CUBE, 6u | (6u << 8) | ((u32)TF_DXT1 << 16) | ((u32)TFLAG_MIPS << 24), (u32)cube_levels});
    cmd(w, GC_TEXENV, {0, TE_MODULATE});
    cmd(w, GC_STATE, {ST_DEPTH_TEST | ST_DEPTH_WRITE | ST_TEX0 | ST_BILINEAR});
    cmd(w, GC_DRAW_INDEXED, {IDX_CUBE, (u32)cube_idx.size(), PRIM_TRIS});
    cmd(w, GC_SET_TEX, {0, TEX_SPHERE, 7u | (6u << 8) | ((u32)TF_RGB565 << 16) | (TFLAG_CLAMP << 24), 1});
    cmd(w, GC_DRAW_INDEXED, {IDX_SPHERE, (u32)sph_idx.size(), PRIM_TRIS});
    cmd(w, GC_END, {});
    for (size_t i = 0; i < w.size(); i++) bus().raw_w32(GPU_LIST + i * 4, w[i]);
    gpu_words = (u32)w.size();
  }
  void build_copper() {
    u32 i = 0; auto put = [&](u32 a, u32 b) { bus().raw_w32(COPPER_LIST + i * 8, a); bus().raw_w32(COPPER_LIST + i * 8 + 4, b); i++; };
    for (int y = 0; y < 180; y += 12) {               // céu em degradê
      float t = y / 180.0f; u32 r = (u32)(20 + 90 * t), g = (u32)(30 + 120 * t), b = (u32)(80 + 150 * t);
      put(cop_wait(y), 0); put(cop_move(VIDEO_BASE + VID_BG), 0xFF000000 | (r << 16) | (g << 8) | b);
    }
    put(cop_wait(180), 0); put(cop_move(VIDEO_BASE + VID_BG), 0xFF2A5A2A);     // chão
    put(cop_wait(215), 0); put(cop_move(VIDEO_BASE + VID_BG), 0xFF1E441E);
    put(0, 0);
  }
  void build_audio() {
    // "pluck": 64 amostras PCM16 com 3 harmônicos; notas por passo (Q16.16) relativo a 750 Hz
    std::vector<int16_t> wave(64); for (int i = 0; i < 64; i++) wave[i] = (int16_t)std::lround(7000 * std::sin(2 * M_PI * i / 64) + 2500 * std::sin(4 * M_PI * i / 64) + 1200 * std::sin(6 * M_PI * i / 64));
    for (int i = 0; i < 64; i++) wbe16(&bus().aram[0x1000 + i * 2], (u16)wave[i]);
    const double freq[8] = {261.63, 311.13, 392.00, 523.25, 392.00, 311.13, 261.63, 196.00};
    std::vector<std::vector<u32>> ev; u32 t = 0;
    for (int n = 0; n < 16; n++) {
      int v = n & 3; double f = freq[n % 8]; u32 step = (u32)std::lround(f / 750.0 * 65536.0); float x = (n % 2) ? 3.0f : -3.0f;
      u32 base = PAULA_BASE + PA_VOICE0 + 0;  (void)base;
      ev.push_back({t, (u32)v, PV_CTRL, 0}); ev.push_back({t, (u32)v, PV_ADDR, 0x1000}); ev.push_back({t, (u32)v, PV_LEN, 64}); ev.push_back({t, (u32)v, PV_LOOP_START, 0});
      ev.push_back({t, (u32)v, PV_STEP, step}); ev.push_back({t, (u32)v, PV_X, f2u(x)}); ev.push_back({t, (u32)v, PV_Z, f2u(-2.0f)}); ev.push_back({t, (u32)v, PV_REV_SEND, 160});
      ev.push_back({t, (u32)v, PV_VOL_L, 256}); ev.push_back({t, (u32)v, PV_VOL_R, 256}); ev.push_back({t, (u32)v, PV_CTRL, 1 | 2 | (1 << 2) | 0x10 | 0x20});
      ev.push_back({t, (u32)v, PV_LP_COEF, 90});
      ev.push_back({t + 9000, (u32)v, PV_CTRL, 0});
      t += 12000;                                       // 4 notas por segundo... 0,25 s por nota
    }
    u32 a = 0x00170000; for (auto& e : ev) { for (int k = 0; k < 4; k++) bus().raw_w32(a + k * 4, e[k]); a += 16; }
    seq_events = (u32)ev.size();
    soc.paula.master = 220; soc.paula.rev_wet = 90;
  }
  u32 seq_events = 0;

  void init_video() {
    bus().raw_w32(0, 0);
    auto& v = soc.video; v.write(VID_MODE, VMODE_240P); v.write(VID_P0_ADDR, RT); v.write(VID_P0_PITCH, RT_PITCH);
    v.write(VID_P1_ADDR, OVERLAY); v.write(VID_P1_PITCH, RT_PITCH); v.write(VID_CTRL, 3 | 4);
    soc.cop.write(COP_LIST, COPPER_LIST); soc.cop.write(COP_CTRL, 1);
  }

  void update_constants(int f) {
    using namespace kern;
    double t = f / 60.0;
    auto obj = [&](u32 at, Mat4 model) {
      Mat4 view = translate(0, -0.2, -5.5), proj = perspective(60.0 * M_PI / 180.0, (double)W / H, 0.1, 100.0), mvp = mul(proj, mul(view, model));
      for (int c = 0; c < 4; c++) put_v(at + c * 16, (float)mvp.m[0][c], (float)mvp.m[1][c], (float)mvp.m[2][c], (float)mvp.m[3][c]);
      for (int c = 0; c < 3; c++) put_v(at + (4 + c) * 16, (float)model.m[0][c], (float)model.m[1][c], (float)model.m[2][c], 0);
      double L[3] = {0.4, 0.7, 0.6}, ln = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
      put_v(at + 7 * 16, (float)(L[0] / ln), (float)(L[1] / ln), (float)(L[2] / ln), 0); put_v(at + 8 * 16, W / 2.0f, -H / 2.0f, 0.5f, 0); put_v(at + 9 * 16, W / 2.0f, H / 2.0f, 0.5f, 0);
      put_v(at + 10 * 16, 0, 0, 0, 1); put_v(at + 11 * 16, 0.75f, 0.7f, 0.65f, 0); put_v(at + 12 * 16, 0, 0, 0, 0); put_v(at + 13 * 16, 0.25f, 0.25f, 0.3f, 1); put_v(at + 14 * 16, 1, 1, 1, 1);
    };
    obj(CONST_DDR, mul(translate(-1.5, 1.2 + 0.25 * std::sin(t * 2.0), 0), mul(rotate_y(t * 1.1), rotate_x(t * 0.7))));
    obj(CONST_DDR + 0x100, mul(translate(1.5, 1.2 - 0.25 * std::sin(t * 2.0), 0), rotate_y(-t * 0.9)));
  }

  void build_program() {
    A a(CODE); int uid = 0;
    auto wr = [&](u32 addr, u32 val) { a.movel(A::Imm(val), A::Abs(addr)); };
    auto dma = [&](int ch, u32 src, u32 dst, u32 len) { u32 b = BRIDGE_BASE + BR_DMA0 + 0x20 * ch; wr(b + DMA_SRC, src); wr(b + DMA_DST, dst); wr(b + DMA_LEN, len); wr(b + DMA_FLAGS, 0); wr(b + DMA_CTRL, DMAC_START_REGS); };
    auto wait_bit = [&](u32 reg, int bit) { std::string l = "w" + std::to_string(uid++); a.label(l); a.movel(A::Abs(reg), A::D(0)); a.btst_i(bit, A::D(0)); a.beq(l); };
    auto dma_wait = [&](int ch) { wait_bit(BRIDGE_BASE + BR_DMA0 + 0x20 * ch + DMA_STATUS, 1); };
    auto vjob = [&](u32 base, u32 s0, u32 s1, u32 s2, u32 s3) { wr(base + VPU_PC, 0); wr(base + VPU_S0, s0); wr(base + VPU_S0 + 4, s1); wr(base + VPU_S0 + 8, s2); wr(base + VPU_S0 + 12, s3); wr(base + VPU_CTRL, VPUC_START); };
    // ----- boot: carga única de vértices e partículas nas memórias locais das VPUs -----
    boot_entry = a.here();
    dma(0, VERT_DDR, VPUA_DMEM + DM_CUBE_IN, VERT_BYTES); dma(1, PCONST_DDR, VPUB_DMEM + 0, 0x100 + NPART * 32);
    dma_wait(0); dma_wait(1); a.halt();
    // ----- quadro -----
    frame_entry = a.here();
    dma(0, CONST_DDR, VPUA_DMEM + 0, 0x1F0); dma_wait(0);
    vjob(VPUA_BASE, DM_CUBE_IN, DM_OUT, DM_CUBE_IN + CUBE_V * 48, 0);
    vjob(VPUA_BASE, DM_SPH_IN, DM_SPH_OUT, DM_SPH_IN + SPH_V * 48, 0x100);
    vjob(VPUB_BASE, kern::PART_IN, 0, kern::PART_IN + NPART * 32, 0);
    wait_bit(VPUA_BASE + VPU_STATUS, 1); wait_bit(VPUB_BASE + VPU_STATUS, 1);
    dma(0, VPUA_DMEM + DM_OUT, GEO_OUT, VERT_BYTES); dma(1, VPUB_DMEM + kern::PART_IN, PBUF_OUT, NPART * 32);
    dma_wait(0); dma_wait(1);
    wr(GPU_BASE + GPU_CMD_ADDR, GPU_LIST); wr(GPU_BASE + GPU_CMD_LEN, gpu_words); wr(GPU_BASE + GPU_CTRL, 1);
    // coerência explícita: a CPU vai ler o que o DMA escreveu
    wr(BRIDGE_BASE + BR_CACHE_ADDR, PBUF_OUT); wr(BRIDGE_BASE + BR_CACHE_LEN, NPART * 32); wr(BRIDGE_BASE + BR_CACHE_OP, CACHE_INVAL_RANGE);
    auto blit_idle = [&]() { std::string l = "b" + std::to_string(uid++); a.label(l); a.movel(A::Abs(BLT_BASE + BLT_STATUS), A::D(2)); a.btst_i(0, A::D(2)); a.bne(l); };
    // limpa o plano de sobreposição
    wr(BLT_BASE + BLT_OP, BOP_FILL); wr(BLT_BASE + BLT_DST, OVERLAY); wr(BLT_BASE + BLT_DST_PITCH, RT_PITCH); wr(BLT_BASE + BLT_W, W); wr(BLT_BASE + BLT_H, H); wr(BLT_BASE + BLT_COLOR, 0); wr(BLT_BASE + BLT_CTRL, 1);
    // a CPU monta a lista de sprites (um por partícula) e o Blitter desenha tudo numa operação só (BOP_SPRITES, com recorte)
    a.lea(A::Abs(PBUF_OUT), 0); a.lea(A::Abs(SPRITE_TAB), 1); a.lea(A::Abs(SPRITE_LIST), 2); a.moveq(NPART, 7);
    a.label("part");
    a.fop_ea(0x00, 1, A::Ind(0), 0); a.fop_ea(0x23, 1, A::Ind(1), 0); a.fop_ea(0x22, 1, A::Disp(1, 4), 0); a.fstore(0, 0, A::D(0));        // d0 = x*40 + 156
    a.fop_ea(0x00, 1, A::Disp(0, 4), 1); a.fop_ea(0x23, 1, A::Disp(1, 8), 1); a.fop_ea(0x22, 1, A::Disp(1, 12), 1); a.fstore(0, 1, A::D(1)); // d1 = y*-50 + 201
    a.movel(A::Imm(SPRITE), A::Ind(2)); a.move(2, A::Imm(32), A::Disp(2, 4)); a.move(2, A::Imm(0), A::Disp(2, 6));
    a.move(2, A::D(0), A::Disp(2, 8)); a.move(2, A::D(1), A::Disp(2, 10)); a.move(2, A::Imm(8), A::Disp(2, 12)); a.move(2, A::Imm(8), A::Disp(2, 14));
    a.adda(4, A::Imm(16), 0); a.adda(4, A::Imm(16), 2); a.subq(4, 1, A::D(7)); a.bne("part");
    // a lista foi escrita pela CPU (cache não coerente): flush antes do Blitter ler
    wr(BRIDGE_BASE + BR_CACHE_ADDR, SPRITE_LIST); wr(BRIDGE_BASE + BR_CACHE_LEN, NPART * SPRITE_ENTRY_SIZE); wr(BRIDGE_BASE + BR_CACHE_OP, CACHE_FLUSH_RANGE);
    blit_idle();
    wr(BLT_BASE + BLT_OP, BOP_SPRITES); wr(BLT_BASE + BLT_SRC, SPRITE_LIST); wr(BLT_BASE + BLT_COLOR, NPART);
    wr(BLT_BASE + BLT_DST, OVERLAY); wr(BLT_BASE + BLT_DST_PITCH, RT_PITCH); wr(BLT_BASE + BLT_W, W); wr(BLT_BASE + BLT_H, H); wr(BLT_BASE + BLT_CTRL, 1);
    wait_bit(GPU_BASE + GPU_STATUS, 1);
    a.halt();
    auto code = a.finish(); soc.load(CODE, code);
    // quadro 0 inicia os eventos de áudio
    soc.paula.write(PA_SEQ_ADDR, 0x00170000); soc.paula.write(PA_SEQ_LEN, seq_events); soc.paula.write(PA_SEQ_CTRL, 1 | 2);
  }
};

}}  // namespace sf::demo
