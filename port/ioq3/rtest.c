/* rtest.c - harness do renderer SF-100 SEM o motor: implementa refimport_t sobre stdio/SnowFS, carrega um mapa BSP, um MD3 e desenha 3D + 2D por alguns quadros. */
#include "sfr/sfr_local.h"
#include "sf_fs.h"
#include <unistd.h>

void QDECL Com_Error(int level, const char *fmt, ...) { va_list ap; char b[512]; va_start(ap, fmt); Q_vsnprintf(b, sizeof b, fmt, ap); va_end(ap); printf("Com_Error(%d): %s\n", level, b); exit(2); }
void QDECL Com_Printf(const char *fmt, ...) { va_list ap; char b[512]; va_start(ap, fmt); Q_vsnprintf(b, sizeof b, fmt, ap); va_end(ap); fputs(b, stdout); }

static void QDECL im_printf(int lvl, const char *fmt, ...) { va_list ap; char b[512]; (void)lvl; va_start(ap, fmt); Q_vsnprintf(b, sizeof b, fmt, ap); va_end(ap); fputs(b, stdout); }
static void QDECL im_error(int lvl, const char *fmt, ...) { va_list ap; char b[512]; (void)lvl; va_start(ap, fmt); Q_vsnprintf(b, sizeof b, fmt, ap); va_end(ap); printf("ri.Error: %s\n", b); exit(3); }
static int im_ms(void) { return (int)(sf_time_ns() / 1000000ull); }
static void *im_hunk(int n, ha_pref p, char *label, char *file, int line) { (void)p; (void)label; (void)file; (void)line; return calloc(1, (size_t)n); }
static void *im_tmp(int n) { return malloc((size_t)n); }
static void im_free(void *p) { free(p); }
static void *im_malloc(int n) { return malloc((size_t)n); }
static cvar_t cvars[128]; static int ncv;
static cvar_t *im_cvar_get(const char *name, const char *val, int flags) {
  int i; cvar_t *c;
  for (i = 0; i < ncv; i++) if (!Q_stricmp(cvars[i].name, name)) return &cvars[i];
  c = &cvars[ncv++]; memset(c, 0, sizeof *c); c->name = strdup(name); c->string = strdup(val); c->value = (float)atof(val); c->integer = atoi(val); (void)flags; return c;
}
static void im_cvar_set(const char *n, const char *v) { cvar_t *c = im_cvar_get(n, v, 0); c->string = strdup(v); c->value = (float)atof(v); c->integer = atoi(v); }
static void im_cvar_setv(const char *n, float v) { char b[32]; snprintf(b, sizeof b, "%f", v); im_cvar_set(n, b); }
static void im_cvar_range(cvar_t *c, float a, float b, qboolean i) { (void)c; (void)a; (void)b; (void)i; }
static void im_cvar_desc(cvar_t *c, const char *d) { (void)c; (void)d; }
static int im_cvar_int(const char *n) { return im_cvar_get(n, "0", 0)->integer; }
static void im_cmd_add(const char *n, void (*f)(void)) { (void)n; (void)f; }
static void im_cmd_rm(const char *n) { (void)n; }
static int im_argc(void) { return 0; }
static char *im_argv(int i) { (void)i; return ""; }
static void im_exec(int w, const char *t) { (void)w; (void)t; }
static byte *im_pvs(int c) { (void)c; return NULL; }
static void im_dbg(void (*d)(int, int, float *)) { (void)d; }
static int im_in_pak(const char *n, int *c) { (void)n; (void)c; return -1; }
static long im_readfile(const char *name, void **buf) {
  FILE *f = fopen(name, "rb"); long n; void *b;
  if (!f) { if (buf) *buf = NULL; return -1; }
  fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
  if (!buf) { fclose(f); return n; }
  b = malloc((size_t)n + 1); if (!b) { fclose(f); return -1; }
  fread(b, 1, (size_t)n, f); ((char *)b)[n] = 0; fclose(f); *buf = b; return n;
}
static void im_freefile(void *b) { free(b); }
static char **im_listfiles(const char *dir, const char *ext, int *num) {
  int i, n = 0, c = sf_fs_count(), dl = (int)strlen(dir), el = (int)strlen(ext); char **list = calloc((size_t)c + 1, sizeof *list);
  for (i = 0; i < c; i++) { const char *nm = sf_fs_name(i); if (strncmp(nm, dir, dl) || nm[dl] != '/' || strchr(nm + dl + 1, '/')) continue; if (el && (int)strlen(nm) < el + dl + 1) continue; if (el && Q_stricmp(nm + strlen(nm) - el, ext)) continue; list[n++] = strdup(nm + dl + 1); }
  *num = n; return list;
}
static void im_freelist(char **l) { (void)l; }
static void im_writefile(const char *q, const void *b, int s) { (void)q; (void)b; (void)s; }
static qboolean im_exists(const char *f) { return sf_fs_find(f) >= 0; }
static void im_cin_up(int h) { (void)h; }
static int im_cin_play(const char *a, int x, int y, int w, int h, int b) { (void)a; (void)x; (void)y; (void)w; (void)h; (void)b; return -1; }
static e_status im_cin_run(int h) { (void)h; return FMV_EOF; }
static void im_avi(const byte *b, int s) { (void)b; (void)s; }
static void im_in_init(void *w) { (void)w; }
static void im_in_shut(void) { }
static void im_in_rst(void) { }
static long im_ftol(float f) { return (long)f; }
static void im_setenv(const char *n, const char *v) { (void)n; (void)v; }
static void im_glsafe(void) { }
static qboolean im_lowmem(void) { return qfalse; }

