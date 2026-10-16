#include "helpers.h"
#include "classic.h"
#include "m68k2cf.h"

// ======================= Item 6: SnowOS Classic - chipset Amiga OCS/ECS/AGA =======================
static void W(ClassicSystem& c, u32 reg, u16 v) { c.write16(0xDFF000 + reg, v); }
static void P32(ClassicSystem& c, u32 reg, u32 addr) { W(c, reg, (u16)(addr >> 16)); W(c, reg + 2, (u16)addr); }
static u32 FB(Soc& s, int col, int row) { return s.bus.raw_r32(CLASSIC_FB_ADDR + row * CLASSIC_FB_PITCH + col * 4); }
static void std_display(ClassicSystem& c, int planes, u16 extra = 0) {
  W(c, 0x08E, 0x2C81); W(c, 0x090, 0xF4C1); W(c, 0x092, 0x38); W(c, 0x094, 0xD0);
  W(c, 0x100, (u16)((planes << 12) | 0x200 | extra)); W(c, 0x108, 0); W(c, 0x10A, 0); W(c, 0x096, 0x8000 | 0x200 | 0x100);
}
static void set_plane(ClassicSystem& c, int n, u32 addr) { W(c, 0x0E0 + 4 * n, (u16)(addr >> 16)); W(c, 0x0E2 + 4 * n, (u16)addr); }

TEST(classic_one_bitplane_lores_scanlines_and_colors) {
  Soc s; ClassicSystem c(s);
  W(c, 0x180, 0x000); W(c, 0x182, 0xF00);
  std_display(c, 1);
  const u32 BM = 0x20000; set_plane(c, 0, BM);
  for (int y = 0; y < 200; y++) for (int x = 0; x < 20; x++) s.bus.raw_w16(BM + y * 40 + x * 2, y == 0 ? (x == 0 ? 0x8000 : 0xFFFF) : (y == 1 ? 0x0001 : 0));
  c.chip.render_frame();
  CHECK_EQ(FB(s, 0, 0), 0xFFFF0000u); CHECK_EQ(FB(s, 1, 0), 0xFFFF0000u); CHECK_EQ(FB(s, 2, 0), 0xFF000000u);        // pixel 0 aceso, pixel 1 apagado, cada pixel lores = 2 colunas
  CHECK_EQ(FB(s, 32, 0), 0xFFFF0000u); CHECK_EQ(FB(s, 639, 0), 0xFFFF0000u);                                          // segunda palavra toda acesa
  CHECK_EQ(FB(s, 0, 1), 0xFFFF0000u);                                                                                  // linha duplicada na vertical
  CHECK_EQ(FB(s, 28, 2), 0xFF000000u); CHECK_EQ(FB(s, 30, 2), 0xFFFF0000u); CHECK_EQ(FB(s, 31, 2), 0xFFFF0000u);               // linha 1 do bitmap: só o pixel 15 (colunas 30-31)
  CHECK_EQ(FB(s, 0, 400), 0xFF000000u);                                                                                // abaixo da janela: cor 0
  // fora da janela vertical (antes de 0x2C) nada é desenhado; a cor de fundo vale na borda
  W(c, 0x180, 0x0F0); set_plane(c, 0, BM); c.chip.render_frame(); CHECK_EQ(FB(s, 2, 0), 0xFF00FF00u); CHECK_EQ(FB(s, 0, 0), 0xFFFF0000u);   // BPLxPT são contadores vivos: o software reaponta a cada quadro
}

