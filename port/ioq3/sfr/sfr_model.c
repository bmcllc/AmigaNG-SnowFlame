/* sfr_model.c - modelos MD3 (itens, armas, personagens), skins, tags e modelos inline do BSP ("*N"). */
#include "sfr_local.h"

#define MAX_MODELS 512
#define BMODEL_BASE 100000
typedef struct { char name[MAX_QPATH]; int nverts, ntris, nshaders; int *tris; float *st; short *xyz; shader_t *shader; } sfmsurf_t;
typedef struct { char name[MAX_QPATH]; int nframes, ntags, nsurfs; md3Frame_t *frames; md3Tag_t *tags; sfmsurf_t *surfs; } sfmodel_t;
static sfmodel_t models[MAX_MODELS]; static int nmodels;
typedef struct { char name[MAX_QPATH]; int n; char surf[16][MAX_QPATH]; shader_t *sh[16]; } sfskin_t;
static sfskin_t skins[128]; static int nskins;

void SFR_ModelsShutdown(void) { nmodels = 0; nskins = 0; }
static void *hunk(int n) { return SFR_HUNK(n); }
static int sw(int v) { return LittleLong(v); }
static float swf(float v) { return LittleFloat(v); }

qhandle_t SFR_RegisterSkin(const char *name) {
  int i; char *buf = NULL, *p, *line; long n; sfskin_t *sk;
  for (i = 0; i < nskins; i++) if (!Q_stricmp(skins[i].name, name)) return i + 1;
  if (nskins >= 128) return 0;
  n = ri.FS_ReadFile(name, (void **)&buf); if (n <= 0 || !buf) return 0;
  sk = &skins[nskins]; memset(sk, 0, sizeof *sk); Q_strncpyz(sk->name, name, MAX_QPATH);
  p = buf;
  while (p && *p) {
    char sname[MAX_QPATH], shname[MAX_QPATH]; char *c;
    line = p; c = strchr(p, '\n'); if (c) { *c = 0; p = c + 1; } else p = NULL;
    { char *cr = strchr(line, '\r'); if (cr) *cr = 0; }
    c = strchr(line, ','); if (!c) continue;
    *c = 0; Q_strncpyz(sname, line, sizeof sname); Q_strncpyz(shname, c + 1, sizeof shname);
    if (!strncmp(sname, "tag_", 4)) continue;
    if (sk->n < 16) { Q_strncpyz(sk->surf[sk->n], sname, MAX_QPATH); sk->sh[sk->n] = SFR_FindShader(shname, -1, 1); sk->n++; }
  }
  ri.FS_FreeFile(buf);
  return ++nskins;
}

static qboolean load_md3(sfmodel_t *m, const char *name) {
  byte *buf = NULL; long n; md3Header_t *h; int i, f, ntags, nframes, nsurfs; byte *base; md3Surface_t *s;
  n = ri.FS_ReadFile(name, (void **)&buf); if (n <= 0 || !buf) return qfalse;
  h = (md3Header_t *)buf;
  if (sw(h->ident) != MD3_IDENT || sw(h->version) != MD3_VERSION) { ri.FS_FreeFile(buf); return qfalse; }
  nframes = sw(h->numFrames); ntags = sw(h->numTags); nsurfs = sw(h->numSurfaces);
  Q_strncpyz(m->name, name, MAX_QPATH); m->nframes = nframes; m->ntags = ntags; m->nsurfs = nsurfs;
  m->frames = hunk(nframes * sizeof(md3Frame_t));
  for (f = 0; f < nframes; f++) { md3Frame_t *src = (md3Frame_t *)(buf + sw(h->ofsFrames)) + f, *d = &m->frames[f]; int c; for (c = 0; c < 3; c++) { d->bounds[0][c] = swf(src->bounds[0][c]); d->bounds[1][c] = swf(src->bounds[1][c]); d->localOrigin[c] = swf(src->localOrigin[c]); } d->radius = swf(src->radius); memcpy(d->name, src->name, 16); }
  m->tags = hunk(nframes * ntags * sizeof(md3Tag_t) + 4);
  for (i = 0; i < nframes * ntags; i++) { md3Tag_t *src = (md3Tag_t *)(buf + sw(h->ofsTags)) + i, *d = &m->tags[i]; int c, a; memcpy(d->name, src->name, MAX_QPATH); for (c = 0; c < 3; c++) { d->origin[c] = swf(src->origin[c]); for (a = 0; a < 3; a++) d->axis[a][c] = swf(src->axis[a][c]); } }
  m->surfs = hunk(nsurfs * sizeof(sfmsurf_t) + 4);
  base = buf + sw(h->ofsSurfaces);
  for (i = 0; i < nsurfs; i++) {
    sfmsurf_t *d = &m->surfs[i]; int nv, nt, k; short *xyz; md3Shader_t *shd;
    s = (md3Surface_t *)base; nv = sw(s->numVerts); nt = sw(s->numTriangles);
    Q_strncpyz(d->name, s->name, MAX_QPATH); d->nverts = nv; d->ntris = nt; d->nshaders = sw(s->numShaders);
    { char *dot = strrchr(d->name, '_'); (void)dot; }
    d->tris = hunk(nt * 3 * sizeof(int)); for (k = 0; k < nt * 3; k++) d->tris[k] = sw(((int *)(base + sw(s->ofsTriangles)))[k]);
    d->st = hunk(nv * 2 * sizeof(float)); for (k = 0; k < nv * 2; k++) d->st[k] = swf(((float *)(base + sw(s->ofsSt)))[k]);
    d->xyz = hunk(nv * nframes * 4 * sizeof(short)); xyz = (short *)(base + sw(s->ofsXyzNormals));
    for (k = 0; k < nv * nframes * 4; k++) d->xyz[k] = (short)LittleShort(xyz[k]);
    shd = (md3Shader_t *)(base + sw(s->ofsShaders));
    d->shader = d->nshaders > 0 ? SFR_FindShader(shd->name, -1, 1) : SFR_ShaderByHandle(0);
    base += sw(s->ofsEnd);
  }
  ri.FS_FreeFile(buf);
  return qtrue;
}

