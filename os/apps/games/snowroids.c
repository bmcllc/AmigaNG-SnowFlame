/* snowroids.c - SnowRoids: jogo original, estilo Asteroids, para o SnowOS Native do SF-100 (games/snowroids.bin).
 * Gráficos vetoriais neon (linhas = quads finos com mistura aditiva no FlameCore), física em ponto fixo (1/256 px), partículas, ondas,
 * vidas extras, recorde salvo no cartão de memória e som gerado em tempo de execução (SnowPaula).
 * Controles: girar = setas/A,D ou direcional; propulsor = seta cima/W ou botão A; tiro = espaço ou botão B; Start/P = pausa; Esc/Select = menu. */
#include "snowos_all.c"
#include "ui.h"

#define MAXA 28
#define MAXB 6
#define MAXP 110
#define NV 10
#define W256 (SW * 256)
#define H256 (SH * 256)
#define WAVE_DELAY 110

enum { S_TITLE, S_PLAY, S_PAUSE, S_OVER };
#define G_LEFT 1
#define G_RIGHT 2
#define G_THRUST 4
#define G_FIRE 8
#define G_START 16
#define G_PAUSE 32
#define G_QUIT 64

struct Ast { int on, x, y, vx, vy, ang, spin, size, r; unsigned char rad[NV]; };
struct Bul { int on, x, y, vx, vy, life; };
struct Prt { int on, x, y, vx, vy, life, maxlife; unsigned col; };

static struct Ast ast[MAXA];
static struct Bul bul[MAXB];
static struct Prt prt[MAXP];
static int state, tick, score, hiscore, lives, wave, next_extra, wave_t, over_t, hi_new;
static int sx, sy, svx, svy, sang, sdead, sinv, sfire, sthrust, spawn_wait;
static unsigned held, held_prev, kheld, kpress, seed = 12345;
static int star_x[90], star_y[90], star_p[90];
static int mc_present, rt_back = 1, flame;

static unsigned rnd(void) { seed = seed * 1664525u + 1013904223u; return seed >> 8; }
static int rrange(int lo, int hi) { return lo + (int)(rnd() % (unsigned)(hi - lo + 1)); }
static int isqrt(int v) { int r = 0, bit = 1 << 28; if (v <= 0) return 0; while (bit > v) bit >>= 2; while (bit) { if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; } return r; }
static int wrap(int v, int m) { while (v < 0) v += m; while (v >= m) v -= m; return v; }
static int dwrap(int d, int m) { if (d > m / 2) d -= m; else if (d < -m / 2) d += m; return d; }   /* menor distância num mundo que dá a volta */

/* ------------------------------ som ------------------------------ */
#define SND_BUF ((short *)0x01700000)
#define AR_PEW 0x0000
#define AR_BOOM 0x2000
#define AR_RUMBLE 0x20000
#define AR_CHIME 0x24000
static void make_sounds(void) {
  int i, ph = 0, y = 0; short *b = SND_BUF;
  for (i = 0; i < 2400; i++) {                                  /* tiro: varredura descendente */
    int step = (38 * 256) - i * 12; ph += step; b[i] = (short)(((isin(ph >> 8) >> 4) * (2400 - i)) / 2400 * 3 / 4);
  }
  snd_load(AR_PEW, b, 2400 * 2);
  for (i = 0; i < 14000; i++) {                                  /* explosão: ruído filtrado com decaimento */
    int n = (int)(rnd() & 0x7FFF) - 0x4000; y += (n - y) >> 2; b[i] = (short)(y * (14000 - i) / 14000 * 2);
  }
  snd_load(AR_BOOM, b, 14000 * 2);
  y = 0;
  for (i = 0; i < 4096; i++) { int n = (int)(rnd() & 0x7FFF) - 0x4000; y += (n - y) >> 5; b[i] = (short)(y * 3); }   /* propulsor: ruído grave em laço */
  for (i = 0; i < 64; i++) b[4095 - i] = (short)((b[4095 - i] * i + b[i] * (64 - i)) / 64);                          /* emenda do laço */
  snd_load(AR_RUMBLE, b, 4096 * 2);
  ph = 0;
  for (i = 0; i < 6000; i++) { ph += (i < 3000 ? 22 : 33) * 256; b[i] = (short)((isin(ph >> 8) >> 5) * (6000 - i) / 6000); }   /* "jingle" de onda/vida extra */
  snd_load(AR_CHIME, b, 6000 * 2);
}
static void sfx_shot(void) { snd_voice_play(1, AR_PEW, 2400, SND_PCM16, 0x10000, 120, 120, 0); }
static void sfx_boom(int size, int v) { snd_voice_play(v, AR_BOOM, 14000, SND_PCM16, size == 3 ? 0x6000 : size == 2 ? 0xB000 : 0x16000, 230, 230, 0); }
static void sfx_chime(void) { snd_voice_play(4, AR_CHIME, 6000, SND_PCM16, 0x10000, 140, 140, 0); }
static int thrust_on;
static void sfx_thrust(int on) {
  if (on && !thrust_on) snd_voice_play(0, AR_RUMBLE, 4096, SND_PCM16, 0x10000, 90, 90, 1);
  if (!on && thrust_on) snd_voice_stop(0);
  thrust_on = on;
}