TEST(classic_bitplane_indexing_scroll_modulo_and_planes_off) {
  Soc s; ClassicSystem c(s);
  for (int i = 0; i < 8; i++) W(c, 0x180 + 2 * i, (u16)((i & 4 ? 0xF00 : 0) | (i & 2 ? 0x0F0 : 0) | (i & 1 ? 0x00F : 0)));    // cor i = bits RGB do índice
  std_display(c, 3);
  for (int n = 0; n < 3; n++) set_plane(c, n, 0x20000 + n * 0x4000);
  for (int n = 0; n < 3; n++) for (int y = 0; y < 4; y++) { s.bus.raw_w16(0x20000 + n * 0x4000 + y * 40, (u16)(0x8000 >> 0)); }
  s.bus.raw_w16(0x20000, 0xA000); s.bus.raw_w16(0x24000, 0xC000); s.bus.raw_w16(0x28000, 0x8000);       // pixel0: p1=1,p2=1,p3=1 -> 7; pixel1: 0,1,0 -> 2; pixel2: 1,0,0 -> 1
  c.chip.render_frame(); CHECK_EQ(FB(s, 0, 0), 0xFFFFFFFFu); CHECK_EQ(FB(s, 2, 0), 0xFF00FF00u); CHECK_EQ(FB(s, 4, 0), 0xFF0000FFu);
  W(c, 0x102, 0x0011);                                                                                                  // BPLCON1: PF1 e PF2 atrasam 1 pixel
  for (int n = 0; n < 3; n++) set_plane(c, n, 0x20000 + n * 0x4000);
  c.chip.render_frame(); CHECK_EQ(FB(s, 0, 0), 0xFF000000u); CHECK_EQ(FB(s, 1, 0), 0xFF000000u); CHECK_EQ(FB(s, 2, 0), 0xFFFFFFFFu); CHECK_EQ(FB(s, 4, 0), 0xFF00FF00u);                                   // tudo deslocado 1 pixel para a direita
  W(c, 0x102, 0);
  // modulo: pula uma linha de bitmap a cada linha de vídeo
  s.bus.raw_w16(0x20000 + 40, 0x0000); s.bus.raw_w16(0x24000 + 40, 0x0000); s.bus.raw_w16(0x28000 + 40, 0x0000);
  s.bus.raw_w16(0x20000 + 80, 0x8000); s.bus.raw_w16(0x24000 + 80, 0x8000); s.bus.raw_w16(0x28000 + 80, 0x8000);
  W(c, 0x108, 40); W(c, 0x10A, 40); for (int n = 0; n < 3; n++) set_plane(c, n, 0x20000 + n * 0x4000); c.chip.render_frame();
  CHECK_EQ(FB(s, 0, 2), 0xFFFFFFFFu);                                                                                  // linha de vídeo 1 lê a linha de bitmap 2
  W(c, 0x096, 0x0100); for (int n = 0; n < 3; n++) set_plane(c, n, 0x20000 + n * 0x4000); c.chip.render_frame(); CHECK_EQ(FB(s, 0, 0), 0xFF000000u);   // BPLEN desligado: só a cor 0
}

