/* sfr_image.c - imagens do renderer SF-100: carrega TGA/JPG/PNG/BMP/PCX (carregadores do renderercommon do ioq3), redimensiona para potência de 2,
 * converte para RGB565 / ARGB4444 (com mipmaps) e grava na VRAM do GPU. */
#include "sfr_local.h"

void R_LoadBMP(const char *name, byte **pic, int *width, int *height);
void R_LoadJPG(const char *name, byte **pic, int *width, int *height);
void R_LoadPCX(const char *name, byte **pic, int *width, int *height);
void R_LoadPNG(const char *name, byte **pic, int *width, int *height);
void R_LoadTGA(const char *name, byte **pic, int *width, int *height);

static sfimg_t images[SFR_MAX_IMAGES]; static int nimages;
static unsigned vram_next;
sfimg_t *sfr_whiteimage, *sfr_defaultimage;
int sfr_vram_used;

static unsigned vram_alloc(unsigned bytes) {
  unsigned a = (vram_next + 15) & ~15u;
  if (a + bytes > SFR_TEX_END) return 0;
  vram_next = a + bytes; return a;
}
static int ilog2(int v) { int l = 0; while ((1 << l) < v) l++; return l; }
static int nearest_pot(int v) { int lo = 1; while (lo * 2 <= v) lo *= 2; return (v - lo) < (lo * 2 - v) ? lo : lo * 2; }

