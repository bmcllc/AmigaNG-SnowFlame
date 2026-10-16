#include "helpers.h"
#include <random>

// ======================= Cobertura da ISA ColdFire (SnowFire-CF266) =======================
TEST(isa_addx_subx_negx_extended_precision) {
  Soc s; A a(CODE_BASE);
  // soma de 64 bits: (0x00000001_FFFFFFFF) + (0x00000000_00000001) = 0x00000002_00000000
  a.movel(A::Imm(0xFFFFFFFF), A::D(0)); a.moveq(1, 1); a.moveq(1, 2); a.moveq(0, 3);
  a.add(4, A::D(2), 0); a.addx_l(3, 1);            // d0 += d2; d1 += d3 + X
  // subtração de 64 bits: 0x00000002_00000000 - 1 = 0x00000001_FFFFFFFF
  a.moveq(1, 4); a.moveq(0, 5);
  a.movel(A::D(0), A::D(6)); a.movel(A::D(1), A::D(7));
  a.sub(4, A::D(4), 6); a.subx_l(5, 7);
  // NEGX: -(0) - X com X=1 -> 0xFFFFFFFF
  a.movel(A::Imm(0xFFFFFFFF), A::D(2)); a.addq(4, 1, A::D(2)); a.moveq(0, 3); a.negx_l(3);
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_EQ(s.cpu.d[0], 0); CHECK_EQ(s.cpu.d[1], 2); CHECK_EQ(s.cpu.d[6], 0xFFFFFFFF); CHECK_EQ(s.cpu.d[7], 1); CHECK_EQ(s.cpu.d[3], 0xFFFFFFFF);
}

TEST(isa_addx_zero_flag_is_sticky_clear_only) {
  Soc s; A a(CODE_BASE);
  a.moveq(0, 0); a.moveq(0, 1); a.moveq(0, 2);
  a.moveq(1, 3); a.moveq(-1, 4); a.add(4, A::D(3), 4);   // d4 = 0, carry (X=1), Z=1
  a.moveq(0, 5); a.moveq(0, 6); a.addx_l(5, 6);           // 0+0+1 = 1 -> Z limpo
  a.move_from_ccr(0);
  a.moveq(0, 5); a.moveq(0, 7); a.moveq(-1, 1); a.addq(4, 1, A::D(1)); // d1=0, X=1, Z=1
  a.moveq(0, 7); a.addx_l(5, 7);                          // 1 -> Z limpo
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_EQ(s.cpu.d[6], 1); CHECK_EQ(s.cpu.d[0] & 4, 0); CHECK_EQ(s.cpu.d[7], 1);
}

TEST(isa_mov3q_mvs_mvz) {
  Soc s; A a(CODE_BASE);
  a.mov3q(5, A::D(0)); a.mov3q(-1, A::D(1)); a.mov3q(3, A::Abs(0x3000));
  s.bus.raw_w32(0x3100, 0x8081FF80);
  a.mvs(1, A::Abs(0x3103), 2);    // 0x80 -> 0xFFFFFF80
  a.mvz(1, A::Abs(0x3103), 3);    // 0x80 -> 0x00000080
  a.mvs(2, A::Abs(0x3102), 4);    // 0xFF80 -> 0xFFFFFF80
  a.mvz(2, A::Abs(0x3102), 5);    // 0x0000FF80
  a.mvs(2, A::Abs(0x3100), 6);    // 0x8081 -> 0xFFFF8081
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_EQ(s.cpu.d[0], 5); CHECK_EQ(s.cpu.d[1], 0xFFFFFFFF); CHECK_EQ(s.bus.raw_r32(0x3000), 3);
  CHECK_EQ(s.cpu.d[2], 0xFFFFFF80); CHECK_EQ(s.cpu.d[3], 0x80); CHECK_EQ(s.cpu.d[4], 0xFFFFFF80); CHECK_EQ(s.cpu.d[5], 0xFF80); CHECK_EQ(s.cpu.d[6], 0xFFFF8081);
}

