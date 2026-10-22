#include "helpers.h"

// ======================= Item 3: I-cache com dados, MMU em acessos que cruzam página, modelo de ciclos =======================
TEST(icache_holds_stale_instructions_until_invalidated) {
  Soc s; A a(CODE_BASE);
  a.moveq(0, 2); a.moveq(0, 3);
  a.bsr("sub"); a.movel(A::D(0), A::D(1));                                            // d1 = 1
  a.move(2, A::Imm(0x7002), A::Abs(a.base() + 0x100));                                // reescreve "moveq #1,d0" -> "moveq #2,d0" (via D-cache)
  a.movel(A::Imm(a.base() + 0x100), A::Abs(BRIDGE_BASE + BR_CACHE_ADDR)); a.movel(A::Imm(16), A::Abs(BRIDGE_BASE + BR_CACHE_LEN));
  a.movel(A::Imm(CACHE_FLUSH_RANGE), A::Abs(BRIDGE_BASE + BR_CACHE_OP));              // a memória já tem o código novo...
  a.bsr("sub"); a.movel(A::D(0), A::D(2));                                            // ...mas a I-cache ainda serve a instrução velha: d2 = 1
  a.movel(A::Imm(ICACHE_INVAL_RANGE), A::Abs(BRIDGE_BASE + BR_CACHE_OP));
  a.bsr("sub"); a.movel(A::D(0), A::D(3));                                            // depois da invalidação: d3 = 2
  a.halt();
  while (a.here() < a.base() + 0x100) a.nop();
  a.label("sub"); a.moveq(1, 0); a.rts();
  // "sub" precisa estar em a.base()+0x100
  CHECK_EQ(a.label_addr("sub"), a.base() + 0x100);
  CHECK(run_prog(s, a));
  CHECK_EQ(s.cpu.d[1], 1); CHECK_EQ(s.cpu.d[2], 1); CHECK_EQ(s.cpu.d[3], 2);
  CHECK(s.bus.ihits > 0 && s.bus.imisses > 0);
}

TEST(icache_can_be_disabled_and_loader_invalidates) {
  Soc s;
  s.bus.raw_w32(0x2000, 0x70010000 | 0x4E75); s.bus.raw_w16(0x2000, 0x7001);        // moveq #1,d0 ; rts
  u16 w = s.bus.fetch16(0x2000); CHECK_EQ(w, 0x7001);
  s.bus.raw_w16(0x2000, 0x7002); CHECK_EQ(s.bus.fetch16(0x2000), 0x7001);           // raw_w16 não invalida: linha velha
  u8 nw[2] = {0x70, 0x03}; s.bus.raw_write(0x2000, nw, 2); CHECK_EQ(s.bus.fetch16(0x2000), 0x7003);   // o carregador invalida
  mmio_w(s, BRIDGE_BASE + BR_CACHE_CTRL, 1 | 2);                                       // D-cache ligada, I-cache desligada
  s.bus.raw_w16(0x2000, 0x7004); CHECK_EQ(s.bus.fetch16(0x2000), 0x7004);
  CHECK_EQ(mmio_r(s, BRIDGE_BASE + BR_CACHE_CTRL), 3);
}

TEST(mmu_access_crossing_a_page_translates_each_page) {
  Soc s; s.bus.cache_enabled = false;
  s.bus.tlb[0] = {true, 0x40, 0x80, MP_R | MP_W | MP_X}; s.bus.tlb[1] = {true, 0x41, 0x90, MP_R | MP_W | MP_X};   // páginas virtuais contíguas, físicas não
  s.bus.mmu_on = true;
  s.bus.cpu_write(0x40FFE, 4, 0xAABBCCDD);
  CHECK(!s.bus.fault); CHECK_EQ(s.bus.ddr[0x80FFE], 0xAA); CHECK_EQ(s.bus.ddr[0x80FFF], 0xBB); CHECK_EQ(s.bus.ddr[0x90000], 0xCC); CHECK_EQ(s.bus.ddr[0x90001], 0xDD);
  CHECK_EQ(s.bus.cpu_read(0x40FFE, 4), 0xAABBCCDD); CHECK_EQ(s.bus.cpu_read(0x40FFF, 2), 0xBBCC); CHECK_EQ(s.bus.cpu_read(0x40FFD, 4), 0x00AABBCC);
  s.bus.ddr[0x80FFF] = 0x12; s.bus.ddr[0x90000] = 0x34; CHECK_EQ(s.bus.fetch16(0x40FFF), 0x1234);               // busca de instrução cruzando a página
  // segunda página sem tradução: falha e nenhuma escrita parcial
  s.bus.tlb[1].v = false; s.bus.ddr[0x80FFE] = 0; s.bus.ddr[0x80FFF] = 0;
  s.bus.cpu_write(0x40FFE, 4, 0x11223344);
  CHECK(s.bus.fault); CHECK_EQ(s.bus.mmu_far, 0x41000); CHECK_EQ(s.bus.mmu_fsr & 3, 1); CHECK_EQ(s.bus.ddr[0x80FFE], 0);
  s.bus.fault = false;
  // proteção na segunda página (somente leitura)
  s.bus.tlb[1] = {true, 0x41, 0x90, MP_R}; s.bus.cpu_write(0x40FFE, 4, 0x55667788);
  CHECK(s.bus.fault); CHECK_EQ(s.bus.mmu_fsr & 3, 2); CHECK_EQ(s.bus.ddr[0x80FFE], 0);
}

