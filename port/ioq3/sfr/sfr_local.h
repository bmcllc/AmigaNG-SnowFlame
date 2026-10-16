/* sfr_local.h - renderer do ioquake3 para o FlameCore do SF-100 ("SnowFlame renderer").
 * Fluxo: o jogo chama a API refexport_t; o renderer projeta/recorta na CPU (FPU do ColdFire), acumula triângulos já em espaço de tela num buffer de vértices
 * (formato de 48 bytes do FlameCore) e, no EndFrame, monta UMA lista de comandos do GPU para o quadro inteiro (renderização por tiles + HSR). */
#ifndef SFR_LOCAL_H
#define SFR_LOCAL_H
#ifndef NDEBUG
#define HUNK_DEBUG                    /* como em qcommon.h: refimport_t tem Hunk_AllocDebug (5 argumentos) nesta configuração */
#endif
#include "q_shared.h"
#include "qfiles.h"
#include "tr_public.h"
#include "tr_types.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

extern refimport_t ri;
#define SFR_HUNK(n) ri.Hunk_AllocDebug((n), h_low, "sfr", __FILE__, __LINE__)
extern glconfig_t sfr_glconfig;

/* ---- ABI do GPU (espelha src/abi.h) ---- */
#define GPU_REG(o) (*(volatile unsigned *)(0xF0030000u + (o)))
#define VID_REG(o) (*(volatile unsigned *)(0xF0070000u + (o)))
#define BRG_REG(o) (*(volatile unsigned *)(0xF0000000u + (o)))
enum { GC_END = 0, GC_SET_RT = 1, GC_CLEAR = 2, GC_VERTEX_BASE = 3, GC_SET_TEX = 4, GC_STATE = 5, GC_BLEND = 6, GC_FOG = 7, GC_STENCIL = 8, GC_TEXENV = 9, GC_SCISSOR = 10, GC_DRAW_ARRAYS = 0x11 };
enum { ST_DEPTH_TEST = 1, ST_DEPTH_WRITE = 2, ST_BLEND = 4, ST_CULL_CW = 8, ST_CULL_CCW = 16, ST_FOG = 32, ST_TEX0 = 64, ST_TEX1 = 128, ST_STENCIL = 256, ST_BILINEAR = 512, ST_UV1 = 1024, ST_ATEST = 2048 };
enum { BFZERO = 0, BFONE = 1, BFSRCA = 2, BFINVSRCA = 3, BFDSTA = 4, BFINVDSTA = 5 };
enum { TFX_RGB565 = 1, TFX_ARGB4444 = 2 };
enum { TF_CLAMP = 1, TF_MIPS = 2 };
enum { TEV_MODULATE = 0, TEV_REPLACE = 1 };

#define SFR_W 640
#define SFR_H 480
#define SFR_MAXTEX 256
#define SFR_MAX_IMAGES 1024
#define SFR_MAX_SHADERS 2048
#define SFR_MAX_STAGES 6
#define SFR_MAX_VERTS 98304                  /* vértices de tela por quadro (4,5 MB) */
#define SFR_MAX_PACKETS 6144

/* ---- imagens (texturas na VRAM) ---- */
typedef struct sfimg_s {
  char name[MAX_QPATH]; int w, h, lw, lh, fmt, nlevels, flags; unsigned addr; int alpha, id;
} sfimg_t;
extern sfimg_t *sfr_whiteimage, *sfr_defaultimage;
void SFR_ImageInit(void); void SFR_ImageShutdown(void);
sfimg_t *SFR_FindImage(const char *name, int mip, int clamp);
sfimg_t *SFR_CreateImage(const char *name, const byte *rgba, int w, int h, int mip, int clamp);
sfimg_t *SFR_CreateLightmap(int index, const byte *rgb);     /* 128x128x3 do BSP, com "overbright" x2 */

