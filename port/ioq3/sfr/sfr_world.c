/* sfr_world.c - mapa BSP do Quake 3 (versão 46): superfícies planares, malhas e patches de Bézier tesselados, lightmaps, PVS por cluster e culling por frustum. */
#include "sfr_local.h"

typedef struct { int type; shader_t *sh; sfimg_t *lm; int nv, ni; float (*pos)[3]; float (*st)[2]; float (*lms)[2]; byte (*col)[4]; int *idx; float mins[3], maxs[3]; int stamp; float pn[3], pd; /* plano (MST_PLANAR) */ } wsurf_t;
static dheader_t *bsp; static byte *bspdata;
static wsurf_t *surfs; static int nsurfs;
static dplane_t *planes; static dnode_t *nodes; static dleaf_t *leafs; static int *leafsurf; static dmodel_t *models; static int nmodels, nnodes, nleafs;
static byte *vis; static int vis_clusters, vis_rowbytes;
static int *node_parent, *leaf_parent, *node_mark, *leaf_mark, mark_count, mark_cluster;   /* R_MarkLeaves: nós com folha visível pelo PVS */
static char *entstr, *entp; static sfimg_t **lmimgs; static int nlm;
qboolean sfr_world_loaded; static int framestamp; static int sf_leafs_drawn;
static float sky_color[3];

static int swap_int(int v) { return LittleLong(v); }
static float swap_float(float v) { return LittleFloat(v); }
/* troca os bytes de um lump de palavras de 32 bits (int/float) uma vez, na carga: o desenho lê os campos direto (LongSwap custava 5% por quadro) */
static void swap_lump(void *p, int bytes) { int *w = p, i; for (i = 0; i < bytes / 4; i++) w[i] = LittleLong(w[i]); }
#define LUMP(i, type, out, cnt) do { int ofs = swap_int(bsp->lumps[i].fileofs), len = swap_int(bsp->lumps[i].filelen); (out) = (type *)(bspdata + ofs); (cnt) = len / (int)sizeof(type); } while (0)

static void *hunk(int n) { void *p = SFR_HUNK(n); return p; }
extern int sfr_vram_used;
static int sfr_vram_used_kb(void) { return sfr_vram_used / 1024; }
static void *hunk_temp(int n) { return ri.Hunk_AllocateTempMemory(n); }
static void hunk_temp_free(void *p) { ri.Hunk_FreeTempMemory(p); }

/* ---- patches: biquadrático de Bézier 3x3 por sub-patch. Como o r_subdivisions do Q3, cada coluna/linha de sub-patches recebe
   n segmentos pela curvatura: o erro de n cordas de uma quadrática é |P0-2P1+P2|/(4n²), então n = ceil(sqrt(dev/(4*tol))), até SUBDIV.
   shortcut: sem costura entre patches vizinhos (R_StitchPatches do Q3): podem sobrar frestas de até SUBDIV_TOL unidades; costurar se aparecerem ---- */
