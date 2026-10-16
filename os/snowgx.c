/* snowgx.c - SnowGX: biblioteca gráfica do SnowOS (listas de comandos do FlameCore, Blitter, Copper e vídeo) */
#include "snowos.h"

static Sem gx_gpu_sem, gx_blt_sem, gx_vbl_sem;

void gx_init(void) {
  sem_init(&gx_gpu_sem, 0); sem_init(&gx_blt_sem, 0); sem_init(&gx_vbl_sem, 0);
  os_irq_attach(6, &gx_gpu_sem); os_irq_attach(7, &gx_blt_sem);
}

void gx_begin(GxList *l, unsigned *buf, int cap) { l->buf = buf; l->n = 0; l->cap = cap; }
static void gx_cmd(GxList *l, unsigned op, int np) { if (l->n + 1 + np <= l->cap) l->buf[l->n++] = (op << 24) | (unsigned)np; }
static void gx_w(GxList *l, unsigned v) { if (l->n < l->cap) l->buf[l->n++] = v; }

void gx_set_rt(GxList *l, unsigned addr, unsigned pitch, int w, int h) { gx_cmd(l, GC_SET_RT, 3); gx_w(l, addr); gx_w(l, pitch); gx_w(l, (unsigned)w | ((unsigned)h << 16)); }
void gx_clear(GxList *l, unsigned argb, unsigned depth_bits, unsigned flags) { gx_cmd(l, GC_CLEAR, 3); gx_w(l, argb); gx_w(l, depth_bits); gx_w(l, flags); }
void gx_vertex_base(GxList *l, unsigned addr) { gx_cmd(l, GC_VERTEX_BASE, 1); gx_w(l, addr); }
void gx_state(GxList *l, unsigned flags) { gx_cmd(l, GC_STATE, 1); gx_w(l, flags); }
void gx_blend(GxList *l, unsigned src, unsigned dst) { gx_cmd(l, GC_BLEND, 2); gx_w(l, src); gx_w(l, dst); }
void gx_set_tex(GxList *l, int unit, unsigned addr, int lw, int lh, int fmt, unsigned flags, int nlevels) {
  gx_cmd(l, GC_SET_TEX, 4); gx_w(l, (unsigned)unit); gx_w(l, addr); gx_w(l, (unsigned)lw | ((unsigned)lh << 8) | ((unsigned)fmt << 16) | (flags << 24)); gx_w(l, (unsigned)nlevels);
}
void gx_set_palette(GxList *l, int unit, unsigned addr, int count) { gx_cmd(l, GC_SET_PALETTE, 3); gx_w(l, (unsigned)unit); gx_w(l, addr); gx_w(l, (unsigned)count); }
void gx_texenv(GxList *l, int unit, int mode) { gx_cmd(l, GC_TEXENV, 2); gx_w(l, (unsigned)unit); gx_w(l, (unsigned)mode); }
void gx_scissor(GxList *l, int x, int y, int w, int h) { gx_cmd(l, GC_SCISSOR, 4); gx_w(l, (unsigned)x); gx_w(l, (unsigned)y); gx_w(l, (unsigned)w); gx_w(l, (unsigned)h); }
void gx_draw_arrays(GxList *l, int first, int count, int prim) { gx_cmd(l, GC_DRAW_ARRAYS, 3); gx_w(l, (unsigned)first); gx_w(l, (unsigned)count); gx_w(l, (unsigned)prim); }
void gx_draw_indexed(GxList *l, unsigned idx_addr, int count, int prim) { gx_cmd(l, GC_DRAW_INDEXED, 3); gx_w(l, idx_addr); gx_w(l, (unsigned)count); gx_w(l, (unsigned)prim); }
int gx_end(GxList *l) { gx_cmd(l, GC_END, 0); return l->n; }

int gx_submit(GxList *l) {
  unsigned st;
  os_cache_flush((unsigned)l->buf, (unsigned)l->n * 4);
  while (REG(GPU_BASE + GPU_STATUS) & GPUS_BUSY) task_yield();
  gx_gpu_sem.count = 0;
  REG(GPU_BASE + GPU_CMD_ADDR) = (unsigned)l->buf; REG(GPU_BASE + GPU_CMD_LEN) = (unsigned)l->n; REG(GPU_BASE + GPU_CTRL) = 3;   /* START | IRQ */
  sem_wait(&gx_gpu_sem);
  st = REG(GPU_BASE + GPU_STATUS);
  return (st & GPUS_ERROR) ? -1 : 0;
}