TEST(isa_bitrev_byterev_ff1_sats_tas) {
  Soc s; A a(CODE_BASE);
  a.movel(A::Imm(0x00000001), A::D(0)); a.bitrev(0);                       // 0x80000000
  a.movel(A::Imm(0x11223344), A::D(1)); a.byterev(1);                      // 0x44332211
  a.movel(A::Imm(0x00010000), A::D(2)); a.ff1(2);                          // bit 16 -> 15
  a.moveq(0, 3); a.ff1(3); a.move_from_ccr(4);                             // 32, Z
  a.movel(A::Imm(0x80000000), A::D(5)); a.ff1(5); a.move_from_ccr(6);      // 0, N
  a.movel(A::Imm(0x7FFFFFFF), A::D(7)); a.addq(4, 1, A::D(7)); a.sats(7);  // overflow -> 0x7FFFFFFF
  s.bus.raw_w32(0x3000, 0x00000000);
  a.tas(A::Abs(0x3000)); a.move_from_ccr(1);
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_EQ(s.cpu.d[0], 0x80000000); CHECK_EQ(s.cpu.d[2], 15); CHECK_EQ(s.cpu.d[3], 32); CHECK_EQ(s.cpu.d[4] & 4, 4);
  CHECK_EQ(s.cpu.d[5], 0); CHECK_EQ(s.cpu.d[6] & 8, 8); CHECK_EQ(s.cpu.d[7], 0x7FFFFFFF);
  CHECK_EQ(s.bus.ddr[0x3000], 0x80); CHECK_EQ(s.cpu.d[1] & 4, 4);          // TAS vê o byte antigo = 0 (Z)
}

TEST(isa_bchg_bclr_bset_register_and_tpf) {
  Soc s; A a(CODE_BASE);
  a.moveq(0, 0); a.moveq(5, 1); a.bset_d(1, A::D(0));              // d0 = 0x20
  a.moveq(3, 2); a.bchg_d(2, A::D(0));                             // d0 = 0x28
  a.bclr_d(1, A::D(0));                                            // d0 = 0x08
  a.bchg_i(0, A::D(0));                                            // d0 = 0x09
  a.tpf(); a.moveq(7, 3);
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_EQ(s.cpu.d[0], 0x09); CHECK_EQ(s.cpu.d[3], 7);
}

TEST(isa_divs_w_int_min_does_not_trap_the_host) {
  Soc s; A a(CODE_BASE);
  a.movel(A::Imm(0x80000000), A::D(0)); a.moveq(-1, 1); a.divs_w(A::D(1), 0);   // overflow (V), não pode causar SIGFPE
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_EQ(s.cpu.d[0], 0x80000000);
}

TEST(isa_fpu_all_fbcc_conditions) {
  // valores de teste: -1, 0, +1 e NaN; para cada um, avalia as 16 condições e compara com a tabela IEEE.
  for (int val = 0; val < 4; val++) {
    for (int cc = 0; cc < 16; cc++) {
      Soc s; A a(CODE_BASE);
      double v[4] = {-1.0, 0.0, 1.0, std::nan("")};
      s.bus.raw_w32(0x3000, (u32)(d2u(v[val]) >> 32)); s.bus.raw_w32(0x3004, (u32)d2u(v[val]));
      a.fop_ea(0x00, 5, A::Abs(0x3000), 0); a.fop_rr(0x3A, 0, 0);
      a.moveq(1, 0); a.fbcc(cc, "t"); a.moveq(0, 0); a.label("t"); a.halt();
      CHECK(run_prog(s, a));
      bool N = val == 0, Z = val == 1, U = val == 3;
      bool exp[16] = {false, Z, !(U || Z || N), Z || !(U || N), N && !(U || Z), Z || (N && !U), !(U || Z), !U,
                      U, U || Z, U || !(N || Z), U || Z || !N, U || (N && !Z), U || Z || N, !Z, true};
      CHECK_EQ(s.cpu.d[0], exp[cc] ? 1 : 0);
    }
  }
}

