/* sfr_shader.c - scripts .shader do Quake 3 (subconjunto): estágios com map/clampMap/animMap, blendFunc, alphaFunc, rgbGen, tcMod (scroll/scale/rotate),
 * cull, sort, surfaceparm, skyparms. Shaders sem script ganham um estágio padrão (lightmap + difuso, vértice, ou 2D). */
#include "sfr_local.h"

#define MAX_SCRIPTS 4096
typedef struct { char name[MAX_QPATH]; const char *text; } script_t;
static script_t scripts[MAX_SCRIPTS]; static int nscripts;
static short script_hash[1024];
static shader_t shaders[SFR_MAX_SHADERS]; static int nshaders;
static char **script_files; static int nscript_files;
qhandle_t SFR_DefaultShader;

static unsigned hash_name(const char *s) { unsigned h = 5381; while (*s) { int c = *s++; if (c >= 'A' && c <= 'Z') c += 32; h = h * 33 + (unsigned)c; } return h & 1023; }

static void index_script_file(char *text) {
  const char *p = text;
  for (;;) {
    char name[MAX_QPATH]; char *tok = COM_ParseExt((char **)&p, qtrue); int depth;
    if (!tok[0]) break;
    Q_strncpyz(name, tok, sizeof name);
    tok = COM_ParseExt((char **)&p, qtrue);
    if (tok[0] != '{') break;                                        /* arquivo malformado */
    if (nscripts < MAX_SCRIPTS) {
      unsigned h = hash_name(name);
      Q_strncpyz(scripts[nscripts].name, name, MAX_QPATH); scripts[nscripts].text = p; script_hash[h] = (short)(nscripts + 1);
      /* encadeamento simples por busca linear a partir do hash: guardamos só o último; a busca percorre tudo se não casar */
      nscripts++;
    }
    depth = 1;
    while (depth > 0) { tok = COM_ParseExt((char **)&p, qtrue); if (!tok[0]) break; if (tok[0] == '{' && !tok[1]) depth++; else if (tok[0] == '}' && !tok[1]) depth--; }
  }
}

void SFR_ShaderInit(void) {
  int i;
  nscripts = 0; nshaders = 0; memset(script_hash, 0, sizeof script_hash);
  script_files = ri.FS_ListFiles("scripts", ".shader", &nscript_files);
  for (i = 0; i < nscript_files; i++) {
    char path[MAX_QPATH]; char *buf = NULL; long n;
    Com_sprintf(path, sizeof path, "scripts/%s", script_files[i]);
    n = ri.FS_ReadFile(path, (void **)&buf);
    if (n <= 0 || !buf) continue;
    index_script_file(buf);                                          /* o buffer fica vivo: os scripts apontam para ele */
  }
  /* shader 0: branco (DrawStretchPic com handle 0) */
  memset(&shaders[0], 0, sizeof shaders[0]); Q_strncpyz(shaders[0].name, "*white", MAX_QPATH); shaders[0].nstages = 1; shaders[0].st[0].img = sfr_whiteimage; shaders[0].st[0].is_white = 1;
  shaders[0].st[0].blend = 1; shaders[0].st[0].src = BFSRCA; shaders[0].st[0].dst = BFINVSRCA; shaders[0].st[0].rgbgen = RGB_VERTEX; shaders[0].st[0].alphagen = AG_VERTEX; shaders[0].sort = 10; shaders[0].cull = 2;
  nshaders = 1;
  SFR_DefaultShader = 0;
}
void SFR_ShaderShutdown(void) { nscripts = 0; nshaders = 0; }

static const char *find_script(const char *name) {
  int i;
  for (i = nscripts - 1; i >= 0; i--) if (!Q_stricmp(scripts[i].name, name)) return scripts[i].text;     /* o último definido vence, como no Q3 */
  return NULL;
}

