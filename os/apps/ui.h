/* ui.h - biblioteca 2D dos aplicativos do SF-100 (menu e jogos): geometria em 1/4 de pixel escrita por tabelas de floats (sem FPU no snowcc),
 * texto por atlas A8 (sys/font.raw) com larguras proporcionais, segmentos de vértices por estado do FlameCore, cartão de memória.
 * Uso: #include "snowos_all.c" e depois #include "ui.h". */
#ifndef TF_A8
#define TF_A8 8
#endif
#ifndef TF_ARGB4444
#define TF_ARGB4444 2
#endif

#define SW 640
#define SH 480
#define QOFF 2048                       /* geometria em 1/4 de pixel; a tabela de floats cobre -512..+1535 px */
#define NQ 8192
#define A_FONT  0x01400000
#define A_FX    0x01440000              /* tabelas de floats: FX[NQ], UVT[1025], CT[256] */
#define A_UVT   (A_FX + NQ * 4)
#define A_CT    (A_UVT + 1032 * 4)
#define A_CMD   0x01480000
#define A_VB    0x01500000
#define A_MC    0x01600000
#define MAXV    9000
#define RT0 VRAM_BASE
#define RT1 (VRAM_BASE + 0x200000)

static unsigned *FX = (unsigned *)A_FX, *UVT = (unsigned *)A_UVT, *CT = (unsigned *)A_CT, *VB = (unsigned *)A_VB, *CMD = (unsigned *)A_CMD;
static int vn;
static unsigned char adv[128];

/* ------------------------------ matemática inteira ------------------------------ */
static int isin(int a) {                         /* a: 1024 = volta; resultado em +-65536 (parábola, ~1%) */
  int ph = a & 1023, h = ph & 511, v = h * (512 - h);
  return ph >= 512 ? -v : v;
}
static int icos(int a) { return isin(a + 256); }
static int iabs(int x) { return x < 0 ? -x : x; }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* ------------------------------ formatação ------------------------------ */
static int bput_num(char *b, int n, unsigned v, unsigned base, int neg, int width, int zero) {
  char tmp[16]; int k = 0, pad, o = n; const char *dig = "0123456789abcdef";
  if (v == 0) tmp[k++] = '0';
  while (v) { tmp[k++] = dig[v % base]; v = v / base; }
  pad = width - k - (neg ? 1 : 0);
  if (neg && zero) b[o++] = '-';
  while (pad-- > 0) b[o++] = zero ? '0' : ' ';
  if (neg && !zero) b[o++] = '-';
  while (k) b[o++] = tmp[--k];
  return o;
}
static void ksprintf(char *b, const char *fmt, ...) {
  va_list ap; int n = 0;
  va_start(ap, fmt);
  while (*fmt) {
    if (*fmt != '%') { b[n++] = *fmt++; continue; }
    fmt++;
    { int zero = 0, width = 0;
      if (*fmt == '0') { zero = 1; fmt++; }
      while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
      if (*fmt == 'd') { int v = va_arg(ap, int); n = v < 0 ? bput_num(b, n, (unsigned)-v, 10, 1, width, zero) : bput_num(b, n, (unsigned)v, 10, 0, width, zero); }
      else if (*fmt == 'u') n = bput_num(b, n, va_arg(ap, unsigned), 10, 0, width, zero);
      else if (*fmt == 'x') n = bput_num(b, n, va_arg(ap, unsigned), 16, 0, width, zero);
      else if (*fmt == 's') { const char *s = va_arg(ap, const char *); while (*s) b[n++] = *s++; }
      else if (*fmt == 'c') b[n++] = (char)va_arg(ap, int);
      else if (*fmt == '%') b[n++] = '%';
      if (*fmt) fmt++;
    }
  }
  b[n] = 0; va_end(ap);
}

