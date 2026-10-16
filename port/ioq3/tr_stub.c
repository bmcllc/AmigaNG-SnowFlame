/* tr_stub.c - renderer "nulo" do SF-100: cumpre a API refexport_t sem desenhar nada (o renderer sobre o FlameCore vem depois). */
#include "q_shared.h"
#include "qcommon.h"
#include "tr_public.h"

static refimport_t ri;
static refexport_t re;
static void Shutdown(qboolean d) { (void)d; }
static void BeginRegistration(glconfig_t *c) {
  memset(c, 0, sizeof *c);
  Q_strncpyz(c->renderer_string, "SnowFlame null renderer", sizeof c->renderer_string); Q_strncpyz(c->vendor_string, "Amiga", sizeof c->vendor_string); Q_strncpyz(c->version_string, "0", sizeof c->version_string);
  c->vidWidth = 640; c->vidHeight = 480; c->windowAspect = 640.0f / 480.0f; c->colorBits = 32; c->depthBits = 24; c->stencilBits = 8; c->isFullscreen = qtrue; c->maxTextureSize = 1024; c->numTextureUnits = 2;
  ri.IN_Init(NULL);
}
static qhandle_t Reg(const char *n) { (void)n; return 1; }
static void LoadWorld(const char *n) { (void)n; } static void SetWorldVisData(const byte *v) { (void)v; } static void EndRegistration(void) { } static void ClearScene(void) { }
static void AddRefEntityToScene(const refEntity_t *r) { (void)r; } static void AddPolyToScene(qhandle_t h, int n, const polyVert_t *v, int num) { (void)h; (void)n; (void)v; (void)num; }
static int LightForPoint(vec3_t p, vec3_t a, vec3_t d, vec3_t l) { (void)p; VectorClear(a); VectorClear(d); VectorClear(l); return 0; }
static void AddLight(const vec3_t o, float i, float r, float g, float b) { (void)o; (void)i; (void)r; (void)g; (void)b; }
static void RenderScene(const refdef_t *fd) { (void)fd; } static void SetColor(const float *c) { (void)c; }
static void DrawStretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t sh) { (void)x; (void)y; (void)w; (void)h; (void)s1; (void)t1; (void)s2; (void)t2; (void)sh; }
static void DrawStretchRaw(int x, int y, int w, int h, int c, int r, const byte *d, int cl, qboolean dirty) { (void)x; (void)y; (void)w; (void)h; (void)c; (void)r; (void)d; (void)cl; (void)dirty; }
static void UploadCinematic(int w, int h, int c, int r, const byte *d, int cl, qboolean dirty) { (void)w; (void)h; (void)c; (void)r; (void)d; (void)cl; (void)dirty; }
static void BeginFrame(stereoFrame_t s) { (void)s; } static void EndFrame(int *f, int *b) { if (f) *f = 0; if (b) *b = 0; }
static int MarkFragments(int n, const vec3_t *p, const vec3_t pr, int mp, vec3_t pb, int mf, markFragment_t *fb) { (void)n; (void)p; (void)pr; (void)mp; (void)pb; (void)mf; (void)fb; return 0; }
static int LerpTag(orientation_t *t, qhandle_t m, int sf, int ef, float fr, const char *n) { (void)m; (void)sf; (void)ef; (void)fr; (void)n; memset(t, 0, sizeof *t); return 0; }
static void ModelBounds(qhandle_t m, vec3_t mins, vec3_t maxs) { (void)m; VectorClear(mins); VectorClear(maxs); }
static void RegisterFont(const char *n, int pt, fontInfo_t *f) { (void)n; (void)pt; memset(f, 0, sizeof *f); }
static void RemapShader(const char *a, const char *b, const char *c) { (void)a; (void)b; (void)c; }
static qboolean GetEntityToken(char *b, int s) { (void)s; b[0] = 0; return qfalse; }
static qboolean inPVS(const vec3_t a, const vec3_t b) { (void)a; (void)b; return qtrue; }
static void TakeVideoFrame(int h, int w, byte *c, byte *e, qboolean m) { (void)h; (void)w; (void)c; (void)e; (void)m; }

refexport_t *GetRefAPI(int apiVersion, refimport_t *rimp) {
  if (apiVersion != REF_API_VERSION) return NULL;
  ri = *rimp; memset(&re, 0, sizeof re);
  re.Shutdown = Shutdown; re.BeginRegistration = BeginRegistration; re.RegisterModel = Reg; re.RegisterSkin = Reg; re.RegisterShader = Reg; re.RegisterShaderNoMip = Reg;
  re.LoadWorld = LoadWorld; re.SetWorldVisData = SetWorldVisData; re.EndRegistration = EndRegistration; re.ClearScene = ClearScene; re.AddRefEntityToScene = AddRefEntityToScene;
  re.AddPolyToScene = AddPolyToScene; re.LightForPoint = LightForPoint; re.AddLightToScene = AddLight; re.AddAdditiveLightToScene = AddLight; re.RenderScene = RenderScene;
  re.SetColor = SetColor; re.DrawStretchPic = DrawStretchPic; re.DrawStretchRaw = DrawStretchRaw; re.UploadCinematic = UploadCinematic; re.BeginFrame = BeginFrame; re.EndFrame = EndFrame;
  re.MarkFragments = MarkFragments; re.LerpTag = LerpTag; re.ModelBounds = ModelBounds; re.RegisterFont = RegisterFont; re.RemapShader = RemapShader; re.GetEntityToken = GetEntityToken;
  re.inPVS = inPVS; re.TakeVideoFrame = TakeVideoFrame;
  return &re;
}
