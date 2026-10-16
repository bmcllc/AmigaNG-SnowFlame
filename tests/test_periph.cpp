#include "helpers.h"

static void tlb_ins(Soc& s, u32 va, u32 pa, u32 perm) {
  mmio_w(s, MMU_BASE + MMU_AR, va); mmio_w(s, MMU_BASE + MMU_TR, pa | perm); mmio_w(s, MMU_BASE + MMU_OP, 1);
}

TEST(mmu_translation_permissions_and_faults) {
  Soc s; tlb_ins(s, 0x00100000, 0x00200000, MP_R | MP_W); tlb_ins(s, 0x00101000, 0x00201000, MP_R); tlb_ins(s, 0x00102000, 0x00202000, MP_R | MP_W | MP_SUPER);
  mmio_w(s, MMU_BASE + MMU_CTRL, 1);
  s.bus.cpu_super = true;
  s.bus.cpu_write(0x00100010, 4, 0xCAFEBABE); s.bus.cache_flush_all(false);
  CHECK_EQ(s.bus.raw_r32(0x00200010), 0xCAFEBABEu); CHECK_EQ(s.bus.raw_r32(0x00100010), 0);          // VA != PA
  CHECK_EQ(s.bus.cpu_read(0x00100010, 4), 0xCAFEBABEu); CHECK(!s.bus.fault);
  s.bus.cpu_read(0x00300000, 4);                                                                       // miss
  CHECK(s.bus.fault); CHECK_EQ(mmio_r(s, MMU_BASE + MMU_FAR), 0x00300000); CHECK_EQ(mmio_r(s, MMU_BASE + MMU_FSR) & 15, 1); s.bus.fault = false;
  s.bus.cpu_write(0x00101000, 4, 1);                                                                   // página somente leitura
  CHECK(s.bus.fault); CHECK_EQ(mmio_r(s, MMU_BASE + MMU_FSR) & 15, 2); CHECK_EQ((mmio_r(s, MMU_BASE + MMU_FSR) >> 4) & 7, MP_W); s.bus.fault = false;
  s.bus.cpu_super = false; s.bus.cpu_read(0x00102000, 4); CHECK(s.bus.fault); s.bus.fault = false;      // só supervisor
  s.bus.cpu_super = true; s.bus.cpu_read(0x00102000, 4); CHECK(!s.bus.fault);
  CHECK_EQ(mmio_r(s, MMU_BASE + MMU_CTRL), 1);                                                         // MMIO nunca é traduzido
  mmio_w(s, MMU_BASE + MMU_AR, 0x00100123); mmio_w(s, MMU_BASE + MMU_OP, 4); CHECK_EQ(mmio_r(s, MMU_BASE + MMU_TR), 0x80200000u | MP_R | MP_W);
  mmio_w(s, MMU_BASE + MMU_OP, 3); mmio_w(s, MMU_BASE + MMU_OP, 4); CHECK_EQ(mmio_r(s, MMU_BASE + MMU_TR), 0);
  mmio_w(s, MMU_BASE + MMU_OP, 2); s.bus.cpu_read(0x00101000, 4); CHECK(s.bus.fault); s.bus.fault = false;   // invalida tudo
}

TEST(mmu_cpu_access_error_exception_vector2) {
  Soc s; A a(CODE_BASE);
  a.moveq(0, 0);
  a.movel(A::Abs(0x00500000), A::D(1));                       // página não mapeada -> erro de acesso
  a.moveq(1, 0); a.halt();
  a.label("h"); a.moveq(77, 7); a.halt();
  auto code = a.finish(); s.load(a.base(), code); s.bus.raw_w32(2 * 4, a.label_addr("h"));
  tlb_ins(s, 0x1000, 0x1000, MP_R | MP_W | MP_X | MP_SUPER); mmio_w(s, MMU_BASE + MMU_CTRL, 1);
  s.cpu.reset_regs(); s.cpu.a[7] = 0x00F00000; s.cpu.set_pc(a.base()); CHECK(s.run_until_halt());
  CHECK_EQ(s.cpu.d[7], 77); CHECK_EQ(s.cpu.d[0], 0); CHECK_EQ(mmio_r(s, MMU_BASE + MMU_FAR), 0x00500000);
  CHECK(s.bus.tlb_misses >= 1);
}

