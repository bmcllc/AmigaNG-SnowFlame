#include "helpers.h"
#include "kernels.h"

static void vset(Vpu& v, int slot, float x, float y, float z, float w) { wbef(v.dmem + slot * 16, x); wbef(v.dmem + slot * 16 + 4, y); wbef(v.dmem + slot * 16 + 8, z); wbef(v.dmem + slot * 16 + 12, w); }
static float vget(Vpu& v, int slot, int lane) { return rbef(v.dmem + slot * 16 + lane * 4); }
static Vpu::Result vrun(Soc& s, const char* src, const u32 sregs[8] = nullptr) {
  std::vector<u32> p; std::string err; bool ok = vpu_assemble(src, p, err); if (!ok) { report_fail(__FILE__, __LINE__, "asm: " + err); return {}; }
  s.vpuA.load_program(p); u32 z[8] = {0}; return s.vpuA.run_now(0, sregs ? sregs : z);
}

TEST(vpu_encoding_big_endian_and_roundtrip) {
  Soc s; std::vector<u32> p; std::string err;
  CHECK(vpu_assemble("VADD v1, v2, v3\nVMUL v4, v5, v6.wzyx\nHALT", p, err)); CHECK_EQ(p.size(), 3);
  CHECK_EQ(p[0], vpu_enc_r(vop::VADD, 1, 2, 3)); CHECK_EQ(p[1], vpu_enc_r(vop::VMUL, 4, 5, 6, 0x1B)); CHECK_EQ(p[2], 0);
  s.vpuA.load_program(p); CHECK_EQ(s.vpuA.imem[0], p[0] >> 24); CHECK_EQ(s.vpuA.imem[3], p[0] & 0xFF);       // palavra big-endian na IMEM
  CHECK(vpu_disasm(p[0]) == "VADD v1, v2, v3"); CHECK(vpu_disasm(p[1]) == "VMUL v4, v5, v6.wzyx");
  CHECK(vpu_disasm(vpu_enc_i(vop::VLD, 3, 2, -4)) == "VLD v3, [s2-4]");
  CHECK(vpu_disasm(vpu_enc_i(vop::SETA, 1, 0, 100)) == "SETA s1, 100");
}

TEST(vpu_assembler_reports_errors) {
  std::vector<u32> p; std::string err;
  CHECK(!vpu_assemble("VADD v1, v2\n", p, err)); CHECK(err.find("linha 1") != std::string::npos);
  CHECK(!vpu_assemble("NOP\nFOO v1,v2,v3\n", p, err)); CHECK(err.find("linha 2") != std::string::npos);
  CHECK(!vpu_assemble("VADD v32, v1, v2\n", p, err));
  CHECK(!vpu_assemble("VADD v1, v2, v3.xyz\n", p, err));
  CHECK(!vpu_assemble("BRA nowhere\n", p, err)); CHECK(err.find("indefinido") != std::string::npos);
  CHECK(!vpu_assemble("a:\na:\nHALT\n", p, err));
  CHECK(!vpu_assemble("SETA s1, 70000\n", p, err));
  CHECK(!vpu_assemble("VLD v0, [v1+0]\n", p, err));
  CHECK(vpu_assemble("  ; comentário\n// outro\nstart: NOP  ; fim\n HALT", p, err)); CHECK_EQ(p.size(), 2);
}