/* ------------------------------ vértices ------------------------------ */
static void vtx(int x, int y, int z, unsigned argb, int u, int v) {
  unsigned *p = VB + vn * 12;
  if (x < -QOFF) x = -QOFF; else if (x > NQ - QOFF - 1) x = NQ - QOFF - 1;      /* coordenadas fora da tabela ficam presas na borda */
  if (y < -QOFF) y = -QOFF; else if (y > NQ - QOFF - 1) y = NQ - QOFF - 1;
  p[0] = FX[x + QOFF]; p[1] = FX[y + QOFF]; p[2] = UVT[z]; p[3] = 0x3F800000u;
  p[4] = CT[(argb >> 16) & 255]; p[5] = CT[(argb >> 8) & 255]; p[6] = CT[argb & 255]; p[7] = CT[argb >> 24];
  p[8] = UVT[u]; p[9] = UVT[v]; p[10] = 0; p[11] = 0;
  vn++;
}
static void quad4(int x0, int y0, int x1, int y1, unsigned c00, unsigned c10, unsigned c01, unsigned c11, int u0, int v0, int u1, int v1) {
  if (vn + 6 > MAXV) return;
  vtx(x0, y0, 0, c00, u0, v0); vtx(x1, y0, 0, c10, u1, v0); vtx(x1, y1, 0, c11, u1, v1);
  vtx(x0, y0, 0, c00, u0, v0); vtx(x1, y1, 0, c11, u1, v1); vtx(x0, y1, 0, c01, u0, v1);
}
static void quadc(int x0, int y0, int x1, int y1, unsigned c) { quad4(x0, y0, x1, y1, c, c, c, c, 0, 0, 0, 0); }
static unsigned with_alpha(unsigned argb, int a) { if (a > 255) a = 255; if (a < 0) a = 0; return ((unsigned)a << 24) | (argb & 0xFFFFFF); }
static unsigned mix(unsigned c0, unsigned c1, int t) {   /* t 0..256 */
  unsigned r = 0; int sh;
  for (sh = 0; sh < 32; sh += 8) r |= (unsigned)((((c0 >> sh) & 255) * (256 - t) + ((c1 >> sh) & 255) * t) >> 8) << sh;
  return r;
}

/* ------------------------------ texto ------------------------------ */
static int text_w(const char *s, int sc) { int w = 0; while (*s) { int c = *s++ & 127; w += ((int)adv[c] + 1) * sc / 100; } return w; }
static void text(int x, int y, const char *s, int sc, unsigned argb) {
  int q = x * 4;
  while (*s) {
    int c = *s++ & 127, a = adv[c], cx = (c & 15) * 16, cy = (c >> 4) * 16;
    if (c > 32 && a) {
      int gw = (a + 2) * sc / 100 * 4, gh = 16 * sc / 100 * 4, tx = cx + (16 - a) / 2 - 1;
      quad4(q, y * 4, q + gw, y * 4 + gh, argb, argb, argb, argb, tx * 4, cy * 8, (tx + a + 2) * 4, (cy + 16) * 8);
    }
    q += (a + 1) * sc / 100 * 4;
  }
}
static void text_c(int cx, int y, const char *s, int sc, unsigned argb) { text(cx - text_w(s, sc) / 2, y, s, sc, argb); }

static int mc_do(unsigned cmd, unsigned sector, unsigned count, unsigned addr) {
  unsigned st;
  REG(MCARD_BASE + MC_SECTOR) = sector; REG(MCARD_BASE + MC_COUNT) = count; REG(MCARD_BASE + MC_ADDR) = addr; REG(MCARD_BASE + MC_CMD) = cmd;
  do { st = REG(MCARD_BASE + MC_STATUS); if (st & 1) task_yield(); } while (st & 1);
  REG(MCARD_BASE + MC_STATUS) = 6;
  return (st & 4) ? -1 : 0;
}

/* ------------------------------ segmentos de vértices: cada um com seu estado (opaco, alfa, aditivo, texturizado) ------------------------------
 * UI_EXT intercala vértices de outro buffer (ex.: saída das SnowVector) com estado próprio do FlameCore, na ordem de chamada. */