TEST(icache_timing_loop_hits_after_first_pass) {
  Soc s; A a(CODE_BASE);
  a.moveq(0, 0); a.label("l"); a.addq(4, 1, A::D(0)); a.cmpi(4, 200, A::D(0)); a.bne("l"); a.halt();
  CHECK(run_prog(s, a));
  CHECK(s.bus.imisses >= 1 && s.bus.imisses <= 4); CHECK(s.bus.ihits > 100);
  Soc t; A b(CODE_BASE); b.moveq(0, 0); b.label("l"); b.addq(4, 1, A::D(0)); b.cmpi(4, 200, A::D(0)); b.bne("l"); b.halt();
  mmio_w(t, BRIDGE_BASE + BR_CACHE_OP, ICACHE_INVAL_ALL); CHECK(run_prog(t, b));
}

TEST(timers_oneshot_and_periodic_irq) {
  Soc s; mmio_w(s, BRIDGE_BASE + BR_IRQ_MASK, IRQ_TIMER);
  mmio_w(s, TIMER_BASE + 0x00, 1000); mmio_w(s, TIMER_BASE + 0x04, 1 | 4);            // timer 0: one-shot 1 ms
  mmio_w(s, TIMER_BASE + 0x10, 250); mmio_w(s, TIMER_BASE + 0x14, 1 | 2 | 4);         // timer 1: periódico 250 us
  advance_ns(s, 600000); CHECK_EQ(s.timers.fired[0], 0); CHECK_EQ(s.timers.fired[1], 2);
  CHECK_EQ(mmio_r(s, TIMER_BASE + 0x08), 400); CHECK_EQ(mmio_r(s, TIMER_BASE + 0x18), 150);
  advance_ns(s, 600000); CHECK_EQ(s.timers.fired[0], 1); CHECK_EQ(s.timers.fired[1], 4); CHECK_EQ(mmio_r(s, TIMER_BASE + 0x04) & 1, 0);   // one-shot desarmou
  CHECK(mmio_r(s, BRIDGE_BASE + BR_IRQ_STATUS) & IRQ_TIMER);
  mmio_w(s, TIMER_BASE + 0x14, 0); advance_ns(s, 1000000); CHECK_EQ(s.timers.fired[1], 4);
}

TEST(dvd_read_latency_data_and_errors) {
  Soc s; CHECK_EQ(mmio_r(s, DVD_BASE + DVD_STATUS) & 8, 0);
  std::vector<u8> img(64 * DVD_SECTOR); for (size_t i = 0; i < img.size(); i++) img[i] = (u8)(i * 7 + (i >> 11));
  s.dvd.insert(img); CHECK_EQ(mmio_r(s, DVD_BASE + DVD_STATUS) & 8, 8); CHECK_EQ(mmio_r(s, DVD_BASE + DVD_CAPACITY), 64);
  mmio_w(s, DVD_BASE + DVD_CTRL, 1); mmio_w(s, BRIDGE_BASE + BR_IRQ_MASK, IRQ_DVD);
  auto rd = [&](u32 lba, u32 n, u32 dst) { mmio_w(s, DVD_BASE + DVD_LBA, lba); mmio_w(s, DVD_BASE + DVD_COUNT, n); mmio_w(s, DVD_BASE + DVD_DST, dst); mmio_w(s, DVD_BASE + DVD_CMD, 1);
    Ps t0 = s.sched.now; s.wait_until([&] { return !(mmio_r(s, DVD_BASE + DVD_STATUS) & 1); }); return s.sched.now - t0; };
  Ps first = rd(0, 4, 0x400000);
  CHECK(first > 800000000000ull);                                              // spin-up
  CHECK_EQ(mmio_r(s, DVD_BASE + DVD_STATUS) & 2, 2); CHECK(mmio_r(s, BRIDGE_BASE + BR_IRQ_STATUS) & IRQ_DVD);
  CHECK(std::memcmp(s.bus.raw(0x400000, 4 * DVD_SECTOR), img.data(), 4 * DVD_SECTOR) == 0);
  Ps xfer = (Ps)(4.0 * DVD_SECTOR / (Dvd::RATE_MBPS * 1e6) * 1e12); CHECK(first >= 800000000000ull + xfer);
  mmio_w(s, DVD_BASE + DVD_STATUS, 2);
  Ps second = rd(4, 4, 0x410000);                                              // sequencial: sem spin-up nem seek
  CHECK(second < 5000000000ull); CHECK(second >= xfer);
  CHECK(std::memcmp(s.bus.raw(0x410000, 4 * DVD_SECTOR), &img[4 * DVD_SECTOR], 4 * DVD_SECTOR) == 0);
  mmio_w(s, DVD_BASE + DVD_STATUS, 2);
  mmio_w(s, DVD_BASE + DVD_LBA, 62); mmio_w(s, DVD_BASE + DVD_COUNT, 8); mmio_w(s, DVD_BASE + DVD_CMD, 1); CHECK_EQ(mmio_r(s, DVD_BASE + DVD_STATUS) & 4, 4);   // além do fim
  mmio_w(s, DVD_BASE + DVD_STATUS, 4); CHECK_EQ(mmio_r(s, DVD_BASE + DVD_STATUS) & 4, 0);
}

