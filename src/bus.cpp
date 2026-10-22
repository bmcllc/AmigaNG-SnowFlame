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
  ep.i++;
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
  dc.misses++; ep.d++; cpu_stall_ps += 60000 + (Ps)16 * 1000000ull / 2128;   // latência DDR + linha de 16 B
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
  if (inval) ep.d++;
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
  ep.d++;
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
    int i = (h + k) & 63; const TlbEntry& e = std::as_const(tlb)[i];
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

void Bus::set_hint(LineHint* tab, int n, u64 epoch, u32 va, u8* data, DCache::Line* ln) {
  LineHint& h = tab[(va >> 4) & (n - 1)];
  h.vline = va & ~15u; h.ep = epoch; h.ti = xl_ti; h.data = data; h.ln = ln;
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
    if (cache_enabled) { cache_bytes(a, buf, size, false); if ((a & 15) + (u32)size <= 16 && !on_access) set_hint(rh, DHINTS, ep.d, va, dc.lines[hit_set][hit_way].data, &dc.lines[hit_set][hit_way]); } else std::memcpy(buf, &ddr[a], size);
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
    if (cache_enabled) { cache_bytes(a, buf, size, true); if ((a & 15) + (u32)size <= 16 && !on_access) set_hint(wh, DHINTS, ep.d, va, dc.lines[hit_set][hit_way].data, &dc.lines[hit_set][hit_way]); } else std::memcpy(&ddr[a], buf, size);
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
  imisses++; ep.i++; int w = irr[set]++ & 3; ivalid[set][w] = true; itag[set][w] = tag; hit_set = set; hit_way = w;
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
    if (!(a & 1)) { const u8* l = iline(a); set_hint(fh, FHINTS, ep.i, va, idata[hit_set][hit_way], nullptr); return rbe16(l + (a & 15)); }
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

// ---------------- Árbitro de memória ----------------
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

}  // namespace sf
