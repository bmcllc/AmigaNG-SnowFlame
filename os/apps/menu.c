/* menu.c - SnowFlame Dashboard: menu principal do SF-100 (boot/menu.bin no DVD do sistema).
 * Carrossel de ícones sobre ondas animadas, flocos de neve e um cubo translúcido; painéis de jogos, cartão de memória, sistema, disco e opções.
 * Tudo é desenhado pelo FlameCore (SnowGX) em 640x480 (480p) com buffer duplo; texto por atlas A8 com larguras proporcionais.
 * Dados do disco: sys/font.raw, sys/font.adv, sys/logo.raw, sys/icons.raw. Jogos: games/NOME.bin (iniciados por os_reboot). */
#include "snowos_all.c"

#include "ui.h"

#define A_LOGO  0x01410000
#define A_ICONS 0x01420000
#define C_WHITE 0xFFFFFFFFu
#define N_ITEMS 5

static int tick, sel, scroll_f, state, panel_item, list_sel, vol = 200, fade, boot_t, boot_count, mc_ok;
static unsigned keys_prev;
static int flake_x[48], flake_y[48], flake_s[48];
static char gname[8][24]; static int ngames;
static short tone_tick[256], tone_ok[1024];
static int rt_cur;

enum { ST_BOOT, ST_MENU, ST_PANEL };
static const char *item_name[N_ITEMS] = { "GAMES", "MEMORY CARD", "SYSTEM", "DISC", "SETTINGS" };
static const char *item_desc[N_ITEMS] = { "Launch a game from the disc", "Saved data and boot counter", "Hardware and OS information", "Contents of the inserted disc", "Volume and sound test" };

/* ------------------------------ entrada ------------------------------ */
#define K_LEFT 1
#define K_RIGHT 2
#define K_UP 4
#define K_DOWN 8
#define K_OK 16
#define K_BACK 32
static unsigned read_keys(void) {
  unsigned k = 0, b = REG(PAD_BASE + PAD_PORT0); int ev;
  if (b & BTN_LEFT) k |= K_LEFT; if (b & BTN_RIGHT) k |= K_RIGHT; if (b & BTN_UP) k |= K_UP; if (b & BTN_DOWN) k |= K_DOWN;
  if (b & (BTN_A | BTN_START)) k |= K_OK; if (b & BTN_B) k |= K_BACK;
  while ((ev = in_key_poll()) >= 0) {                         /* teclado: eventos de pressão viram toques de uma amostra */
    int code = ev & 0xFF;
    if (ev & KEY_UP) continue;
    if (code == 0x50) k |= K_LEFT; else if (code == 0x4F) k |= K_RIGHT; else if (code == 0x52) k |= K_UP; else if (code == 0x51) k |= K_DOWN;
    else if (code == 0x28 || code == 0x2C) k |= K_OK; else if (code == 0x29 || code == 0x2A) k |= K_BACK;
  }
  return k;
}

/* ------------------------------ som ------------------------------ */
static void beep(int v, int fmt_ok) {
  if (fmt_ok) snd_voice_play(v, 0x2000, 1024, SND_PCM16, 0x10000 * 3 / 2, vol, vol, 0);
  else snd_voice_play(v, 0, 256, SND_PCM16, 0x10000 * 2, vol / 2, vol / 2, 0);
}

/* ------------------------------ cartão de memória ------------------------------ */
static void mc_boot_counter(void) {
  unsigned *b = (unsigned *)A_MC; int i;
  mc_ok = (REG(MCARD_BASE + MC_STATUS) & 8) != 0;
  if (!mc_ok) return;
  os_cache_flush(A_MC, 512); os_cache_invalidate(A_MC, 512);
  if (mc_do(1, 0, 1, A_MC)) { mc_ok = 0; return; }
  if (b[0] == 0x53464D43u) boot_count = (int)b[1] + 1; else boot_count = 1;
  for (i = 0; i < 128; i++) b[i] = 0xFFFFFFFFu;
  b[0] = 0x53464D43u; b[1] = (unsigned)boot_count;
  os_cache_flush(A_MC, 512);
  if (mc_do(3, 0, 0, A_MC) || mc_do(2, 0, 1, A_MC)) mc_ok = 0;
}