/* ------------------------------ recorde no cartão de memória ------------------------------ */
#define MC_HI_SECTOR 8
static void hi_load(void) {
  unsigned *b = (unsigned *)A_MC;
  mc_present = (REG(MCARD_BASE + MC_STATUS) & 8) != 0; hiscore = 0;
  if (!mc_present) return;
  os_cache_flush(A_MC, 512); os_cache_invalidate(A_MC, 512);
  if (mc_do(1, MC_HI_SECTOR, 1, A_MC) == 0 && b[0] == 0x53525348u) hiscore = (int)b[1];
}
static void hi_save(void) {
  unsigned *b = (unsigned *)A_MC; int i;
  if (!mc_present) return;
  for (i = 0; i < 128; i++) b[i] = 0xFFFFFFFFu;
  b[0] = 0x53525348u; b[1] = (unsigned)hiscore;
  os_cache_flush(A_MC, 512);
  if (mc_do(3, MC_HI_SECTOR, 0, A_MC) == 0) mc_do(2, MC_HI_SECTOR, 1, A_MC);
}

/* ------------------------------ entrada ------------------------------ */
static unsigned poll_input(void) {
  unsigned b = REG(PAD_BASE + PAD_PORT0), g = 0; int ev;
  while ((ev = in_key_poll()) >= 0) {
    int code = ev & 0xFF, down = !(ev & KEY_UP); unsigned bit = 0;
    if (code == 0x50 || code == 0x04) bit = G_LEFT; else if (code == 0x4F || code == 0x07) bit = G_RIGHT; else if (code == 0x52 || code == 0x1A) bit = G_THRUST;
    else if (code == 0x2C) bit = G_FIRE; else if (code == 0x28) bit = G_START; else if (code == 0x13) bit = G_PAUSE; else if (code == 0x29) bit = G_QUIT;
    if (down) { kheld |= bit; kpress |= bit; } else kheld &= ~bit;      /* kpress: toques curtos (pressionar e soltar no mesmo quadro) não se perdem */
  }
  if (b & BTN_LEFT) g |= G_LEFT; if (b & BTN_RIGHT) g |= G_RIGHT; if (b & (BTN_UP | BTN_A)) g |= G_THRUST; if (b & BTN_B) g |= G_FIRE;
  if (b & BTN_START) g |= G_START; if (b & BTN_SELECT) g |= G_QUIT;
  return g | kheld;
}

