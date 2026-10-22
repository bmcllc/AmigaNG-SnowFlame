// SnowBridge (registradores em BRIDGE_BASE): identificação, relógio, IRQ, cache, semáforos e DMA.
// Modelo de referência em C++; o backend SystemVerilog equivalente está em rtl/sf_bridge.sv.
#include "bus.h"

namespace sf {
using namespace abi;

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
