/* sfr_backend.c - projeção, recorte, acúmulo de triângulos em espaço de tela e montagem da lista de comandos do FlameCore (uma por quadro). */
#include "sfr_local.h"

sfview_t sfr_view; int sfr_2d; float sfr_color2d[4] = {1, 1, 1, 1};
static unsigned *vbuf; static unsigned *cmdbuf; static sfpacket_t *packets; static int nverts, npackets, scene_first_packet, overflow_warned;
static int rt_cur; static unsigned frame_clear = 0xFF000000u; static int have_3d;
int sfr_frame_count;
#define CMD_WORDS 262144

float SFR_Time(void) { static unsigned long long t0; extern unsigned long long sf_time_ns(void); unsigned long long n = sf_time_ns(); if (!t0) t0 = n; return (float)((n - t0) / 1000000ull) * 0.001f; }

void SFR_BackendInit(void) {
  vbuf = malloc((size_t)SFR_MAX_VERTS * 48 + 16); cmdbuf = malloc(CMD_WORDS * 4 + 16); packets = malloc(sizeof(sfpacket_t) * SFR_MAX_PACKETS);
  vbuf = (unsigned *)(((unsigned)vbuf + 15) & ~15u); cmdbuf = (unsigned *)(((unsigned)cmdbuf + 15) & ~15u);
  nverts = npackets = 0; rt_cur = 0;
  VID_REG(0x00) = 2;                                                           /* VMODE_480P */
  VID_REG(0x40 + 0x00) = SFR_RT_ADDR0; VID_REG(0x40 + 0x04) = SFR_W * 4; VID_REG(0x40 + 0x1C) = 1;   /* camada 0: RT 0, habilitada */
}
void SFR_BackendShutdown(void) { }

void SFR_SetView(const refdef_t *fd) {
  int i, j; float tx, ty;
  VectorCopy(fd->vieworg, sfr_view.org);
  for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) sfr_view.axis[i][j] = fd->viewaxis[i][j];
  sfr_view.fovx = fd->fov_x; sfr_view.fovy = fd->fov_y;
  sfr_view.x = fd->x; sfr_view.w = fd->width; sfr_view.h = fd->height; sfr_view.y = SFR_H - (fd->y + fd->height);
  sfr_view.znear = 8.0f; sfr_view.zfar = 16384.0f;
  tx = tanf(fd->fov_x * (float)M_PI / 360.0f); ty = tanf(fd->fov_y * (float)M_PI / 360.0f);
  (void)tx; (void)ty;
}

void SFR_BeginFrame(void) { nverts = 0; npackets = 0; scene_first_packet = 0; have_3d = 0; frame_clear = 0xFF000000u; overflow_warned = 0; sfr_2d = 1; }
int SFR_PacketCount(void) { return npackets; }
void SFR_Packets(int *np, int *nv) { *np = npackets; *nv = nverts; }

/* ordena (estável) os pacotes criados desde 'first' pela chave de sort do shader */
void SFR_FlushSort(int first) {
  int i, j; sfpacket_t t;
  for (i = first + 1; i < npackets; i++) { t = packets[i]; for (j = i; j > first && packets[j - 1].sortkey > t.sortkey; j--) packets[j] = packets[j - 1]; packets[j] = t; }
}

/* ---------------- recorte e projeção ---------------- */
typedef struct { float x, y, z, u, v, u1, v1, r, g, b, a; } cv_t;
static cv_t clipbuf_a[12], clipbuf_b[12];

static int clip_plane(const cv_t *in, int n, cv_t *out, float ax, float ay, float az, float d) {   /* mantém pontos com ax*x+ay*y+az*z >= d */
  int i, m = 0;
  for (i = 0; i < n; i++) {
    const cv_t *a = &in[i], *b = &in[(i + 1) % n];
    float da = ax * a->x + ay * a->y + az * a->z - d, db = ax * b->x + ay * b->y + az * b->z - d;
    if (da >= 0) out[m++] = *a;
    if ((da >= 0) != (db >= 0)) {
      float t = da / (da - db); cv_t *o = &out[m++];
      o->x = a->x + (b->x - a->x) * t; o->y = a->y + (b->y - a->y) * t; o->z = a->z + (b->z - a->z) * t; o->u = a->u + (b->u - a->u) * t; o->v = a->v + (b->v - a->v) * t;
      o->u1 = a->u1 + (b->u1 - a->u1) * t; o->v1 = a->v1 + (b->v1 - a->v1) * t; o->r = a->r + (b->r - a->r) * t; o->g = a->g + (b->g - a->g) * t; o->b = a->b + (b->b - a->b) * t; o->a = a->a + (b->a - a->a) * t;
    }
    if (m > 10) break;
  }
  return m;
}

