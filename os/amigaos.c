/* amigaos.c - camada mínima exec.library / dos.library sobre o SnowOS Native (listas, memória, sinais, portas de mensagem, arquivos) */
#include "snowos.h"

#define AM_TASKS 16
struct AmTask { const char *name; unsigned sig_alloc, sig_recvd; int id, used; Sem sem; };

static struct AmTask am_tasks[AM_TASKS];
static Sem am_lock, am_forbid;
static int am_ready, am_ioerr, forbid_owner = -1, forbid_depth;
static unsigned char am_files_inited;

static void am_init(void) {
  int i;
  if (am_ready) return;
  sem_init(&am_lock, 1); sem_init(&am_forbid, 1);
  for (i = 0; i < AM_TASKS; i++) { am_tasks[i].id = i; sem_init(&am_tasks[i].sem, 0); }
  am_ready = 1;
}
static struct AmTask *am_self(void) { struct AmTask *t; am_init(); t = &am_tasks[task_self() & 15]; t->used = 1; return t; }

/* ------------------------------ listas ------------------------------ */
void NewList(struct List *l) { l->lh_Head = (struct Node *)&l->lh_Tail; l->lh_Tail = 0; l->lh_TailPred = (struct Node *)&l->lh_Head; }
void AddHead(struct List *l, struct Node *n) { n->ln_Succ = l->lh_Head; n->ln_Pred = (struct Node *)&l->lh_Head; l->lh_Head->ln_Pred = n; l->lh_Head = n; }
void AddTail(struct List *l, struct Node *n) { n->ln_Succ = (struct Node *)&l->lh_Tail; n->ln_Pred = l->lh_TailPred; l->lh_TailPred->ln_Succ = n; l->lh_TailPred = n; }
void Remove(struct Node *n) { n->ln_Pred->ln_Succ = n->ln_Succ; n->ln_Succ->ln_Pred = n->ln_Pred; }
struct Node *RemHead(struct List *l) { struct Node *n = l->lh_Head; if (!n->ln_Succ) return 0; Remove(n); return n; }
struct Node *RemTail(struct List *l) { struct Node *n = l->lh_TailPred; if (!n->ln_Pred) return 0; Remove(n); return n; }
void Insert(struct List *l, struct Node *n, struct Node *pred) {
  if (!pred) { AddHead(l, n); return; }
  n->ln_Succ = pred->ln_Succ; n->ln_Pred = pred; pred->ln_Succ->ln_Pred = n; pred->ln_Succ = n;
}
static int am_streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
struct Node *FindName(struct List *l, const char *name) {
  struct Node *n;
  for (n = l->lh_Head; n->ln_Succ; n = n->ln_Succ) if (n->ln_Name && am_streq(n->ln_Name, name)) return n;
  return 0;
}

/* ------------------------------ memória ------------------------------ */
void *AllocMem(unsigned size, unsigned flags) {
  char *p = (char *)os_malloc(size); unsigned i;
  if (p && (flags & MEMF_CLEAR)) for (i = 0; i < size; i++) p[i] = 0;
  if (!p) am_ioerr = ERROR_NO_FREE_STORE;
  return p;
}
void FreeMem(void *p, unsigned size) { (void)size; os_free(p); }
void *AllocVec(unsigned size, unsigned flags) { return AllocMem(size, flags); }
void FreeVec(void *p) { os_free(p); }
unsigned AvailMem(unsigned flags) { (void)flags; return os_heap_free(); }

/* ------------------------------ tarefas e sinais ------------------------------ */
struct AmTask *FindTask(const char *name) {
  int i;
  if (!name) return am_self();
  am_init();
  for (i = 0; i < AM_TASKS; i++) if (am_tasks[i].used && am_tasks[i].name && am_streq(am_tasks[i].name, name)) return &am_tasks[i];
  return 0;
}
struct AmTask *CreateTask(const char *name, int pri, void (*entry)(void), unsigned stack) {
  int id; struct AmTask *t;
  am_init();
  id = task_create(entry, stack, pri); if (id < 0) return 0;
  t = &am_tasks[id & 15]; t->name = name; t->used = 1; t->sig_alloc = 0; t->sig_recvd = 0; sem_init(&t->sem, 0);
  return t;
}
void DeleteTask(struct AmTask *t) { if (t) { task_kill(t->id); t->used = 0; } }