TEST(isa_fpu_fint_rounding_modes_and_fpcr) {
  Soc s; A a(CODE_BASE);
  auto put_d = [&](u32 ad, double d) { s.bus.raw_w32(ad, (u32)(d2u(d) >> 32)); s.bus.raw_w32(ad + 4, (u32)d2u(d)); };
  put_d(0x3000, 2.5); put_d(0x3008, -2.5);
  a.fop_ea(0x01, 5, A::Abs(0x3000), 0); a.fstore(5, 0, A::Abs(0x3100));          // FINT: próx. par -> 2
  a.fop_ea(0x03, 5, A::Abs(0x3008), 1); a.fstore(5, 1, A::Abs(0x3108));          // FINTRZ(-2.5) = -2
  a.movel(A::Imm(0x20), A::D(0)); a.fmove_to_ctrl(4, A::D(0));                    // RND = -inf
  a.fop_ea(0x01, 5, A::Abs(0x3008), 2); a.fstore(5, 2, A::Abs(0x3110));          // floor(-2.5) = -3
  a.fop_ea(0x00, 5, A::Abs(0x3000), 3); a.fstore(0, 3, A::D(1));                  // FMOVE.L 2.5 com RND=-inf -> 2
  a.movel(A::Imm(0x30), A::D(0)); a.fmove_to_ctrl(4, A::D(0));                    // RND = +inf
  a.fop_ea(0x01, 5, A::Abs(0x3000), 4); a.fstore(5, 4, A::Abs(0x3118));          // ceil(2.5) = 3
  a.fmove_from_ctrl(4, A::D(2));
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_NEAR(u2d(rbe64(&s.bus.ddr[0x3100])), 2.0, 0); CHECK_NEAR(u2d(rbe64(&s.bus.ddr[0x3108])), -2.0, 0);
  CHECK_NEAR(u2d(rbe64(&s.bus.ddr[0x3110])), -3.0, 0); CHECK_NEAR(u2d(rbe64(&s.bus.ddr[0x3118])), 3.0, 0);
  CHECK_EQ(s.cpu.d[1], 2); CHECK_EQ(s.cpu.d[2], 0x30);
}

TEST(isa_fpu_single_double_rounding_variants) {
  Soc s; A a(CODE_BASE);
  double x = 1.0 + 1e-10;     // não representável em float
  s.bus.raw_w32(0x3000, (u32)(d2u(x) >> 32)); s.bus.raw_w32(0x3004, (u32)d2u(x));
  a.fop_ea(0x40, 5, A::Abs(0x3000), 0); a.fstore(5, 0, A::Abs(0x3100));   // FSMOVE -> 1.0
  a.fop_ea(0x44, 5, A::Abs(0x3000), 1); a.fstore(5, 1, A::Abs(0x3108));   // FDMOVE -> mantém
  a.fop_ea(0x00, 5, A::Abs(0x3000), 2); a.fstore(5, 2, A::Abs(0x3110));   // FMOVE (PREC extended/double) -> mantém
  a.movel(A::Imm(0x40), A::D(0)); a.fmove_to_ctrl(4, A::D(0));            // PREC = simples
  a.fop_ea(0x00, 5, A::Abs(0x3000), 3); a.fstore(5, 3, A::Abs(0x3118));   // FMOVE com PREC simples -> 1.0
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_NEAR(u2d(rbe64(&s.bus.ddr[0x3100])), 1.0, 0); CHECK_NEAR(u2d(rbe64(&s.bus.ddr[0x3108])), x, 0);
  CHECK_NEAR(u2d(rbe64(&s.bus.ddr[0x3110])), x, 0); CHECK_NEAR(u2d(rbe64(&s.bus.ddr[0x3118])), 1.0, 0);
}

