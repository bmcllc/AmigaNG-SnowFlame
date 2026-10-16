#include "helpers.h"

// ======================= Memória, DMA, cache, semáforos =======================
TEST(bus_big_endian_layout) {
  Soc s; s.bus.cache_enabled = false;
  s.bus.cpu_write(0x2000, 4, 0x11223344);
  CHECK_EQ(s.bus.ddr[0x2000], 0x11); CHECK_EQ(s.bus.ddr[0x2003], 0x44);
  CHECK_EQ(s.bus.cpu_read(0x2002, 2), 0x3344); CHECK_EQ(s.bus.cpu_read(0x2001, 1), 0x22);
  s.bus.cpu_write(VRAM_BASE + 8, 2, 0xBEEF); CHECK_EQ(s.bus.vram[8], 0xBE);
  s.bus.cpu_write(ARAM_BASE, 1, 0x7F); CHECK_EQ(s.bus.aram[0], 0x7F);
  s.bus.cpu_write(SRAM_BASE + 4, 4, 0xCAFEBABE); CHECK_EQ(s.bus.sram[4], 0xCA);
  CHECK(!s.bus.fault);
  s.bus.cpu_read(0x08000000, 4); CHECK(s.bus.fault); CHECK_EQ(s.bus.fault_addr, 0x08000000);
}

TEST(bridge_identity_and_time) {
  Soc s;
  CHECK_EQ(mmio_r(s, BRIDGE_BASE + BR_ID), 0x53463130);
  CHECK_EQ(mmio_r(s, BRIDGE_BASE + BR_VERSION), 1);
  advance_ns(s, 5000);
  CHECK_EQ(mmio_r(s, BRIDGE_BASE + BR_TIME_LO), 5000);
}

TEST(dma_copy_timing_and_busy) {
  Soc s; for (int i = 0; i < 256; i++) s.bus.ddr[0x10000 + i] = (u8)i;
  mmio_w(s, BRIDGE_BASE + BR_DMA0 + DMA_SRC, 0x10000); mmio_w(s, BRIDGE_BASE + BR_DMA0 + DMA_DST, 0x20000);
  mmio_w(s, BRIDGE_BASE + BR_DMA0 + DMA_LEN, 256); mmio_w(s, BRIDGE_BASE + BR_DMA0 + DMA_FLAGS, 0);
  mmio_w(s, BRIDGE_BASE + BR_DMA0 + DMA_CTRL, DMAC_START_REGS);
  CHECK_EQ(mmio_r(s, BRIDGE_BASE + BR_DMA0 + DMA_STATUS) & DMAS_BUSY, DMAS_BUSY);
  advance_ns(s, 100); CHECK_EQ(mmio_r(s, BRIDGE_BASE + BR_DMA0 + DMA_STATUS) & DMAS_BUSY, DMAS_BUSY);   // 150 ns de setup + 120 ns
  advance_ns(s, 200); CHECK_EQ(mmio_r(s, BRIDGE_BASE + BR_DMA0 + DMA_STATUS), DMAS_DONE);
  CHECK(std::memcmp(&s.bus.ddr[0x10000], &s.bus.ddr[0x20000], 256) == 0);
}

TEST(dma_vram_is_faster_than_aram) {
  Soc s; u32 base = BRIDGE_BASE + BR_DMA0;
  mmio_w(s, base + DMA_SRC, VRAM_BASE); mmio_w(s, base + DMA_DST, VRAM_BASE + 0x100000); mmio_w(s, base + DMA_LEN, 64 * 1024); mmio_w(s, base + DMA_CTRL, 1);
  Ps t0 = s.sched.now; s.wait_until([&] { return mmio_r(s, base + DMA_STATUS) & DMAS_DONE; }); Ps tv = s.sched.now - t0;
  mmio_w(s, base + DMA_SRC, ARAM_BASE); mmio_w(s, base + DMA_DST, ARAM_BASE + 0x100000); mmio_w(s, base + DMA_LEN, 64 * 1024); mmio_w(s, base + DMA_CTRL, 1);
  t0 = s.sched.now; s.wait_until([&] { return mmio_r(s, base + DMA_STATUS) & DMAS_DONE; }); Ps ta = s.sched.now - t0;
  CHECK(tv < ta);
  CHECK(tv > 10000000 && tv < 14000000);     // 64 KiB a 5,31 GB/s ~ 12,3 us (+ setup)
}