/* ------------------------------ cena: fundo ------------------------------ */
static void draw_background_waves(void) {
  int i, k;
  for (k = 0; k < 3; k++) {                                   /* três faixas de onda */
    int base = 250 + k * 55, amp = 22 + k * 8, col = k == 0 ? 0x3060FF : k == 1 ? 0x9040FF : 0x40C0FF;
    for (i = 0; i < 40; i++) {
      int x0 = i * 16, x1 = x0 + 16;
      int y0 = base + (isin(x0 * 2 + tick * (5 + k * 3) + k * 200) >> 11) * amp / 32, y1 = base + (isin(x1 * 2 + tick * (5 + k * 3) + k * 200) >> 11) * amp / 32;
      unsigned top = with_alpha(col, 70 - k * 12), bot = with_alpha(col, 0);
      if (vn + 6 > MAXV) break;
      vtx(x0 * 4, y0 * 4, 0, top, 0, 0); vtx(x1 * 4, y1 * 4, 0, top, 0, 0); vtx(x1 * 4, (y1 + 90) * 4, 0, bot, 0, 0);
      vtx(x0 * 4, y0 * 4, 0, top, 0, 0); vtx(x1 * 4, (y1 + 90) * 4, 0, bot, 0, 0); vtx(x0 * 4, (y0 + 90) * 4, 0, bot, 0, 0);
    }
  }
}
static void draw_snow(void) {
  int i;
  for (i = 0; i < 48; i++) {
    int x = flake_x[i] + (isin(tick * 3 + i * 97) >> 11), s = flake_s[i];
    quadc(x * 4, flake_y[i] * 4, (x + s) * 4, (flake_y[i] + s) * 4, with_alpha(0xFFFFFF, 60 + s * 25));
    flake_y[i] += 1 + s / 2; if (flake_y[i] > SH) { flake_y[i] = -8; }
  }
}

/* ------------------------------ cubo girando (faces ordenadas por profundidade) ------------------------------ */
static const signed char cv[8][3] = { {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1}, {-1,-1,1}, {1,-1,1}, {1,1,1}, {-1,1,1} };
static const signed char cf[6][4] = { {0,1,2,3}, {5,4,7,6}, {4,0,3,7}, {1,5,6,2}, {3,2,6,7}, {4,5,1,0} };
static const unsigned cface[6] = { 0xFF3080FFu, 0xFFC040FFu, 0xFF40E0FFu, 0xFFFF6080u, 0xFF60FFA0u, 0xFFFFD050u };
static void draw_cube(int cx, int cy, int size, int alpha) {
  int px[8], py[8], pz[8], i, f, order[6], zs[6], a = tick * 3, b = tick * 2, sa = isin(a), ca = icos(a), sb = isin(b), cb = icos(b);
  for (i = 0; i < 8; i++) {
    int x = cv[i][0] * 256, y = cv[i][1] * 256, z = cv[i][2] * 256, x1, z1, y1, z2;
    x1 = (x * ca - z * sa) >> 16; z1 = (x * sa + z * ca) >> 16;             /* gira em Y */
    y1 = (y * cb - z1 * sb) >> 16; z2 = (y * sb + z1 * cb) >> 16;           /* e em X */
    px[i] = cx * 4 + x1 * size * 4 / 256; py[i] = cy * 4 + y1 * size * 4 / 256; pz[i] = z2 + 600;
    (void)pz;
  }
  for (f = 0; f < 6; f++) { zs[f] = pz[cf[f][0]] + pz[cf[f][1]] + pz[cf[f][2]] + pz[cf[f][3]]; order[f] = f; }
  for (i = 0; i < 5; i++) { int j; for (j = 0; j < 5 - i; j++) if (zs[order[j]] < zs[order[j + 1]]) { int t = order[j]; order[j] = order[j + 1]; order[j + 1] = t; } }
  for (f = 0; f < 6; f++) {
    int k = order[f]; unsigned c = with_alpha(cface[k], alpha), c2 = with_alpha(mix(cface[k], 0xFFFFFFFFu, 90), alpha);
    int a0 = cf[k][0], a1 = cf[k][1], a2 = cf[k][2], a3 = cf[k][3];
    if (vn + 6 > MAXV) return;
    vtx(px[a0], py[a0], 0, c, 0, 0); vtx(px[a1], py[a1], 0, c2, 0, 0); vtx(px[a2], py[a2], 0, c, 0, 0);
    vtx(px[a0], py[a0], 0, c, 0, 0); vtx(px[a2], py[a2], 0, c, 0, 0); vtx(px[a3], py[a3], 0, c2, 0, 0);
  }
}

