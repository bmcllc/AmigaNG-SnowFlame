/* kernel.c - SnowOS Native: kernel preemptivo para o SF-100 (ColdFire V4e)
 *
 * - Tarefas rodam em MODO USUÁRIO (pilha própria); o kernel roda em supervisor, com uma pilha de kernel por tarefa.
 * - Escalonador por prioridade com rodízio (fatia de 1 ms vinda do Timer 0 do SnowBridge).
 * - Chamadas de sistema por TRAP #0 (d0 = número, d1/d2 = argumentos, resultado em d0); os manipuladores usam o estágio
 *   __interrupt do snowcc: recebem o ponteiro do contexto salvo e devolvem o ponteiro do contexto que deve continuar.
 * - Uma exceção em tarefa de usuário (instrução ilegal, privilégio, divisão por zero, acesso inválido) mata só a tarefa.
 * - Proteção de memória: a MMU (TLB de software, mapa identidade) só traduz o modo usuário; o kernel roda sem tradução. As falhas de TLB
 *   (vetor 2) são resolvidas por 'user_perm': código somente-leitura, dados/heap RW, pilha própria RW (com página de guarda), RAM de vídeo/áudio
 *   RW; pilhas de outras tarefas, pilhas do kernel e a página zero são inacessíveis e a tarefa que as tocar é encerrada.
 */
#include "snowos.h"

#define MAX_TASKS 16
#define KSTACK 1024
#define TICK_US 1000
#define PAGE 4096u
#define HEAP_END 0x00C00000u                  /* heap do kernel: [_end, HEAP_END) */
#define USTK_BASE 0x00C00000u                 /* arena de pilhas de usuário (páginas; 1 página de guarda abaixo de cada pilha) */
#define USTK_PAGES 512
#define KSTK_BASE 0x00E00000u                 /* pilhas do kernel (KSTACK por tarefa); nunca mapeadas para o usuário */
#define USER_FREE_LO 0x01000000u              /* DDR livre para a aplicação (buffers, texturas) */
#define USER_FREE_HI 0x04000000u

enum { T_FREE = 0, T_READY, T_RUN, T_SLEEP, T_BLOCK, T_DEAD };
enum { SYS_START, SYS_YIELD, SYS_SLEEP, SYS_EXIT, SYS_CREATE, SYS_SEM_WAIT, SYS_SEM_POST, SYS_SEM_TRY, SYS_MALLOC, SYS_FREE, SYS_SELF, SYS_STATE,
       SYS_SEM_WAIT_TO, SYS_HEAPFREE, SYS_KILL, SYS_HALT, SYS_IRQ_ATTACH };

struct Ctx { unsigned d[8]; unsigned a[7]; unsigned pad; unsigned fmt_sr; unsigned pc; };
struct Task { unsigned ksp, usp, wake, wseq, kbase, ubase, ulen; int state, prio; Sem *wait; };
struct Blk { unsigned size; unsigned used; struct Blk *next; };

static struct Task tasks[MAX_TASKS];
static int cur;
static volatile unsigned os_tick;
static unsigned wseq_ctr;
static Sem *irq_sem[16];
static unsigned vectors[64];
static struct Blk *heap_head;
volatile int os_exit_code;
extern char _end;
extern char _code_end;
static unsigned char ustk_used[USTK_PAGES];
static int mmu_active;

/* ------------------------------ heap (primeiro-que-couber, com divisão e fusão) ------------------------------ */
static void heap_init(unsigned start, unsigned end) {
  heap_head = (struct Blk *)start; heap_head->size = end - start - 12; heap_head->used = 0; heap_head->next = 0;
}
static void *k_malloc(unsigned n) {
  struct Blk *b; n = (n + 3) & ~3u; if (n == 0) n = 4;
  for (b = heap_head; b; b = b->next) {
    if (!b->used && b->size >= n) {
      if (b->size >= n + 12 + 16) {
        struct Blk *r = (struct Blk *)((unsigned)b + 12 + n);
        r->size = b->size - n - 12; r->used = 0; r->next = b->next; b->size = n; b->next = r;
      }
      b->used = 1; return (void *)((unsigned)b + 12);
    }
  }
  return 0;
}
static void k_free(void *p) {
  struct Blk *b, *n;
  if (!p) return;
  ((struct Blk *)((unsigned)p - 12))->used = 0;
  for (b = heap_head; b; b = b->next) {
    while (!b->used && b->next && !b->next->used) { n = b->next; b->size += 12 + n->size; b->next = n->next; }
  }
}
static unsigned k_heap_free(void) { unsigned t = 0; struct Blk *b; for (b = heap_head; b; b = b->next) if (!b->used) t += b->size; return t; }