/* reduz/amplia RGBA w x h -> dw x dh (filtro de caixa ao reduzir; vizinho mais próximo ao ampliar) */
static byte *resample(const byte *src, int w, int h, int dw, int dh) {
  byte *dst = malloc((size_t)dw * dh * 4); int x, y, c;
  if (!dst) return NULL;
  for (y = 0; y < dh; y++) {
    int y0 = y * h / dh, y1 = (y + 1) * h / dh; if (y1 <= y0) y1 = y0 + 1; if (y1 > h) y1 = h;
    for (x = 0; x < dw; x++) {
      int x0 = x * w / dw, x1 = (x + 1) * w / dw, sx, sy; unsigned acc[4] = {0, 0, 0, 0}; unsigned n;
      if (x1 <= x0) x1 = x0 + 1; if (x1 > w) x1 = w;
      for (sy = y0; sy < y1; sy++) for (sx = x0; sx < x1; sx++) for (c = 0; c < 4; c++) acc[c] += src[((size_t)sy * w + sx) * 4 + c];
      n = (unsigned)((y1 - y0) * (x1 - x0));
      for (c = 0; c < 4; c++) dst[((size_t)y * dw + x) * 4 + c] = (byte)(acc[c] / n);
    }
  }
  return dst;
}
static void box_down(const byte *src, int w, int h, byte *dst) {         /* próximo nível de mip (w,h >= 1) */
  int dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1, x, y, c;
  for (y = 0; y < dh; y++) for (x = 0; x < dw; x++) {
    int x0 = w > 1 ? x * 2 : 0, y0 = h > 1 ? y * 2 : 0, x1 = w > 1 ? x0 + 1 : 0, y1 = h > 1 ? y0 + 1 : 0;
    for (c = 0; c < 4; c++) dst[((size_t)y * dw + x) * 4 + c] = (byte)((src[((size_t)y0 * w + x0) * 4 + c] + src[((size_t)y0 * w + x1) * 4 + c] + src[((size_t)y1 * w + x0) * 4 + c] + src[((size_t)y1 * w + x1) * 4 + c] + 2) >> 2);
  }
}
static void put_level(unsigned short *out, const byte *p, int w, int h, int alpha) {
  int i, n = w * h;
  for (i = 0; i < n; i++) {
    unsigned r = p[i * 4], g = p[i * 4 + 1], b = p[i * 4 + 2], a = p[i * 4 + 3];
    out[i] = alpha ? (unsigned short)(((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)) : (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  }
}

sfimg_t *SFR_CreateImage(const char *name, const byte *rgba, int w, int h, int mip, int clamp) {
  sfimg_t *im; int dw, dh, alpha = 0, i, lvl, cw, ch, nl = 1; byte *base, *cur, *nxt; unsigned total = 0, addr, off = 0; unsigned short *dst;
  if (nimages >= SFR_MAX_IMAGES) return sfr_defaultimage;
  dw = nearest_pot(w); dh = nearest_pot(h);
  while (dw > SFR_MAXTEX) dw /= 2;
  while (dh > SFR_MAXTEX) dh /= 2;
  for (i = 0; i < w * h; i++) if (rgba[i * 4 + 3] != 255) { alpha = 1; break; }
  base = (dw == w && dh == h) ? NULL : resample(rgba, w, h, dw, dh);
  if (mip) { cw = dw; ch = dh; while (cw > 1 || ch > 1) { cw = cw > 1 ? cw / 2 : 1; ch = ch > 1 ? ch / 2 : 1; nl++; } }
  cw = dw; ch = dh; for (lvl = 0; lvl < nl; lvl++) { total += (unsigned)cw * ch * 2; cw = cw > 1 ? cw / 2 : 1; ch = ch > 1 ? ch / 2 : 1; }
  addr = vram_alloc(total);
  if (!addr) { if (base) free(base); return sfr_defaultimage; }
  dst = (unsigned short *)addr; cw = dw; ch = dh;
  cur = malloc((size_t)dw * dh * 4); nxt = malloc((size_t)dw * dh * 4);
  memcpy(cur, base ? base : rgba, (size_t)dw * dh * 4);
  for (lvl = 0; lvl < nl; lvl++) {
    put_level(dst + off / 2, cur, cw, ch, alpha); off += (unsigned)cw * ch * 2;
    if (lvl + 1 < nl) { byte *t; box_down(cur, cw, ch, nxt); t = cur; cur = nxt; nxt = t; cw = cw > 1 ? cw / 2 : 1; ch = ch > 1 ? ch / 2 : 1; }
  }
  free(cur); free(nxt); if (base) free(base);
  im = &images[nimages]; memset(im, 0, sizeof *im); im->id = nimages++;
  Q_strncpyz(im->name, name, sizeof im->name); im->w = w; im->h = h; im->lw = ilog2(dw); im->lh = ilog2(dh);
  im->fmt = alpha ? TFX_ARGB4444 : TFX_RGB565; im->nlevels = nl; im->flags = (clamp ? TF_CLAMP : 0) | (mip ? TF_MIPS : 0); im->addr = addr; im->alpha = alpha;
  sfr_vram_used += (int)total;
  return im;
}

/* lightmap do BSP: 128x128 RGB; o x2 compensa o "overbright" do Q3 (sem hardware de gama) */
sfimg_t *SFR_CreateLightmap(int index, const byte *rgb) {
  byte *tmp = malloc(128 * 128 * 4); char nm[32]; int i; sfimg_t *im;
  if (!tmp) return sfr_whiteimage;
  for (i = 0; i < 128 * 128; i++) { int c; for (c = 0; c < 3; c++) { int v = rgb[i * 3 + c] * 2; tmp[i * 4 + c] = (byte)(v > 255 ? 255 : v); } tmp[i * 4 + 3] = 255; }
  Com_sprintf(nm, sizeof nm, "*lightmap%d", index);
  im = SFR_CreateImage(nm, tmp, 128, 128, 0, 1); free(tmp); return im;
}

static sfimg_t *find_cached(const char *name, int mip, int clamp) {
  int i; int want = (clamp ? TF_CLAMP : 0) | (mip ? TF_MIPS : 0);
  for (i = 0; i < nimages; i++) if (!Q_stricmp(images[i].name, name) && images[i].flags == want) return &images[i];
  return NULL;
}
static void strip_ext(char *s) { char *d = strrchr(s, '.'); char *sl = strrchr(s, '/'); if (d && (!sl || d > sl)) *d = 0; }

static qboolean load_any(const char *name, byte **pic, int *w, int *h) {
  char base[MAX_QPATH], path[MAX_QPATH]; const char *ext; int i;
  static const char *exts[] = { ".tga", ".jpg", ".png", ".bmp", ".pcx" };
  *pic = NULL;
  Q_strncpyz(base, name, sizeof base); ext = strrchr(name, '.');
  if (ext && !strchr(ext, '/')) {                                    /* extensão explícita: tenta primeiro */
    if (!Q_stricmp(ext, ".tga")) R_LoadTGA(name, pic, w, h); else if (!Q_stricmp(ext, ".jpg") || !Q_stricmp(ext, ".jpeg")) R_LoadJPG(name, pic, w, h);
    else if (!Q_stricmp(ext, ".png")) R_LoadPNG(name, pic, w, h); else if (!Q_stricmp(ext, ".bmp")) R_LoadBMP(name, pic, w, h); else if (!Q_stricmp(ext, ".pcx")) R_LoadPCX(name, pic, w, h);
    if (*pic) return qtrue;
    strip_ext(base);
  }
  for (i = 0; i < 5; i++) {
    Com_sprintf(path, sizeof path, "%s%s", base, exts[i]);
    if (i == 0) R_LoadTGA(path, pic, w, h); else if (i == 1) R_LoadJPG(path, pic, w, h); else if (i == 2) R_LoadPNG(path, pic, w, h); else if (i == 3) R_LoadBMP(path, pic, w, h); else R_LoadPCX(path, pic, w, h);
    if (*pic) return qtrue;
  }
  return qfalse;
}

sfimg_t *SFR_FindImage(const char *name, int mip, int clamp) {
  sfimg_t *im; byte *pic; int w, h;
  if (!name || !name[0]) return NULL;
  if ((im = find_cached(name, mip, clamp))) return im;
  if (!load_any(name, &pic, &w, &h)) return NULL;
  im = SFR_CreateImage(name, pic, w, h, mip, clamp); ri.Free(pic);
  return im;
}

void SFR_ImageInit(void) {
  byte px[4 * 8 * 8]; int i;
  nimages = 0; vram_next = SFR_TEX_BASE; sfr_vram_used = 0;
  for (i = 0; i < 8 * 8; i++) { int on = ((i & 7) / 4 + (i / 8) / 4) & 1; px[i * 4] = on ? 255 : 90; px[i * 4 + 1] = 0; px[i * 4 + 2] = on ? 255 : 90; px[i * 4 + 3] = 255; }
  sfr_defaultimage = &images[0]; sfr_defaultimage = SFR_CreateImage("*default", px, 8, 8, 1, 0);
  memset(px, 255, sizeof px); sfr_whiteimage = SFR_CreateImage("*white", px, 8, 8, 0, 1);
}
void SFR_ImageShutdown(void) { nimages = 0; }
