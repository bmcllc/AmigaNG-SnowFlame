/* menu.c - SnowFlame Dashboard 2.0: interface principal do SF-100 (boot/menu.bin no DVD do sistema).
 *
 * Cada quadro segue o pipeline da arquitetura (seção 9 da especificação), com os chips trabalhando em paralelo:
 *   1. SnowFire (ColdFire)  entrada, lógica, matrizes em ponto fixo; publica jobs nas filas das VPUs e segue trabalhando
 *   2. SnowVector A         microprograma sys/vx/geometry.vx: transforma e ilumina o emblema 3D (cristal de gelo, chama, anéis)
 *      SnowVector B         microprograma sys/vx/dynamics.vx: neve e brasas residentes na DMEM, emitidas como quads
 *                           (ambas buscam/entregam por DMA local e avisam pelos semáforos 0 e 1 do SnowBridge)
 *   3. FlameCore            renderiza 3D + interface 2D no buffer de trás (fundo transparente)
 *   4. SnowBlitter          compõe o HUD (barras, textos e botões como listas de sprites) numa segunda camada de vídeo
 *      SnowCopper           pinta o céu linha a linha (degradê, aurora e brilho do horizonte) na cor de fundo do vídeo
 *   5. SnowPaula            pad ambiente pelo sequenciador de eventos (sem a CPU), sino de boot e sons de navegação
 *
 * Dados do disco: sys/font.*, sys/logo.raw, sys/icons.raw, sys/ui.raw, sys/hud.raw, sys/emblem.msh, sys/snd.raw, sys/vx/*.vx.
 * Jogos: games/NOME.bin (iniciados por os_reboot). */
#include "snowos_all.c"

#include "ui.h"

/* ------------------------------ mapa de memória do aplicativo ------------------------------ */
#define A_LOGO  0x01410000
#define A_ICONS 0x01420000
#define A_UIT   0x01700000              /* sys/ui.raw: texturas ARGB8888 */
#define A_MESH  0x01740000              /* sys/emblem.msh: entrada da VPU-A */
#define A_VOUT  0x01770000              /* saída da VPU-A (vértices do FlameCore) */
#define A_POUT  0x017A0000              /* saída da VPU-B (6 vértices por partícula) */
#define A_K     0x017B0000              /* blocos de constantes das VPUs (0x200 cada) */
#define A_COP   0x017C0000              /* 2 listas do Copper (0x2000 cada) */
#define A_SPR   0x017C8000              /* lista de sprites do HUD */
#define A_HUDS  0x017D0000              /* sys/hud.raw: botões e emblema pequeno (ARGB8888) */
#define A_FONTS 0x017E0000              /* fonte ARGB8888 para o Blitter: branca, gelo e brasa (0x20000 cada) */
#define A_SND   0x01840000              /* sys/snd.raw */
#define A_SEQ   0x01880000              /* eventos do sequenciador da SnowPaula */
#define A_PST   0x01890000              /* estado inicial das partículas (vai por DMA para a DMEM da VPU-B) */
#define HUD0    (VRAM_BASE + 0x400000)  /* superfícies do HUD (camada 1), buffer duplo */
#define HUD1    (VRAM_BASE + 0x600000)

#define UI_TEX_PILL 0x00000
#define UI_TEX_GLOSS 0x10000
#define UI_TEX_GLOW 0x20000
#define UI_TEX_FLAKE 0x24000
#define UI_TEX_PANEL 0x28000
#define UI_TEX_BORDER 0x2C000
#define HUD_PITCH (24 * 6 * 4)

#define COP_WAIT(y) ((1u << 28) | ((unsigned)(y) << 12))
#define COP_MOVE(a) ((2u << 28) | ((a) & 0x0FFFFFFFu))

#define F(n) ((n) * 65536 / 100)        /* centésimos -> ponto fixo 16.16 */
#define N_ITEMS 5
#define NSNOW 96
#define NEMB 56
#define VPU_BATCH 448                   /* vértices por job de geometria (limite da DMEM do microprograma) */

/* tamanhos das partes do emblema (lidos de sys/emblem.msh) e saída da VPU-A: chama | cristal | anel 1 | anel 2 */
static int part_first[3], part_count[3], out_flame, out_crystal, out_ring1, out_ring2, out_total, geo_jobs;

static int tick, sel, state, panel_item, list_sel, set_sel, vol = 200, music_on = 1, boot_t, boot_count, mc_ok, back = 1;
static int sel_f, panel_f, menu_f, emb_x, emb_y, emb_s, spin, sky_fade;
static unsigned keys_prev;
static char gname[8][24]; static int ngames;
static int snd_off[5], snd_len[5];
static int st_cpu, st_vpua, st_vpub, st_gpu, st_dsp, st_dma, st_spr, st_tris;
static unsigned last_vpua, last_vpub, last_dma;
static int acc_cpu, acc_vpua, acc_vpub, acc_gpu;           /* médias móveis em 1/64 de ponto percentual */

enum { ST_BOOT, ST_MENU, ST_PANEL };
static const char *item_name[N_ITEMS] = { "GAMES", "MEMORY CARD", "SYSTEM", "DISC", "SETTINGS" };
static const char *item_desc[N_ITEMS] = { "Play a game from the disc", "Saved data on the SnowCard", "Chips, memory and live activity", "Files on the inserted disc", "Volume, music and sound test" };

/* ------------------------------ matemática em ponto fixo 16.16 ------------------------------ */
static int bsin(int a) {                         /* a: 1024 = volta; aproximação de Bhaskara (~0,2%) */
  int ph = a & 1023, h = ph & 511, q = h * (512 - h), s = (q << 10) / ((327680 - q) >> 8);
  return ph >= 512 ? -s : s;
}
static int bcos(int a) { return bsin(a + 256); }
static int mulfx(int a, int b) { return (a >> 16) * b + (((a & 0xFFFF) * (b >> 8)) >> 8); }
static int ease(int cur, int target, int div) { int d = target - cur; return cur + d / div + (d > 0 ? 1 : d < 0 ? -1 : 0); }
static void m_ident(int *m) { int i; for (i = 0; i < 16; i++) m[i] = (i % 5) == 0 ? 65536 : 0; }
static void m_mul(int *r, const int *a, const int *b) {
  int t[16], i, j, k;
  for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) { int s = 0; for (k = 0; k < 4; k++) s += mulfx(a[i * 4 + k], b[k * 4 + j]); t[i * 4 + j] = s; }
  for (i = 0; i < 16; i++) r[i] = t[i];
}
static void m_rot(int *m, int axis, int a) {     /* multiplica m por uma rotação em X (0), Y (1) ou Z (2) */
  int r[16], c = bcos(a), s = bsin(a);
  m_ident(r);
  if (axis == 0) { r[5] = c; r[6] = -s; r[9] = s; r[10] = c; }
  else if (axis == 1) { r[0] = c; r[2] = s; r[8] = -s; r[10] = c; }
  else { r[0] = c; r[1] = -s; r[4] = s; r[5] = c; }
  m_mul(m, m, r);
}
static void m_scale(int *m, int sx, int sy, int sz) { int i; for (i = 0; i < 4; i++) { m[i * 4] = mulfx(m[i * 4], sx); m[i * 4 + 1] = mulfx(m[i * 4 + 1], sy); m[i * 4 + 2] = mulfx(m[i * 4 + 2], sz); } }