/* ------------------------------ pilhas de usuário e MMU ------------------------------ */
static unsigned ustk_alloc(unsigned bytes) {        /* devolve o endereço da pilha (alinhado a página) ou 0; reserva uma página de guarda abaixo */
  unsigned np = (bytes + PAGE - 1) / PAGE, i, run = 0;
  for (i = 0; i < USTK_PAGES; i++) {
    run = ustk_used[i] ? 0 : run + 1;
    if (run == np + 1) {
      unsigned s = i + 1 - run, k;
      for (k = 0; k < run; k++) ustk_used[s + k] = 1;
      return USTK_BASE + (s + 1) * PAGE;
    }
  }
  return 0;
}
static void ustk_free(unsigned base, unsigned bytes) {
  unsigned np = (bytes + PAGE - 1) / PAGE, s = (base - USTK_BASE) / PAGE - 1, k;
  for (k = 0; k < np + 1; k++) ustk_used[s + k] = 0;
}
/* permissões (MP_*) que o usuário tem sobre o endereço; 0 = proibido */
static unsigned user_perm(unsigned a, struct Task *t) {
  unsigned cpg = (unsigned)&_code_end & ~(PAGE - 1);
  if (a < PAGE) return 0;                                                   /* página zero: ponteiro nulo */
  if (a < cpg) return MP_R | MP_X;
  if (a < cpg + PAGE) return MP_R | MP_W | MP_X;
  if (a < HEAP_END) return MP_R | MP_W;
  if (a >= USTK_BASE && a < KSTK_BASE) return (a >= t->ubase && a < t->ubase + t->ulen) ? (MP_R | MP_W) : 0;
  if (a >= USER_FREE_LO && a < USER_FREE_HI) return MP_R | MP_W;
  if (a >= 0x10000000u && a < 0x12000000u) return MP_R | MP_W;             /* VRAM */
  if (a >= 0x20000000u && a < 0x20400000u) return MP_R | MP_W;             /* RAM de áudio */
  if (a >= 0x30000000u && a < 0x30008000u) return MP_R | MP_W;             /* SRAM */
  return 0;
}
static void mmu_drop_stack(struct Task *t) {          /* a TLB não tem ASID: esquece as páginas da pilha de quem sai */
  unsigned p;
  if (!mmu_active || !t->ulen) return;
  for (p = t->ubase; p < t->ubase + t->ulen; p += PAGE) { REG(MMU_BASE + MMU_AR) = p; REG(MMU_BASE + MMU_OP) = 3; }
}

/* ------------------------------ escalonador ------------------------------ */
static int pick(void) {
  int best = -1, bp = -1, k, i, st;
  for (k = 1; k <= MAX_TASKS; k++) {
    i = (cur + k) % MAX_TASKS; st = tasks[i].state;
    if ((st == T_READY || st == T_RUN) && tasks[i].prio > bp) { best = i; bp = tasks[i].prio; }
  }
  return best;
}
static unsigned switch_to(unsigned sp) {
  struct Task *t = &tasks[cur];
  t->ksp = sp; t->usp = __get_usp(); mmu_drop_stack(t);
  if (t->state == T_RUN) t->state = T_READY;
  cur = pick(); t = &tasks[cur]; t->state = T_RUN;
  __set_usp(t->usp);
  return t->ksp;
}

static void k_sem_post(Sem *s) {
  int w = -1, i; unsigned bs = 0;
  for (i = 0; i < MAX_TASKS; i++)
    if (tasks[i].state == T_BLOCK && tasks[i].wait == s && (w < 0 || (int)(tasks[i].wseq - bs) < 0)) { w = i; bs = tasks[i].wseq; }
  if (w < 0) { s->count++; return; }
  tasks[w].state = T_READY; tasks[w].wait = 0; tasks[w].wake = 0; ((struct Ctx *)tasks[w].ksp)->d[0] = 1;
}

