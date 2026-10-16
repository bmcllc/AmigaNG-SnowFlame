#include "helpers.h"
#include "classic.h"

// ======================= SnowOS Classic: teclado, mouse e disquete (ADF) =======================
static void WR(ClassicSystem& c, u32 reg, u16 v) { c.write16(0xDFF000 + reg, v); }
static u16 RD(ClassicSystem& c, u32 reg) { return c.read16(0xDFF000 + reg); }

TEST(classic_keyboard_cia_serial_handshake_and_queue) {
  Soc s; ClassicSystem c(s);
  c.chip.cia_write(0, 13, 0x88); WR(c, 0x09A, 0xC000 | 0x0008);                 // SP e INTENA PORTS
  c.chip.key_raw(0x20, true);                                                   // 'A' pressionada
  CHECK_EQ(c.chip.cia_read(0, 12), 0xBF); CHECK(c.chip.cia_read(0, 13) & 8); CHECK_EQ(c.chip.irq_level(), 2);
  c.chip.key_raw(0x21, true); c.chip.key_raw(0x20, false);                      // enfileiradas até o handshake
  CHECK_EQ(c.chip.cia_read(0, 12), 0xBF);
  c.chip.cia_write(0, 14, 0x40); c.chip.cia_write(0, 14, 0x00);                 // SPMODE saída e volta: libera a próxima
  CHECK_EQ(c.chip.cia_read(0, 12), 0xBD);
  c.chip.cia_write(0, 14, 0x40); c.chip.cia_write(0, 14, 0x00);
  CHECK_EQ(c.chip.cia_read(0, 12), 0xBE);                                       // soltura: bit 7 do código
}

TEST(classic_hid_keys_and_mouse_reach_the_amiga) {
  Soc s; ClassicSystem c(s);
  CHECK_EQ(AmigaChipset::hid_to_amiga(0x04), 0x20); CHECK_EQ(AmigaChipset::hid_to_amiga(0x1D), 0x31); CHECK_EQ(AmigaChipset::hid_to_amiga(0x2C), 0x40);
  CHECK_EQ(AmigaChipset::hid_to_amiga(0x52), 0x4C); CHECK_EQ(AmigaChipset::hid_to_amiga(0xE1), 0x60); CHECK_EQ(AmigaChipset::hid_to_amiga(0xFF), -1);
  s.hid.key_event(0x04, true); s.hid.mouse_move(5, -2); s.hid.mouse_button(0, true); s.hid.mouse_button(1, true);
  c.chip.pump_hid();
  CHECK_EQ(c.chip.cia_read(0, 12), 0xBF);
  CHECK_EQ(RD(c, 0x00A), (u16)((0xFE << 8) | 5));
  CHECK_EQ(c.chip.cia_read(0, 0) & 0x40, 0); CHECK_EQ(RD(c, 0x016) & 0x0400, 0);
  s.hid.mouse_button(0, false); s.hid.mouse_button(1, false); c.chip.pump_hid();
  CHECK_EQ(c.chip.cia_read(0, 0) & 0x40, 0x40); CHECK_EQ(RD(c, 0x016) & 0x0400, 0x0400);
}

static std::vector<u8> make_adf() {
  std::vector<u8> adf(AmigaDrive::ADF_SIZE);
  for (size_t i = 0; i < adf.size(); i++) adf[i] = (u8)(i * 31 + (i >> 9) * 7 + (i >> 14));
  return adf;
}

TEST(classic_adf_mfm_roundtrip) {
  AmigaDrive d; d.adf = make_adf(); CHECK(d.present());
  for (int trk : {0, 1, 2, 159}) {
    auto t = d.mfm_track(trk); CHECK_EQ(t.size(), (size_t)AmigaDrive::TRACK_WORDS);
    for (int s = 0; s < 11; s++) {
      std::vector<u8> sec; CHECK(amiga_mfm_decode_sector(t, s, sec));
      CHECK(std::memcmp(sec.data(), &d.adf[(size_t)(trk * 11 + s) * 512], 512) == 0);
    }
  }
  // só os 2 sincronismos por setor aparecem (0x4489 é reservada)
  auto t = d.mfm_track(5); int syncs = 0; for (u16 w : t) if (w == 0x4489) syncs++; CHECK_EQ(syncs, 22);
}