qhandle_t SFR_RegisterModel(const char *name) {
  int i; sfmodel_t *m;
  if (!name || !name[0]) return 0;
  if (name[0] == '*') return BMODEL_BASE + atoi(name + 1);
  for (i = 0; i < nmodels; i++) if (!Q_stricmp(models[i].name, name)) return i + 1;
  if (nmodels >= MAX_MODELS) return 0;
  m = &models[nmodels]; memset(m, 0, sizeof *m);
  if (!load_md3(m, name)) { ri.Printf(PRINT_DEVELOPER, "SFR: modelo %s não carregou\n", name); return 0; }
  return ++nmodels;
}

int SFR_LerpTag(orientation_t *tag, qhandle_t mh, int sf, int ef, float frac, const char *tagName) {
  sfmodel_t *m; int i, a, c;
  memset(tag, 0, sizeof *tag); tag->axis[0][0] = tag->axis[1][1] = tag->axis[2][2] = 1;
  if (mh <= 0 || mh > nmodels) return 0;
  m = &models[mh - 1];
  if (sf < 0 || sf >= m->nframes) sf = 0; if (ef < 0 || ef >= m->nframes) ef = 0;
  for (i = 0; i < m->ntags; i++) if (!strcmp(m->tags[i].name, tagName)) break;
  if (i >= m->ntags) return 0;
  { md3Tag_t *t0 = &m->tags[sf * m->ntags + i], *t1 = &m->tags[ef * m->ntags + i], *o = (md3Tag_t *)0; (void)o;
    for (c = 0; c < 3; c++) { tag->origin[c] = t0->origin[c] * (1 - frac) + t1->origin[c] * frac; for (a = 0; a < 3; a++) tag->axis[a][c] = t0->axis[a][c] * (1 - frac) + t1->axis[a][c] * frac; }
    for (a = 0; a < 3; a++) VectorNormalize(tag->axis[a]); }
  return 1;
}

static void decode_normal(short enc, float n[3]) {
  float lat = ((enc >> 8) & 255) * (float)(2 * M_PI / 255), lng = (enc & 255) * (float)(2 * M_PI / 255);
  n[0] = cosf(lat) * sinf(lng); n[1] = sinf(lat) * sinf(lng); n[2] = cosf(lng);
}

void SFR_DrawEntityModel(const refEntity_t *e) {
  sfmodel_t *m; int s, i; int fr = e->frame, ofr = e->oldframe; float bl = e->backlerp;
  if (e->hModel >= BMODEL_BASE) { float axis[3][3]; int a, c; for (a = 0; a < 3; a++) for (c = 0; c < 3; c++) axis[a][c] = e->axis[a][c]; SFR_RenderBModel(e->hModel - BMODEL_BASE, e->origin, (const float (*)[3])axis); return; }
  if (e->hModel <= 0 || e->hModel > nmodels) return;
  m = &models[e->hModel - 1];
  if (fr < 0 || fr >= m->nframes) fr = 0; if (ofr < 0 || ofr >= m->nframes) ofr = 0;
  for (s = 0; s < m->nsurfs; s++) {
    sfmsurf_t *su = &m->surfs[s]; shader_t *sh = su->shader; float (*pos)[3]; byte (*col)[4]; int nv = su->nverts;
    if (e->customShader) sh = SFR_ShaderByHandle(e->customShader);
    else if (e->customSkin > 0 && e->customSkin <= nskins) { sfskin_t *sk = &skins[e->customSkin - 1]; int k; for (k = 0; k < sk->n; k++) if (!Q_stricmp(sk->surf[k], su->name)) { sh = sk->sh[k]; break; } if (k >= sk->n) continue; }
    if (!sh) continue;
    pos = ri.Hunk_AllocateTempMemory(nv * 12); col = ri.Hunk_AllocateTempMemory(nv * 4);
    for (i = 0; i < nv; i++) {
      short *a = &su->xyz[(fr * nv + i) * 4], *b = &su->xyz[(ofr * nv + i) * 4]; float p[3], n[3], l, w = 1.0f / 64; int c;
      for (c = 0; c < 3; c++) p[c] = (a[c] * (1 - bl) + b[c] * bl) * w;
      for (c = 0; c < 3; c++) pos[i][c] = e->origin[c] + p[0] * e->axis[0][c] + p[1] * e->axis[1][c] + p[2] * e->axis[2][c];
      decode_normal(a[3], n);
      { float wn[3]; for (c = 0; c < 3; c++) wn[c] = n[0] * e->axis[0][c] + n[1] * e->axis[1][c] + n[2] * e->axis[2][c];
        l = 0.55f + 0.45f * (wn[0] * 0.3f + wn[1] * 0.3f + wn[2] * 0.9f); if (l < 0.35f) l = 0.35f; if (l > 1) l = 1; }
      col[i][0] = col[i][1] = col[i][2] = (byte)(l * 255); col[i][3] = e->shaderRGBA[3] ? e->shaderRGBA[3] : 255;
    }
    SFR_DrawShaderTris(sh, nv, (const float (*)[3])pos, (const float (*)[2])su->st, NULL, (const byte (*)[4])col, su->ntris * 3, su->tris, NULL, e->shaderRGBA);
    ri.Hunk_FreeTempMemory(col); ri.Hunk_FreeTempMemory(pos);
  }
}