static unsigned rng = 0x5F3759DFu;
static int rnd(int n) { rng = rng * 1103515245u + 12345u; return (int)((rng >> 8) % (unsigned)n); }

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

/* ------------------------------ SnowPaula ------------------------------ */
#define SND_TICK 0
#define SND_OPEN 1
#define SND_BACK 2
#define SND_BELL 3
#define SND_PAD 4
static const int semi_q16[12] = { 65536, 69433, 73562, 77936, 82570, 87480, 92682, 98193, 104032, 110218, 116771, 123715 };
static unsigned note_step(int n, int ref_note, int ref_step) {   /* passo Q16 da nota MIDI n para uma amostra gravada em ref_note com passo ref_step */
  int d = n - ref_note, oct = 0, k; unsigned s;
  while (d < 0) { d += 12; oct--; } while (d >= 12) { d -= 12; oct++; }
  k = d; s = (unsigned)((ref_step >> 4) * (semi_q16[k] >> 4)) >> 8;
  return oct >= 0 ? s << oct : s >> -oct;
}
static void sfx(int id) {
  int v = id == SND_TICK ? 0 : id == SND_OPEN ? 1 : 2;
  snd_voice_play(v, (unsigned)snd_off[id], (unsigned)snd_len[id], SND_PCM16, 0x10000, vol * 3 / 4, vol * 3 / 4, 0);
  REG(PAULA_BASE + PA_VOICE0 + (unsigned)v * 0x40 + PV_REV_SEND) = 70;
}
static void bell(int voice, int note, int pan) {
  unsigned b = PAULA_BASE + PA_VOICE0 + (unsigned)voice * 0x40;
  snd_voice_play(voice, (unsigned)snd_off[SND_BELL], (unsigned)snd_len[SND_BELL], SND_PCM16, note_step(note, 72, 0x10000), vol * (16 - pan) / 20, vol * (16 + pan) / 20, 0);
  REG(b + PV_REV_SEND) = 150;
}
static unsigned *seq_put(unsigned *e, unsigned t, int voice, unsigned reg, unsigned val) { e[0] = t; e[1] = (unsigned)voice; e[2] = reg; e[3] = val; return e + 4; }
/* pad ambiente: 4 acordes de 4 s, 3 notas x 2 vozes desafinadas (coro), envelopes feitos só por eventos do sequenciador */
static const unsigned char chords[4][3] = { {57, 64, 71}, {53, 60, 69}, {60, 67, 74}, {55, 62, 71} };
static void music_start(void) {
  unsigned *e = (unsigned *)A_SEQ, *e0 = e; int c, n, d, r, base = 16;
  for (n = 0; n < 6; n++) {                                     /* vozes 16..21: laço do pad, volume zero */
    unsigned b = PAULA_BASE + PA_VOICE0 + (unsigned)(base + n) * 0x40;
    snd_voice_play(base + n, (unsigned)snd_off[SND_PAD], (unsigned)snd_len[SND_PAD], SND_PCM16, 0x10000, 0, 0, 1);
    REG(b + PV_REV_SEND) = 200;
  }
  REG(PAULA_BASE + PA_REVERB_WET) = 110;
  for (c = 0; c < 4; c++) {
    unsigned t0 = (unsigned)c * 192000u;
    for (n = 0; n < 3; n++) for (d = 0; d < 2; d++) {
      int v = base + n * 2 + d, pk = 30 - n * 4; unsigned st = note_step(chords[c][n], 42, 66421);   /* amostra: 93,75 Hz = nota 42 (92,5 Hz) + 1,4% */
      if (d) st += st >> 8;                                      /* +0,4%: coro */
      e = seq_put(e, t0, v, PV_STEP, st);
      for (r = 1; r <= 8; r++) {                                 /* ataque de 1 s e soltura de 1 s, em 8 degraus */
        int lv = pk * r / 8, rv = lv * (d ? 6 : 10) / 10, lv2 = lv * (d ? 10 : 6) / 10;
        e = seq_put(e, t0 + (unsigned)r * 6000u, v, PV_VOL_L, (unsigned)lv2); e = seq_put(e, t0 + (unsigned)r * 6000u, v, PV_VOL_R, (unsigned)rv);
      }
      for (r = 7; r >= 0; r--) {
        int lv = pk * r / 8, rv = lv * (d ? 6 : 10) / 10, lv2 = lv * (d ? 10 : 6) / 10; unsigned t = t0 + 192000u - 48000u + (unsigned)(8 - r) * 5900u;
        e = seq_put(e, t, v, PV_VOL_L, (unsigned)lv2); e = seq_put(e, t, v, PV_VOL_R, (unsigned)rv);
      }
    }
  }
  /* ordena por tempo (inserção: a lista é pequena) - o sequenciador exige eventos em ordem */
  { int ne = (int)(e - e0) / 4, i, j;
    for (i = 1; i < ne; i++) { unsigned k[4]; int q; for (q = 0; q < 4; q++) k[q] = e0[i * 4 + q]; j = i - 1;
      while (j >= 0 && e0[j * 4] > k[0]) { for (q = 0; q < 4; q++) e0[(j + 1) * 4 + q] = e0[j * 4 + q]; j--; }
      for (q = 0; q < 4; q++) e0[(j + 1) * 4 + q] = k[q]; }
    snd_seq_play(A_SEQ, ne, 1); }
}
static void music_stop(void) { int n; REG(PAULA_BASE + PA_SEQ_CTRL) = 0; for (n = 16; n < 22; n++) snd_voice_stop(n); }

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

