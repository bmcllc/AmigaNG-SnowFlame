#include "helpers.h"

constexpr u32 SURF_A = 0x100000, SURF_B = 0x180000, SURF_M = 0x1C0000;
static void blt(Soc& s, u32 op, u32 src, u32 sp, u32 dst, u32 dp, u32 w, u32 h, u32 color = 0, u32 mask = 0, u32 mp = 0, u32 minterm = 0, u32 ctrl = 1) {
  mmio_w(s, BLT_BASE + BLT_OP, op); mmio_w(s, BLT_BASE + BLT_SRC, src); mmio_w(s, BLT_BASE + BLT_SRC_PITCH, sp); mmio_w(s, BLT_BASE + BLT_DST, dst);
  mmio_w(s, BLT_BASE + BLT_DST_PITCH, dp); mmio_w(s, BLT_BASE + BLT_W, w); mmio_w(s, BLT_BASE + BLT_H, h); mmio_w(s, BLT_BASE + BLT_COLOR, color);
  mmio_w(s, BLT_BASE + BLT_MASK, mask); mmio_w(s, BLT_BASE + BLT_MASK_PITCH, mp); mmio_w(s, BLT_BASE + BLT_MINTERM, minterm); mmio_w(s, BLT_BASE + BLT_CTRL, ctrl);
}
static void settle(Soc& s) { s.wait_until([&] { return !s.blt.busy(); }); }

TEST(blitter_fill_copy_and_overlapping_scroll) {
  Soc s;
  blt(s, BOP_FILL, 0, 0, SURF_A, 128, 16, 8, 0xFF112233); settle(s);
  for (int y = 0; y < 8; y++) for (int x = 0; x < 16; x++) CHECK_EQ(s.bus.raw_r32(SURF_A + y * 128 + x * 4), 0xFF112233);
  CHECK_EQ(s.bus.raw_r32(SURF_A + 16 * 4), 0);            // fora do retângulo
  CHECK_EQ(s.bus.raw_r32(SURF_A + 8 * 128), 0);
  for (int x = 0; x < 16; x++) s.bus.raw_w32(SURF_A + x * 4, 0xFF000000 | x);
  blt(s, BOP_COPY, SURF_A, 128, SURF_B, 256, 16, 2); settle(s);
  for (int x = 0; x < 16; x++) CHECK_EQ(s.bus.raw_r32(SURF_B + x * 4), 0xFF000000 | (u32)x);
  CHECK_EQ(s.bus.raw_r32(SURF_B + 256 + 5 * 4), 0xFF112233);
  // rolagem: copia a linha 0 deslocada 4 pixels à direita sobre si mesma (origem e destino se sobrepõem)
  blt(s, BOP_COPY, SURF_A, 128, SURF_A + 16, 128, 12, 1); settle(s);
  for (int x = 0; x < 12; x++) CHECK_EQ(s.bus.raw_r32(SURF_A + (x + 4) * 4), 0xFF000000 | (u32)x);
}

