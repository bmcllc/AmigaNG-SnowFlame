/* snowvx.c - SnowVX: biblioteca das unidades vetoriais SnowVector-128 (carga de microprograma, dados e execução) */
#include "snowos.h"

static Sem vx_sem[2];
static int vx_ready;

static unsigned vbase(int vpu) { return vpu ? VPUB_BASE : VPUA_BASE; }
static unsigned vimem(int vpu) { return vpu ? VPUB_IMEM : VPUA_IMEM; }
static unsigned vdmem(int vpu) { return vpu ? VPUB_DMEM : VPUA_DMEM; }
static void vx_setup(void) {
  if (vx_ready) return; vx_ready = 1;
  sem_init(&vx_sem[0], 0); sem_init(&vx_sem[1], 0); os_irq_attach(4, &vx_sem[0]); os_irq_attach(5, &vx_sem[1]);
}

void vx_load_program(int vpu, const unsigned *code, int nwords) { int i; unsigned d = vimem(vpu); for (i = 0; i < nwords; i++) REG(d + 4 * (unsigned)i) = code[i]; }
void vx_load_data(int vpu, unsigned off, const unsigned *src, int nwords) { int i; unsigned d = vdmem(vpu) + off; for (i = 0; i < nwords; i++) REG(d + 4 * (unsigned)i) = src[i]; }
void vx_read_data(int vpu, unsigned off, unsigned *dst, int nwords) { int i; unsigned d = vdmem(vpu) + off; for (i = 0; i < nwords; i++) dst[i] = REG(d + 4 * (unsigned)i); }
unsigned vx_cycles(int vpu) { return REG(vbase(vpu) + VPU_CYCLES); }

int vx_run(int vpu, unsigned pc, unsigned s0, unsigned s1, unsigned s2, unsigned s3) {
  unsigned b = vbase(vpu), st;
  vx_setup();
  REG(b + VPU_PC) = pc; REG(b + VPU_S0) = s0; REG(b + VPU_S0 + 4) = s1; REG(b + VPU_S0 + 8) = s2; REG(b + VPU_S0 + 12) = s3;
  vx_sem[vpu].count = 0;
  REG(b + VPU_CTRL) = VPUC_START | VPUC_IRQ;
  sem_wait(&vx_sem[vpu]);
  st = REG(b + VPU_STATUS);
  return (st & VPUS_FAULT) ? -1 : 0;
}

/* ---------------- filas de jobs, DMA e semáforos (CPU não fica bloqueada) ---------------- */
int vx_submit(int vpu, unsigned pc, const unsigned *s, int ns) {
  unsigned b = vbase(vpu); int i;
  if (((REG(b + VPU_STATUS) >> 8) & 0xFF) + 1 >= VPU_QUEUE_DEPTH) return -1;   /* fila (+ job em execução) cheia */
  REG(b + VPU_PC) = pc;
  for (i = 0; i < 8; i++) REG(b + VPU_S0 + 4 * (unsigned)i) = i < ns ? s[i] : 0;
  REG(b + VPU_CTRL) = VPUC_START;
  return 0;
}
int vx_busy(int vpu) { return (REG(vbase(vpu) + VPU_STATUS) & (VPUS_BUSY | 0xFF00)) != 0; }
void vx_wait(int vpu) { while (vx_busy(vpu)) task_yield(); }
int vx_sem_take(int n) { return REG(BRIDGE_BASE + BR_SEM0 + 4 * (unsigned)n) > 0; }
void vx_sem_wait(int n) { while (!vx_sem_take(n)) task_yield(); }
void vx_sem_reset(int n) { while (vx_sem_take(n)) ; }
int vx_dma_to_dmem(int vpu, unsigned dmem_off, const void *src, unsigned len) { return dma_copy(vdmem(vpu) + dmem_off, (unsigned)src, len, 0); }
int vx_load_file(int vpu, const char *path, unsigned at_word) {
  unsigned *buf = (unsigned *)os_malloc(VPU_IMEM_SIZE); int n;
  if (!buf) return -1;
  n = fs_load(path, buf, VPU_IMEM_SIZE - at_word * 4);
  if (n > 0 && dma_copy(vimem(vpu) + at_word * 4, (unsigned)buf, (unsigned)n, 0)) n = -1;
  os_free(buf);
  return n > 0 ? n / 4 : -1;
}