/* ------------------------------ jogo ------------------------------ */
static void particles(int x, int y, int n, int speed, unsigned col, int life) {
  int i, k;
  for (k = 0; k < n; k++) {
    for (i = 0; i < MAXP && prt[i].on; i++) { }
    if (i == MAXP) return;
    { int a = rrange(0, 1023), sp = rrange(speed / 4, speed);
      prt[i].on = 1; prt[i].x = x; prt[i].y = y; prt[i].vx = isin(a) * sp >> 16; prt[i].vy = -icos(a) * sp >> 16; prt[i].life = prt[i].maxlife = rrange(life / 2, life); prt[i].col = col; }
  }
}
static void spawn_ast(int size, int x, int y, int vx, int vy) {
  int i, k;
  for (i = 0; i < MAXA && ast[i].on; i++) { }
  if (i == MAXA) return;
  ast[i].on = 1; ast[i].x = x; ast[i].y = y; ast[i].vx = vx; ast[i].vy = vy; ast[i].size = size; ast[i].r = size == 3 ? 46 : size == 2 ? 25 : 13;
  ast[i].ang = rrange(0, 1023); ast[i].spin = rrange(-9, 9);
  for (k = 0; k < NV; k++) ast[i].rad[k] = (unsigned char)rrange(92, 150);
}
static void new_wave(void) {
  int n = imin(3 + wave, 9), i;
  for (i = 0; i < n; i++) {
    int x, y, tries = 0;
    do { x = rrange(0, SW - 1) * 256; y = rrange(0, SH - 1) * 256; tries++; }
    while (tries < 30 && iabs(dwrap(x - sx, W256)) < 170 * 256 && iabs(dwrap(y - sy, H256)) < 170 * 256);
    { int a = rrange(0, 1023), sp = rrange(70, 150 + wave * 12); spawn_ast(3, x, y, isin(a) * sp >> 16, -icos(a) * sp >> 16); }
  }
  wave_t = WAVE_DELAY;
}
static void ship_reset(void) { sx = W256 / 2; sy = H256 / 2; svx = svy = 0; sang = 0; sdead = 0; sinv = 150; }
static void new_game(void) {
  int i;
  for (i = 0; i < MAXA; i++) ast[i].on = 0;
  for (i = 0; i < MAXB; i++) bul[i].on = 0;
  for (i = 0; i < MAXP; i++) prt[i].on = 0;
  score = 0; lives = 3; wave = 1; next_extra = 10000; hi_new = 0; spawn_wait = 0; ship_reset(); new_wave(); state = S_PLAY;
}
static void add_score(int v) {
  score += v;
  if (score >= next_extra) { lives++; next_extra += 10000; sfx_chime(); }
  if (score > hiscore) { hiscore = score; hi_new = 1; }
}
static void kill_ship(void) {
  sdead = 1; lives--; sfx_thrust(0); sfx_boom(3, 3);
  particles(sx, sy, 40, 900, 0xFFB0E8FFu, 70); particles(sx, sy, 24, 500, 0xFFFFC060u, 50);
  spawn_wait = 110;
  if (lives <= 0) { state = S_OVER; over_t = 0; if (hi_new) hi_save(); }
}
static void hit_ast(int i, int by_bullet) {
  struct Ast a = ast[i]; int k;
  ast[i].on = 0; (void)by_bullet;
  add_score(a.size == 3 ? 20 : a.size == 2 ? 50 : 100);
  sfx_boom(a.size, 2 + (tick & 1));
  particles(a.x, a.y, a.size * 9, 260 + a.size * 120, a.size == 3 ? 0xFF9CC8FFu : a.size == 2 ? 0xFFB8E0FFu : 0xFFE8F8FFu, 40);
  if (a.size > 1) for (k = 0; k < 2; k++) { int ang = rrange(0, 1023), sp = rrange(90, 200 + wave * 10); spawn_ast(a.size - 1, a.x, a.y, a.vx / 2 + (isin(ang) * sp >> 16), a.vy / 2 + (-icos(ang) * sp >> 16)); }
}
static void fire(void) {
  int i;
  for (i = 0; i < MAXB && bul[i].on; i++) { }
  if (i == MAXB) return;
  bul[i].on = 1; bul[i].life = 56; bul[i].x = sx + (isin(sang) * 14 >> 8); bul[i].y = sy + (-icos(sang) * 14 >> 8);
  bul[i].vx = svx + (isin(sang) * 9 >> 8); bul[i].vy = svy + (-icos(sang) * 9 >> 8);
  sfx_shot();
}