TEST(blitter_colorkey_alpha_minterm_and_565) {
  Soc s;
  for (int i = 0; i < 8; i++) { s.bus.raw_w32(SURF_A + i * 4, i % 2 ? 0xFFFF00FF : 0xFF00FF00); s.bus.raw_w32(SURF_B + i * 4, 0xFF0000AA); }
  blt(s, BOP_COLORKEY, SURF_A, 64, SURF_B, 64, 8, 1, 0xFFFF00FF); settle(s);          // magenta é transparente
  for (int i = 0; i < 8; i++) CHECK_EQ(s.bus.raw_r32(SURF_B + i * 4), i % 2 ? 0xFF0000AAu : 0xFF00FF00u);
  s.bus.raw_w32(SURF_A, 0x80FF0000); s.bus.raw_w32(SURF_A + 4, 0xFFFF0000); s.bus.raw_w32(SURF_A + 8, 0x00FF0000);
  for (int i = 0; i < 3; i++) s.bus.raw_w32(SURF_B + i * 4, 0xFF0000FF);
  blt(s, BOP_ALPHA, SURF_A, 64, SURF_B, 64, 3, 1); settle(s);
  u32 p0 = s.bus.raw_r32(SURF_B); CHECK(std::abs((int)((p0 >> 16) & 255) - 128) <= 1); CHECK(std::abs((int)(p0 & 255) - 127) <= 1); CHECK_EQ(p0 >> 24, 255);
  CHECK_EQ(s.bus.raw_r32(SURF_B + 4), 0xFFFF0000); CHECK_EQ(s.bus.raw_r32(SURF_B + 8), 0xFF0000FF);          // opaco substitui, transparente preserva
  // minterm 0xCA (cookie-cut: D = A ? B : C) e 0x66 (B xor C)
  s.bus.raw_w32(SURF_M, 0xFFFF0000); s.bus.raw_w32(SURF_M + 4, 0x0000FFFF);      // máscara A por bits
  s.bus.raw_w32(SURF_A, 0x12345678); s.bus.raw_w32(SURF_A + 4, 0x12345678);     // B (origem)
  s.bus.raw_w32(SURF_B, 0xAAAAAAAA); s.bus.raw_w32(SURF_B + 4, 0xAAAAAAAA);     // C = D (destino)
  blt(s, BOP_MINTERM, SURF_A, 64, SURF_B, 64, 2, 1, 0, SURF_M, 64, 0xCA); settle(s);
  CHECK_EQ(s.bus.raw_r32(SURF_B), 0x1234AAAA); CHECK_EQ(s.bus.raw_r32(SURF_B + 4), 0xAAAA5678);
  s.bus.raw_w32(SURF_B, 0xFFFF0000); s.bus.raw_w32(SURF_A, 0x0FF00FF0);
  blt(s, BOP_MINTERM, SURF_A, 64, SURF_B, 64, 1, 1, 0, SURF_M, 64, 0x66); settle(s);
  CHECK_EQ(s.bus.raw_r32(SURF_B), 0x0FF00FF0u ^ 0xFFFF0000u);
  // conversão RGB565 -> ARGB8888
  s.bus.raw_w16(SURF_A, 0xF800); s.bus.raw_w16(SURF_A + 2, 0x07E0); s.bus.raw_w16(SURF_A + 4, 0x001F);
  blt(s, BOP_CONV565, SURF_A, 64, SURF_B, 64, 3, 1); settle(s);
  CHECK_EQ(s.bus.raw_r32(SURF_B), 0xFFFF0000); CHECK_EQ(s.bus.raw_r32(SURF_B + 4), 0xFF00FF00); CHECK_EQ(s.bus.raw_r32(SURF_B + 8), 0xFF0000FF);
}

TEST(blitter_timing_irq_queue_and_errors) {
  Soc s; s.bus.irq_mask = IRQ_BLT;
  blt(s, BOP_COPY, SURF_A, 256, SURF_B, 256, 64, 16, 0, 0, 0, 0, 3);        // 64*16*2 ciclos @100 MHz = 20,48 us
  CHECK_EQ(mmio_r(s, BLT_BASE + BLT_STATUS) & 1, 1); Ps t0 = s.sched.now;
  CHECK(s.wait_until([&] { return s.bus.irq_pending(); })); CHECK_EQ(s.sched.now - t0, cyc2ps(64 * 16 * 2, CHIP_HZ));
  blt(s, BOP_FILL, 0, 0, 0xF0000000, 64, 4, 4, 1); CHECK_EQ(mmio_r(s, BLT_BASE + BLT_STATUS) & 4, 4);     // destino MMIO inválido
  mmio_w(s, BLT_BASE + BLT_STATUS, 4); CHECK_EQ(mmio_r(s, BLT_BASE + BLT_STATUS) & 4, 0);
  for (int i = 0; i < 3; i++) blt(s, BOP_FILL, 0, 0, SURF_A, 64, 4, 4, 0xFF000000 + i);
  settle(s); CHECK_EQ(s.bus.raw_r32(SURF_A), 0xFF000002u);                   // jobs executam em ordem
}

