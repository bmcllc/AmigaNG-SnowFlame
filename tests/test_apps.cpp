#include "helpers.h"
#include "snowcc.h"
#include "sffs.h"
#include <fstream>

// ======================= Aplicativos do disco do sistema: menu (Dashboard) e SnowRoids =======================
static std::string slurp_s(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }

static CcResult g_game;                                                            // símbolos do SnowRoids (para os testes cutucarem o estado)
static std::vector<u8> build_system_dvd() {
  CcOptions opt; opt.include_dirs = {"os", "os/include", "os/apps"};
  std::vector<SffsFile> files; std::map<std::string, u32> fl;
  for (auto& a : std::vector<std::pair<std::string, std::string>>{{"boot/menu.bin", "os/apps/menu.c"}, {"games/snowroids.bin", "os/apps/games/snowroids.c"}}) {
    CcResult r = cc_compile(slurp_s(a.second), a.second, opt);
    if (!r.ok) { std::printf("    erro de compilação (%s): %s\n", a.second.c_str(), r.error.c_str()); return {}; }
    files.push_back({a.first, r.image}); fl[a.first] = r.entry; if (a.first == "games/snowroids.bin") g_game = r;
  }
  for (const char* n : {"font.raw", "font.adv", "logo.raw"}) { std::string d = slurp_s(std::string("assets/") + n); files.push_back({std::string("sys/") + n, std::vector<u8>(d.begin(), d.end())}); }
  files.push_back({"sys/icons.raw", std::vector<u8>(65536, 0)});
  std::vector<u8> disc; sffs_build(files, disc, nullptr, &fl); return disc;
}
static int lit_pixels(const std::vector<u32>& f) { int n = 0; for (u32 p : f) if (p & 0xFFFFFF) n++; return n; }

TEST(system_dvd_menu_boots_counts_boots_in_memory_card_and_game_runs) {
  auto disc = build_system_dvd(); CHECK(!disc.empty());
  std::vector<u8> card;
  for (int run = 1; run <= 2; run++) {                                             // o contador de boots no cartão sobrevive ao reinício
    Soc s; s.dvd.insert(disc); if (!card.empty()) s.mcard.data = card;
    CHECK(s.boot_dvd()); s.video.start(); s.run_frames(110);
    CHECK(s.console.out.find("SnowFlame Dashboard iniciado") != std::string::npos);
    char want[64]; std::snprintf(want, sizeof want, "boots no cartao: %d)", run); CHECK(s.console.out.find(want) != std::string::npos);
    CHECK(s.console.out.find("encerrada") == std::string::npos);
    CHECK_EQ(s.video.width(), 640); CHECK(lit_pixels(s.video.frame) > 50000);
    card = s.mcard.data;
  }
  Soc g; g.dvd.insert(disc); CHECK(g.boot_dvd("games/snowroids.bin")); g.video.start(); g.run_frames(100);
  g.hid.key_event(0x28, true); g.hid.key_event(0x28, false); g.run_frames(60);          // Enter inicia a partida
  CHECK(g.console.out.find("[snowroids] iniciado") != std::string::npos); CHECK(g.console.out.find("encerrada") == std::string::npos);
  CHECK(lit_pixels(g.video.frame) > 20000);
  g.hid.key_event(0x29, true); g.hid.key_event(0x29, false); g.run_frames(40);          // Esc devolve ao menu pelo IPL
  CHECK_EQ(g.boots, 2u); CHECK(g.last_boot == "boot/menu.bin");
}

TEST(snowroids_game_over_saves_high_score_to_memory_card) {
  auto disc = build_system_dvd(); CHECK(!disc.empty());
  auto sym = [&](const char* n) { auto it = g_game.symbols.find(n); CHECK(it != g_game.symbols.end()); return it == g_game.symbols.end() ? 0u : it->second; };
  Soc g; g.dvd.insert(disc); CHECK(g.boot_dvd("games/snowroids.bin")); g.video.start(); g.run_frames(100);
  g.hid.key_event(0x28, true); g.hid.key_event(0x28, false); g.run_frames(30);
  g.bus.cache_flush_all(true);                                                         // a CPU escreve pela D-cache: descarrega antes de cutucar a memória
  u32 lives = sym("lives"), score = sym("score"), sinv = sym("sinv"), sx = sym("sx"), sy = sym("sy"), ast = sym("ast");
  CHECK_EQ(g.bus.raw_r32(lives), 3u);
  g.bus.raw_w32(lives, 1); g.bus.raw_w32(score, 777); g.bus.raw_w32(sinv, 0); g.bus.raw_w32(sym("hiscore"), 777); g.bus.raw_w32(sym("hi_new"), 1);   // como se add_score tivesse batido o recorde
  g.bus.raw_w32(ast + 4, g.bus.raw_r32(sx)); g.bus.raw_w32(ast + 8, g.bus.raw_r32(sy));   // um asteroide em cima da nave (struct Ast: on, x, y, ...)
  g.bus.raw_w32(ast + 0, 1); g.bus.raw_w32(ast + 28, 3); g.bus.raw_w32(ast + 32, 46);
  g.run_frames(40);
  g.bus.cache_flush_all(false);
  CHECK_EQ(g.bus.raw_r32(lives), 0u);
  CHECK_EQ(g.mcard.data[8 * 512 + 0], 'S'); CHECK_EQ(g.mcard.data[8 * 512 + 3], 'H');                                   // 'SRHS' no setor 8
  u32 hi = ((u32)g.mcard.data[8 * 512 + 4] << 24) | ((u32)g.mcard.data[8 * 512 + 5] << 16) | ((u32)g.mcard.data[8 * 512 + 6] << 8) | g.mcard.data[8 * 512 + 7];
  CHECK_EQ(hi, 777u);
  CHECK(g.console.out.find("encerrada") == std::string::npos);
}