#define UI_OPAQUE 0
#define UI_ALPHA 1
#define UI_ADD 2
#define UI_TEX 3
#define UI_TEXADD 4
#define UI_EXT 5
#define MAXSEG 48
static int seg_first[MAXSEG], seg_count[MAXSEG], seg_kind[MAXSEG], seg_lw[MAXSEG], seg_lh[MAXSEG], seg_fmt[MAXSEG], nseg;
static unsigned seg_addr[MAXSEG], seg_vb[MAXSEG], seg_state[MAXSEG], seg_bs[MAXSEG], seg_bd[MAXSEG];
static void ui_close(void) { if (seg_kind[nseg] != UI_EXT) seg_count[nseg] = vn - seg_first[nseg]; }
static void ui_begin(void) { vn = 0; nseg = 0; seg_first[0] = 0; seg_kind[0] = UI_OPAQUE; }
static void ui_seg(int kind) {
  ui_close();
  if (nseg > 0 && seg_kind[nseg] != UI_EXT && seg_count[nseg] == 0) nseg--;      /* descarta segmento vazio */
  if (nseg < MAXSEG - 1) nseg++;
  seg_first[nseg] = vn; seg_kind[nseg] = kind;
}
static void ui_seg_tex(unsigned addr, int lw, int lh, int fmt) { ui_seg(UI_TEX); seg_addr[nseg] = addr; seg_lw[nseg] = lw; seg_lh[nseg] = lh; seg_fmt[nseg] = fmt; }
static void ui_seg_texadd(unsigned addr, int lw, int lh, int fmt) { ui_seg(UI_TEXADD); seg_addr[nseg] = addr; seg_lw[nseg] = lw; seg_lh[nseg] = lh; seg_fmt[nseg] = fmt; }
/* desenho externo: 'count' vértices a partir de 'first' no buffer 'vb'; tex = 0 sem textura */
static void ui_ext(unsigned vb, int first, int count, unsigned state, unsigned bs, unsigned bd, unsigned tex, int lw, int lh, int fmt) {
  ui_seg(UI_EXT);
  seg_vb[nseg] = vb; seg_first[nseg] = first; seg_count[nseg] = count; seg_state[nseg] = state; seg_bs[nseg] = bs; seg_bd[nseg] = bd;
  seg_addr[nseg] = tex; seg_lw[nseg] = lw; seg_lh[nseg] = lh; seg_fmt[nseg] = fmt;
  ui_seg(UI_ALPHA);
}
static void ui_end(unsigned rt, unsigned clear_argb) {
  GxList l; int i; unsigned cur_vb = 0;
  ui_close(); nseg++;
  os_cache_flush(A_VB, (unsigned)vn * 48);
  gx_begin(&l, CMD, 4096);
  gx_set_rt(&l, rt, SW * 4, SW, SH);
  gx_clear(&l, clear_argb, 0x3F800000, CLR_COLOR | CLR_DEPTH);
  for (i = 0; i < nseg; i++) {
    int first = seg_first[i], count = seg_count[i], kind = seg_kind[i]; unsigned vb = kind == UI_EXT ? seg_vb[i] : A_VB;
    if (count <= 0) continue;
    if (vb != cur_vb) { gx_vertex_base(&l, vb); cur_vb = vb; }
    /* o alfa do alvo acumula cobertura ("sobre") e brilhos aditivos não o alteram: o alvo sai pré-multiplicado (camada LF_PREMUL) */
    if (kind == UI_OPAQUE) gx_state(&l, 0);
    else if (kind == UI_ADD) { gx_blend_sep(&l, BF_ONE, BF_ONE, BF_ZERO, BF_ONE); gx_state(&l, ST_BLEND); }
    else if (kind == UI_ALPHA) { gx_blend_sep(&l, BF_SRC_ALPHA, BF_INV_SRC_ALPHA, BF_ONE, BF_INV_SRC_ALPHA); gx_state(&l, ST_BLEND); }
    else if (kind == UI_EXT) {
      if (seg_bd[i] == BF_ONE) gx_blend_sep(&l, seg_bs[i], seg_bd[i], BF_ZERO, BF_ONE); else gx_blend_sep(&l, seg_bs[i], seg_bd[i], BF_ONE, BF_INV_SRC_ALPHA);
      if (seg_addr[i]) { gx_set_tex(&l, 0, seg_addr[i], seg_lw[i], seg_lh[i], seg_fmt[i], TFLAG_CLAMP, 1); gx_texenv(&l, 0, TE_MODULATE); }
      gx_state(&l, seg_state[i]);
    } else {
      if (kind == UI_TEXADD) gx_blend_sep(&l, BF_SRC_ALPHA, BF_ONE, BF_ZERO, BF_ONE); else gx_blend_sep(&l, BF_SRC_ALPHA, BF_INV_SRC_ALPHA, BF_ONE, BF_INV_SRC_ALPHA);
      gx_set_tex(&l, 0, seg_addr[i], seg_lw[i], seg_lh[i], seg_fmt[i], TFLAG_CLAMP, 1); gx_texenv(&l, 0, TE_MODULATE);
      gx_state(&l, ST_BLEND | ST_TEX0 | ST_BILINEAR);
    }
    gx_draw_arrays(&l, first, count, PRIM_TRIS);
  }
  gx_end(&l);
  gx_submit(&l);
}

/* ------------------------------ inicialização ------------------------------ */
static void ui_init(void) {
  int i, q;
  gx_init(); snd_init(); fs_mount();
  fs_load("sys/font.adv", adv, 128); fs_load("sys/font.raw", (void *)A_FONT, 32768);
  for (q = 0; q < NQ; q++) FX[q] = gx_f32_fx((q - QOFF) << 14);
  for (i = 0; i <= 1024; i++) UVT[i] = gx_f32_fx(i << 6);
  for (i = 0; i < 256; i++) CT[i] = gx_f32_fx((i << 16) / 255);
  gx_video_mode(VMODE_480P); gx_layer(0, RT0, SW * 4, LF_ENABLE); gx_video_start();
}