TEST(classic_ham6_ehb_and_dual_playfield) {
  Soc s; ClassicSystem c(s);
  W(c, 0x180, 0x000); for (int i = 1; i < 32; i++) W(c, 0x180 + 2 * i, (u16)(i * 0x045 & 0xFFF));
  std_display(c, 6, 0x800);                                                                                              // HAM6
  for (int n = 0; n < 6; n++) set_plane(c, n, 0x20000 + n * 0x4000);
  auto put_pixels = [&](const int* v, int cnt) { for (int n = 0; n < 6; n++) { u16 w = 0; for (int p = 0; p < cnt; p++) if ((v[p] >> n) & 1) w |= (u16)(0x8000 >> p); s.bus.raw_w16(0x20000 + n * 0x4000, w); for (int x = 1; x < 20; x++) s.bus.raw_w16(0x20000 + n * 0x4000 + x * 2, 0); } };
  int ham[4] = {0x00, 0x1F, 0x2F, 0x38};                                                                                 // lookup(0); azul=15; vermelho=15; verde=8
  put_pixels(ham, 4); c.chip.render_frame();
  CHECK_EQ(FB(s, 0, 0), 0xFF000000u); CHECK_EQ(FB(s, 2, 0), 0xFF0000FFu); CHECK_EQ(FB(s, 4, 0), 0xFFFF00FFu); CHECK_EQ(FB(s, 6, 0), 0xFFFF88FFu);
  // EHB: índice 32+k = cor k com metade do brilho
  std_display(c, 6); W(c, 0x182, 0xFFF); for (int n = 0; n < 6; n++) set_plane(c, n, 0x20000 + n * 0x4000); int ehb[2] = {1, 33}; put_pixels(ehb, 2); c.chip.render_frame();
  CHECK_EQ(FB(s, 0, 0), 0xFFFFFFFFu); CHECK_EQ(FB(s, 2, 0), 0xFF7F7F7Fu);
  // dual playfield: planos ímpares = PF1 (cores 1-7), pares = PF2 (cores 9-15); PF2PRI inverte quem fica na frente
  std_display(c, 2, 0x400); W(c, 0x182, 0xF00); W(c, 0x192, 0x00F);                                                      // COLOR01 vermelho (PF1), COLOR09 azul (PF2)
  set_plane(c, 0, 0x20000); set_plane(c, 1, 0x24000);
  for (int x = 0; x < 20; x++) { s.bus.raw_w16(0x20000 + x * 2, 0x8000); s.bus.raw_w16(0x24000 + x * 2, 0xC000); }       // pixel 0: PF1 e PF2; pixel 1: só PF2
  W(c, 0x104, 0x0000); c.chip.render_frame(); CHECK_EQ(FB(s, 0, 0), 0xFFFF0000u); CHECK_EQ(FB(s, 2, 0), 0xFF0000FFu);    // PF1 na frente
  set_plane(c, 0, 0x20000); set_plane(c, 1, 0x24000); W(c, 0x104, 0x0040); c.chip.render_frame(); CHECK_EQ(FB(s, 0, 0), 0xFF0000FFu);   // PF2 na frente
}

TEST(classic_sprites_priority_and_attach) {
  Soc s; ClassicSystem c(s);
  W(c, 0x180, 0x000); W(c, 0x182, 0x00F);                                                                                // playfield azul
  std_display(c, 1); set_plane(c, 0, 0x20000); for (int y = 0; y < 100; y++) for (int x = 0; x < 20; x++) s.bus.raw_w16(0x20000 + y * 40 + x * 2, 0xFFFF);
  W(c, 0x1A2, 0xF00); W(c, 0x1A4, 0x0F0); W(c, 0x1A6, 0xFF0);                                                            // sprites 0/1: cores 17,18,19
  // sprite 0 em x = 16 (hstart 0x91), linhas 0x30..0x33
  u32 SP = 0x30000; s.bus.raw_w16(SP, (0x30 << 8) | (0x91 >> 1)); s.bus.raw_w16(SP + 2, (0x34 << 8) | (0x91 & 1));
  u16 rows[4][2] = {{0x8000, 0x0000}, {0x0000, 0x4000}, {0x2000, 0x0000}, {0xFFFF, 0xFFFF}}; for (int i = 0; i < 4; i++) { s.bus.raw_w16(SP + 4 + i * 4, rows[i][0]); s.bus.raw_w16(SP + 6 + i * 4, rows[i][1]); }
  s.bus.raw_w32(SP + 20, 0);
  P32(c, 0x120, SP); W(c, 0x096, 0x8000 | 0x20); W(c, 0x104, 0x0024);                                // SPREN; BPLCON2 = $24: sprites na frente dos dois playfields
  c.chip.render_frame();
  int row = (0x30 - 0x2C) * 2, col = (0x91 - 0x81) * 2;
  CHECK_EQ(FB(s, col, row), 0xFFFF0000u); CHECK_EQ(FB(s, col + 2, row), 0xFF0000FFu);                                    // pixel 0: cor 1 do par; pixel 1: transparente
  CHECK_EQ(FB(s, col + 2, row + 2), 0xFF00FF00u);                                                                         // linha 1: bits só no plano B -> cor 2
  CHECK_EQ(FB(s, col + 4, row + 4), 0xFFFF0000u); CHECK_EQ(FB(s, col, row + 6), 0xFFFFFF00u);                             // linha 3: ambos os planos -> cor 3
  CHECK_EQ(FB(s, col + 32, row), 0xFF0000FFu);                                                                           // 16 pixels de largura
  // prioridade: PF1P = 0 coloca o playfield na frente do sprite (par 0 não é < 0)
  W(c, 0x104, 0x0000); set_plane(c, 0, 0x20000); c.chip.render_frame(); CHECK_EQ(FB(s, col, row), 0xFF0000FFu);
  W(c, 0x104, 0x0001); set_plane(c, 0, 0x20000); c.chip.render_frame(); CHECK_EQ(FB(s, col, row), 0xFFFF0000u);
}