TEST(vpu_vector_arithmetic) {
  Soc s; Vpu& v = s.vpuA;
  vset(v, 0, 1, -2, 3, 4); vset(v, 1, 0.5f, 2, -1, 8); vset(v, 2, 10, 10, 10, 10);
  auto r = vrun(s, "SETA s1,0\nVLD v0,[s1+0]\nVLD v1,[s1+1]\nVLD v2,[s1+2]\n"
                   "VADD v3,v0,v1\nVST [s1+8],v3\n"
                   "VSUB v3,v0,v1\nVST [s1+9],v3\n"
                   "VMUL v3,v0,v1\nVST [s1+10],v3\n"
                   "VMOV v4,v2\nVMAC v4,v0,v1\nVST [s1+11],v4\n"
                   "VMIN v3,v0,v1\nVST [s1+12],v3\n"
                   "VMAX v3,v0,v1\nVST [s1+13],v3\n"
                   "VABS v3,v0\nVST [s1+14],v3\nHALT");
  CHECK(!r.fault);
  float e_add[4] = {1.5f, 0, 2, 12}, e_sub[4] = {0.5f, -4, 4, -4}, e_mul[4] = {0.5f, -4, -3, 32}, e_mac[4] = {10.5f, 6, 7, 42};
  float e_min[4] = {0.5f, -2, -1, 4}, e_max[4] = {1, 2, 3, 8}, e_abs[4] = {1, 2, 3, 4};
  for (int l = 0; l < 4; l++) {
    CHECK_EQ(f2u(vget(v, 8, l)), f2u(e_add[l])); CHECK_EQ(f2u(vget(v, 9, l)), f2u(e_sub[l])); CHECK_EQ(f2u(vget(v, 10, l)), f2u(e_mul[l]));
    CHECK_EQ(f2u(vget(v, 11, l)), f2u(e_mac[l])); CHECK_EQ(f2u(vget(v, 12, l)), f2u(e_min[l])); CHECK_EQ(f2u(vget(v, 13, l)), f2u(e_max[l])); CHECK_EQ(f2u(vget(v, 14, l)), f2u(e_abs[l]));
  }
}

TEST(vpu_swizzle_dot_reciprocal) {
  Soc s; Vpu& v = s.vpuA;
  vset(v, 0, 1, 2, 3, 4); vset(v, 1, 5, 6, 7, 8); vset(v, 2, 4, 16, 0.25f, 2);
  auto r = vrun(s, "SETA s1,0\nVLD v0,[s1+0]\nVLD v1,[s1+1]\nVLD v2,[s1+2]\n"
                   "VMUL v3,v0,v1.wzyx\nVST [s1+8],v3\n"          // (1*8, 2*7, 3*6, 4*5)
                   "VADD v3,v0,v1.xxxx\nVST [s1+9],v3\n"
                   "VDOT3 v3,v0,v1\nVST [s1+10],v3\n"             // 5+12+21 = 38 splat
                   "VDOT4 v3,v0,v1\nVST [s1+11],v3\n"             // 38+32 = 70
                   "VRCP v3,v2\nVST [s1+12],v3\n"
                   "VRSQ v3,v2.yyyy\nVST [s1+13],v3\n"
                   "VMOV v3,v0.zzwx\nVST [s1+14],v3\nHALT");
  CHECK(!r.fault);
  float e0[4] = {8, 14, 18, 20}, e1[4] = {6, 7, 8, 9}, e4[4] = {0.25f, 1.0f / 16, 4, 0.5f}, e6[4] = {3, 3, 4, 1};
  for (int l = 0; l < 4; l++) {
    CHECK_EQ(f2u(vget(v, 8, l)), f2u(e0[l])); CHECK_EQ(f2u(vget(v, 9, l)), f2u(e1[l])); CHECK_EQ(vget(v, 10, l), 38.0f); CHECK_EQ(vget(v, 11, l), 70.0f);
    CHECK_EQ(f2u(vget(v, 12, l)), f2u(e4[l])); CHECK_EQ(vget(v, 13, l), 0.25f); CHECK_EQ(vget(v, 14, l), e6[l]);
  }
}

TEST(vpu_scalar_loop_cycle_count) {
  Soc s; Vpu& v = s.vpuA; vset(v, 1, 1, 2, 3, 4);
  auto r = vrun(s, "SETA s1,0\nSETA s2,10\nVLD v1,[s1+1]\nVSUB v0,v0,v0\nloop:\nVADD v0,v0,v1\nSADD s1,s1,1\nSCMP s1,s2\nBRLT loop\nVST [s3+4],v0\nHALT");
  CHECK(!r.fault); CHECK_EQ(vget(v, 4, 0), 10.0f); CHECK_EQ(vget(v, 4, 3), 40.0f);
  CHECK_EQ(r.cycles, 4 + 40 + 2);       // 4 iniciais, 10 x 4 do laço, VST + HALT
}

