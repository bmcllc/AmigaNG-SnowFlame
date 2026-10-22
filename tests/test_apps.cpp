#include "helpers.h"
#include "snowcc.h"
#include "sffs.h"
#include "sysassets.h"
#include <fstream>

// ======================= Aplicativos do disco do sistema: menu (Dashboard) e SnowRoids =======================
static std::string slurp_s(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }

static CcResult g_game, g_menu;                                                    // símbolos do SnowRoids e do menu (para os testes cutucarem o estado)
// 'extra' soma arquivos ao disco (jogos de mentira, dados...); sys/games.inf dá título e descrição ao SnowRoids.
static std::vector<u8> build_system_dvd(const std::vector<SffsFile>& extra = {}) {
  CcOptions opt; opt.include_dirs = {"os", "os/include", "os/apps"};
  std::vector<SffsFile> files; std::map<std::string, u32> fl;
  for (auto& a : std::vector<std::pair<std::string, std::string>>{{"boot/menu.bin", "os/apps/menu.c"}, {"games/snowroids.bin", "os/apps/games/snowroids.c"}}) {
    CcResult r = cc_compile(slurp_s(a.second), a.second, opt);
    if (!r.ok) { std::printf("    erro de compilação (%s): %s\n", a.second.c_str(), r.error.c_str()); return {}; }
    files.push_back({a.first, r.image}); fl[a.first] = r.entry; if (a.first == "games/snowroids.bin") g_game = r; else g_menu = r;
  }
  for (auto& f : sysassets::system_files()) files.push_back(f);
  const std::string inf = "snowroids|SnowRoids|An original neon vector shooter\n"; files.push_back({"sys/games.inf", std::vector<u8>(inf.begin(), inf.end())});
  for (auto& f : extra) files.push_back(f);
  std::vector<u8> disc; sffs_build(files, disc, nullptr, &fl); return disc;
}
// Variáveis do menu (estáticas do menu.c) lidas da DDR; a D-cache da CPU é write-back, então descarrega antes.
static u32 mget(Soc& s, const char* name, u32 idx = 0) {
  auto it = g_menu.symbols.find(name); CHECK(it != g_menu.symbols.end()); if (it == g_menu.symbols.end()) return 0;
  s.bus.cache_flush_all(false); return s.bus.raw_r32(it->second + idx * 4);
}
static std::string mstr(Soc& s, const char* name, u32 idx, u32 stride) {
  auto it = g_menu.symbols.find(name); CHECK(it != g_menu.symbols.end()); std::string r; if (it == g_menu.symbols.end()) return r;
  s.bus.cache_flush_all(false); for (u32 a = it->second + idx * stride; s.bus.raw_r8(a); a++) r += (char)s.bus.raw_r8(a);
  return r;
}
static u32 card_w(const std::vector<u8>& d, size_t off) { return ((u32)d[off] << 24) | ((u32)d[off + 1] << 16) | ((u32)d[off + 2] << 8) | d[off + 3]; }
static void card_put(std::vector<u8>& d, size_t off, u32 v) { for (int k = 0; k < 4; k++) d[off + k] = (u8)(v >> (24 - 8 * k)); }
enum { KUP = 0x52, KDOWN = 0x51, KLEFT = 0x50, KRIGHT = 0x4F, KENTER = 0x28, KESC = 0x29, KPGDN = 0x4E };
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

