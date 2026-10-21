/* sfr_world.c - mapa BSP do Quake 3 (versão 46): superfícies planares, malhas e patches de Bézier tesselados, lightmaps, PVS por cluster e culling por frustum. */
#include "sfr_local.h"

typedef struct { int type; shader_t *sh; sfimg_t *lm; int nv, ni; float (*pos)[3]; float (*st)[2]; float (*lms)[2]; byte (*col)[4]; int *idx; float mins[3], maxs[3]; int stamp; } wsurf_t;
static dheader_t *bsp; static byte *bspdata;
static wsurf_t *surfs; static int nsurfs;
static dplane_t *planes; static dnode_t *nodes; static dleaf_t *leafs; static int *leafsurf; static dmodel_t *models; static int nmodels, nnodes, nleafs;
static byte *vis; static int vis_clusters, vis_rowbytes;
static char *entstr, *entp; static sfimg_t **lmimgs; static int nlm;
qboolean sfr_world_loaded; static int framestamp; static int sf_leafs_drawn;
static float sky_color[3];

static int swap_int(int v) { return LittleLong(v); }
static float swap_float(float v) { return LittleFloat(v); }
#define LUMP(i, type, out, cnt) do { int ofs = swap_int(bsp->lumps[i].fileofs), len = swap_int(bsp->lumps[i].filelen); (out) = (type *)(bspdata + ofs); (cnt) = len / (int)sizeof(type); } while (0)

static void *hunk(int n) { void *p = SFR_HUNK(n); return p; }
extern int sfr_vram_used;
static int sfr_vram_used_kb(void) { return sfr_vram_used / 1024; }
static void *hunk_temp(int n) { return ri.Hunk_AllocateTempMemory(n); }
static void hunk_temp_free(void *p) { ri.Hunk_FreeTempMemory(p); }

/* ---- patches: biquadrático de Bézier 3x3 por sub-patch, tesselado em SUBDIV segmentos ---- */
#define SUBDIV 5
static void bez3(float t, float *w) { float s = 1 - t; w[0] = s * s; w[1] = 2 * s * t; w[2] = t * t; }
static void tessellate(wsurf_t *ws, const drawVert_t *cp, int pw, int ph) {
  int nsx = (pw - 1) / 2, nsy = (ph - 1) / 2, nx = nsx * SUBDIV + 1, ny = nsy * SUBDIV + 1, gx, gy, k, c; float wx[3], wy[3];
  ws->nv = nx * ny; ws->ni = (nx - 1) * (ny - 1) * 6;
  ws->pos = hunk(ws->nv * sizeof *ws->pos); ws->st = hunk(ws->nv * sizeof *ws->st); ws->lms = hunk(ws->nv * sizeof *ws->lms); ws->col = hunk(ws->nv * sizeof *ws->col); ws->idx = hunk(ws->ni * sizeof(int));
  for (gy = 0; gy < ny; gy++) for (gx = 0; gx < nx; gx++) {
    int a = gx / SUBDIV, b = gy / SUBDIV, v = gy * nx + gx; float tx, ty, col[4] = {0, 0, 0, 0}; int i, j;
    if (a >= nsx) a = nsx - 1; if (b >= nsy) b = nsy - 1;
    tx = (float)(gx - a * SUBDIV) / SUBDIV; ty = (float)(gy - b * SUBDIV) / SUBDIV; bez3(tx, wx); bez3(ty, wy);
    ws->pos[v][0] = ws->pos[v][1] = ws->pos[v][2] = 0; ws->st[v][0] = ws->st[v][1] = 0; ws->lms[v][0] = ws->lms[v][1] = 0;
    for (j = 0; j < 3; j++) for (i = 0; i < 3; i++) {
      const drawVert_t *d = &cp[(b * 2 + j) * pw + (a * 2 + i)]; float wgt = wx[i] * wy[j];
      for (c = 0; c < 3; c++) ws->pos[v][c] += swap_float(d->xyz[c]) * wgt;
      for (c = 0; c < 2; c++) { ws->st[v][c] += swap_float(d->st[c]) * wgt; ws->lms[v][c] += swap_float(d->lightmap[c]) * wgt; }
      for (c = 0; c < 4; c++) col[c] += d->color[c] * wgt;
    }
    for (c = 0; c < 4; c++) ws->col[v][c] = (byte)(col[c] + 0.5f);
    SFR_ColorShift(ws->col[v], SFR_OVERBRIGHT_SHIFT);
  }
  k = 0;
  for (gy = 0; gy + 1 < ny; gy++) for (gx = 0; gx + 1 < nx; gx++) { int i0 = gy * nx + gx; ws->idx[k++] = i0; ws->idx[k++] = i0 + nx; ws->idx[k++] = i0 + 1; ws->idx[k++] = i0 + 1; ws->idx[k++] = i0 + nx; ws->idx[k++] = i0 + nx + 1; }
}