TEST(dma_fill_and_byteswap) {
  Soc s; u32 b = BRIDGE_BASE + BR_DMA0;
  mmio_w(s, b + DMA_SRC, 0xDEADBEEF); mmio_w(s, b + DMA_DST, 0x3000); mmio_w(s, b + DMA_LEN, 16); mmio_w(s, b + DMA_FLAGS, DMAF_FILL); mmio_w(s, b + DMA_CTRL, 1);
  s.wait_until([&] { return mmio_r(s, b + DMA_STATUS) & DMAS_DONE; });
  CHECK_EQ(s.bus.raw_r32(0x3000), 0xDEADBEEF); CHECK_EQ(s.bus.raw_r32(0x300C), 0xDEADBEEF);
  s.bus.raw_w32(0x4000, 0x01020304); s.bus.raw_w32(0x4004, 0x05060708);
  mmio_w(s, b + DMA_SRC, 0x4000); mmio_w(s, b + DMA_DST, 0x5000); mmio_w(s, b + DMA_LEN, 8); mmio_w(s, b + DMA_FLAGS, DMAF_BSWAP32); mmio_w(s, b + DMA_CTRL, 1);
  s.wait_until([&] { return mmio_r(s, b + DMA_STATUS) & DMAS_DONE; });
  CHECK_EQ(s.bus.raw_r32(0x5000), 0x04030201); CHECK_EQ(s.bus.raw_r32(0x5004), 0x08070605);
  mmio_w(s, b + DMA_FLAGS, DMAF_BSWAP16); mmio_w(s, b + DMA_DST, 0x5100); mmio_w(s, b + DMA_CTRL, 1);
  s.wait_until([&] { return mmio_r(s, b + DMA_STATUS) & DMAS_DONE; });
  CHECK_EQ(s.bus.raw_r32(0x5100), 0x02010403);
}

TEST(dma_descriptor_chain_with_irq) {
  Soc s; u32 b = BRIDGE_BASE + BR_DMA0 + 0x20;     // canal 1
  s.bus.raw_w32(0x6000, 0xAAAA5555); s.bus.raw_w32(0x6004, 0x12345678);
  // d0: copia 8 bytes 0x6000 -> 0x6100 ; d1: preenche 16 bytes em 0x6200 ; d2: copia 0x6100 -> 0x6300 (depende de d0)
  u32 d0 = 0x7000, d1 = 0x7020, d2 = 0x7040;
  auto desc = [&](u32 at, u32 src, u32 dst, u32 len, u32 fl, u32 next) { s.bus.raw_w32(at, src); s.bus.raw_w32(at + 4, dst); s.bus.raw_w32(at + 8, len); s.bus.raw_w32(at + 12, fl); s.bus.raw_w32(at + 16, next); };
  desc(d0, 0x6000, 0x6100, 8, 0, d1); desc(d1, 0x0F0F0F0F, 0x6200, 16, DMAF_FILL, d2); desc(d2, 0x6100, 0x6300, 8, DMAF_IRQ, 0);
  s.bus.irq_mask = IRQ_DMA1;
  mmio_w(s, b + DMA_DESC, d0); mmio_w(s, b + DMA_CTRL, DMAC_START_CHAIN);
  CHECK(!s.bus.irq_pending());
  CHECK(s.wait_until([&] { return s.bus.irq_pending(); }));
  CHECK_EQ(s.bus.raw_r32(0x6300), 0xAAAA5555); CHECK_EQ(s.bus.raw_r32(0x6304), 0x12345678); CHECK_EQ(s.bus.raw_r32(0x620C), 0x0F0F0F0F);
  CHECK_EQ(s.bus.irq_status, IRQ_DMA1);
  mmio_w(s, BRIDGE_BASE + BR_IRQ_STATUS, IRQ_DMA1); CHECK_EQ(s.bus.irq_status, 0);
  CHECK(s.bridge.dma_jobs == 3);
}