TEST(isa_fpu_fmovem_and_fpsr) {
  Soc s; A a(CODE_BASE);
  for (int i = 0; i < 3; i++) { double d = 1.5 * (i + 1); s.bus.raw_w32(0x3000 + i * 8, (u32)(d2u(d) >> 32)); s.bus.raw_w32(0x3004 + i * 8, (u32)d2u(d)); }
  a.fmovem_load(A::Abs(0x3000), 0xE0);              // FP0..FP2 <- memória
  a.fmovem_store(0x07, A::Abs(0x3200));             // FP5..FP7 (zeros) -> memória
  a.fmovem_store(0xE0, A::Abs(0x3100));             // FP0..FP2 -> memória
  a.fop_rr(0x28, 0, 0); a.fmove_from_ctrl(2, A::D(0));    // FP0 - FP0 = 0 -> Z no FPSR
  a.fop_ea(0x00, 5, A::Abs(0x3000), 3); a.fop_rr(0x1A, 3, 3); a.fmove_from_ctrl(2, A::D(1));   // negativo -> N
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_NEAR(u2d(rbe64(&s.bus.ddr[0x3108])), 3.0, 0); CHECK_NEAR(u2d(rbe64(&s.bus.ddr[0x3110])), 4.5, 0);
  CHECK_NEAR(u2d(rbe64(&s.bus.ddr[0x3200])), 0.0, 0);
  CHECK_EQ(s.cpu.d[0] & (1u << 26), 1u << 26); CHECK_EQ(s.cpu.d[1] & (1u << 27), 1u << 27);
}

// Varredura: executa uma vez cada um dos 65536 opcodes com extensões variadas. Nenhum pode derrubar o hospedeiro
// (divisão por zero/overflow, acesso fora de faixa) nem deixar a CPU num estado inconsistente.
TEST(isa_rox_and_memory_shifts) {
  Soc s; A a(CODE_BASE);
  s.bus.raw_w32(0x3000, 0x80018001);
  a.moveq(-1, 1); a.addq(4, 1, A::D(1));          // d1 = 0, X = 0 ... (-1+1 gera carry => X=1)
  a.movel(A::Imm(0x80000001), A::D(2));
  a.w(0xE392);                                    // roxl.l #1,d2 com X=1: 0x80000001 -> 0x00000003
  a.w(0xE1F8); a.w(0x3000);                          // asl.w (0x3000).w : 0x8001 -> 0x0002, C=1
  a.w(0xE2F8); a.w(0x3002);                          // lsr.w (0x3002).w : 0x8001 -> 0x4000, C=1
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_EQ(s.bus.raw_r16(0x3000), 0x0002); CHECK_EQ(s.bus.raw_r16(0x3002), 0x4000);
  CHECK_EQ(s.cpu.d[2], 3u);                       // X entrou pelo bit 0; bit 31 saiu
}

TEST(isa_every_opcode_is_survivable) {
  Soc s; s.bus.cache_enabled = false;
  const u16 ext[4][3] = {{0x0000, 0x0000, 0x0000}, {0xFFFF, 0xFFFF, 0xFFFF}, {0x0F00, 0x2468, 0x1357}, {0x8000, 0x0001, 0xA5A5}};
  int illegal = 0, ok = 0;
  for (int e = 0; e < 4; e++) for (u32 op = 0; op < 0x10000; op++) {
    s.cpu.reset_regs();
    for (int i = 0; i < 8; i++) { s.cpu.d[i] = (e == 1) ? 0x80000000u + i : (e == 3) ? 0xFFFFFFFFu : 0x12345 * (i + 1); s.cpu.a[i] = 0x20000 + i * 0x100; }
    s.cpu.a[7] = 0x00F00000; s.cpu.d[2] = e == 1 ? 0xFFFFFFFFu : s.cpu.d[2];
    s.bus.raw_w32(0x1000, ((u32)op << 16) | ext[e][0]); s.bus.raw_w32(0x1004, ((u32)ext[e][1] << 16) | ext[e][2]);
    s.bus.raw_w32(0x1008, 0x4AC80000);   // HALT
    s.bus.icache_inval_all();                              // raw_w32 não invalida a I-cache
    s.cpu.set_pc(0x1000); s.bus.fault = false;
    int c = s.cpu.step(); (void)c;
    if (s.cpu.halted && s.cpu.reason == Cpu::NO_VECTOR) illegal++; else ok++;
  }
  CHECK(ok > 20000);       // muitos opcodes válidos
  CHECK(illegal > 1000);   // e muitos ilegais, tratados via exceção
}