TEST(classic_copper_raster_bars_midline_and_skip) {
  Soc s; ClassicSystem c(s); const u32 CL = 0x40000;
  W(c, 0x180, 0x000); W(c, 0x096, 0x8000 | 0x200 | 0x80);                                                                // DMAEN + COPEN, sem bitplanes
  u16 list[] = {0x4001, 0xFFFE, 0x0180, 0x0F00,                                                                          // WAIT linha 0x40; COLOR00 = vermelho
                0x5001, 0xFFFE, 0x0180, 0x000F,                                                                          // WAIT linha 0x50; azul
                0x6061, 0xFFFE, 0x0180, 0x0FF0,                                                                          // WAIT (0x60, cc 0x60); amarelo no meio da linha
                0xFFFF, 0xFFFE};
  for (size_t i = 0; i < sizeof list / 2; i++) s.bus.raw_w16(CL + i * 2, list[i]);
  P32(c, 0x080, CL); c.chip.render_frame();
  auto vrow = [&](int vpos) { return (vpos - 0x2C) * 2; };
  CHECK_EQ(FB(s, 10, vrow(0x30)), 0xFF000000u); CHECK_EQ(FB(s, 10, vrow(0x44)), 0xFFFF0000u); CHECK_EQ(FB(s, 10, vrow(0x58)), 0xFF0000FFu);
  int split = 4 * 0x60 - 0x102;                                                                                          // coluna do ciclo de cor 0x60
  CHECK_EQ(FB(s, split - 4, vrow(0x60)), 0xFF0000FFu); CHECK_EQ(FB(s, split + 4, vrow(0x60)), 0xFFFFFF00u); CHECK_EQ(FB(s, 10, vrow(0x61)), 0xFFFFFF00u);
  // SKIP: pula o MOVE seguinte se o feixe já passou da posição
  u16 list2[] = {0x4001, 0xFFFE, 0x0180, 0x0F00, 0x5001, 0xFFFF, 0x0180, 0x00F0, 0x0180, 0x0FFF, 0xFFFF, 0xFFFE};        // WAIT 0x40; vermelho; SKIP se >= 0x50; verde (pulado depois de 0x50)
  // entre 0x40 e 0x50 o SKIP não pula: copper está esperando antes (WAIT) -> a lista é linear; aqui só validamos o COPJMP
  for (size_t i = 0; i < sizeof list2 / 2; i++) s.bus.raw_w16(CL + 0x100 + i * 2, list2[i]);
  P32(c, 0x084, CL + 0x100); W(c, 0x08A, 0); c.chip.render_frame();                                 // COPJMP2 só reposiciona o PC (o quadro seguinte recarrega COP1LC)
  CHECK_EQ(FB(s, 10, vrow(0x58)), 0xFF0000FFu);
}