/* ------------------------------ SnowVector A: emblema 3D ------------------------------ */
static void kput(unsigned *k, int slot, int x, int y, int z, int w) { k += slot * 4; k[0] = gx_f32_fx(x); k[1] = gx_f32_fx(y); k[2] = gx_f32_fx(z); k[3] = gx_f32_fx(w); }
/* bloco de constantes do geometry.vx para a matriz de modelo m (16.16) e o material; luz fixa no espaço da câmera */
static void geo_consts(int blk, const int *model, unsigned light, unsigned amb, int spec, unsigned rim, int rim_a) {
  static const int proj[16] = { 151277, 0, 0, 0,   0, 201700, 0, 0,   0, 0, -77451, -142987,   0, 0, -65536, 0 };   /* fov 36°, 4:3, perto 1, longe 12 */
  int mv[16], mvp[16], c; unsigned *k = (unsigned *)(A_K + (unsigned)blk * 0x200);
  for (c = 0; c < 16; c++) mv[c] = model[c];
  mv[11] -= 275251;                                               /* câmera em z = +4,2 */
  m_mul(mvp, proj, mv);
  for (c = 0; c < 4; c++) kput(k, c, mvp[c], mvp[4 + c], mvp[8 + c], mvp[12 + c]);
  for (c = 0; c < 3; c++) kput(k, 4 + c, mv[c], mv[4 + c], mv[8 + c], 0);
  kput(k, 7, F(-42), F(63), F(65), 0);
  kput(k, 8, 320 << 16, -240 << 16, F(50), 0); kput(k, 9, emb_x << 16, emb_y << 16, F(50), 0);
  kput(k, 10, 0, 0, 0, 65536);
  kput(k, 11, (int)((light >> 16) & 255) * 257, (int)((light >> 8) & 255) * 257, (int)(light & 255) * 257, 0);
  kput(k, 12, 0, 0, 0, 0);
  kput(k, 13, (int)((amb >> 16) & 255) * 257, (int)((amb >> 8) & 255) * 257, (int)(amb & 255) * 257, 65536);
  kput(k, 14, 65536, 65536, 65536, 65536);
  kput(k, 15, F(-23), F(35), F(91), 0);
  kput(k, 16, spec, spec, spec, 0);
  kput(k, 17, (int)((rim >> 16) & 255) * 257, (int)((rim >> 8) & 255) * 257, (int)(rim & 255) * 257, rim_a);
}
static void geo_submit(int part, int blk, int out_first) {
  int first = part_first[part], count = part_count[part], done = 0;
  while (done < count) {
    int n = count - done < VPU_BATCH ? count - done : VPU_BATCH; unsigned s[5];
    s[0] = A_MESH + 32 + (unsigned)(first + done) * 64; s[1] = A_VOUT + (unsigned)(out_first + done) * 48;
    s[2] = (unsigned)n * 64; s[3] = A_K + (unsigned)blk * 0x200; s[4] = (unsigned)n * 48;
    while (vx_submit(0, 0, s, 5) < 0) task_yield();             /* fila cheia: espera um job terminar */
    done += n; geo_jobs++;
  }
}
static void emblem_submit(void) {
  int m[16], fl = 65536 + (bsin(tick * 37) >> 4) + (bsin(tick * 23 + 300) >> 5), glow = state == ST_BOOT ? (boot_t < 70 ? 0 : boot_t < 110 ? (boot_t - 70) * 6 : 240) : 240;
  int s = emb_s;
  /* cristal e chama: giro em Y com leve balanço */
  m_ident(m); m_scale(m, s, s, s); m_rot(m, 1, spin); m_rot(m, 0, bsin(tick * 3) >> 12); m_rot(m, 2, bsin(tick * 2 + 100) >> 13);
  geo_consts(1, m, 0x8CA6CC, 0x5A6B8C, F(90), 0x80D8FF, F(55));
  { int f[16], i; for (i = 0; i < 16; i++) f[i] = m[i];
    m_scale(f, mulfx(fl, glow * 273), mulfx(fl + (bsin(tick * 41) >> 4), glow * 273), mulfx(fl, glow * 273));
    geo_consts(0, f, 0, 0xFFFFFF, 0, 0, 0); }
  /* anéis de órbita: gelo e brasa, inclinações diferentes */
  m_ident(m); m_scale(m, mulfx(s, F(132)), mulfx(s, F(132)), mulfx(s, F(132))); m_rot(m, 1, tick * 2); m_rot(m, 0, 230 + (bsin(tick * 2) >> 12));
  geo_consts(2, m, 0x7FB2FF, 0x2A5CB0, F(100), 0x5096FF, 0);
  m_ident(m); m_scale(m, mulfx(s, F(118)), mulfx(s, F(118)), mulfx(s, F(118))); m_rot(m, 1, 512 - tick * 3); m_rot(m, 0, 190 + (bsin(tick * 3 + 200) >> 12)); m_rot(m, 2, 90);
  geo_consts(3, m, 0xFF9040, 0x8C3810, F(90), 0xFF7020, 0);
  os_cache_flush(A_K, 4 * 0x200);
  vx_sem_reset(0); geo_jobs = 0;
  geo_submit(2, 2, out_ring1); geo_submit(2, 3, out_ring2); geo_submit(0, 0, out_flame); geo_submit(1, 1, out_crystal);
}

/* ------------------------------ SnowVector B: neve e brasas ------------------------------ */
static void part_consts(int blk, int speed, int x0, int y0, int w, int h, int k, int amp) {
  unsigned *c = (unsigned *)(A_K + (unsigned)blk * 0x200);
  kput(c, 0, speed, speed, speed, 0); kput(c, 1, -k, k, 0, 0); kput(c, 2, amp, 0, 0, 0);
  kput(c, 3, w << 16, h << 16, 65536, 0); kput(c, 4, x0 << 16, y0 << 16, 0, 65536);
  kput(c, 5, 65536, 65536, 65536, 65536); kput(c, 6, 0, 0, 0, 0);
  kput(c, 7, -65536, -65536, 0, 0); kput(c, 8, 65536, -65536, 0, 0); kput(c, 9, 65536, 65536, 0, 0); kput(c, 10, -65536, 65536, 0, 0);
  kput(c, 11, 0, 0, 0, 0); kput(c, 12, 65536, 0, 0, 0); kput(c, 13, 65536, 65536, 0, 0); kput(c, 14, 0, 65536, 0, 0);
  kput(c, 15, 4 << 16, 4 << 16, 4 << 16, 4 << 16);
}
static void particles_init(void) {
  unsigned *p = (unsigned *)A_PST; int i;
  for (i = 0; i < NSNOW + NEMB; i++, p += 16) {
    int emb = i >= NSNOW, z = rnd(65536), a = rnd(1024), sz, br;
    kput(p, 0, rnd(65536), rnd(65536), z, 0);
    if (!emb) { sz = 2 + z / 22000 + rnd(2); kput(p, 1, rnd(120) - 60, 70 + z / 300 + rnd(60), 0, sz << 16); br = 45 + z / 1200;
                kput(p, 3, F(78) * br / 100, F(90) * br / 100, br * 655, br * 655); }
    else { sz = 2 + rnd(3); kput(p, 1, rnd(80) - 40, -(140 + rnd(260)), 0, sz << 16); br = 55 + rnd(45);
           kput(p, 3, br * 655, F(52) * br / 100, F(16) * br / 100, br * 655); }
    kput(p, 2, bcos(a), bsin(a), 0, 0);
  }
  vx_dma_to_dmem(1, 0x100, (void *)A_PST, (NSNOW + NEMB) * 64);
}
static void particles_submit(void) {
  unsigned s[6]; int calm = state == ST_PANEL ? 40 : 100;
  part_consts(4, F(calm), -30, -30, 700, 540, F(4), F(2));
  part_consts(5, F(calm), emb_x - 150, emb_y - 230, 300, 360, F(7), F(4));
  os_cache_flush(A_K + 4 * 0x200, 2 * 0x200);
  vx_sem_reset(1);
  s[0] = 0x100; s[1] = 0; s[2] = 0x100 + NSNOW * 64; s[3] = A_POUT; s[4] = NSNOW * 288; s[5] = A_K + 4 * 0x200;
  while (vx_submit(1, 0, s, 6) < 0) task_yield();
  s[0] = 0x100 + NSNOW * 64; s[2] = 0x100 + (NSNOW + NEMB) * 64; s[3] = A_POUT + NSNOW * 288; s[4] = NEMB * 288; s[5] = A_K + 5 * 0x200;
  while (vx_submit(1, 0, s, 6) < 0) task_yield();
}
static void vpus_wait(void) {                    /* cada job de geometria dá SIGNAL 0; cada job de partículas, SIGNAL 1 */
  int i;
  for (i = 0; i < geo_jobs; i++) vx_sem_wait(0);
  vx_sem_wait(1); vx_sem_wait(1);
}