// ======================= Item 2: formato completo, usuário/supervisor, FScc/FSAVE, eMAC =======================
TEST(isa_full_format_extension_word) {
  Soc s; A a(CODE_BASE);
  s.bus.raw_w32(0x3008, 0x4000); s.bus.raw_w32(0x3000, 0x4000);
  a.movel(A::Imm(0x3000), A::D(7)); a.movec_to(0, false, 7); a.nop();     // CACR: só garante que MOVEC escreve (ignorado)
  a.lea(A::Abs(0x3000), 0); a.moveq(4, 1);
  a.lea(A::Full(0, 0x100, 1, 2, 3), 2);                      // a0 + 0x100 + d1*4        = 0x3110
  a.lea(A::Full(0, 0x20, -1, 0, 2), 3);                      // índice suprimido, bd word = 0x3020
  a.lea(A::Full(-1, 0x5000, -1, 0, 3), 4);                   // base suprimida           = 0x5000
  a.moveq(8, 1);
  a.lea(A::Full(0, 0, 1, 0, 1, 2, 0x10), 5);                 // pré-indexado: [a0+d1] + 0x10 = 0x4010
  a.lea(A::Full(0, 0, 1, 0, 1, 6, 0x10), 6);                 // pós-indexado: [a0] + d1 + 0x10 = 0x4018
  a.lea(A::Full(0, 0, -1, 0, 1, 2, 0x10), 1);                // IS=1 indireto: [a0] + 0x10 = 0x4010
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_EQ(s.cpu.a[2], 0x3110); CHECK_EQ(s.cpu.a[3], 0x3020); CHECK_EQ(s.cpu.a[4], 0x5000);
  CHECK_EQ(s.cpu.a[5], 0x4010); CHECK_EQ(s.cpu.a[6], 0x4018); CHECK_EQ(s.cpu.a[1], 0x4010);
}

TEST(isa_full_format_pc_relative_and_data_access) {
  Soc s; A a(CODE_BASE);
  s.bus.raw_w32(0x3100, 0xCAFEF00D);
  a.lea(A::Abs(0x3000), 0); a.moveq(0, 1);
  u32 at = a.here();
  a.lea(A::Full(-1, 0x40, -1, 0, 2, 0, 0, true), 2);         // PC completo, sem índice: (ext word) + 0x40
  a.movel(A::Full(0, 0x100, 1, 0, 3), A::D(3));              // leitura via (bd,An,Xn)
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_EQ(s.cpu.a[2], at + 2 + 0x40); CHECK_EQ(s.cpu.d[3], 0xCAFEF00D);
}