static void update_play(unsigned pressed) {
  int i, j, any = 0;
  if (pressed & G_PAUSE) { state = S_PAUSE; sfx_thrust(0); return; }
  if (!sdead) {
    if (held & G_LEFT) sang = (sang - 11) & 1023;
    if (held & G_RIGHT) sang = (sang + 11) & 1023;
    sthrust = (held & G_THRUST) != 0;
    if (sthrust) { svx += isin(sang) * 26 >> 16; svy += -icos(sang) * 26 >> 16; flame++; }
    svx -= svx / 90; svy -= svy / 90;
    sx = wrap(sx + svx, W256); sy = wrap(sy + svy, H256);
    sfx_thrust(sthrust);
    if (sfire > 0) sfire--;
    if ((held & G_FIRE) && sfire == 0) { fire(); sfire = 11; }
    if (sinv > 0) sinv--;
  } else {
    sfx_thrust(0);
    if (spawn_wait > 0) spawn_wait--;
    else if (lives > 0) {                                       /* renasce quando o centro está livre */
      int clear = 1;
      for (i = 0; i < MAXA; i++) if (ast[i].on && iabs(dwrap(ast[i].x - W256 / 2, W256)) < 110 * 256 && iabs(dwrap(ast[i].y - H256 / 2, H256)) < 110 * 256) clear = 0;
      if (clear) ship_reset();
    }
  }
  for (i = 0; i < MAXB; i++) if (bul[i].on) {
    bul[i].x = wrap(bul[i].x + bul[i].vx, W256); bul[i].y = wrap(bul[i].y + bul[i].vy, H256);
    if (--bul[i].life <= 0) bul[i].on = 0;
  }
  for (i = 0; i < MAXA; i++) if (ast[i].on) {
    ast[i].x = wrap(ast[i].x + ast[i].vx, W256); ast[i].y = wrap(ast[i].y + ast[i].vy, H256); ast[i].ang = (ast[i].ang + ast[i].spin) & 1023; any = 1;
  }
  for (i = 0; i < MAXB; i++) if (bul[i].on) for (j = 0; j < MAXA; j++) if (ast[j].on) {   /* tiro x asteroide */
    int dx = dwrap(bul[i].x - ast[j].x, W256) >> 8, dy = dwrap(bul[i].y - ast[j].y, H256) >> 8, r = ast[j].r + 2;
    if (dx * dx + dy * dy < r * r) { bul[i].on = 0; hit_ast(j, 1); break; }
  }
  if (!sdead && sinv == 0) for (j = 0; j < MAXA; j++) if (ast[j].on) {                    /* nave x asteroide */
    int dx = dwrap(sx - ast[j].x, W256) >> 8, dy = dwrap(sy - ast[j].y, H256) >> 8, r = ast[j].r * 85 / 100 + 7;
    if (dx * dx + dy * dy < r * r) { kill_ship(); break; }
  }
  for (i = 0; i < MAXP; i++) if (prt[i].on) {
    prt[i].x = wrap(prt[i].x + prt[i].vx, W256); prt[i].y = wrap(prt[i].y + prt[i].vy, H256); prt[i].vx -= prt[i].vx / 30; prt[i].vy -= prt[i].vy / 30;
    if (--prt[i].life <= 0) prt[i].on = 0;
  }
  if (state == S_PLAY && !any) {
    if (wave_t == 0) { wave++; new_wave(); sfx_chime(); } else if (wave_t > 0) wave_t--;
    if (wave_t == WAVE_DELAY) wave_t = WAVE_DELAY - 1;
  }
}