#define SUBDIV 5
#define SUBDIV_TOL 4.0f
static void bez3(float t, float *w) { float s = 1 - t; w[0] = s * s; w[1] = 2 * s * t; w[2] = t * t; }
static float cpv(const drawVert_t *d, int c) { return swap_float(d->xyz[c]); }
static int segs(float dev) { int n = 1; while (n < SUBDIV && dev > 4 * SUBDIV_TOL * n * n) n++; return n; }
static void tessellate(wsurf_t *ws, const drawVert_t *cp, int pw, int ph) {
  int nsx = (pw - 1) / 2, nsy = (ph - 1) / 2, nx = 1, ny = 1, gx, gy, k, c, i, j; float wx[3], wy[3];
  int segx[16], segy[16], gxa[128], gya[128]; float txs[128], tys[128];
  if (nsx > 16 || nsy > 16) { nsx = nsx > 16 ? 16 : nsx; nsy = nsy > 16 ? 16 : nsy; }
  for (i = 0; i < nsx; i++) { float dev = 0; for (j = 0; j < ph; j++) { const drawVert_t *r = cp + j * pw + i * 2; float e[3], l; for (c = 0; c < 3; c++) e[c] = cpv(r, c) - 2 * cpv(r + 1, c) + cpv(r + 2, c); l = VectorLength(e); if (l > dev) dev = l; } segx[i] = segs(dev); }
  for (j = 0; j < nsy; j++) { float dev = 0; for (i = 0; i < pw; i++) { const drawVert_t *r = cp + j * 2 * pw + i; float e[3], l; for (c = 0; c < 3; c++) e[c] = cpv(r, c) - 2 * cpv(r + pw, c) + cpv(r + 2 * pw, c); l = VectorLength(e); if (l > dev) dev = l; } segy[j] = segs(dev); }
  /* posição de cada linha/coluna da grade: sub-patch e parâmetro t */
  for (i = 0; i < nsx; i++) for (k = 0; k < segx[i]; k++) { gxa[nx - 1] = i; txs[nx - 1] = (float)k / segx[i]; nx++; } gxa[nx - 1] = nsx - 1; txs[nx - 1] = 1;
  for (j = 0; j < nsy; j++) for (k = 0; k < segy[j]; k++) { gya[ny - 1] = j; tys[ny - 1] = (float)k / segy[j]; ny++; } gya[ny - 1] = nsy - 1; tys[ny - 1] = 1;
  ws->nv = nx * ny; ws->ni = (nx - 1) * (ny - 1) * 6;
  ws->pos = hunk(ws->nv * sizeof *ws->pos); ws->st = hunk(ws->nv * sizeof *ws->st); ws->lms = hunk(ws->nv * sizeof *ws->lms); ws->col = hunk(ws->nv * sizeof *ws->col); ws->idx = hunk(ws->ni * sizeof(int));
  for (gy = 0; gy < ny; gy++) for (gx = 0; gx < nx; gx++) {
    int a = gxa[gx], b = gya[gy], v = gy * nx + gx; float col[4] = {0, 0, 0, 0};
    bez3(txs[gx], wx); bez3(tys[gy], wy);
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
  swap_lump(planes, nplanes * (int)sizeof *planes); swap_lump(nodes, nnodes * (int)sizeof *nodes); swap_lump(leafs, nleafs * (int)sizeof *leafs);
  swap_lump(leafsurf, i * (int)sizeof *leafsurf); swap_lump(models, nmodels * (int)sizeof *models);   /* daqui em diante, nativos */
  node_parent = hunk(nnodes * sizeof(int)); leaf_parent = hunk(nleafs * sizeof(int)); node_mark = hunk(nnodes * sizeof(int)); leaf_mark = hunk(nleafs * sizeof(int));
  memset(node_mark, 0, nnodes * sizeof(int)); memset(leaf_mark, 0, nleafs * sizeof(int)); mark_count = 0; mark_cluster = -2; node_parent[0] = -1;
  for (i = 0; i < nnodes; i++) { int c; for (c = 0; c < 2; c++) { int ch = nodes[i].children[c]; if (ch >= 0) node_parent[ch] = i; else leaf_parent[-ch - 1] = i; } }
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
      if (type == MST_PLANAR && nv) { for (c = 0; c < 3; c++) ws->pn[c] = swap_float(d->lightmapVecs[2][c]); ws->pd = DotProduct(ws->pos[0], ws->pn); }
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
  while (n >= 0) { dnode_t *nd = &nodes[n]; dplane_t *pl = &planes[nd->planeNum]; float d = pl->normal[0] * p[0] + pl->normal[1] * p[1] + pl->normal[2] * p[2] - pl->dist; n = nd->children[d >= 0 ? 0 : 1]; }
  return -n - 1;
}
static int cluster_visible(int from, int to) {
  if (!vis || from < 0) return 1; if (to < 0) return 0;
  return (vis[(size_t)from * vis_rowbytes + (to >> 3)] & (1 << (to & 7))) != 0;
}
qboolean SFR_InPVS(const vec3_t a, const vec3_t b) {
  int la, lb; if (!sfr_world_loaded) return qtrue;
  la = point_leaf(a); lb = point_leaf(b); return (qboolean)cluster_visible(leafs[la].cluster, leafs[lb].cluster);
}
qboolean SFR_GetEntityToken(char *buf, int size) {
  char *t; if (!entp) { buf[0] = 0; return qfalse; }
  t = COM_Parse(&entp); Q_strncpyz(buf, t, size); if (!entp && !t[0]) return qfalse; return t[0] != 0;
}
int SFR_LightForPoint(vec3_t p, vec3_t amb, vec3_t dir, vec3_t ldir) { (void)p; VectorSet(amb, 150, 150, 150); VectorSet(dir, 100, 100, 100); VectorSet(ldir, 0.3f, 0.3f, 0.9f); return 1; }

/* ---- desenho ---- */
static float fr_n[4][3];                                             /* normais dos 4 planos laterais do frustum (mundo), todos passando pelo olho */
static float fr_u[4][3];                                             /* as mesmas normais, unitárias (teste de esfera) */
void SFR_SetupFrustum(void) {
  float tx = tanf(sfr_view.fovx * (float)M_PI / 360.0f), ty = tanf(sfr_view.fovy * (float)M_PI / 360.0f);
  const float *f = sfr_view.axis[0], *l = sfr_view.axis[1], *u = sfr_view.axis[2]; int i;
  /* direita: normal aponta para dentro = f*tx - ... usando eixos (left = +axis[1]) */
  for (i = 0; i < 3; i++) { fr_n[0][i] = f[i] * tx + l[i] * (-1); fr_n[1][i] = f[i] * tx + l[i]; fr_n[2][i] = f[i] * ty + u[i]; fr_n[3][i] = f[i] * ty - u[i]; }
  /* os vetores acima não são unitários mas o teste só usa o sinal */
  /* plano esquerdo/direito: ponto p está dentro se dot(p-eye, n) >= 0; right: n = f*tx - (-l)... ajustado abaixo */
  for (i = 0; i < 3; i++) { fr_n[0][i] = f[i] * tx - l[i]; fr_n[1][i] = f[i] * tx + l[i]; }
  for (i = 0; i < 4; i++) { VectorCopy(fr_n[i], fr_u[i]); VectorNormalize(fr_u[i]); }
}
/* esfera fora de algum plano lateral do frustum (atrás do olho também: os quatro planos passam por ele) */
int SFR_CullSphere(const float c[3], float r) {
  int p; float d[3]; VectorSubtract(c, sfr_view.org, d);
  for (p = 0; p < 4; p++) if (DotProduct(d, fr_u[p]) < -r) return 1;
  return 0;
}
/* caixa contra os planos do frustum em 'mask' (bit p = plano p ainda corta o pai): -1 se fora; senão os planos que ainda cortam a caixa.
   Um nó inteiro dentro de um plano dispensa os filhos desse teste (planeBits do R_CullLocalBox do Q3). */
static int box_cull(const float mn[3], const float mx[3], int mask) {
  int p;
  for (p = 0; p < 4; p++) if (mask & (1 << p)) {
    const float *n = fr_n[p], *o = sfr_view.org;
    float far = ((n[0] >= 0 ? mx[0] : mn[0]) - o[0]) * n[0] + ((n[1] >= 0 ? mx[1] : mn[1]) - o[1]) * n[1] + ((n[2] >= 0 ? mx[2] : mn[2]) - o[2]) * n[2];
    if (far < 0) return -1;
    if (((n[0] >= 0 ? mn[0] : mx[0]) - o[0]) * n[0] + ((n[1] >= 0 ? mn[1] : mx[1]) - o[1]) * n[1] + ((n[2] >= 0 ? mn[2] : mx[2]) - o[2]) * n[2] >= 0) mask &= ~(1 << p);
  }
  return mask;
}
static void draw_surface(wsurf_t *ws) {
  sfr_allow_cull = 1;
  if (ws->type == MST_PLANAR && ws->sh->cull != 2) {                 /* face planar: o plano decide para a superfície inteira (R_CullSurface do Q3) */
    float d = DotProduct(sfr_view.org, ws->pn) - ws->pd;
    if (ws->sh->cull == 0 ? d < -8 : d > 8) return;
    if (ws->sh->cull == 0 ? d > 8 : d < -8) sfr_allow_cull = 0;       /* longe do plano: todos os triângulos já estão do lado visível */
  }
  SFR_DrawShaderTris(ws->sh, ws->nv, (const float (*)[3])ws->pos, (const float (*)[2])ws->st, (const float (*)[2])ws->lms, (const byte (*)[4])ws->col, ws->ni, ws->idx, ws->lm, NULL);
}

static int camera_cluster;
static void mark_leaves(void) {                                      /* só quando a câmera muda de cluster */
  int i, n;
  if (camera_cluster == mark_cluster) return;
  mark_cluster = camera_cluster; mark_count++;
  for (i = 0; i < nleafs; i++) {
    if (!cluster_visible(camera_cluster, leafs[i].cluster)) continue;
    leaf_mark[i] = mark_count;
    for (n = leaf_parent[i]; n >= 0 && node_mark[n] != mark_count; n = node_parent[n]) node_mark[n] = mark_count;
  }
}
static void walk(int n, int mask) {
  if (n < 0) {
    dleaf_t *lf = &leafs[-n - 1]; int k, first, cnt; float mn[3], mx[3];
    if (leaf_mark[-n - 1] != mark_count) return;
    for (k = 0; k < 3; k++) { mn[k] = (float)lf->mins[k]; mx[k] = (float)lf->maxs[k]; }
    if (mask && (mask = box_cull(mn, mx, mask)) < 0) return;
    first = lf->firstLeafSurface; cnt = lf->numLeafSurfaces; sf_leafs_drawn++;
    for (k = 0; k < cnt; k++) {
      wsurf_t *ws = &surfs[leafsurf[first + k]];
      if (ws->stamp == framestamp || !ws->nv) continue; ws->stamp = framestamp;
      if (mask && box_cull(ws->mins, ws->maxs, mask) < 0) continue;
      draw_surface(ws);
    }
    return;
  }
  if (node_mark[n] != mark_count) return;
  { dnode_t *nd = &nodes[n]; float mn[3], mx[3]; int k; for (k = 0; k < 3; k++) { mn[k] = (float)nd->mins[k]; mx[k] = (float)nd->maxs[k]; }
    if (mask && (mask = box_cull(mn, mx, mask)) < 0) return;
    walk(nd->children[0], mask); walk(nd->children[1], mask); }
}

void SFR_RenderWorld(void) {
  int leaf; extern void SFR_SetClear(unsigned argb);
  if (!sfr_world_loaded) return;
  framestamp++; sf_leafs_drawn = 0;
  leaf = point_leaf(sfr_view.org); camera_cluster = leafs[leaf].cluster; mark_leaves();
  walk(0, 15);
  SFR_SetClear(0xFF000000u | ((unsigned)(sky_color[0] * 255) << 16) | ((unsigned)(sky_color[1] * 255) << 8) | (unsigned)(sky_color[2] * 255));
}

int SFR_BModelCount(void) { return nmodels; }
void SFR_RenderBModel(int idx, const float org[3], const float axis[3][3]) {
  dmodel_t *m; int i, first, cnt; (void)axis;
  if (!sfr_world_loaded || idx < 1 || idx >= nmodels) return;
  m = &models[idx]; first = m->firstSurface; cnt = m->numSurfaces;
  for (i = 0; i < cnt; i++) {
    wsurf_t *ws = &surfs[first + i]; float (*tp)[3]; int k; if (!ws->nv) continue;
    tp = hunk_temp(ws->nv * 12);
    for (k = 0; k < ws->nv; k++) { float *p = ws->pos[k]; tp[k][0] = org[0] + p[0] * axis[0][0] + p[1] * axis[1][0] + p[2] * axis[2][0]; tp[k][1] = org[1] + p[0] * axis[0][1] + p[1] * axis[1][1] + p[2] * axis[2][1]; tp[k][2] = org[2] + p[0] * axis[0][2] + p[1] * axis[1][2] + p[2] * axis[2][2]; }
    sfr_allow_cull = 1; SFR_DrawShaderTris(ws->sh, ws->nv, (const float (*)[3])tp, (const float (*)[2])ws->st, (const float (*)[2])ws->lms, (const byte (*)[4])ws->col, ws->ni, ws->idx, ws->lm, NULL);
    hunk_temp_free(tp);
  }
}
void SFR_ModelBounds(qhandle_t h, vec3_t mins, vec3_t maxs) {
  int idx = h - 1000000; if (sfr_world_loaded && idx >= 0 && idx < nmodels) { int c; for (c = 0; c < 3; c++) { mins[c] = models[idx].mins[c]; maxs[c] = models[idx].maxs[c]; } } else { VectorClear(mins); VectorClear(maxs); }
}