/* ------------------------------ interface ------------------------------ */
static void draw_card(int i) {
  int d = i * 256 - scroll_f, ad = iabs(d), t = imin(ad, 512);
  int cx = SW / 2 + d * 150 / 256, scale = 100 - t * 38 / 512, alpha = 255 - t * 150 / 512, half = 56 * scale / 100, cy = 215;
  unsigned bgc = with_alpha(0x2A3C90, alpha * 150 / 255);
  if (i == sel) {                                                 /* brilho pulsante atrás do cartão selecionado */
    int g = 70 + (isin(tick * 6) >> 12);
    quadc((cx - half - 10) * 4, (cy - half - 10) * 4, (cx + half + 10) * 4, (cy + half + 10) * 4, with_alpha(0x60A0FF, g));
  }
  quad4((cx - half - 2) * 4, (cy - half - 2) * 4, (cx + half + 2) * 4, (cy + half + 2) * 4, with_alpha(0xFFFFFF, alpha / 2), with_alpha(0x8090FF, alpha / 2), with_alpha(0x8090FF, alpha / 2), with_alpha(0xFFFFFF, alpha / 3), 0, 0, 0, 0);
  quad4((cx - half) * 4, (cy - half) * 4, (cx + half) * 4, (cy + half) * 4, bgc, bgc, with_alpha(0x101850, alpha * 170 / 255), with_alpha(0x101850, alpha * 170 / 255), 0, 0, 0, 0);
}
static void draw_icon(int i) {
  int d = i * 256 - scroll_f, ad = iabs(d), t = imin(ad, 512);
  int cx = SW / 2 + d * 150 / 256, scale = 100 - t * 38 / 512, alpha = 255 - t * 150 / 512, half = 44 * scale / 100, cy = 215;
  quad4((cx - half) * 4, (cy - half) * 4, (cx + half) * 4, (cy + half) * 4, with_alpha(0xFFFFFF, alpha), with_alpha(0xFFFFFF, alpha), with_alpha(0xFFFFFF, alpha), with_alpha(0xFFFFFF, alpha), i * 128, 0, i * 128 + 128, 1024);
}

static void panel_box(int x0, int y0, int x1, int y1) {
  quad4(x0 * 4, y0 * 4, x1 * 4, y1 * 4, 0xE0101C50u, 0xE0101C50u, 0xE0080C28u, 0xE0080C28u, 0, 0, 0, 0);
  quadc(x0 * 4, y0 * 4, x1 * 4, (y0 + 2) * 4, 0xC0A0C0FFu); quadc(x0 * 4, (y1 - 2) * 4, x1 * 4, y1 * 4, 0x60A0C0FFu);
}

static void panel_shapes(void) {                   /* formas sem textura do painel (ficam no segmento ALPHA, antes do texto) */
  int y = 150;
  if (panel_item == 0 && ngames > 0) quadc(110 * 4, (y + list_sel * 30 - 2) * 4, 530 * 4, (y + list_sel * 30 + 24) * 4, 0x604070FFu);
  if (panel_item == 4) {
    quadc(110 * 4, (y + 40) * 4, 520 * 4, (y + 48) * 4, 0x80405090u);
    quadc(110 * 4, (y + 40) * 4, (110 + vol * 410 / 256) * 4, (y + 48) * 4, 0xFF60A0FFu);
  }
}
static void panel_text(void) {
  char buf[64]; int y = 150, i; unsigned col = 0xFFE8F0FFu, dim = 0xFF9AA8D8u;
  text_c(SW / 2, 126, item_name[panel_item], 130, 0xFFFFFFFFu);
  if (panel_item == 0) {
    if (ngames == 0) { text_c(SW / 2, 250, "No games found on this disc", 110, dim); return; }
    for (i = 0; i < ngames; i++) {
      unsigned c = i == list_sel ? 0xFFFFFFFFu : dim;
      text(130, y + i * 30, gname[i], 130, c);
    }
    text_c(SW / 2, 380, "A / Enter: start     B / Esc: back", 90, dim);
  } else if (panel_item == 1) {
    if (!mc_ok) { text_c(SW / 2, 250, "No memory card inserted", 110, dim); return; }
    ksprintf(buf, "Capacity: %d KB   (%d sectors)", (int)(REG(MCARD_BASE + MC_CAPACITY) / 2), (int)REG(MCARD_BASE + MC_CAPACITY)); text(110, y, buf, 110, col);
    ksprintf(buf, "Slot A: SnowFlame save data"); text(110, y + 32, buf, 110, col);
    ksprintf(buf, "System boots recorded: %d", boot_count); text(110, y + 64, buf, 130, 0xFF80FFB0u);
    text(110, y + 110, "The counter lives in flash sector 0 of the card", 90, dim);
    text(110, y + 130, "and survives resets (erase + program, 4 KB blocks).", 90, dim);
  } else if (panel_item == 2) {
    ksprintf(buf, "CPU     ColdFire V4e @ 266 MHz   MMU: on"); text(110, y, buf, 100, col);
    text(110, y + 26, "VPU     2 x SnowVector-128 @ 150 MHz", 100, col);
    text(110, y + 52, "GPU     FlameCore 3D tile renderer @ 200 MHz", 100, col);
    text(110, y + 78, "AUDIO   SnowPaula DSP, 64 voices, 48 kHz", 100, col);
    text(110, y + 104, "MEMORY  64 MB DDR + 32 MB VRAM + 4 MB audio", 100, col);
    ksprintf(buf, "OS      SnowOS Native   uptime %d s", (int)(os_ticks() / 1000)); text(110, y + 130, buf, 100, col);
    ksprintf(buf, "Heap free: %d KB   Tasks: preemptive 1 ms", (int)(os_heap_free() / 1024)); text(110, y + 156, buf, 100, dim);
  } else if (panel_item == 3) {
    ksprintf(buf, "Disc: %d sectors   (%d KB)", (int)REG(DVD_BASE + DVD_CAPACITY), (int)(REG(DVD_BASE + DVD_CAPACITY) * 2)); text(110, y, buf, 110, col);
    ksprintf(buf, "Files: %d", fs_count()); text(110, y + 28, buf, 110, col);
    for (i = 0; i < fs_count() && i < 6; i++) { ksprintf(buf, "%s", fs_name(i)); text(130, y + 62 + i * 22, buf, 90, dim); }
  } else {
    ksprintf(buf, "Volume  %d%%", vol * 100 / 256); text(110, y, buf, 120, col);
    text(110, y + 70, "Left / Right: adjust       A / Enter: sound test", 90, dim);
  }
}

