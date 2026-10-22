/* sys_snowflame.c - camada de plataforma do ioquake3 no SF-100: Sys_*, rede nula, entrada (HID/controle), som (SnowPaula) e main().
 * O motor roda "bare metal" em ColdFire: arquivos vêm do SnowFS do DVD (stdio do runtime), não há rede nem bibliotecas dinâmicas. */
#include "q_shared.h"
#include "qcommon.h"
#include "keys.h"
#include "snd_local.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "sf_fs.h"

#define BR(o) (*(volatile unsigned *)(0xF0000000u + (o)))
#define SRAM ((volatile char *)0x30000000u)

/* ------------------------------ tela de carregamento ------------------------------ */
/* O motor passa mais de um minuto simulado carregando as VMs e o mapa sem desenhar nada: uma faixa na camada de vídeo 1 (acima da
   camada 0 do renderer) mostra o título, a última mensagem do console e uma barra de progresso aproximada. Usa a fonte do sistema
   do DVD (sys/font.raw, atlas A8 16x16 de ASCII, e sys/font.adv); sem ela, não há faixa. Termina em "entered the game" (ou ao fim da
   inicialização, se a linha de comando não pede mapa). */
#define VIDR(o) (*(volatile unsigned *)(0xF0070000u + (o)))
#define SPL_W 640
#define SPL_H 96
static unsigned spl_buf[SPL_W * SPL_H] __attribute__((aligned(16)));
static unsigned char *spl_font, spl_adv[128]; static int spl_on, spl_wait_map, spl_pct;
static void spl_flush(void) { BR(0x24) = (unsigned)spl_buf; BR(0x28) = sizeof spl_buf; BR(0x20) = 4; }
static void spl_text(int x, int y, const char *t, int scale, unsigned rgb, int maxw) {
  for (; *t; t++) {
    int c = (unsigned char)*t & 127, cx = (c % 16) * 16, cy = (c / 16) * 16, w = (c == 32 ? 4 : spl_adv[c]) + 1, px, py;
    if (x + w * scale > maxw) break;
    for (py = 0; py < 16 * scale; py++) for (px = 0; px < 16 * scale; px++) {
      unsigned a = spl_font[(cy + py / scale) * 256 + cx + px / scale], *d; int X = x + px - (16 - w) * scale / 2, Y = y + py;
      if (!a || X < 0 || X >= SPL_W || Y < 0 || Y >= SPL_H) continue;
      d = &spl_buf[Y * SPL_W + X];
      { unsigned r = (((*d >> 16) & 255) * (255 - a) + ((rgb >> 16) & 255) * a) / 255, g = (((*d >> 8) & 255) * (255 - a) + ((rgb >> 8) & 255) * a) / 255, b = ((*d & 255) * (255 - a) + (rgb & 255) * a) / 255; *d = 0xFF000000u | r << 16 | g << 8 | b; }
    }
    x += w * scale;
  }
}
static void spl_draw(const char *msg, unsigned color) {
  int i, x, y, bar = spl_pct * (SPL_W - 80) / 100;
  for (i = 0; i < SPL_W * SPL_H; i++) spl_buf[i] = 0xE0101822u;                                   /* fundo azul-grafite, quase opaco */
#ifdef SF_NATIVE_GAME
  spl_text(40, 6, "QUAKE III ARENA", 2, 0xFFB000, SPL_W);
#else
  spl_text(40, 6, "OPENARENA", 2, 0xFFB000, SPL_W);
#endif
  spl_text(40, 46, msg, 1, color, SPL_W - 40);
  for (y = 74; y < 82; y++) for (x = 40; x < SPL_W - 40; x++) spl_buf[y * SPL_W + x] = (x - 40 < bar) ? 0xFFFF8A00u : 0xFF2A3440u;
  spl_flush();
}
static void spl_start(void) {
  FILE *f = fopen("sys/font.raw", "rb"), *g;
  if (!f) return;
  spl_font = malloc(32768); g = fopen("sys/font.adv", "rb");
  if (!spl_font || !g || fread(spl_font, 1, 32768, f) != 32768 || fread(spl_adv, 1, 128, g) != 128) { if (g) fclose(g); fclose(f); free(spl_font); spl_font = NULL; return; }
  fclose(f); fclose(g);
  VIDR(0x00) = 2;                                                                                  /* VMODE_480P */
  VIDR(0x40 + 0x40 * 1 + 0x00) = (unsigned)spl_buf; VIDR(0x40 + 0x40 * 1 + 0x04) = SPL_W * 4; VIDR(0x40 + 0x40 * 1 + 0x20) = SPL_W | (SPL_H << 16);
  VIDR(0x40 + 0x40 * 1 + 0x10) = 255; spl_draw("INICIANDO...", 0xE0E8F0); VIDR(0x40 + 0x40 * 1 + 0x1C) = 1;   /* camada 1, habilitada */
  spl_on = 1;
}
static void spl_end(void) { if (spl_on) { VIDR(0x40 + 0x40 * 1 + 0x1C) = 0; spl_on = 0; } }
static void spl_msg(const char *msg) {
  static const struct { const char *key; int pct; } ms[] = { {"FS_Startup", 5}, {"Client Initialization", 10}, {"Initializing Renderer", 14}, {"vm/ui.qvm", 22}, {"arenas parsed", 28},
    {"Common Initialization Complete", 32}, {"Server:", 38}, {"vm/qagame.qvm", 48}, {"Game Initialization", 56}, {"vm/cgame.qvm", 66}, {"SFR: mapa", 82}, {"CL_InitCGame", 92} };
  char line[96]; int i, n = 0;
  if (!spl_on) return;
  for (i = 0; i < (int)(sizeof ms / sizeof ms[0]); i++) if (ms[i].pct > spl_pct && strstr(msg, ms[i].key)) spl_pct = ms[i].pct;
  if (strstr(msg, "entered the game") || (!spl_wait_map && strstr(msg, "Common Initialization Complete"))) { spl_end(); return; }
  while (*msg == '\n' || *msg == ' ' || *msg == '-' || *msg == '^') { if (*msg == '^' && msg[1]) msg += 2; else msg++; }          /* ignora quebras e cores do Q3 */
  for (; *msg && *msg != '\n' && n < 90; msg++) if ((unsigned char)*msg >= 32 && (unsigned char)*msg < 127) line[n++] = *msg;
  line[n] = 0;
  if (n) spl_draw(line, 0xE0E8F0);
}