static void put_vertex(const cv_t *c, float cx0, float cy0, float fx, float fy) {
  unsigned *p; float rhw = 1.0f / c->z, depth;
  union { float f; unsigned u; } cvt;
  if (nverts >= SFR_MAX_VERTS) return;
  depth = (sfr_view.zfar / (sfr_view.zfar - sfr_view.znear)) * (1.0f - sfr_view.znear * rhw);
  if (depth < 0) depth = 0; if (depth > 1) depth = 1;
  p = vbuf + nverts * 12;
#define PF(i, v) do { cvt.f = (v); p[i] = cvt.u; } while (0)
  PF(0, cx0 + c->x * rhw * fx); PF(1, cy0 - c->y * rhw * fy); PF(2, depth); PF(3, rhw);
  PF(4, c->r); PF(5, c->g); PF(6, c->b); PF(7, c->a); PF(8, c->u); PF(9, c->v); PF(10, c->u1); PF(11, c->v1);
#undef PF
  nverts++;
}

static void emit_poly(cv_t *poly, int n) {
  float fx, fy, cx0, cy0, tx, ty; int i; float gb = 1.4f;
  cv_t *src = poly, *dst = clipbuf_b; int allin = 1;
  tx = tanf(sfr_view.fovx * (float)M_PI / 360.0f); ty = tanf(sfr_view.fovy * (float)M_PI / 360.0f);
  fx = (sfr_view.w * 0.5f) / tx; fy = (sfr_view.h * 0.5f) / ty; cx0 = sfr_view.x + sfr_view.w * 0.5f; cy0 = sfr_view.y + sfr_view.h * 0.5f;
  for (i = 0; i < n; i++) {
    if (poly[i].z < sfr_view.znear || poly[i].x + poly[i].z * tx * gb < 0 || poly[i].z * tx * gb - poly[i].x < 0 || poly[i].y + poly[i].z * ty * gb < 0 || poly[i].z * ty * gb - poly[i].y < 0) { allin = 0; break; }
  }
  if (!allin) {
    n = clip_plane(src, n, dst, 0, 0, 1, sfr_view.znear); if (n < 3) return; src = dst; dst = (src == clipbuf_a) ? clipbuf_b : clipbuf_a;
    n = clip_plane(src, n, dst, 1, 0, tx * gb, 0); if (n < 3) return; src = dst; dst = (src == clipbuf_a) ? clipbuf_b : clipbuf_a;
    n = clip_plane(src, n, dst, -1, 0, tx * gb, 0); if (n < 3) return; src = dst; dst = (src == clipbuf_a) ? clipbuf_b : clipbuf_a;
    n = clip_plane(src, n, dst, 0, 1, ty * gb, 0); if (n < 3) return; src = dst; dst = (src == clipbuf_a) ? clipbuf_b : clipbuf_a;
    n = clip_plane(src, n, dst, 0, -1, ty * gb, 0); if (n < 3) return; src = dst;
  }
  if (nverts + (n - 2) * 3 > SFR_MAX_VERTS) { if (!overflow_warned) { ri.Printf(PRINT_WARNING, "SFR: buffer de vértices cheio\n"); overflow_warned = 1; } return; }
  for (i = 1; i + 1 < n; i++) { put_vertex(&src[0], cx0, cy0, fx, fy); put_vertex(&src[i], cx0, cy0, fx, fy); put_vertex(&src[i + 1], cx0, cy0, fx, fy); }
}