static void bounds(wsurf_t *ws) {
  int i, c; for (c = 0; c < 3; c++) { ws->mins[c] = 1e9f; ws->maxs[c] = -1e9f; }
  for (i = 0; i < ws->nv; i++) for (c = 0; c < 3; c++) { if (ws->pos[i][c] < ws->mins[c]) ws->mins[c] = ws->pos[i][c]; if (ws->pos[i][c] > ws->maxs[c]) ws->maxs[c] = ws->pos[i][c]; }
}

void SFR_FreeWorld(void) { bspdata = NULL; surfs = NULL; nsurfs = 0; sfr_world_loaded = qfalse; entstr = entp = NULL; lmimgs = NULL; nlm = 0; vis = NULL; }

void SFR_LoadWorld(const char *name) {
  int i, nshaders_l = 0, nverts_l, nidx_l, nsurf_l, nplanes; dshader_t *dsh; drawVert_t *dv; int *di; dsurface_t *ds; byte *lmdata; int lmlen; long n;
  SFR_FreeWorld();
  n = ri.FS_ReadFile(name, (void **)&bspdata);
  if (n <= 0 || !bspdata) { ri.Error(ERR_DROP, "SFR_LoadWorld: não achei %s", name); return; }
  bsp = (dheader_t *)bspdata;
  if (swap_int(bsp->ident) != BSP_IDENT || swap_int(bsp->version) != BSP_VERSION) { ri.Error(ERR_DROP, "SFR_LoadWorld: %s não é um BSP v46 (ident %x versão %d, %ld bytes)", name, swap_int(bsp->ident), swap_int(bsp->version), n); return; }
  /* entidades */
  { int ofs = swap_int(bsp->lumps[LUMP_ENTITIES].fileofs), len = swap_int(bsp->lumps[LUMP_ENTITIES].filelen); entstr = hunk(len + 1); memcpy(entstr, bspdata + ofs, len); entstr[len] = 0; entp = entstr; }
  LUMP(LUMP_SHADERS, dshader_t, dsh, nshaders_l); LUMP(LUMP_PLANES, dplane_t, planes, nplanes); LUMP(LUMP_NODES, dnode_t, nodes, nnodes); LUMP(LUMP_LEAFS, dleaf_t, leafs, nleafs);
  LUMP(LUMP_LEAFSURFACES, int, leafsurf, i); LUMP(LUMP_MODELS, dmodel_t, models, nmodels); LUMP(LUMP_DRAWVERTS, drawVert_t, dv, nverts_l); LUMP(LUMP_DRAWINDEXES, int, di, nidx_l); LUMP(LUMP_SURFACES, dsurface_t, ds, nsurf_l);
  LUMP(LUMP_LIGHTMAPS, byte, lmdata, lmlen);
  { int ofs = swap_int(bsp->lumps[LUMP_VISIBILITY].fileofs), len = swap_int(bsp->lumps[LUMP_VISIBILITY].filelen);
    if (len > 8) { int *h = (int *)(bspdata + ofs); vis_clusters = swap_int(h[0]); vis_rowbytes = swap_int(h[1]); vis = bspdata + ofs + 8; } else vis = NULL; }
  /* lightmaps */
  nlm = lmlen / (128 * 128 * 3); lmimgs = hunk((nlm + 1) * sizeof *lmimgs);
  for (i = 0; i < nlm; i++) lmimgs[i] = SFR_CreateLightmap(i, lmdata + (size_t)i * 128 * 128 * 3);
  /* superfícies */
  surfs = hunk(nsurf_l * sizeof *surfs); nsurfs = nsurf_l; memset(surfs, 0, nsurf_l * sizeof *surfs);
  for (i = 0; i < nsurf_l; i++) {
    wsurf_t *ws = &surfs[i]; dsurface_t *d = &ds[i]; int type = swap_int(d->surfaceType), shn = swap_int(d->shaderNum), lmn = swap_int(d->lightmapNum), fv = swap_int(d->firstVert), nv = swap_int(d->numVerts);
    int fi = swap_int(d->firstIndex), ni = swap_int(d->numIndexes), k, c;
    ws->type = type;
    ws->sh = SFR_FindShader(dsh[shn < 0 || shn >= nshaders_l ? 0 : shn].shader, lmn >= 0 ? lmn : -3, 1);
    ws->lm = (lmn >= 0 && lmn < nlm) ? lmimgs[lmn] : NULL;
    if (type == MST_PATCH) { tessellate(ws, dv + fv, swap_int(d->patchWidth), swap_int(d->patchHeight)); }
    else if (type == MST_PLANAR || type == MST_TRIANGLE_SOUP) {
      ws->nv = nv; ws->ni = ni; ws->pos = hunk(nv * sizeof *ws->pos); ws->st = hunk(nv * sizeof *ws->st); ws->lms = hunk(nv * sizeof *ws->lms); ws->col = hunk(nv * sizeof *ws->col); ws->idx = hunk(ni * sizeof(int));
      for (k = 0; k < nv; k++) { const drawVert_t *v = &dv[fv + k]; for (c = 0; c < 3; c++) ws->pos[k][c] = swap_float(v->xyz[c]); for (c = 0; c < 2; c++) { ws->st[k][c] = swap_float(v->st[c]); ws->lms[k][c] = swap_float(v->lightmap[c]); } for (c = 0; c < 4; c++) ws->col[k][c] = v->color[c]; SFR_ColorShift(ws->col[k], SFR_OVERBRIGHT_SHIFT); }
      for (k = 0; k < ni; k++) ws->idx[k] = swap_int(di[fi + k]);
    } else continue;                                                 /* flares: ignorados */
    bounds(ws);
  }
  sfr_world_loaded = qtrue; framestamp = 0;
  /* cor do céu: se algum shader da lista for de céu, usa um azul; a Fase 4 não desenha skybox */
  sky_color[0] = 0.25f; sky_color[1] = 0.40f; sky_color[2] = 0.65f;
  ri.Printf(PRINT_ALL, "SFR: mapa %s: %d superfícies, %d lightmaps, %d folhas, VRAM de texturas %d KB\n", name, nsurfs, nlm, nleafs, sfr_vram_used_kb());
}

