#include "helpers.h"
#include "adpcm.h"
#include "util.h"

static void pv(Soc& s, int v, u32 reg, u32 val) { mmio_w(s, PAULA_BASE + PA_VOICE0 + v * 0x40 + reg, val); }
static void aram16(Soc& s, u32 off, const std::vector<int16_t>& d) { for (size_t i = 0; i < d.size(); i++) wbe16(&s.bus.aram[off + i * 2], (u16)d[i]); }
static int crossings(const std::vector<i16>& pcm, int ch, size_t from, size_t to) {
  int n = 0; for (size_t i = from + 1; i < to; i++) if ((pcm[(i - 1) * 2 + ch] < 0) != (pcm[i * 2 + ch] < 0)) n++; return n;
}
static std::vector<int16_t> sine(int n, int period, int amp) { std::vector<int16_t> v(n); for (int i = 0; i < n; i++) v[i] = (int16_t)std::lround(amp * std::sin(2 * M_PI * i / period)); return v; }
static int peak(const std::vector<i16>& p, int ch, size_t from = 0, size_t to = ~0ull) { int m = 0; to = std::min(to, p.size() / 2); for (size_t i = from; i < to; i++) m = std::max(m, std::abs((int)p[i * 2 + ch])); return m; }

TEST(paula_pcm8_pitch_and_resampling) {
  Soc s; for (int i = 0; i < 48; i++) s.bus.aram[i] = (u8)(i8)std::lround(100 * std::sin(2 * M_PI * i / 48));
  pv(s, 0, PV_ADDR, 0); pv(s, 0, PV_LEN, 48); pv(s, 0, PV_STEP, 0x10000); pv(s, 0, PV_CTRL, 1 | 2 | (0 << 2));
  s.paula.render(4800);       // 100 ms de 1 kHz
  int c = crossings(s.paula.pcm, 0, 0, 4800); CHECK(c >= 198 && c <= 200);
  CHECK(std::abs(peak(s.paula.pcm, 0) - (int)(100.0 / 128 * 32767)) < 700);
  CHECK_EQ(s.paula.pcm[0], s.paula.pcm[1]);                                        // vol L = R = 256
  s.paula.pcm.clear(); pv(s, 0, PV_STEP, 0x8000); pv(s, 0, PV_CTRL, 0); pv(s, 0, PV_CTRL, 1 | 2);   // uma oitava abaixo
  s.paula.render(4800); c = crossings(s.paula.pcm, 0, 0, 4800); CHECK(c >= 98 && c <= 101);
  s.paula.pcm.clear(); pv(s, 0, PV_STEP, 0x20000); pv(s, 0, PV_CTRL, 0); pv(s, 0, PV_CTRL, 1 | 2);   // uma oitava acima
  s.paula.render(4800); c = crossings(s.paula.pcm, 0, 0, 4800); CHECK(c >= 396 && c <= 401);
}

TEST(paula_pcm16_oneshot_and_loop_points) {
  Soc s; std::vector<int16_t> d(100, 12000); aram16(s, 0x1000, d);
  pv(s, 1, PV_ADDR, 0x1000); pv(s, 1, PV_LEN, 100); pv(s, 1, PV_CTRL, 1 | (1 << 2)); pv(s, 1, PV_VOL_R, 0);
  s.paula.render(200);
  CHECK(std::abs(s.paula.pcm[50 * 2] - 12000) <= 1); CHECK_EQ(s.paula.pcm[50 * 2 + 1], 0);          // só esquerda
  CHECK_EQ(s.paula.pcm[150 * 2], 0); CHECK_EQ(mmio_r(s, PAULA_BASE + PA_VOICE0 + 1 * 0x40 + PV_CTRL) & 1, 0);   // terminou e desligou
  // laço parcial: amostras 0-49 = 1000, 50-99 = -3000; loop em [50,100)
  for (int i = 0; i < 100; i++) d[i] = i < 50 ? 1000 : -3000; aram16(s, 0x2000, d); s.paula.pcm.clear();
  pv(s, 2, PV_ADDR, 0x2000); pv(s, 2, PV_LEN, 100); pv(s, 2, PV_LOOP_START, 50); pv(s, 2, PV_LOOP_LEN, 50); pv(s, 2, PV_CTRL, 1 | 2 | (1 << 2));
  s.paula.render(400);
  CHECK(std::abs(s.paula.pcm[10 * 2] - 1000) <= 1); CHECK(std::abs(s.paula.pcm[300 * 2] + 3000) <= 1); CHECK(std::abs(s.paula.pcm[399 * 2] + 3000) <= 1);
}