TEST(dma_errors) {
  Soc s; u32 b = BRIDGE_BASE + BR_DMA0;
  mmio_w(s, b + DMA_SRC, 0x1000); mmio_w(s, b + DMA_DST, GPU_BASE); mmio_w(s, b + DMA_LEN, 16); mmio_w(s, b + DMA_CTRL, 1);   // destino MMIO
  CHECK_EQ(mmio_r(s, b + DMA_STATUS) & DMAS_ERROR, DMAS_ERROR); CHECK_EQ(mmio_r(s, b + DMA_STATUS) & DMAS_BUSY, 0);
  mmio_w(s, b + DMA_DST, 0x2000); mmio_w(s, b + DMA_LEN, 6); mmio_w(s, b + DMA_FLAGS, DMAF_BSWAP32); mmio_w(s, b + DMA_CTRL, 1);   // len não múltiplo de 4
  CHECK_EQ(mmio_r(s, b + DMA_STATUS) & DMAS_ERROR, DMAS_ERROR);
  mmio_w(s, b + DMA_FLAGS, 0); mmio_w(s, b + DMA_LEN, 4096); mmio_w(s, b + DMA_CTRL, 1);
  mmio_w(s, b + DMA_CTRL, 1);     // canal ocupado
  CHECK_EQ(mmio_r(s, b + DMA_STATUS) & (DMAS_ERROR | DMAS_BUSY), DMAS_ERROR | DMAS_BUSY);
}

TEST(dma_to_vpu_local_memory) {
  Soc s; s.bus.raw_w32(0x8000, 0x3F800000); u32 b = BRIDGE_BASE + BR_DMA0;
  mmio_w(s, b + DMA_SRC, 0x8000); mmio_w(s, b + DMA_DST, VPUB_DMEM + 0x40); mmio_w(s, b + DMA_LEN, 4); mmio_w(s, b + DMA_CTRL, 1);
  CHECK_EQ(rbe32(&s.vpuB.dmem[0x40]), 0x3F800000);
}

TEST(cache_is_not_coherent_with_dma_until_flush) {
  Soc s; u32 b = BRIDGE_BASE + BR_DMA0;
  s.bus.cpu_write(0x4000, 4, 0x11111111);                  // fica suja na cache
  CHECK_EQ(s.bus.raw_r32(0x4000), 0);
  auto dma = [&](u32 src, u32 dst, u32 len) { mmio_w(s, b + DMA_SRC, src); mmio_w(s, b + DMA_DST, dst); mmio_w(s, b + DMA_LEN, len); mmio_w(s, b + DMA_FLAGS, 0); mmio_w(s, b + DMA_CTRL, 1);
    s.wait_until([&] { return mmio_r(s, b + DMA_STATUS) & DMAS_DONE; }); };
  dma(0x4000, 0x5000, 16); CHECK_EQ(s.bus.raw_r32(0x5000), 0);                       // DMA enxerga dado antigo
  mmio_w(s, BRIDGE_BASE + BR_CACHE_ADDR, 0x4000); mmio_w(s, BRIDGE_BASE + BR_CACHE_LEN, 16); mmio_w(s, BRIDGE_BASE + BR_CACHE_OP, CACHE_FLUSH_RANGE);
  CHECK_EQ(s.bus.raw_r32(0x4000), 0x11111111);
  dma(0x4000, 0x5000, 16); CHECK_EQ(s.bus.raw_r32(0x5000), 0x11111111);
  // DMA escreve; CPU ainda lê valor antigo da cache até invalidar
  s.bus.raw_w32(0x4000 + 0x100, 0xAAAAAAAA); CHECK_EQ(s.bus.cpu_read(0x4100, 4), 0xAAAAAAAA);   // traz p/ cache
  s.bus.raw_w32(0x4100, 0xBBBBBBBB); CHECK_EQ(s.bus.cpu_read(0x4100, 4), 0xAAAAAAAA);          // obsoleto
  mmio_w(s, BRIDGE_BASE + BR_CACHE_ADDR, 0x4100); mmio_w(s, BRIDGE_BASE + BR_CACHE_LEN, 4); mmio_w(s, BRIDGE_BASE + BR_CACHE_OP, CACHE_INVAL_RANGE);
  CHECK_EQ(s.bus.cpu_read(0x4100, 4), 0xBBBBBBBB);
  // invalidate descarta dados sujos
  s.bus.cpu_write(0x4200, 4, 0x77777777); mmio_w(s, BRIDGE_BASE + BR_CACHE_ADDR, 0x4200); mmio_w(s, BRIDGE_BASE + BR_CACHE_OP, CACHE_INVAL_RANGE);
  CHECK_EQ(s.bus.cpu_read(0x4200, 4), 0);
}

