// SnowBridge em SystemVerilog: identificação, relógio, IRQ, cache, semáforos e motor DMA de 4 canais com descritores
// encadeados. Mesmo comportamento de src/bridge.cpp. O estado do barramento consultado pela CPU a cada acesso (pendências
// e máscara de IRQ, semáforos, habilitação das caches) é mantido pelo hospedeiro (fab_busreg).
module sf_bridge;
  import fabric::*; import sf100_abi::*;
  chandle h; int dev;
  u32_t c_src[4], c_dst[4], c_len[4], c_flags[4], c_desc[4], c_status[4];
  u32_t time_hi_latch, cache_addr, cache_len;
  longint unsigned dma_bytes, dma_jobs;
  export "DPI-C" function bridge_bind;
  export "DPI-C" function bridge_read;
  export "DPI-C" function bridge_write;
  export "DPI-C" function bridge_event;

  function automatic void bridge_bind(chandle hh, int d);
    h = hh; dev = d; time_hi_latch = 0; cache_addr = 0; cache_len = 0; dma_bytes = 0; dma_jobs = 0;
    for (int c = 0; c < 4; c++) begin c_src[c] = 0; c_dst[c] = 0; c_len[c] = 0; c_flags[c] = 0; c_desc[c] = 0; c_status[c] = 0; end
  endfunction

  function automatic int region_of(u32_t a);
    if (a < DDR_SIZE) return 0;
    if (a >= VRAM_BASE && a < VRAM_BASE + VRAM_SIZE) return 1;
    if (a >= ARAM_BASE && a < ARAM_BASE + ARAM_SIZE) return 2;
    return 3;
  endfunction

  function automatic u32_t bridge_read(u32_t off);
    longint unsigned ns; u32_t s; int c;
    if (off == BR_ID) return 32'h53463130;
    if (off == BR_VERSION) return 32'h0001;
    if (off == BR_TIME_LO) begin ns = fab_now(h) / 1000; time_hi_latch = u32_t'(ns >> 32); return u32_t'(ns); end
    if (off == BR_TIME_HI) return time_hi_latch;
    if (off == BR_IRQ_STATUS) return fab_busreg(h, BUS_IRQ_STATUS);
    if (off == BR_IRQ_MASK) return fab_busreg(h, BUS_IRQ_MASK);
    if (off == BR_CACHE_CTRL) return (fab_busreg(h, BUS_DCACHE_EN) != 0 ? 1 : 0) | (fab_busreg(h, BUS_ICACHE_EN) != 0 ? 0 : 2);   // bit1 = I-cache desligada
    if (off >= BR_SEM0 && off < BR_SEM0 + 64) begin
      s = fab_busreg(h, BUS_SEM0 + int'((off - BR_SEM0) / 4));
      if (s > 0) fab_set_busreg(h, BUS_SEM0 + int'((off - BR_SEM0) / 4), s - 1);
      return s;                                     // P(): devolve o valor antigo; 0 = falhou
    end
    if (off >= BR_DMA0 && off < BR_DMA0 + 32'h80) begin
      c = int'((off - BR_DMA0) / 32'h20);
      case ((off - BR_DMA0) % 32'h20)
        DMA_SRC: return c_src[c]; DMA_DST: return c_dst[c]; DMA_LEN: return c_len[c];
        DMA_FLAGS: return c_flags[c]; DMA_DESC: return c_desc[c]; DMA_STATUS: return c_status[c];
        default: ;
      endcase
    end
    return 0;
  endfunction

  function automatic void bridge_write(u32_t off, u32_t v);
    int c, n;
    if (off == BR_IRQ_STATUS) begin fab_set_busreg(h, BUS_IRQ_STATUS, fab_busreg(h, BUS_IRQ_STATUS) & ~v); return; end
    if (off == BR_IRQ_MASK) begin fab_set_busreg(h, BUS_IRQ_MASK, v); return; end
    if (off == BR_SLEEP_US) begin fab_stall(h, 64'(v) * 1000000); return; end
    if (off == BR_SYS_CTRL) begin if ((v & 1) != 0) fab_out(h, dev, 0, 1); return; end   // reinício pelo IPL (frontend)
    if (off == BR_CACHE_ADDR) begin cache_addr = v; return; end
    if (off == BR_CACHE_LEN) begin cache_len = v; return; end
    if (off == BR_CACHE_CTRL) begin
      if (fab_busreg(h, BUS_DCACHE_EN) != 0 && (v & 1) == 0) fab_cache(h, CACHE_FLUSH_INVAL_ALL, 0, 0);
      fab_set_busreg(h, BUS_DCACHE_EN, v & 1); fab_set_busreg(h, BUS_ICACHE_EN, (v & 2) == 0 ? 1 : 0); return;
    end
    if (off == BR_CACHE_OP) begin fab_cache(h, v, cache_addr, cache_len); return; end
    if (off >= BR_SEM0 && off < BR_SEM0 + 64) begin                                   // V()
      n = BUS_SEM0 + int'((off - BR_SEM0) / 4); fab_set_busreg(h, n, fab_busreg(h, n) + (v != 0 ? v : 1)); return;
    end
    if (off >= BR_DMA0 && off < BR_DMA0 + 32'h80) begin
      c = int'((off - BR_DMA0) / 32'h20);
      case ((off - BR_DMA0) % 32'h20)
        DMA_SRC: c_src[c] = v; DMA_DST: c_dst[c] = v; DMA_LEN: c_len[c] = v; DMA_FLAGS: c_flags[c] = v; DMA_DESC: c_desc[c] = v;
        DMA_CTRL: if ((v & DMAC_START_REGS) != 0) start(c, 0); else if ((v & DMAC_START_CHAIN) != 0) start(c, 1);
        default: ;
      endcase
    end
  endfunction

  function automatic void fail(int c);
    c_status[c] = (c_status[c] & ~DMAS_BUSY) | DMAS_ERROR; if ((c_flags[c] & DMAF_IRQ) != 0) fab_irq(h, IRQ_DMA0 << c);
  endfunction

  function automatic void start(int c, bit chain);
    u32_t d;
    if ((c_status[c] & DMAS_BUSY) != 0) begin c_status[c] |= DMAS_ERROR; return; end   // canal ocupado: ignora e sinaliza
    c_status[c] = DMAS_BUSY;
    if (!chain) begin exec(c, c_src[c], c_dst[c], c_len[c], c_flags[c], 0); return; end
    d = c_desc[c];
    if (!fab_ok(h, d, DMA_DESC_SIZE)) begin fail(c); return; end
    exec(c, fab_r32(h, d), fab_r32(h, d + 4), fab_r32(h, d + 8), fab_r32(h, d + 12), fab_r32(h, d + 16));
  endfunction

  function automatic void exec(int c, u32_t src, u32_t dst, u32_t len, u32_t flags, u32_t next);
    bit fill, dok, sok, bad; ps_t now, t0, fin, r;
    fill = (flags & DMAF_FILL) != 0;
    dok = fab_ok(h, dst, len != 0 ? len : 1);
    sok = fill ? 0 : fab_ok(h, src, len != 0 ? len : 1);
    bad = (!dok && len != 0) || (!fill && !sok && len != 0);
    if ((flags & DMAF_BSWAP32) != 0 && (len & 3) != 0) bad = 1;
    if ((flags & DMAF_BSWAP16) != 0 && (len & 1) != 0) bad = 1;
    if (bad) begin fail(c); return; end              // destino MMIO também é recusado (fab_ok só aceita memória)
    if (len != 0) begin
      if (fill) fab_fill(h, dst, src, len);
      else fab_copy(h, dst, src, len, (flags & DMAF_BSWAP32) != 0 ? 1 : (flags & DMAF_BSWAP16) != 0 ? 2 : 0);   // memmove
    end
    c_src[c] = src; c_dst[c] = dst; c_len[c] = len; c_flags[c] = flags;
    dma_bytes += 64'(len); dma_jobs++; fab_obs(h, dev, 0, dma_bytes); fab_obs(h, dev, 1, dma_jobs);
    now = fab_now(h); t0 = now + 150000;            // 150 ns de setup
    fin = fab_reserve(h, dst, 64'(len), t0);
    if (!fill && region_of(src) != region_of(dst)) begin r = fab_reserve(h, src, 64'(len), t0); if (r > fin) fin = r; end   // mesma região: um único trânsito
    fab_at(h, dev, fin - now, c, 64'(next), 64'((flags & DMAF_IRQ) != 0));
  endfunction

  function automatic void bridge_event(int c, longint unsigned next, longint unsigned irq);
    u32_t n;
    n = u32_t'(next);
    if (irq != 0) fab_irq(h, IRQ_DMA0 << c);
    if (n != 0) begin
      if (!fab_ok(h, n, DMA_DESC_SIZE)) begin fail(c); return; end
      c_desc[c] = n;
      exec(c, fab_r32(h, n), fab_r32(h, n + 4), fab_r32(h, n + 8), fab_r32(h, n + 12), fab_r32(h, n + 16));
    end else c_status[c] = DMAS_DONE;
  endfunction
endmodule