TEST(paula_adpcm_roundtrip_quality_and_4to1_size) {
  Soc s; auto pcm = sine(2400, 48, 12000); auto enc = adpcm::encode(pcm);
  CHECK_EQ(enc.size(), pcm.size() / 2); std::memcpy(&s.bus.aram[0x4000], enc.data(), enc.size());
  pv(s, 3, PV_ADDR, 0x4000); pv(s, 3, PV_LEN, 2400); pv(s, 3, PV_CTRL, 1 | (2 << 2)); pv(s, 3, PV_VOL_R, 0);
  s.paula.render(2400);
  double sig = 0, err = 0; for (int i = 0; i < 2400; i++) { double o = pcm[i], d = s.paula.pcm[i * 2] - o; sig += o * o; err += d * d; }
  double snr = 10 * std::log10(sig / std::max(err, 1.0)); CHECK(snr > 25.0);
  // laço ADPCM recomeça do início com o estado inicial
  Soc t; std::memcpy(&t.bus.aram[0], enc.data(), 100); t.paula.pcm.clear();
  mmio_w(t, PAULA_BASE + PA_VOICE0 + PV_ADDR, 0); mmio_w(t, PAULA_BASE + PA_VOICE0 + PV_LEN, 200); mmio_w(t, PAULA_BASE + PA_VOICE0 + PV_CTRL, 1 | 2 | (2 << 2));
  t.paula.render(600); for (int i = 0; i < 100; i++) CHECK_EQ(t.paula.pcm[i * 2], t.paula.pcm[(i + 200) * 2]);
}

TEST(paula_3d_positioning_pan_and_distance) {
  Soc s; std::vector<int16_t> d(64, 16000); aram16(s, 0, d);
  auto play = [&](float x, float z) { s.paula.pcm.clear(); pv(s, 4, PV_CTRL, 0); pv(s, 4, PV_ADDR, 0); pv(s, 4, PV_LEN, 64); pv(s, 4, PV_X, f2u(x)); pv(s, 4, PV_Y, f2u(0)); pv(s, 4, PV_Z, f2u(z));
    pv(s, 4, PV_CTRL, 1 | 2 | (1 << 2) | 0x10); s.paula.render(32); return std::make_pair(peak(s.paula.pcm, 0), peak(s.paula.pcm, 1)); };
  auto r = play(10, 0); CHECK(r.second > r.first * 5);              // à direita
  auto l = play(-10, 0); CHECK(l.first > l.second * 5);
  auto c = play(0, -1); CHECK(std::abs(c.first - c.second) <= 2);   // à frente: igual
  auto near_ = play(0, -1), far_ = play(0, -9);
  double ratio = (double)near_.first / far_.first; CHECK_NEAR(ratio, 5.0, 0.05);     // 1/(1+d): (1/2)/(1/10)
}

TEST(paula_reverb_adds_decaying_tail_only_when_enabled) {
  auto run = [&](u32 wet, u32 send) {
    Soc s; std::vector<int16_t> click(10, 20000); aram16(s, 0, click);
    mmio_w(s, PAULA_BASE + PA_REVERB_WET, wet);
    pv(s, 5, PV_ADDR, 0); pv(s, 5, PV_LEN, 10); pv(s, 5, PV_REV_SEND, send); pv(s, 5, PV_CTRL, 1 | (1 << 2));
    s.paula.render(30000); return s.paula.pcm;
  };
  auto dry = run(0, 256); CHECK(peak(dry, 0, 0, 10) > 1000); CHECK_EQ(peak(dry, 0, 1500, 30000), 0);   // sem reverb: silêncio após o clique
  auto wet = run(256, 256); CHECK(peak(wet, 0, 1500, 4000) > 50); CHECK(peak(wet, 1, 1500, 4000) > 50);
  CHECK(peak(wet, 0, 25000, 30000) < peak(wet, 0, 1500, 6000));                    // decai
  CHECK(wet[2000 * 2] != wet[2000 * 2 + 1] || wet[2500 * 2] != wet[2500 * 2 + 1]);   // estéreo descorrelacionado
}

TEST(paula_lowpass_attenuates_high_frequencies) {
  Soc s; std::vector<int16_t> alt(64); for (int i = 0; i < 64; i++) alt[i] = (i & 1) ? -16000 : 16000; aram16(s, 0, alt);
  pv(s, 6, PV_ADDR, 0); pv(s, 6, PV_LEN, 64); pv(s, 6, PV_CTRL, 1 | 2 | (1 << 2));
  s.paula.render(200); int open_ = peak(s.paula.pcm, 0, 50, 200); s.paula.pcm.clear();
  pv(s, 6, PV_CTRL, 0); pv(s, 6, PV_LP_COEF, 30); pv(s, 6, PV_CTRL, 1 | 2 | (1 << 2) | 0x20); s.paula.render(200);
  int filt = peak(s.paula.pcm, 0, 100, 200); CHECK(open_ > 15000); CHECK(filt < open_ / 3);
}

