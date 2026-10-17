#include "bus.h"

namespace sf {
using namespace abi;

void Bus::icache_inval_range(u32 a, u32 len) {
  if (!len || a >= DDR_SIZE) return;
  u64 end = std::min<u64>((u64)a + len, DDR_SIZE);
  for (u64 la = a & ~15u; la < end; la += 16) {
    int set = (la >> 4) & 127; u32 tag = (u32)la >> 11;
    for (int w = 0; w < 4; w++) if (ivalid[set][w] && itag[set][w] == tag) ivalid[set][w] = false;
  }
}

Bus::Bus() : ddr(DDR_SIZE, 0), vram(VRAM_SIZE, 0), aram(ARAM_SIZE, 0), sram(SRAM_SIZE, 0) {}

void Bus::map_device(u32 base, u32 size, Device* d) { maps.push_back({base, size, d}); }
void Bus::add_window(u32 base, u32 size, u8* ptr) { wins.push_back({base, size, ptr}); }

u8* Bus::raw(u32 a, u32 len) {
  u64 end = (u64)a + len;
  if (a < DDR_SIZE) return end <= DDR_SIZE ? &ddr[a] : nullptr;
  if (a >= VRAM_BASE && a < VRAM_BASE + VRAM_SIZE) return end <= (u64)VRAM_BASE + VRAM_SIZE ? &vram[a - VRAM_BASE] : nullptr;
  if (a >= ARAM_BASE && a < ARAM_BASE + ARAM_SIZE) return end <= (u64)ARAM_BASE + ARAM_SIZE ? &aram[a - ARAM_BASE] : nullptr;
  if (a >= SRAM_BASE && a < SRAM_BASE + SRAM_SIZE) return end <= (u64)SRAM_BASE + SRAM_SIZE ? &sram[a - SRAM_BASE] : nullptr;
  for (auto& w : wins) if (a >= w.base && a < w.base + w.size) return end <= (u64)w.base + w.size ? w.ptr + (a - w.base) : nullptr;
  return nullptr;
}
u32 Bus::raw_r32(u32 a) { u8* p = raw(a, 4); if (!p) { fault = true; fault_addr = a; return 0; } return rbe32(p); }
void Bus::raw_w32(u32 a, u32 v) { u8* p = raw(a, 4); if (!p) { fault = true; fault_addr = a; return; } wbe32(p, v); }
u16 Bus::raw_r16(u32 a) { u8* p = raw(a, 2); if (!p) { fault = true; fault_addr = a; return 0; } return rbe16(p); }
void Bus::raw_w16(u32 a, u16 v) { u8* p = raw(a, 2); if (!p) { fault = true; fault_addr = a; return; } wbe16(p, v); }
u8 Bus::raw_r8(u32 a) { u8* p = raw(a, 1); if (!p) { fault = true; fault_addr = a; return 0; } return *p; }
void Bus::raw_w8(u32 a, u8 v) { u8* p = raw(a, 1); if (!p) { fault = true; fault_addr = a; return; } *p = v; }
void Bus::raw_write(u32 a, const void* src, u32 n) {
  u8* p = raw(a, n); if (!p) throw std::runtime_error("raw_write: endereço inválido");
  std::memcpy(p, src, n); icache_inval_range(a, n);       // carregador do host: o código novo vale já
}
void Bus::raw_read(u32 a, void* dst, u32 n) {
  u8* p = raw(a, n); if (!p) throw std::runtime_error("raw_read: endereço inválido");
  std::memcpy(dst, p, n);
}

bool Bus::peek(u32 a, void* dst, u32 n) const {
  u8* o = (u8*)dst;
  for (u32 i = 0; i < n; i++) {
    u32 ad = a + i;
    if (ad < DDR_SIZE) {
      int set = (ad >> 4) & (DCache::SETS - 1); u32 tag = ad >> 11; bool hit = false;
      for (int w = 0; w < DCache::WAYS; w++) { const auto& l = dc.lines[set][w]; if (l.v && l.tag == tag) { o[i] = l.data[ad & 15]; hit = true; break; } }
      if (!hit) o[i] = ddr[ad];
    } else { const u8* p = raw(ad, 1); if (!p) return false; o[i] = *p; }
  }
  return true;
}

Device* Bus::find_dev(u32 a, u32& off) {
  for (auto& m : maps) if (a >= m.base && a < m.base + m.size) { off = a - m.base; return m.dev; }
  return nullptr;
}

// ---------------- Cache ----------------
void Bus::wb(DCache::Line& l, int set) {
  u32 addr = (l.tag << 11) | ((u32)set << 4);
  std::memcpy(&ddr[addr], l.data, 16);
  l.d = false; dc.writebacks++;
}
DCache::Line& Bus::cache_line(u32 a) {
  int set = (a >> 4) & (DCache::SETS - 1); u32 tag = a >> 11;
  for (int w = 0; w < DCache::WAYS; w++) {
    auto& l = dc.lines[set][w];
    if (l.v && l.tag == tag) { dc.hits++; hit_set = set; hit_way = w; return l; }
  }
  dc.misses++; cpu_stall_ps += 60000 + (Ps)16 * 1000000ull / 2128;   // latência DDR + linha de 16 B
  int way = -1;
  for (int w = 0; w < DCache::WAYS; w++) if (!dc.lines[set][w].v) { way = w; break; }
  if (way < 0) way = dc.rr[set]++ & 3;
  auto& l = dc.lines[set][way]; hit_set = set; hit_way = way;
  if (l.v && l.d) wb(l, set);
  l.v = true; l.d = false; l.tag = tag;
  std::memcpy(l.data, &ddr[a & ~15u], 16);
  return l;
}
void Bus::cache_bytes(u32 a, u8* buf, int n, bool wr) {
  while (n > 0) {
    auto& l = cache_line(a);
    int o = a & 15, c = std::min(n, 16 - o);
    if (wr) { std::memcpy(l.data + o, buf, c); l.d = true; } else std::memcpy(buf, l.data + o, c);
    a += c; buf += c; n -= c;
  }
}
void Bus::cache_flush_range(u32 a, u32 len, bool inval) {
  if (len == 0) return;
  u64 end = std::min<u64>((u64)a + len, DDR_SIZE);
  u32 first = a & ~15u;
  for (u64 la = first; la < end; la += 16) {
    int set = (la >> 4) & (DCache::SETS - 1); u32 tag = (u32)la >> 11;
    for (int w = 0; w < DCache::WAYS; w++) {
      auto& l = dc.lines[set][w];
      if (l.v && l.tag == tag) { if (l.d) wb(l, set); if (inval) l.v = false; }
    }
  }
}
void Bus::cache_inval_range(u32 a, u32 len) {
  if (len == 0) return;
  u64 end = std::min<u64>((u64)a + len, DDR_SIZE);
  for (u64 la = a & ~15u; la < end; la += 16) {
    int set = (la >> 4) & (DCache::SETS - 1); u32 tag = (u32)la >> 11;
    for (int w = 0; w < DCache::WAYS; w++) {
      auto& l = dc.lines[set][w];
      if (l.v && l.tag == tag) { l.v = false; l.d = false; }   // descarta dados sujos (semântica de invalidate)
    }
  }
}

// ---------------- Acesso do CPU ----------------
bool Bus::xlate(u32 va, u32 acc, u32& pa) {
  pa = va; xl_ti = -1;
  if (!mmu_on || mmu_bypass || va >= MMIO_BASE || (mmu_super_bypass && cpu_super)) return true;
  u32 vpn = va >> 12;
  int h = tlb_hint[vpn & 63];
  for (int k = 0; k < 64; k++) {
    int i = (h + k) & 63; auto& e = tlb[i];
    if (!e.v || e.vpn != vpn) continue;
    tlb_hint[vpn & 63] = (u8)i;
    if (((e.perm & acc) != acc) || ((e.perm & MP_SUPER) && !cpu_super)) { mmu_fsr = 2 | (acc << 4); mmu_far = va; fault = true; fault_addr = va; return false; }
    tlb_hits++; pa = (e.ppn << 12) | (va & 0xFFF); xl_ti = i; return true;
  }
  tlb_misses++; mmu_fsr = 1 | (acc << 4); mmu_far = va; fault = true; fault_addr = va; return false;
}

// Traduz os n bytes a partir de va byte a byte (acessos que cruzam página podem cair em páginas físicas não contíguas).
// Só devolve true se todas as páginas traduzirem; em falha nada foi acessado e fault/FAR/FSR descrevem a página que falhou.
bool Bus::xlate_span(u32 va, int n, u32 acc, u32* pa) {
  for (int i = 0; i < n; i++) if (!xlate(va + i, acc, pa[i])) return false;
  return true;
}
static inline bool crosses_page(u32 a, int size) { return (a & 0xFFF) + (u32)size > 0x1000; }

void Bus::set_hint(LineHint* tab, int n, u32 va, u32 pa) {
  LineHint& h = tab[(va >> 4) & (n - 1)];
  h.vline = va & ~15u; h.ep = mode_epoch; h.ti = xl_ti; h.set = hit_set; h.way = hit_way; h.tag = pa >> 11;
  if (xl_ti >= 0) { h.vpn = tlb[xl_ti].vpn; h.ppn = tlb[xl_ti].ppn; h.perm = tlb[xl_ti].perm; }
}

u32 Bus::cpu_read_slow(u32 a, int size) {
  if (on_access && !mmu_bypass) on_access(a, size, false);
  if (mmu_on && !mmu_bypass && a < MMIO_BASE && size > 1 && crosses_page(a, size)) {
    u32 pa[4]; if (!xlate_span(a, size, MP_R, pa)) return 0;
    mmu_bypass = true; u32 v = 0; for (int i = 0; i < size; i++) v = (v << 8) | cpu_read(pa[i], 1); mmu_bypass = false;
    return v;
  }
  u32 va = a;
  { u32 pa; if (!xlate(a, MP_R, pa)) return 0; a = pa; }
  if (a < DDR_SIZE && (u64)a + size <= DDR_SIZE) {
    u8 buf[4];
    if (cache_enabled) { cache_bytes(a, buf, size, false); if ((a & 15) + (u32)size <= 16 && !on_access) set_hint(rh, DHINTS, va, a); } else std::memcpy(buf, &ddr[a], size);
    return size == 1 ? buf[0] : size == 2 ? rbe16(buf) : rbe32(buf);
  }
  if (a >= MMIO_BASE) {
    u32 off; Device* d = find_dev(a & ~3u, off);
    if (d) {
      if (d->native16()) {
        if (size == 4) return ((u32)d->read16(off) << 16) | d->read16(off + 2);
        if (size == 2) return d->read16(off + (a & 2));
        u16 w = d->read16(off + (a & 2)); return (a & 1) ? (w & 0xFF) : (w >> 8);
      }
      u32 v = d->read(off);
      if (size == 4) return v;
      if (size == 2) return (v >> (((a & 2) ^ 2) * 8)) & 0xFFFF;
      return (v >> (((a & 3) ^ 3) * 8)) & 0xFF;
    }
  }
  u8* p = raw(a, size);
  if (!p) { fault = true; fault_addr = a; return 0; }
  return size == 1 ? p[0] : size == 2 ? rbe16(p) : rbe32(p);
}
void Bus::cpu_write_slow(u32 a, int size, u32 v) {
  if (on_access && !mmu_bypass) on_access(a, size, true);
  if (mmu_on && !mmu_bypass && a < MMIO_BASE && size > 1 && crosses_page(a, size)) {
    u32 pa[4]; if (!xlate_span(a, size, MP_W, pa)) return;        // traduz tudo antes de escrever: falha não deixa escrita parcial
    mmu_bypass = true; for (int i = 0; i < size; i++) cpu_write(pa[i], 1, v >> (8 * (size - 1 - i))); mmu_bypass = false;
    return;
  }
  u32 va = a;
  { u32 pa; if (!xlate(a, MP_W, pa)) return; a = pa; }
  if (a < DDR_SIZE && (u64)a + size <= DDR_SIZE) {
    u8 buf[4];
    if (size == 1) buf[0] = (u8)v; else if (size == 2) wbe16(buf, (u16)v); else wbe32(buf, v);
    if (cache_enabled) { cache_bytes(a, buf, size, true); if ((a & 15) + (u32)size <= 16 && !on_access) set_hint(wh, DHINTS, va, a); } else std::memcpy(&ddr[a], buf, size);
    return;
  }
  if (a >= MMIO_BASE) {
    u32 off; Device* d = find_dev(a & ~3u, off);
    if (d) {
      if (d->native16()) {
        if (size == 4) { d->write16(off, (u16)(v >> 16)); d->write16(off + 2, (u16)v); return; }
        if (size == 2) { d->write16(off + (a & 2), (u16)v); return; }
        u16 w = d->read16(off + (a & 2)); w = (a & 1) ? (u16)((w & 0xFF00) | (v & 0xFF)) : (u16)((w & 0x00FF) | ((v & 0xFF) << 8)); d->write16(off + (a & 2), w); return;
      }
      if (size == 4) { d->write(off, v); return; }
      u32 cur = d->read(off);
      int sh = size == 2 ? ((a & 2) ^ 2) * 8 : ((a & 3) ^ 3) * 8;
      u32 m = size == 2 ? 0xFFFFu : 0xFFu;
      d->write(off, (cur & ~(m << sh)) | ((v & m) << sh));
      return;
    }
  }
  u8* p = raw(a, size);
  if (!p) { fault = true; fault_addr = a; return; }
  if (size == 1) p[0] = (u8)v; else if (size == 2) wbe16(p, (u16)v); else wbe32(p, v);
}
// Linha da I-cache que contém pa (preenche em caso de falta). Só DDR é cacheável.
const u8* Bus::iline(u32 pa) {
  int set = (pa >> 4) & 127; u32 tag = pa >> 11;
  for (int w = 0; w < 4; w++) if (ivalid[set][w] && itag[set][w] == tag) { ihits++; hit_set = set; hit_way = w; return idata[set][w]; }
  imisses++; int w = irr[set]++ & 3; ivalid[set][w] = true; itag[set][w] = tag; hit_set = set; hit_way = w;
  std::memcpy(idata[set][w], &ddr[pa & ~15u], 16);
  cpu_stall_ps += 60000 + (Ps)16 * 1000000ull / 2128;
  return idata[set][w];
}
u16 Bus::fetch16_slow(u32 a) {
  ifetches++;
  if (mmu_on && !mmu_bypass && a < MMIO_BASE && (a & 0xFFF) == 0xFFF) {      // palavra de instrução cruzando a página
    u32 pa[2]; if (!xlate_span(a, 2, MP_X, pa)) return 0;
    mmu_bypass = true; u16 v = (u16)((fetch16_byte(pa[0]) << 8) | fetch16_byte(pa[1])); mmu_bypass = false; return v;
  }
  u32 va = a;
  { u32 pa; if (!xlate(a, MP_X, pa)) return 0; a = pa; }
  if (a < DDR_SIZE - 1 && icache_enabled) {
    if (!(a & 1)) { const u8* l = iline(a); set_hint(fh, FHINTS, va, a); return rbe16(l + (a & 15)); }
    return (u16)((fetch16_byte(a) << 8) | fetch16_byte(a + 1));
  }
  u8* p = raw(a, 2);
  if (!p) { fault = true; fault_addr = a; return 0; }
  return rbe16(p);
}
u8 Bus::fetch16_byte(u32 pa) {
  if (pa < DDR_SIZE && icache_enabled) return iline(pa)[pa & 15];
  u8* p = raw(pa, 1); if (!p) { fault = true; fault_addr = pa; return 0; } return *p;
}

// ---------------- Bridge (registradores) ----------------
Bus::Region Bus::region_of(u32 a) {
  if (a < DDR_SIZE) return R_DDR;
  if (a >= VRAM_BASE && a < VRAM_BASE + VRAM_SIZE) return R_VRAM;
  if (a >= ARAM_BASE && a < ARAM_BASE + ARAM_SIZE) return R_ARAM;
  return R_OTHER;
}
u32 Bus::region_mbps(Region r) {
  switch (r) { case R_DDR: return 2128;      // 64 bits x 133 MHz DDR
    case R_VRAM: return 5312;                // 128 bits x 166 MHz DDR
    case R_ARAM: return 800; default: return 4000; }
}
Ps Bus::reserve(u32 addr, u64 bytes, Ps earliest) {
  Region r = region_of(addr); Ps dur = (Ps)(bytes * 1000000ull / region_mbps(r));
  Ps start = earliest;
  if (arb_enabled && arb_busy[r] > start) { arb_wait_ps += arb_busy[r] - start; start = arb_busy[r]; }
  arb_bytes[r] += bytes;
  if (arb_enabled) arb_busy[r] = start + dur;
  return start + dur;
}

u32 Bridge::read(u32 off) {
  if (off == BR_ID) return 0x53463130;
  if (off == BR_VERSION) return 0x0001;
  if (off == BR_TIME_LO) { u64 ns = bus.sched->now / 1000; time_hi_latch = (u32)(ns >> 32); return (u32)ns; }
  if (off == BR_TIME_HI) return time_hi_latch;
  if (off == BR_IRQ_STATUS) return bus.irq_status;
  if (off == BR_IRQ_MASK) return bus.irq_mask;
  if (off == BR_CACHE_CTRL) return (bus.cache_enabled ? 1 : 0) | (bus.icache_enabled ? 0 : 2);   // bit1 = I-cache desligada
  if (off >= BR_SEM0 && off < BR_SEM0 + 64) {
    u32& s = bus.sem[(off - BR_SEM0) / 4];
    u32 old = s; if (s > 0) s--; return old;     // P(): devolve valor antigo; 0 = falhou
  }
  if (off >= BR_DMA0 && off < BR_DMA0 + 0x80) {
    int c = (off - BR_DMA0) / 0x20; u32 r = (off - BR_DMA0) % 0x20; Ch& h = ch[c];
    switch (r) { case DMA_SRC: return h.src; case DMA_DST: return h.dst; case DMA_LEN: return h.len;
                 case DMA_FLAGS: return h.flags; case DMA_DESC: return h.desc; case DMA_STATUS: return h.status; }
  }
  return 0;
}
void Bridge::write(u32 off, u32 v) {
  if (off == BR_IRQ_STATUS) { bus.irq_status &= ~v; return; }
  if (off == BR_IRQ_MASK) { bus.irq_mask = v; return; }
  if (off == BR_SLEEP_US) { bus.cpu_stall_ps += (Ps)v * 1000000ull; return; }
  if (off == BR_SYS_CTRL) { if ((v & 1) && on_reboot) on_reboot(); return; }
  if (off == BR_CACHE_ADDR) { cache_addr = v; return; }
  if (off == BR_CACHE_LEN) { cache_len = v; return; }
  if (off == BR_CACHE_CTRL) { if (bus.cache_enabled && !(v & 1)) bus.cache_flush_all(true); bus.cache_enabled = v & 1; bus.icache_enabled = !(v & 2); return; }
  if (off == BR_CACHE_OP) {
    switch (v) {
      case CACHE_FLUSH_ALL: bus.cache_flush_all(false); break;
      case CACHE_INVAL_ALL: bus.cache_inval_all(); break;
      case CACHE_FLUSH_INVAL_ALL: bus.cache_flush_all(true); break;
      case CACHE_FLUSH_RANGE: bus.cache_flush_range(cache_addr, cache_len, false); break;
      case CACHE_INVAL_RANGE: bus.cache_inval_range(cache_addr, cache_len); break;
      case CACHE_FLUSH_INVAL_RANGE: bus.cache_flush_range(cache_addr, cache_len, true); break;
      case ICACHE_INVAL_ALL: bus.icache_inval_all(); break;
      case ICACHE_INVAL_RANGE: bus.icache_inval_range(cache_addr, cache_len); break;
    }
    return;
  }
  if (off >= BR_SEM0 && off < BR_SEM0 + 64) { u32& s = bus.sem[(off - BR_SEM0) / 4]; s += v ? v : 1; return; }   // V()
  if (off >= BR_DMA0 && off < BR_DMA0 + 0x80) {
    int c = (off - BR_DMA0) / 0x20; u32 r = (off - BR_DMA0) % 0x20; Ch& h = ch[c];
    switch (r) {
      case DMA_SRC: h.src = v; break; case DMA_DST: h.dst = v; break; case DMA_LEN: h.len = v; break;
      case DMA_FLAGS: h.flags = v; break; case DMA_DESC: h.desc = v; break;
      case DMA_CTRL: if (v & DMAC_START_REGS) start(c, false); else if (v & DMAC_START_CHAIN) start(c, true); break;
      case DMA_STATUS: break;
    }
  }
}

void Bridge::fail(int c) { ch[c].status = (ch[c].status & ~DMAS_BUSY) | DMAS_ERROR; if (ch[c].flags & DMAF_IRQ) bus.raise_irq(IRQ_DMA0 << c); }

void Bridge::start(int c, bool chain) {
  Ch& h = ch[c];
  if (h.status & DMAS_BUSY) { h.status |= DMAS_ERROR; return; }   // canal ocupado: ignora e sinaliza
  h.status = DMAS_BUSY;
  if (!chain) { exec(c, h.src, h.dst, h.len, h.flags, 0); return; }
  u32 d = h.desc;
  if (!bus.raw_ok(d, DMA_DESC_SIZE)) { fail(c); return; }
  exec(c, bus.raw_r32(d), bus.raw_r32(d + 4), bus.raw_r32(d + 8), bus.raw_r32(d + 12), bus.raw_r32(d + 16));
}

void Bridge::exec(int c, u32 src, u32 dst, u32 len, u32 flags, u32 next) {
  Ch& h = ch[c];
  bool fill = flags & DMAF_FILL;
  u8* dp = bus.raw(dst, len ? len : 1);
  u8* sp = fill ? nullptr : bus.raw(src, len ? len : 1);
  bool bad = (!dp && len) || (!fill && !sp && len);
  if ((flags & DMAF_BSWAP32) && (len & 3)) bad = true;
  if ((flags & DMAF_BSWAP16) && (len & 1)) bad = true;
  // Região de destino não pode ser MMIO (raw já exclui).
  if (bad) { fail(c); return; }
  if (len) {
    if (fill) {
      u8 pat[4]; wbe32(pat, src);
      for (u32 i = 0; i < len; i++) dp[i] = pat[i & 3];
    } else {
      std::vector<u8> tmp(sp, sp + len);   // memmove: tolera sobreposição
      if (flags & DMAF_BSWAP32) for (u32 i = 0; i < len; i += 4) { std::swap(tmp[i], tmp[i + 3]); std::swap(tmp[i + 1], tmp[i + 2]); }
      else if (flags & DMAF_BSWAP16) for (u32 i = 0; i < len; i += 2) std::swap(tmp[i], tmp[i + 1]);
      std::memcpy(dp, tmp.data(), len);
    }
  }
  h.src = src; h.dst = dst; h.len = len; h.flags = flags;
  dma_bytes += len; dma_jobs++;
  Ps t0 = bus.sched->now + 150000;                   // 150 ns de setup
  Ps fin = bus.reserve(dst, len, t0);
  if (!fill && Bus::region_of(src) != Bus::region_of(dst)) fin = std::max(fin, bus.reserve(src, len, t0));
  else if (!fill && src != dst && len) { /* mesma região: um único trânsito */ }
  Ps dur = fin - bus.sched->now;
  bool irq = flags & DMAF_IRQ;
  bus.sched->at(dur, [this, c, next, irq]() {
    Ch& hh = ch[c];
    if (irq) bus.raise_irq(IRQ_DMA0 << c);
    if (next) {
      if (!bus.raw_ok(next, DMA_DESC_SIZE)) { fail(c); return; }
      hh.desc = next;
      exec(c, bus.raw_r32(next), bus.raw_r32(next + 4), bus.raw_r32(next + 8), bus.raw_r32(next + 12), bus.raw_r32(next + 16));
    } else {
      hh.status = DMAS_DONE;
    }
  });
}

}  // namespace sf