// O Dashboard usa o pipeline da especificação: VPU-A (geometria) e VPU-B (partículas) por filas, DMA local e semáforos,
// FlameCore numa camada pré-multiplicada, HUD pelo Blitter, céu pelo Copper e pad pelo sequenciador da SnowPaula.
// A navegação pelos painéis também cobre a corrida entre a falta de TLB e a troca de tarefas no kernel.
TEST(dashboard_drives_every_chip_and_survives_panel_navigation) {
  auto disc = build_system_dvd(); CHECK(!disc.empty());
  Soc s; s.dvd.insert(disc); CHECK(s.boot_dvd()); s.video.start();
  auto key = [&](u8 k) { s.hid.key_event(k, true); s.hid.key_event(k, false); };
  s.run_frames(90); key(0x28);                                                         // pula a abertura
  s.run_frames(20);
  for (u8 k : {0x51, 0x51, 0x28}) { key(k); s.run_frames(8); }                        // SYSTEM
  s.run_frames(10); key(0x29); s.run_frames(8);
  for (u8 k : {0x51, 0x51, 0x28, 0x51}) { key(k); s.run_frames(8); }                  // SETTINGS, desce até "Ambient music"
  s.run_frames(16);
  CHECK(s.console.out.find("encerrada") == std::string::npos);
  CHECK(s.vpuA.jobs_done > 100); CHECK(s.vpuB.jobs_done > 100); CHECK(s.vpuA.signals == s.vpuA.jobs_done); CHECK(s.vpuB.signals > 100);
  CHECK(s.vpuA.dma_bytes > 1000000); CHECK(s.vpuB.dma_bytes > 100000);
  CHECK(s.blt.sprites_drawn > 1000);
  CHECK(s.video.layer[0].flags & LF_PREMUL); CHECK(s.video.layer[1].flags & LF_ENABLE);
  CHECK_EQ(s.bus.cpu_read(PAULA_BASE + PA_SEQ_STATUS, 4), 1u);
  int orange = 0, ice = 0;
  for (u32 p : s.video.frame) { int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255; if (r > 200 && g > 70 && g < 170 && b < 90) orange++; if (b > 200 && g > 160 && r < 190) ice++; }
  CHECK(orange > 300); CHECK(ice > 300);                                               // brasa (seleção/anel) e gelo (cristal)
  CHECK(lit_pixels(s.video.frame) > 250000);                                           // o céu do Copper preenche o fundo
}

// Ajustes e último jogo vão para o SnowCard (bloco 0, setor 1); voltar de um jogo pelo IPL não repete a abertura e reabre GAMES nele.
TEST(dashboard_keeps_settings_and_last_game_on_the_snowcard_and_returns_from_a_game_to_games) {
  auto disc = build_system_dvd(); CHECK(!disc.empty());
  std::vector<u8> card;
  {
    Soc s; s.dvd.insert(disc); CHECK(s.boot_dvd()); s.video.start();
    auto key = [&](u8 k, int n = 8) { s.hid.key_event(k, true); s.hid.key_event(k, false); s.run_frames(n); };
    s.run_frames(90); key(KENTER, 20);                                                 // pula a abertura
    for (int i = 0; i < 4; i++) key(KDOWN);
    key(KENTER, 24);                                                                   // SETTINGS
    key(KLEFT);                                                                        // volume 200 -> 184
    for (int i = 0; i < 3; i++) key(KDOWN);
    key(KRIGHT);                                                                       // Effects High -> Low
    key(KDOWN); key(KENTER);                                                           // Boot intro -> Off
    s.run_frames(120);                                                                 // o bloco é gravado 1,5 s depois da última alteração
    CHECK_EQ(card_w(s.mcard.data, 0), 0x53464D43u); CHECK_EQ(card_w(s.mcard.data, 4), 1u);        // 'SFMC' e o contador de boots seguem lá
    CHECK_EQ(card_w(s.mcard.data, 512), 0x53465354u); CHECK_EQ(card_w(s.mcard.data, 516), 1u);     // 'SFST' versão 1
    CHECK_EQ(card_w(s.mcard.data, 520), 184u); CHECK_EQ(card_w(s.mcard.data, 524), 1u); CHECK_EQ(card_w(s.mcard.data, 528), 1u); CHECK_EQ(card_w(s.mcard.data, 532), 0u); CHECK_EQ(card_w(s.mcard.data, 536), 1u);
    CHECK_EQ(mget(s, "fx_level"), 1u); CHECK_EQ(mget(s, "n_snow"), 36u); CHECK_EQ(mget(s, "n_emb"), 20u);
    key(KESC);                                                                         // volta ao menu
    for (int i = 0; i < 4; i++) key(KUP);
    key(KENTER, 24); CHECK_EQ(mget(s, "panel_item"), 0u); CHECK_EQ(mget(s, "ngames"), 1u);
    CHECK(mstr(s, "gtitle", 0, 28) == "SnowRoids"); CHECK(mstr(s, "gdesc", 0, 44) == "An original neon vector shooter"); CHECK_EQ(mget(s, "gsize", 0), (u32)(g_game.image.size()));
    key(KENTER, 30);                                                                   // joga o SnowRoids
    CHECK_EQ(s.boots, 2u); CHECK(s.last_boot == "games/snowroids.bin");
    CHECK(std::string((const char*)&s.mcard.data[544]) == "snowroids");            // último jogo gravado junto com os ajustes
    s.run_frames(100);
    s.hid.key_event(KESC, true); s.hid.key_event(KESC, false); s.run_frames(40);       // o jogo devolve ao menu pelo IPL
    CHECK_EQ(s.boots, 3u); CHECK(s.last_boot == "boot/menu.bin");
    s.run_frames(180);
    CHECK(s.console.out.find("retorno de jogo; ajustes: volume 184 musica 1 efeitos 1 abertura 0; 1 jogos") != std::string::npos);
    CHECK_EQ(mget(s, "state"), 2u); CHECK_EQ(mget(s, "panel_item"), 0u); CHECK_EQ(mget(s, "boot_logo"), 0u);   // ST_PANEL em GAMES, sem o logotipo da abertura
    CHECK(s.console.out.find("encerrada") == std::string::npos);
    card = s.mcard.data;
  }
  Soc c; c.dvd.insert(disc); c.mcard.data = card; CHECK(c.boot_dvd()); c.video.start(); c.run_frames(120);   // partida a frio com o mesmo cartão
  CHECK(c.console.out.find("partida a frio; ajustes: volume 184 musica 1 efeitos 1 abertura 0; 1 jogos") != std::string::npos);
  CHECK_EQ(mget(c, "boot_logo"), 0u);                                                  // "Boot intro: Off" vale também a frio
  c.bus.cache_flush_all(true);
  CHECK_EQ(c.bus.cpu_read(PAULA_BASE + PA_MASTER, 4), 184u);                           // o volume gravado volta ao mestre da SnowPaula
}

