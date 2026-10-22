// SnowBlitter em SystemVerilog: DMA 2D com cópia, preenchimento, color key, mistura alfa, minterms (3 fontes), conversão
// RGB565 -> ARGB e listas de sprites. Fila de 8 jobs; cada job executa no início (funcional) e conclui após a latência em
// ciclos do chipset (100 MHz) ou do árbitro de memória, o que for maior. Mesmo comportamento de src/chipset.cpp.
module sf_blitter;
  import fabric::*; import sf100_abi::*;
  typedef struct { u32_t op, src, spitch, dst, dpitch, w, h, color, mask, mpitch, minterm; bit irq; } job_t;
  chandle h; int dev;
  u32_t r[16]; job_t q[$]; bit running, error;
  longint unsigned jobs, pixels, sprites_drawn, spr_px, spr_n; ps_t spr_fin;
  export "DPI-C" function blt_bind;
  export "DPI-C" function blt_read;
  export "DPI-C" function blt_write;
  export "DPI-C" function blt_event;
  export "DPI-C" function blt_clear_error;

  function automatic void blt_bind(chandle hh, int d);
    h = hh; dev = d; for (int i = 0; i < 16; i++) r[i] = 0; q.delete(); running = 0; error = 0;
    jobs = 0; pixels = 0; sprites_drawn = 0; spr_px = 0; spr_n = 0; spr_fin = 0;
  endfunction

  // Espelho dos campos públicos do frontend: 0 error, 1 jobs, 2 pixels, 3 sprites_drawn, 4 running
  function automatic void obs();
    fab_obs(h, dev, 0, 64'(error)); fab_obs(h, dev, 1, jobs); fab_obs(h, dev, 2, pixels); fab_obs(h, dev, 3, sprites_drawn); fab_obs(h, dev, 4, 64'(running));
  endfunction
  function automatic void blt_clear_error(); error = 0; endfunction

  function automatic u32_t blt_read(u32_t off);
    if (off == BLT_STATUS) return (running ? 1 : 0) | (error ? 4 : 0) | ((!running && jobs != 0) ? 2 : 0);
    if (off < 32'h40) return r[off / 4];
    return 0;
  endfunction
  function automatic void blt_write(u32_t off, u32_t v);
    job_t j;
    if (off == BLT_STATUS) begin if ((v & 4) != 0) error = 0; obs(); return; end
    if (off == BLT_CTRL) begin
      if ((v & 1) != 0) begin
        j.op = r[BLT_OP / 4]; j.src = r[BLT_SRC / 4]; j.spitch = r[BLT_SRC_PITCH / 4]; j.dst = r[BLT_DST / 4]; j.dpitch = r[BLT_DST_PITCH / 4];
        j.w = r[BLT_W / 4]; j.h = r[BLT_H / 4]; j.color = r[BLT_COLOR / 4]; j.mask = r[BLT_MASK / 4]; j.mpitch = r[BLT_MASK_PITCH / 4];
        j.minterm = r[BLT_MINTERM / 4]; j.irq = (v & 2) != 0;
        if (!submit(j)) error = 1;
        obs();
      end
      return;
    end
    if (off < 32'h40) r[off / 4] = v;
  endfunction

  function automatic bit submit(job_t j);
    if (q.size() >= 8) return 0;
    q.push_back(j); if (!running) kick(); return 1;
  endfunction

  function automatic ps_t cyc2ps(longint unsigned c, longint unsigned hz); return ps_t'((128'(c) * 128'(64'd1000000000000)) / 128'(hz)); endfunction
  function automatic ps_t pmax(ps_t a, ps_t b); return a > b ? a : b; endfunction

  function automatic void kick();
    job_t j; longint unsigned cyc, px; ps_t dur, now, fin; bit reads_dst;
    if (q.size() == 0) begin running = 0; return; end
    j = q.pop_front(); running = 1;
    cyc = execute(j);
    dur = cyc2ps(cyc > 1 ? cyc : 1, 100000000);    // CHIP_HZ
    if (cyc != 0) begin                             // disputa de barramento (bytes por região, por tipo de operação)
      now = fab_now(h); px = 64'(j.w) * 64'(j.h);
      if (j.op == BOP_SPRITES) dur = pmax(dur, spr_fin - now);
      else begin
        reads_dst = j.op == BOP_ALPHA || j.op == BOP_COLORKEY || j.op == BOP_MINTERM;   // leitura-modificação-escrita do destino
        fin = fab_reserve(h, j.dst, px * 4 * (reads_dst ? 2 : 1), now);
        if (j.op != BOP_FILL) fin = pmax(fin, fab_reserve(h, j.src, px * (j.op == BOP_CONV565 ? 2 : 4), now));
        if (j.op == BOP_MINTERM) fin = pmax(fin, fab_reserve(h, j.mask, px * 4, now));
        dur = pmax(dur, fin - now);
      end
    end
    fab_at(h, dev, dur, 0, 64'(j.irq), 0);
  endfunction

  function automatic void blt_event(int tag, longint unsigned irq, longint unsigned unused);
    running = 0; jobs++; if (irq != 0) fab_irq(h, IRQ_BLT); kick(); obs();
  endfunction

  function automatic u32_t alpha_over(u32_t s, u32_t d);
    u32_t sa, da, o, c, oa;
    sa = s >> 24; if (sa == 255) return s; if (sa == 0) return d;
    da = d >> 24; o = 0;
    for (int sh = 0; sh < 24; sh += 8) begin
      c = (((s >> sh) & 255) * sa + ((d >> sh) & 255) * (255 - sa) + 127) / 255; o |= c << sh;
    end
    oa = sa + (da * (255 - sa) + 127) / 255; return o | ((oa < 255 ? oa : 255) << 24);
  endfunction

  function automatic longint unsigned exec_sprites(job_t j);
    u32_t n, W, H, e, sa, sp, fl, w, hh, sx, sy, s, dp; int x0, y0, xa, xb, ya, yb; longint unsigned cyc, vis; ps_t now;
    n = j.color; W = j.w; H = j.h; spr_px = 0; spr_n = 0; now = fab_now(h); spr_fin = now;
    if (n > SPRITE_MAX || W == 0 || H == 0) begin error = 1; return 0; end
    if (n == 0) return 1;
    if (!fab_ok(h, j.src, n * SPRITE_ENTRY_SIZE) || !fab_ok(h, j.dst, (H - 1) * j.dpitch + W * 4)) begin error = 1; return 0; end
    cyc = 0;
    for (u32_t i = 0; i < n; i++) begin
      e = j.src + i * SPRITE_ENTRY_SIZE;
      sa = fab_r32(h, e); sp = 32'(fab_r16(h, e + 4)); fl = 32'(fab_r16(h, e + 6));
      x0 = int'(shortint'(fab_r16(h, e + 8))); y0 = int'(shortint'(fab_r16(h, e + 10))); w = 32'(fab_r16(h, e + 12)); hh = 32'(fab_r16(h, e + 14));
      cyc += 8; if (w == 0 || hh == 0) continue;
      if (!fab_ok(h, sa, (hh - 1) * sp + w * 4)) begin error = 1; return 0; end
      xa = x0 > 0 ? x0 : 0; xb = x0 + int'(w) < int'(W) ? x0 + int'(w) : int'(W);
      ya = y0 > 0 ? y0 : 0; yb = y0 + int'(hh) < int'(H) ? y0 + int'(hh) : int'(H);
      if (xa >= xb || ya >= yb) continue;
      for (int y = ya; y < yb; y++) for (int x = xa; x < xb; x++) begin
        sx = (fl & SPF_HFLIP) != 0 ? w - 1 - u32_t'(x - x0) : u32_t'(x - x0);
        sy = (fl & SPF_VFLIP) != 0 ? hh - 1 - u32_t'(y - y0) : u32_t'(y - y0);
        s = fab_r32(h, sa + sy * sp + sx * 4); dp = j.dst + u32_t'(y) * j.dpitch + u32_t'(x) * 4;
        fab_w32(h, dp, (fl & SPF_OPAQUE) != 0 ? s : alpha_over(s, fab_r32(h, dp)));
      end
      vis = 64'(xb - xa) * 64'(yb - ya); spr_px += vis; cyc += vis * 3; sprites_drawn++;
      spr_fin = pmax(spr_fin, fab_reserve(h, sa, vis * 4, now));
    end
    spr_n = 64'(n); pixels += spr_px;
    spr_fin = pmax(spr_fin, fab_reserve(h, j.src, 64'(n) * SPRITE_ENTRY_SIZE, now));
    spr_fin = pmax(spr_fin, fab_reserve(h, j.dst, spr_px * 8, now));       // RMW do destino
    return cyc;
  endfunction

  function automatic longint unsigned execute(job_t j);
    u32_t W, H, sbpp, c, rr, gg, bb, d, s, o, a, ma, mb, mc, dp, ch; bit needs_src; u32_t src[];
    if (j.op == BOP_SPRITES) return exec_sprites(j);
    W = j.w; H = j.h; if (W == 0 || H == 0) return 1;
    sbpp = j.op == BOP_CONV565 ? 2 : 4;
    needs_src = j.op != BOP_FILL;
    if (!fab_ok(h, j.dst, (H - 1) * j.dpitch + W * 4)) begin error = 1; return 0; end
    if (needs_src && !fab_ok(h, j.src, (H - 1) * j.spitch + W * sbpp)) begin error = 1; return 0; end
    if (j.op == BOP_MINTERM && !fab_ok(h, j.mask, (H - 1) * j.mpitch + W * 4)) begin error = 1; return 0; end
    // Copia a origem inteira (semântica de memmove: rolagem com sobreposição funciona)
    if (needs_src) begin
      src = new[W * H];
      for (u32_t y = 0; y < H; y++) for (u32_t x = 0; x < W; x++) begin
        if (j.op == BOP_CONV565) begin
          c = 32'(fab_r16(h, j.src + y * j.spitch + x * 2)); rr = (c >> 11) & 31; gg = (c >> 5) & 63; bb = c & 31;
          src[y * W + x] = 32'hFF000000 | (((rr << 3) | (rr >> 2)) << 16) | (((gg << 2) | (gg >> 4)) << 8) | ((bb << 3) | (bb >> 2));
        end else src[y * W + x] = fab_r32(h, j.src + y * j.spitch + x * 4);
      end
    end
    for (u32_t y = 0; y < H; y++) for (u32_t x = 0; x < W; x++) begin
      dp = j.dst + y * j.dpitch + x * 4; d = fab_r32(h, dp); s = needs_src ? src[y * W + x] : 0; o = d;
      case (j.op)
        BOP_COPY, BOP_CONV565: o = s;
        BOP_FILL: o = j.color;
        BOP_COLORKEY: o = ((s & 32'hFFFFFF) == (j.color & 32'hFFFFFF)) ? d : s;
        BOP_ALPHA: o = alpha_over(s, d);
        BOP_MINTERM: begin
          a = fab_r32(h, j.mask + y * j.mpitch + x * 4); o = 0;
          for (int k = 0; k < 8; k++) if ((j.minterm & (32'd1 << k)) != 0) begin
            ma = (k & 4) != 0 ? a : ~a; mb = (k & 2) != 0 ? s : ~s; mc = (k & 1) != 0 ? d : ~d; o |= ma & mb & mc;
          end
        end
        default: begin error = 1; return 0; end
      endcase
      fab_w32(h, dp, o);
    end
    pixels += 64'(W) * 64'(H);
    ch = j.op == BOP_FILL ? 1 : j.op == BOP_MINTERM ? 3 : 2;
    return 64'(W) * 64'(H) * 64'(ch);
  endfunction
endmodule
