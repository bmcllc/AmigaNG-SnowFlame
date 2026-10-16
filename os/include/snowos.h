/* snowos.h - API do SnowOS Native (kernel preemptivo do SF-100) e das bibliotecas SnowGX / SnowVX / SnowAudio */
#ifndef SNOWOS_H
#define SNOWOS_H
#include "snowflame.h"

typedef char *va_list;
#define va_start(ap, last) ((ap) = (char *)&(last) + 4)
#define va_arg(ap, type) (*(type *)(((ap) += 4) - 4))
#define va_end(ap) ((void)0)

/* ------------------------------ núcleo ------------------------------ */
typedef struct { int count; unsigned pad; } Sem;
typedef struct { Sem items, space, lock; unsigned *buf; int cap, head, tail; } Queue;

#define TASK_FREE  0
#define TASK_READY 1
#define TASK_RUN   2
#define TASK_SLEEP 3
#define TASK_BLOCK 4
#define TASK_DEAD  5

/* tarefas (modo usuário; o kernel roda em modo supervisor; prioridade maior = mais urgente; 0 é a ociosa) */
int  task_create(void (*entry)(void), unsigned stack_size, int prio);   /* id (>= 2) ou -1 */
void task_exit(void);
int  task_self(void);
void task_yield(void);
void task_sleep(unsigned ms);
int  task_state(int id);
int  task_kill(int id);
unsigned os_ticks(void);                         /* ms desde o boot */
void os_exit(int code);                          /* para o sistema (HALT); o código fica em os_exit_code */
extern volatile int os_exit_code;

/* sincronização */
void sem_init(Sem *s, int initial);
void sem_wait(Sem *s);
int  sem_trywait(Sem *s);
int  sem_wait_timeout(Sem *s, unsigned ms);      /* 1 = obteve, 0 = estourou o tempo */
void sem_post(Sem *s);
void queue_init(Queue *q, unsigned *storage, int capacity);
void queue_send(Queue *q, unsigned v);
unsigned queue_recv(Queue *q);
int  queue_tryrecv(Queue *q, unsigned *v);

/* memória */
void *os_malloc(unsigned n);
void os_free(void *p);
unsigned os_heap_free(void);

/* interrupções de dispositivos: 'sem' recebe um sem_post a cada IRQ do bit (índice 0..15); 0 desanexa */
void os_irq_attach(int bit, Sem *sem);

/* console de depuração */
void os_putc(int c);
void os_puts(const char *s);
int  os_printf(const char *fmt, ...);            /* %d %u %x %X %c %s %% com largura e '0' */

/* cache (o D-cache da CPU não é coerente com DMA/GPU/Blitter/VPU) */
void os_cache_flush(unsigned addr, unsigned len);
void os_cache_invalidate(unsigned addr, unsigned len);
void os_icache_invalidate(unsigned addr, unsigned len);
int  dma_copy(unsigned dst, unsigned src, unsigned len, unsigned flags);   /* bloqueante, canal 0 */

void os_reboot(const char *path);                 /* IPL: reinicia e carrega 'path' do DVD (não retorna), ex.: "boot/menu.bin" */

/* a aplicação define app_main (roda como tarefa de prioridade 5) */
void app_main(void);