TEST(copper_changes_background_per_scanline) {
  Soc s; u32 L = 0x80000;
  auto put = [&](u32 i, u32 a, u32 b) { s.bus.raw_w32(L + i * 8, a); s.bus.raw_w32(L + i * 8 + 4, b); };
  put(0, cop_move(VIDEO_BASE + VID_BG), 0xFFFF0000); put(1, cop_wait(100), 0); put(2, cop_move(VIDEO_BASE + VID_BG), 0xFF0000FF);
  put(3, cop_move(COP_BASE + COP_CTRL), 0);          // Copper tentando se desligar: ignorado
  put(4, cop_wait(200), 0); put(5, cop_move(VIDEO_BASE + VID_BG), 0xFF00FF00); put(6, 0, 0);
  mmio_w(s, COP_BASE + COP_LIST, L); mmio_w(s, COP_BASE + COP_CTRL, 1);
  s.video.vblank();
  CHECK_EQ(s.video.width(), 320); CHECK_EQ(s.video.height(), 240);
  CHECK_EQ(s.video.frame[0], 0xFF0000u); CHECK_EQ(s.video.frame[99 * 320 + 7], 0xFF0000u);
  CHECK_EQ(s.video.frame[100 * 320], 0x0000FFu); CHECK_EQ(s.video.frame[199 * 320 + 319], 0x0000FFu);
  CHECK_EQ(s.video.frame[200 * 320], 0x00FF00u); CHECK_EQ(s.video.frame[239 * 320], 0x00FF00u);
  CHECK_EQ(mmio_r(s, COP_BASE + COP_CTRL), 1);
  // JUMP e quadro seguinte reinicia a lista (BG volta a ser vermelho na linha 0)
  s.video.vblank(); CHECK_EQ(s.video.frame[0], 0xFF0000u);
  // desabilitado: BG permanece o último valor
  mmio_w(s, COP_BASE + COP_CTRL, 0); s.video.vblank(); CHECK_EQ(s.video.frame[0], 0x00FF00u);
}

TEST(video_compose_two_planes_with_alpha_and_modes) {
  Soc s; mmio_w(s, VIDEO_BASE + VID_BG, 0xFF101010);
  for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) { s.bus.raw_w32(VRAM_BASE + y * 1280 + x * 4, x < 100 ? 0x00000000 : 0xFFFF0000); s.bus.raw_w32(SURF_A + y * 1280 + x * 4, x >= 200 ? 0x800000FF : 0); }
  mmio_w(s, VIDEO_BASE + VID_P0_ADDR, VRAM_BASE); mmio_w(s, VIDEO_BASE + VID_P0_PITCH, 1280); mmio_w(s, VIDEO_BASE + VID_P1_ADDR, SURF_A); mmio_w(s, VIDEO_BASE + VID_P1_PITCH, 1280);
  mmio_w(s, VIDEO_BASE + VID_CTRL, 3); s.video.vblank();
  CHECK_EQ(s.video.frame[10 * 320 + 50], 0x101010u); CHECK_EQ(s.video.frame[10 * 320 + 150], 0xFF0000u);
  u32 mix = s.video.frame[10 * 320 + 250]; CHECK(std::abs((int)(mix >> 16) - 127) <= 1); CHECK(std::abs((int)(mix & 255) - 128) <= 1);
  mmio_w(s, VIDEO_BASE + VID_MODE, VMODE_480P); CHECK_EQ(s.video.width(), 640); CHECK_EQ(s.video.height(), 480);
  mmio_w(s, VIDEO_BASE + VID_MODE, VMODE_720P); CHECK_EQ(s.video.width(), 1280); CHECK_EQ(s.video.height(), 720);
  mmio_w(s, VIDEO_BASE + VID_MODE, VMODE_240P); CHECK_EQ(s.video.width(), 320);
}

TEST(video_vblank_cadence_irq_and_interlace) {
  Soc s; s.bus.irq_mask = IRQ_VBLANK; mmio_w(s, VIDEO_BASE + VID_CTRL, 4); int cb = 0; s.video.on_frame = [&](Video&) { cb++; };
  s.video.start(); s.sched.advance_to(FRAME_PS * 5 + 1000);
  CHECK_EQ(s.video.frames, 5); CHECK_EQ(cb, 5); CHECK(s.bus.irq_pending()); CHECK_EQ(mmio_r(s, VIDEO_BASE + VID_FRAME), 5);
  Soc t; mmio_w(t, VIDEO_BASE + VID_MODE, VMODE_480I); mmio_w(t, VIDEO_BASE + VID_BG, 0xFF0000FF);
  t.video.vblank();   // campo 0: linhas pares
  CHECK_EQ(t.video.frame[0], 0x0000FFu); CHECK_EQ(t.video.frame[640], 0u);
  mmio_w(t, VIDEO_BASE + VID_BG, 0xFF00FF00); t.video.vblank();   // campo 1: linhas ímpares; pares mantêm o campo anterior
  CHECK_EQ(t.video.frame[640], 0x00FF00u); CHECK_EQ(t.video.frame[0], 0x0000FFu);
}