/* ---- shaders ---- */
typedef struct {
  sfimg_t *img; int is_lightmap, is_white; int blend, src, dst; int atest; int rgbgen; byte rgb[3]; int depthwrite, clamp, tcgen_env;
  float scroll[2], scale[2]; float rotate; int has_tcmod; int alphagen; sfimg_t *anim[8]; int anim_n; float anim_fps;
} sfstage_t;
enum { AG_IDENTITY, AG_VERTEX, AG_ENTITY, AG_CONST, AG_PORTAL };
enum { RGB_IDENTITY, RGB_VERTEX, RGB_ENTITY, RGB_CONST, RGB_LIGHTING, RGB_ONEMINUSVERTEX };
typedef struct shader_s {
  char name[MAX_QPATH]; int index, lightmapIndex, nstages; sfstage_t st[SFR_MAX_STAGES];
  int cull; /* 0 front, 1 back, 2 none */ int sort; qboolean nolm, nodraw, sky, lightmapped, nomip, translucent, fog, polyoffset, twosided, defaulted;
} shader_t;
void SFR_ShaderInit(void); void SFR_ShaderShutdown(void);
shader_t *SFR_FindShader(const char *name, int lightmapIndex, int mip);
shader_t *SFR_ShaderByHandle(qhandle_t h);
extern qhandle_t SFR_DefaultShader;

/* ---- backend: pacotes de desenho ---- */
typedef struct { float x, y, z; float u, v, u1, v1; float r, g, b, a; } sfv_t;     /* vértice em espaço de câmera + atributos */
typedef struct { int first, count, flags, src, dst; unsigned a0, a1; byte nt0, nt1; int lw0, lh0, fmt0, lw1, lh1, fmt1, fl0, fl1, nl0, nl1, sortkey; } sfpacket_t;
void SFR_BackendInit(void); void SFR_BackendShutdown(void);
void SFR_BeginFrame(void); void SFR_EndFrame(void);
typedef struct { float org[3], axis[3][3]; float fovx, fovy; int x, y, w, h; float znear, zfar; } sfview_t;
extern sfview_t sfr_view;
extern int sfr_2d;                                   /* 1 = desenho 2D (sem profundidade) */
void SFR_SetView(const refdef_t *fd);
/* emite um triângulo em espaço de câmera (recorta, projeta, acumula) */
void SFR_EmitTri(const sfv_t *a, const sfv_t *b, const sfv_t *c, shader_t *sh, int stage_pass, const byte rgba_override[4], sfimg_t *lm);
void SFR_DrawShaderTris(shader_t *sh, int nverts, const float (*pos)[3], const float (*st)[2], const float (*lm)[2], const byte (*col)[4], int nidx, const int *idx, sfimg_t *lm_img, const byte ent_rgba[4]);
void SFR_Draw2D(float x, float y, float w, float h, float s1, float t1, float s2, float t2, shader_t *sh, const float *rgba);
void SFR_Packets(int *np, int *nv);
void SFR_FlushSort(int first_packet);
int SFR_PacketCount(void);
extern float sfr_color2d[4];

/* ---- mundo ---- */
void SFR_LoadWorld(const char *name); void SFR_FreeWorld(void);
void SFR_RenderWorld(void);
extern qboolean sfr_world_loaded;
qboolean SFR_GetEntityToken(char *buf, int size);
qboolean SFR_InPVS(const vec3_t a, const vec3_t b);
int SFR_BModelCount(void);
void SFR_RenderBModel(int idx, const float org[3], const float axis[3][3]);
void SFR_ModelBounds(qhandle_t h, vec3_t mins, vec3_t maxs);
int SFR_LightForPoint(vec3_t p, vec3_t amb, vec3_t dir, vec3_t ldir);

/* ---- modelos MD3 ---- */
qhandle_t SFR_RegisterModel(const char *name);
void SFR_ModelsShutdown(void);
void SFR_DrawEntityModel(const refEntity_t *e);
int SFR_LerpTag(orientation_t *tag, qhandle_t model, int sf, int ef, float frac, const char *tagName);
qhandle_t SFR_RegisterSkin(const char *name);

/* ---- cena ---- */
void SFR_ClearScene(void); void SFR_AddEntity(const refEntity_t *e); void SFR_AddPoly(qhandle_t h, int n, const polyVert_t *v, int num);
void SFR_AddLight(const vec3_t org, float intensity, float r, float g, float b);
void SFR_RenderScene(const refdef_t *fd);
extern int sfr_frame_count;

/* utilidades */
float SFR_Time(void);                                 /* segundos */
#define SFR_RT_ADDR0 0x10000000u
#define SFR_RT_ADDR1 0x10140000u
#define SFR_TEX_BASE 0x10300000u
#define SFR_TEX_END  0x11F00000u
#endif