/* ------------------------------ sistema ------------------------------ */
/* Sai do jogo: se o disco tem o menu (boot/menu.bin), pede ao IPL que volte a ele (após 'wait_ms', para a mensagem de erro ficar na
   tela); sem menu (sf100_run, testes) encerra com o código dado. */
static void sf_leave(int code, int wait_ms) {
  FILE *f = fopen("boot/menu.bin", "rb");
  if (!f) exit(code);
  fclose(f);
  if (wait_ms > 0) BR(0x0F4) = (unsigned)wait_ms * 1000u;
  { static const char path[] = "boot/menu.bin"; int i; for (i = 0; path[i]; i++) SRAM[i] = path[i]; SRAM[i] = 0; }
  BR(0x0F0) = 1;                                              /* BR_SYS_CTRL: reinicia e carrega o programa de SRAM_BASE */
  for (;;) BR(0x0F4) = 1000000u;
}
void Sys_Init(void) { Cvar_Set("arch", "m68k-coldfire"); Cvar_Set("username", "player"); }
#ifdef SF_NATIVE_GAME
/* módulos de jogo nativos (build_ioq3.sh NATIVE=1): qagame, cgame e ui compilados com o GCC e ligados no motor, com os símbolos internos
   locais e marcas em .data/.bss (n_data_start...). "Carregar" de novo restaura .data e zera .bss, como um dlopen novo faria. */
typedef void (*sf_dllentry_t)(intptr_t(QDECL *)(intptr_t, ...));
#define SFMOD(n) extern void n##_dllEntry(intptr_t(QDECL *)(intptr_t, ...)); extern intptr_t n##_vmMain(int, int, int, int, int, int, int, int, int, int, int, int, int); \
  extern char n##_data_start[], n##_data_end[], n##_bss_start[], n##_bss_end[];