TEST(vpu_branches_ne_bra) {
  Soc s; Vpu& v = s.vpuA; vset(v, 0, 1, 1, 1, 1);
  vrun(s, "SETA s1,0\nSETA s2,3\nVLD v0,[s1+0]\nVSUB v1,v1,v1\nl:\nVADD v1,v1,v0\nSADD s1,s1,1\nSCMP s1,s2\nBRNE l\nBRA end\nVADD v1,v1,v0\nend:\nVST [s3+4],v1\nHALT");
  CHECK_EQ(vget(v, 4, 0), 3.0f);        // BRA pulou o VADD extra
}

TEST(vpu_faults) {
  Soc s;
  u32 sr[8] = {0}; sr[1] = 8;
  auto r = vrun(s, "VLD v0,[s1+0]\nHALT", sr); CHECK(r.fault); CHECK_EQ(r.pc, 0);                // desalinhado
  sr[1] = 0xFFF0 + 0x10000; r = vrun(s, "VLD v0,[s1+0]\nHALT", sr); CHECK(r.fault);               // fora da DMEM
  sr[1] = 0xFFF0; r = vrun(s, "VST [s1+0],v0\nHALT", sr); CHECK(!r.fault);                        // última posição válida
  sr[1] = 0xFFF0; r = vrun(s, "VST [s1+1],v0\nHALT", sr); CHECK(r.fault);
  wbe32(s.vpuA.imem, 63u << 26); r = s.vpuA.run_now(0, sr); CHECK(r.fault);                       // opcode inválido
  r = vrun(s, "l:\nBRA l\n"); CHECK(r.fault); CHECK(r.cycles >= 4000000);                         // watchdog
  r = vrun(s, "NOP\nNOP\nHALT\n"); CHECK(!r.fault);                                                // memória zerada = HALT
  for (u32 i = 0; i < VPU_IMEM_SIZE; i += 4) wbe32(s.vpuA.imem + i, (u32)vop::NOP << 26);
  r = s.vpuA.run_now(0, sr); CHECK(r.fault); CHECK_EQ(r.pc, VPU_IMEM_SIZE / 4);                  // executar além da IMEM
}

TEST(vpu_peak_throughput_matches_spec) {
  Soc s; std::string src; for (int i = 0; i < 100; i++) src += "VMAC v0,v1,v2\n"; src += "HALT\n";
  auto r = vrun(s, src.c_str());
  CHECK_EQ(r.flops, 800); CHECK_EQ(r.cycles, 101);
  double gflops_per_unit = (double)r.flops / (r.cycles - 1) * 150e6 / 1e9;       // 8 FLOP/ciclo x 150 MHz
  CHECK_NEAR(gflops_per_unit, 1.2, 1e-9); CHECK_NEAR(2 * gflops_per_unit, 2.4, 1e-9);
}

TEST(vpu_job_queue_timing_and_irq) {
  Soc s; Vpu& v = s.vpuA;
  std::vector<u32> p; std::string err;
  CHECK(vpu_assemble("SADD s1,s0,0\nVLD v0,[s1+0]\nVADD v0,v0,v0\nVST [s1+0],v0\nHALT\n", p, err)); v.load_program(p);
  for (int i = 0; i < 3; i++) vset(v, 8 + i, 1.0f + i, 0, 0, 0);
  s.bus.irq_mask = IRQ_VPUA;
  for (int i = 0; i < 3; i++) { mmio_w(s, VPUA_BASE + VPU_S0, (8 + i) * 16); mmio_w(s, VPUA_BASE + VPU_PC, 0); mmio_w(s, VPUA_BASE + VPU_CTRL, VPUC_START | (i == 2 ? (u32)VPUC_IRQ : 0u)); }
  CHECK_EQ(mmio_r(s, VPUA_BASE + VPU_STATUS) & VPUS_BUSY, VPUS_BUSY); CHECK_EQ((mmio_r(s, VPUA_BASE + VPU_STATUS) >> 8) & 0xFF, 2);   // 1 executando + 2 na fila
  CHECK(!s.bus.irq_pending());
  Ps t0 = s.sched.now; CHECK(s.wait_until([&] { return s.bus.irq_pending(); }));
  CHECK_EQ(s.sched.now - t0, cyc2ps(5, VPU_HZ) * 3);                      // 3 jobs de 5 ciclos em série
  CHECK_EQ(mmio_r(s, VPUA_BASE + VPU_STATUS) & (VPUS_BUSY | VPUS_DONE), VPUS_DONE);
  for (int i = 0; i < 3; i++) CHECK_EQ(vget(v, 8 + i, 0), 2.0f * (1 + i));
  // estouro da fila
  for (int i = 0; i < 9; i++) mmio_w(s, VPUA_BASE + VPU_CTRL, VPUC_START);
  CHECK_EQ(mmio_r(s, VPUA_BASE + VPU_STATUS) & VPUS_FAULT, VPUS_FAULT);
  mmio_w(s, VPUA_BASE + VPU_CTRL, VPUC_RESET); CHECK_EQ(mmio_r(s, VPUA_BASE + VPU_STATUS), 0);
}