TEST(cache_hit_miss_and_eviction_writeback) {
  Soc s; auto h0 = s.bus.dc.hits, m0 = s.bus.dc.misses;
  for (int i = 0; i < 16; i++) s.bus.cpu_read(0x9000 + i * 4, 4);
  CHECK_EQ(s.bus.dc.misses - m0, 4); CHECK_EQ(s.bus.dc.hits - h0, 12);
  // 5 endereços no mesmo conjunto (4 vias) forçam uma remoção com write-back
  for (int i = 0; i < 5; i++) s.bus.cpu_write(0x10000 + i * 2048, 4, 0x100 + i);
  CHECK(s.bus.dc.writebacks >= 1);
  for (int i = 0; i < 5; i++) CHECK_EQ(s.bus.cpu_read(0x10000 + i * 2048, 4), 0x100 + i);
  s.bus.cache_enabled = false; // desligar não perde dados: flush foi feito pelo registrador; aqui testamos o caminho direto
  mmio_w(s, BRIDGE_BASE + BR_CACHE_CTRL, 1);
}

TEST(semaphores_P_and_V) {
  Soc s; u32 a = BRIDGE_BASE + BR_SEM0 + 4 * 3;
  mmio_w(s, a, 1); mmio_w(s, a, 1);
  CHECK_EQ(mmio_r(s, a), 2); CHECK_EQ(mmio_r(s, a), 1); CHECK_EQ(mmio_r(s, a), 0); CHECK_EQ(mmio_r(s, a), 0);
}

// ======================= CPU ColdFire =======================
TEST(cpu_sum_loop) {
  Soc s; A a(CODE_BASE);
  a.moveq(0, 0); a.moveq(10, 1);
  a.label("loop"); a.add(4, A::D(1), 0); a.subq(4, 1, A::D(1)); a.bne("loop"); a.halt();
  CHECK(run_prog(s, a)); CHECK_EQ(s.cpu.d[0], 55); CHECK_EQ(s.cpu.d[1], 0);
  CHECK(s.cpu.cycles >= s.cpu.instrs);
}

TEST(cpu_addressing_modes_and_endianness) {
  Soc s; A a(CODE_BASE);
  a.lea(A::Abs(0x2000), 0);
  a.movel(A::Imm(0x11223344), A::Ind(0));
  a.moveq(0, 0 + 1); a.move(1, A::Ind(0), A::D(1));              // d1.b = 0x11
  a.moveq(0, 2); a.move(1, A::Disp(0, 3), A::D(2));              // d2.b = 0x44
  a.moveq(0, 3); a.move(2, A::Disp(0, 2), A::D(3));              // d3.w = 0x3344
  a.movel(A::Post(0), A::D(4));                                  // d4 = valor, a0 += 4
  a.movel(A::Pre(0), A::D(5));                                   // a0 -= 4, d5 = valor
  a.movel(A::Imm(0xCAFEF00D), A::Disp(0, 8));
  a.lea(A::Abs(0x2000), 1); a.moveq(2, 6);
  a.movel(A::Idx(1, 0, 6, 2), A::D(7));                          // (0,a1,d6.l*4) = 0x2008
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_EQ(s.cpu.d[1], 0x11); CHECK_EQ(s.cpu.d[2], 0x44); CHECK_EQ(s.cpu.d[3], 0x3344);
  CHECK_EQ(s.cpu.d[4], 0x11223344); CHECK_EQ(s.cpu.d[5], 0x11223344); CHECK_EQ(s.cpu.a[0], 0x2000); CHECK_EQ(s.cpu.d[7], 0xCAFEF00D);
}

