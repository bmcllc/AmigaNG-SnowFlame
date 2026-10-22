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

/* ------------------------------ vértices ------------------------------
 * Vértice do FlameCore: 48 B (x y z w, r g b a, u v, 2 palavras de enchimento) com os campos vindos das tabelas de floats (sem FPU no snowcc).
 * O código do snowcc é ingênuo (cada operação passa pela pilha), então o caminho quente copia um molde pronto (cópia de struct = MOVE.L (A1)+,(A0)+
 * em fila) e só grava o que varia; cada quad calcula seus 4 cantos uma vez e duplica 2 (a ordem dos 6 vértices é a de sempre: saída idêntica). */
struct Vx { unsigned x, y, z, w, r, g, b, a, u, v, p0, p1; };
static struct Vx vt_one = { 0, 0, 0, 0x3F800000u, 0, 0, 0, 0, 0, 0, 0, 0 };   /* molde: w = 1,0 */
static struct Vx vt_col;                                                       /* molde com a cor do quad/texto corrente */
#define QCL(c) ((c) < -QOFF ? -QOFF : (c) > NQ - QOFF - 1 ? NQ - QOFF - 1 : (c))   /* coordenadas fora da tabela ficam presas na borda; só variáveis simples */
static unsigned *vp;                              /* próximo vértice (= VB + vn * 12) */
static void vcol(struct Vx *p, unsigned argb) { p->r = CT[(argb >> 16) & 255]; p->g = CT[(argb >> 8) & 255]; p->b = CT[argb & 255]; p->a = CT[argb >> 24]; }
static void vtx(int x, int y, int z, unsigned argb, int u, int v) {
  struct Vx *p = (struct Vx *)vp; unsigned *fq = FX + QOFF;
  x = QCL(x); y = QCL(y);
  p->x = fq[x]; p->y = fq[y]; p->z = z ? UVT[z] : 0; p->w = 0x3F800000u; vcol(p, argb);
  p->u = UVT[u]; p->v = UVT[v]; p->p0 = 0; p->p1 = 0;
  vp = (unsigned *)(p + 1); vn++;
}
static void quad4(int x0, int y0, int x1, int y1, unsigned c00, unsigned c10, unsigned c01, unsigned c11, int u0, int v0, int u1, int v1) {
  struct Vx *p; unsigned *fq = FX + QOFF, fx0, fx1, fy0, fy1, fu0, fu1, fv0, fv1;
  if (vn + 6 > MAXV) return;
  x0 = QCL(x0); x1 = QCL(x1); y0 = QCL(y0); y1 = QCL(y1);
  fx0 = fq[x0]; fx1 = fq[x1]; fy0 = fq[y0]; fy1 = fq[y1]; fu0 = UVT[u0]; fu1 = UVT[u1]; fv0 = UVT[v0]; fv1 = UVT[v1];
  p = (struct Vx *)vp;
  if (c00 == c10 && c00 == c01 && c00 == c11) { vt_col = vt_one; vcol(&vt_col, c00); p[0] = vt_col; p[1] = vt_col; p[2] = vt_col; p[5] = vt_col; }
  else { p[0] = vt_one; p[1] = vt_one; p[2] = vt_one; p[5] = vt_one; vcol(&p[0], c00); vcol(&p[1], c10); vcol(&p[2], c11); vcol(&p[5], c01); }
  p[0].x = fx0; p[0].y = fy0; p[0].u = fu0; p[0].v = fv0;
  p[1].x = fx1; p[1].y = fy0; p[1].u = fu1; p[1].v = fv0;
  p[2].x = fx1; p[2].y = fy1; p[2].u = fu1; p[2].v = fv1;
  p[5].x = fx0; p[5].y = fy1; p[5].u = fu0; p[5].v = fv1;
  p[3] = p[0]; p[4] = p[2];
  vp = (unsigned *)(p + 6); vn += 6;
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
  int q = x * 4, gh = 16 * sc / 100 * 4, y0 = y * 4, y1 = y0 + gh;
  unsigned *fq = FX + QOFF, fy0, fy1, fx0, fx1, fu0, fu1, fv0, fv1; struct Vx *p;
  y0 = QCL(y0); y1 = QCL(y1); fy0 = fq[y0]; fy1 = fq[y1];
  vt_col = vt_one; vcol(&vt_col, argb);                                           /* cor e y valem para a linha inteira */
  while (*s) {
    int c = *s++ & 127, a = adv[c];
    if (c > 32 && a && vn + 6 <= MAXV) {
      int x0 = q, x1 = q + (a + 2) * sc / 100 * 4, cy = (c >> 4) * 16, tx = (c & 15) * 16 + (16 - a) / 2 - 1;
      x0 = QCL(x0); x1 = QCL(x1);
      fx0 = fq[x0]; fx1 = fq[x1]; fu0 = UVT[tx * 4]; fu1 = UVT[(tx + a + 2) * 4]; fv0 = UVT[cy * 8]; fv1 = UVT[(cy + 16) * 8];
      p = (struct Vx *)vp; p[0] = vt_col; p[1] = vt_col; p[2] = vt_col; p[5] = vt_col;
      p[0].x = fx0; p[0].y = fy0; p[0].u = fu0; p[0].v = fv0;
      p[1].x = fx1; p[1].y = fy0; p[1].u = fu1; p[1].v = fv0;
      p[2].x = fx1; p[2].y = fy1; p[2].u = fu1; p[2].v = fv1;
      p[5].x = fx0; p[5].y = fy1; p[5].u = fu0; p[5].v = fv1;
      p[3] = p[0]; p[4] = p[2];
      vp = (unsigned *)(p + 6); vn += 6;
    }
    q += (a + 1) * sc / 100 * 4;
  }
}
static void text_c(int cx, int y, const char *s, int sc, unsigned argb) { text(cx - text_w(s, sc) / 2, y, s, sc, argb); }

/* cartão de memória: comando do SnowCard (1 ler, 2 programar, 3 apagar bloco); espera pelo IRQ do cartão (a tarefa dorme, o emulador pula o tempo ocioso)
 * em vez de girar em task_yield; 0 = ok, -1 = erro */
static Sem mc_sem; static int mc_irq;
static int mc_do(unsigned cmd, unsigned sector, unsigned count, unsigned addr) {
  unsigned st;
  if (!mc_irq) { sem_init(&mc_sem, 0); os_irq_attach(12, &mc_sem); REG(MCARD_BASE + MC_CTRL) = 1; mc_irq = 1; }
  mc_sem.count = 0; REG(MCARD_BASE + MC_STATUS) = 6;
  REG(MCARD_BASE + MC_SECTOR) = sector; REG(MCARD_BASE + MC_COUNT) = count; REG(MCARD_BASE + MC_ADDR) = addr; REG(MCARD_BASE + MC_CMD) = cmd;
  for (;;) { st = REG(MCARD_BASE + MC_STATUS); if (!(st & 1)) break; sem_wait_timeout(&mc_sem, 50); }
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
static void ui_begin(void) { vn = 0; vp = VB; nseg = 0; seg_first[0] = 0; seg_kind[0] = UI_OPAQUE; }
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