// Palavra de extensão numa página sem tradução: a busca falha e devolve 0, e ADDQ #1,-4(A6) (ext. FFFC) enxergava d16 = 0 e gravava em (A6)
// antes do reinício (no Dashboard isso incrementava o A6 salvo na pilha). Com o erro de acesso sinalizado nada pode ser gravado.
TEST(a_fetch_fault_on_the_extension_word_writes_nothing_before_the_restart) {
  Soc s; s.bus.cache_enabled = false;
  s.bus.tlb[0] = {true, 0x40, 0x80, MP_R | MP_X}; s.bus.tlb[1] = {true, 0x50, 0xA0, MP_R | MP_W};    // código em 0x40000 (a página 0x41000 não tem tradução); dados em 0x50000
  s.bus.mmu_on = true;
  s.bus.raw_w16(0x80FFE, 0x52AE);                                                  // addq.l #1,-4(a6): opcode em 0x40FFE, extensão em 0x41000
  s.bus.raw_w32(0xA0000, 0x11111111); s.bus.raw_w32(0xA0004, 0x22222222);
  s.bus.raw_w32(0x2008, 0x3000);                                                   // vetor 2 (erro de acesso) -> 0x3000
  s.cpu.reset_regs(); s.cpu.sr = 0x0000; s.cpu.vbr = 0x2000; s.cpu.other_sp = 0x00F00000; s.cpu.a[7] = 0x60000; s.cpu.a[6] = 0x50004; s.cpu.set_pc(0x40FFE);
  s.cpu.step();
  CHECK_EQ(s.cpu.exc_vec, 2); CHECK_EQ(s.cpu.exc_pc, 0x40FFEu); CHECK_EQ(s.cpu.pc, 0x3000u);       // reiniciável: a instrução volta a começar em 0x40FFE
  CHECK_EQ(s.cpu.a[6], 0x50004u);
  CHECK_EQ(s.bus.raw_r32(0xA0000), 0x11111111u); CHECK_EQ(s.bus.raw_r32(0xA0004), 0x22222222u);   // nem (A6-4) nem (A6) foram tocados
}

// ---- modelo de ciclos ----
static u64 cycles_of(A& a) { Soc s; CHECK(run_prog(s, a)); return s.cpu.cycles; }

TEST(timing_branch_prediction_backward_taken) {
  { A a(CODE_BASE); a.moveq(3, 0); a.label("l"); a.subq(4, 1, A::D(0)); a.bne("l"); a.halt();
    // moveq 1 + 3 x subq 1 + 2 x bne tomado previsto (2) + bne final previsto-tomado mas não tomado (1 + 5) + halt 1
    CHECK_EQ(cycles_of(a), 1 + 3 + 2 * 2 + 6 + 1); }
  { A a(CODE_BASE); a.moveq(0, 0); a.tst(4, A::D(0)); a.bne("skip"); a.moveq(1, 1); a.label("skip"); a.halt();
    // bne para frente, não tomado: previsto certo (custo base 1)
    CHECK_EQ(cycles_of(a), 1 + 1 + 1 + 1 + 1); }
  { A a(CODE_BASE); a.moveq(1, 0); a.tst(4, A::D(0)); a.bne("skip"); a.moveq(1, 1); a.label("skip"); a.halt();
    // bne para frente tomado: errou a predição (1 + 5)
    CHECK_EQ(cycles_of(a), 1 + 1 + 6 + 1); }
}

TEST(timing_address_generation_interlock) {
  // lea + uso imediato do An como base custa 1 ciclo a mais; com um nop no meio o custo some (e o nop paga o ciclo)
  u64 back_to_back, spaced;
  { A a(CODE_BASE); a.lea(A::Abs(0x3000), 0); a.movel(A::Ind(0), A::D(1)); a.halt(); back_to_back = cycles_of(a); }
  { A a(CODE_BASE); a.lea(A::Abs(0x3000), 0); a.nop(); a.movel(A::Ind(0), A::D(1)); a.halt(); spaced = cycles_of(a); }
  CHECK_EQ(spaced, back_to_back);
  u64 other_reg;
  { A a(CODE_BASE); a.lea(A::Abs(0x3000), 0); a.movel(A::Ind(1), A::D(1)); a.halt(); other_reg = cycles_of(a); }
  CHECK_EQ(other_reg + 1, back_to_back);                                  // A1 não foi tocado: sem interlock
}

TEST(timing_arithmetic_and_fpu_latencies) {
  auto prog = [&](int which) { A a(CODE_BASE); a.moveq(7, 0); a.moveq(3, 1);
    switch (which) { case 0: a.nop(); break; case 1: a.muls_l(A::D(1), 0); break; case 2: a.divs_l(A::D(1), 0, 2); break;
      case 3: a.fop_rr(0x22, 0, 1); break; case 4: a.fop_rr(0x23, 0, 1); break; case 5: a.fop_rr(0x20, 0, 1); break; case 6: a.fop_rr(0x04, 0, 1); break; }
    a.halt(); return cycles_of(a); };
  u64 base = prog(0);
  CHECK(prog(1) > base + 1 && prog(1) < prog(2));                           // mul < div
  CHECK(prog(2) >= base + 15);
  u64 fadd = prog(3), fmul = prog(4), fdiv = prog(5), fsqrt = prog(6);
  CHECK(fadd < fmul && fmul < fdiv && fdiv < fsqrt);
  CHECK(fdiv - base >= 20);
}
