// Testes de integração: a demo completa (programa ColdFire + DMA + 2 VPUs + GPU + Blitter + Copper + Paula).
#include "framework.h"
#include "../demo/scene.h"
#include <set>

using namespace sf; using namespace sf::demo;

static u64 hash_frame(const std::vector<u32>& f) { u64 h = 1469598103934665603ull; for (u32 p : f) { h ^= p; h *= 1099511628211ull; } return h; }

static std::vector<u64> run_hashes(bool hsr, int n, FrameStats* last = nullptr, std::vector<i16>* pcm = nullptr) {
  SnowDemo d; d.soc.gpu.hsr = hsr; d.soc.video.start(); d.boot(); std::vector<u64> h;
  for (int f = 0; f < n; f++) { FrameStats fs = d.frame(f); if (last) *last = fs; h.push_back(hash_frame(d.soc.video.frame)); }
  if (pcm) { d.soc.sync_audio(); *pcm = d.soc.paula.pcm; }
  return h;
}

TEST(integ_demo_renders_scene_without_gpu_error) {
  SnowDemo d; d.soc.video.start(); d.boot(); FrameStats fs;
  for (int f = 0; f < 6; f++) fs = d.frame(f);
  CHECK(fs.tris > 100); CHECK(fs.covered > 1000); CHECK(fs.blits >= 2); CHECK(d.soc.blt.sprites_drawn > 10);   // limpeza do overlay + lista de sprites numa só operação
  CHECK(fs.shaded <= fs.covered);
  CHECK_EQ(d.soc.video.width(), W); CHECK_EQ(d.soc.video.height(), H);
  std::set<u32> colors(d.soc.video.frame.begin(), d.soc.video.frame.end());
  CHECK(colors.size() > 50);   // céu em degradê (Copper), cubo, esfera, sprites
}

TEST(integ_frame_time_within_budget) {
  FrameStats fs; run_hashes(true, 4, &fs);
  CHECK(fs.cpu_ps < FRAME_PS); CHECK(fs.gpu_cycles > 0); CHECK(fs.vpuA_cycles > 0); CHECK(fs.vpuB_cycles > 0);
}

TEST(integ_deterministic_replay) {
  auto a = run_hashes(true, 5), b = run_hashes(true, 5);
  CHECK(a == b); CHECK(a[0] != a[4]);   // há animação
}

TEST(integ_hsr_on_equals_off) {
  auto a = run_hashes(true, 3), b = run_hashes(false, 3);
  CHECK(a == b);
}

TEST(integ_audio_not_silent) {
  std::vector<i16> pcm; run_hashes(true, 3, nullptr, &pcm);
  long long peak = 0; for (i16 s : pcm) peak = std::max<long long>(peak, std::abs((int)s));
  CHECK(pcm.size() >= 2 * 1000); CHECK(peak > 500);
}

TEST(integ_stale_cache_requires_invalidate) {
  Soc s; s.bus.raw_w32(0x2000, 0x11111111);
  CHECK_EQ(s.bus.cpu_read(0x2000, 4), 0x11111111u);      // linha entra na cache
  s.bus.raw_w32(0x2000, 0x22222222);                     // "DMA" escreve na DDR por fora
  CHECK_EQ(s.bus.cpu_read(0x2000, 4), 0x11111111u);      // CPU ainda vê dado velho
  s.bus.cache_inval_range(0x2000, 16);
  CHECK_EQ(s.bus.cpu_read(0x2000, 4), 0x22222222u);
}

// ---- contenção de barramento ----
static Ps dma_pair_time(bool arb, int nch) {
  Soc s; s.bus.arb_enabled = arb; Ps t0 = s.sched.now; const u32 N = 64 * 1024;
  for (int c = 0; c < nch; c++) {
    u32 b = BRIDGE_BASE + BR_DMA0 + 0x20 * c;
    s.bus.cpu_write(b + DMA_SRC, 4, 0x100000 + c * 0x40000); s.bus.cpu_write(b + DMA_DST, 4, 0x800000 + c * 0x40000);
    s.bus.cpu_write(b + DMA_LEN, 4, N); s.bus.cpu_write(b + DMA_FLAGS, 4, 0); s.bus.cpu_write(b + DMA_CTRL, 4, DMAC_START_REGS);
  }
  s.wait_until([&] { for (int c = 0; c < nch; c++) if (s.bus.cpu_read(BRIDGE_BASE + BR_DMA0 + 0x20 * c + DMA_STATUS, 4) & DMAS_BUSY) return false; return true; });
  return s.sched.now - t0;
}

TEST(integ_dma_channels_contend_for_ddr) {
  Ps one = dma_pair_time(true, 1), two = dma_pair_time(true, 2), free2 = dma_pair_time(false, 2);
  CHECK(two > one * 3 / 2);        // dois canais na mesma DDR ~ serializados
  CHECK(free2 <= one + 1000);      // sem árbitro: paralelismo ideal (modelo antigo)
}

// ---- sessão completa: todos os periféricos convivendo com a demo em execução ----
TEST(integ_full_system_session) {
  SnowDemo d; Soc& s = d.soc; d.soc.video.start();
  std::vector<u8> disc(256 * DVD_SECTOR); for (size_t i = 0; i < disc.size(); i++) disc[i] = (u8)(i ^ (i >> 8));
  s.dvd.insert(disc); s.pads.pad[0].connected = true; s.pads.pad[0].buttons = BTN_START;
  d.boot();
  s.bus.cpu_write(MMIO_BASE | (TIMER_BASE & 0xFFFFFF), 4, 0);                 // acesso por CPU a MMIO funciona com a MMU desligada
  s.bus.cpu_write(TIMER_BASE + 0x00, 4, 5000); s.bus.cpu_write(TIMER_BASE + 0x04, 4, 1 | 2);
  s.bus.cpu_write(DVD_BASE + DVD_LBA, 4, 10); s.bus.cpu_write(DVD_BASE + DVD_COUNT, 4, 8); s.bus.cpu_write(DVD_BASE + DVD_DST, 4, 0x01000000); s.bus.cpu_write(DVD_BASE + DVD_CMD, 4, 1);
  for (int f = 0; f < 70; f++) d.frame(f);                                      // > 1 s: spin-up do DVD termina durante a demo
  CHECK_EQ(s.bus.cpu_read(DVD_BASE + DVD_STATUS, 4) & 3, 2);
  CHECK(std::memcmp(s.bus.raw(0x01000000, 8 * DVD_SECTOR), &disc[10 * DVD_SECTOR], 8 * DVD_SECTOR) == 0);
  CHECK(s.timers.fired[0] > 200);                                               // 5 ms periódico por > 1 s
  CHECK_EQ(s.bus.cpu_read(PAD_BASE + PAD_PORT0, 4), BTN_START);
  CHECK(s.paula.dsp_peak > 0 && s.paula.dsp_peak <= 1000); CHECK_EQ(s.paula.dsp_drops, 0);
  CHECK(s.bus.ihits > s.bus.imisses);
}