TEST(vpu_two_units_run_in_parallel) {
  Soc s; std::vector<u32> p; std::string err; std::string src; for (int i = 0; i < 99; i++) src += "VMAC v0,v1,v2\n"; src += "HALT\n";
  CHECK(vpu_assemble(src, p, err)); s.vpuA.load_program(p); s.vpuB.load_program(p);
  mmio_w(s, VPUA_BASE + VPU_CTRL, VPUC_START); mmio_w(s, VPUB_BASE + VPU_CTRL, VPUC_START);
  Ps t0 = s.sched.now;
  CHECK(s.wait_until([&] { return !s.vpuA.busy() && !s.vpuB.busy(); }));
  CHECK_EQ(s.sched.now - t0, cyc2ps(100, VPU_HZ));                         // não 200 ciclos
  CHECK_EQ(s.vpuA.total_flops + s.vpuB.total_flops, 2 * 99 * 8);
}

TEST(vpu_geometry_kernel_matches_reference) {
  Soc s; Vpu& v = s.vpuA; using namespace kern;
  const double W = 320, H = 240;
  Mat4 model = mul(translate(0.3, -0.2, 0), mul(rotate_y(0.7), rotate_x(0.3)));
  Mat4 view = translate(0, 0, -5); Mat4 proj = perspective(60.0 * M_PI / 180.0, W / H, 0.1, 100.0);
  Mat4 mvp = mul(proj, mul(view, model));
  for (int c = 0; c < 4; c++) vset(v, c, (float)mvp.m[0][c], (float)mvp.m[1][c], (float)mvp.m[2][c], (float)mvp.m[3][c]);
  for (int c = 0; c < 3; c++) vset(v, 4 + c, (float)model.m[0][c], (float)model.m[1][c], (float)model.m[2][c], 0);
  double L[3] = {0.3, 0.8, 0.52}; double ln = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]); for (auto& x : L) x /= ln;
  vset(v, 7, (float)L[0], (float)L[1], (float)L[2], 0); vset(v, 8, W / 2, -H / 2, 0.5f, 0); vset(v, 9, W / 2, H / 2, 0.5f, 0);
  vset(v, 10, 0, 0, 0, 1); vset(v, 11, 0.7f, 0.6f, 0.5f, 0); vset(v, 12, 0, 0, 0, 0); vset(v, 13, 0.2f, 0.2f, 0.25f, 1); vset(v, 14, 1, 1, 1, 1);
  const int N = 6; float pos[N][4] = {{1, 1, 1, 1}, {-1, 1, 0.5f, 1}, {0, -1, 0, 1}, {0.25f, 0.5f, -1, 1}, {2, 0, 0, 1}, {0, 0, 0, 1}};
  float nor[N][3] = {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}, {0, 0.6f, 0.8f}, {-1, 0, 0}, {0, -1, 0}};
  u32 in = GEO_IN, out = GEO_IN + N * 48;
  for (int i = 0; i < N; i++) { vset(v, in / 16 + i * 3, pos[i][0], pos[i][1], pos[i][2], pos[i][3]); vset(v, in / 16 + i * 3 + 1, nor[i][0], nor[i][1], nor[i][2], 0); vset(v, in / 16 + i * 3 + 2, 0.1f * i, 0.2f * i, 0, 0); }
  std::vector<u32> prog; std::string err; CHECK(vpu_assemble(geometry_src(), prog, err)); v.load_program(prog);
  u32 sr[8] = {in, out, in + N * 48, 0, 0, 0, 0, 0}; auto r = v.run_now(0, sr); CHECK(!r.fault);
  for (int i = 0; i < N; i++) {
    double p[4] = {pos[i][0], pos[i][1], pos[i][2], pos[i][3]}, clip[4] = {0, 0, 0, 0};
    for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) clip[a] += mvp.m[a][b] * p[b];
    double iw = 1.0 / clip[3]; double sx = clip[0] * iw * (W / 2) + W / 2, sy = clip[1] * iw * (-H / 2) + H / 2, sz = clip[2] * iw * 0.5 + 0.5;
    double n[3] = {0, 0, 0}; for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) n[a] += model.m[a][b] * nor[i][b];
    double nl = std::max(0.0, n[0] * L[0] + n[1] * L[1] + n[2] * L[2]);
    double cr = std::min(1.0, 0.7 * nl + 0.2), cg = std::min(1.0, 0.6 * nl + 0.2), cb = std::min(1.0, 0.5 * nl + 0.25);
    int o = out / 16 + i * 3;
    CHECK_NEAR(vget(v, o, 0), sx, 2e-3); CHECK_NEAR(vget(v, o, 1), sy, 2e-3); CHECK_NEAR(vget(v, o, 2), sz, 1e-5); CHECK_NEAR(vget(v, o, 3), iw, 1e-6);
    CHECK_NEAR(vget(v, o + 1, 0), cr, 1e-5); CHECK_NEAR(vget(v, o + 1, 1), cg, 1e-5); CHECK_NEAR(vget(v, o + 1, 2), cb, 1e-5); CHECK_NEAR(vget(v, o + 1, 3), 1.0, 0);
    CHECK_NEAR(vget(v, o + 2, 0), 0.1f * i, 1e-7);
  }
  CHECK_EQ(r.cycles, 15 + N * 27 + 1);        // 27 instruções por vértice
}