/* ------------------------------ desenho ------------------------------ */
static void line(int x0, int y0, int x1, int y1, int w, unsigned col) {   /* coordenadas em 1/4 px; w = espessura em 1/4 px */
  int dx = x1 - x0, dy = y1 - y0, len = isqrt(dx * dx + dy * dy), nx, ny;
  if (len == 0) len = 1;
  nx = -dy * w / len / 2; ny = dx * w / len / 2;
  if (vn + 6 > MAXV) return;
  vtx(x0 + nx, y0 + ny, 0, col, 0, 0); vtx(x1 + nx, y1 + ny, 0, col, 0, 0); vtx(x1 - nx, y1 - ny, 0, col, 0, 0);
  vtx(x0 + nx, y0 + ny, 0, col, 0, 0); vtx(x1 - nx, y1 - ny, 0, col, 0, 0); vtx(x0 - nx, y0 - ny, 0, col, 0, 0);
}
static void glow_line(int x0, int y0, int x1, int y1, unsigned rgb, int bright) {
  line(x0, y0, x1, y1, 26, with_alpha(rgb, 34 * bright / 255)); line(x0, y0, x1, y1, 9, with_alpha(rgb, 255 * bright / 255));
}
static void rot(int px, int py, int a, int *ox, int *oy) { int s = isin(a), c = icos(a); *ox = (px * c - py * s) >> 16; *oy = (px * s + py * c) >> 16; }
static void draw_ship_at(int cx, int cy, int a, int blink, int flame_on) {   /* cx,cy em px */
  int p[4][2] = { {0, -15}, {-10, 11}, {0, 6}, {10, 11} }, q[4][2], i;
  unsigned col = blink ? 0x80A0C0 : 0xA8E8FF;
  for (i = 0; i < 4; i++) { rot(p[i][0], p[i][1], a, &q[i][0], &q[i][1]); q[i][0] = (cx + q[i][0]) * 4; q[i][1] = (cy + q[i][1]) * 4; }
  for (i = 0; i < 4; i++) glow_line(q[i][0], q[i][1], q[(i + 1) & 3][0], q[(i + 1) & 3][1], col, 255);
  if (flame_on) {
    int fl = 10 + (flame & 3) * 4, f0[2], f1[2], f2[2];
    rot(-5, 9, a, &f0[0], &f0[1]); rot(0, 9 + fl, a, &f1[0], &f1[1]); rot(5, 9, a, &f2[0], &f2[1]);
    glow_line((cx + f0[0]) * 4, (cy + f0[1]) * 4, (cx + f1[0]) * 4, (cy + f1[1]) * 4, 0xFF9030, 255);
    glow_line((cx + f1[0]) * 4, (cy + f1[1]) * 4, (cx + f2[0]) * 4, (cy + f2[1]) * 4, 0xFF9030, 255);
  }
}
static void draw_ast_at(const struct Ast *a, int cx, int cy) {
  int i, px[NV], py[NV]; unsigned col = a->size == 3 ? 0x80B8FF : a->size == 2 ? 0x90D8FF : 0xC8F4FF;
  for (i = 0; i < NV; i++) {
    int ang = a->ang + i * 1024 / NV, rr = a->r * a->rad[i] / 128;
    px[i] = (cx + (isin(ang) * rr >> 16)) * 4; py[i] = (cy - (icos(ang) * rr >> 16)) * 4;
  }
  for (i = 0; i < NV; i++) glow_line(px[i], py[i], px[(i + 1) % NV], py[(i + 1) % NV], col, 255);
}
static void draw_wrapped(int cxq, int cyq, int r, int kind, const struct Ast *a, int ang, int blink, int fl) {
  int cx = cxq >> 8, cy = cyq >> 8, ox, oy;
  for (ox = -1; ox <= 1; ox++) for (oy = -1; oy <= 1; oy++) {
    int x = cx + ox * SW, y = cy + oy * SH;
    if (x + r < 0 || x - r > SW || y + r < 0 || y - r > SH) continue;
    if (kind == 0) draw_ship_at(x, y, ang, blink, fl); else draw_ast_at(a, x, y);
  }
}
static void draw_world(int show_ship) {
  int i;
  for (i = 0; i < MAXA; i++) if (ast[i].on) draw_wrapped(ast[i].x, ast[i].y, ast[i].r * 3 / 2, 1, &ast[i], 0, 0, 0);
  if (show_ship && !sdead && !(sinv > 0 && (sinv & 8))) draw_wrapped(sx, sy, 20, 0, 0, sang, sinv > 0, sthrust);
  for (i = 0; i < MAXB; i++) if (bul[i].on) {
    int x = bul[i].x >> 6, y = bul[i].y >> 6;                       /* 1/256 px -> 1/4 px */
    quadc(x - 18, y - 18, x + 18, y + 18, 0x40FFE890u); quadc(x - 6, y - 6, x + 6, y + 6, 0xFFFFFFE0u);
  }
  for (i = 0; i < MAXP; i++) if (prt[i].on) {
    int x = prt[i].x >> 6, y = prt[i].y >> 6, a = 255 * prt[i].life / prt[i].maxlife, s = 6 + 6 * prt[i].life / prt[i].maxlife;
    quadc(x - s, y - s, x + s, y + s, with_alpha(prt[i].col, a));
  }
}
static void draw_lives_icons(void) {
  int i;
  for (i = 0; i < lives && i < 8; i++) draw_ship_at(SW - 30 - i * 26, 38, 0, 0, 0);
}