/* pacote de desenho corrente (mesmo estado = estende) */
static sfpacket_t *packet_for(int flags, int src, int dst, const sfimg_t *t0, const sfimg_t *t1, int sortkey) {
  sfpacket_t *p = npackets ? &packets[npackets - 1] : NULL; unsigned a0 = t0 ? t0->addr : 0, a1 = t1 ? t1->addr : 0;
  if (p && p->count && p->first + p->count == nverts && p->flags == flags && p->src == src && p->dst == dst && p->a0 == a0 && p->a1 == a1 && p->sortkey == sortkey) return p;
  if (npackets >= SFR_MAX_PACKETS) return NULL;
  p = &packets[npackets++]; memset(p, 0, sizeof *p); p->first = nverts; p->flags = flags; p->src = src; p->dst = dst; p->a0 = a0; p->a1 = a1; p->sortkey = sortkey;
  if (t0) { p->lw0 = t0->lw; p->lh0 = t0->lh; p->fmt0 = t0->fmt; p->fl0 = t0->flags; p->nl0 = t0->nlevels; }
  if (t1) { p->lw1 = t1->lw; p->lh1 = t1->lh; p->fmt1 = t1->fmt; p->fl1 = t1->flags; p->nl1 = t1->nlevels; }
  return p;
}

static void stage_uv(const sfstage_t *st, float t, float *u, float *v) {
  if (!st->has_tcmod) return;
  { float uu = *u * st->scale[0], vv = *v * st->scale[1];
    if (st->rotate != 0) { float a = st->rotate * t * (float)M_PI / 180.0f, s = sinf(a), c = cosf(a), du = uu - 0.5f, dv = vv - 0.5f; uu = du * c - dv * s + 0.5f; vv = du * s + dv * c + 0.5f; }
    *u = uu + st->scroll[0] * t; *v = vv + st->scroll[1] * t; }
}

/* desenha triângulos de um shader: pos em mundo (ou já em câmera se cam_space), st/lm/col por vértice */
static sfv_t *scratch; static int scratch_cap;
void SFR_DrawShaderTris(shader_t *sh, int nv, const float (*pos)[3], const float (*st)[2], const float (*lm)[2], const byte (*col)[4], int nidx, const int *idx, sfimg_t *lm_img, const byte ent_rgba[4]) {
  int pass, i, t, used_lm = 0; float time = SFR_Time();
  if (sh->nodraw || sh->sky || nv < 3 || nidx < 3) return;
  if (nv > scratch_cap) { free(scratch); scratch_cap = nv + 256; scratch = malloc(sizeof(sfv_t) * (size_t)scratch_cap); }
  for (i = 0; i < nv; i++) {                                         /* mundo -> câmera */
    float d[3]; VectorSubtract(pos[i], sfr_view.org, d);
    scratch[i].x = -DotProduct(d, sfr_view.axis[1]); scratch[i].y = DotProduct(d, sfr_view.axis[2]); scratch[i].z = DotProduct(d, sfr_view.axis[0]);
  }
  /* descarta se tudo atrás da câmera */
  { int front = 0; for (i = 0; i < nv; i++) if (scratch[i].z > sfr_view.znear) { front = 1; break; } if (!front) return; }
  for (pass = 0; pass < sh->nstages; pass++) {
    const sfstage_t *s = &sh->st[pass]; sfimg_t *t0 = NULL, *t1 = NULL; int flags, src = BFONE, dst = BFZERO;
    if (s->is_lightmap) continue;
    t0 = s->img; if (s->anim_n > 1) t0 = s->anim[(int)(time * s->anim_fps) % s->anim_n]; if (!t0) t0 = sfr_defaultimage;
    flags = ST_TEX0 | ST_BILINEAR | (sfr_2d ? 0 : ST_DEPTH_TEST) | (sfr_2d ? 0 : (1u << 12));          /* profundidade LEQUAL (passes extras na mesma superfície) */
    if (sh->lightmapped && !used_lm && (s->blend == 0 || s->blend == 2) && !sfr_2d) { t1 = lm_img ? lm_img : sfr_whiteimage; flags |= ST_TEX1 | ST_UV1; used_lm = 1; }
    if (s->blend == 1) { flags |= ST_BLEND; src = s->src; dst = s->dst; }
    else if (!sfr_2d) flags |= ST_DEPTH_WRITE;
    if (s->atest) flags |= ST_ATEST;
    { sfpacket_t *pk = packet_for(flags, src, dst, t0, t1, sfr_2d ? 0 : sh->sort); if (!pk) return;
      for (t = 0; t + 2 < nidx; t += 3) {
        cv_t poly[3]; int k, before = nverts;
        for (k = 0; k < 3; k++) {
          int vi = idx[t + k]; cv_t *c = &poly[k]; float u = st[vi][0], v = st[vi][1];
          c->x = scratch[vi].x; c->y = scratch[vi].y; c->z = scratch[vi].z;
          if (s->tcgen_env) { u = 0.5f; v = 0.5f; }
          stage_uv(s, time, &u, &v); c->u = u; c->v = v;
          c->u1 = lm ? lm[vi][0] : 0; c->v1 = lm ? lm[vi][1] : 0;
          switch (s->rgbgen) {
            case RGB_VERTEX: if (col) { c->r = col[vi][0] * (1.0f / 255); c->g = col[vi][1] * (1.0f / 255); c->b = col[vi][2] * (1.0f / 255); } else c->r = c->g = c->b = 1; break;
            case RGB_ONEMINUSVERTEX: if (col) { c->r = 1 - col[vi][0] * (1.0f / 255); c->g = 1 - col[vi][1] * (1.0f / 255); c->b = 1 - col[vi][2] * (1.0f / 255); } else c->r = c->g = c->b = 0; break;
            case RGB_LIGHTING: if (col) { c->r = col[vi][0] * (1.0f / 255); c->g = col[vi][1] * (1.0f / 255); c->b = col[vi][2] * (1.0f / 255); break; }
            case RGB_ENTITY: if (ent_rgba) { c->r = ent_rgba[0] * (1.0f / 255); c->g = ent_rgba[1] * (1.0f / 255); c->b = ent_rgba[2] * (1.0f / 255); } else c->r = c->g = c->b = 1; break;
            case RGB_CONST: c->r = s->rgb[0] * (1.0f / 255); c->g = s->rgb[1] * (1.0f / 255); c->b = s->rgb[2] * (1.0f / 255); break;
            default: c->r = c->g = c->b = 1; break;
          }
          c->a = 1;
          if (s->alphagen == AG_VERTEX && col) c->a = col[vi][3] * (1.0f / 255); else if (s->alphagen == AG_ENTITY && ent_rgba) c->a = ent_rgba[3] * (1.0f / 255);
          else if (s->rgbgen == RGB_VERTEX && sfr_2d && col) c->a = col[vi][3] * (1.0f / 255);
        }
        emit_poly(poly, 3);
        pk->count += nverts - before;
      }
    }
  }
}