TEST(isa_user_mode_usp_trap_and_privilege_violation) {
  Soc s; A a(CODE_BASE);
  a.movel(A::Imm(0x00E00000), A::A(0)); a.move_usp_from(0);                 // USP = 0xE00000
  a.moveq(0, 5); a.moveq(0, 6);
  a.movel(A::ImmL("user"), A::Pre(7)); a.movel(A::Imm(0x40000000), A::Pre(7));   // quadro para RTE: formato 4, SR=0 (modo usuário)
  a.rte();
  a.label("user"); a.movel(A::A(7), A::D(4)); a.trap(0); a.movel(A::A(7), A::D(3));
  a.stop(0x2000);                                                             // privilegiada em modo usuário -> vetor 8
  a.halt();                                                                    // (também privilegiada; nunca chega aqui)
  a.label("trap"); a.addq(4, 1, A::D(5)); a.movel(A::A(7), A::Abs(0x3000)); a.rte();
  a.label("priv"); a.moveq(1, 6); a.movel(A::Ind(7), A::D(2)); a.move_usp_to(1); a.halt();
  auto code = a.finish(); s.load(a.base(), code);
  s.bus.raw_w32(32 * 4, a.label_addr("trap")); s.bus.raw_w32(8 * 4, a.label_addr("priv"));
  s.cpu.reset_regs(); s.cpu.a[7] = 0x00F00000; s.cpu.set_pc(a.base());
  CHECK(s.run_until_halt()); s.bus.cache_flush_all(false);
  CHECK_EQ(s.cpu.d[4], 0x00E00000);                    // em usuário, A7 é a USP
  CHECK_EQ(s.cpu.d[3], 0x00E00000);                    // a USP volta intacta depois da TRAP
  CHECK_EQ(s.cpu.d[5], 1); CHECK_EQ(s.cpu.d[6], 1);
  CHECK(s.bus.raw_r32(0x3000) < 0x00F00000 && s.bus.raw_r32(0x3000) > 0x00EFF000);   // no handler a pilha é a de supervisor
  CHECK_EQ((s.cpu.d[2] >> 18) & 0xFF, 8);              // quadro da violação de privilégio
  CHECK_EQ(s.cpu.a[1], 0x00E00000);                    // MOVE USP,A1 em supervisor
  CHECK((s.cpu.sr & 0x2000) != 0);
}

TEST(isa_user_mode_cannot_change_sr_or_movec) {
  Soc s; A a(CODE_BASE);
  a.movel(A::ImmL("user"), A::Pre(7)); a.movel(A::Imm(0x40000000), A::Pre(7)); a.rte();
  a.label("user"); a.moveq(0, 7); a.move_to_sr(A::D(7)); a.halt();       // MOVE to SR privilegiado
  a.label("priv"); a.movel(A::Imm(77), A::D(0)); a.halt();
  auto code = a.finish(); s.load(a.base(), code); s.bus.raw_w32(8 * 4, a.label_addr("priv"));
  s.cpu.reset_regs(); s.cpu.a[7] = 0x00F00000; s.cpu.set_pc(a.base());
  CHECK(s.run_until_halt()); CHECK_EQ(s.cpu.d[0], 77);
}

TEST(isa_fscc_fsave_frestore) {
  Soc s; A a(CODE_BASE);
  s.bus.raw_w32(0x3000, 0x40000000); s.bus.raw_w32(0x3004, 0);                      // 2.0
  a.fop_ea(0x00, 5, A::Abs(0x3000), 0); a.fop_rr(0x3A, 0, 0);                         // FTST: positivo
  a.moveq(0, 1); a.moveq(0, 2); a.moveq(0, 3);
  a.fscc(0x12, A::D(1));                                                               // GT -> 0xFF
  a.fscc(0x14, A::D(2));                                                               // LT -> 0
  a.fscc(0x0F, A::Abs(0x3010));                                                        // T em memória
  a.movel(A::Imm(0x40), A::D(3)); a.fmove_to_ctrl(4, A::D(3));                         // FPCR: PREC simples
  a.lea(A::Abs(0x3100), 0); a.fsave(A::Ind(0));                                         // salva estado
  a.moveq(0, 4); a.fmove_to_ctrl(4, A::D(4));                                          // zera FPCR
  a.frestore(A::Ind(0)); a.fmove_from_ctrl(4, A::D(5));                                // restaura -> 0x40
  s.bus.raw_w32(0x3200, 0);                                                            // quadro nulo reseta a FPU
  a.lea(A::Abs(0x3200), 1); a.frestore(A::Ind(1)); a.fmove_from_ctrl(4, A::D(6));
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_EQ(s.cpu.d[1] & 0xFF, 0xFF); CHECK_EQ(s.cpu.d[2] & 0xFF, 0); CHECK_EQ(s.bus.ddr[0x3010], 0xFF);
  CHECK_EQ(s.bus.raw_r32(0x3100), 0x41000000); CHECK_EQ(s.bus.raw_r32(0x3104), 0x40);
  CHECK_EQ(s.cpu.d[5], 0x40); CHECK_EQ(s.cpu.d[6], 0);
}