int AllocSignal(int bit) {
  struct AmTask *t = am_self(); int b = -1, i;
  sem_wait(&am_lock);
  if (bit >= 0) { if (bit < 32 && !(t->sig_alloc & (1u << bit))) b = bit; }
  else for (i = 4; i < 32; i++) if (!(t->sig_alloc & (1u << i))) { b = i; break; }   /* bits 0..3 são do sistema */
  if (b >= 0) { t->sig_alloc |= 1u << b; t->sig_recvd &= ~(1u << b); }
  sem_post(&am_lock);
  return b;
}
void FreeSignal(int bit) { struct AmTask *t = am_self(); if (bit < 0 || bit > 31) return; sem_wait(&am_lock); t->sig_alloc &= ~(1u << bit); sem_post(&am_lock); }
unsigned Wait(unsigned mask) {
  struct AmTask *t = am_self(); unsigned got;
  for (;;) {
    sem_wait(&am_lock); got = t->sig_recvd & mask;
    if (got) { t->sig_recvd &= ~got; sem_post(&am_lock); return got; }
    sem_post(&am_lock);
    sem_wait(&t->sem);
  }
}
void Signal(struct AmTask *t, unsigned mask) { sem_wait(&am_lock); t->sig_recvd |= mask; sem_post(&am_lock); sem_post(&t->sem); }
unsigned SetSignal(unsigned nb, unsigned mask) {
  struct AmTask *t = am_self(); unsigned old;
  sem_wait(&am_lock); old = t->sig_recvd; t->sig_recvd = (old & ~mask) | (nb & mask); sem_post(&am_lock);
  return old;
}
void Forbid(void) {
  int me = task_self();
  am_init();
  if (forbid_owner == me) { forbid_depth++; return; }
  sem_wait(&am_forbid); forbid_owner = me; forbid_depth = 1;
}
void Permit(void) {
  if (forbid_owner != task_self()) return;
  if (--forbid_depth == 0) { forbid_owner = -1; sem_post(&am_forbid); }
}

/* ------------------------------ portas de mensagem ------------------------------ */
struct MsgPort *CreateMsgPort(void) {
  struct MsgPort *p = (struct MsgPort *)AllocMem(sizeof(struct MsgPort), MEMF_CLEAR); int b;
  if (!p) return 0;
  b = AllocSignal(-1); if (b < 0) { FreeMem(p, sizeof(struct MsgPort)); return 0; }
  p->mp_SigTask = am_self(); p->mp_SigBit = (unsigned char)b; NewList(&p->mp_MsgList);
  return p;
}
void DeleteMsgPort(struct MsgPort *p) { if (!p) return; FreeSignal(p->mp_SigBit); FreeMem(p, sizeof(struct MsgPort)); }
void PutMsg(struct MsgPort *p, struct Message *m) {
  sem_wait(&am_lock); AddTail(&p->mp_MsgList, &m->mn_Node); sem_post(&am_lock);
  Signal(p->mp_SigTask, 1u << p->mp_SigBit);
}
struct Message *GetMsg(struct MsgPort *p) { struct Node *n; sem_wait(&am_lock); n = RemHead(&p->mp_MsgList); sem_post(&am_lock); return (struct Message *)n; }
struct Message *WaitPort(struct MsgPort *p) {
  for (;;) {
    sem_wait(&am_lock);
    if (!IsListEmpty(&p->mp_MsgList)) { struct Message *m = (struct Message *)p->mp_MsgList.lh_Head; sem_post(&am_lock); return m; }
    sem_post(&am_lock);
    Wait(1u << p->mp_SigBit);
  }
}
void ReplyMsg(struct Message *m) { if (m->mn_ReplyPort) PutMsg(m->mn_ReplyPort, m); }

/* ------------------------------ dos.library ------------------------------ */
#define DOS_STDOUT 0x7FFF0001u
#define DOS_STDIN  0x7FFF0002u
static void dos_mount(void) { if (!am_files_inited) { am_files_inited = 1; fs_mount(); } }
BPTR Open(const char *name, int mode) {
  int h;
  if (mode == MODE_NEWFILE || mode == MODE_READWRITE) { am_ioerr = ERROR_WRITE_PROTECTED; return 0; }
  dos_mount(); h = fs_open(name);
  if (h < 0) { am_ioerr = ERROR_OBJECT_NOT_FOUND; return 0; }
  return (BPTR)(h + 1);
}
int Close(BPTR fh) { if (fh == 0 || fh >= 0x7FFF0000u) return 0; fs_close((int)fh - 1); return 1; }
int Read(BPTR fh, void *buf, int len) { if (fh == 0 || fh >= 0x7FFF0000u) return -1; return fs_read((int)fh - 1, buf, (unsigned)len); }
int Write(BPTR fh, const void *buf, int len) {
  const char *s = (const char *)buf; int i;
  if (fh != DOS_STDOUT) { am_ioerr = ERROR_WRITE_PROTECTED; return -1; }
  for (i = 0; i < len; i++) os_putc(s[i]);
  return len;
}
int Seek(BPTR fh, int pos, int mode) {
  int h = (int)fh - 1; unsigned old, base;
  if (fh == 0 || fh >= 0x7FFF0000u) return -1;
  old = fs_tell(h);
  base = mode == OFFSET_BEGINNING ? 0 : mode == OFFSET_CURRENT ? old : fs_size(h);
  if (pos < 0 && (unsigned)-pos > base) { am_ioerr = ERROR_SEEK_ERROR; return -1; }
  if (fs_seek(h, base + (unsigned)pos)) { am_ioerr = ERROR_SEEK_ERROR; return -1; }
  return (int)old;
}
BPTR Output(void) { return DOS_STDOUT; }
BPTR Input(void) { return DOS_STDIN; }
int PutStr(const char *s) { os_puts(s); return 0; }
int IoErr(void) { return am_ioerr; }
void Delay(int ticks) { if (ticks > 0) task_sleep((unsigned)ticks * 20); }
int FileSize(const char *name) {
  int h, sz;
  dos_mount(); h = fs_open(name);
  if (h < 0) { am_ioerr = ERROR_OBJECT_NOT_FOUND; return -1; }
  sz = (int)fs_size(h); fs_close(h); return sz;
}