void os_task_exit(void) { __trapcall(0, SYS_EXIT, 0, 0); for (;;) { } }

static int k_task_create(void (*entry)(void), unsigned ssize, int prio) {
  int i, slot = -1; struct Task *t; unsigned ub, kb, top, fsp, usp; struct Ctx *c;
  for (i = 1; i < MAX_TASKS; i++) {                      /* recolhe tarefas mortas (suas pilhas) e acha um slot */
    if (i >= 2 && tasks[i].state == T_DEAD) { ustk_free(tasks[i].ubase, tasks[i].ulen); tasks[i].ulen = 0; tasks[i].state = T_FREE; }
    if (slot < 0 && tasks[i].state == T_FREE) slot = i;
  }
  if (slot < 0) return -1;
  if (ssize < 256) ssize = 256;
  ssize = (ssize + PAGE - 1) & ~(PAGE - 1);
  ub = ustk_alloc(ssize); kb = KSTK_BASE + (unsigned)slot * KSTACK;
  if (!ub) return -1;
  top = (kb + KSTACK) & ~3u; fsp = top - 72; c = (struct Ctx *)fsp;
  for (i = 0; i < 18; i++) ((unsigned *)fsp)[i] = 0;
  c->fmt_sr = (4u << 28) | (32u << 18);                  /* SR = 0: modo usuário, interrupções habilitadas */
  c->pc = (unsigned)entry;
  usp = ((ub + ssize) & ~3u) - 4; *(unsigned *)usp = (unsigned)&os_task_exit;   /* retorno da função de entrada = encerrar a tarefa */
  t = &tasks[slot]; t->ksp = fsp; t->usp = usp; t->kbase = kb; t->ubase = ub; t->ulen = ssize; t->state = T_READY; t->prio = prio; t->wake = 0; t->wait = 0; t->wseq = 0;
  return slot;
}

static void kill_task(int id) {
  if (id < 2 || id >= MAX_TASKS) return;
  if (tasks[id].state == T_FREE || tasks[id].state == T_DEAD) return;
  tasks[id].state = T_DEAD; tasks[id].wait = 0;
}

/* ------------------------------ manipuladores ------------------------------ */
__interrupt unsigned os_irq(unsigned sp) {
  unsigned st = REG(BRIDGE_BASE + BR_IRQ_STATUS) & REG(BRIDGE_BASE + BR_IRQ_MASK);
  int resched = 0, b, i;
  if (st & IRQ_TIMER) {
    REG(BRIDGE_BASE + BR_IRQ_STATUS) = IRQ_TIMER; os_tick++; resched = 1;
    for (i = 1; i < MAX_TASKS; i++) {
      struct Task *t = &tasks[i];
      if (t->state == T_SLEEP && (int)(os_tick - t->wake) >= 0) t->state = T_READY;
      else if (t->state == T_BLOCK && t->wake && (int)(os_tick - t->wake) >= 0) { t->state = T_READY; t->wait = 0; t->wake = 0; ((struct Ctx *)t->ksp)->d[0] = 0; }
    }
  }
  for (b = 0; b < 16; b++) {
    unsigned m = 1u << b;
    if (b == 10 || !(st & m)) continue;
    REG(BRIDGE_BASE + BR_IRQ_STATUS) = m;
    if (irq_sem[b]) { k_sem_post(irq_sem[b]); resched = 1; } else REG(BRIDGE_BASE + BR_IRQ_MASK) = REG(BRIDGE_BASE + BR_IRQ_MASK) & ~m;   /* sem dono: desliga */
  }
  if (resched) return switch_to(sp);
  return sp;
}