TEST(paula_legacy_amiga_channels_period_and_hard_pan) {
  Soc s; for (int i = 0; i < 16; i++) s.bus.aram[0x100 + i] = (u8)(i < 8 ? 100 : -100);      // onda quadrada de 16 amostras
  auto leg = [&](int c, u32 off, u32 v) { mmio_w(s, PAULA_BASE + PA_LEG0 + c * 0x10 + off, v); };
  leg(0, 0, 0x100); leg(0, 4, 8); leg(0, 8, 428); leg(0, 12, 0x80000000u | 0x40000000u | 64);
  s.paula.render(4800);
  CHECK(peak(s.paula.pcm, 0) > 5000); CHECK_EQ(peak(s.paula.pcm, 1), 0);                      // canal 0 -> esquerda
  double f = 3546895.0 / 428 / 16; int c = crossings(s.paula.pcm, 0, 0, 4800); CHECK_NEAR(c, 2 * f * 0.1, 3);   // ~83 cruzamentos em 100 ms
  Soc t; for (int i = 0; i < 16; i++) t.bus.aram[i] = 100;
  mmio_w(t, PAULA_BASE + PA_LEG0 + 0x10, 0); mmio_w(t, PAULA_BASE + PA_LEG0 + 0x14, 8); mmio_w(t, PAULA_BASE + PA_LEG0 + 0x18, 200); mmio_w(t, PAULA_BASE + PA_LEG0 + 0x1C, 0x80000000u | 64);
  t.paula.render(2000); CHECK(peak(t.paula.pcm, 1) > 5000); CHECK_EQ(peak(t.paula.pcm, 0), 0);      // canal 1 -> direita
  CHECK_EQ(mmio_r(t, PAULA_BASE + PA_LEG0 + 0x1C) & 0x80000000u, 0);                           // sem laço: parou ao fim de 16 bytes
}

TEST(paula_event_sequencer_is_sample_accurate) {
  Soc s; std::vector<int16_t> d(1000, 10000); aram16(s, 0, d);
  pv(s, 7, PV_ADDR, 0); pv(s, 7, PV_LEN, 1000); pv(s, 7, PV_VOL_R, 0);
  auto ev = [&](int i, u32 t, u32 voice, u32 reg, u32 val) { u32 a = 0x20000 + i * 16; s.bus.raw_w32(a, t); s.bus.raw_w32(a + 4, voice); s.bus.raw_w32(a + 8, reg); s.bus.raw_w32(a + 12, val); };
  ev(0, 100, 7, PV_CTRL, 1 | (1 << 2) | 2); ev(1, 300, 7, PV_CTRL, 0); ev(2, 400, 7, PV_CTRL, 1 | (1 << 2) | 2); ev(3, 500, 0xFFFFFFFF, PA_MASTER, 128);
  mmio_w(s, PAULA_BASE + PA_SEQ_ADDR, 0x20000); mmio_w(s, PAULA_BASE + PA_SEQ_LEN, 4); mmio_w(s, PAULA_BASE + PA_SEQ_CTRL, 1);
  s.bus.irq_mask = IRQ_AUDIO; s.paula.render(700);
  auto& p = s.paula.pcm;
  CHECK_EQ(p[99 * 2], 0); CHECK(std::abs(p[100 * 2] - 10000) <= 1); CHECK(std::abs(p[299 * 2] - 10000) <= 1); CHECK_EQ(p[300 * 2], 0); CHECK_EQ(p[399 * 2], 0);
  CHECK(std::abs(p[400 * 2] - 10000) <= 1); CHECK(std::abs(p[499 * 2] - 10000) <= 1); CHECK(std::abs(p[500 * 2] - 5000) <= 1);   // master 128 = metade
  CHECK(s.bus.irq_pending()); CHECK_EQ(mmio_r(s, PAULA_BASE + PA_SEQ_STATUS), 0);
}

TEST(paula_master_clipping_and_time_base) {
  Soc s; std::vector<int16_t> d(100, 30000); aram16(s, 0, d);
  for (int v = 0; v < 2; v++) { pv(s, v, PV_ADDR, 0); pv(s, v, PV_LEN, 100); pv(s, v, PV_CTRL, 1 | 2 | (1 << 2)); }
  s.paula.render(10); CHECK_EQ(s.paula.pcm[8], 32767);                           // soma 60000 -> satura
  mmio_w(s, PAULA_BASE + PA_MASTER, 64); s.paula.render(10); CHECK(std::abs(s.paula.pcm[20 * 2 - 2] - 15000) <= 2);
  Soc t; t.paula.render_until(1000000000ull); CHECK_EQ(t.paula.frames_rendered, 48);        // 1 ms = 48 amostras
  t.paula.render_until(1000000000ull); CHECK_EQ(t.paula.frames_rendered, 48);
  t.paula.render_until(FRAME_PS); CHECK_EQ(t.paula.frames_rendered, 800);                   // 1/60 s = 800 amostras
  std::vector<i16> st(10, 0); CHECK(write_wav("/tmp/_sf_test.wav", st));
}