TEST(vpu_particle_kernel_bounces_and_stays_above_floor) {
  Soc s; Vpu& v = s.vpuB; using namespace kern; (void)v;
  Vpu& u = s.vpuB;
  vset(u, 0, 0.01f, 0.01f, 0.01f, 0.01f); vset(u, 1, 0, -9.8f, 0, 0); vset(u, 2, 0, 0, 0, 0); vset(u, 3, 0, 1, 0, 0); vset(u, 4, 0, -2, 0, 0);
  vset(u, 5, 1000, 1000, 1000, 1000); vset(u, 6, -1, -1, -1, -1); vset(u, 7, 0.6f, 0.6f, 0.6f, 0.6f);
  const int N = 4; for (int i = 0; i < N; i++) { vset(u, PART_IN / 16 + i * 2, -1.0f + i, 1.0f + i, 0, 1); vset(u, PART_IN / 16 + i * 2 + 1, 0.5f, 0, 0.1f * i, 0); }
  std::vector<u32> prog; std::string err; CHECK(vpu_assemble(particles_src(), prog, err)); u.load_program(prog);
  u32 sr[8] = {PART_IN, 0, PART_IN + N * 32, 0, 0, 0, 0, 0};
  float minY = 1e9f, firstBounceMax = 0; bool bounced = false; float prevVy = 0;
  for (int step = 0; step < 300; step++) {
    sr[0] = PART_IN; auto r = u.run_now(0, sr); CHECK(!r.fault);
    float y = vget(u, PART_IN / 16, 1), vy = vget(u, PART_IN / 16 + 1, 1); minY = std::min(minY, y);
    if (prevVy < 0 && vy > 0) bounced = true;
    if (bounced && vy > 0) firstBounceMax = std::max(firstBounceMax, y);
    prevVy = vy;
  }
  CHECK(minY >= 0.0f); CHECK(bounced);
  CHECK(firstBounceMax > 0.1f && firstBounceMax < 1.0f);              // sobe menos que a altura inicial (restituição 0,6)
  CHECK_NEAR(vget(u, PART_IN / 16, 0), -1.0f + 0.5f * 0.01f * 300, 1e-3);   // movimento horizontal uniforme
}