__interrupt unsigned os_fault(unsigned sp) {
  struct Ctx *c = (struct Ctx *)sp;
  unsigned vec = (c->fmt_sr >> 18) & 0xFF;
  if (c->fmt_sr & 0x2000) { os_printf("[os] PANIC: vetor %d no kernel, pc=%x\n", vec, c->pc); os_exit_code = -1; __halt(); }
  if (vec == 2 && mmu_active && (REG(MMU_BASE + MMU_FSR) & 3) == 1) {   /* falha de TLB: preenche se o mapa permitir e refaz a instrução */
    unsigned va = REG(MMU_BASE + MMU_FAR) & ~(PAGE - 1), perm = user_perm(va, &tasks[cur]);
    if (perm) { REG(MMU_BASE + MMU_FSR) = 0; REG(MMU_BASE + MMU_AR) = va; REG(MMU_BASE + MMU_TR) = va | perm; REG(MMU_BASE + MMU_OP) = 1; return sp; }
  }
  if (vec == 2) os_printf("[os] tarefa %d encerrada: acesso inválido a %x em pc=%x\n", cur, REG(MMU_BASE + MMU_FAR), c->pc);
  else os_printf("[os] tarefa %d encerrada: exceção vetor %d em pc=%x\n", cur, vec, c->pc);
  tasks[cur].state = T_DEAD; tasks[cur].wait = 0;
  return switch_to(sp);
}

__interrupt unsigned os_syscall(unsigned sp) {
  struct Ctx *c; unsigned num, a, b; struct Task *t; int sw = 0;
  __set_sr(0x2700);                                      /* o TRAP não mascara interrupções: o kernel não é reentrante */
  c = (struct Ctx *)sp; num = c->d[0]; a = c->d[1]; b = c->d[2]; t = &tasks[cur]; c->d[0] = 0;
  switch (num) {
    case SYS_START: tasks[0].state = T_DEAD; sw = 1; break;
    case SYS_YIELD: sw = 1; break;
    case SYS_SLEEP: if (a) { t->state = T_SLEEP; t->wake = os_tick + a; } sw = 1; break;
    case SYS_EXIT: t->state = T_DEAD; sw = 1; break;
    case SYS_CREATE: c->d[0] = (unsigned)k_task_create((void (*)(void))a, b & 0xFFFFFF, (int)(b >> 24)); sw = 1; break;
    case SYS_SEM_WAIT: case SYS_SEM_WAIT_TO: {
      Sem *s = (Sem *)a;
      if (s->count > 0) { s->count--; c->d[0] = 1; }
      else if (num == SYS_SEM_WAIT_TO && b == 0) c->d[0] = 0;
      else { t->state = T_BLOCK; t->wait = s; t->wseq = ++wseq_ctr; t->wake = num == SYS_SEM_WAIT_TO ? os_tick + b : 0; sw = 1; }
      break;
    }
    case SYS_SEM_POST: k_sem_post((Sem *)a); sw = 1; break;
    case SYS_SEM_TRY: { Sem *s = (Sem *)a; if (s->count > 0) { s->count--; c->d[0] = 1; } break; }
    case SYS_MALLOC: c->d[0] = (unsigned)k_malloc(a); break;
    case SYS_FREE: k_free((void *)a); break;
    case SYS_SELF: c->d[0] = (unsigned)cur; break;
    case SYS_STATE: c->d[0] = (a < MAX_TASKS) ? (unsigned)tasks[a].state : 0; break;
    case SYS_HEAPFREE: c->d[0] = k_heap_free(); break;
    case SYS_KILL: kill_task((int)a); c->d[0] = 0; if ((int)a == cur) sw = 1; break;
    case SYS_HALT: os_exit_code = (int)a; __halt(); break;
    case SYS_IRQ_ATTACH: irq_sem[a & 15] = (Sem *)b; if (b) REG(BRIDGE_BASE + BR_IRQ_MASK) = REG(BRIDGE_BASE + BR_IRQ_MASK) | (1u << (a & 15)); else REG(BRIDGE_BASE + BR_IRQ_MASK) = REG(BRIDGE_BASE + BR_IRQ_MASK) & ~(1u << (a & 15)); break;
    default: c->d[0] = (unsigned)-1;
  }
  if (sw) return switch_to(sp);
  return sp;
}