refexport_t *GetRefAPI(int apiVersion, refimport_t *rimp);
void sf_report(const char *what) { printf("[rtest] %s\n", what); }

int main(void) {
  refimport_t imp; refexport_t *re; glconfig_t cfg; refdef_t fd; refEntity_t ent; int frame, nframes = 4; qhandle_t bar, cube; float color[4] = {1, 1, 1, 1};
  memset(&imp, 0, sizeof imp);
  imp.Printf = im_printf; imp.Error = im_error; imp.Milliseconds = im_ms; imp.Hunk_AllocDebug = im_hunk; imp.Hunk_AllocateTempMemory = im_tmp; imp.Hunk_FreeTempMemory = im_free; imp.Malloc = im_malloc; imp.Free = im_free;
  imp.Cvar_Get = im_cvar_get; imp.Cvar_Set = im_cvar_set; imp.Cvar_SetValue = im_cvar_setv; imp.Cvar_CheckRange = im_cvar_range; imp.Cvar_SetDescription = im_cvar_desc; imp.Cvar_VariableIntegerValue = im_cvar_int;
  imp.Cmd_AddCommand = im_cmd_add; imp.Cmd_RemoveCommand = im_cmd_rm; imp.Cmd_Argc = im_argc; imp.Cmd_Argv = im_argv; imp.Cmd_ExecuteText = im_exec; imp.CM_ClusterPVS = im_pvs; imp.CM_DrawDebugSurface = im_dbg;
  imp.FS_FileIsInPAK = im_in_pak; imp.FS_ReadFile = im_readfile; imp.FS_FreeFile = im_freefile; imp.FS_ListFiles = im_listfiles; imp.FS_FreeFileList = im_freelist; imp.FS_WriteFile = im_writefile; imp.FS_FileExists = im_exists;
  imp.CIN_UploadCinematic = im_cin_up; imp.CIN_PlayCinematic = im_cin_play; imp.CIN_RunCinematic = im_cin_run; imp.CL_WriteAVIVideoFrame = im_avi;
  imp.IN_Init = im_in_init; imp.IN_Shutdown = im_in_shut; imp.IN_Restart = im_in_rst; imp.ftol = im_ftol; imp.Sys_SetEnv = im_setenv; imp.Sys_GLimpSafeInit = im_glsafe; imp.Sys_GLimpInit = im_glsafe; imp.Sys_LowPhysicalMemory = im_lowmem;
  re = GetRefAPI(REF_API_VERSION, &imp); if (!re) { printf("GetRefAPI falhou\n"); return 1; }
  re->BeginRegistration(&cfg); printf("[rtest] %s %dx%d\n", cfg.renderer_string, cfg.vidWidth, cfg.vidHeight);
  re->LoadWorld("maps/test.bsp"); bar = re->RegisterShaderNoMip("gfx/2d/bar"); cube = re->RegisterModel("models/test/cube.md3"); re->EndRegistration();
  { char tok[256]; int n = 0; while (re->GetEntityToken(tok, sizeof tok)) n++; printf("[rtest] tokens de entidade: %d; cubo=%d\n", n, (int)cube); }
  for (frame = 0; frame < nframes; frame++) {
    float a = 0.8f + frame * 0.35f, ca = cosf(a), sa = sinf(a);
    memset(&fd, 0, sizeof fd); fd.x = 0; fd.y = 0; fd.width = 640; fd.height = 480; fd.fov_x = 90; fd.fov_y = 2 * atanf(tanf(45 * (float)M_PI / 180) * 480.0f / 640.0f) * 180 / (float)M_PI; fd.time = frame * 100;
    { static const float cam[4][4] = { {0, -230, 70, 90}, {-60, -60, 80, 219}, {60, 0, 80, 66}, {20, -200, 60, 70} };
      float yaw = cam[frame][3] * (float)M_PI / 180, f[3] = { cosf(yaw), sinf(yaw), 0 }, l[3] = { -sinf(yaw), cosf(yaw), 0 }, u[3] = { 0, 0, 1 }; int k;
      fd.vieworg[0] = cam[frame][0]; fd.vieworg[1] = cam[frame][1]; fd.vieworg[2] = cam[frame][2]; (void)a; (void)ca; (void)sa;
      for (k = 0; k < 3; k++) { fd.viewaxis[0][k] = f[k]; fd.viewaxis[1][k] = l[k]; fd.viewaxis[2][k] = u[k]; } }
    re->BeginFrame(STEREO_CENTER);
    re->ClearScene();
    memset(&ent, 0, sizeof ent); ent.reType = RT_MODEL; ent.hModel = cube; ent.axis[0][0] = ent.axis[1][1] = ent.axis[2][2] = 1; ent.origin[0] = 120; ent.origin[1] = -60; ent.origin[2] = 40; ent.frame = frame & 1; ent.oldframe = 0; ent.backlerp = 0.0f;
    ent.shaderRGBA[0] = ent.shaderRGBA[1] = ent.shaderRGBA[2] = ent.shaderRGBA[3] = 255; re->AddRefEntityToScene(&ent);
    re->RenderScene(&fd);
    re->SetColor(color); re->DrawStretchPic(0, 440, 640, 40, 0, 0, 1, 1, bar);
    color[0] = 1; color[1] = 0.8f; color[2] = 0.2f; color[3] = 0.9f; re->SetColor(color); re->DrawStretchPic(20, 20, 120, 12, 0, 0, 1, 1, 0);
    re->SetColor(NULL);
    re->EndFrame(NULL, NULL);
    printf("[rtest] quadro %d desenhado\n", frame); usleep(250000);
  }
  usleep(400000);                                                  /* deixa o vídeo exibir o último quadro antes de sair */
  re->Shutdown(qtrue);
  return 0;
}