/* ------------------------------ renderização ------------------------------ */
static void render(unsigned rt) {
  int i;
  ui_begin();
  quad4(0, 0, SW * 4, SH * 4, 0xFF050B24u, 0xFF050B24u, 0xFF1C1250u, 0xFF3A1A5Cu, 0, 0, 0, 0);
  ui_seg(UI_ALPHA); draw_background_waves(); draw_snow(); draw_cube(SW / 2, 215, 85, state == ST_PANEL ? 24 : 46);
  ui_seg_tex(A_LOGO, 9, 6, TF_A8);
  if (state == ST_BOOT) {
    int a = imin(255, boot_t * 255 / 60); if (boot_t > 120) a = imax(0, 255 - (boot_t - 120) * 255 / 30);
    quad4(64 * 4, 190 * 4, 576 * 4, 254 * 4, with_alpha(0xFFFFFF, a), with_alpha(0xFFFFFF, a), with_alpha(0xB0C8FF, a), with_alpha(0xB0C8FF, a), 0, 0, 1024, 1024);
  } else {
    quad4(170 * 4, 14 * 4, 470 * 4, 62 * 4, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFB0C8FFu, 0xFFB0C8FFu, 0, 0, 1024, 1024);
  }
  if (state == ST_MENU) {
    ui_seg(UI_ALPHA); for (i = 0; i < N_ITEMS; i++) draw_card(i);
    ui_seg_tex(A_ICONS, 9, 6, TF_ARGB4444); for (i = 0; i < N_ITEMS; i++) draw_icon(i);
  } else if (state == ST_PANEL) {
    ui_seg(UI_ALPHA); panel_box(80, 100, 560, 420); panel_shapes();
  }
  ui_seg_tex(A_FONT, 8, 7, TF_A8);
  if (state == ST_MENU) {
    text_c(SW / 2, 310, item_name[sel], 190, 0xFFFFFFFFu);
    text_c(SW / 2, 352, item_desc[sel], 100, 0xFFB8C4F0u);
    text_c(SW / 2, 440, "Left / Right: select      A / Enter: open", 85, 0xFF8090C8u);
  } else if (state == ST_PANEL) panel_text();
  else if (boot_t > 40 && boot_t < 140) text_c(SW / 2, 270, "Amiga SF-100  -  SnowOS", 100, with_alpha(0xB8C8FF, imin(255, (boot_t - 40) * 6)));
  { char b[40]; ksprintf(b, "%d:%02d", (int)(os_ticks() / 60000), (int)((os_ticks() / 1000) % 60)); text(SW - 70, SH - 28, b, 90, 0xFF7080B8u); }
  ui_end(rt, 0xFF000000u);
}