/* ------------------------------ SnowCopper: céu por scanline ------------------------------ */
static unsigned rgb_scale(int r, int g, int b, int f) { r = r * f >> 8; g = g * f >> 8; b = b * f >> 8; return 0xFF000000u | ((unsigned)imin(r, 255) << 16) | ((unsigned)imin(g, 255) << 8) | (unsigned)imin(b, 255); }
static void copper_sky(unsigned list) {
  unsigned *c = (unsigned *)list; int y, n = 0;
  for (y = 0; y < SH; y += 2) {
    int r, g, b, au, hz;
    if (y < 330) { int t = y * 256 / 330; r = 2 + t * 16 / 256; g = 6 + t * 34 / 256; b = 22 + t * 76 / 256; }
    else { int t = (y - 330) * 256 / 150; r = 18 - t * 12 / 256; g = 36 - t * 24 / 256; b = 84 - t * 52 / 256; }
    au = (bsin(y * 5 + tick * 2) + bsin(y * 3 - tick * 3 + 400)) >> 10;                    /* aurora: -128..128 */
    if (y > 40 && y < 240) { int env = y < 140 ? (y - 40) * 256 / 100 : (240 - y) * 256 / 100; au = imax(0, au) * env >> 8; r += au / 10; g += au * 60 / 128; b += au * 40 / 128; }
    hz = 330 - y; if (hz < 0) hz = -hz;                                                     /* brilho do horizonte */
    if (hz < 40) { int k = (40 - hz) * (40 - hz) / 16; r += k / 3; g += k * 2 / 3; b += k; }
    c[n++] = COP_WAIT(y); c[n++] = 0;
    c[n++] = COP_MOVE(VIDEO_BASE + VID_BG); c[n++] = rgb_scale(r, g, b, sky_fade);
  }
  c[n++] = 0; c[n++] = 0;
  os_cache_flush(list, (unsigned)n * 4);
}

/* ------------------------------ SnowBlitter: HUD (barras, texto e botões como sprites) ------------------------------ */
static unsigned *spr; static int nspr;
static void spr_add(unsigned src, unsigned pitch, int x, int y, int w, int h) {
  if (nspr >= 400) return;
  spr[0] = src; spr[1] = pitch << 16; spr[2] = ((unsigned)x << 16) | ((unsigned)y & 0xFFFF); spr[3] = ((unsigned)w << 16) | (unsigned)h; spr += 4; nspr++;
}
static int hud_text(int x, int y, const char *s, int sheet) {
  unsigned base = A_FONTS + (unsigned)sheet * 0x20000;
  while (*s) {
    int c = *s++ & 127, a = adv[c], cx = (c & 15) * 16, cy = (c >> 4) * 16;
    if (c > 32 && a) { int tx = cx + (16 - a) / 2 - 1; spr_add(base + (unsigned)cy * 1024 + (unsigned)tx * 4, 1024, x, y, a + 2, 16); }
    x += a + 1;
  }
  return x;
}
static int hud_text_w(const char *s) { int w = 0; while (*s) w += adv[*s++ & 127] + 1; return w; }
static void hud_icon(int cell, int x, int y) { spr_add(A_HUDS + (unsigned)cell * 96, HUD_PITCH, x, y, 24, 24); }
static void hud_button(int cell, const char *letter, const char *label, int x, int y) {
  hud_icon(cell, x, y); hud_text(x + 12 - hud_text_w(letter) / 2 - 1, y + 3, letter, 0); hud_text(x + 28, y + 3, label, 0);
}
static void hud_draw(unsigned surf) {
  char b[64]; int x;
  gx_blit(BOP_FILL, 0, 0, surf, SW * 4, SW, 32, 0xA8050A20u);
  gx_blit(BOP_FILL, 0, 0, surf + 32 * SW * 4, SW * 4, SW, 1, 0xFF60C8FFu);
  gx_blit(BOP_FILL, 0, 0, surf + 446 * SW * 4, SW * 4, SW, 1, 0xC0FF8A30u);
  gx_blit(BOP_FILL, 0, 0, surf + 447 * SW * 4, SW * 4, SW, 33, 0xA8050A20u);
  spr = (unsigned *)A_SPR; nspr = 0;
  hud_icon(5, 10, 4); x = hud_text(40, 8, "AMIGA", 0); hud_text(x + 6, 8, "SnowFlame", 1);
  ksprintf(b, "%d:%02d", (int)(os_ticks() / 60000), (int)((os_ticks() / 1000) % 60)); hud_text(SW - 14 - hud_text_w(b), 8, b, 0);
  hud_icon(mc_ok ? 3 : 4, SW - 214, 4); hud_text(SW - 188, 8, "CARD", mc_ok ? 0 : 1);
  hud_icon(2, SW - 136, 4); hud_text(SW - 110, 8, "DVD", 0);
  if (state == ST_PANEL) { hud_button(0, "A", panel_item == 0 ? "Play" : "Select", 14, 452); hud_button(1, "B", "Back", 120, 452); }
  else { hud_button(0, "A", "Open", 14, 452); hud_button(1, "B", "Info", 120, 452); }
  ksprintf(b, "CPU %d%%   VPU-A %d%%   VPU-B %d%%   GPU %d%%", st_cpu, st_vpua, st_vpub, st_gpu); hud_text(SW - 14 - hud_text_w(b), 456, b, 1);
  st_spr = nspr;
  gx_blit_sprites(A_SPR, nspr, surf, SW * 4, SW, SH);
}