static int parse_gl_factor(const char *t, int is_src) {
  if (!Q_stricmp(t, "GL_ONE")) return BFONE; if (!Q_stricmp(t, "GL_ZERO")) return BFZERO;
  if (!Q_stricmp(t, "GL_SRC_ALPHA")) return BFSRCA; if (!Q_stricmp(t, "GL_ONE_MINUS_SRC_ALPHA")) return BFINVSRCA;
  if (!Q_stricmp(t, "GL_DST_ALPHA")) return BFDSTA; if (!Q_stricmp(t, "GL_ONE_MINUS_DST_ALPHA")) return BFINVDSTA;
  if (!Q_stricmp(t, "GL_SRC_COLOR")) return BFSRCA; if (!Q_stricmp(t, "GL_ONE_MINUS_SRC_COLOR")) return BFINVSRCA;
  if (!Q_stricmp(t, "GL_DST_COLOR")) return is_src ? BFZERO : BFONE; if (!Q_stricmp(t, "GL_ONE_MINUS_DST_COLOR")) return BFZERO;
  return is_src ? BFONE : BFZERO;
}
static void parse_blendfunc(char **p, sfstage_t *st) {
  char *t = COM_ParseExt(p, qfalse);
  if (!Q_stricmp(t, "add")) { st->blend = 1; st->src = BFONE; st->dst = BFONE; }
  else if (!Q_stricmp(t, "filter")) { st->blend = 2; st->src = BFZERO; st->dst = BFONE; }       /* multiplicação: combinada com o lightmap quando possível */
  else if (!Q_stricmp(t, "blend")) { st->blend = 1; st->src = BFSRCA; st->dst = BFINVSRCA; }
  else {
    char a[32]; int s, d; Q_strncpyz(a, t, sizeof a); s = parse_gl_factor(a, 1); t = COM_ParseExt(p, qfalse); d = parse_gl_factor(t, 0);
    if (!Q_stricmp(a, "GL_DST_COLOR") && !Q_stricmp(t, "GL_ZERO")) { st->blend = 2; st->src = BFZERO; st->dst = BFONE; }
    else if (s == BFONE && d == BFZERO) st->blend = 0;
    else { st->blend = 1; st->src = s; st->dst = d; }
  }
}
static void skip_line(char **p) { while (1) { char *t = COM_ParseExt(p, qfalse); if (!t[0]) break; } }

static void parse_stage(char **p, shader_t *sh) {
  sfstage_t *st; char *t;
  if (sh->nstages >= SFR_MAX_STAGES) { int depth = 1; while (depth > 0) { t = COM_ParseExt(p, qtrue); if (!t[0]) return; if (t[0] == '{') depth++; else if (t[0] == '}') depth--; } return; }
  st = &sh->st[sh->nstages]; memset(st, 0, sizeof *st); st->depthwrite = -1; st->scale[0] = st->scale[1] = 1; st->rgbgen = RGB_IDENTITY; st->rgb[0] = st->rgb[1] = st->rgb[2] = 255;
  for (;;) {
    t = COM_ParseExt(p, qtrue);
    if (!t[0]) return;
    if (t[0] == '}') break;
    if (!Q_stricmp(t, "map") || !Q_stricmp(t, "clampMap")) {
      int clamp = !Q_stricmp(t, "clampMap"); char *m = COM_ParseExt(p, qfalse);
      if (!Q_stricmp(m, "$lightmap")) st->is_lightmap = 1; else if (!Q_stricmp(m, "$whiteimage")) { st->is_white = 1; st->img = sfr_whiteimage; }
      else { st->img = SFR_FindImage(m, !sh->nomip, clamp); if (!st->img) st->img = sfr_defaultimage; }
      st->clamp = clamp;
    } else if (!Q_stricmp(t, "animMap")) {
      char *f = COM_ParseExt(p, qfalse); st->anim_fps = (float)atof(f); st->anim_n = 0;
      for (;;) { char *m = COM_ParseExt(p, qfalse); if (!m[0]) break; if (st->anim_n < 8) { st->anim[st->anim_n] = SFR_FindImage(m, !sh->nomip, 0); if (!st->anim[st->anim_n]) st->anim[st->anim_n] = sfr_defaultimage; st->anim_n++; } }
      st->img = st->anim_n ? st->anim[0] : sfr_defaultimage;
    } else if (!Q_stricmp(t, "blendFunc")) parse_blendfunc(p, st);
    else if (!Q_stricmp(t, "alphaFunc")) { char *f = COM_ParseExt(p, qfalse); (void)f; st->atest = 1; }
    else if (!Q_stricmp(t, "depthWrite")) st->depthwrite = 1;
    else if (!Q_stricmp(t, "rgbGen")) {
      char *g = COM_ParseExt(p, qfalse);
      if (!Q_stricmp(g, "vertex") || !Q_stricmp(g, "exactVertex")) st->rgbgen = RGB_VERTEX; else if (!Q_stricmp(g, "entity")) st->rgbgen = RGB_ENTITY;
      else if (!Q_stricmp(g, "oneMinusVertex")) st->rgbgen = RGB_ONEMINUSVERTEX; else if (!Q_stricmp(g, "lightingDiffuse")) st->rgbgen = RGB_LIGHTING;
      else if (!Q_stricmp(g, "const")) {
        char *a; float c[3]; int k; COM_ParseExt(p, qfalse);                                   /* '(' */
        for (k = 0; k < 3; k++) { a = COM_ParseExt(p, qfalse); c[k] = (float)atof(a); } COM_ParseExt(p, qfalse);
        st->rgbgen = RGB_CONST; for (k = 0; k < 3; k++) st->rgb[k] = (byte)(c[k] < 0 ? 0 : c[k] > 1 ? 255 : c[k] * 255);
      } else skip_line(p);
    } else if (!Q_stricmp(t, "alphaGen")) {
      char *g = COM_ParseExt(p, qfalse);
      if (!Q_stricmp(g, "vertex") || !Q_stricmp(g, "lightingSpecular")) st->alphagen = AG_VERTEX; else if (!Q_stricmp(g, "entity")) st->alphagen = AG_ENTITY; else if (!Q_stricmp(g, "const")) st->alphagen = AG_CONST;
      else skip_line(p);
    } else if (!Q_stricmp(t, "tcGen")) { char *g = COM_ParseExt(p, qfalse); if (!Q_stricmp(g, "environment")) st->tcgen_env = 1; skip_line(p); }
    else if (!Q_stricmp(t, "tcMod")) {
      char *m = COM_ParseExt(p, qfalse);
      if (!Q_stricmp(m, "scroll")) { st->scroll[0] = (float)atof(COM_ParseExt(p, qfalse)); st->scroll[1] = (float)atof(COM_ParseExt(p, qfalse)); st->has_tcmod = 1; }
      else if (!Q_stricmp(m, "scale")) { st->scale[0] = (float)atof(COM_ParseExt(p, qfalse)); st->scale[1] = (float)atof(COM_ParseExt(p, qfalse)); st->has_tcmod = 1; }
      else if (!Q_stricmp(m, "rotate")) { st->rotate = (float)atof(COM_ParseExt(p, qfalse)); st->has_tcmod = 1; }
      else skip_line(p);
    } else skip_line(p);
  }
  sh->nstages++;
}

