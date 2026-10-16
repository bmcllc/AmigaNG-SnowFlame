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

/* ------------------------------ sistema ------------------------------ */
void Sys_Init(void) { Cvar_Set("arch", "m68k-coldfire"); Cvar_Set("username", "player"); }
void *QDECL Sys_LoadGameDll(const char *name, vmMainProc *entryPoint, intptr_t(QDECL *systemcalls)(intptr_t, ...)) { (void)name; (void)entryPoint; (void)systemcalls; return NULL; }   /* sem dlopen: o jogo roda como QVM */
void Sys_UnloadDll(void *h) { (void)h; }
qboolean Sys_DllExtension(const char *name) { (void)name; return qfalse; }
char *Sys_GetCurrentUser(void) { return "player"; }
void QDECL Sys_Error(const char *error, ...) {
  va_list ap; char msg[1024];
  va_start(ap, error); Q_vsnprintf(msg, sizeof msg, error, ap); va_end(ap);
  printf("\n*** Sys_Error: %s\n", msg);
  exit(1);
}
void Sys_Quit(void) { printf("[ioq3] Sys_Quit\n"); exit(0); }
char *Sys_GetClipboardData(void) { return NULL; }
void Sys_Print(const char *msg) { fputs(msg, stdout); }
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
  const char *dflt = "+set dedicated 1 +set com_hunkMegs 16 +set com_zoneMegs 12 +set com_soundMegs 2 +set com_maxfps 60 +set fs_basepath .";
  if (SRAM[0x200]) { int i; for (i = 0; i < 1023 && SRAM[0x200 + i]; i++) cmd[i] = SRAM[0x200 + i]; cmd[i] = 0; } else strcpy(cmd, dflt);
  Com_Init(cmd);
  NET_Init();
  for (;;) { IN_Frame(); Com_Frame(); }
  return 0;
}