/* quad 2D em coordenadas virtuais 640x480 */
void SFR_Draw2D(float x, float y, float w, float h, float s1, float t1, float s2, float t2, shader_t *sh, const float *rgba) {
  static const int idx[6] = { 0, 1, 2, 0, 2, 3 }; float pos[4][3], st[4][2]; byte col[4][4]; int i, saved = sfr_2d; sfview_t sv = sfr_view; float c[4];
  if (rgba) { for (i = 0; i < 4; i++) c[i] = rgba[i]; } else { for (i = 0; i < 4; i++) c[i] = sfr_color2d[i]; }
  /* câmera "ortográfica": vista de 90 graus olhando para +z a z = 1; mapeamos pixels para x,y de câmera nessa distância */
  sfr_view.fovx = 90.0f; sfr_view.fovy = 90.0f; sfr_view.x = 0; sfr_view.y = 0; sfr_view.w = SFR_W; sfr_view.h = SFR_H;
  sfr_view.org[0] = sfr_view.org[1] = sfr_view.org[2] = 0; { int a, b; for (a = 0; a < 3; a++) for (b = 0; b < 3; b++) sfr_view.axis[a][b] = (a == b); }
  /* axis: forward = X, left = Y, up = Z => câmera x = -Y(left), y = Z(up), z = X(forward) */
  { float X[4] = { x, x + w, x + w, x }, Y[4] = { y, y, y + h, y + h };
    float us[4] = { s1, s2, s2, s1 }, vs[4] = { t1, t1, t2, t2 };
    for (i = 0; i < 4; i++) {
      float cx = (X[i] - SFR_W * 0.5f) / (SFR_W * 0.5f), cy = -(Y[i] - SFR_H * 0.5f) / (SFR_H * 0.5f);   /* NDC com visão de 90 graus a z = 16 */
      pos[i][0] = 16.0f; pos[i][1] = -cx * 16.0f; pos[i][2] = cy * 16.0f;
      st[i][0] = us[i]; st[i][1] = vs[i];
      col[i][0] = (byte)(c[0] * 255); col[i][1] = (byte)(c[1] * 255); col[i][2] = (byte)(c[2] * 255); col[i][3] = (byte)(c[3] * 255);
    }
  }
  sfr_2d = 1;
  SFR_DrawShaderTris(sh ? sh : SFR_ShaderByHandle(0), 4, (const float (*)[3])pos, (const float (*)[2])st, NULL, (const byte (*)[4])col, 6, idx, NULL, NULL);
  sfr_2d = saved; sfr_view = sv;
}

