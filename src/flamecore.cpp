#include "flamecore.h"
#include "texutil.h"

namespace sf {
using namespace abi;

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline u8 to8(float x) { return (u8)(clampf(x, 0.0f, 1.0f) * 255.0f + 0.5f); }

static bool dfunc(u32 f, float z, float cur) {
  switch (f) { case 0: return z < cur; case 1: return z <= cur; case 2: return z > cur; case 3: return z >= cur;
               case 4: return z == cur; case 5: return z != cur; case 6: return true; default: return false; }
}
static bool sfunc(u32 f, u32 ref, u32 val) {
  switch (f) { case 0: return ref < val; case 1: return ref <= val; case 2: return ref > val; case 3: return ref >= val;
               case 4: return ref == val; case 5: return ref != val; case 6: return true; default: return false; }
}

// ---------------- Fila / registradores ----------------
bool FlameCore::submit(u32 addr, u32 n, bool irq) {
  if ((int)queue.size() + (running ? 1 : 0) >= GPU_QUEUE_DEPTH) return false;
  queue.push_back({addr, n, irq});
  if (!running) kick();
  return true;
}
void FlameCore::kick() {
  if (queue.empty()) { running = false; return; }
  Job j = queue.front(); queue.pop_front(); running = true;
  Stats s = execute(j.addr, j.n);
  bool irq = j.irq;
  Ps dur = cyc2ps(std::max<u64>(s.cycles, 1), GPU_HZ);
  { Ps fin = bus.reserve(VRAM_BASE, s.vram_write + s.texfetch, bus.sched->now); dur = std::max(dur, fin - bus.sched->now); }
  bus.sched->at(dur, [this, irq]() {
    running = false; lists_done++;
    if (irq) bus.raise_irq(IRQ_GPU);
    kick();
  });
}

u32 FlameCore::read(u32 off) {
  switch (off) {
    case GPU_CMD_ADDR: return reg_addr; case GPU_CMD_LEN: return reg_len;
    case GPU_STATUS: return (running ? GPUS_BUSY : 0u) | (lists_done && !running && queue.empty() ? GPUS_DONE : 0u) | (error ? GPUS_ERROR : 0u);
    case GPU_HSR: return hsr ? 1 : 0;
    case GPU_STAT_TRIS_IN: return (u32)last.tris_in; case GPU_STAT_TRIS_DRAWN: return (u32)last.tris_drawn;
    case GPU_STAT_PIX_COVERED: return (u32)last.pix_covered; case GPU_STAT_PIX_SHADED: return (u32)last.pix_shaded;
    case GPU_STAT_TILES: return (u32)last.tiles; case GPU_STAT_CYCLES: return (u32)last.cycles;
    case GPU_STAT_TEXFETCH: return (u32)last.texfetch; case GPU_STAT_VRAM_WRITE: return (u32)last.vram_write;
  }
  return 0;
}
void FlameCore::write(u32 off, u32 v) {
  switch (off) {
    case GPU_CMD_ADDR: reg_addr = v; break; case GPU_CMD_LEN: reg_len = v; break;
    case GPU_HSR: hsr = v & 1; break;
    case GPU_STATUS: if (v & GPUS_ERROR) error = false; break;
    case GPU_CTRL: if (v & 1) { if (!submit(reg_addr, reg_len, v & 2)) { error = true; error_msg = "fila cheia"; } } break;
  }
}

// ---------------- Execução da lista ----------------
FlameCore::Stats FlameCore::execute(u32 addr, u32 nwords) {
  cur = RState(); state_dirty = true; states.clear(); tris.clear(); st = Stats(); rt = Fb(); clr = ClearSt();
  vbase = 0; scx = scy = 0; scw = sch = 65535; error = false; error_msg.clear();
  auto fail = [&](const std::string& m) { error = true; error_msg = m; };
  u32 p = addr, end = addr + nwords * 4;
  if (!bus.raw_ok(addr, nwords * 4)) { fail("lista de comandos fora da memória"); last = st; return st; }
  while (p < end && !error) {
    u32 w = bus.raw_r32(p); u32 op = w >> 24, np = w & 0xFFFFFF;
    if (p + 4 + np * 4 > end) { fail("comando truncado"); break; }
    const u32 pb = p;
    auto P = [&, pb](u32 i) { return bus.raw_r32(pb + 4 + i * 4); };
    p += 4 + np * 4;
    switch (op) {
      case GC_END: flush(); p = end; break;
      case GC_SET_RT: {
        if (np < 3) { fail("SET_RT"); break; }
        rt.addr = P(0); rt.pitch = P(1); rt.w = P(2) & 0xFFFF; rt.h = P(2) >> 16;
        if (!bus.raw_ok(rt.addr, rt.pitch * (u32)rt.h) || rt.pitch < (u32)rt.w * 4) fail("render target inválido");
        break;
      }
      case GC_CLEAR: if (np < 3) { fail("CLEAR"); break; } clr.argb = P(0); clr.depth = u2f(P(1)); clr.flags = P(2); break;
      case GC_VERTEX_BASE: if (np < 1) { fail("VERTEX_BASE"); break; } vbase = P(0); break;
      case GC_SET_TEX: {
        if (np < 4) { fail("SET_TEX"); break; }
        u32 unit = P(0); if (unit > 1) { fail("unidade de textura"); break; }
        Tex t; t.addr = P(1); u32 pk = P(2); t.lw = pk & 0xFF; t.lh = (pk >> 8) & 0xFF; t.fmt = (pk >> 16) & 0xFF; t.flags = pk >> 24;
        t.nlevels = (t.flags & TFLAG_MIPS) ? clampi((int)P(3), 1, 12) : 1;
        if (t.lw > 11 || t.lh > 11 || t.fmt >= (int)TF_COUNT) { fail("textura inválida"); break; }
        u32 o = 0;
        for (int l = 0; l < t.nlevels; l++) { t.off[l] = o; o += tex_level_bytes(t.fmt, std::max(1, (1 << t.lw) >> l), std::max(1, (1 << t.lh) >> l)); }
        t.ptr = bus.raw(t.addr, o); if (!t.ptr) { fail("textura fora da memória"); break; }
        t.valid = true; cur.tex[unit] = t; state_dirty = true; break;
      }
      case GC_SET_PALETTE: {
        if (np < 3 || P(0) > 1 || P(2) > 256) { fail("SET_PALETTE"); break; }
        u32 unit = P(0), pa = P(1), n = P(2);
        if (!bus.raw_ok(pa, n * 4)) { fail("paleta fora da memória"); break; }
        for (u32 i = 0; i < n; i++) cur.pal[unit][i] = bus.raw_r32(pa + i * 4);
        state_dirty = true; break;
      }
      case GC_STATE: if (np < 1) { fail("STATE"); break; } cur.flags = P(0); state_dirty = true; break;
      case GC_BLEND: if (np < 2) { fail("BLEND"); break; } cur.bsrc = P(0); cur.bdst = P(1);
        cur.basrc = np >= 4 ? P(2) : P(0); cur.badst = np >= 4 ? P(3) : P(1); state_dirty = true; break;   // 4 params: fatores próprios do alfa
      case GC_FOG: if (np < 3) { fail("FOG"); break; } cur.fog_color = P(0); cur.fog_start = u2f(P(1)); cur.fog_end = u2f(P(2)); state_dirty = true; break;
      case GC_STENCIL: if (np < 6) { fail("STENCIL"); break; } cur.sfunc = P(0); cur.sref = P(1); cur.smask = P(2); cur.sfail = P(3); cur.zfail = P(4); cur.zpass = P(5); state_dirty = true; break;
      case GC_TEXENV: if (np < 2 || P(0) > 1) { fail("TEXENV"); break; } cur.texenv[P(0)] = P(1); cur.uvscale[P(0)] = np >= 3 ? u2f(P(2)) : 1.0f; state_dirty = true; break;
      case GC_SCISSOR: if (np < 4) { fail("SCISSOR"); break; } scx = P(0); scy = P(1); scw = P(2); sch = P(3); break;
      case GC_DRAW_INDEXED: {
        if (np < 3) { fail("DRAW_INDEXED"); break; }
        u32 ia = P(0), cnt = P(1), prim = P(2);
        if (!bus.raw_ok(ia, cnt * 2)) { fail("índices fora da memória"); break; }
        draw(prim, cnt, [&](u32 i) { return (u32)bus.raw_r16(ia + i * 2); });
        break;
      }
      case GC_DRAW_ARRAYS: {
        if (np < 3) { fail("DRAW_ARRAYS"); break; }
        u32 first = P(0), cnt = P(1), prim = P(2);
        draw(prim, cnt, [&](u32 i) { return first + i; });
        break;
      }
      case GC_FLUSH: flush(); break;
      default: fail("comando desconhecido " + std::to_string(op));
    }
  }
  if (!error) flush();
  u64 compute = st.tris_drawn * 12 + st.pix_shaded / 2 + st.pix_covered / 8 + st.tiles * 40;
  compute = std::max(compute, st.texfetch / 4);   // 4 TMUs (2 por pipeline): 1 texel por ciclo cada
  u64 membytes = st.vram_write + st.texfetch * 4 / 4;
  u64 mem = membytes * 1000 / 26560;            // 5,31 GB/s a 200 MHz ~ 26,56 B/ciclo
  st.cycles = std::max(compute, mem);
  last = st;
  total.tris_in += st.tris_in; total.tris_drawn += st.tris_drawn; total.pix_covered += st.pix_covered;
  total.pix_shaded += st.pix_shaded; total.tiles += st.tiles; total.cycles += st.cycles; total.texfetch += st.texfetch;
  total.vram_write += st.vram_write; total.bin_entries += st.bin_entries;
  return st;
}

int FlameCore::cur_state() {
  if (state_dirty) { states.push_back(cur); state_dirty = false; }
  return (int)states.size() - 1;
}

FlameCore::Vtx FlameCore::load_vertex(u32 idx) {
  Vtx v{}; u32 a = vbase + idx * VERTEX_SIZE;
  const u8* p = bus.raw(a, VERTEX_SIZE);
  if (!p) { error = true; error_msg = "vértice fora da memória"; return v; }
  v.x = rbef(p); v.y = rbef(p + 4); v.z = rbef(p + 8); v.rhw = rbef(p + 12);
  v.r = rbef(p + 16); v.g = rbef(p + 20); v.b = rbef(p + 24); v.a = rbef(p + 28);
  v.u = rbef(p + 32); v.v = rbef(p + 36); v.fog = rbef(p + 40); v.u1 = v.fog; v.v1 = rbef(p + 44);
  return v;
}

void FlameCore::draw(u32 prim, u32 count, const std::function<u32(u32)>& index) {
  if (prim == PRIM_TRIS) {
    for (u32 i = 0; i + 2 < count && !error; i += 3) emit(load_vertex(index(i)), load_vertex(index(i + 1)), load_vertex(index(i + 2)));
  } else if (prim == PRIM_STRIP) {
    for (u32 i = 0; i + 2 < count && !error; i++) {
      if (i & 1) emit(load_vertex(index(i + 1)), load_vertex(index(i)), load_vertex(index(i + 2)));
      else emit(load_vertex(index(i)), load_vertex(index(i + 1)), load_vertex(index(i + 2)));
    }
  } else if (prim == PRIM_FAN) {
    for (u32 i = 1; i + 1 < count && !error; i++) emit(load_vertex(index(0)), load_vertex(index(i)), load_vertex(index(i + 1)));
  } else { error = true; error_msg = "primitiva inválida"; }
}

void FlameCore::emit(const Vtx& a, const Vtx& b, const Vtx& c) {
  st.tris_in++;
  Vtx v[3] = {a, b, c};
  for (auto& q : v) {
    if (!(q.rhw > 0) || !std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) return;   // atrás da câmera / inválido
    if (std::fabs(q.x) > 8000.0f || std::fabs(q.y) > 8000.0f) return;                                // guard-band
  }
  int si = cur_state(); const RState& s = states[si];
  i32 X[3], Y[3];
  for (int i = 0; i < 3; i++) { X[i] = (i32)std::lround(v[i].x * 16.0f); Y[i] = (i32)std::lround(v[i].y * 16.0f); }
  i64 area = (i64)(X[1] - X[0]) * (Y[2] - Y[0]) - (i64)(Y[1] - Y[0]) * (X[2] - X[0]);
  if (area == 0) return;
  if (area > 0 && (s.flags & ST_CULL_CW)) return;
  if (area < 0) {
    if (s.flags & ST_CULL_CCW) return;
    std::swap(v[1], v[2]); std::swap(X[1], X[2]); std::swap(Y[1], Y[2]); area = -area;
  }
  Tri t; for (int i = 0; i < 3; i++) { t.v[i] = v[i]; t.x[i] = X[i]; t.y[i] = Y[i]; }
  t.state = si; t.area = area;
  int mnx = std::min({X[0], X[1], X[2]}), mxx = std::max({X[0], X[1], X[2]}), mny = std::min({Y[0], Y[1], Y[2]}), mxy = std::max({Y[0], Y[1], Y[2]});
  t.minx = std::max(mnx >> 4, std::max(0, scx)); t.maxx = std::min((mxx + 15) >> 4, std::min(rt.w - 1, scx + scw - 1));
  t.miny = std::max(mny >> 4, std::max(0, scy)); t.maxy = std::min((mxy + 15) >> 4, std::min(rt.h - 1, scy + sch - 1));
  if (t.minx > t.maxx || t.miny > t.maxy) return;
  // LOD por triângulo (nível de mip mais próximo)
  for (int u = 0; u < 2; u++) {
    t.lod[u] = 0;
    const Tex& tx = s.tex[u];
    if (tx.valid && tx.nlevels > 1) {
      float sc = s.uvscale[u];
      float uvpar = std::fabs((v[1].u - v[0].u) * (v[2].v - v[0].v) - (v[2].u - v[0].u) * (v[1].v - v[0].v)) * sc * sc * (float)(1 << tx.lw) * (float)(1 << tx.lh);
      float pixpar = (float)area / 256.0f;
      float lod = uvpar > 0 ? 0.5f * std::log2(uvpar / pixpar) : 0.0f;
      t.lod[u] = clampf(lod, 0.0f, (float)(tx.nlevels - 1));
    }
  }
  st.tris_drawn++;
  tris.push_back(t);
}

// ---------------- Rasterização ----------------
bool FlameCore::cover(const Tri& t, int px, int py, float& l0, float& l1, float& l2) {
  i64 sx = (i64)px * 16 + 8, sy = (i64)py * 16 + 8;
  auto edge = [&](int a, int b) { return (i64)(t.x[b] - t.x[a]) * (sy - t.y[a]) - (i64)(t.y[b] - t.y[a]) * (sx - t.x[a]); };
  auto tl = [&](int a, int b) { i32 dx = t.x[b] - t.x[a], dy = t.y[b] - t.y[a]; return (dy == 0 && dx > 0) || dy < 0; };
  i64 e0 = edge(1, 2), e1 = edge(2, 0), e2 = edge(0, 1);
  if (e0 < 0 || (e0 == 0 && !tl(1, 2))) return false;
  if (e1 < 0 || (e1 == 0 && !tl(2, 0))) return false;
  if (e2 < 0 || (e2 == 0 && !tl(0, 1))) return false;
  double inv = 1.0 / (double)t.area;
  l0 = (float)(e0 * inv); l1 = (float)(e1 * inv); l2 = (float)(e2 * inv);
  return true;
}

u32 FlameCore::texel(const Tex& t, const u32* pal, int level, int x, int y) {
  int w = std::max(1, (1 << t.lw) >> level), h = std::max(1, (1 << t.lh) >> level);
  if (t.flags & TFLAG_CLAMP) { x = clampi(x, 0, w - 1); y = clampi(y, 0, h - 1); } else { x &= (w - 1); y &= (h - 1); }
  const u8* base = t.ptr + t.off[level];
  switch (t.fmt) {
    case TF_ARGB8888: return rbe32(base + ((size_t)y * w + x) * 4);
    case TF_RGB565: { u16 c = rbe16(base + ((size_t)y * w + x) * 2); int r, g, b; from565(c, r, g, b); return pack_argb(255, r, g, b); }
    case TF_ARGB4444: { u16 c = rbe16(base + ((size_t)y * w + x) * 2); return pack_argb(((c >> 12) & 15) * 17, ((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17); }
    case TF_RGBA5551: { u16 c = rbe16(base + ((size_t)y * w + x) * 2); int r = (c >> 11) & 31, g = (c >> 6) & 31, b = (c >> 1) & 31;
      return pack_argb((c & 1) * 255, (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)); }
    case TF_L8: { int l = base[(size_t)y * w + x]; return pack_argb(255, l, l, l); }
    case TF_A8: return pack_argb(base[(size_t)y * w + x], 255, 255, 255);
    case TF_LA88: { u16 c = rbe16(base + ((size_t)y * w + x) * 2); return pack_argb(c >> 8, c & 255, c & 255, c & 255); }
    case TF_CI8: return pal[base[(size_t)y * w + x]];
    case TF_CI4: { u8 b = base[(size_t)y * ((w + 1) / 2) + (x >> 1)]; return pal[(x & 1) ? (b & 15) : (b >> 4)]; }
    case TF_VQ: {
      int bw = std::max(1, (w + 1) / 2); int idx = base[2048 + (size_t)(y >> 1) * bw + (x >> 1)];
      u16 c = rbe16(base + idx * 8 + (((y & 1) * 2) + (x & 1)) * 2); int r, g, b; from565(c, r, g, b); return pack_argb(255, r, g, b);
    }
    case TF_ETC1: { int bw = std::max(1, (w + 3) / 4); return etc1_texel(base + ((size_t)(y >> 2) * bw + (x >> 2)) * 8, x & 3, y & 3); }
    default: {                                               // DXT1 (8 B/bloco), DXT3/DXT5 (16 B/bloco: alfa + cor)
      int bw = std::max(1, (w + 3) / 4); bool two = t.fmt != TF_DXT1;
      const u8* blk = base + ((size_t)(y >> 2) * bw + (x >> 2)) * (two ? 16 : 8);
      u32 alpha = 255;
      if (t.fmt == TF_DXT3) alpha = ((rbe16(blk + 2 * (y & 3)) >> (4 * (x & 3))) & 15) * 17;
      else if (t.fmt == TF_DXT5) {
        u32 a0 = blk[0], a1 = blk[1]; u64 bits = 0; for (int k = 0; k < 6; k++) bits = (bits << 8) | blk[2 + k];
        u32 idx = (u32)(bits >> (3 * ((y & 3) * 4 + (x & 3)))) & 7;
        if (idx == 0) alpha = a0; else if (idx == 1) alpha = a1;
        else if (a0 > a1) alpha = ((8 - idx) * a0 + (idx - 1) * a1) / 7;
        else if (idx < 6) alpha = ((6 - idx) * a0 + (idx - 1) * a1) / 5;
        else alpha = idx == 6 ? 0 : 255;
      }
      if (two) blk += 8;
      u16 c0 = rbe16(blk), c1 = rbe16(blk + 2); int code = (blk[4 + (y & 3)] >> (2 * (x & 3))) & 3;
      int r0, g0, b0, r1, g1, b1; from565(c0, r0, g0, b0); from565(c1, r1, g1, b1);
      if (c0 > c1 || two) {                                   // DXT3/5 sempre usam o modo de 4 cores
        switch (code) { case 0: return pack_argb(alpha, r0, g0, b0); case 1: return pack_argb(alpha, r1, g1, b1);
          case 2: return pack_argb(alpha, (2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3);
          default: return pack_argb(alpha, (r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3); }
      }
      switch (code) { case 0: return pack_argb(255, r0, g0, b0); case 1: return pack_argb(255, r1, g1, b1);
        case 2: return pack_argb(255, (r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2); default: return 0; }
    }
  }
}

void FlameCore::sample(const RState& s, int unit, float u, float v, float lod, float out[4]) {
  const Tex& t = s.tex[unit];
  if (!t.valid) { out[0] = out[1] = out[2] = out[3] = 1.0f; return; }
  int level = t.nlevels > 1 ? clampi((int)(lod + 0.5f), 0, t.nlevels - 1) : 0;
  int w = std::max(1, (1 << t.lw) >> level), h = std::max(1, (1 << t.lh) >> level);
  float fu = u * s.uvscale[unit] * (float)w, fv = v * s.uvscale[unit] * (float)h;
  auto unpack = [](u32 c, float* o) { o[3] = (c >> 24) / 255.0f; o[0] = ((c >> 16) & 255) / 255.0f; o[1] = ((c >> 8) & 255) / 255.0f; o[2] = (c & 255) / 255.0f; };
  if (s.flags & ST_BILINEAR) {
    fu -= 0.5f; fv -= 0.5f; int x0 = (int)std::floor(fu), y0 = (int)std::floor(fv); float tx = fu - x0, ty = fv - y0;
    float c00[4], c10[4], c01[4], c11[4];
    const u32* pal = s.pal[unit];
    unpack(texel(t, pal, level, x0, y0), c00); unpack(texel(t, pal, level, x0 + 1, y0), c10);
    unpack(texel(t, pal, level, x0, y0 + 1), c01); unpack(texel(t, pal, level, x0 + 1, y0 + 1), c11);
    for (int i = 0; i < 4; i++) out[i] = (c00[i] * (1 - tx) + c10[i] * tx) * (1 - ty) + (c01[i] * (1 - tx) + c11[i] * tx) * ty;
    st.texfetch += 4;
  } else {
    unpack(texel(t, s.pal[unit], level, (int)std::floor(fu), (int)std::floor(fv)), out); st.texfetch += 1;
  }
}

u32 FlameCore::shade(const Tri& t, float l0, float l1, float l2) {
  const RState& s = states[t.state];
  float rhw = l0 * t.v[0].rhw + l1 * t.v[1].rhw + l2 * t.v[2].rhw;
  float p0 = l0 * t.v[0].rhw / rhw, p1 = l1 * t.v[1].rhw / rhw, p2 = l2 * t.v[2].rhw / rhw;
  auto I = [&](float a, float b, float c) { return p0 * a + p1 * b + p2 * c; };
  float col[4] = {I(t.v[0].r, t.v[1].r, t.v[2].r), I(t.v[0].g, t.v[1].g, t.v[2].g), I(t.v[0].b, t.v[1].b, t.v[2].b), I(t.v[0].a, t.v[1].a, t.v[2].a)};
  float u = I(t.v[0].u, t.v[1].u, t.v[2].u), v = I(t.v[0].v, t.v[1].v, t.v[2].v), fogc = I(t.v[0].fog, t.v[1].fog, t.v[2].fog);
  float u1 = I(t.v[0].u1, t.v[1].u1, t.v[2].u1), v1 = I(t.v[0].v1, t.v[1].v1, t.v[2].v1);
  for (int unit = 0; unit < 2; unit++) {
    if (!(s.flags & (unit ? ST_TEX1 : ST_TEX0))) continue;
    float tx[4]; if (unit == 1 && (s.flags & ST_UV1)) sample(s, unit, u1, v1, t.lod[unit], tx); else sample(s, unit, u, v, t.lod[unit], tx);
    switch (s.texenv[unit]) {
      case TE_REPLACE: for (int i = 0; i < 4; i++) col[i] = tx[i]; break;
      case TE_DECAL: for (int i = 0; i < 3; i++) col[i] = tx[i] * tx[3] + col[i] * (1 - tx[3]); break;
      case TE_ADD: for (int i = 0; i < 3; i++) col[i] += tx[i]; col[3] *= tx[3]; break;
      default: for (int i = 0; i < 4; i++) col[i] *= tx[i]; break;
    }
  }
  if ((s.flags & ST_FOG) && !(s.flags & ST_UV1)) {
    float den = s.fog_end - s.fog_start; float f = den != 0 ? clampf((fogc - s.fog_start) / den, 0, 1) : 1.0f;
    float fc[3] = {((s.fog_color >> 16) & 255) / 255.0f, ((s.fog_color >> 8) & 255) / 255.0f, (s.fog_color & 255) / 255.0f};
    for (int i = 0; i < 3; i++) col[i] = col[i] * (1 - f) + fc[i] * f;
  }
  return pack_argb(to8(col[3]), to8(col[0]), to8(col[1]), to8(col[2]));
}

static u32 blend_px(u32 s, u32 d, u32 sf, u32 df, u32 saf, u32 daf) {
  int sa = s >> 24, da = d >> 24;
  auto fac = [&](u32 k) { switch (k) { case BF_ZERO: return 0; case BF_ONE: return 255; case BF_SRC_ALPHA: return sa; case BF_INV_SRC_ALPHA: return 255 - sa;
                                         case BF_DST_ALPHA: return da; default: return 255 - da; } };
  int fs = fac(sf), fd = fac(df); u32 out = 0;
  for (int sh = 0; sh < 32; sh += 8) {
    if (sh == 24) { fs = fac(saf); fd = fac(daf); }                          // canal alfa: fatores próprios (GC_BLEND com 4 parâmetros)
    int c = (int)(((s >> sh) & 255) * fs + ((d >> sh) & 255) * fd + 127) / 255; if (c > 255) c = 255;
    out |= (u32)c << sh;
  }
  return out;
}

bool FlameCore::eligible(const Tri& t) const {
  const RState& s = states[t.state]; u32 f = s.flags, df = (f >> ST_DEPTH_FUNC_SHIFT) & 7;
  return (f & ST_DEPTH_TEST) && (f & ST_DEPTH_WRITE) && !(f & ST_BLEND) && !(f & ST_STENCIL) && !(f & ST_ATEST) && (df == 0 || df == 1);
}

static void stencil_op(u8& sv, u32 op, u32 ref) {
  switch (op) { case SOP_ZERO: sv = 0; break; case SOP_REPLACE: sv = (u8)ref; break; case SOP_INCR: if (sv < 255) sv++; break;
                case SOP_DECR: if (sv > 0) sv--; break; case SOP_INVERT: sv = ~sv; break; default: break; }
}

void FlameCore::imm_tri(const Tri& t, int) {
  const RState& s = states[t.state]; u32 f = s.flags, df = (f >> ST_DEPTH_FUNC_SHIFT) & 7;
  int x0 = std::max(t.minx, tox), x1 = std::min(t.maxx, tox + TILE - 1), y0 = std::max(t.miny, toy), y1 = std::min(t.maxy, toy + TILE - 1);
  for (int py = y0; py <= y1; py++) for (int px = x0; px <= x1; px++) {
    float l0, l1, l2; if (!cover(t, px, py, l0, l1, l2)) continue;
    st.pix_covered++;
    int i = (py - toy) * TILE + (px - tox);
    float z = l0 * t.v[0].z + l1 * t.v[1].z + l2 * t.v[2].z; if (z < 0 || z > 1) continue;
    if (f & ST_STENCIL) {
      if (!sfunc(s.sfunc, s.sref & s.smask, tst[i] & s.smask)) { stencil_op(tst[i], s.sfail, s.sref); continue; }
    }
    bool dp = !(f & ST_DEPTH_TEST) || dfunc(df, z, tz[i]);
    if (!dp) { if (f & ST_STENCIL) stencil_op(tst[i], s.zfail, s.sref); continue; }
    u32 src = shade(t, l0, l1, l2); st.pix_shaded++;
    if ((f & ST_ATEST) && (src >> 24) < 128) continue;                      // alpha test: descartado antes de Z/stencil/cor
    if (f & ST_STENCIL) stencil_op(tst[i], s.zpass, s.sref);
    if ((f & ST_DEPTH_TEST) && (f & ST_DEPTH_WRITE)) tz[i] = z;
    tcol[i] = (f & ST_BLEND) ? blend_px(src, tcol[i], s.bsrc, s.bdst, s.basrc, s.badst) : src;
  }
}

void FlameCore::render_tile(int tx, int ty, const std::vector<u32>& bin) {
  tox = tx * TILE; toy = ty * TILE;
  for (int y = 0; y < TILE; y++) for (int x = 0; x < TILE; x++) {
    int i = y * TILE + x, px = tox + x, py = toy + y;
    if (clr.flags & CLR_COLOR) tcol[i] = clr.argb;
    else tcol[i] = (px < rt.w && py < rt.h) ? bus.raw_r32(rt.addr + py * rt.pitch + px * 4) : 0;
    tz[i] = clr.depth; tst[i] = 0;
  }
  size_t i = 0;
  while (i < bin.size()) {
    if (hsr && eligible(tris[bin[i]])) {
      size_t j = i; while (j < bin.size() && eligible(tris[bin[j]])) j++;
      for (int k = 0; k < TILE * TILE; k++) tvis[k] = -1;
      for (size_t k = i; k < j; k++) {
        const Tri& t = tris[bin[k]]; u32 df = (states[t.state].flags >> ST_DEPTH_FUNC_SHIFT) & 7;
        int x0 = std::max(t.minx, tox), x1 = std::min(t.maxx, tox + TILE - 1), y0 = std::max(t.miny, toy), y1 = std::min(t.maxy, toy + TILE - 1);
        for (int py = y0; py <= y1; py++) for (int px = x0; px <= x1; px++) {
          float l0, l1, l2; if (!cover(t, px, py, l0, l1, l2)) continue;
          st.pix_covered++;
          int q = (py - toy) * TILE + (px - tox);
          float z = l0 * t.v[0].z + l1 * t.v[1].z + l2 * t.v[2].z; if (z < 0 || z > 1) continue;
          if (dfunc(df, z, tz[q])) { tz[q] = z; tvis[q] = bin[k]; }
        }
      }
      for (int q = 0; q < TILE * TILE; q++) if (tvis[q] >= 0) {     // sombreia apenas o pixel visível, uma vez
        const Tri& t = tris[tvis[q]]; float l0, l1, l2;
        cover(t, tox + (q % TILE), toy + (q / TILE), l0, l1, l2);
        tcol[q] = shade(t, l0, l1, l2); st.pix_shaded++;
      }
      i = j;
    } else { imm_tri(tris[bin[i]], (int)i); i++; }
  }
  for (int y = 0; y < TILE; y++) for (int x = 0; x < TILE; x++) {
    int px = tox + x, py = toy + y;
    if (px < rt.w && py < rt.h) { bus.raw_w32(rt.addr + py * rt.pitch + px * 4, tcol[y * TILE + x]); st.vram_write += 4; }
  }
  st.tiles++;
}

void FlameCore::flush() {
  if (error) { tris.clear(); return; }
  if (rt.w == 0) { if (!tris.empty() || clr.flags) { error = true; error_msg = "FLUSH sem render target"; } tris.clear(); return; }
  int tw = (rt.w + TILE - 1) / TILE, th = (rt.h + TILE - 1) / TILE;
  std::vector<std::vector<u32>> bins((size_t)tw * th);
  for (size_t i = 0; i < tris.size(); i++) {
    const Tri& t = tris[i];
    for (int ty = t.miny / TILE; ty <= t.maxy / TILE; ty++) for (int tx = t.minx / TILE; tx <= t.maxx / TILE; tx++) { bins[(size_t)ty * tw + tx].push_back((u32)i); st.bin_entries++; }
  }
  for (int ty = 0; ty < th; ty++) for (int tx = 0; tx < tw; tx++) {
    auto& b = bins[(size_t)ty * tw + tx];
    if (b.empty() && !(clr.flags & CLR_COLOR)) continue;       // bloco intacto: nenhuma escrita na VRAM
    render_tile(tx, ty, b);
  }
  tris.clear(); states.clear(); state_dirty = true; clr.flags = 0;
}

}  // namespace sf