// Gerenciador do cartão: lista os saves por bloco (varredura do 1º setor), protege o do sistema e apaga o escolhido só depois da confirmação.
TEST(dashboard_memory_card_manager_lists_saves_and_deletes_one_after_confirmation) {
  auto disc = build_system_dvd(); CHECK(!disc.empty());
  Soc s; s.dvd.insert(disc); s.mcard.data.assign(s.mcard.data.size(), 0xFF);
  card_put(s.mcard.data, 8 * 512, 0x53525348u); card_put(s.mcard.data, 8 * 512 + 4, 4820);       // recorde do SnowRoids (bloco 1)
  card_put(s.mcard.data, 40 * 512, 0x58595A57u); card_put(s.mcard.data, 40 * 512 + 4, 7);        // dados de outro dono (bloco 5)
  CHECK(s.boot_dvd()); s.video.start();
  auto key = [&](u8 k, int n = 8) { s.hid.key_event(k, true); s.hid.key_event(k, false); s.run_frames(n); };
  s.run_frames(90); key(KENTER, 20);
  key(KDOWN); key(KENTER, 70);                                                         // MEMORY CARD; a varredura (8 blocos por quadro) já terminou
  CHECK_EQ(mget(s, "mc_n"), 3u); CHECK_EQ(mget(s, "mc_used"), 3u); CHECK_EQ(mget(s, "mc_hi"), 4820u);
  CHECK_EQ(mget(s, "mc_list", 0), 0u); CHECK_EQ(mget(s, "mc_list", 1), 1u); CHECK_EQ(mget(s, "mc_list", 2), 5u);
  key(KENTER); CHECK_EQ(mget(s, "dlg"), 0u);                                           // o bloco do sistema não se apaga
  key(KDOWN); key(KENTER, 30); CHECK_EQ(mget(s, "dlg"), 1u);                           // SnowRoids: pede confirmação, e começa em "Cancel"
  key(KENTER, 30); CHECK_EQ(mget(s, "dlg"), 0u); CHECK_EQ(card_w(s.mcard.data, 8 * 512), 0x53525348u);      // cancelar não apaga
  key(KENTER, 30); key(KRIGHT); key(KENTER, 30);                                       // confirma "Delete"
  CHECK_EQ(mget(s, "dlg"), 0u); CHECK_EQ(mget(s, "mc_n"), 2u); CHECK_EQ(mget(s, "mc_used"), 2u); CHECK_EQ(mget(s, "mc_hi"), 0xFFFFFFFFu);
  for (int i = 0; i < 8 * 512; i += 4) CHECK_EQ(card_w(s.mcard.data, 8 * 512 + i), 0xFFFFFFFFu);  // o bloco 1 inteiro foi apagado...
  CHECK_EQ(card_w(s.mcard.data, 40 * 512), 0x58595A57u); CHECK_EQ(card_w(s.mcard.data, 0), 0x53464D43u);  // ...e os outros ficaram como estavam
  CHECK(s.console.out.find("encerrada") == std::string::npos);
}