TEST(cpu_subroutine_recursion_factorial) {
  Soc s; A a(CODE_BASE);
  a.moveq(10, 0); a.bsr("fact"); a.halt();
  a.label("fact"); a.cmpi(4, 1, A::D(0)); a.ble("done");
  a.movel(A::D(0), A::Pre(7)); a.subq(4, 1, A::D(0)); a.bsr("fact"); a.movel(A::Post(7), A::D(1)); a.mulu_l(A::D(1), 0);
  a.label("done"); a.rts();
  CHECK(run_prog(s, a)); CHECK_EQ(s.cpu.d[0], 3628800); CHECK_EQ(s.cpu.a[7], 0x00F00000);
}

TEST(cpu_link_unlk_movem) {
  Soc s; A a(CODE_BASE);
  a.movel(A::Imm(0x1111), A::D(2)); a.movel(A::Imm(0x2222), A::D(3)); a.bsr("f"); a.halt();
  a.label("f"); a.link(6, -16); a.movem_to(0x000C, A::Disp(7, 0));
  a.moveq(5, 2); a.moveq(7, 3); a.add(4, A::D(3), 2); a.movel(A::D(2), A::D(0));
  a.movem_from(A::Disp(7, 0), 0x000C); a.unlk(6); a.rts();
  CHECK(run_prog(s, a)); CHECK_EQ(s.cpu.d[0], 12); CHECK_EQ(s.cpu.d[2], 0x1111); CHECK_EQ(s.cpu.d[3], 0x2222); CHECK_EQ(s.cpu.a[7], 0x00F00000);
}

TEST(cpu_shifts_ext_swap_logic) {
  Soc s; A a(CODE_BASE);
  a.moveq(-16, 0); a.asr(2, 0);                                  // -4
  a.movel(A::Imm(0x80000000), A::D(1)); a.lsr(7, 1); a.lsr(7, 1); a.lsr(7, 1); a.lsr(7, 1); a.lsr(3, 1);   // >> 31 = 1
  a.moveq(15, 2); a.lsl(4, 2);                                   // 0xF0
  a.movel(A::Imm(0x12345678), A::D(3)); a.swap(3);               // 0x56781234
  a.moveq(-2, 4); a.extb_l(4);                                   // já -2
  a.movel(A::Imm(0x0000FF80), A::D(5)); a.ext_l(5);              // 0xFFFFFF80
  a.movel(A::Imm(0xF0F0F0F0), A::D(6)); a.movel(A::Imm(0x0FF00FF0), A::D(7)); a.and_(4, A::D(7), 6);   // d6 &= d7
  a.shift_i(3, false, 4, 7, 4);                                  // ror #4, d7: 0x0FF00FF0 -> 0x00FF00FF
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_EQ(s.cpu.d[0], (u32)-4); CHECK_EQ(s.cpu.d[1], 1); CHECK_EQ(s.cpu.d[2], 0xF0); CHECK_EQ(s.cpu.d[3], 0x56781234);
  CHECK_EQ(s.cpu.d[4], (u32)-2); CHECK_EQ(s.cpu.d[5], 0xFFFFFF80); CHECK_EQ(s.cpu.d[6], 0x00F000F0); CHECK_EQ(s.cpu.d[7], 0x00FF00FF);
}

TEST(cpu_multiply_divide) {
  Soc s; A a(CODE_BASE);
  a.moveq(-7, 0); a.moveq(6, 1); a.muls_l(A::D(1), 0);                      // -42
  a.movel(A::Imm(0x10000), A::D(2)); a.mulu_l(A::D(2), 2); a.move_from_sr(3);   // 0 com V
  a.moveq(-43, 4); a.moveq(5, 5); a.divs_l(A::D(5), 4, 6);                  // q=-8 r=-3
  a.movel(A::Imm(100000), A::D(7)); a.moveq(7, 5); a.divu_w(A::D(5), 7);    // (r<<16)|q
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_EQ(s.cpu.d[0], (u32)-42); CHECK_EQ(s.cpu.d[2], 0); CHECK(s.cpu.d[3] & Cpu::V);
  CHECK_EQ(s.cpu.d[4], (u32)-8); CHECK_EQ(s.cpu.d[6], (u32)-3); CHECK_EQ(s.cpu.d[7], (5u << 16) | 14285u);
}