/* ------------------------------ FlameCore 2D: formas da interface ------------------------------ */
static void qpoly(int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3, unsigned c0, unsigned c1) {   /* quadrilátero qualquer (1/4 px) */
  if (vn + 6 > MAXV) return;
  vtx(x0, y0, 0, c0, 0, 0); vtx(x1, y1, 0, c0, 0, 0); vtx(x2, y2, 0, c1, 0, 0);
  vtx(x0, y0, 0, c0, 0, 0); vtx(x2, y2, 0, c1, 0, 0); vtx(x3, y3, 0, c1, 0, 0);
}
static void nine(int x0, int y0, int x1, int y1, int k, unsigned c) {   /* 9 fatias de uma textura 64x64 com cantos de 16 texels (k px na tela) */
  int xs[4], ys[4], us[4] = {0, 256, 768, 1024}, i, j;
  xs[0] = x0; xs[1] = x0 + k; xs[2] = x1 - k; xs[3] = x1; ys[0] = y0; ys[1] = y0 + k; ys[2] = y1 - k; ys[3] = y1;
  for (j = 0; j < 3; j++) for (i = 0; i < 3; i++) quad4(xs[i] * 4, ys[j] * 4, xs[i + 1] * 4, ys[j + 1] * 4, c, c, c, c, us[i], us[j], us[i + 1], us[j + 1]);
}
static void floor_grid(void) {                                  /* grade em perspectiva rumo ao horizonte, andando devagar */
  int i, hz = 330, ph = (tick * 3) & 255;
  for (i = 0; i < 9; i++) {
    int z = i * 256 + 256 - ph, y = hz + 38400 / z, a;
    if (y >= SH || y <= hz) continue;
    a = imin(90, (y - hz) * 90 / 150) * sky_fade >> 8;
    quadc(0, y * 4, SW * 4, y * 4 + 4, with_alpha(0x50B8FF, a));
  }
  for (i = -10; i <= 10; i++) {
    int xb = 320 + i * 70, a = (90 - iabs(i) * 7) * sky_fade >> 8;
    if (a <= 0) continue;
    qpoly(320 * 4 + i * 14, hz * 4, 320 * 4 + i * 14 + 2, hz * 4, xb * 4 + 5, SH * 4, xb * 4 - 2, SH * 4, with_alpha(0x50B8FF, 0), with_alpha(0x50B8FF, a));
  }
}
static int pill_y(int i) { return 98 + i * 64; }
static void bar(int x, int y, int w, int pct, unsigned c0, unsigned c1) {
  int f = w * imin(pct, 100) / 100;
  quadc(x * 4, y * 4, (x + w) * 4, (y + 8) * 4, 0x40203860u);
  if (f > 0) quad4(x * 4, y * 4, (x + f) * 4, (y + 8) * 4, c0, mix(c0, c1, pct * 256 / 100), c0, mix(c0, c1, pct * 256 / 100), 0, 0, 0, 0);
}

/* ------------------------------ painéis ------------------------------ */
#define PX0 286
#define PX1 620
#define PY0 62
#define PY1 436
static void panel_shapes(int ox) {
  int x0 = PX0 + ox, y = PY0 + 64, i;
  if (panel_item == 0 && ngames > 0) quad4((x0 + 18) * 4, (y + list_sel * 30 - 3) * 4, (PX1 + ox - 18) * 4, (y + list_sel * 30 + 23) * 4, 0xC0FF8A2Au, 0x30FF8A2Au, 0xC0FF8A2Au, 0x30FF8A2Au, 0, 0, 0, 0);
  if (panel_item == 2) {
    int pv[5]; pv[0] = st_cpu; pv[1] = st_vpua; pv[2] = st_vpub; pv[3] = st_gpu; pv[4] = st_dsp;
    for (i = 0; i < 5; i++) bar(x0 + 112, y + 168 + i * 24 + 5, 150, pv[i], 0xFF40B8FFu, 0xFFFF7A20u);
  }
  if (panel_item == 4) {
    quadc((x0 + 18) * 4, (y + set_sel * 44 - 6) * 4, (PX1 + ox - 18) * 4, (y + set_sel * 44 + 30) * 4, 0x50FF8A2Au);
    bar(x0 + 130, y + 8, 170, vol * 100 / 256, 0xFF40B8FFu, 0xFFFF9A40u);
  }
}
static void panel_text(int ox) {
  char buf[64]; int x0 = PX0 + ox + 26, y = PY0 + 64, i; unsigned col = 0xFFE8F0FFu, dim = 0xFF8FA6D8u, ice = 0xFF8ED8FFu, ember = 0xFFFFB070u;
  text(x0 + 44, PY0 + 18, item_name[panel_item], 150, 0xFFFFFFFFu);
  if (panel_item == 0) {
    if (ngames == 0) { text(x0, y + 60, "No games found on this disc", 110, dim); return; }
    for (i = 0; i < ngames; i++) text(x0 + 8, y + i * 30, gname[i], 130, i == list_sel ? 0xFFFFFFFFu : dim);
  } else if (panel_item == 1) {
    if (!mc_ok) { text(x0, y + 60, "No SnowCard inserted in slot A", 110, dim); return; }
    ksprintf(buf, "SnowCard A   %d KB  /  %d sectors", (int)(REG(MCARD_BASE + MC_CAPACITY) / 2), (int)REG(MCARD_BASE + MC_CAPACITY)); text(x0, y, buf, 105, col);
    ksprintf(buf, "System boots recorded: %d", boot_count); text(x0, y + 36, buf, 130, ember);
    text(x0, y + 84, "SnowFlame system data   -   sector 0", 95, ice);
    text(x0, y + 104, "SnowRoids high score    -   sector 8", 95, ice);
    text(x0, y + 150, "Flash: program clears bits, erase in 4 KB", 88, dim);
    text(x0, y + 168, "blocks. The counter survives every reset.", 88, dim);
  } else if (panel_item == 2) {
    text(x0, y - 4, "SnowFire    ColdFire V4e 266 MHz, FPU, eMAC, MMU", 82, col);
    text(x0, y + 16, "SnowVector  2 x FP32x4 SIMD 150 MHz, 2.4 GFLOPS", 82, col);
    text(x0, y + 36, "FlameCore   tile renderer 200 MHz, 2 pipes, 4 TMU", 82, col);
    text(x0, y + 56, "SnowPaula   DSP 100 MHz, 64 voices, 48 kHz", 82, col);
    text(x0, y + 76, "Memory      64 MB DDR, 32 MB VRAM, 4 MB ARAM", 82, col);
    ksprintf(buf, "SnowOS Native   up %d s   heap %d KB", (int)(os_ticks() / 1000), (int)(os_heap_free() / 1024)); text(x0, y + 100, buf, 88, ice);
    text(x0, y + 140, "LIVE ACTIVITY", 100, ember);
    { const char *nm[5]; int pv[5]; nm[0] = "SnowFire"; nm[1] = "Vector A"; nm[2] = "Vector B"; nm[3] = "FlameCore"; nm[4] = "SnowPaula";
      pv[0] = st_cpu; pv[1] = st_vpua; pv[2] = st_vpub; pv[3] = st_gpu; pv[4] = st_dsp;
      for (i = 0; i < 5; i++) { text(x0, y + 168 + i * 24, nm[i], 90, dim); ksprintf(buf, "%d%%", pv[i]); text(x0 + 252, y + 168 + i * 24, buf, 90, col); } }
    ksprintf(buf, "VPU DMA %d KB/frame  Blitter %d sprites  %d tris", st_dma, st_spr, st_tris); text(x0, y + 292, buf, 78, dim);
  } else if (panel_item == 3) {
    ksprintf(buf, "SnowFS disc   %d sectors  (%d KB)", (int)REG(DVD_BASE + DVD_CAPACITY), (int)(REG(DVD_BASE + DVD_CAPACITY) * 2)); text(x0, y, buf, 100, col);
    ksprintf(buf, "%d files", fs_count()); text(x0, y + 24, buf, 100, ice);
    for (i = 0; i < fs_count() && i < 12; i++) { ksprintf(buf, "%s", fs_name(i)); text(x0 + 8, y + 54 + i * 20, buf, 86, dim); }
  } else {
    ksprintf(buf, "Volume"); text(x0, y + 2, buf, 115, set_sel == 0 ? 0xFFFFFFFFu : dim);
    ksprintf(buf, "%d%%", vol * 100 / 256); text(x0 + 238, y + 2, buf, 100, col);
    text(x0, y + 46, "Ambient music", 115, set_sel == 1 ? 0xFFFFFFFFu : dim); text(x0 + 238, y + 46, music_on ? "On" : "Off", 110, music_on ? ember : dim);
    text(x0, y + 90, "Sound test", 115, set_sel == 2 ? 0xFFFFFFFFu : dim); text(x0 + 238, y + 90, "Play", 110, ice);
    text(x0, y + 150, "Pad: SnowPaula event sequencer + reverb", 86, dim);
    text(x0, y + 168, "Bell: 5-partial sample pitched per voice", 86, dim);
  }
}