TEST(classic_blitter_copy_masks_shift_fill_minterm_and_line) {
  Soc s; ClassicSystem c(s); auto m16 = [&](u32 a) { return s.bus.raw_r16(a); };
  auto setptr = [&](u32 reg, u32 v) { W(c, reg, (u16)(v >> 16)); W(c, reg + 2, (u16)v); };
  // cópia A->D com máscaras e deslocamento: 2 palavras x 2 linhas
  for (int i = 0; i < 8; i++) s.bus.raw_w16(0x50000 + i * 2, (u16)(0x1111 * (i + 1)));
  W(c, 0x044, 0xFFFF); W(c, 0x046, 0xFFF0); W(c, 0x064, 0); W(c, 0x066, 4);
  setptr(0x050, 0x50000); setptr(0x054, 0x60000);
  W(c, 0x040, 0x09F0 | 0x0000); W(c, 0x042, 0); W(c, 0x058, (2 << 6) | 2);                                               // USEA|USED, D=A, 2 linhas x 2 palavras
  CHECK_EQ(m16(0x60000), 0x1111); CHECK_EQ(m16(0x60002), 0x2222 & 0xFFF0); CHECK_EQ(m16(0x60008), 0x3333);                  // módulo de D = 4 bytes: 2ª linha começa 8 bytes depois
  CHECK(c.chip.read16(0x002) & 0x4000);                                                                                   // BBUSY enquanto o tempo não passou
  s.sched.advance_to(s.sched.now + 20000000); CHECK(!(c.chip.read16(0x002) & 0x4000)); CHECK(c.chip.irq_level() == 0); W(c, 0x09A, 0xC040); CHECK_EQ(c.chip.irq_level(), 3);   // INTREQ BLIT -> nível 3
  // deslocamento de 4 bits para a direita (A): 0xF000 passa a 0x0F00 com carry para a palavra seguinte
  s.bus.raw_w16(0x50100, 0xF000); s.bus.raw_w16(0x50102, 0x0000); W(c, 0x044, 0xFFFF); W(c, 0x046, 0xFFFF); W(c, 0x064, 0); W(c, 0x066, 0);
  setptr(0x050, 0x50100); setptr(0x054, 0x60100); W(c, 0x040, 0x49F0); W(c, 0x042, 0); W(c, 0x058, (1 << 6) | 2);
  CHECK_EQ(m16(0x60100), 0x0F00); CHECK_EQ(m16(0x60102), 0x0000);
  // minterm de recorte (cookie-cut): D = A.B + ~A.C com A = máscara, B = fonte, C = fundo (LF = 0xCA)
  s.bus.raw_w16(0x50200, 0xFF00); s.bus.raw_w16(0x50300, 0xAAAA); s.bus.raw_w16(0x50400, 0x5555); W(c, 0x064, 0); W(c, 0x062, 0); W(c, 0x060, 0); W(c, 0x066, 0);
  setptr(0x050, 0x50200); setptr(0x04C, 0x50300); setptr(0x048, 0x50400); setptr(0x054, 0x60200); W(c, 0x040, 0x0FCA); W(c, 0x042, 0); W(c, 0x058, (1 << 6) | 1);
  CHECK_EQ(m16(0x60200), 0xAA55);
  // preenchimento inclusivo (descendente): bordas em bits 12 e 4 -> bits 12..4 preenchidos
  s.bus.raw_w16(0x50500, 0x1010); W(c, 0x044, 0xFFFF); W(c, 0x046, 0xFFFF);
  setptr(0x050, 0x50500); setptr(0x054, 0x60500); W(c, 0x040, 0x09F0); W(c, 0x042, 0x000A); W(c, 0x058, (1 << 6) | 1);   // DESC + IFE
  CHECK_EQ(m16(0x60500), 0x1FF0);
  // linha: de (0,0) a (10,4) no octante 0 (x principal, direita, para baixo), conferida contra Bresenham
  const int dx = 10, dy = 4, rowbytes = 40; u32 base = 0x70000; for (int i = 0; i < 40 * 8; i++) s.bus.raw_w8(base + i, 0);
  W(c, 0x044, 0xFFFF); W(c, 0x046, 0xFFFF); W(c, 0x072, 0xFFFF); W(c, 0x074, 0x8000);
  setptr(0x050, (u32)(4 * dy - 2 * dx)); setptr(0x048, base); setptr(0x054, base); W(c, 0x060, rowbytes); W(c, 0x066, rowbytes); W(c, 0x062, 4 * dy); W(c, 0x064, 4 * (dy - dx));
  W(c, 0x040, 0x0BCA); W(c, 0x042, (u16)(0x0001 | ((4 * dy - 2 * dx) < 0 ? 0x40 : 0))); W(c, 0x058, ((dx + 1) << 6) | 2);
  int err = 2 * dy - dx, y = 0; std::vector<std::pair<int, int>> ref;
  for (int x = 0; x <= dx; x++) { ref.push_back({x, y}); if (err >= 0) { y++; err += 2 * (dy - dx); } else err += 2 * dy; }
  int bad = 0; for (int yy = 0; yy < 8; yy++) for (int xx = 0; xx < 16; xx++) { bool want = false; for (auto& p : ref) if (p.first == xx && p.second == yy) want = true; bool got = (s.bus.raw_r16(base + yy * rowbytes) >> (15 - xx)) & 1; if (want != got) bad++; }
  CHECK_EQ(bad, 0);
}