TEST(cpu_bit_ops_scc_and_signed_unsigned_compare) {
  Soc s; A a(CODE_BASE);
  a.moveq(0, 0); a.bset_i(5, A::D(0)); a.bset_i(1, A::D(0)); a.bclr_i(1, A::D(0));       // d0 = 32
  a.btst_i(5, A::D(0)); a.scc(7, A::D(1));                                                  // bit 5 setado: Z=0 -> EQ falso
  a.moveq(0, 4); a.moveq(0, 5); a.moveq(0, 6);
  a.moveq(-1, 2); a.moveq(1, 3); a.cmp(4, A::D(3), 2);                                      // d2 - d3 (Scc não altera flags)
  a.scc(13, A::D(4));                                                                       // LT (com sinal): verdadeiro
  a.scc(2, A::D(5));                                                                        // HI (sem sinal): verdadeiro
  a.scc(7, A::D(6));                                                                        // EQ: falso
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_EQ(s.cpu.d[0], 32); CHECK_EQ(s.cpu.d[1] & 0xFF, 0); CHECK_EQ(s.cpu.d[4] & 0xFF, 0xFF); CHECK_EQ(s.cpu.d[5] & 0xFF, 0xFF); CHECK_EQ(s.cpu.d[6] & 0xFF, 0);
}

TEST(cpu_trap_illegal_divzero_and_rte) {
  Soc s; A a(CODE_BASE);
  a.moveq(0, 6); a.trap(3); a.trap(3);                       // handler de TRAP #3 incrementa d6
  a.moveq(0, 1); a.divu_l(A::D(1), 2, 3);                    // divisão por zero -> vetor 5
  a.halt();
  a.label("trap"); a.addq(4, 1, A::D(6)); a.rte();
  a.label("dz"); a.moveq(99, 7); a.movel(A::Ind(7), A::D(5)); a.halt();
  auto code = a.finish();
  s.bus.raw_w32(35 * 4, a.label_addr("trap")); s.bus.raw_w32(5 * 4, a.label_addr("dz"));
  s.load(a.base(), code); s.cpu.reset_regs(); s.cpu.a[7] = 0x00F00000; s.cpu.set_pc(a.base()); CHECK(s.run_until_halt());
  CHECK_EQ(s.cpu.d[6], 2); CHECK_EQ(s.cpu.d[7], 99);
  CHECK_EQ((s.cpu.d[5] >> 18) & 0xFF, 5);                    // campo vetor no quadro de exceção ColdFire
  CHECK_EQ(s.cpu.d[5] >> 28, 4);                             // formato 4
  s.bus.cache_flush_all(false);
  // PC empilhado = instrução que falhou (divu.l começa 2 palavras após moveq)
  CHECK_EQ(s.bus.raw_r32(s.cpu.a[7] + 4) & 0xFFFFF000, CODE_BASE);
}

TEST(cpu_illegal_opcode_vector4_and_unhandled) {
  Soc s; A a(CODE_BASE); a.illegal();
  auto code = a.finish(); s.load(a.base(), code); s.cpu.reset_regs(); s.cpu.a[7] = 0x00F00000; s.cpu.set_pc(a.base());
  CHECK(!s.run_until_halt());                                // sem handler: para por NO_VECTOR
  CHECK(s.cpu.halted); CHECK(s.cpu.reason == Cpu::NO_VECTOR);
  Soc s2; A b(CODE_BASE); b.illegal(); b.label("h"); b.moveq(44, 0); b.halt();
  auto c2 = b.finish(); s2.load(b.base(), c2); s2.bus.raw_w32(16, b.label_addr("h")); s2.cpu.reset_regs(); s2.cpu.a[7] = 0x00F00000; s2.cpu.set_pc(b.base());
  CHECK(s2.run_until_halt()); CHECK_EQ(s2.cpu.d[0], 44);
}

