#include "flamecore.h"
#include "texutil.h"

namespace sf {
using namespace abi;

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

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
  t.inv = 1.0 / (double)area;
  for (int k = 0; k < 3; k++) {
    t.col[k] = f4{v[k].r, v[k].g, v[k].b, v[k].a}; t.uv[k] = f4{v[k].u, v[k].v, v[k].u1, v[k].v1}; t.fog[k] = v[k].fog;
  }
  t.rhwv = f4{v[0].rhw, v[1].rhw, v[2].rhw, 0.0f};
  for (int u = 0; u < 2; u++) {
    const Tex& tx = s.tex[u]; TexU& q = t.tu[u]; q = TexU();
    if (!tx.valid) continue;
    int level = tx.nlevels > 1 ? clampi((int)(t.lod[u] + 0.5f), 0, tx.nlevels - 1) : 0;
    q.w = std::max(1, (1 << tx.lw) >> level); q.h = std::max(1, (1 << tx.lh) >> level); q.wf = (float)q.w; q.hf = (float)q.h;
    q.base = tx.ptr + tx.off[level]; q.uvs = s.uvscale[u];
  }
  st.tris_drawn++;
  tris.push_back(t);
}

// ---------------- Rasterização ----------------
bool FlameCore::cover(const Tri& t, int px, int py, float& l0, float& l1, float& l2) const {
  i64 sx = (i64)px * 16 + 8, sy = (i64)py * 16 + 8;
  auto edge = [&](int a, int b) { return (i64)(t.x[b] - t.x[a]) * (sy - t.y[a]) - (i64)(t.y[b] - t.y[a]) * (sx - t.x[a]); };
  auto tl = [&](int a, int b) { i32 dx = t.x[b] - t.x[a], dy = t.y[b] - t.y[a]; return (dy == 0 && dx > 0) || dy < 0; };
  i64 e0 = edge(1, 2), e1 = edge(2, 0), e2 = edge(0, 1);
  if (e0 < 0 || (e0 == 0 && !tl(1, 2))) return false;
  if (e1 < 0 || (e1 == 0 && !tl(2, 0))) return false;
  if (e2 < 0 || (e2 == 0 && !tl(0, 1))) return false;
  l0 = (float)(e0 * t.inv); l1 = (float)(e1 * t.inv); l2 = (float)(e2 * t.inv);
  return true;
}

// Percorre os pixels cobertos de [x0,x1]x[y0,y1] chamando fn(px, py, l0, l1, l2). Funções de aresta avançadas por incremento ao
// longo da linha (inteiras e exatas: o mesmo resultado de cover()); o triângulo é convexo, então a linha termina ao sair dele.
template <class F> void FlameCore::raster(const Tri& t, int x0, int x1, int y0, int y1, F&& fn) {
  const int ea[3] = {1, 2, 0}, eb[3] = {2, 0, 1}; bool tlf[3]; i64 step[3], ddx[3], ddy[3];
  for (int k = 0; k < 3; k++) {
    i32 dx = t.x[eb[k]] - t.x[ea[k]], dy = t.y[eb[k]] - t.y[ea[k]];
    tlf[k] = (dy == 0 && dx > 0) || dy < 0; step[k] = -(i64)dy * 16; ddx[k] = dx; ddy[k] = dy;
  }
  const double inv = t.inv;
  for (int py = y0; py <= y1; py++) {
    i64 e[3], sy = (i64)py * 16 + 8, sx = (i64)x0 * 16 + 8; bool was_in = false;
    for (int k = 0; k < 3; k++) e[k] = ddx[k] * (sy - t.y[ea[k]]) - ddy[k] * (sx - t.x[ea[k]]);
    for (int px = x0; px <= x1; px++, e[0] += step[0], e[1] += step[1], e[2] += step[2]) {
      bool in = (e[0] > 0 || (e[0] == 0 && tlf[0])) && (e[1] > 0 || (e[1] == 0 && tlf[1])) && (e[2] > 0 || (e[2] == 0 && tlf[2]));
      if (!in) { if (was_in) break; continue; }
      was_in = true;
      fn(px, py, (float)(e[0] * inv), (float)(e[1] * inv), (float)(e[2] * inv));
    }
  }
}

// u8 -> float exatamente como (v / 255.0f), por tabela
struct U8F { float f[256]; U8F() { for (int i = 0; i < 256; i++) f[i] = i / 255.0f; } };
static const U8F u8f;