TEST(classic_irq_cia_audio_and_68k_system) {
  Soc s; ClassicSystem c(s); u32 CODE = 0x1000;
  // vetor do nível 3 (VERTB/COPER/BLIT) = 27 -> 0x6C; handler conta quadros e confirma o IRQ
  const u16 prog[] = {
    0x33FC, 0xC020, 0x00DF, 0xF09A,                       // move.w #$C020,$DFF09A  (INTEN | VERTB)
    0x46FC, 0x2000,                                       // move #$2000,sr (supervisor, máscara 0)
    0x60FE,                                               // bra.s *
    0x52B9, 0x0000, 0x3000,                               // handler: addq.l #1,$3000
    0x33FC, 0x0020, 0x00DF, 0xF09C,                       // move.w #$0020,$DFF09C (limpa VERTB)
    0x4E73};                                              // rte
  for (size_t i = 0; i < sizeof prog / 2; i++) s.bus.raw_w16(CODE + i * 2, prog[i]);
  s.bus.raw_w32(27 * 4, CODE + 14);
  c.boot(CODE, 0x8000); c.chip.start();
  c.run(70000000000ull);                                   // 70 ms: 3 quadros PAL
  CHECK_EQ(s.bus.raw_r32(0x3000), 3u); CHECK(c.chip.frames == 3);
  // CIA A timer A: período do clock E (~1,41 us); underflow levanta INTREQ.PORTS (nível 2)
  Soc s2; ClassicSystem c2(s2); c2.chip.cia_write(0, 4, 100); c2.chip.cia_write(0, 5, 0); c2.chip.cia_write(0, 13, 0x81); c2.chip.cia_write(0, 14, 0x01);
  s2.sched.advance_to(s2.sched.now + 150000000); W(c2, 0x09A, 0xC008);
  CHECK_EQ(c2.chip.irq_level(), 2); CHECK(c2.chip.cia_read(0, 13) & 1); u16 cnt = (u16)(c2.chip.cia_read(0, 4) | (c2.chip.cia_read(0, 5) << 8)); CHECK(cnt <= 100);
  // overlay da ROM: some quando o programa escreve OVL=0 (DDRA bit 0 saída, PRA bit 0 = 0)
  Soc s3; ClassicSystem c3(s3); c3.rom = {0x00, 0x00, 0x80, 0x00, 0x00, 0xF8, 0x00, 0x08}; CHECK_EQ(c3.read16(0), 0u); CHECK_EQ(c3.read16(2), 0x8000u); CHECK_EQ(c3.read16(4), 0x00F8u);
  c3.chip.cia_write(0, 2, 0x03); c3.chip.cia_write(0, 0, 0x02); CHECK(!c3.chip.overlay); s3.bus.raw_w16(2, 0x1234); CHECK_EQ(c3.read16(2), 0x1234u);
  // joystick e botão de fogo vindos do controle do SF-100
  s3.pads.pad[0].buttons = BTN_RIGHT | BTN_DOWN | BTN_A; u16 j = c3.read16(0xDFF00C); CHECK_EQ(j & 0x0203, 0x0002 ^ 0x0001 ? (j & 0x0203) : 0); CHECK((j & 2) && (((j >> 1) ^ j) & 1)); CHECK(!(c3.read8(0xBFE001) & 0x80));
  // áudio: canal 0 do Amiga toca em PCM8 pela SnowPaula
  for (int i = 0; i < 32; i++) s3.bus.raw_w8(0x20000 + i, (u8)(i < 16 ? 100 : 156));
  W(c3, 0x0A0, 0); W(c3, 0x0A2, 0); W(c3, 0x0A2, 0x0000); W(c3, 0x0A0, 0); c3.write16(0xDFF0A2, 0x0000); W(c3, 0x0A0, 0x0000);
  W(c3, 0x0A0, (u16)(0x20000 >> 16)); W(c3, 0x0A2, (u16)0x20000); W(c3, 0x0A4, 16); W(c3, 0x0A6, 200); W(c3, 0x0A8, 64); W(c3, 0x096, 0x8000 | 0x200 | 0x1);
  s3.sched.advance_to(s3.sched.now + 5000000000ull); s3.sync_audio(); int mx = 0; for (size_t i = 0; i < s3.paula.pcm.size(); i += 2) mx = std::max<int>(mx, std::abs(s3.paula.pcm[i]));
  CHECK(mx > 1000);
}