/* ---------------- lista de comandos do GPU ---------------- */
static unsigned cmdn;
static void w(unsigned v) { if (cmdn < CMD_WORDS - 8) cmdbuf[cmdn++] = v; }
static void cmd(unsigned op, int np) { w((op << 24) | (unsigned)np); }
static unsigned f2u(float f) { union { float f; unsigned u; } c; c.f = f; return c.u; }
static void settex(int unit, unsigned addr, int lw, int lh, int fmt, int fl, int nl) {
  cmd(GC_SET_TEX, 4); w((unsigned)unit); w(addr); w((unsigned)lw | ((unsigned)lh << 8) | ((unsigned)fmt << 16) | ((unsigned)((fl & TF_CLAMP ? 1 : 0) | (fl & TF_MIPS ? 2 : 0)) << 24)); w((unsigned)nl);
  cmd(GC_TEXENV, 2); w((unsigned)unit); w(TEV_MODULATE);
}
void SFR_SetClear(unsigned argb) { frame_clear = argb; have_3d = 1; }

void SFR_EndFrame(void) {
  int i; unsigned rt, last_flags = 0xFFFFFFFFu, last_a0 = 0xFFFFFFFFu, last_a1 = 0xFFFFFFFFu; int last_src = -1, last_dst = -1; unsigned st;
  rt = rt_cur ? SFR_RT_ADDR1 : SFR_RT_ADDR0; cmdn = 0;
  cmd(GC_SET_RT, 3); w(rt); w(SFR_W * 4); w(SFR_W | (SFR_H << 16));
  cmd(GC_CLEAR, 3); w(frame_clear); w(f2u(1.0f)); w(3);
  cmd(GC_VERTEX_BASE, 1); w((unsigned)vbuf);
  for (i = 0; i < npackets; i++) {
    sfpacket_t *p = &packets[i];
    if (p->count <= 0) continue;
    if (p->a0 != last_a0) { settex(0, p->a0, p->lw0, p->lh0, p->fmt0, p->fl0, p->nl0); last_a0 = p->a0; }
    if ((p->flags & ST_TEX1) && p->a1 != last_a1) { settex(1, p->a1, p->lw1, p->lh1, p->fmt1, p->fl1, p->nl1); last_a1 = p->a1; }
    if ((p->flags & ST_BLEND) && (p->src != last_src || p->dst != last_dst)) { cmd(GC_BLEND, 2); w((unsigned)p->src); w((unsigned)p->dst); last_src = p->src; last_dst = p->dst; }
    if ((unsigned)p->flags != last_flags) { cmd(GC_STATE, 1); w((unsigned)p->flags); last_flags = (unsigned)p->flags; }
    cmd(GC_DRAW_ARRAYS, 3); w((unsigned)p->first); w((unsigned)p->count); w(0);
  }
  cmd(GC_END, 0);
  BRG_REG(0x24) = (unsigned)vbuf; BRG_REG(0x28) = (unsigned)nverts * 48; BRG_REG(0x20) = 4;      /* descarrega vértices e comandos (D-cache não é coerente) */
  BRG_REG(0x24) = (unsigned)cmdbuf; BRG_REG(0x28) = cmdn * 4; BRG_REG(0x20) = 4;
  while (GPU_REG(0x0C) & 1) BRG_REG(0x0F4) = 50;
  GPU_REG(0x00) = (unsigned)cmdbuf; GPU_REG(0x04) = cmdn; GPU_REG(0x08) = 1;
  do { st = GPU_REG(0x0C); if (st & 1) BRG_REG(0x0F4) = 100; } while (st & 1);
  if (st & 4) ri.Printf(PRINT_WARNING, "SFR: o GPU rejeitou a lista de comandos\n");
  VID_REG(0x40 + 0x00) = rt;                                                    /* exibe este RT; o próximo quadro vai no outro */
  rt_cur ^= 1; sfr_frame_count++;
  (void)have_3d;
}