TEST(isa_emac_accumulators_fractional_and_saturation) {
  Soc s; A a(CODE_BASE);
  // MACSR = 0: inteiro, sem sinal=0, sem saturação. Duas contas independentes em ACC1 e ACC2.
  a.movel(A::Imm(1000), A::D(0)); a.movel(A::Imm(-3), A::D(1));
  a.mac_x(false, 0, 1, 1); a.mac_x(false, 0, 1, 1); a.mac_x(true, 0, 1, 2);       // acc1 = -6000, acc2 = +3000
  a.mac_getacc(1, 2); a.mac_getacc(2, 3); a.movel(A::D(2), A::Abs(0x3300)); a.movel(A::D(3), A::Abs(0x3304));
  // palavras: Ry alto (0x0004) x Dx baixo (0x0005), modo W; deslocamento <<1
  a.movel(A::Imm(0x00040002), A::D(4)); a.movel(A::Imm(0x00090005), A::D(5)); a.mac_setacc(7, 0);   // d7 = 0 -> acc0 = 0
  a.mac_x(false, 4, 5, 0, true, true, false, +1);                                  // (4 * 5) << 1 = 40
  a.mac_movclr(0, 6); a.mac_getacc(0, 7);                                          // d6 = 40, acc0 limpo -> d7 = 0
  // fracionário: 0.5 * 0.5 = 0.25 (Q31): 0x40000000 * 0x40000000 >> 31 = 0x20000000
  a.movel(A::Imm(0x20), A::D(0)); a.mac_set_macsr(A::D(0));                        // F=1
  a.movel(A::Imm(0x40000000), A::D(1)); a.mac_x(false, 1, 1, 3); a.mac_getacc(3, 0);   // d0 = 0x20000000
  // saturação (OMC): 0x7FFFFFFF * 2 em inteiro satura
  a.movel(A::Imm(0x80), A::D(2)); a.mac_set_macsr(A::D(2));                        // OMC=1, inteiro
  a.movel(A::Imm(0x7FFFFFFF), A::D(3)); a.moveq(1, 4); a.mac_setacc(3, 0); a.mac_x(false, 4, 3, 0);   // acc0 = 0x7FFFFFFF + 0x7FFFFFFF = 0xFFFFFFFE: EV/PAV0, leitura satura
  a.mac_getacc(0, 5); a.mac_get_macsr(2);
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_EQ(s.bus.raw_r32(0x3300), (u32)-6000); CHECK_EQ(s.bus.raw_r32(0x3304), 3000);
  CHECK_EQ(s.cpu.d[5], 0x7FFFFFFF); CHECK(s.cpu.d[2] & 2);                          // saturou e V ficou ligado (sticky)
  CHECK_EQ(s.cpu.d[6], 40); CHECK_EQ(s.cpu.d[7], 0); CHECK_EQ(s.cpu.d[0], 0x20000000);
}