SFMOD(qagame) SFMOD(cgame) SFMOD(ui)
#define SFMOD_ENT(n) { #n, n##_dllEntry, n##_vmMain, n##_data_start, n##_data_end, n##_bss_start, n##_bss_end, NULL }
static struct { const char *name; sf_dllentry_t entry; vmMainProc main; char *d0, *d1, *b0, *b1, *saved; } sf_mods[] = { SFMOD_ENT(qagame), SFMOD_ENT(cgame), SFMOD_ENT(ui) };
void *QDECL Sys_LoadGameDll(const char *name, vmMainProc *entryPoint, intptr_t(QDECL *systemcalls)(intptr_t, ...)) {
  unsigned i;
  for (i = 0; i < sizeof(sf_mods) / sizeof(sf_mods[0]); i++) if (!strcmp(name, sf_mods[i].name)) {
    if (!sf_mods[i].saved) { sf_mods[i].saved = malloc(sf_mods[i].d1 - sf_mods[i].d0); if (!sf_mods[i].saved) return NULL; memcpy(sf_mods[i].saved, sf_mods[i].d0, sf_mods[i].d1 - sf_mods[i].d0); }
    else memcpy(sf_mods[i].d0, sf_mods[i].saved, sf_mods[i].d1 - sf_mods[i].d0);
    memset(sf_mods[i].b0, 0, sf_mods[i].b1 - sf_mods[i].b0);
    sf_mods[i].entry(systemcalls); *entryPoint = sf_mods[i].main;
    return &sf_mods[i];
  }
  return NULL;
}
#else
void *QDECL Sys_LoadGameDll(const char *name, vmMainProc *entryPoint, intptr_t(QDECL *systemcalls)(intptr_t, ...)) { (void)name; (void)entryPoint; (void)systemcalls; return NULL; }   /* sem dlopen: o jogo roda como QVM */
#endif
void Sys_UnloadDll(void *h) { (void)h; }
qboolean Sys_DllExtension(const char *name) { (void)name; return qfalse; }
char *Sys_GetCurrentUser(void) { return "player"; }
void QDECL Sys_Error(const char *error, ...) {
  va_list ap; char msg[1024];
  va_start(ap, error); Q_vsnprintf(msg, sizeof msg, error, ap); va_end(ap);
  printf("\n*** Sys_Error: %s\n", msg);
  if (spl_on) { spl_pct = spl_pct ? spl_pct : 1; spl_draw(msg, 0xFF6060); }
  sf_leave(1, 5000);
}
void Sys_Quit(void) { printf("[ioq3] Sys_Quit\n"); sf_leave(0, 0); }
char *Sys_GetClipboardData(void) { return NULL; }
void Sys_Print(const char *msg) { fputs(msg, stdout); spl_msg(msg); }
int Sys_Milliseconds(void) { return (int)(sf_time_ns() / 1000000ull); }
qboolean Sys_RandomBytes(byte *s, int len) { unsigned long long t = sf_time_ns() * 6364136223846793005ull + 1442695040888963407ull; int i; for (i = 0; i < len; i++) { t = t * 6364136223846793005ull + 1; s[i] = (byte)(t >> 33); } return qtrue; }
void Sys_DisplaySystemConsole(qboolean show) { (void)show; }
cpuFeatures_t Sys_GetProcessorFeatures(void) { return (cpuFeatures_t)0; }
void Sys_SetErrorText(const char *text) { (void)text; }
FILE *Sys_FOpen(const char *ospath, const char *mode) { return fopen(ospath, mode); }
qboolean Sys_Mkdir(const char *path) { (void)path; return qtrue; }
FILE *Sys_Mkfifo(const char *ospath) { (void)ospath; return NULL; }
char *Sys_Cwd(void) { return "."; }
static char install_path[MAX_OSPATH] = ".";
void Sys_SetDefaultInstallPath(const char *path) { Q_strncpyz(install_path, path, sizeof install_path); }
char *Sys_DefaultInstallPath(void) { return install_path; }
char *Sys_SteamPath(void) { return ""; }
char *Sys_GogPath(void) { return ""; }
char *Sys_MicrosoftStorePath(void) { return ""; }
char *Sys_DefaultHomeConfigPath(void) { return NULL; }
char *Sys_DefaultHomeDataPath(void) { return NULL; }
char *Sys_DefaultHomeStatePath(void) { return NULL; }
char *Sys_ConsoleInput(void) { return NULL; }
void Sys_Sleep(int msec) { if (msec > 0) BR(0x0F4) = (unsigned)msec * 1000u; }       /* BR_SLEEP_US: o tempo simulado avança sem queimar instruções */
qboolean Sys_LowPhysicalMemory(void) { return qtrue; }
void Sys_SetEnv(const char *name, const char *value) { (void)name; (void)value; }
qboolean Sys_OpenFolderInFileManager(const char *path, qboolean create) { (void)path; (void)create; return qfalse; }
void Sys_RemovePIDFile(const char *gamedir) { (void)gamedir; }
void Sys_InitPIDFile(const char *gamedir) { (void)gamedir; }
void Sys_GLimpSafeInit(void) { }
void Sys_GLimpInit(void) { }
dialogResult_t Sys_Dialog(dialogType_t type, const char *message, const char *title) { printf("[dialog %d] %s: %s\n", (int)type, title, message); return DR_OK; }
const char *Sys_Dirname(char *path) { static char dir[MAX_OSPATH]; char *s; Q_strncpyz(dir, path, sizeof dir); s = strrchr(dir, '/'); if (s) *s = 0; else strcpy(dir, "."); return dir; }
const char *Sys_Basename(char *path) { char *s = strrchr(path, '/'); return s ? s + 1 : path; }