TEST(memcard_flash_semantics_erase_program_read) {
  Soc s; auto cmd = [&](u32 c, u32 sec, u32 n, u32 addr) { mmio_w(s, MCARD_BASE + MC_SECTOR, sec); mmio_w(s, MCARD_BASE + MC_COUNT, n); mmio_w(s, MCARD_BASE + MC_ADDR, addr); mmio_w(s, MCARD_BASE + MC_CMD, c);
    Ps t0 = s.sched.now; s.wait_until([&] { return !(mmio_r(s, MCARD_BASE + MC_STATUS) & 1); }); return s.sched.now - t0; };
  CHECK_EQ(mmio_r(s, MCARD_BASE + MC_CAPACITY), (1u << 20) / 512);
  for (int i = 0; i < 512; i++) s.bus.raw_w8(0x1000 + i, 0xF0); for (int i = 0; i < 512; i++) s.bus.raw_w8(0x2000 + i, 0x3C);
  Ps tp = cmd(2, 8, 1, 0x1000);                                                // programa em flash apagada (0xFF)
  CHECK(std::all_of(s.mcard.data.begin() + 8 * 512, s.mcard.data.begin() + 9 * 512, [](u8 b) { return b == 0xF0; }));
  cmd(2, 8, 1, 0x2000); CHECK_EQ(s.mcard.data[8 * 512], 0xF0 & 0x3C);          // reprogramar só limpa bits (AND)
  Ps te = cmd(3, 8, 0, 0); CHECK_EQ(s.mcard.data[8 * 512], 0xFF); CHECK(te > tp);
  cmd(2, 9, 1, 0x1000); cmd(1, 9, 1, 0x3000);
  for (int i = 0; i < 512; i++) CHECK_EQ(s.bus.raw_r8(0x3000 + i), 0xF0);
  mmio_w(s, MCARD_BASE + MC_SECTOR, 3); mmio_w(s, MCARD_BASE + MC_CMD, 3); CHECK_EQ(mmio_r(s, MCARD_BASE + MC_STATUS) & 4, 4);   // apagar desalinhado
  mmio_w(s, MCARD_BASE + MC_STATUS, 4); s.mcard.present = false; CHECK_EQ(mmio_r(s, MCARD_BASE + MC_STATUS) & 8, 0);
}

TEST(pads_latch_at_vblank) {
  Soc s; s.pads.pad[1].connected = true; s.pads.pad[1].buttons = BTN_A | BTN_START; s.pads.pad[1].lx = 200; s.pads.pad[1].rt = 99;
  CHECK_EQ(mmio_r(s, PAD_BASE + PAD_PORT0 + 0x10), 0);                          // ainda não latched
  s.video.vblank();
  CHECK_EQ(mmio_r(s, PAD_BASE + PAD_PORT0 + 0x10), BTN_A | BTN_START); CHECK_EQ(mmio_r(s, PAD_BASE + PAD_PORT0 + 0x14) >> 24, 200);
  CHECK_EQ(mmio_r(s, PAD_BASE + PAD_PORT0 + 0x18) & 255, 99); CHECK_EQ(mmio_r(s, PAD_BASE + PAD_PORT0 + 0x1C), 1); CHECK_EQ(mmio_r(s, PAD_BASE + PAD_PORT0 + 0x00), 0);
  s.pads.pad[1].buttons = 0; CHECK_EQ(mmio_r(s, PAD_BASE + PAD_PORT0 + 0x10), BTN_A | BTN_START); s.video.vblank(); CHECK_EQ(mmio_r(s, PAD_BASE + PAD_PORT0 + 0x10), 0);
  mmio_w(s, PAD_BASE + PAD_PORT0 + 0x28, 5); CHECK_EQ(s.pads.pad[2].rumble, 5);
}