/* ------------------------------ API de usuário ------------------------------ */
int task_create(void (*entry)(void), unsigned stack_size, int prio) { return __trapcall(0, SYS_CREATE, (int)entry, (int)((stack_size & 0xFFFFFF) | ((unsigned)prio << 24))); }
void task_exit(void) { os_task_exit(); }
int task_self(void) { return __trapcall(0, SYS_SELF, 0, 0); }
void task_yield(void) { __trapcall(0, SYS_YIELD, 0, 0); }
void task_sleep(unsigned ms) { __trapcall(0, SYS_SLEEP, (int)ms, 0); }
int task_state(int id) { return __trapcall(0, SYS_STATE, id, 0); }
int task_kill(int id) { return __trapcall(0, SYS_KILL, id, 0); }
unsigned os_ticks(void) { return os_tick; }
void os_exit(int code) { __trapcall(0, SYS_HALT, code, 0); for (;;) { } }

void sem_init(Sem *s, int initial) { s->count = initial; s->pad = 0; }
void sem_wait(Sem *s) { __trapcall(0, SYS_SEM_WAIT, (int)s, 0); }
int sem_trywait(Sem *s) { return __trapcall(0, SYS_SEM_TRY, (int)s, 0); }
int sem_wait_timeout(Sem *s, unsigned ms) { return __trapcall(0, SYS_SEM_WAIT_TO, (int)s, (int)ms); }
void sem_post(Sem *s) { __trapcall(0, SYS_SEM_POST, (int)s, 0); }

void queue_init(Queue *q, unsigned *storage, int capacity) {
  sem_init(&q->items, 0); sem_init(&q->space, capacity); sem_init(&q->lock, 1); q->buf = storage; q->cap = capacity; q->head = 0; q->tail = 0;
}
void queue_send(Queue *q, unsigned v) {
  sem_wait(&q->space); sem_wait(&q->lock); q->buf[q->head] = v; q->head = (q->head + 1) % q->cap; sem_post(&q->lock); sem_post(&q->items);
}
unsigned queue_recv(Queue *q) {
  unsigned v; sem_wait(&q->items); sem_wait(&q->lock); v = q->buf[q->tail]; q->tail = (q->tail + 1) % q->cap; sem_post(&q->lock); sem_post(&q->space); return v;
}
int queue_tryrecv(Queue *q, unsigned *v) {
  if (!sem_trywait(&q->items)) return 0;
  sem_wait(&q->lock); *v = q->buf[q->tail]; q->tail = (q->tail + 1) % q->cap; sem_post(&q->lock); sem_post(&q->space); return 1;
}

void *os_malloc(unsigned n) { return (void *)__trapcall(0, SYS_MALLOC, (int)n, 0); }
void os_free(void *p) { __trapcall(0, SYS_FREE, (int)p, 0); }
unsigned os_heap_free(void) { return (unsigned)__trapcall(0, SYS_HEAPFREE, 0, 0); }
void os_irq_attach(int bit, Sem *sem) { __trapcall(0, SYS_IRQ_ATTACH, bit, (int)sem); }

/* ------------------------------ console / printf ------------------------------ */
void os_putc(int c) { REG(DBG_BASE) = (unsigned)c; }
void os_puts(const char *s) { while (*s) os_putc(*s++); }
static void put_num(unsigned v, unsigned base, int neg, int width, int zero, int upper) {
  char tmp[34]; int n = 0, pad; const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  if (v == 0) tmp[n++] = '0';
  while (v) { tmp[n++] = dig[v % base]; v = v / base; }
  pad = width - n - (neg ? 1 : 0);
  if (neg && zero) os_putc('-');
  while (pad-- > 0) os_putc(zero ? '0' : ' ');
  if (neg && !zero) os_putc('-');
  while (n) os_putc(tmp[--n]);
}
int os_printf(const char *fmt, ...) {
  va_list ap; int count = 0;
  va_start(ap, fmt);
  while (*fmt) {
    if (*fmt != '%') { os_putc(*fmt++); count++; continue; }
    fmt++;
    { int zero = 0, width = 0;
      if (*fmt == '0') { zero = 1; fmt++; }
      while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
      switch (*fmt) {
        case 'd': { int v = va_arg(ap, int); if (v < 0) put_num((unsigned)-v, 10, 1, width, zero, 0); else put_num((unsigned)v, 10, 0, width, zero, 0); break; }
        case 'u': put_num(va_arg(ap, unsigned), 10, 0, width, zero, 0); break;
        case 'x': put_num(va_arg(ap, unsigned), 16, 0, width, zero, 0); break;
        case 'X': put_num(va_arg(ap, unsigned), 16, 0, width, zero, 1); break;
        case 'c': os_putc(va_arg(ap, int)); break;
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; while (*s) os_putc(*s++); break; }
        case '%': os_putc('%'); break;
        default: os_putc('%'); os_putc(*fmt);
      }
      if (*fmt) fmt++;
    }
    count++;
  }
  va_end(ap);
  return count;
}