/* ------------------------------ renderização de um quadro ------------------------------ */
static void render(unsigned rt) {
  int i, mf = menu_f, pf = panel_f, ox = (256 - pf) * 360 / 256;
  ui_begin();
  ui_seg(UI_ALPHA); floor_grid();
  /* halo atrás do emblema (gelo + brasa) */
  ui_seg_texadd(A_UIT + UI_TEX_GLOW, 6, 6, TF_ARGB8888);
  { int r = emb_s * 260 >> 16, r2 = emb_s * 130 >> 16, g = state == ST_BOOT ? imin(255, boot_t * 3) : 255;
    quad4((emb_x - r) * 4, (emb_y - r) * 4, (emb_x + r) * 4, (emb_y + r) * 4, with_alpha(0x2060C0, g * 120 >> 8), with_alpha(0x2060C0, g * 120 >> 8), with_alpha(0x2060C0, g * 120 >> 8), with_alpha(0x2060C0, g * 120 >> 8), 0, 0, 1024, 1024);
    quad4((emb_x - r2) * 4, (emb_y - r2) * 4, (emb_x + r2) * 4, (emb_y + r2) * 4, with_alpha(0xFF7020, g * 110 >> 8), with_alpha(0xFF7020, g * 110 >> 8), with_alpha(0xFF7020, g * 110 >> 8), with_alpha(0xFF7020, g * 110 >> 8), 0, 0, 1024, 1024); }
  /* saídas das VPUs */
  ui_ext(A_POUT, 0, NSNOW * 6, ST_BLEND | ST_TEX0 | ST_BILINEAR, BF_SRC_ALPHA, BF_ONE, A_UIT + UI_TEX_FLAKE, 6, 6, TF_ARGB8888);
  ui_ext(A_VOUT, out_ring1, part_count[2] * 2, ST_DEPTH_TEST | ST_DEPTH_WRITE, BF_ONE, BF_ZERO, 0, 0, 0, 0);
  ui_ext(A_VOUT, out_flame, part_count[0], ST_DEPTH_TEST | ST_BLEND, BF_SRC_ALPHA, BF_ONE, 0, 0, 0, 0);
  ui_ext(A_VOUT, out_crystal, part_count[1], ST_DEPTH_TEST | ST_BLEND, BF_SRC_ALPHA, BF_ONE, 0, 0, 0, 0);   /* gelo que brilha: aditivo */
  ui_ext(A_POUT, NSNOW * 6, NEMB * 6, ST_BLEND | ST_TEX0 | ST_BILINEAR, BF_SRC_ALPHA, BF_ONE, A_UIT + UI_TEX_GLOW, 6, 6, TF_ARGB8888);
  st_tris = (part_count[0] + part_count[1] + part_count[2] * 2) / 3 + (NSNOW + NEMB) * 2;
  if (state == ST_BOOT) {                                        /* logotipo do boot: "AMIGA" em brasa, SNOWFLAME em gelo */
    int out = boot_t > 168 ? imax(0, 255 - (boot_t - 168) * 9) : 255;
    int a = boot_t < 120 ? 0 : imin(out, (boot_t - 120) * 5), b = boot_t < 100 ? 0 : imin(out, (boot_t - 100) * 5), c = boot_t < 140 ? 0 : imin(out, (boot_t - 140) * 6);
    ui_seg_tex(A_LOGO, 9, 6, TF_A8);
    quad4(64 * 4, 372 * 4, 576 * 4, 436 * 4, with_alpha(0xFFFFFF, a), with_alpha(0xFFFFFF, a), with_alpha(0x9FD8FF, a), with_alpha(0x9FD8FF, a), 0, 0, 1024, 1024);
    ui_seg_tex(A_FONT, 8, 7, TF_A8);
    text_c(SW / 2, 346, "A  M  I  G  A", 150, with_alpha(0xFFB070, b));
    text_c(SW / 2, 446, "SF-100   -   SnowOS", 95, with_alpha(0x8FB8E8, c));
    ui_end(rt, 0x00000000u);
    return;
  }
  /* cápsulas "gel" do menu */
  if (mf > 0) {
    int ofs = (256 - mf) * 300 / 256 + pf * 340 / 256;
    ui_seg_texadd(A_UIT + UI_TEX_GLOW, 6, 6, TF_ARGB8888);
    { int y = pill_y(sel), x0 = 382 + ofs - sel_f, g = (200 + (bsin(tick * 6) >> 11)) * mf >> 8;   /* aura pulsante da seleção */
      quad4((x0 - 70) * 4, (y - 44) * 4, (x0 + 306) * 4, (y + 100) * 4, with_alpha(0xFF6A1A, g), with_alpha(0xFF6A1A, g), with_alpha(0xFF6A1A, g), with_alpha(0xFF6A1A, g), 0, 0, 1024, 1024);
      quad4((x0 - 70) * 4, (y - 44) * 4, (x0 + 306) * 4, (y + 100) * 4, with_alpha(0xFF6A1A, g), with_alpha(0xFF6A1A, g), with_alpha(0xFF6A1A, g), with_alpha(0xFF6A1A, g), 0, 0, 1024, 1024); }
    ui_seg_tex(A_UIT + UI_TEX_PILL, 8, 6, TF_ARGB8888);
    for (i = 0; i < N_ITEMS; i++) {
      int y = pill_y(i), x0 = 382 + ofs - (i == sel ? sel_f : 0), x1 = x0 + 236, a = mf * (i == sel ? 255 : 210) >> 8;
      unsigned c = i == sel ? 0xFF8A2A : 0x2A62C8, c2 = i == sel ? 0xE04A10 : 0x183C90;
      quad4(x0 * 4, y * 4, x1 * 4, (y + 56) * 4, with_alpha(c, a), with_alpha(c, a), with_alpha(c2, a), with_alpha(c2, a), 0, 0, 1024, 1024);
    }
    ui_seg_tex(A_UIT + UI_TEX_GLOSS, 8, 6, TF_ARGB8888);
    for (i = 0; i < N_ITEMS; i++) {
      int y = pill_y(i), x0 = 382 + ofs - (i == sel ? sel_f : 0), a = mf * (i == sel ? 255 : 200) >> 8;
      quad4(x0 * 4, y * 4, (x0 + 236) * 4, (y + 56) * 4, with_alpha(0xFFFFFF, a), with_alpha(0xFFFFFF, a), with_alpha(0xFFFFFF, a), with_alpha(0xFFFFFF, a), 0, 0, 1024, 1024);
    }
    ui_seg_tex(A_ICONS, 9, 6, TF_ARGB4444);
    for (i = 0; i < N_ITEMS; i++) {
      int y = pill_y(i) + 11, x0 = 400 + ofs - (i == sel ? sel_f : 0), a = mf * (i == sel ? 255 : 190) >> 8;
      quad4(x0 * 4, y * 4, (x0 + 34) * 4, (y + 34) * 4, with_alpha(0xFFFFFF, a), with_alpha(0xFFFFFF, a), with_alpha(0xFFFFFF, a), with_alpha(0xFFFFFF, a), i * 128, 0, i * 128 + 128, 1024);
    }
  }
  /* painel de vidro fosco */
  if (pf > 0) {
    ui_seg_tex(A_UIT + UI_TEX_PANEL, 6, 6, TF_ARGB8888);
    nine(PX0 + ox, PY0, PX1 + ox, PY1, 18, with_alpha(0x0A1638, pf * 215 >> 8));
    nine(PX0 + ox + 6, PY0 + 6, PX1 + ox - 6, PY0 + 50, 14, with_alpha(0x2A62C8, pf * 150 >> 8));
    ui_seg_tex(A_UIT + UI_TEX_BORDER, 6, 6, TF_ARGB8888);
    nine(PX0 + ox, PY0, PX1 + ox, PY1, 18, with_alpha(0x8ED8FF, pf * 200 >> 8));
    ui_seg_tex(A_UIT + UI_TEX_GLOSS, 8, 6, TF_ARGB8888);
    quad4((PX0 + ox + 8) * 4, (PY0 + 4) * 4, (PX1 + ox - 8) * 4, (PY0 + 40) * 4, with_alpha(0xFFFFFF, pf * 120 >> 8), with_alpha(0xFFFFFF, pf * 120 >> 8), with_alpha(0xFFFFFF, pf * 120 >> 8), with_alpha(0xFFFFFF, pf * 120 >> 8), 0, 0, 1024, 1024);
    ui_seg(UI_ALPHA); panel_shapes(ox);
    ui_seg_tex(A_ICONS, 9, 6, TF_ARGB4444);
    quad4((PX0 + ox + 22) * 4, (PY0 + 11) * 4, (PX0 + ox + 56) * 4, (PY0 + 45) * 4, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, panel_item * 128, 0, panel_item * 128 + 128, 1024);
  }
  ui_seg_tex(A_FONT, 8, 7, TF_A8);
  if (mf > 0) {
    int ofs = (256 - mf) * 300 / 256 + pf * 340 / 256;
    for (i = 0; i < N_ITEMS; i++) text(444 + ofs - (i == sel ? sel_f : 0), pill_y(i) + 17, item_name[i], 135, with_alpha(0xFFFFFF, mf * (i == sel ? 255 : 215) >> 8));
    { int a = mf * (256 - pf) >> 8;
      text_c(emb_x, 392, item_name[sel], 175, with_alpha(0xFFFFFF, a)); text_c(emb_x, 420, item_desc[sel], 92, with_alpha(0x9FC8F0, a)); }
  }
  if (pf > 0) panel_text(ox);
  ui_end(rt, 0x00000000u);
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
  music_stop();
  os_reboot(path);
}
static void update(unsigned nk) {
  if (state == ST_BOOT) {
    int t = ++boot_t;
    /* emblema: nasce minúsculo girando rápido, cresce e desacelera; depois desliza para a esquerda */
    if (t < 150) { emb_s = t < 100 ? 55700 * t / 100 * (200 - t) / 100 : 55700; emb_x = 320; emb_y = 190; spin += imax(4, 60 - t * 56 / 100); }
    else { emb_s = ease(emb_s, 47000, 8); emb_x = ease(emb_x, 190, 8); emb_y = ease(emb_y, 226, 8); spin += 4; }
    if (t == 40) bell(8, 72, -8); if (t == 48) bell(9, 76, -4); if (t == 56) bell(10, 79, 0); if (t == 64) bell(11, 83, 4); if (t == 76) { bell(12, 86, 8); bell(13, 60, 0); }
    sky_fade = t < 150 ? 0 : imin(256, (t - 150) * 6);
    if (t == 150 && music_on) music_start();
    if (t >= 200 || (nk && t > 20)) {
      state = ST_MENU; emb_s = 47000; emb_x = 190; emb_y = 226; sky_fade = 256;
      if (t < 150 && music_on) music_start();
      REG(VIDEO_BASE + VID_LAYER0 + VID_LAYER_STRIDE + LR_FLAGS) = LF_ENABLE;
    }
    return;
  }
  spin += 3; menu_f = ease(menu_f, state == ST_MENU ? 256 : 0, 5); panel_f = ease(panel_f, state == ST_PANEL ? 256 : 0, 5); sel_f = ease(sel_f, 18, 4);
  emb_x = ease(emb_x, state == ST_PANEL ? 150 : 190, 6);
  if (state == ST_MENU) {
    if ((nk & K_UP) && sel > 0) { sel--; sel_f = 0; sfx(SND_TICK); }
    if ((nk & K_DOWN) && sel < N_ITEMS - 1) { sel++; sel_f = 0; sfx(SND_TICK); }
    if (nk & K_OK) { panel_item = sel; list_sel = 0; set_sel = 0; if (sel == 0) scan_games(); state = ST_PANEL; sfx(SND_OPEN); }
  } else {
    if (nk & K_BACK) { state = ST_MENU; sfx(SND_BACK); }
    else if (panel_item == 0) {
      if ((nk & K_UP) && list_sel > 0) { list_sel--; sfx(SND_TICK); }
      if ((nk & K_DOWN) && list_sel < ngames - 1) { list_sel++; sfx(SND_TICK); }
      if ((nk & K_OK) && ngames > 0) { sfx(SND_OPEN); launch_game(list_sel); }
    } else if (panel_item == 4) {
      if ((nk & K_UP) && set_sel > 0) { set_sel--; sfx(SND_TICK); }
      if ((nk & K_DOWN) && set_sel < 2) { set_sel++; sfx(SND_TICK); }
      if (set_sel == 0 && (nk & K_LEFT)) { vol = imax(0, vol - 16); snd_master(vol); sfx(SND_TICK); }
      if (set_sel == 0 && (nk & K_RIGHT)) { vol = imin(256, vol + 16); snd_master(vol); sfx(SND_TICK); }
      if (set_sel == 1 && (nk & (K_OK | K_LEFT | K_RIGHT))) { music_on = !music_on; if (music_on) music_start(); else music_stop(); sfx(SND_OPEN); }
      if (set_sel == 2 && (nk & K_OK)) { bell(8, 72, -8); bell(9, 79, 0); bell(10, 84, 8); }
    }
  }
}
static void stats_update(unsigned busy_ns) {                   /* carga de cada chip no quadro (média lenta) */
  unsigned va = REG(VPUA_BASE + VPU_TOTAL_CYCLES), vb = REG(VPUB_BASE + VPU_TOTAL_CYCLES), dm = REG(VPUA_BASE + VPU_DMA_BYTES) + REG(VPUB_BASE + VPU_DMA_BYTES);
  int cpu = (int)(busy_ns / 166667), pa = (int)((va - last_vpua) / 25000), pb = (int)((vb - last_vpub) / 25000), g = (int)(REG(GPU_BASE + GPU_STAT_CYCLES) / 33333);
  last_vpua = va; last_vpub = vb;
  acc_cpu += (imin(cpu, 100) * 64 - acc_cpu) / 8; acc_vpua += (imin(pa, 100) * 64 - acc_vpua) / 8; acc_vpub += (imin(pb, 100) * 64 - acc_vpub) / 8; acc_gpu += (imin(g, 100) * 64 - acc_gpu) / 8;
  st_cpu = acc_cpu / 64; st_vpua = acc_vpua / 64; st_vpub = acc_vpub / 64; st_gpu = acc_gpu / 64;
  st_dma = (int)((dm - last_dma) / 1024); last_dma = dm;
  if ((tick & 31) == 0) { st_dsp = (int)(REG(PAULA_BASE + PA_DSP_LOAD) / 10); REG(PAULA_BASE + PA_DSP_DROPS) = 0; }
}