/* ---- consultas ---- */
static int point_leaf(const float p[3]) {
  int n = 0;
  while (n >= 0) { dnode_t *nd = &nodes[n]; dplane_t *pl = &planes[swap_int(nd->planeNum)]; float d = swap_float(pl->normal[0]) * p[0] + swap_float(pl->normal[1]) * p[1] + swap_float(pl->normal[2]) * p[2] - swap_float(pl->dist); n = swap_int(nd->children[d >= 0 ? 0 : 1]); }
  return -n - 1;
}
static int cluster_visible(int from, int to) {
  if (!vis || from < 0) return 1; if (to < 0) return 0;
  return (vis[(size_t)from * vis_rowbytes + (to >> 3)] & (1 << (to & 7))) != 0;
}
qboolean SFR_InPVS(const vec3_t a, const vec3_t b) {
  int la, lb; if (!sfr_world_loaded) return qtrue;
  la = point_leaf(a); lb = point_leaf(b); return (qboolean)cluster_visible(swap_int(leafs[la].cluster), swap_int(leafs[lb].cluster));
}
qboolean SFR_GetEntityToken(char *buf, int size) {
  char *t; if (!entp) { buf[0] = 0; return qfalse; }
  t = COM_Parse(&entp); Q_strncpyz(buf, t, size); if (!entp && !t[0]) return qfalse; return t[0] != 0;
}
int SFR_LightForPoint(vec3_t p, vec3_t amb, vec3_t dir, vec3_t ldir) { (void)p; VectorSet(amb, 150, 150, 150); VectorSet(dir, 100, 100, 100); VectorSet(ldir, 0.3f, 0.3f, 0.9f); return 1; }

/* ---- desenho ---- */
static float fr_n[4][3];                                             /* normais dos 4 planos laterais do frustum (mundo), todos passando pelo olho */
static void setup_frustum(void) {
  float tx = tanf(sfr_view.fovx * (float)M_PI / 360.0f), ty = tanf(sfr_view.fovy * (float)M_PI / 360.0f);
  const float *f = sfr_view.axis[0], *l = sfr_view.axis[1], *u = sfr_view.axis[2]; int i;
  /* direita: normal aponta para dentro = f*tx - ... usando eixos (left = +axis[1]) */
  for (i = 0; i < 3; i++) { fr_n[0][i] = f[i] * tx + l[i] * (-1); fr_n[1][i] = f[i] * tx + l[i]; fr_n[2][i] = f[i] * ty + u[i]; fr_n[3][i] = f[i] * ty - u[i]; }
  /* os vetores acima não são unitários mas o teste só usa o sinal */
  /* plano esquerdo/direito: ponto p está dentro se dot(p-eye, n) >= 0; right: n = f*tx - (-l)... ajustado abaixo */
  for (i = 0; i < 3; i++) { fr_n[0][i] = f[i] * tx - l[i]; fr_n[1][i] = f[i] * tx + l[i]; }
}
static int box_outside(const float mn[3], const float mx[3]) {
  int p, c;
  for (p = 0; p < 4; p++) {
    float s = 0; for (c = 0; c < 3; c++) { float v = fr_n[p][c] >= 0 ? mx[c] : mn[c]; s += (v - sfr_view.org[c]) * fr_n[p][c]; }
    if (s < 0) return 1;
  }
  return 0;
}
static void draw_surface(wsurf_t *ws) { sfr_allow_cull = 1; SFR_DrawShaderTris(ws->sh, ws->nv, (const float (*)[3])ws->pos, (const float (*)[2])ws->st, (const float (*)[2])ws->lms, (const byte (*)[4])ws->col, ws->ni, ws->idx, ws->lm, NULL); }