/* ------------------------------ SnowGX ------------------------------ */
typedef struct { unsigned *buf; int n, cap; } GxList;
void gx_init(void);
void gx_begin(GxList *l, unsigned *buf, int cap);
void gx_set_rt(GxList *l, unsigned addr, unsigned pitch, int w, int h);
void gx_clear(GxList *l, unsigned argb, unsigned depth_bits, unsigned flags);
void gx_vertex_base(GxList *l, unsigned addr);
void gx_state(GxList *l, unsigned flags);
void gx_blend(GxList *l, unsigned src, unsigned dst);
void gx_set_tex(GxList *l, int unit, unsigned addr, int lw, int lh, int fmt, unsigned flags, int nlevels);
void gx_set_palette(GxList *l, int unit, unsigned addr, int count);
void gx_texenv(GxList *l, int unit, int mode);
void gx_scissor(GxList *l, int x, int y, int w, int h);
void gx_draw_arrays(GxList *l, int first, int count, int prim);
void gx_draw_indexed(GxList *l, unsigned idx_addr, int count, int prim);
int  gx_end(GxList *l);                          /* termina a lista; devolve o nº de palavras */
int  gx_submit(GxList *l);                       /* envia à GPU e espera a conclusão (0 = ok, -1 = erro da GPU) */
unsigned gx_f32(int v);                          /* inteiro -> bits de float IEEE 754 */
unsigned gx_f32_fx(int fx16);                    /* ponto fixo 16.16 -> bits de float */
void gx_put_vertex(unsigned *v, int x_fx, int y_fx, int z_fx, unsigned argb, int u_fx, int v_fx);
void gx_blit(unsigned op, unsigned src, unsigned spitch, unsigned dst, unsigned dpitch, int w, int h, unsigned color);
void gx_blit_sprites(unsigned list, int count, unsigned dst, unsigned dpitch, int w, int h);
void gx_video_mode(int mode);
void gx_layer(int i, unsigned addr, unsigned pitch, unsigned flags);
void gx_layer_scroll(int i, int x, int y);
void gx_video_start(void);                       /* habilita o IRQ de vblank */
void gx_wait_vblank(void);

/* ------------------------------ SnowVX ------------------------------ */
void vx_load_program(int vpu, const unsigned *code, int nwords);
void vx_load_data(int vpu, unsigned dmem_off, const unsigned *src, int nwords);
void vx_read_data(int vpu, unsigned dmem_off, unsigned *dst, int nwords);
int  vx_run(int vpu, unsigned pc, unsigned s0, unsigned s1, unsigned s2, unsigned s3);   /* bloqueante; 0 = ok, -1 = falha */
unsigned vx_cycles(int vpu);

/* ------------------------------ SnowAudio ---------------------------- */
#define SND_PCM8  0
#define SND_PCM16 1
#define SND_ADPCM 2
void snd_init(void);
void snd_load(unsigned aram_off, const void *src, unsigned len);
void snd_voice_play(int v, unsigned aram_off, unsigned nsamples, int fmt, unsigned step_q16, int vol_l, int vol_r, int loop);
void snd_voice_stop(int v);
void snd_master(int vol);
void snd_legacy_play(int ch, unsigned addr_aram, unsigned len_words, unsigned period, int vol, int loop);
void snd_seq_play(unsigned ddr_addr, int nevents, int loop);
int  snd_sine(short *dst, int n, int period, int amp);   /* preenche PCM16 com uma senoide de 'period' amostras */

/* ------------------------------ SnowFS (arquivos no DVD) ------------------------- */
#define FS_NAME_MAX 48
int  fs_mount(void);                             /* lê o superbloco e o diretório; 0 = ok, -1 = sem disco/formato inválido */
int  fs_count(void);                             /* nº de arquivos */
const char *fs_name(int i);                      /* nome do arquivo i (ou NULL) */
int  fs_open(const char *path);                  /* handle (>= 0) ou -1 */
int  fs_read(int h, void *dst, unsigned n);      /* bytes lidos (0 no fim do arquivo); -1 = erro */
int  fs_seek(int h, unsigned pos);               /* 0 = ok */
unsigned fs_tell(int h);
unsigned fs_size(int h);
void fs_close(int h);
int  fs_load(const char *path, void *dst, unsigned max);   /* abre, lê tudo (até max) e fecha; devolve o tamanho ou -1 */

/* ------------------------------ SnowIn (teclado e mouse) ------------------------- */
#define KEY_UP      0x100                        /* bit do evento: tecla solta */
#define KEYM_SHIFT  1
#define KEYM_CTRL   2
#define KEYM_ALT    4
#define KEYM_META   8
int  in_key_poll(void);                          /* evento (usage | KEY_UP | mods << 16) ou -1 se não houver */
int  in_key_wait(void);                          /* bloqueia até haver um evento */
int  in_key_ascii(int ev);                       /* ASCII do evento de tecla pressionada (layout US), 0 se não imprimível/soltura */
void in_mouse(int *dx, int *dy, int *wheel);     /* deslocamento acumulado desde a última leitura */
unsigned in_mouse_buttons(void);                 /* bit0 esq, bit1 dir, bit2 meio */