/* ------------------------------ inicialização ------------------------------ */
static void load_assets(void) {
  unsigned *h = (unsigned *)A_MESH, *sh = (unsigned *)A_SND, *w0, *w1, *w2; unsigned char *font = (unsigned char *)A_FONT; int i, n;
  ui_init();
  fs_load("sys/logo.raw", (void *)A_LOGO, 32768); fs_load("sys/icons.raw", (void *)A_ICONS, 65536);
  fs_load("sys/ui.raw", (void *)A_UIT, 0x30000); fs_load("sys/hud.raw", (void *)A_HUDS, 0x4000);
  fs_load("sys/emblem.msh", (void *)A_MESH, 0x30000); fs_load("sys/snd.raw", (void *)A_SND, 0x40000);
  if (vx_load_file(0, "sys/vx/geometry.vx", 0) < 0 || vx_load_file(1, "sys/vx/dynamics.vx", 0) < 0) os_printf("[menu] microprogramas SnowVector ausentes\n");
  for (i = 0; i < 3; i++) { part_first[i] = (int)h[2 + i * 2]; part_count[i] = (int)h[3 + i * 2]; }
  out_ring1 = 0; out_ring2 = part_count[2]; out_flame = out_ring2 + part_count[2]; out_crystal = out_flame + part_count[0]; out_total = out_crystal + part_count[1];
  os_cache_flush(A_MESH, 32 + (unsigned)(part_first[2] + part_count[2]) * 64);
  n = (int)sh[0];
  for (i = 0; i < n && i < 5; i++) { snd_off[i] = (int)sh[1 + i * 2]; snd_len[i] = (int)sh[2 + i * 2]; }
  snd_load(0, (void *)(A_SND + 4 + (unsigned)n * 8), (unsigned)(snd_off[n - 1] + snd_len[n - 1] * 2));
  /* fonte A8 -> três folhas ARGB8888 (branca, gelo, brasa) para o Blitter compor o HUD */
  w0 = (unsigned *)A_FONTS; w1 = (unsigned *)(A_FONTS + 0x20000); w2 = (unsigned *)(A_FONTS + 0x40000);
  for (i = 0; i < 256 * 128; i++) { unsigned a = (unsigned)font[i] << 24; w0[i] = a | 0xFFFFFFu; w1[i] = a | 0x8ED8FFu; w2[i] = a | 0xFFB070u; }
  os_cache_flush(A_FONTS, 0x60000); os_cache_flush(A_HUDS, 0x4000);
  /* superfícies do HUD transparentes; camada 1 = HUD (ligada ao fim do boot) */
  gx_blit(BOP_FILL, 0, 0, HUD0, SW * 4, SW, SH, 0); gx_blit(BOP_FILL, 0, 0, HUD1, SW * 4, SW, SH, 0);
  gx_layer(1, HUD0, SW * 4, 0);
  particles_init();
  REG(COP_BASE + COP_LIST) = A_COP; copper_sky(A_COP); REG(COP_BASE + COP_CTRL) = 1;
}

