/* sfr_scene.c - cena: entidades (modelos, sprites, vigas), polígonos e RenderScene. */
#include "sfr_local.h"

#define MAX_ENTS 1024
#define MAX_POLYS 1024
static refEntity_t ents[MAX_ENTS]; static int nents;
typedef struct { qhandle_t sh; int nv; polyVert_t *v; } scpoly_t;
static scpoly_t polys[MAX_POLYS]; static int npolys; static polyVert_t polyverts[8192]; static int npolyverts;

void SFR_ClearScene(void) { nents = 0; npolys = 0; npolyverts = 0; }
void SFR_AddEntity(const refEntity_t *e) { if (nents < MAX_ENTS && e->reType != RT_PORTALSURFACE) ents[nents++] = *e; }
void SFR_AddPoly(qhandle_t h, int n, const polyVert_t *v, int num) {
  int i; (void)num;
  if (npolys >= MAX_POLYS || npolyverts + n > 8192 || n < 3) return;
  polys[npolys].sh = h; polys[npolys].nv = n; polys[npolys].v = &polyverts[npolyverts]; for (i = 0; i < n; i++) polyverts[npolyverts++] = v[i]; npolys++;
}
void SFR_AddLight(const vec3_t org, float intensity, float r, float g, float b) { (void)org; (void)intensity; (void)r; (void)g; (void)b; }

static void draw_quad_world(shader_t *sh, const float p[4][3], const byte rgba[4]) {
  static const int idx[6] = { 0, 1, 2, 0, 2, 3 }; static const float st[4][2] = { {0, 0}, {1, 0}, {1, 1}, {0, 1} }; byte col[4][4]; int i;
  for (i = 0; i < 4; i++) { col[i][0] = rgba[0]; col[i][1] = rgba[1]; col[i][2] = rgba[2]; col[i][3] = rgba[3]; }
  SFR_DrawShaderTris(sh, 4, p, st, NULL, (const byte (*)[4])col, 6, idx, NULL, rgba);
}

static void draw_sprite(const refEntity_t *e) {
  float r = e->radius, up[3], left[3], p[4][3]; int i; byte rgba[4]; shader_t *sh = SFR_ShaderByHandle(e->customShader);
  float a = e->rotation * (float)M_PI / 180.0f, s = sinf(a), c = cosf(a);
  for (i = 0; i < 3; i++) { left[i] = (sfr_view.axis[1][i] * c + sfr_view.axis[2][i] * s) * r; up[i] = (sfr_view.axis[2][i] * c - sfr_view.axis[1][i] * s) * r; }
  for (i = 0; i < 3; i++) { p[0][i] = e->origin[i] + left[i] + up[i]; p[1][i] = e->origin[i] - left[i] + up[i]; p[2][i] = e->origin[i] - left[i] - up[i]; p[3][i] = e->origin[i] + left[i] - up[i]; }
  memcpy(rgba, e->shaderRGBA, 4); if (!rgba[3]) rgba[3] = 255;
  draw_quad_world(sh, p, rgba);
}
static void draw_beam(const refEntity_t *e) {
  float dir[3], toview[3], side[3], p[4][3], w = (e->reType == RT_BEAM || e->reType == RT_LIGHTNING) ? (e->frame > 0 ? (float)e->frame : 4.0f) : (e->reType == RT_RAIL_CORE ? 16.0f : 8.0f);
  int i; byte rgba[4]; shader_t *sh = SFR_ShaderByHandle(e->customShader); float len;
  VectorSubtract(e->oldorigin, e->origin, dir); VectorSubtract(sfr_view.org, e->origin, toview);
  CrossProduct(dir, toview, side); len = VectorNormalize(side); if (len < 0.001f) return;
  for (i = 0; i < 3; i++) { p[0][i] = e->origin[i] + side[i] * w * 0.5f; p[1][i] = e->oldorigin[i] + side[i] * w * 0.5f; p[2][i] = e->oldorigin[i] - side[i] * w * 0.5f; p[3][i] = e->origin[i] - side[i] * w * 0.5f; }
  memcpy(rgba, e->shaderRGBA, 4); if (!rgba[3]) rgba[3] = 255;
  draw_quad_world(sh, p, rgba);
}

void SFR_RenderScene(const refdef_t *fd) {
  int i, first; extern void SFR_SetClear(unsigned argb);
  SFR_SetView(fd); SFR_SetupFrustum(); sfr_2d = 0; first = SFR_PacketCount();
  if (!(fd->rdflags & RDF_NOWORLDMODEL)) SFR_RenderWorld(); else SFR_SetClear(0xFF000000u);
  for (i = 0; i < nents; i++) {
    const refEntity_t *e = &ents[i];
    if (e->renderfx & RF_THIRD_PERSON) continue;                       /* o corpo do jogador em primeira pessoa */
    switch (e->reType) {
      case RT_MODEL: SFR_DrawEntityModel(e); break;
      case RT_SPRITE: draw_sprite(e); break;
      case RT_BEAM: case RT_LIGHTNING: case RT_RAIL_CORE: draw_beam(e); break;
      default: break;
    }
  }
  for (i = 0; i < npolys; i++) {
    scpoly_t *pl = &polys[i]; float (*pos)[3] = ri.Hunk_AllocateTempMemory(pl->nv * 12); float (*st)[2] = ri.Hunk_AllocateTempMemory(pl->nv * 8); byte (*col)[4] = ri.Hunk_AllocateTempMemory(pl->nv * 4);
    int *idx = ri.Hunk_AllocateTempMemory((pl->nv - 2) * 3 * sizeof(int)), k, c;
    for (k = 0; k < pl->nv; k++) { VectorCopy(pl->v[k].xyz, pos[k]); st[k][0] = pl->v[k].st[0]; st[k][1] = pl->v[k].st[1]; for (c = 0; c < 4; c++) col[k][c] = pl->v[k].modulate[c]; }
    for (k = 0; k + 2 < pl->nv; k++) { idx[k * 3] = 0; idx[k * 3 + 1] = k + 1; idx[k * 3 + 2] = k + 2; }
    SFR_DrawShaderTris(SFR_ShaderByHandle(pl->sh), pl->nv, (const float (*)[3])pos, (const float (*)[2])st, NULL, (const byte (*)[4])col, (pl->nv - 2) * 3, idx, NULL, NULL);
    ri.Hunk_FreeTempMemory(idx); ri.Hunk_FreeTempMemory(col); ri.Hunk_FreeTempMemory(st); ri.Hunk_FreeTempMemory(pos);
  }
  SFR_FlushSort(first);
  sfr_2d = 1;
  /* a vista do 2D é redefinida em SFR_Draw2D; restaura o estado de 2D */
  nents = 0; npolys = 0; npolyverts = 0;
}