/* ------------------------------ lógica ------------------------------ */
static void scan_games(void) {
  int i, n = fs_count(); ngames = 0;
  for (i = 0; i < n && ngames < 8; i++) {
    const char *f = fs_name(i); int len = 0, k;
    if (f[0] != 'g' || f[1] != 'a' || f[2] != 'm' || f[3] != 'e' || f[4] != 's' || f[5] != '/') continue;
    while (f[len]) len++;
    if (len < 11 || f[len - 4] != '.' || f[len - 3] != 'b') continue;
    for (k = 6; k < len - 4 && k - 6 < 23; k++) gname[ngames][k - 6] = f[k];
    gname[ngames][k - 6] = 0; ngames++;
  }
}
static void launch_game(int g) {
  char path[48]; ksprintf(path, "games/%s.bin", gname[g]);
  os_printf("[menu] iniciando %s\n", path);
  os_reboot(path);
}
static void update(unsigned nk) {
  int target = sel * 256, diff;
  if (state == ST_BOOT) {
    boot_t++;
    if (boot_t == 2) { int v; for (v = 0; v < 3; v++) snd_voice_play(v, 0x2000, 1024, SND_PCM16, 0x10000 + v * 0x4000 + (v == 2 ? 0x2000 : 0), 0, 0, 1); }
    if (boot_t < 150) { int v, g = imin(boot_t * 3, 150) * vol / 256; if (boot_t > 100) g = g * (150 - boot_t) / 50; for (v = 0; v < 3; v++) { REG(PAULA_BASE + PA_VOICE0 + v * 0x40 + PV_VOL_L) = (unsigned)g; REG(PAULA_BASE + PA_VOICE0 + v * 0x40 + PV_VOL_R) = (unsigned)g; } }
    if (boot_t >= 150 || nk) { int v; for (v = 0; v < 3; v++) snd_voice_stop(v); state = ST_MENU; }
  } else if (state == ST_MENU) {
    if ((nk & K_LEFT) && sel > 0) { sel--; beep(3, 0); }
    if ((nk & K_RIGHT) && sel < N_ITEMS - 1) { sel++; beep(3, 0); }
    if (nk & K_OK) { panel_item = sel; list_sel = 0; if (sel == 0) scan_games(); state = ST_PANEL; beep(4, 1); }
  } else {
    if (nk & K_BACK) { state = ST_MENU; beep(3, 0); }
    else if (panel_item == 0) {
      if ((nk & K_UP) && list_sel > 0) { list_sel--; beep(3, 0); }
      if ((nk & K_DOWN) && list_sel < ngames - 1) { list_sel++; beep(3, 0); }
      if ((nk & K_OK) && ngames > 0) launch_game(list_sel);
    } else if (panel_item == 4) {
      if (nk & K_LEFT) { vol = imax(0, vol - 16); snd_master(vol); beep(3, 0); }
      if (nk & K_RIGHT) { vol = imin(256, vol + 16); snd_master(vol); beep(3, 0); }
      if (nk & K_OK) { beep(4, 1); beep(5, 1); }
    }
  }
  diff = target - scroll_f;
  scroll_f += diff / 6 + (diff > 0 ? 1 : diff < 0 ? -1 : 0); if (iabs(target - scroll_f) < 2) scroll_f = target;
}

static void load_assets(void) {
  int i;
  ui_init();
  fs_load("sys/logo.raw", (void *)A_LOGO, 32768); fs_load("sys/icons.raw", (void *)A_ICONS, 65536);
  for (i = 0; i < 48; i++) { flake_x[i] = (i * 137) % SW; flake_y[i] = (i * 71) % SH; flake_s[i] = 2 + i % 3; }
  snd_sine(tone_tick, 256, 32, 11000); snd_load(0, tone_tick, sizeof tone_tick);
  snd_sine(tone_ok, 1024, 128, 9000); snd_load(0x2000, tone_ok, sizeof tone_ok);
}

void app_main(void) {
  int back = 1;
  load_assets(); mc_boot_counter();
  state = ST_BOOT;
  os_printf("[menu] SnowFlame Dashboard iniciado (boots no cartao: %d)\n", boot_count);
  for (;;) {
    unsigned k = read_keys(), nk = k & ~keys_prev; unsigned rt = back ? RT1 : RT0;
    keys_prev = k;
    update(nk); render(rt);
    gx_layer(0, rt, SW * 4, LF_ENABLE); back = !back;
    gx_wait_vblank(); tick++;
  }
}