// Listas longas: GAMES rola (até 24 jogos), DISC rola com todos os arquivos, e segurar o direcional repete (25 quadros, depois a cada 5).
TEST(dashboard_scrolls_long_lists_and_repeats_held_directions) {
  std::vector<SffsFile> extra; std::vector<u8> dummy(300, 0x4E);
  for (int i = 0; i < 12; i++) { char n[32]; std::snprintf(n, sizeof n, "games/extra%02d.bin", i); extra.push_back({n, dummy}); }
  for (int i = 0; i < 20; i++) { char n[32]; std::snprintf(n, sizeof n, "data/file%02d.dat", i); extra.push_back({n, dummy}); }
  auto disc = build_system_dvd(extra); CHECK(!disc.empty());
  Soc s; s.dvd.insert(disc); s.pads.pad[0].connected = true; CHECK(s.boot_dvd()); s.video.start();
  auto key = [&](u8 k, int n = 8) { s.hid.key_event(k, true); s.hid.key_event(k, false); s.run_frames(n); };
  s.run_frames(90); key(KENTER, 20);
  key(KENTER, 24); CHECK_EQ(mget(s, "ngames"), 13u); CHECK(mstr(s, "gtitle", 1, 28) == "Extra00");
  s.pads.pad[0].buttons = BTN_DOWN; s.run_frames(60);                                  // 1 passo + repetições a partir do quadro 25: ~8 linhas
  s.pads.pad[0].buttons = 0; s.run_frames(8);
  u32 sel = mget(s, "list_sel"); CHECK(sel >= 6 && sel <= 12); CHECK(mget(s, "list_top") > 0);
  s.pads.pad[0].buttons = BTN_DOWN; s.run_frames(120); s.pads.pad[0].buttons = 0; s.run_frames(8);
  CHECK_EQ(mget(s, "list_sel"), 12u); CHECK_EQ(mget(s, "list_top"), 6u);               // 13 jogos, 7 linhas: a janela termina no último
  key(KESC); for (int i = 0; i < 3; i++) key(KDOWN);
  key(KENTER, 24); CHECK_EQ(mget(s, "panel_item"), 3u);                                // DISC: 12 arquivos do sistema + 12 jogos + 20 de dados
  key(KPGDN); key(KPGDN); CHECK(mget(s, "dsel") >= 20); CHECK(mget(s, "dtop") > 0);   // PgDn anda uma página
  key(KESC); key(KESC, 20); CHECK_EQ(mget(s, "panel_item"), 5u);                        // B no menu abre a ajuda (INFO)
  CHECK(s.console.out.find("encerrada") == std::string::npos);
}

// "Effects: Off" desliga a neve e as brasas: a SnowVector-B (partículas) fica parada; a A (emblema) segue.
TEST(dashboard_effects_off_leaves_the_second_vector_unit_idle) {
  auto disc = build_system_dvd(); CHECK(!disc.empty());
  Soc s; s.dvd.insert(disc); s.mcard.data.assign(s.mcard.data.size(), 0xFF);
  card_put(s.mcard.data, 0, 0x53464D43u); card_put(s.mcard.data, 4, 0);                           // sistema + ajustes: volume 200, música ligada, Effects Off
  card_put(s.mcard.data, 512, 0x53465354u); card_put(s.mcard.data, 516, 1); card_put(s.mcard.data, 520, 200); card_put(s.mcard.data, 524, 1); card_put(s.mcard.data, 528, 2); card_put(s.mcard.data, 532, 1);
  CHECK(s.boot_dvd()); s.video.start(); s.run_frames(240);
  CHECK(s.vpuA.jobs_done > 100); CHECK_EQ(s.vpuB.jobs_done, 0u);
  CHECK(s.console.out.find("encerrada") == std::string::npos);
}