// ---- motor de sprites (lista de 16 bytes por sprite, mistura alfa, recorte, espelhamento) ----
static void put_sprite(Soc& s, u32 list, int i, u32 src, u32 pitch, u32 flags, int x, int y, u32 w, u32 h) {
  u32 a = list + i * SPRITE_ENTRY_SIZE; s.bus.raw_w32(a, src); s.bus.raw_w16(a + 4, (u16)pitch); s.bus.raw_w16(a + 6, (u16)flags);
  s.bus.raw_w16(a + 8, (u16)(i16)x); s.bus.raw_w16(a + 10, (u16)(i16)y); s.bus.raw_w16(a + 12, (u16)w); s.bus.raw_w16(a + 14, (u16)h);
}
TEST(blitter_sprites_blend_clip_flip_and_order) {
  Soc s; const u32 LIST = 0x1F0000, SPR0 = 0x1E0000, SPR1 = 0x1E1000;
  for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) s.bus.raw_w32(SPR0 + y * 16 + x * 4, 0xFF000000 | (x << 4) | (y << 12));     // opaco, gradiente
  for (int i = 0; i < 16; i++) s.bus.raw_w32(SPR1 + i * 4, i == 0 ? 0x00000000 : 0x80FF0000);                                          // 1 pixel transparente, resto 50% vermelho
  blt(s, BOP_FILL, 0, 0, SURF_A, 64, 16, 16, 0xFF0000FF); settle(s);                                                                  // fundo azul 16x16
  put_sprite(s, LIST, 0, SPR0, 16, 0, 2, 2, 4, 4);                      // opaco normal
  put_sprite(s, LIST, 1, SPR0, 16, SPF_HFLIP, 8, 2, 4, 4);              // espelhado
  put_sprite(s, LIST, 2, SPR0, 16, SPF_VFLIP, -2, -2, 4, 4);            // recortado no canto superior esquerdo
  put_sprite(s, LIST, 3, SPR1, 16, 0, 10, 10, 4, 4);                    // translúcido, borda inferior direita (recorta em 16)
  blt(s, BOP_SPRITES, LIST, 0, SURF_A, 64, 16, 16, 4); settle(s);
  CHECK(!s.blt.error); CHECK_EQ(s.blt.sprites_drawn, 4);
  CHECK_EQ(s.bus.raw_r32(SURF_A + 2 * 64 + 2 * 4), 0xFF000000u);                   // sprite 0 (0,0) -> gradiente 0
  CHECK_EQ(s.bus.raw_r32(SURF_A + 3 * 64 + 5 * 4), 0xFF000000u | (3 << 4) | (1 << 12));
  CHECK_EQ(s.bus.raw_r32(SURF_A + 2 * 64 + 8 * 4), 0xFF000000u | (3 << 4));        // HFLIP: x=0 mostra a coluna 3
  CHECK_EQ(s.bus.raw_r32(SURF_A), 0xFF000000u | (2 << 4) | (1 << 12));             // VFLIP em (-2,-2): tela (0,0) -> sprite (2, 3-2=1)
  CHECK_EQ(s.bus.raw_r32(SURF_A + 10 * 64 + 10 * 4), 0xFF0000FFu);                 // pixel transparente: fundo intacto
  u32 mix = s.bus.raw_r32(SURF_A + 11 * 64 + 11 * 4); CHECK((mix >> 16 & 255) > 120 && (mix >> 16 & 255) < 136); CHECK((mix & 255) > 120 && (mix & 255) < 136);   // ~50% vermelho + azul
  CHECK_EQ(s.bus.raw_r32(SURF_A + 15 * 64 + 15 * 4) >> 24, 0xFF);                  // dentro do recorte; nada escrito fora de 16x16
  CHECK_EQ(s.bus.raw_r32(SURF_A + 16 * 64), 0);
}
TEST(blitter_sprites_opaque_flag_errors_and_bus_traffic) {
  Soc s; const u32 LIST = 0x1F0000, SPR = 0x1E0000;
  for (int i = 0; i < 4; i++) s.bus.raw_w32(SPR + i * 4, 0x00123456);          // alfa 0
  blt(s, BOP_FILL, 0, 0, SURF_A, 16, 4, 1, 0xFF777777); settle(s);
  put_sprite(s, LIST, 0, SPR, 4, SPF_OPAQUE, 0, 0, 1, 1);                      // copia sem mistura -> escreve 0x00123456
  blt(s, BOP_SPRITES, LIST, 0, SURF_A, 16, 4, 1, 1); settle(s);
  CHECK_EQ(s.bus.raw_r32(SURF_A), 0x00123456u); CHECK_EQ(s.bus.raw_r32(SURF_A + 4), 0xFF777777u);
  Ps t0 = s.sched.now; u64 b0 = s.bus.arb_bytes[Bus::R_DDR];
  blt(s, BOP_SPRITES, LIST, 0, SURF_A, 16, 4, 1, 1); settle(s);
  CHECK_EQ(s.bus.arb_bytes[Bus::R_DDR] - b0, 16 + 4 + 8);                      // lista + pixel de origem + RMW do destino
  CHECK(s.sched.now > t0);
  blt(s, BOP_SPRITES, LIST, 0, SURF_A, 16, 4, 1, SPRITE_MAX + 1); settle(s);   // lista grande demais
  CHECK(s.blt.error);
  s.blt.write(BLT_STATUS, 4); CHECK(!s.blt.error);
  put_sprite(s, LIST, 0, 0x08000000, 4, 0, 0, 0, 1, 1);                        // origem fora da memória
  blt(s, BOP_SPRITES, LIST, 0, SURF_A, 16, 4, 1, 1); settle(s);
  CHECK(s.blt.error);
}