/* ------------------------------ cache e DMA ------------------------------ */
void os_cache_flush(unsigned addr, unsigned len) {
  REG(BRIDGE_BASE + BR_CACHE_ADDR) = addr; REG(BRIDGE_BASE + BR_CACHE_LEN) = len; REG(BRIDGE_BASE + BR_CACHE_OP) = CACHE_FLUSH_RANGE;
}
void os_cache_invalidate(unsigned addr, unsigned len) {
  REG(BRIDGE_BASE + BR_CACHE_ADDR) = addr; REG(BRIDGE_BASE + BR_CACHE_LEN) = len; REG(BRIDGE_BASE + BR_CACHE_OP) = CACHE_INVAL_RANGE;
}
void os_icache_invalidate(unsigned addr, unsigned len) {
  REG(BRIDGE_BASE + BR_CACHE_ADDR) = addr; REG(BRIDGE_BASE + BR_CACHE_LEN) = len; REG(BRIDGE_BASE + BR_CACHE_OP) = ICACHE_INVAL_RANGE;
}
int dma_copy(unsigned dst, unsigned src, unsigned len, unsigned flags) {
  unsigned st;
  if (!(flags & DMAF_FILL) && src < 0x04000000) os_cache_flush(src, len);
  while (REG(BRIDGE_BASE + BR_DMA0 + DMA_STATUS) & DMAS_BUSY) task_yield();
  REG(BRIDGE_BASE + BR_DMA0 + DMA_SRC) = src; REG(BRIDGE_BASE + BR_DMA0 + DMA_DST) = dst; REG(BRIDGE_BASE + BR_DMA0 + DMA_LEN) = len;
  REG(BRIDGE_BASE + BR_DMA0 + DMA_FLAGS) = flags & ~DMAF_IRQ; REG(BRIDGE_BASE + BR_DMA0 + DMA_CTRL) = DMAC_START_REGS;
  do { st = REG(BRIDGE_BASE + BR_DMA0 + DMA_STATUS); if (st & DMAS_BUSY) task_yield(); } while (st & DMAS_BUSY);
  if (dst < 0x04000000) os_cache_invalidate(dst, len);
  return (st & DMAS_ERROR) ? -1 : 0;
}

/* ------------------------------ boot ------------------------------ */
static void idle_task(void) { for (;;) { } }
static void app_wrapper(void) { app_main(); os_exit(0); }

int main(void) {
  int i;
  heap_init(((unsigned)&_end + 15) & ~15u, HEAP_END);
  for (i = 0; i < 64; i++) vectors[i] = (unsigned)&os_fault;
  vectors[28] = (unsigned)&os_irq; vectors[32] = (unsigned)&os_syscall;
  __set_vbr((unsigned)vectors);
  for (i = 0; i < MAX_TASKS; i++) tasks[i].state = T_FREE;
  tasks[0].state = T_RUN; tasks[0].prio = 100; cur = 0;
  k_task_create(idle_task, 256, 0);
  k_task_create(app_wrapper, 16384, 5);
  REG(TIMER_BASE + TMR_LOAD) = TICK_US; REG(TIMER_BASE + TMR_CTRL) = 7;
  REG(BRIDGE_BASE + BR_IRQ_MASK) = IRQ_TIMER;
  REG(MMU_BASE + MMU_CTRL) = 3; mmu_active = 1;           /* MMU ligada, kernel (supervisor) sem tradução */
  __set_sr(0x2000);
  __trapcall(0, SYS_START, 0, 0);
  for (;;) { }
}