static void parse_shader(const char *text, shader_t *sh) {
  char *p = (char *)text, *t; int i;
  for (;;) {
    t = COM_ParseExt(&p, qtrue);
    if (!t[0] || t[0] == '}') break;
    if (t[0] == '{') { parse_stage(&p, sh); continue; }
    if (!Q_stricmp(t, "surfaceparm")) {
      char *s = COM_ParseExt(&p, qfalse);
      if (!Q_stricmp(s, "nodraw") || !Q_stricmp(s, "nodrawnonsolid") || !Q_stricmp(s, "clusterportal") || !Q_stricmp(s, "areaportal") || !Q_stricmp(s, "hint") || !Q_stricmp(s, "skip") || !Q_stricmp(s, "origin")) sh->nodraw = qtrue;
      else if (!Q_stricmp(s, "sky")) sh->sky = qtrue; else if (!Q_stricmp(s, "trans")) sh->translucent = qtrue; else if (!Q_stricmp(s, "fog")) sh->fog = qtrue;
      else if (!Q_stricmp(s, "nolightmap")) sh->nolm = qtrue;
    } else if (!Q_stricmp(t, "skyParms")) { sh->sky = qtrue; skip_line(&p); }
    else if (!Q_stricmp(t, "cull")) { char *c = COM_ParseExt(&p, qfalse); if (!Q_stricmp(c, "none") || !Q_stricmp(c, "twosided") || !Q_stricmp(c, "disable")) sh->cull = 2; else if (!Q_stricmp(c, "back") || !Q_stricmp(c, "backside") || !Q_stricmp(c, "backsided")) sh->cull = 1; }
    else if (!Q_stricmp(t, "sort")) { char *s = COM_ParseExt(&p, qfalse);
      sh->sort = !Q_stricmp(s, "portal") ? 1 : !Q_stricmp(s, "sky") ? 2 : !Q_stricmp(s, "opaque") ? 3 : !Q_stricmp(s, "decal") ? 4 : !Q_stricmp(s, "seeThrough") ? 5 : !Q_stricmp(s, "banner") ? 6
        : !Q_stricmp(s, "additive") ? 9 : !Q_stricmp(s, "nearest") ? 15 : !Q_stricmp(s, "underwater") ? 8 : atoi(s); if (sh->sort < 1) sh->sort = 1; if (sh->sort > 15) sh->sort = 15; }
    else if (!Q_stricmp(t, "nomipmaps") || !Q_stricmp(t, "nopicmip")) { if (!Q_stricmp(t, "nomipmaps")) sh->nomip = qtrue; skip_line(&p); }
    else if (!Q_stricmp(t, "polygonOffset")) sh->polyoffset = qtrue;
    else skip_line(&p);
  }
  /* pós-processamento: estágio de lightmap + estágio de difuso viram multitextura; sort padrão */
  for (i = 0; i < sh->nstages; i++) if (sh->st[i].is_lightmap) sh->lightmapped = qtrue;
  if (sh->nolm) sh->lightmapped = qfalse;
  if (!sh->sort) {
    int blended = 0, additive = 0;
    for (i = 0; i < sh->nstages; i++) if (sh->st[i].blend == 1) { blended = 1; if (sh->st[i].src == BFONE && sh->st[i].dst == BFONE) additive = 1; }
    sh->sort = sh->sky ? 2 : (blended ? (additive ? 9 : 10) : 3);
    if (sh->translucent && !blended) sh->sort = 10;
  }
}