// ---- camadas de vídeo: scroll com wrap, ordem, alfa, mistura aditiva, janela e Copper (paralaxe, SKIP, IRQ) ----
static void layer_w(Soc& s, int i, u32 reg, u32 v) { mmio_w(s, VIDEO_BASE + VID_LAYER0 + i * VID_LAYER_STRIDE + reg, v); }
static void layer_setup(Soc& s, int i, u32 addr, u32 pitch, u32 flags) { layer_w(s, i, LR_ADDR, addr); layer_w(s, i, LR_PITCH, pitch); layer_w(s, i, LR_FLAGS, flags); }

TEST(video_layer_scroll_wrap_and_clamp) {
  Soc s; const u32 MAP = 0x100000;
  for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) s.bus.raw_w32(MAP + y * 64 + x * 4, 0xFF000000 | (x << 8) | y);      // mapa 16x16: g = x, b = y
  layer_setup(s, 2, MAP, 64, LF_ENABLE | LF_WRAP); layer_w(s, 2, LR_SIZE, 16 | (16 << 16));
  s.video.vblank(); CHECK_EQ(s.video.frame[0], 0x000000u); CHECK_EQ(s.video.frame[5 * 320 + 3], 0x000305u);
  CHECK_EQ(s.video.frame[20 * 320 + 35], 0x000304u | 0u);                              // (35 mod 16 = 3, 20 mod 16 = 4)
  layer_w(s, 2, LR_SCX, 2); layer_w(s, 2, LR_SCY, (u32)-1);                            // tela (0,0) lê mapa (2, -1 -> 15)
  s.video.vblank(); CHECK_EQ(s.video.frame[0], 0x00020Fu);
  layer_w(s, 2, LR_FLAGS, LF_ENABLE); s.video.vblank();                                // sem wrap: fora do mapa fica no fundo
  CHECK_EQ(s.video.frame[0], 0u); CHECK_EQ(s.video.frame[1 * 320 + 0], 0x000200u); CHECK_EQ(s.video.frame[1 * 320 + 20], 0u);
}