static void render(unsigned rt) {
  char b[64]; int i;
  ui_begin();
  quad4(0, 0, SW * 4, SH * 4, 0xFF02040Cu, 0xFF02040Cu, 0xFF0B1238u, 0xFF140E30u, 0, 0, 0, 0);
  ui_seg(UI_ADD);                                               /* estrelas (a faixa mais distante rola devagar) */
  for (i = 0; i < 90; i++) {
    int x = (star_x[i] + tick * star_p[i] / 16) % SW, y = star_y[i], tw = 60 + 50 * star_p[i] / 3 + ((isin(tick * 5 + i * 83) >> 11) * (star_p[i] + 1));
    quadc(x * 4, y * 4, (x + 2 + star_p[i] / 2) * 4, (y + 2 + star_p[i] / 2) * 4, with_alpha(0xB8D0FF, imax(8, imin(255, tw))));
  }
  if (state == S_TITLE) {
    draw_world(0);
  } else {
    draw_world(1); draw_lives_icons();
  }
  ui_seg_tex(A_FONT, 8, 7, TF_A8);
  if (state != S_TITLE) { ksprintf(b, "%06d", score); text(24, 14, b, 150, 0xFFE8F4FFu); ksprintf(b, "WAVE %d", wave); text(24, 46, b, 90, 0xFF70809Cu); }
  ksprintf(b, "HI %06d", hiscore); text_c(SW / 2, 16, b, 110, hi_new && state != S_TITLE ? 0xFFFFE070u : 0xFF8CA0D0u);
  if (state == S_TITLE) {
    text_c(SW / 2, 160, "SNOWROIDS", 300, 0xFFC8E8FFu);
    text_c(SW / 2, 250, "an original SnowFlame vector shooter", 95, 0xFF7C90C0u);
    if ((tick >> 5) & 1) text_c(SW / 2, 320, mc_present ? "PRESS START / ENTER" : "PRESS START / ENTER   (no memory card: score not saved)", 120, 0xFFFFFFFFu);
    text_c(SW / 2, 400, "Arrows / A D: rotate    Up / W: thrust    Space: fire    P: pause    Esc: menu", 80, 0xFF6A7CAAu);
  } else if (state == S_PAUSE) {
    text_c(SW / 2, 200, "PAUSED", 300, 0xFFFFFFFFu); text_c(SW / 2, 290, "Start / P: resume      Esc / Select: back to menu", 100, 0xFF9CB0E0u);
  } else if (state == S_OVER) {
    text_c(SW / 2, 170, "GAME OVER", 330, 0xFFFFA0A0u);
    if (hi_new) text_c(SW / 2, 270, "NEW HIGH SCORE!", 150, 0xFFFFE070u);
    if (over_t > 60 && ((tick >> 5) & 1)) text_c(SW / 2, 340, "Press START / ENTER", 110, 0xFFFFFFFFu);
  } else if (wave_t > 0 && wave_t < WAVE_DELAY) {
    ksprintf(b, "WAVE %d", wave + 1); text_c(SW / 2, 210, b, 260, with_alpha(0xB8E0FF, imin(255, wave_t * 5)));
  } else if (wave == 1 && tick < 200 && state == S_PLAY && score == 0) text_c(SW / 2, 390, "Destroy the ice rocks", 110, with_alpha(0xB0C8F0, imin(255, (200 - tick) * 4)));
  ui_end(rt, 0xFF000000u);
}

/* ------------------------------ principal ------------------------------ */
static void title_ambient(void) { int i; for (i = 0; i < 6; i++) { int a = rrange(0, 1023), sp = rrange(40, 140); spawn_ast(rrange(1, 3), rrange(0, SW - 1) * 256, rrange(0, SH - 1) * 256, isin(a) * sp >> 16, -icos(a) * sp >> 16); } }

void app_main(void) {
  int i;
  ui_init(); make_sounds(); hi_load();
  for (i = 0; i < 90; i++) { star_x[i] = rrange(0, SW - 1); star_y[i] = rrange(0, SH - 1); star_p[i] = i % 3; }
  state = S_TITLE; title_ambient(); sdead = 1; sx = W256 / 2; sy = H256 / 2;
  os_printf("[snowroids] iniciado (recorde: %d)\n", hiscore);
  for (;;) {
    unsigned pressed;
    held_prev = held; held = poll_input(); pressed = (held & ~held_prev) | kpress; kpress = 0;
    if (pressed & G_QUIT) { sfx_thrust(0); os_printf("[snowroids] voltando ao menu\n"); os_reboot("boot/menu.bin"); }
    if (state == S_TITLE) {
      for (i = 0; i < MAXA; i++) if (ast[i].on) { ast[i].x = wrap(ast[i].x + ast[i].vx, W256); ast[i].y = wrap(ast[i].y + ast[i].vy, H256); ast[i].ang = (ast[i].ang + ast[i].spin) & 1023; }
      if (pressed & (G_START | G_FIRE)) { sfx_chime(); new_game(); }
    } else if (state == S_PLAY) update_play(pressed);
    else if (state == S_PAUSE) { if (pressed & (G_PAUSE | G_START)) state = S_PLAY; }
    else {
      over_t++; update_play(0); state = S_OVER;
      if (over_t > 60 && (pressed & (G_START | G_FIRE))) { for (i = 0; i < MAXA; i++) ast[i].on = 0; state = S_TITLE; title_ambient(); sdead = 1; }
    }
    { unsigned rt = rt_back ? RT1 : RT0; render(rt); gx_layer(0, rt, SW * 4, LF_ENABLE); rt_back = !rt_back; }
    gx_wait_vblank(); tick++;
  }
}