/* ------------------------------ AmigaOS: exec.library / dos.library (camada mínima sobre o SnowOS) ------------------------------ */
/* Tipos e nomes do AmigaOS 3.x para portar fontes C. Não há tabelas de bibliotecas (OpenLibrary): as funções são chamadas direto. */
#define MEMF_ANY    0
#define MEMF_PUBLIC 1
#define MEMF_CHIP   2
#define MEMF_FAST   4
#define MEMF_CLEAR  0x10000
struct Node { struct Node *ln_Succ, *ln_Pred; unsigned char ln_Type; signed char ln_Pri; char *ln_Name; };
struct List { struct Node *lh_Head, *lh_Tail, *lh_TailPred; unsigned char lh_Type, l_pad; };
struct AmTask;                                   /* "struct Task" do AmigaOS (renomeada: o kernel já usa esse nome) */
struct MsgPort { struct Node mp_Node; struct AmTask *mp_SigTask; unsigned char mp_SigBit, mp_Flags; struct List mp_MsgList; };
struct Message { struct Node mn_Node; struct MsgPort *mn_ReplyPort; unsigned short mn_Length; };
void  NewList(struct List *l);
void  AddHead(struct List *l, struct Node *n);
void  AddTail(struct List *l, struct Node *n);
void  Remove(struct Node *n);
struct Node *RemHead(struct List *l);
struct Node *RemTail(struct List *l);
void  Insert(struct List *l, struct Node *n, struct Node *pred);
struct Node *FindName(struct List *l, const char *name);
#define IsListEmpty(l) ((l)->lh_TailPred == (struct Node *)(l))
void *AllocMem(unsigned size, unsigned flags);
void  FreeMem(void *p, unsigned size);
void *AllocVec(unsigned size, unsigned flags);
void  FreeVec(void *p);
unsigned AvailMem(unsigned flags);
struct AmTask *FindTask(const char *name);       /* NULL = a tarefa corrente */
struct AmTask *CreateTask(const char *name, int pri, void (*entry)(void), unsigned stack);
void  DeleteTask(struct AmTask *t);
int   AllocSignal(int bit);                      /* -1 = qualquer; devolve o bit ou -1 */
void  FreeSignal(int bit);
unsigned Wait(unsigned mask);
void  Signal(struct AmTask *t, unsigned mask);
unsigned SetSignal(unsigned new_bits, unsigned mask);
void  Forbid(void);                              /* exclusão mútua cooperativa entre quem chama Forbid/Permit */
void  Permit(void);
struct MsgPort *CreateMsgPort(void);
void  DeleteMsgPort(struct MsgPort *p);
void  PutMsg(struct MsgPort *p, struct Message *m);
struct Message *GetMsg(struct MsgPort *p);
struct Message *WaitPort(struct MsgPort *p);
void  ReplyMsg(struct Message *m);
#define MODE_READWRITE 1004
#define MODE_OLDFILE   1005
#define MODE_NEWFILE   1006
#define OFFSET_BEGINNING (-1)
#define OFFSET_CURRENT   0
#define OFFSET_END       1
#define ERROR_NO_FREE_STORE 103
#define ERROR_OBJECT_NOT_FOUND 205
#define ERROR_SEEK_ERROR 219
#define ERROR_WRITE_PROTECTED 223
#define ERROR_INVALID_LOCK 211
typedef unsigned BPTR;                            /* 0 = falha */
BPTR  Open(const char *name, int mode);          /* arquivos do SnowFS (somente leitura) */
int   Close(BPTR fh);
int   Read(BPTR fh, void *buf, int len);         /* bytes lidos, 0 no fim, -1 erro */
int   Write(BPTR fh, const void *buf, int len);  /* só Output() é gravável (console) */
int   Seek(BPTR fh, int pos, int mode);          /* posição anterior ou -1 */
BPTR  Output(void);
BPTR  Input(void);
int   PutStr(const char *s);
int   IoErr(void);
void  Delay(int ticks);                          /* ticks de 1/50 s */
int   FileSize(const char *name);                /* -1 se não existe */
#define Printf os_printf

#endif