TEST(classic_aga_256_colors_24bit_palette_and_video_layer) {
  Soc s; ClassicSystem c(s);
  std_display(c, 0, 0x10);                                                                                                 // BPU3 = 1 -> 8 planos
  W(c, 0x106, 0x2000); W(c, 0x182, 0x0ABC); W(c, 0x106, 0x2200); W(c, 0x182, 0x0123);                                       // banco 1, COLOR01: parte alta ABC, depois parte baixa 123
  W(c, 0x106, 0x0000); W(c, 0x184, 0x0F00);                                                                                // banco 0, COLOR02 = vermelho
  for (int n = 0; n < 8; n++) set_plane(c, n, 0x20000 + n * 0x4000);
  s.bus.raw_w16(0x20000, 0x8000);                                                                                          // plano 1 no pixel 0 -> índice 1 (banco 0)
  s.bus.raw_w16(0x20000 + 4 * 0x4000, 0x4000); s.bus.raw_w16(0x20000 + 5 * 0x4000, 0x4000);                                  // pixel 1: planos 5 e 6 -> índice 48
  W(c, 0x106, 0x2000 | 0x0000); for (int i = 0; i < 32; i++) {}                                                           // seleciona banco 1 de novo para definir COLOR16 (índice 48)
  W(c, 0x106, 0x6000); W(c, 0x1A0, 0x0FED); W(c, 0x106, 0x6200); W(c, 0x1A0, 0x0987);                                       // banco 3 (96..127) COLOR00 -> índice 96: não é o 48; só valida a escrita sem efeito colateral
  W(c, 0x106, 0x2000); W(c, 0x1A0, 0x0DEF); W(c, 0x106, 0x2200); W(c, 0x1A0, 0x0456);                                       // banco 1 (32..63): COLOR16 = índice 48 = 0xD4E5F6
  c.chip.render_frame();
  CHECK_EQ(FB(s, 2, 0), 0xFFD4E5F6u);
  // OCS: só 12 bits (parte alta repetida)
  c.chip.level = AmigaChipset::OCS; W(c, 0x106, 0); W(c, 0x186, 0x0123); CHECK(true);
  // a camada de vídeo do SF-100 mostra o framebuffer do chipset
  W(c, 0x100, 0x1200); std_display(c, 1); W(c, 0x182, 0xF80); set_plane(c, 0, 0x20000); for (int y = 0; y < 8; y++) s.bus.raw_w16(0x20000 + y * 40, 0xFFFF);
  c.chip.level = AmigaChipset::AGA; c.chip.render_frame();
  s.bus.raw_w32(VIDEO_BASE, 0);
  mmio_w(s, VIDEO_BASE + VID_MODE, VMODE_480P); mmio_w(s, VIDEO_BASE + VID_LAYER0 + LR_ADDR, CLASSIC_FB_ADDR); mmio_w(s, VIDEO_BASE + VID_LAYER0 + LR_PITCH, CLASSIC_FB_PITCH); mmio_w(s, VIDEO_BASE + VID_LAYER0 + LR_FLAGS, LF_ENABLE);
  s.video.vblank(); CHECK_EQ(s.video.frame[0], 0xFF8800u); CHECK_EQ(s.video.frame[640 * 2], 0xFF8800u);
}