void app_main(void) {
  unsigned t0;
  load_assets(); mc_boot_counter();
  state = ST_BOOT; emb_x = 320; emb_y = 210;
  os_printf("[menu] SnowFlame Dashboard iniciado (boots no cartao: %d)\n", boot_count);
  os_printf("[menu] emblema: %d vertices em jobs de VPU-A; %d particulas na DMEM da VPU-B\n", part_count[0] + part_count[1] + part_count[2] * 2, NSNOW + NEMB);
  for (;;) {
    unsigned k = read_keys(), nk = k & ~keys_prev, rt = back ? RT1 : RT0, hud = back ? HUD1 : HUD0, cop = A_COP + (back ? 0x2000u : 0u);
    t0 = REG(BRIDGE_BASE + BR_TIME_LO);
    keys_prev = k;
    update(nk);
    emblem_submit(); particles_submit();          /* 1-2: as VPUs começam enquanto a CPU monta o resto */
    copper_sky(cop);
    vpus_wait();
    render(rt);                                   /* 3: FlameCore */
    if (state != ST_BOOT) hud_draw(hud);          /* 4: Blitter */
    stats_update(REG(BRIDGE_BASE + BR_TIME_LO) - t0);
    gx_layer(0, rt, SW * 4, LF_ENABLE | LF_PREMUL);
    if (state != ST_BOOT) gx_layer(1, hud, SW * 4, LF_ENABLE);
    REG(COP_BASE + COP_LIST) = cop;
    back = !back;
    gx_wait_vblank(); tick++;
  }
}