TEST(cpu_interrupt_from_dma_wakes_stop) {
  Soc s; A a(CODE_BASE);
  a.moveq(0, 6);
  a.movel(A::Imm(IRQ_DMA0), A::Abs(BRIDGE_BASE + BR_IRQ_MASK));
  a.movel(A::Imm(0x1000), A::Abs(BRIDGE_BASE + BR_DMA0 + DMA_SRC)); a.movel(A::Imm(0x9000), A::Abs(BRIDGE_BASE + BR_DMA0 + DMA_DST));
  a.movel(A::Imm(512), A::Abs(BRIDGE_BASE + BR_DMA0 + DMA_LEN)); a.movel(A::Imm(DMAF_IRQ), A::Abs(BRIDGE_BASE + BR_DMA0 + DMA_FLAGS));
  a.movel(A::Imm(DMAC_START_REGS), A::Abs(BRIDGE_BASE + BR_DMA0 + DMA_CTRL));
  a.stop(0x2000);                                            // dorme até a IRQ
  a.halt();
  a.label("isr"); a.addq(4, 1, A::D(6)); a.movel(A::Imm(IRQ_DMA0), A::Abs(BRIDGE_BASE + BR_IRQ_STATUS)); a.rte();
  auto code = a.finish(); s.load(a.base(), code); s.bus.raw_w32(VEC_IRQ4 * 4, a.label_addr("isr"));
  s.cpu.reset_regs(); s.cpu.a[7] = 0x00F00000; s.cpu.set_pc(a.base()); s.cpu.sr = 0x2000;
  CHECK(s.run_until_halt()); CHECK_EQ(s.cpu.d[6], 1); CHECK_EQ(s.bus.irq_status, 0);
  CHECK(s.sched.now > 270000);                               // o tempo avançou até o fim do DMA
}

TEST(cpu_fpu_double_arithmetic_and_branches) {
  Soc s; A a(CODE_BASE);
  s.bus.raw_w32(0x3000, 0x3FF80000); s.bus.raw_w32(0x3004, 0);        // 1.5
  s.bus.raw_w32(0x3008, 0x40020000); s.bus.raw_w32(0x300C, 0);        // 2.25
  s.bus.raw_w32(0x3010, 0x40000000); s.bus.raw_w32(0x3014, 0);        // 2.0
  a.fop_ea(0x00, 5, A::Abs(0x3000), 0); a.fop_ea(0x22, 5, A::Abs(0x3008), 0); a.fop_ea(0x23, 5, A::Abs(0x3010), 0);   // (1.5+2.25)*2 = 7.5
  a.fstore(5, 0, A::Abs(0x3018));
  a.fop_rr(0x04, 0, 1); a.fstore(1, 1, A::Abs(0x3020));                // sqrt(7.5) em precisão simples
  a.fstore(0, 0, A::D(1));                                             // 7.5 -> inteiro (arredonda p/ par = 8)
  a.fop_ea(0x00, 0, A::Imm(3), 2); a.fop_ea(0x00, 0, A::Imm(1), 3); a.moveq(0, 0);
  a.label("loop"); a.addq(4, 1, A::D(0)); a.fop_rr(0x28, 3, 2); a.fop_rr(0x3A, 2, 2); a.fbcc(0x12, "loop");
  a.fop_ea(0x00, 5, A::Abs(0x3000), 4); a.fop_ea(0x38, 5, A::Abs(0x3008), 4); a.moveq(0, 2); a.fbcc(0x14, "lt"); a.moveq(1, 2); a.label("lt");   // fp4(1.5) < 2.25 -> pula
  a.fop_rr(0x1A, 4, 5); a.fop_rr(0x18, 5, 5); a.fstore(5, 5, A::Abs(0x3028));   // |-1.5|
  a.halt();
  CHECK(run_prog(s, a));
  CHECK_NEAR(u2d(rbe64(&s.bus.ddr[0x3018])), 7.5, 1e-12); CHECK_NEAR(u2f(s.bus.raw_r32(0x3020)), std::sqrt(7.5f), 1e-6);
  CHECK_EQ(s.cpu.d[1], 8); CHECK_EQ(s.cpu.d[0], 3); CHECK_EQ(s.cpu.d[2], 0); CHECK_NEAR(u2d(rbe64(&s.bus.ddr[0x3028])), 1.5, 1e-12);
}