static inline f4 unpack4(u32 c) { return f4{u8f.f[(c >> 16) & 255], u8f.f[(c >> 8) & 255], u8f.f[c & 255], u8f.f[c >> 24]}; }
static inline int ifloor(float f) { int i = (int)f; return (float)i > f ? i - 1 : i; }

u32 FlameCore::texel(const Tex& t, const TexU& q, const u32* pal, int x, int y) {
  const int w = q.w, h = q.h;
  if (t.flags & TFLAG_CLAMP) { x = clampi(x, 0, w - 1); y = clampi(y, 0, h - 1); } else { x &= (w - 1); y &= (h - 1); }
  const u8* base = q.base;
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

f4 FlameCore::sample(TileCtx& c, const RState& s, const TexU& q, int unit, float u, float v) const {
  const Tex& t = s.tex[unit];
  if (!t.valid) return f4{1.0f, 1.0f, 1.0f, 1.0f};
  float fu = u * q.uvs * q.wf, fv = v * q.uvs * q.hf;
  const u32* pal = s.pal[unit];
  if (s.flags & ST_BILINEAR) {
    fu -= 0.5f; fv -= 0.5f; int x0 = ifloor(fu), y0 = ifloor(fv); float tx = fu - x0, ty = fv - y0;
    f4 c00 = unpack4(texel(t, q, pal, x0, y0)), c10 = unpack4(texel(t, q, pal, x0 + 1, y0));
    f4 c01 = unpack4(texel(t, q, pal, x0, y0 + 1)), c11 = unpack4(texel(t, q, pal, x0 + 1, y0 + 1));
    c.st.texfetch += 4;
    return (c00 * (1 - tx) + c10 * tx) * (1 - ty) + (c01 * (1 - tx) + c11 * tx) * ty;
  }
  c.st.texfetch += 1;
  return unpack4(texel(t, q, pal, ifloor(fu), ifloor(fv)));
}

// to8 dos quatro canais (r,g,b,a) de uma vez: mesma sequência de operações escalares (grampo, *255, +0,5, truncamento) em cada lane
typedef int i4 __attribute__((vector_size(16)));
static inline u32 pack8(f4 col) {
  const f4 zero = {0, 0, 0, 0}, one = {1, 1, 1, 1};
  f4 t = col < zero ? zero : col; t = t > one ? one : t;
  i4 q = __builtin_convertvector(t * 255.0f + 0.5f, i4);
  return ((u32)q[3] & 255) << 24 | ((u32)q[0] & 255) << 16 | ((u32)q[1] & 255) << 8 | ((u32)q[2] & 255);
}

u32 FlameCore::shade(TileCtx& c, const Tri& t, const RState& s, float l0, float l1, float l2) const {
  const f4 pr = f4{l0, l1, l2, 0.0f} * t.rhwv;                 // l_k * rhw_k
  const float rhw = pr[0] + pr[1] + pr[2];
  const f4 P = pr / rhw;                                       // (l_k * rhw_k) / rhw
  f4 col = P[0] * t.col[0] + P[1] * t.col[1] + P[2] * t.col[2];
  const u32 f = s.flags;
  if (f & (ST_TEX0 | ST_TEX1)) {
    const f4 T = P[0] * t.uv[0] + P[1] * t.uv[1] + P[2] * t.uv[2];   // u, v, u1, v1
    for (int unit = 0; unit < 2; unit++) {
      if (!(f & (unit ? ST_TEX1 : ST_TEX0))) continue;
      f4 tx = (unit == 1 && (f & ST_UV1)) ? sample(c, s, t.tu[unit], unit, T[2], T[3]) : sample(c, s, t.tu[unit], unit, T[0], T[1]);
      switch (s.texenv[unit]) {
        case TE_REPLACE: col = tx; break;
        case TE_DECAL: { f4 d = tx * tx[3] + col * (1 - tx[3]); col[0] = d[0]; col[1] = d[1]; col[2] = d[2]; break; }
        case TE_ADD: { float a = col[3] * tx[3]; col += tx; col[3] = a; break; }
        default: col *= tx; break;
      }
    }
  }
  if ((f & ST_FOG) && !(f & ST_UV1)) {
    float fogc = P[0] * t.fog[0] + P[1] * t.fog[1] + P[2] * t.fog[2];
    float den = s.fog_end - s.fog_start; float fg = den != 0 ? clampf((fogc - s.fog_start) / den, 0, 1) : 1.0f;
    const f4 fc = {u8f.f[(s.fog_color >> 16) & 255], u8f.f[(s.fog_color >> 8) & 255], u8f.f[s.fog_color & 255], 0.0f};
    f4 m = col * (1 - fg) + fc * fg;
    col[0] = m[0]; col[1] = m[1]; col[2] = m[2];
  }
  return pack8(col);
}

static inline u32 blend_px(u32 s, u32 d, u32 sf, u32 df, u32 saf, u32 daf) {
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

void FlameCore::imm_tri(TileCtx& c, const Tri& t) const {
  const RState& s = states[t.state]; const u32 f = s.flags, df = (f >> ST_DEPTH_FUNC_SHIFT) & 7;
  const bool stencil = f & ST_STENCIL, dtest = f & ST_DEPTH_TEST, dwrite = dtest && (f & ST_DEPTH_WRITE), blend = f & ST_BLEND, atest = f & ST_ATEST;
  const float z0 = t.v[0].z, z1 = t.v[1].z, z2 = t.v[2].z;
  const int x0 = std::max(t.minx, c.tox), x1 = std::min(t.maxx, c.tox + TILE - 1), y0 = std::max(t.miny, c.toy), y1 = std::min(t.maxy, c.toy + TILE - 1);
  u64 covered = 0, shaded = 0;
  raster(t, x0, x1, y0, y1, [&](int px, int py, float l0, float l1, float l2) {
    covered++;
    int i = (py - c.toy) * TILE + (px - c.tox);
    float z = l0 * z0 + l1 * z1 + l2 * z2; if (z < 0 || z > 1) return;
    if (stencil) {
      if (!sfunc(s.sfunc, s.sref & s.smask, c.tst[i] & s.smask)) { stencil_op(c.tst[i], s.sfail, s.sref); return; }
    }
    bool dp = !dtest || dfunc(df, z, c.tz[i]);
    if (!dp) { if (stencil) stencil_op(c.tst[i], s.zfail, s.sref); return; }
    u32 src = shade(c, t, s, l0, l1, l2); shaded++;
    if (atest && (src >> 24) < 128) return;                      // alpha test: descartado antes de Z/stencil/cor
    if (stencil) stencil_op(c.tst[i], s.zpass, s.sref);
    if (dwrite) c.tz[i] = z;
    c.tcol[i] = blend ? blend_px(src, c.tcol[i], s.bsrc, s.bdst, s.basrc, s.badst) : src;
  });
  c.st.pix_covered += covered; c.st.pix_shaded += shaded;
}

void FlameCore::render_tile(TileCtx& c, int tx, int ty, const std::vector<u32>& bin) const {
  const int tox = c.tox = tx * TILE, toy = c.toy = ty * TILE;
  u8* fb = bus.raw(rt.addr, rt.pitch * (u32)rt.h);            // validado em GC_SET_RT
  for (int y = 0; y < TILE; y++) {
    const int py = toy + y; u32* row = c.tcol + y * TILE;
    if (clr.flags & CLR_COLOR) { for (int x = 0; x < TILE; x++) row[x] = clr.argb; }
    else {
      const int n = py < rt.h ? clampi(rt.w - tox, 0, TILE) : 0; const u8* src = fb + (size_t)py * rt.pitch + (size_t)tox * 4;
      for (int x = 0; x < n; x++) row[x] = rbe32(src + x * 4);
      for (int x = n; x < TILE; x++) row[x] = 0;
    }
  }
  for (int i = 0; i < TILE * TILE; i++) c.tz[i] = clr.depth;
  std::memset(c.tst, 0, sizeof c.tst);
  size_t i = 0;
  while (i < bin.size()) {
    if (hsr && eligible(tris[bin[i]])) {
      size_t j = i; while (j < bin.size() && eligible(tris[bin[j]])) j++;
      std::memset(c.tvis, 0xFF, sizeof c.tvis);
      for (size_t k = i; k < j; k++) {
        const Tri& t = tris[bin[k]]; u32 df = (states[t.state].flags >> ST_DEPTH_FUNC_SHIFT) & 7;
        const float z0 = t.v[0].z, z1 = t.v[1].z, z2 = t.v[2].z; const int bk = (int)bin[k];
        int x0 = std::max(t.minx, tox), x1 = std::min(t.maxx, tox + TILE - 1), y0 = std::max(t.miny, toy), y1 = std::min(t.maxy, toy + TILE - 1);
        u64 covered = 0;
        raster(t, x0, x1, y0, y1, [&](int px, int py, float l0, float l1, float l2) {
          covered++;
          int q = (py - toy) * TILE + (px - tox);
          float z = l0 * z0 + l1 * z1 + l2 * z2; if (z < 0 || z > 1) return;
          if (dfunc(df, z, c.tz[q])) { c.tz[q] = z; c.tvis[q] = bk; }
        });
        c.st.pix_covered += covered;
      }
      for (int q = 0; q < TILE * TILE; q++) if (c.tvis[q] >= 0) {     // sombreia apenas o pixel visível, uma vez
        const Tri& t = tris[c.tvis[q]]; float l0 = 0, l1 = 0, l2 = 0;
        cover(t, tox + (q % TILE), toy + (q / TILE), l0, l1, l2);
        c.tcol[q] = shade(c, t, states[t.state], l0, l1, l2); c.st.pix_shaded++;
      }
      i = j;
    } else { imm_tri(c, tris[bin[i]]); i++; }
  }
  for (int y = 0; y < TILE; y++) {
    const int py = toy + y;
    if (py >= rt.h) break;
    const int n = clampi(rt.w - tox, 0, TILE); u8* dst = fb + (size_t)py * rt.pitch + (size_t)tox * 4; const u32* row = c.tcol + y * TILE;
    for (int x = 0; x < n; x++) wbe32(dst + x * 4, row[x]);
    c.st.vram_write += 4 * (u64)n;
  }
  c.st.tiles++;
}

// Alguma textura usada nesta lista lê a memória do render target? (render-to-texture no mesmo FLUSH: a ordem dos blocos
// importa, então a renderização fica serial)
bool FlameCore::rt_is_texture() const {
  u64 r0 = rt.addr, r1 = r0 + (u64)rt.pitch * (u32)rt.h;
  for (const RState& s : states) for (const Tex& t : s.tex) {
    if (!t.valid) continue;
    u64 sz = 0; for (int l = 0; l < t.nlevels; l++) sz = t.off[l] + tex_level_bytes(t.fmt, std::max(1, (1 << t.lw) >> l), std::max(1, (1 << t.lh) >> l));
    if (t.addr < r1 && r0 < t.addr + sz) return true;
  }
  return false;
}

FlameCore::FlameCore(Bus& b) : bus(b) {}
FlameCore::~FlameCore() {}

static void add_stats(FlameCore::Stats& a, const FlameCore::Stats& b) {
  a.pix_covered += b.pix_covered; a.pix_shaded += b.pix_shaded; a.tiles += b.tiles; a.texfetch += b.texfetch; a.vram_write += b.vram_write;
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
  std::vector<int> work;                                       // blocos a renderizar (intactos: nenhuma escrita na VRAM)
  size_t load = 0;
  for (int k = 0; k < tw * th; k++) if (!bins[k].empty() || (clr.flags & CLR_COLOR)) { work.push_back(k); load += bins[k].size(); }
  // Blocos em paralelo no host (cada bloco só lê a lista e escreve a própria área do render target; estatísticas por thread
  // somadas no fim): idêntico ao serial. Render-to-texture no mesmo FLUSH depende da ordem e fica serial.
  int nthr = host_threads > 0 ? host_threads : HostPool::auto_threads();
  if (!(work.size() >= 4 && load >= 64 && !rt_is_texture())) nthr = 1;
  if (nthr > 1 && (!pool || pool->size() != nthr)) pool.reset(new HostPool(nthr));
  int nctx = nthr > 1 ? nthr : 1;
  while ((int)ctx.size() < nctx) ctx.emplace_back(new TileCtx);
  for (int k = 0; k < nctx; k++) ctx[k]->st = Stats();
  auto job = [&](int w, int thr) { render_tile(*ctx[thr], work[w] % tw, work[w] / tw, bins[work[w]]); };
  if (nthr > 1) pool->run((int)work.size(), job); else for (int w = 0; w < (int)work.size(); w++) job(w, 0);
  for (int k = 0; k < nctx; k++) add_stats(st, ctx[k]->st);
  tris.clear(); states.clear(); state_dirty = true; clr.flags = 0;
}

}  // namespace sf