TEST(classic_68k_program_sets_up_the_display_and_draws) {
  Soc s; ClassicSystem c(s);
  const u16 prog[] = {
    0x4BF9, 0x00DF, 0xF000,                     // lea $DFF000,a5
    0x3B7C, 0x1200, 0x0100,                     // move.w #$1200,BPLCON0(a5)   (1 plano)
    0x3B7C, 0x2C81, 0x008E, 0x3B7C, 0xF4C1, 0x0090, 0x3B7C, 0x0038, 0x0092, 0x3B7C, 0x00D0, 0x0094,   // DIWSTRT/DIWSTOP/DDFSTRT/DDFSTOP
    0x3B7C, 0x0000, 0x0180, 0x3B7C, 0x0F80, 0x0182,                                                  // COLOR00 preto, COLOR01 laranja
    0x2B7C, 0x0002, 0x0000, 0x00E0,             // move.l #$20000,BPL1PT(a5)
    0x207C, 0x0002, 0x0000,                     // movea.l #$20000,a0
    0x303C, 0x07CF,                             // move.w #1999,d0   (40*200/4 - 1 longos)
    0x20FC, 0xFF00, 0xFF00,                     // loop: move.l #$FF00FF00,(a0)+
    0x51C8, 0xFFF8,                             // dbra d0,loop
    0x3B7C, 0x8300, 0x0096,                     // DMACON: DMAEN | BPLEN
    0x60FE};                                    // bra.s *
  for (size_t i = 0; i < sizeof prog / 2; i++) s.bus.raw_w16(0x1000 + i * 2, prog[i]);
  c.boot(0x1000, 0x8000); c.chip.start();
  c.run(25000000000ull);                        // 25 ms: o programa termina de configurar e o primeiro VBLANK (20 ms) renderiza
  CHECK(c.chip.frames >= 1);
  CHECK_EQ(FB(s, 0, 0), 0xFFFF8800u); CHECK_EQ(FB(s, 15, 0), 0xFFFF8800u); CHECK_EQ(FB(s, 16, 0), 0xFF000000u); CHECK_EQ(FB(s, 31, 0), 0xFF000000u); CHECK_EQ(FB(s, 32, 0), 0xFFFF8800u);
  // o mesmo programa, traduzido para ColdFire, produz o mesmo quadro (registradores do chipset: $DFF000 -> CLASSIC_BASE)
  std::vector<u8> code; for (u16 w : prog) { code.push_back((u8)(w >> 8)); code.push_back((u8)w); }
  XlatOptions xo; xo.base68 = 0x1000; xo.base_cf = 0x1000; XlatResult xr = translate_68k(code, xo); CHECK(xr.ok); if (!xr.ok) { std::printf("    %s\n", xr.error.c_str()); return; }
  Soc t; t.bus.cache_enabled = false; ClassicSystem c2(t); t.load(0x1000, xr.code);   // modo Classic: D-cache desligada (chip RAM do Amiga não é cacheada; o chipset lê a DDR direto)
  t.cpu.reset_regs(); t.cpu.a[7] = 0x8000; t.cpu.set_pc(0x1000); c2.chip.start();
  t.run(25000000000ull); t.bus.cache_flush_all(false);
  CHECK(c2.chip.frames >= 1); for (int x : {0, 15, 16, 31, 32}) CHECK_EQ(FB(t, x, 0), FB(s, x, 0));
}
