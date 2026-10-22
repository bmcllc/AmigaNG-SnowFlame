#include "chipset.h"
#include <memory>

namespace sf {
using namespace abi;

// ---------------- Blitter ----------------
u32 Blitter::read(u32 off) {
  if (off == BLT_STATUS) return (running ? 1u : 0u) | (error ? 4u : 0u) | ((!running && jobs) ? 2u : 0u);
  if (off < 0x40) return r[off / 4 < 16 ? off / 4 : 0];
  return 0;
}
void Blitter::write(u32 off, u32 v) {
  if (off == BLT_STATUS) { if (v & 4) error = false; return; }
  if (off == BLT_CTRL) {
    if (v & 1) {
      Job j{r[BLT_OP / 4], r[BLT_SRC / 4], r[BLT_SRC_PITCH / 4], r[BLT_DST / 4], r[BLT_DST_PITCH / 4], r[BLT_W / 4], r[BLT_H / 4],
            r[BLT_COLOR / 4], r[BLT_MASK / 4], r[BLT_MASK_PITCH / 4], r[BLT_MINTERM / 4], (v & 2) != 0};
      if (!submit(j)) error = true;
    }
    return;
  }
  if (off < 0x40) r[off / 4 < 16 ? off / 4 : 0] = v;
}
bool Blitter::submit(const Job& j) {
  if (q.size() >= 8) return false;
  q.push_back(j); if (!running) kick(); return true;
}
void Blitter::kick() {
  if (q.empty()) { running = false; return; }
  Job j = q.front(); q.pop_front(); running = true;
  u64 cyc = execute(j);
  Ps dur = cyc2ps(std::max<u64>(cyc, 1), CHIP_HZ);
  if (cyc) {                                            // disputa de barramento (bytes por região, por tipo de operação)
    Ps now = bus.sched->now; u64 px = (u64)j.w * j.h;
    if (j.op == BOP_SPRITES) { dur = std::max(dur, spr_fin - now); goto scheduled; }
    bool reads_dst = j.op == BOP_ALPHA || j.op == BOP_COLORKEY || j.op == BOP_MINTERM;   // leitura-modificação-escrita do destino
    Ps fin = bus.reserve(j.dst, px * 4 * (reads_dst ? 2 : 1), now);
    if (j.op != BOP_FILL) fin = std::max(fin, bus.reserve(j.src, px * (j.op == BOP_CONV565 ? 2 : 4), now));
    if (j.op == BOP_MINTERM) fin = std::max(fin, bus.reserve(j.mask, px * 4, now));
    dur = std::max(dur, fin - now);
  }
scheduled:
  bus.sched->at(dur, [this, j]() {
    running = false; jobs++; if (j.irq) bus.raise_irq(IRQ_BLT); kick();
  });
}

static u32 alpha_over(u32 s, u32 d) {
  u32 sa = s >> 24; if (sa == 255) return s; if (sa == 0) return d;
  u32 da = d >> 24; u32 out = 0;
  for (int sh = 0; sh < 24; sh += 8) {
    u32 c = (((s >> sh) & 255) * sa + ((d >> sh) & 255) * (255 - sa) + 127) / 255; out |= c << sh;
  }
  u32 oa = sa + (da * (255 - sa) + 127) / 255; return out | (std::min<u32>(oa, 255) << 24);
}

u64 Blitter::exec_sprites(const Job& j) {
  const u32 n = j.color, W = j.w, H = j.h; spr_px = spr_n = 0; Ps now = bus.sched->now; spr_fin = now;
  if (n > SPRITE_MAX || !W || !H) { error = true; return 0; }
  if (n == 0) return 1;
  if (!bus.raw_ok(j.src, n * SPRITE_ENTRY_SIZE) || !bus.raw_ok(j.dst, (H - 1) * j.dpitch + W * 4)) { error = true; return 0; }
  u64 cyc = 0;
  for (u32 i = 0; i < n; i++) {
    const u8* e = bus.raw(j.src + i * SPRITE_ENTRY_SIZE, SPRITE_ENTRY_SIZE);
    u32 sa = rbe32(e), sp = rbe16(e + 4), fl = rbe16(e + 6); int x0 = (i16)rbe16(e + 8), y0 = (i16)rbe16(e + 10); u32 w = rbe16(e + 12), h = rbe16(e + 14);
    cyc += 8; if (!w || !h) continue;
    if (!bus.raw_ok(sa, (h - 1) * sp + w * 4)) { error = true; return 0; }
    int xa = std::max(x0, 0), xb = std::min<int>(x0 + (int)w, (int)W), ya = std::max(y0, 0), yb = std::min<int>(y0 + (int)h, (int)H);
    if (xa >= xb || ya >= yb) continue;
    for (int y = ya; y < yb; y++) for (int x = xa; x < xb; x++) {
      u32 sx = (fl & SPF_HFLIP) ? w - 1 - (x - x0) : (u32)(x - x0), sy = (fl & SPF_VFLIP) ? h - 1 - (y - y0) : (u32)(y - y0);
      u32 s = rbe32(bus.raw(sa + sy * sp + sx * 4, 4)); u8* dp = bus.raw(j.dst + y * j.dpitch + x * 4, 4);
      wbe32(dp, (fl & SPF_OPAQUE) ? s : alpha_over(s, rbe32(dp)));
    }
    u64 vis = (u64)(xb - xa) * (yb - ya); spr_px += vis; cyc += vis * 3; sprites_drawn++;
    spr_fin = std::max(spr_fin, bus.reserve(sa, vis * 4, now));
  }
  spr_n = n; pixels += spr_px;
  spr_fin = std::max(spr_fin, bus.reserve(j.src, (u64)n * SPRITE_ENTRY_SIZE, now));
  spr_fin = std::max(spr_fin, bus.reserve(j.dst, spr_px * 8, now));       // RMW do destino
  return cyc;
}

u64 Blitter::execute(const Job& j) {
  if (j.op == BOP_SPRITES) return exec_sprites(j);
  const u32 W = j.w, H = j.h; if (!W || !H) return 1;
  u32 sbpp = j.op == BOP_CONV565 ? 2 : 4;
  bool needs_src = j.op != BOP_FILL;
  if (!bus.raw_ok(j.dst, (H - 1) * j.dpitch + W * 4)) { error = true; return 0; }
  if (needs_src && !bus.raw_ok(j.src, (H - 1) * j.spitch + W * sbpp)) { error = true; return 0; }
  if (j.op == BOP_MINTERM && !bus.raw_ok(j.mask, (H - 1) * j.mpitch + W * 4)) { error = true; return 0; }
  // Copia a origem inteira (semântica de memmove: rolagem com sobreposição funciona)
  std::vector<u32> src;
  if (needs_src) {
    src.resize((size_t)W * H);
    for (u32 y = 0; y < H; y++) for (u32 x = 0; x < W; x++) {
      if (j.op == BOP_CONV565) { u16 c = rbe16(bus.raw(j.src + y * j.spitch + x * 2, 2)); int rr = (c >> 11) & 31, gg = (c >> 5) & 63, bb = c & 31;
        src[(size_t)y * W + x] = 0xFF000000u | ((u32)((rr << 3) | (rr >> 2)) << 16) | ((u32)((gg << 2) | (gg >> 4)) << 8) | (u32)((bb << 3) | (bb >> 2)); }
      else src[(size_t)y * W + x] = rbe32(bus.raw(j.src + y * j.spitch + x * 4, 4));
    }
  }
  for (u32 y = 0; y < H; y++) for (u32 x = 0; x < W; x++) {
    u8* dp = bus.raw(j.dst + y * j.dpitch + x * 4, 4); u32 d = rbe32(dp), s = needs_src ? src[(size_t)y * W + x] : 0, o = d;
    switch (j.op) {
      case BOP_COPY: case BOP_CONV565: o = s; break;
      case BOP_FILL: o = j.color; break;
      case BOP_COLORKEY: o = ((s & 0xFFFFFF) == (j.color & 0xFFFFFF)) ? d : s; break;
      case BOP_ALPHA: o = alpha_over(s, d); break;
      case BOP_MINTERM: {
        u32 a = rbe32(bus.raw(j.mask + y * j.mpitch + x * 4, 4)); o = 0;
        for (int k = 0; k < 8; k++) if (j.minterm & (1u << k)) {
          u32 ma = (k & 4) ? a : ~a, mb = (k & 2) ? s : ~s, mc = (k & 1) ? d : ~d; o |= ma & mb & mc;
        }
        break;
      }
      default: error = true; return 0;
    }
    wbe32(dp, o);
  }
  pixels += (u64)W * H;
  u32 ch = j.op == BOP_FILL ? 1 : j.op == BOP_MINTERM ? 3 : 2;
  return (u64)W * H * ch;
}

// ---------------- Copper ----------------
u32 Copper::read(u32 off) { return off == COP_LIST ? list : off == COP_CTRL ? (enabled ? 1 : 0) : 0; }
void Copper::write(u32 off, u32 v) { if (off == COP_LIST) list = v; else if (off == COP_CTRL) enabled = v & 1; }
void Copper::run_line(int line) {
  int guard = 0;
  while (!ended && guard++ < 4096) {
    if (!bus.raw_ok(pc, 8)) { ended = true; return; }
    u32 w0 = bus.raw_r32(pc), w1 = bus.raw_r32(pc + 4); pc += 8; ops_executed++;
    switch (w0 >> 28) {
      case COPOP_END: ended = true; return;
      case COPOP_WAIT: if ((int)((w0 >> 12) & 0xFFFF) > line) { pc -= 8; ops_executed--; return; } break;
      case COPOP_MOVE: { u32 a = MMIO_BASE | (w0 & 0x0FFFFFFF); if (a >= COP_BASE && a < COP_BASE + 0x10000) break; if (before_move) before_move(a); bus.cpu_write(a, 4, w1); break; }
      case COPOP_JUMP: pc = w1; break;
      case COPOP_SKIP: if (line >= (int)((w0 >> 12) & 0xFFFF)) pc += 8; break;
      case COPOP_IRQ: bus.raise_irq(IRQ_COPPER); break;
      default: ended = true; return;
    }
  }
}

// ---------------- Video ----------------
void Video::set_mode(u32 m) {
  mode = m;
  switch (m) { case VMODE_240P: W = 320; H = 240; break; case VMODE_480I: case VMODE_480P: W = 640; H = 480; break; default: W = 1280; H = 720; break; }
  frame.assign((size_t)W * H, 0);
}
u32 Video::read(u32 off) {
  if (off >= VID_PALETTE && off < VID_PALETTE + 0x400) return palette[(off - VID_PALETTE) / 4];
  if (off >= VID_LAYER0 && off < VID_LAYER0 + VID_LAYERS * VID_LAYER_STRIDE) {
    const Layer& l = layer[(off - VID_LAYER0) / VID_LAYER_STRIDE];
    switch ((off - VID_LAYER0) % VID_LAYER_STRIDE) {
      case LR_ADDR: return l.addr; case LR_PITCH: return l.pitch; case LR_SCX: return (u32)l.scx; case LR_SCY: return (u32)l.scy;
      case LR_ALPHA: return l.alpha; case LR_WINX: return l.winx; case LR_WINY: return l.winy; case LR_FLAGS: return l.flags; case LR_SIZE: return l.size;
    }
    return 0;
  }
  switch (off) {
    case VID_MODE: return mode; case VID_BG: return bg; case VID_P0_ADDR: return layer[0].addr; case VID_P0_PITCH: return layer[0].pitch;
    case VID_P1_ADDR: return layer[1].addr; case VID_P1_PITCH: return layer[1].pitch; case VID_CTRL: return ctrl; case VID_FRAME: return frames; case VID_LINE: return cur_line;
    case VID_ORDER: return order;
  }
  return 0;
}
void Video::write(u32 off, u32 v) {
  if (off >= VID_PALETTE && off < VID_PALETTE + 0x400) { palette[(off - VID_PALETTE) / 4] = v; return; }
  if (off >= VID_LAYER0 && off < VID_LAYER0 + VID_LAYERS * VID_LAYER_STRIDE) {
    Layer& l = layer[(off - VID_LAYER0) / VID_LAYER_STRIDE];
    switch ((off - VID_LAYER0) % VID_LAYER_STRIDE) {
      case LR_ADDR: l.addr = v; break; case LR_PITCH: l.pitch = v; break; case LR_SCX: l.scx = (i32)v; break; case LR_SCY: l.scy = (i32)v; break;
      case LR_ALPHA: l.alpha = std::min<u32>(v, 255); break; case LR_WINX: l.winx = v; break; case LR_WINY: l.winy = v; break;
      case LR_FLAGS: l.flags = v & 31; break; case LR_SIZE: l.size = v; break;
    }
    return;
  }
  switch (off) {
    case VID_MODE: set_mode(v & 3); break; case VID_BG: bg = v; break; case VID_P0_ADDR: layer[0].addr = v; break; case VID_P0_PITCH: layer[0].pitch = v; break;
    case VID_P1_ADDR: layer[1].addr = v; break; case VID_P1_PITCH: layer[1].pitch = v; break; case VID_CTRL: ctrl = v; break;
    case VID_ORDER: order = v & 0xFFFF; break;
  }
}
void Video::start() {
  if (started) return; started = true;
  auto fn = std::make_shared<std::function<void()>>();
  *fn = [this, fn]() { vblank(); bus.sched->at(FRAME_PS, [fn]() { (*fn)(); }); };
  bus.sched->at(FRAME_PS, [fn]() { (*fn)(); });
}
void Video::vblank() {
  if (on_vblank_hook) on_vblank_hook();
  compose(); frames++;
  if (ctrl & 4) bus.raise_irq(IRQ_VBLANK);
  if (on_frame) on_frame(*this);
}
void Video::compose_line(const Line& ln) {
  const u32* pal = ln.pal >= 0 ? &pend_pal[(size_t)ln.pal * 256] : palette;
  u32* out = &frame[(size_t)ln.y * W];
  for (int x = 0; x < W; x++) out[x] = ln.bg & 0xFFFFFF;
  // Camada por camada (na ordem) em vez de pixel por pixel: cada pixel recebe a mesma sequência de composições. O trecho
  // visível de cada camada e o índice de origem são calculados uma vez por linha, sem módulo nem testes por pixel.
  for (int k = 0; k < ln.nrows; k++) {
    const Row& r = ln.rows[k];
    i64 lo = std::max(r.x0, 0), hi = std::min(r.x1, W), sx;
    if (r.wrap) { sx = (lo + r.scx) % r.w; if (sx < 0) sx += r.w; }
    else { lo = std::max<i64>(lo, -(i64)r.scx); hi = std::min<i64>(hi, (i64)r.w - r.scx); sx = lo + r.scx; }
    if (lo >= hi) continue;
    const bool full = r.alpha == 255, premul = r.pre && !r.add, add = r.add;
    for (i64 x = lo; x < hi; x++) {
      u32 s = r.idx ? pal[r.p[sx]] : rbe32(r.p + sx * 4);
      if (++sx == r.w) sx = 0;
      u32 a = full ? s >> 24 : (s >> 24) * r.alpha / 255, c = out[x];
      if (premul) {                                                           // pré-multiplicado: brilhos aditivos (alfa 0) também aparecem
        if (!(s & 0xFFFFFF) && !a) continue;
        u32 o = 0; for (int sh = 0; sh < 24; sh += 8) o |= std::min<u32>(255, ((((s >> sh) & 255) * r.alpha + 127) / 255) + ((((c >> sh) & 255) * (255 - a) + 127) / 255)) << sh;
        out[x] = o; continue;
      }
      if (!a) continue;
      if (add) {
        u32 o = 0; for (int sh = 0; sh < 24; sh += 8) o |= std::min<u32>(255, ((c >> sh) & 255) + ((((s >> sh) & 255) * a + 127) / 255)) << sh;
        out[x] = o; continue;
      }
      if (a == 255) { out[x] = s & 0xFFFFFF; continue; }
      u32 o = 0; for (int sh = 0; sh < 24; sh += 8) o |= ((((s >> sh) & 255) * a + ((c >> sh) & 255) * (255 - a) + 127) / 255) << sh;
      out[x] = o;
    }
  }
}
// Compõe as linhas pendentes (a memória e o estado que elas leem não mudam até aqui; cada linha escreve só a sua parte do quadro).
void Video::compose_flush() {
  int n = (int)pend.size(), nthr = HostPool::auto_threads();
  if (n >= 32 && nthr > 1) {
    if (!pool) pool.reset(new HostPool(nthr));
    const int per = 8;
    pool->run((n + per - 1) / per, [&](int t, int) { for (int k = t * per; k < std::min(n, t * per + per); k++) compose_line(pend[k]); });
  } else for (const Line& ln : pend) compose_line(ln);
  pend.clear(); pend_pal.clear();
}
void Video::compose() {
  cop.restart();
  // MOVE do Copper para fora dos registradores do vídeo (ou troca de modo) pode alterar memória/estado lidos pelas linhas já
  // capturadas: compõe as pendentes antes, como na ordem serial.
  cop.before_move = [this](u32 a) { if (a < VIDEO_BASE || a >= VIDEO_BASE + 0x1000 || a == VIDEO_BASE + VID_MODE) compose_flush(); };
  bool interlaced = mode == VMODE_480I; int field = frames & 1;
  for (int y = 0; y < H; y++) {
    cur_line = y;
    cop.run_line(y);                                   // efeitos por scanline: Copper altera registradores do vídeo
    if (interlaced && (y & 1) != field) continue;
    // Linha de cada camada (ponteiro para o início da linha do mapa, já com scroll vertical aplicado)
    Line ln; ln.y = y; ln.nrows = 0; ln.pal = -1; ln.bg = bg; bool anyidx = false;
    for (int k = 0; k < VID_LAYERS; k++) {
      int i = (order >> (4 * k)) & 15; if (i >= VID_LAYERS || !enabled(i)) continue;
      const Layer& l = layer[i]; bool idx = indexed(i), wrap = l.flags & LF_WRAP; int bpp = idx ? 1 : 4;
      int w = (l.size & 0xFFFF) ? (int)(l.size & 0xFFFF) : W, h = (l.size >> 16) ? (int)(l.size >> 16) : H;
      int wy0 = l.winy & 0xFFFF, wy1 = l.winy >> 16; if (wy1 && (y < wy0 || y >= wy1)) continue;
      i64 sy = (i64)y + l.scy;
      if (wrap) { sy %= h; if (sy < 0) sy += h; } else if (sy < 0 || sy >= h) continue;
      u32 ra = l.addr + (u32)sy * l.pitch; if (!bus.raw_ok(ra, (u32)w * bpp)) continue;
      int x0 = l.winx & 0xFFFF, x1 = l.winx >> 16; if (!x1) { x0 = 0; x1 = W; }
      ln.rows[ln.nrows++] = Row{bus.raw(ra, (u32)w * bpp), w, bpp, idx, wrap, (bool)(l.flags & LF_ADD), (bool)(l.flags & LF_PREMUL), l.scx, l.alpha, x0, std::min(x1, W)};
      anyidx |= idx;
    }
    if (anyidx) { ln.pal = (int)(pend_pal.size() / 256); pend_pal.insert(pend_pal.end(), palette, palette + 256); }   // o Copper pode reescrever a paleta
    pend.push_back(ln);
  }
  compose_flush();
  cop.before_move = nullptr;
  cur_line = 0;
}

}  // namespace sf