/* ------------------------------ listagem de arquivos (SnowFS + arquivos em RAM) ------------------------------ */
#define MAX_FOUND 0x1000
static int all_names(const char **out, int max) {
  int i, n = 0, c = sf_fs_count();
  for (i = 0; i < c && n < max; i++) out[n++] = sf_fs_name(i);
  for (i = 0; i < sf_ram_count() && n < max; i++) { const char *r = sf_ram_name(i); if (r) out[n++] = r; }
  return n;
}
static char **copy_list(char **list, int n) { char **r = Z_Malloc((n + 1) * sizeof *r); int i; for (i = 0; i < n; i++) r[i] = list[i]; r[n] = NULL; return r; }
char **Sys_ListFiles(const char *directory, const char *extension, char *filter, int *numfiles, qboolean wantsubs) {
  static const char *names[MAX_FOUND]; char *list[MAX_FOUND]; int total = all_names(names, MAX_FOUND), i, nfiles = 0, dl; qboolean dironly = wantsubs; int extLen;
  while (directory[0] == '.' && directory[1] == '/') directory += 2;
  dl = (int)strlen(directory);
  if (filter) {                                                   /* recursivo: todos os arquivos sob 'directory' que casam com o filtro */
    for (i = 0; i < total && nfiles < MAX_FOUND - 1; i++) {
      const char *n = names[i]; char rel[MAX_OSPATH];
      if (dl && (strncmp(n, directory, dl) || n[dl] != '/')) continue;
      Com_sprintf(rel, sizeof rel, "/%s", n + (dl ? dl + 1 : 0));
      if (!Com_FilterPath(filter, rel, qfalse)) continue;
      list[nfiles++] = CopyString(rel);
    }
    *numfiles = nfiles; return nfiles ? copy_list(list, nfiles) : NULL;
  }
  if (!extension) extension = "";
  if (extension[0] == '/' && extension[1] == 0) { extension = ""; dironly = qtrue; }
  extLen = (int)strlen(extension);
  for (i = 0; i < total && nfiles < MAX_FOUND - 1; i++) {
    const char *n = names[i], *rest, *slash; char leaf[MAX_OSPATH]; int k, dup = 0;
    if (dl) { if (strncmp(n, directory, dl) || n[dl] != '/') continue; rest = n + dl + 1; } else rest = n;
    slash = strchr(rest, '/');
    if (slash) { if (!dironly) continue; Q_strncpyz(leaf, rest, (int)(slash - rest) + 1); }
    else { if (dironly) continue; Q_strncpyz(leaf, rest, sizeof leaf); }
    if (extLen && ((int)strlen(leaf) < extLen || Q_stricmp(leaf + strlen(leaf) - extLen, extension))) continue;
    for (k = 0; k < nfiles; k++) if (!strcmp(list[k], leaf)) { dup = 1; break; }       /* subdiretórios aparecem uma vez só */
    if (!dup) list[nfiles++] = CopyString(leaf);
  }
  *numfiles = nfiles; return nfiles ? copy_list(list, nfiles) : NULL;
}
void Sys_FreeFileList(char **list) { int i; if (!list) return; for (i = 0; list[i]; i++) Z_Free(list[i]); Z_Free(list); }