TEST(classic_floppy_drive_status_step_and_dma_read) {
  Soc s; ClassicSystem c(s); auto& ch = c.chip; auto adf = make_adf();
  ch.cia_write(1, 3, 0xFF); ch.cia_write(0, 2, 0x03);
  ch.cia_write(1, 1, 0xFF); CHECK_EQ(ch.cia_read(0, 0) & 0x3C, 0x3C);           // nada selecionado: tudo inativo
  ch.cia_write(1, 1, 0x75);                                                     // motor ligado, DF0, lado 0, para dentro, /STEP alto
  CHECK_EQ(ch.cia_read(0, 0) & 0x04, 0);                                        // sem disco: /CHNG baixo
  CHECK_EQ(ch.cia_read(0, 0) & 0x20, 0x20);                                     // e não está pronto
  ch.insert_adf(adf); CHECK_EQ(ch.cia_read(0, 0) & 0x04, 0);                    // disco novo: ainda "trocado"
  ch.cia_write(1, 1, 0x74); ch.cia_write(1, 1, 0x75);                           // um passo para dentro: cilindro 1, limpa /CHNG
  CHECK_EQ(ch.df0.cyl, 1); CHECK_EQ(ch.cia_read(0, 0) & 0x04, 0x04); CHECK_EQ(ch.cia_read(0, 0) & 0x10, 0x10);   // /TK0 inativo
  CHECK_EQ(ch.cia_read(0, 0) & 0x20, 0x20);                                     // motor ainda acelerando
  advance_ns(s, 600000000); CHECK_EQ(ch.cia_read(0, 0) & 0x20, 0);              // /RDY ativo após ~500 ms
  ch.cia_write(1, 1, 0x77); ch.cia_write(1, 1, 0x76); ch.cia_write(1, 1, 0x77); CHECK_EQ(ch.df0.cyl, 0); CHECK_EQ(ch.cia_read(0, 0) & 0x10, 0);   // para fora: /TK0 ativo
  ch.cia_write(1, 1, 0x74); ch.cia_write(1, 1, 0x75); CHECK_EQ(ch.df0.cyl, 1);
  WR(c, 0x09A, 0xC002); WR(c, 0x096, 0x8000 | 0x0200 | 0x0010);                // INTENA DSKBLK; DMAEN + DSKEN
  WR(c, 0x09E, 0x8400); WR(c, 0x07E, 0x4489);                                  // WORDSYNC, sincronismo 4489
  WR(c, 0x020, 0x0002); WR(c, 0x022, 0x0000);                                   // DSKPT = 0x20000
  WR(c, 0x024, 0x8000 | 6400); CHECK_EQ(RD(c, 0x01E) & 2, 0); WR(c, 0x024, 0x8000 | 6400);   // duas escritas iniciam
  CHECK(RD(c, 0x01E) & 0x1000);                                                 // DSKSYN
  s.wait_until([&] { return RD(c, 0x01E) & 2; }); CHECK(RD(c, 0x01E) & 2); CHECK_EQ(c.chip.irq_level(), 1);
  std::vector<u16> trk(6400); for (int i = 0; i < 6400; i++) trk[i] = (u16)((s.bus.raw(0x20000 + 2 * i, 2)[0] << 8) | s.bus.raw(0x20000 + 2 * i, 2)[1]);
  CHECK_EQ(trk[0], 0x4489);
  for (int sec = 0; sec < 11; sec++) { std::vector<u8> d; CHECK(amiga_mfm_decode_sector(trk, sec, d)); CHECK(std::memcmp(d.data(), &adf[(size_t)(2 * 11 + sec) * 512], 512) == 0); }
}
