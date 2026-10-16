/* sfr_init.c - GetRefAPI: a API refexport_t do renderer SF-100. */
#include "sfr_local.h"

refimport_t ri; glconfig_t sfr_glconfig; glconfig_t glConfig;
cvar_t *r_saveFontData;
static refexport_t re; static int inited;
void RE_RegisterFont(const char *fontName, int pointSize, fontInfo_t *font);
void R_IssuePendingRenderCommands(void) { }
qhandle_t RE_RegisterShaderNoMip(const char *name);
qhandle_t RE_RegisterShader(const char *name);
qhandle_t RE_RegisterShaderLightMap(const char *name, int lightmapIndex) { return SFR_FindShader(name, lightmapIndex, 1)->index; }
qhandle_t RE_RegisterShader(const char *name) { return SFR_FindShader(name, -4, 1)->index; }
qhandle_t RE_RegisterShaderNoMip(const char *name) { return SFR_FindShader(name, -4, 0)->index; }

static void sf_init(void) {
  if (inited) return;
  SFR_ImageInit(); SFR_ShaderInit(); SFR_BackendInit(); inited = 1;
}
static void Shutdown(qboolean d) { (void)d; SFR_FreeWorld(); SFR_ModelsShutdown(); SFR_ShaderShutdown(); SFR_ImageShutdown(); SFR_BackendShutdown(); inited = 0; }
static void BeginRegistration(glconfig_t *c) {
  sf_init();
  memset(&sfr_glconfig, 0, sizeof sfr_glconfig);
  Q_strncpyz(sfr_glconfig.renderer_string, "SnowFlame FlameCore", sizeof sfr_glconfig.renderer_string); Q_strncpyz(sfr_glconfig.vendor_string, "Amiga", sizeof sfr_glconfig.vendor_string); Q_strncpyz(sfr_glconfig.version_string, "SF-100", sizeof sfr_glconfig.version_string);
  sfr_glconfig.vidWidth = SFR_W; sfr_glconfig.vidHeight = SFR_H; sfr_glconfig.windowAspect = (float)SFR_W / SFR_H; sfr_glconfig.colorBits = 32; sfr_glconfig.depthBits = 24; sfr_glconfig.stencilBits = 8;
  sfr_glconfig.isFullscreen = qtrue; sfr_glconfig.maxTextureSize = SFR_MAXTEX; sfr_glconfig.numTextureUnits = 2; sfr_glconfig.textureCompression = TC_NONE;
  glConfig = sfr_glconfig; *c = sfr_glconfig;
  ri.IN_Init(NULL);
}
static void LoadWorld(const char *n) { SFR_LoadWorld(n); }
static void SetWorldVisData(const byte *v) { (void)v; }
static void EndRegistration(void) { }
static qhandle_t RegisterModel(const char *n) { return SFR_RegisterModel(n); }
static qhandle_t RegisterSkin(const char *n) { return SFR_RegisterSkin(n); }
static int LightForPoint(vec3_t p, vec3_t a, vec3_t d, vec3_t l) { return SFR_LightForPoint(p, a, d, l); }
static void AddEntity(const refEntity_t *e) { SFR_AddEntity(e); }
static void AddPoly(qhandle_t h, int n, const polyVert_t *v, int num) { SFR_AddPoly(h, n, v, num); }
static void AddLight(const vec3_t o, float i, float r, float g, float b) { SFR_AddLight(o, i, r, g, b); }
static void RenderScene(const refdef_t *fd) { SFR_RenderScene(fd); }
static void SetColor(const float *c) { if (c) { sfr_color2d[0] = c[0]; sfr_color2d[1] = c[1]; sfr_color2d[2] = c[2]; sfr_color2d[3] = c[3]; } else sfr_color2d[0] = sfr_color2d[1] = sfr_color2d[2] = sfr_color2d[3] = 1; }
static void DrawStretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t sh) { SFR_Draw2D(x, y, w, h, s1, t1, s2, t2, SFR_ShaderByHandle(sh), NULL); }
static void DrawStretchRaw(int x, int y, int w, int h, int c, int r, const byte *d, int cl, qboolean dirty) { (void)x; (void)y; (void)w; (void)h; (void)c; (void)r; (void)d; (void)cl; (void)dirty; }
static void UploadCinematic(int w, int h, int c, int r, const byte *d, int cl, qboolean dirty) { (void)w; (void)h; (void)c; (void)r; (void)d; (void)cl; (void)dirty; }
static void BeginFrame(stereoFrame_t s) { (void)s; SFR_BeginFrame(); }
static void EndFrame(int *f, int *b) { SFR_EndFrame(); if (f) *f = 0; if (b) *b = 0; }
static int MarkFragments(int n, const vec3_t *p, const vec3_t pr, int mp, vec3_t pb, int mf, markFragment_t *fb) { (void)n; (void)p; (void)pr; (void)mp; (void)pb; (void)mf; (void)fb; return 0; }
static int LerpTag(orientation_t *t, qhandle_t m, int sf, int ef, float fr, const char *n) { return SFR_LerpTag(t, m, sf, ef, fr, n); }
static void ModelBounds(qhandle_t m, vec3_t mins, vec3_t maxs) { SFR_ModelBounds(m, mins, maxs); }
static void RemapShader(const char *a, const char *b, const char *c) { (void)a; (void)b; (void)c; }
static qboolean GetEntityToken(char *b, int s) { return SFR_GetEntityToken(b, s); }
static qboolean inPVS(const vec3_t a, const vec3_t b) { return SFR_InPVS(a, b); }
static void TakeVideoFrame(int h, int w, byte *c, byte *e, qboolean m) { (void)h; (void)w; (void)c; (void)e; (void)m; }

refexport_t *GetRefAPI(int apiVersion, refimport_t *rimp) {
  if (apiVersion != REF_API_VERSION) return NULL;
  ri = *rimp; memset(&re, 0, sizeof re);
  re.Shutdown = Shutdown; re.BeginRegistration = BeginRegistration; re.RegisterModel = RegisterModel; re.RegisterSkin = RegisterSkin; re.RegisterShader = RE_RegisterShader; re.RegisterShaderNoMip = RE_RegisterShaderNoMip;
  re.LoadWorld = LoadWorld; re.SetWorldVisData = SetWorldVisData; re.EndRegistration = EndRegistration; re.ClearScene = SFR_ClearScene; re.AddRefEntityToScene = AddEntity; re.AddPolyToScene = AddPoly;
  re.LightForPoint = LightForPoint; re.AddLightToScene = AddLight; re.AddAdditiveLightToScene = AddLight; re.RenderScene = RenderScene; re.SetColor = SetColor; re.DrawStretchPic = DrawStretchPic;
  re.DrawStretchRaw = DrawStretchRaw; re.UploadCinematic = UploadCinematic; re.BeginFrame = BeginFrame; re.EndFrame = EndFrame; re.MarkFragments = MarkFragments; re.LerpTag = LerpTag; re.ModelBounds = ModelBounds;
  re.RegisterFont = RE_RegisterFont; re.RemapShader = RemapShader; re.GetEntityToken = GetEntityToken; re.inPVS = inPVS; re.TakeVideoFrame = TakeVideoFrame;
  return &re;
}