static int camera_cluster;
static void walk(int n) {
  if (n < 0) {
    dleaf_t *lf = &leafs[-n - 1]; int cl = swap_int(lf->cluster), k, first, cnt; float mn[3], mx[3];
    if (!cluster_visible(camera_cluster, cl)) return;
    for (k = 0; k < 3; k++) { mn[k] = (float)swap_int(lf->mins[k]); mx[k] = (float)swap_int(lf->maxs[k]); }
    if (box_outside(mn, mx)) return;
    first = swap_int(lf->firstLeafSurface); cnt = swap_int(lf->numLeafSurfaces); sf_leafs_drawn++;
    for (k = 0; k < cnt; k++) {
      wsurf_t *ws = &surfs[swap_int(leafsurf[first + k])];
      if (ws->stamp == framestamp || !ws->nv) continue; ws->stamp = framestamp;
      if (box_outside(ws->mins, ws->maxs)) continue;
      draw_surface(ws);
    }
    return;
  }
  { dnode_t *nd = &nodes[n]; float mn[3], mx[3]; int k; for (k = 0; k < 3; k++) { mn[k] = (float)swap_int(nd->mins[k]); mx[k] = (float)swap_int(nd->maxs[k]); }
    if (box_outside(mn, mx)) return;
    walk(swap_int(nd->children[0])); walk(swap_int(nd->children[1])); }
}

void SFR_RenderWorld(void) {
  int leaf; extern void SFR_SetClear(unsigned argb);
  if (!sfr_world_loaded) return;
  framestamp++; sf_leafs_drawn = 0; setup_frustum();
  leaf = point_leaf(sfr_view.org); camera_cluster = swap_int(leafs[leaf].cluster);
  walk(0);
  SFR_SetClear(0xFF000000u | ((unsigned)(sky_color[0] * 255) << 16) | ((unsigned)(sky_color[1] * 255) << 8) | (unsigned)(sky_color[2] * 255));
}

int SFR_BModelCount(void) { return nmodels; }
void SFR_RenderBModel(int idx, const float org[3], const float axis[3][3]) {
  dmodel_t *m; int i, first, cnt; (void)axis;
  if (!sfr_world_loaded || idx < 1 || idx >= nmodels) return;
  m = &models[idx]; first = swap_int(m->firstSurface); cnt = swap_int(m->numSurfaces);
  for (i = 0; i < cnt; i++) {
    wsurf_t *ws = &surfs[first + i]; float (*tp)[3]; int k; if (!ws->nv) continue;
    tp = hunk_temp(ws->nv * 12);
    for (k = 0; k < ws->nv; k++) { float *p = ws->pos[k]; tp[k][0] = org[0] + p[0] * axis[0][0] + p[1] * axis[1][0] + p[2] * axis[2][0]; tp[k][1] = org[1] + p[0] * axis[0][1] + p[1] * axis[1][1] + p[2] * axis[2][1]; tp[k][2] = org[2] + p[0] * axis[0][2] + p[1] * axis[1][2] + p[2] * axis[2][2]; }
    sfr_allow_cull = 1; SFR_DrawShaderTris(ws->sh, ws->nv, (const float (*)[3])tp, (const float (*)[2])ws->st, (const float (*)[2])ws->lms, (const byte (*)[4])ws->col, ws->ni, ws->idx, ws->lm, NULL);
    hunk_temp_free(tp);
  }
}
void SFR_ModelBounds(qhandle_t h, vec3_t mins, vec3_t maxs) {
  int idx = h - 1000000; if (sfr_world_loaded && idx >= 0 && idx < nmodels) { int c; for (c = 0; c < 3; c++) { mins[c] = swap_float(models[idx].mins[c]); maxs[c] = swap_float(models[idx].maxs[c]); } } else { VectorClear(mins); VectorClear(maxs); }
}