/* ------------------------------ rede nula (só loopback, que fica em net_chan.c) ------------------------------ */
void NET_Init(void) { }
void NET_Shutdown(void) { }
void NET_Restart_f(void) { }
void NET_Config(qboolean enable) { (void)enable; }
void NET_Sleep(int msec) { Sys_Sleep(msec); }
void NET_JoinMulticast6(void) { }
void NET_LeaveMulticast6(void) { }
qboolean NET_CompareBaseAdrMask(netadr_t a, netadr_t b, int mask) { (void)mask; return a.type == b.type; }
qboolean NET_CompareBaseAdr(netadr_t a, netadr_t b) { return a.type == b.type; }
qboolean NET_CompareAdr(netadr_t a, netadr_t b) { return a.type == b.type; }
qboolean NET_IsLocalAddress(netadr_t adr) { return adr.type == NA_LOOPBACK; }
const char *NET_AdrToString(netadr_t a) { return a.type == NA_LOOPBACK ? "loopback" : a.type == NA_BOT ? "bot" : "bad"; }
const char *NET_AdrToStringwPort(netadr_t a) { return NET_AdrToString(a); }
void Sys_SendPacket(int length, const void *data, netadr_t to) { (void)length; (void)data; (void)to; }
qboolean Sys_StringToAdr(const char *s, netadr_t *a, netadrtype_t family) { (void)family; if (!strcmp(s, "localhost") || !strcmp(s, "loopback")) { memset(a, 0, sizeof *a); a->type = NA_LOOPBACK; return qtrue; } return qfalse; }
qboolean Sys_IsLANAddress(netadr_t adr) { (void)adr; return qfalse; }
void Sys_ShowIP(void) { }