TEST(video_layer_order_alpha_additive_and_window) {
  Soc s; const u32 A_ = 0x100000, B_ = 0x180000;
  for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) { s.bus.raw_w32(A_ + y * 1280 + x * 4, 0xFFFF0000); s.bus.raw_w32(B_ + y * 1280 + x * 4, 0xFF0000FF); }
  mmio_w(s, VIDEO_BASE + VID_BG, 0xFF000000);
  layer_setup(s, 0, A_, 1280, LF_ENABLE); layer_setup(s, 3, B_, 1280, LF_ENABLE);
  s.video.vblank(); CHECK_EQ(s.video.frame[0], 0x0000FFu);                             // camada 3 em cima
  mmio_w(s, VIDEO_BASE + VID_ORDER, 0x0123); s.video.vblank(); CHECK_EQ(s.video.frame[0], 0xFF0000u);   // ordem invertida
  layer_w(s, 0, LR_ALPHA, 128); s.video.vblank();                                      // vermelho 50% sobre azul (camada 3 embaixo agora)
  u32 m = s.video.frame[0]; CHECK(std::abs((int)(m >> 16) - 128) <= 1); CHECK(std::abs((int)(m & 255) - 127) <= 1);
  layer_w(s, 0, LR_ALPHA, 255); layer_w(s, 0, LR_FLAGS, LF_ENABLE | LF_ADD); s.video.vblank(); CHECK_EQ(s.video.frame[0], 0xFF00FFu);   // aditivo
  layer_w(s, 0, LR_WINX, 100 | (200 << 16)); layer_w(s, 0, LR_WINY, 50 | (60 << 16)); s.video.vblank();
  CHECK_EQ(s.video.frame[55 * 320 + 150], 0xFF00FFu); CHECK_EQ(s.video.frame[55 * 320 + 99], 0x0000FFu); CHECK_EQ(s.video.frame[55 * 320 + 200], 0x0000FFu);
  CHECK_EQ(s.video.frame[49 * 320 + 150], 0x0000FFu); CHECK_EQ(s.video.frame[60 * 320 + 150], 0x0000FFu);
}

TEST(video_copper_parallax_skip_and_irq) {
  Soc s; const u32 MAP = 0x100000, L = 0x80000; s.bus.irq_mask = IRQ_COPPER;
  for (int y = 0; y < 8; y++) for (int x = 0; x < 64; x++) s.bus.raw_w32(MAP + y * 256 + x * 4, 0xFF000000 | x);                   // faixa horizontal com gradiente em x
  layer_setup(s, 1, MAP, 256, LF_ENABLE | LF_WRAP); layer_w(s, 1, LR_SIZE, 64 | (8 << 16));
  auto put = [&](u32 i, u32 a, u32 b) { s.bus.raw_w32(L + i * 8, a); s.bus.raw_w32(L + i * 8 + 4, b); };
  u32 SCX = VIDEO_BASE + VID_LAYER0 + 1 * VID_LAYER_STRIDE + LR_SCX; u32 i = 0;
  put(i++, cop_move(SCX), 0);  put(i++, cop_wait(80), 0); put(i++, cop_move(SCX), 8);       // faixa do meio rola 8 px
  put(i++, cop_wait(160), 0);  put(i++, cop_skip(200), 0); put(i++, cop_move(SCX), 24);    // linha 160 < 200: não pula; aplica 24
  put(i++, cop_wait(220), 0);  put(i++, cop_skip(200), 0); put(i++, cop_move(SCX), 99);    // linha 220 >= 200: pula o MOVE
  put(i++, cop_irq(), 0); put(i++, 0, 0);
  mmio_w(s, COP_BASE + COP_LIST, L); mmio_w(s, COP_BASE + COP_CTRL, 1);
  s.video.vblank();
  CHECK_EQ(s.video.frame[10 * 320 + 0], 0x000000u); CHECK_EQ(s.video.frame[100 * 320 + 0], 0x000008u);
  CHECK_EQ(s.video.frame[170 * 320 + 0], 0x000018u); CHECK_EQ(s.video.frame[239 * 320 + 0], 0x000018u);   // o MOVE de 99 foi pulado
  CHECK(s.bus.irq_pending()); CHECK(s.bus.irq_status & IRQ_COPPER);
}