/* inteiro -> float IEEE 754 (truncado): sem FPU no compilador, as bibliotecas montam os bits na mão */
unsigned gx_f32(int v) {
  unsigned s = 0, m; int e = 31;
  if (v == 0) return 0;
  if (v < 0) { s = 0x80000000u; m = (unsigned)-v; } else m = (unsigned)v;
  while (!(m & 0x80000000u)) { m <<= 1; e--; }
  return s | ((unsigned)(e + 127) << 23) | ((m >> 8) & 0x7FFFFF);
}
unsigned gx_f32_fx(int fx16) {
  unsigned s = 0, m; int e = 31;
  if (fx16 == 0) return 0;
  if (fx16 < 0) { s = 0x80000000u; m = (unsigned)-fx16; } else m = (unsigned)fx16;
  while (!(m & 0x80000000u)) { m <<= 1; e--; }
  return s | ((unsigned)(e - 16 + 127) << 23) | ((m >> 8) & 0x7FFFFF);
}
void gx_put_vertex(unsigned *v, int x_fx, int y_fx, int z_fx, unsigned argb, int u_fx, int v_fx) {
  v[0] = gx_f32_fx(x_fx); v[1] = gx_f32_fx(y_fx); v[2] = gx_f32_fx(z_fx); v[3] = 0x3F800000;                       /* x y z 1/w */
  v[4] = gx_f32_fx((int)((((argb >> 16) & 255) << 16) / 255)); v[5] = gx_f32_fx((int)((((argb >> 8) & 255) << 16) / 255));
  v[6] = gx_f32_fx((int)(((argb & 255) << 16) / 255)); v[7] = gx_f32_fx((int)(((argb >> 24) << 16) / 255));        /* r g b a */
  v[8] = gx_f32_fx(u_fx); v[9] = gx_f32_fx(v_fx); v[10] = 0; v[11] = 0;                                              /* u v fog pad */
}

/* ------------------------------ Blitter ------------------------------ */
static void gx_blt_go(void) {
  gx_blt_sem.count = 0;
  while (REG(BLT_BASE + BLT_STATUS) & 1) task_yield();
  REG(BLT_BASE + BLT_CTRL) = 3;                          /* START | IRQ */
  sem_wait(&gx_blt_sem);
}
void gx_blit(unsigned op, unsigned src, unsigned spitch, unsigned dst, unsigned dpitch, int w, int h, unsigned color) {
  REG(BLT_BASE + BLT_OP) = op; REG(BLT_BASE + BLT_SRC) = src; REG(BLT_BASE + BLT_SRC_PITCH) = spitch; REG(BLT_BASE + BLT_DST) = dst; REG(BLT_BASE + BLT_DST_PITCH) = dpitch;
  REG(BLT_BASE + BLT_W) = (unsigned)w; REG(BLT_BASE + BLT_H) = (unsigned)h; REG(BLT_BASE + BLT_COLOR) = color;
  if (src < 0x04000000) os_cache_flush(src, spitch * (unsigned)h);
  gx_blt_go();
}
void gx_blit_sprites(unsigned list, int count, unsigned dst, unsigned dpitch, int w, int h) {
  os_cache_flush(list, (unsigned)count * SPRITE_ENTRY_SIZE);
  REG(BLT_BASE + BLT_OP) = BOP_SPRITES; REG(BLT_BASE + BLT_SRC) = list; REG(BLT_BASE + BLT_COLOR) = (unsigned)count;
  REG(BLT_BASE + BLT_DST) = dst; REG(BLT_BASE + BLT_DST_PITCH) = dpitch; REG(BLT_BASE + BLT_W) = (unsigned)w; REG(BLT_BASE + BLT_H) = (unsigned)h;
  gx_blt_go();
}

/* ------------------------------ vídeo ------------------------------ */
void gx_video_mode(int mode) { REG(VIDEO_BASE + VID_MODE) = (unsigned)mode; }
void gx_layer(int i, unsigned addr, unsigned pitch, unsigned flags) {
  unsigned b = VIDEO_BASE + VID_LAYER0 + (unsigned)i * VID_LAYER_STRIDE;
  REG(b + LR_ADDR) = addr; REG(b + LR_PITCH) = pitch; REG(b + LR_FLAGS) = flags;
}
void gx_layer_scroll(int i, int x, int y) { unsigned b = VIDEO_BASE + VID_LAYER0 + (unsigned)i * VID_LAYER_STRIDE; REG(b + LR_SCX) = (unsigned)x; REG(b + LR_SCY) = (unsigned)y; }
void gx_video_start(void) { os_irq_attach(8, &gx_vbl_sem); REG(VIDEO_BASE + VID_CTRL) = REG(VIDEO_BASE + VID_CTRL) | VCTRL_VBLIRQ; }
void gx_wait_vblank(void) { gx_vbl_sem.count = 0; sem_wait(&gx_vbl_sem); }