TEST(video_indexed_plane_copper_palette_swap) {
  Soc s; u32 L = 0x80000, FB = 0x100000;
  for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) s.bus.raw_w8(FB + y * 320 + x, x < 160 ? 1 : 2);
  mmio_w(s, VIDEO_BASE + VID_PALETTE + 4, 0xFFFF0000); mmio_w(s, VIDEO_BASE + VID_PALETTE + 8, 0xFF00FF00);
  auto put = [&](u32 i, u32 a, u32 b) { s.bus.raw_w32(L + i * 8, a); s.bus.raw_w32(L + i * 8 + 4, b); };
  put(0, cop_wait(100), 0); put(1, cop_move(VIDEO_BASE + VID_PALETTE + 4), 0xFF0000FF); put(2, 0, 0);
  mmio_w(s, COP_BASE + COP_LIST, L); mmio_w(s, COP_BASE + COP_CTRL, 1);
  mmio_w(s, VIDEO_BASE + VID_P0_ADDR, FB); mmio_w(s, VIDEO_BASE + VID_P0_PITCH, 320); mmio_w(s, VIDEO_BASE + VID_CTRL, VCTRL_P0 | VCTRL_P0_IDX);
  s.video.vblank();
  CHECK_EQ(s.video.frame[50 * 320 + 10], 0xFF0000u); CHECK_EQ(s.video.frame[150 * 320 + 10], 0x0000FFu);      // paleta trocada no meio do quadro
  CHECK_EQ(s.video.frame[50 * 320 + 200], 0x00FF00u); CHECK_EQ(s.video.frame[150 * 320 + 200], 0x00FF00u);
  CHECK_EQ(mmio_r(s, VIDEO_BASE + VID_PALETTE + 4), 0xFF0000FFu);
}

TEST(paula_dsp_budget_drops_voices_when_overloaded) {
  Soc s; for (int v = 0; v < 64; v++) { u32 b = PAULA_BASE + PA_VOICE0 + v * 0x40;
    mmio_w(s, b + PV_ADDR, 0); mmio_w(s, b + PV_LEN, 4000); mmio_w(s, b + PV_CTRL, 1 | 2 | (2 << 2) | 0x10 | 0x20); }
  mmio_w(s, PAULA_BASE + PA_REVERB_WET, 128); s.paula.render(64);
  CHECK(mmio_r(s, PAULA_BASE + PA_DSP_DROPS) > 0); CHECK(mmio_r(s, PAULA_BASE + PA_DSP_LOAD) >= 950); CHECK(mmio_r(s, PAULA_BASE + PA_DSP_LOAD) <= 1000);
  mmio_w(s, PAULA_BASE + PA_DSP_DROPS, 0); CHECK_EQ(mmio_r(s, PAULA_BASE + PA_DSP_DROPS), 0);
  for (int v = 0; v < 64; v++) mmio_w(s, PAULA_BASE + PA_VOICE0 + v * 0x40 + PV_CTRL, v < 16 ? (1 | 2 | (1 << 2)) : 0);
  s.paula.render(64); CHECK_EQ(mmio_r(s, PAULA_BASE + PA_DSP_DROPS), 0); CHECK(mmio_r(s, PAULA_BASE + PA_DSP_LOAD) < 600);
}

TEST(blitter_contends_with_dma_on_ddr) {
  auto run = [](bool arb) {
    Soc s; s.bus.arb_enabled = arb; Ps t0 = s.sched.now;
    u32 d = BRIDGE_BASE + BR_DMA0; mmio_w(s, d + DMA_SRC, 0x200000); mmio_w(s, d + DMA_DST, 0x600000); mmio_w(s, d + DMA_LEN, 4 * 1024 * 1024); mmio_w(s, d + DMA_FLAGS, 0); mmio_w(s, d + DMA_CTRL, DMAC_START_REGS);
    mmio_w(s, BLT_BASE + BLT_OP, BOP_FILL); mmio_w(s, BLT_BASE + BLT_DST, 0x800000); mmio_w(s, BLT_BASE + BLT_DST_PITCH, 1024); mmio_w(s, BLT_BASE + BLT_W, 256); mmio_w(s, BLT_BASE + BLT_H, 256);
    mmio_w(s, BLT_BASE + BLT_COLOR, 0xFF00FF00); mmio_w(s, BLT_BASE + BLT_CTRL, 1);
    s.wait_until([&] { return s.blt.jobs >= 1; }); return s.sched.now - t0; };   // tempo até o blit terminar
  Ps shared = run(true), free_ = run(false);
  CHECK(shared > free_ * 2);   // o blit espera a DMA de 4 MB liberar a DDR
}