// Codificações do eMAC conferidas com as tabelas "Operation Code Map" e os formatos do CFPRM rev. 3.
TEST(isa_emac_opcodes_match_cfprm) {
  A a(0x1000);
  a.mac_x(false, 1, 2, 0);                                  // MAC.L d1,d2,ACC0          -> A401 0800
  a.mac_x(true, 1, 2, 1, true, true, false, +1);            // MSAC.W d1.U,d2.L<<1,ACC1  -> A481 0340
  a.mac_load(false, 3, A::Ind(4), 0, 5, 0);                 // MAC.L d0,d5,(a4),d3,ACC0  -> A694 5800
  a.mac_copy(2, 1);                                         // MOVE.L ACC1,ACC2          -> A511
  a.mac_get(2, 3);                                          // MOVE.L ACC2,d3            -> A583
  a.mac_movclr(2, 3);                                       // MOVCLR.L ACC2,d3          -> A5C3
  a.mac_set_macsr(A::D(2));                                 // MOVE.L d2,MACSR           -> A902
  a.mac_get_macsr(2);                                       // MOVE.L MACSR,d2           -> A982
  a.mac_macsr_ccr();                                        // MOVE.L MACSR,CCR          -> A9C0
  a.mac_set_mask(A::D(1));                                  // MOVE.L d1,MASK            -> AD01
  a.mac_get_mask(1);                                        // MOVE.L MASK,d1            -> AD81
  a.mac_set_accext(1, A::D(1));                             // MOVE.L d1,ACCext23        -> AF01
  a.mac_get_accext(0, 1);                                   // MOVE.L ACCext01,d1        -> AB81
  a.mac_x(false, 0, 1, 2, false, false, false, 0, 3, 1);    // MASAC.L d0,d1,ACC2,ACC1   -> A200 0817
  auto c = a.finish();
  auto w = [&](size_t i) { return (u32)((c[2 * i] << 8) | c[2 * i + 1]); };
  const u32 want[] = {0xA401, 0x0800, 0xA481, 0x0340, 0xA694, 0x5800, 0xA511, 0xA583, 0xA5C3, 0xA902, 0xA982, 0xA9C0,
                      0xAD01, 0xAD81, 0xAF01, 0xAB81};
  size_t k = 0;
  for (u32 x : want) CHECK_EQ(w(k++), x);
  // MASAC.L: op = A000 | d1<<9 | ACCx lsb(0) | d0; ext = longo | ACCx msb(1)<<4 | ACCw(1)<<2 | dual(3)
  CHECK_EQ(w(k), 0xA200u); CHECK_EQ(w(k + 1), 0x0817u);
}

TEST(isa_emac_behaviour_copy_clear_dual_load) {
  Soc s; A a(CODE_BASE);
  // MOVE.L <ea>,ACC e leitura: integer sem sinal, acumuladores independentes
  a.movel(A::Imm(3), A::D(0)); a.movel(A::Imm(4), A::D(1));
  a.mac_set_acc(0, A::Imm(0)); a.mac_set_acc(1, A::Imm(0));
  a.mac_x(false, 0, 1, 0, false, false, false, 0, 1, 1);   // MAAAC: acc0 += 12 e acc1 += 12
  a.mac_x(false, 0, 1, 0, false, false, false, 0, 3, 1);   // MASAC: acc0 += 12 e acc1 -= 12
  a.mac_get(0, 2); a.mac_get(1, 3);                          // d2 = 24, d3 = 0
  a.mac_copy(2, 0); a.mac_get(2, 4);                         // ACC2 = ACC0 -> d4 = 24
  a.movel(A::Imm(0x33000), A::A(4)); a.movel(A::Imm(0x2468), A::Abs(0x33000));
  a.mac_load(false, 5, A::Ind(4), 0, 1, 3);                  // acc3 += d0*d1 = 12, d5 = [a4] = 0x2468
  a.mac_movclr(3, 6); a.mac_get(3, 7);                       // d6 = 12, acc3 limpo -> d7 = 0
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_EQ(s.cpu.d[2], 24u); CHECK_EQ(s.cpu.d[3], 0u); CHECK_EQ(s.cpu.d[4], 24u);
  CHECK_EQ(s.cpu.d[5], 0x2468u); CHECK_EQ(s.cpu.d[6], 12u); CHECK_EQ(s.cpu.d[7], 0u);
}