static void make_default(shader_t *sh, int lm_class, const char *name, int mip) {
  sfstage_t *st; sfimg_t *img = SFR_FindImage(name, mip, lm_class == 2);
  sh->defaulted = qtrue; if (!img) { img = sfr_defaultimage; }
  if (lm_class == 0) {                                               /* lightmapped: [lightmap][difuso filtro] */
    sh->lightmapped = qtrue; sh->nstages = 2;
    memset(&sh->st[0], 0, sizeof sh->st[0]); sh->st[0].is_lightmap = 1;
    st = &sh->st[1]; memset(st, 0, sizeof *st); st->img = img; st->blend = 2; st->src = BFZERO; st->dst = BFONE; st->scale[0] = st->scale[1] = 1; st->rgb[0] = st->rgb[1] = st->rgb[2] = 255;
    sh->sort = 3;
  } else if (lm_class == 2) {                                        /* 2D */
    sh->nstages = 1; st = &sh->st[0]; memset(st, 0, sizeof *st); st->img = img; st->blend = 1; st->src = BFSRCA; st->dst = BFINVSRCA; st->rgbgen = RGB_VERTEX; st->alphagen = AG_VERTEX; st->scale[0] = st->scale[1] = 1; st->rgb[0] = st->rgb[1] = st->rgb[2] = 255;
    sh->sort = 10; sh->cull = 2; sh->nomip = qtrue;
  } else {                                                           /* 1 = modelos (iluminação dinâmica), 3 = cor de vértice, 4 = branco */
    sh->nstages = 1; st = &sh->st[0]; memset(st, 0, sizeof *st); st->img = img; st->rgbgen = lm_class == 3 ? RGB_VERTEX : lm_class == 4 ? RGB_IDENTITY : RGB_LIGHTING; st->scale[0] = st->scale[1] = 1; st->rgb[0] = st->rgb[1] = st->rgb[2] = 255;
    sh->sort = 3;
  }
}

shader_t *SFR_FindShader(const char *name, int lightmapIndex, int mip) {
  char key[MAX_QPATH]; int i, cls; const char *text; shader_t *sh;
  Q_strncpyz(key, name, sizeof key); { char *d = strrchr(key, '.'); char *sl = strrchr(key, '/'); if (d && (!sl || d > sl)) *d = 0; }
  cls = lightmapIndex >= 0 ? 0 : (lightmapIndex == -4 ? 2 : lightmapIndex == -3 ? 3 : lightmapIndex == -2 ? 4 : 1);   /* 0 lightmap, 1 iluminação dinâmica, 2 2D, 3 cor de vértice, 4 branco */
  for (i = 1; i < nshaders; i++) if (!Q_stricmp(shaders[i].name, key) && shaders[i].lightmapIndex == cls) return &shaders[i];
  if (nshaders >= SFR_MAX_SHADERS) return &shaders[0];
  sh = &shaders[nshaders]; memset(sh, 0, sizeof *sh); Q_strncpyz(sh->name, key, MAX_QPATH); sh->index = nshaders; sh->nomip = !mip;
  text = find_script(key);
  if (text) { parse_shader(text, sh); sh->lightmapIndex = cls; if (!sh->nstages && !sh->nodraw && !sh->sky) { make_default(sh, cls, key, mip); } }
  else make_default(sh, cls, key, mip);
  sh->lightmapIndex = cls;
  /* textura não encontrada e sem script -> mantém o xadrez padrão mas continua desenhável */
  nshaders++;
  return sh;
}
shader_t *SFR_ShaderByHandle(qhandle_t h) { return (h >= 0 && h < nshaders) ? &shaders[h] : &shaders[0]; }