/* ------------------------------ entrada: teclado e mouse (HID) e controle ------------------------------ */
#define HID(o) (*(volatile unsigned *)(0xF00F0000u + (o)))
#define PAD(o) (*(volatile unsigned *)(0xF00B0000u + (o)))
static int hid_to_q3(int u) {
  if (u >= 0x04 && u <= 0x1D) return 'a' + (u - 4);
  if (u >= 0x1E && u <= 0x26) return '1' + (u - 0x1E);
  if (u >= 0x3A && u <= 0x45) return K_F1 + (u - 0x3A);
  switch (u) {
    case 0x27: return '0'; case 0x28: return K_ENTER; case 0x29: return K_ESCAPE; case 0x2A: return K_BACKSPACE; case 0x2B: return K_TAB; case 0x2C: return K_SPACE;
    case 0x2D: return '-'; case 0x2E: return '='; case 0x2F: return '['; case 0x30: return ']'; case 0x31: return '\\'; case 0x33: return ';'; case 0x34: return '\'';
    case 0x35: return K_CONSOLE; case 0x36: return ','; case 0x37: return '.'; case 0x38: return '/'; case 0x39: return K_CAPSLOCK;
    case 0x49: return K_INS; case 0x4A: return K_HOME; case 0x4B: return K_PGUP; case 0x4C: return K_DEL; case 0x4D: return K_END; case 0x4E: return K_PGDN;
    case 0x4F: return K_RIGHTARROW; case 0x50: return K_LEFTARROW; case 0x51: return K_DOWNARROW; case 0x52: return K_UPARROW;
    case 0xE0: case 0xE4: return K_CTRL; case 0xE1: case 0xE5: return K_SHIFT; case 0xE2: case 0xE6: return K_ALT;
    default: return 0;
  }
}
static int hid_char(int u, int shift) {                         /* ASCII (layout US) para os eventos SE_CHAR */
  static const char lo[] = "abcdefghijklmnopqrstuvwxyz1234567890\r\033\b\t -=[]\\#;'`,./", hi[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\033\b\t _+{}|~:\"~<>?";
  if (u < 0x04 || u > 0x38) return 0;
  { char c = shift ? hi[u - 4] : lo[u - 4]; return (c == '\r' || c == '\033' || c == '\b' || c == '\t' || c == '`' || c == '~') ? 0 : c; }
}
static unsigned pad_prev, mouse_prev;
void IN_Init(void *windowData) { (void)windowData; HID(0x00) = 0; Cvar_Get("in_nograb", "1", CVAR_ARCHIVE); }
void IN_Shutdown(void) { }
void IN_Restart(void) { }
void IN_Frame(void) {
  int t = Sys_Milliseconds(), ev, dx, dy; unsigned b, ch, i;
  while ((ev = (int)HID(0x08)) != -1) {                         /* HID_KEY_DATA: usage | solto<<8 | mods<<16 */
    int u = ev & 0xFF, down = !(ev & 0x100), shift = (ev >> 16) & 1, k = hid_to_q3(u);
    if (k) Com_QueueEvent(t, SE_KEY, k, down, 0, NULL);
    if (down && (k == K_ENTER || hid_char(u, shift) || k == K_BACKSPACE)) { int c = k == K_ENTER ? '\r' : k == K_BACKSPACE ? '\b' : hid_char(u, shift); Com_QueueEvent(t, SE_CHAR, c, 0, 0, NULL); }
  }
  dx = (int)HID(0x10); dy = (int)HID(0x14); if (dx || dy) Com_QueueEvent(t, SE_MOUSE, dx, dy, 0, NULL);
  b = HID(0x18); ch = b ^ mouse_prev; mouse_prev = b;
  for (i = 0; i < 3; i++) if (ch & (1u << i)) Com_QueueEvent(t, SE_KEY, K_MOUSE1 + (int)i, (b >> i) & 1, 0, NULL);
  b = PAD(0x10); ch = b ^ pad_prev; pad_prev = b;               /* botões do controle -> K_JOY1.. (direcional também vira setas) */
  for (i = 0; i < 12; i++) if (ch & (1u << i)) {
    static const int dirkey[4] = { K_UPARROW, K_DOWNARROW, K_LEFTARROW, K_RIGHTARROW };
    Com_QueueEvent(t, SE_KEY, i < 4 ? dirkey[i] : K_JOY1 + (int)(i - 4), (b >> i) & 1, 0, NULL);
  }
}

/* ------------------------------ som: buffer DMA do Quake -> duas vozes mono da SnowPaula (esq/dir) ------------------------------ */
#define PAULA(o) (*(volatile unsigned *)(0xF0060000u + (o)))
#define SND_FRAMES 16384
#define ARAM_L 0x080000
#define ARAM_R 0x0C0000
static unsigned long long snd_t0; static int snd_up;
static short *snd_mono;
static void voice_start(int v, unsigned off, unsigned vl, unsigned vr, unsigned step) {
  unsigned b = 0x100 + (unsigned)v * 0x40;
  PAULA(b + 0x1C) = 0; PAULA(b + 0x00) = off; PAULA(b + 0x04) = SND_FRAMES; PAULA(b + 0x08) = 0; PAULA(b + 0x0C) = SND_FRAMES;
  PAULA(b + 0x10) = step; PAULA(b + 0x14) = vl; PAULA(b + 0x18) = vr; PAULA(b + 0x1C) = 1 | 2 | (1 << 2);       /* ligada | laço | PCM16 */
}
static void dma_copy_aram(unsigned aram_off, const void *src, unsigned len) {            /* canal 0 do SnowBridge, polling */
  volatile unsigned *d = (volatile unsigned *)0xF0000100u;
  BR(0x24) = (unsigned)src; BR(0x28) = len; BR(0x20) = 4;                                  /* flush do D-cache do trecho */
  while (d[5] & 1) { }
  d[0] = (unsigned)src; d[1] = 0x20000000u + aram_off; d[2] = len; d[3] = 0; d[6] = 1;
  while (d[5] & 1) { }
}
qboolean SNDDMA_Init(void) {
  dma.samplebits = 16; dma.speed = 22050; dma.channels = 2; dma.samples = SND_FRAMES * 2; dma.submission_chunk = 1; dma.isfloat = 0;
  dma.buffer = calloc(SND_FRAMES * 2, 2); snd_mono = calloc(SND_FRAMES, 2);
  if (!dma.buffer || !snd_mono) return qfalse;
  voice_start(0, ARAM_L, 256, 0, (unsigned)((unsigned long long)dma.speed * 65536ull / 48000ull));
  voice_start(1, ARAM_R, 0, 256, (unsigned)((unsigned long long)dma.speed * 65536ull / 48000ull));
  snd_t0 = sf_time_ns(); snd_up = 1; return qtrue;
}
int SNDDMA_GetDMAPos(void) { unsigned long long fr; if (!snd_up) return 0; fr = (sf_time_ns() - snd_t0) * (unsigned long long)dma.speed / 1000000000ull; return (int)((fr % SND_FRAMES) * 2); }
void SNDDMA_Shutdown(void) { if (snd_up) { PAULA(0x100 + 0x1C) = 0; PAULA(0x140 + 0x1C) = 0; snd_up = 0; } }
void SNDDMA_BeginPainting(void) { }
void SNDDMA_Submit(void) {
  const short *s = (const short *)dma.buffer; int i;
  if (!snd_up) return;
  for (i = 0; i < SND_FRAMES; i++) snd_mono[i] = s[i * 2]; dma_copy_aram(ARAM_L, snd_mono, SND_FRAMES * 2);
  for (i = 0; i < SND_FRAMES; i++) snd_mono[i] = s[i * 2 + 1]; dma_copy_aram(ARAM_R, snd_mono, SND_FRAMES * 2);
}
qboolean S_AL_Init(soundInterface_t *si) { (void)si; return qfalse; }

/* ------------------------------ main ------------------------------ */
int main(void) {
  /* a linha de comando do motor vem do host em SRAM_BASE+0x200 (NUL-terminada); sem ela, usa o padrão */
  static char cmd[1024];
  /* padrão: o jogo (OpenArena, mapa oa_dm1 do pk3 montado por tools/mkoa.py) em tela cheia sobre o FlameCore, sem bots (não há .aas) */
#ifdef SF_NATIVE_GAME
  /* versão nativa: Quake 3 (pk3 originais em baseq3/ no disco), qagame/cgame/ui ligados no motor */
  const char *dflt = "+set com_basegame baseq3 +set vm_game 0 +set vm_cgame 0 +set vm_ui 0 +set com_hunkMegs 20+set com_zoneMegs 8 +set com_soundMegs 2 +set com_maxfps 60 +set fs_basepath . +set sv_pure 0 +set bot_enable 0 +set s_initsound 0 +devmap q3dm1";
#else
  const char *dflt = "+set com_hunkMegs 24 +set com_zoneMegs 8 +set com_soundMegs 2 +set com_maxfps 60 +set fs_basepath . +set sv_pure 0 +set bot_enable 0 +set s_initsound 0 +set model sarge +set headmodel sarge +devmap oa_dm1";
#endif
  if (SRAM[0x200]) { int i; for (i = 0; i < 1023 && SRAM[0x200 + i]; i++) cmd[i] = SRAM[0x200 + i]; cmd[i] = 0; } else strcpy(cmd, dflt);
  spl_wait_map = strstr(cmd, "map ") != NULL;
  spl_start();
  Com_Init(cmd);
  NET_Init();
  for (;;) { IN_Frame(); Com_Frame(); }
  return 0;
}