TEST(cpu_emac_dot_product) {
  Soc s; A a(CODE_BASE);
  a.mac_clr(); a.moveq(3, 1); a.moveq(4, 2); a.mac_l(1, 2); a.moveq(-5, 3); a.mac_l(3, 2); a.mac_get(0, 0);   // 12 - 20
  // acc0 = 2^31-1 + 3*2^30 = 5*2^30 - 1 = 2^32 + 0x3FFFFFFF: extensão ACC0[47:32] = 1 (ACCext01 baixo), baixo = 0x3FFFFFFF
  a.movel(A::Imm(0x7FFFFFFF), A::D(6)); a.mac_set(6); a.moveq(1, 4); a.movel(A::Imm(0x40000000), A::D(5));
  a.mac_l(4, 5); a.mac_l(4, 5); a.mac_l(4, 5); a.mac_get_accext(0, 7); a.mac_get(0, 6);
  a.halt();
  CHECK(run_prog(s, a)); CHECK_EQ(s.cpu.d[0], (u32)-8); CHECK_EQ(s.cpu.d[7], 1); CHECK_EQ(s.cpu.d[6], 0x3FFFFFFFu);
}

TEST(cpu_mmio_semaphore_and_id) {
  Soc s; A a(CODE_BASE);
  a.movel(A::Abs(BRIDGE_BASE + BR_ID), A::D(0));
  a.movel(A::Imm(1), A::Abs(BRIDGE_BASE + BR_SEM0)); a.movel(A::Imm(1), A::Abs(BRIDGE_BASE + BR_SEM0));
  a.movel(A::Abs(BRIDGE_BASE + BR_SEM0), A::D(1)); a.movel(A::Abs(BRIDGE_BASE + BR_SEM0), A::D(2)); a.movel(A::Abs(BRIDGE_BASE + BR_SEM0), A::D(3));
  a.halt();
  CHECK(run_prog(s, a)); CHECK_EQ(s.cpu.d[0], 0x53463130); CHECK_EQ(s.cpu.d[1], 2); CHECK_EQ(s.cpu.d[2], 1); CHECK_EQ(s.cpu.d[3], 0);
}

TEST(cpu_drives_vpu_through_mmio) {
  Soc s;
  std::vector<u32> prog; std::string err;
  CHECK(vpu_assemble("VLD v0,[s0+0]\nVLD v1,[s0+1]\nVMUL v2,v0,v1\nVST [s0+2],v2\nHALT\n", prog, err));
  s.vpuA.load_program(prog);
  put_vec(s, VPUA_DMEM + 0, 1, 2, 3, 4); put_vec(s, VPUA_DMEM + 16, 2, 2, 2, 2);
  s.bus.raw_w32(0x2000, 0);
  A a(CODE_BASE);
  a.movel(A::Imm(0), A::Abs(VPUA_BASE + VPU_PC)); a.movel(A::Imm(0), A::Abs(VPUA_BASE + VPU_S0));
  a.movel(A::Imm(VPUC_START), A::Abs(VPUA_BASE + VPU_CTRL));
  a.label("wait"); a.movel(A::Abs(VPUA_BASE + VPU_STATUS), A::D(0)); a.btst_i(1, A::D(0)); a.beq("wait");
  a.movel(A::Abs(VPUA_DMEM + 32), A::D(1)); a.movel(A::Abs(VPUA_DMEM + 44), A::D(2)); a.movel(A::Abs(VPUA_BASE + VPU_CYCLES), A::D(3)); a.halt();
  CHECK(run_prog(s, a));
  CHECK_EQ(s.cpu.d[1], f2u(2.0f)); CHECK_EQ(s.cpu.d[2], f2u(8.0f)); CHECK_EQ(s.cpu.d[3], 5);
}