// Bytes que cada operação do Blitter disputa no árbitro (leitura da origem + escrita do destino; RMW nas operações de mistura).
static u64 blit_ddr_bytes(u32 op, u32 w, u32 h) {
  Soc s; mmio_w(s, BLT_BASE + BLT_OP, op); mmio_w(s, BLT_BASE + BLT_SRC, 0x100000); mmio_w(s, BLT_BASE + BLT_SRC_PITCH, w * 4);
  mmio_w(s, BLT_BASE + BLT_DST, 0x200000); mmio_w(s, BLT_BASE + BLT_DST_PITCH, w * 4); mmio_w(s, BLT_BASE + BLT_MASK, 0x300000); mmio_w(s, BLT_BASE + BLT_MASK_PITCH, w * 4);
  mmio_w(s, BLT_BASE + BLT_MINTERM, 0xCA); mmio_w(s, BLT_BASE + BLT_W, w); mmio_w(s, BLT_BASE + BLT_H, h); mmio_w(s, BLT_BASE + BLT_CTRL, 1);
  s.wait_until([&] { return s.blt.jobs >= 1; }); return s.bus.arb_bytes[Bus::R_DDR];
}
TEST(blitter_bus_traffic_per_operation) {
  const u64 px = 64 * 32;
  CHECK_EQ(blit_ddr_bytes(BOP_FILL, 64, 32), px * 4);          // só escreve
  CHECK_EQ(blit_ddr_bytes(BOP_COPY, 64, 32), px * 8);          // lê + escreve
  CHECK_EQ(blit_ddr_bytes(BOP_ALPHA, 64, 32), px * 12);        // lê origem, lê e escreve destino
  CHECK_EQ(blit_ddr_bytes(BOP_COLORKEY, 64, 32), px * 12);
  CHECK_EQ(blit_ddr_bytes(BOP_MINTERM, 64, 32), px * 16);      // + máscara
  CHECK_EQ(blit_ddr_bytes(BOP_CONV565, 64, 32), px * 6);       // origem 16 bpp, destino 32 bpp
}

TEST(hid_keyboard_fifo_modifiers_and_mouse) {
  Soc s; CHECK_EQ(mmio_r(s, HID_BASE + HID_STATUS) & 3, 3); CHECK_EQ(mmio_r(s, HID_BASE + HID_KEY_DATA), 0xFFFFFFFFu);
  mmio_w(s, HID_BASE + HID_CTRL, 1); mmio_w(s, BRIDGE_BASE + BR_IRQ_MASK, IRQ_HID);
  s.hid.key_event(0xE1, true); s.hid.key_event(0x04, true); s.hid.key_event(0x04, false);
  CHECK(mmio_r(s, BRIDGE_BASE + BR_IRQ_STATUS) & IRQ_HID); CHECK_EQ(mmio_r(s, HID_BASE + HID_KEY_COUNT), 3);
  CHECK_EQ(mmio_r(s, HID_BASE + HID_KEY_DATA), 0xE1u | (HIDM_SHIFT << 16));
  CHECK_EQ(mmio_r(s, HID_BASE + HID_KEY_DATA), 0x04u | (HIDM_SHIFT << 16));
  CHECK_EQ(mmio_r(s, HID_BASE + HID_KEY_DATA), 0x04u | HID_KEY_UP | (HIDM_SHIFT << 16));
  for (int i = 0; i < 70; i++) s.hid.key_event(0x05, true);
  CHECK_EQ(mmio_r(s, HID_BASE + HID_KEY_COUNT), (u32)HID_FIFO_DEPTH); CHECK_EQ(mmio_r(s, HID_BASE + HID_STATUS) & 4, 4);
  mmio_w(s, HID_BASE + HID_STATUS, 4); CHECK_EQ(mmio_r(s, HID_BASE + HID_STATUS) & 4, 0);
  s.hid.mouse_move(5, -3); s.hid.mouse_move(2, 1); s.hid.mouse_button(1, true); s.hid.mouse_wheel(-1);
  CHECK_EQ((int)mmio_r(s, HID_BASE + HID_MOUSE_DX), 7); CHECK_EQ((int)mmio_r(s, HID_BASE + HID_MOUSE_DY), -2);
  CHECK_EQ(mmio_r(s, HID_BASE + HID_MOUSE_DX), 0u); CHECK_EQ(mmio_r(s, HID_BASE + HID_MOUSE_BTN), 2u); CHECK_EQ((int)mmio_r(s, HID_BASE + HID_MOUSE_WHEEL), -1);
}
