// FlameCore 3D em SystemVerilog: GPU de renderização em blocos (TBR, 32x32) com eliminação de superfícies ocultas (HSR) por
// bloco, rasterização com precisão sub-pixel 28.4 e regra top-left, interpolação com correção de perspectiva, 2 unidades de
// textura (14 formatos, mipmaps, bilinear), fog, blend com fatores próprios do alfa, stencil, alpha test e scissor.
// Mesmo comportamento de src/flamecore.cpp (que renderiza blocos em paralelo no host com resultado idêntico ao serial):
// as contas FP32 são da FPU do hospedeiro, na mesma ordem de operações do modelo de referência (cada lane dos vetores f4
// dele é uma conta escalar aqui). Fila de 4 listas de comandos; cada lista executa no kick e conclui após os ciclos (200 MHz)
// ou a banda de VRAM, o que for maior.
package sf_flamecore_pkg;
  import fabric::*; import sf100_abi::*;
  localparam int T = 32;                             // TILE
  localparam f32_t F_16 = 32'h41800000, F_256 = 32'h43800000, F_8000 = 32'h45fa0000;
  typedef struct packed { bit valid; u32_t addr; int lw, lh, fmt, flags, nlevels; logic [15:0][31:0] off; } tex_t;
  typedef struct packed {
    u32_t flags, bsrc, bdst, basrc, badst, fog_color; f32_t fog_start, fog_end;
    u32_t sfunc, sref, smask, sfail, zfail, zpass;
    logic [1:0][31:0] texenv; logic [1:0][31:0] uvscale; tex_t tex1, tex0; int pal_base;
  } rstate_t;
  typedef struct packed {
    int state; int x0, x1, x2, y0, y1, y2; longint area; logic [63:0] inv; int minx, miny, maxx, maxy;
    logic [2:0][31:0] z, rhw, fog; logic [11:0][31:0] col, uv;      // col/uv: vértice k, lane i em [k*4+i]
    logic [1:0][31:0] tbase, tw, th, twf, thf, tuvs;                  // amostrador por unidade (nível de mip resolvido)
  } tri_t;
  typedef struct packed { longint unsigned tris_in, tris_drawn, pix_covered, pix_shaded, tiles, cycles, texfetch, vram_write, bin_entries; } stats_t;
  typedef struct packed { u32_t addr, n; bit irq; } job_t;

  // O núcleo é uma classe: os métodos viram funções C++ (funções de módulo seriam expandidas em linha em cada chamada,
  // e o caminho do pixel shade -> sample -> texel aparece em vários pontos).
  class fc_core;


  chandle h; int dev;
  function new(); endfunction
  u32_t reg_addr, reg_len; bit running, error; job_t jq[$]; longint unsigned lists_done;
  stats_t st, last, total;
  // estado da lista de comandos
  rstate_t cur; u32_t cur_pal[2][256]; bit state_dirty; rstate_t states[$]; u32_t pal_store[$]; tri_t tris[$];
  u32_t vbase, rt_addr, rt_pitch; int rt_w, rt_h, scx, scy, scw, sch; u32_t clr_argb, clr_flags; f32_t clr_depth;
  // bloco em renderização
  u32_t tcol[T * T]; f32_t tz[T * T]; byte unsigned tst[T * T]; int tvis[T * T]; int tox, toy;
  int bin_items[];                                   // triângulos de cada bloco (CSR: bloco b em [bin_start[b], bin_start[b+1]))
  f32_t u8f[256];


  function void gpu_bind(chandle hh, int d);
    h = hh; dev = d; reg_addr = 0; reg_len = 0; running = 0; error = 0; jq.delete(); lists_done = 0; st = '0; last = '0; total = '0;
    for (int i = 0; i < 256; i++) u8f[i] = f32_div(f32_from_i32(i), F32_255);   // u8 -> float como (v / 255.0f)
  endfunction
  function bit hsr(); return fab_in(h, dev, 0) != 0; endfunction       // GPU_HSR: configuração do frontend (campo público)

  // Espelhos: 0..8 last (tris_in..bin_entries), 16..24 total, 32 error, 33 lists_done, 34 running
  function void obs();
    stats_t a[2]; a[0] = last; a[1] = total;
    for (int k = 0; k < 2; k++) begin
      fab_obs(h, dev, 16 * k + 0, a[k].tris_in); fab_obs(h, dev, 16 * k + 1, a[k].tris_drawn); fab_obs(h, dev, 16 * k + 2, a[k].pix_covered);
      fab_obs(h, dev, 16 * k + 3, a[k].pix_shaded); fab_obs(h, dev, 16 * k + 4, a[k].tiles); fab_obs(h, dev, 16 * k + 5, a[k].cycles);
      fab_obs(h, dev, 16 * k + 6, a[k].texfetch); fab_obs(h, dev, 16 * k + 7, a[k].vram_write); fab_obs(h, dev, 16 * k + 8, a[k].bin_entries);
    end
    fab_obs(h, dev, 32, 64'(error)); fab_obs(h, dev, 33, lists_done); fab_obs(h, dev, 34, 64'(running));
  endfunction
  function void fail(string m); error = 1; fab_msg(h, dev, m); endfunction

  // ---------------- Fila / registradores ----------------
  function bit gpu_submit(u32_t addr, u32_t n, bit irq);
    job_t j;
    if (jq.size() + (running ? 1 : 0) >= GPU_QUEUE_DEPTH) return 0;
    j.addr = addr; j.n = n; j.irq = irq; jq.push_back(j);
    if (!running) kick();
    obs(); return 1;
  endfunction
  function void kick();
    job_t j; ps_t dur, now, fin; longint unsigned c;
    if (jq.size() == 0) begin running = 0; return; end
    j = jq.pop_front(); running = 1;
    gpu_execute(j.addr, j.n);
    c = st.cycles > 1 ? st.cycles : 1;
    dur = ps_t'((128'(c) * 128'(64'd1000000000000)) / 128'(200000000));
    now = fab_now(h); fin = fab_reserve(h, VRAM_BASE, st.vram_write + st.texfetch, now); if (fin - now > dur) dur = fin - now;
    fab_at(h, dev, dur, 0, 64'(j.irq), 0);
  endfunction
  function void gpu_event(int tag, longint unsigned irq, longint unsigned unused);
    running = 0; lists_done++;
    if (irq != 0) fab_irq(h, IRQ_GPU);
    kick(); obs();
  endfunction

  function u32_t gpu_read(u32_t off);
    case (off)
      GPU_CMD_ADDR: return reg_addr; GPU_CMD_LEN: return reg_len;
      GPU_STATUS: return (running ? GPUS_BUSY : 0) | ((lists_done != 0 && !running && jq.size() == 0) ? GPUS_DONE : 0) | (error ? GPUS_ERROR : 0);
      GPU_HSR: return hsr() ? 1 : 0;
      GPU_STAT_TRIS_IN: return u32_t'(last.tris_in); GPU_STAT_TRIS_DRAWN: return u32_t'(last.tris_drawn);
      GPU_STAT_PIX_COVERED: return u32_t'(last.pix_covered); GPU_STAT_PIX_SHADED: return u32_t'(last.pix_shaded);
      GPU_STAT_TILES: return u32_t'(last.tiles); GPU_STAT_CYCLES: return u32_t'(last.cycles);
      GPU_STAT_TEXFETCH: return u32_t'(last.texfetch); GPU_STAT_VRAM_WRITE: return u32_t'(last.vram_write);
      default: return 0;
    endcase
  endfunction
  function void gpu_write(u32_t off, u32_t v);
    case (off)
      GPU_CMD_ADDR: reg_addr = v; GPU_CMD_LEN: reg_len = v;
      GPU_HSR: fab_out(h, dev, 0, 64'(v & 1));
      GPU_STATUS: if ((v & GPUS_ERROR) != 0) error = 0;
      GPU_CTRL: if ((v & 1) != 0) begin if (!gpu_submit(reg_addr, reg_len, (v & 2) != 0)) fail("fila cheia"); end
      default: ;
    endcase
    obs();
  endfunction

  // ---------------- Utilitários ----------------
  function int clampi(int v, int lo, int hi); return v < lo ? lo : v > hi ? hi : v; endfunction
  function f32_t clampf(f32_t v, f32_t lo, f32_t hi); return f32_lt(v, lo) ? lo : f32_gt(v, hi) ? hi : v; endfunction
  function int imax(int a, int b); return a > b ? a : b; endfunction
  function int imin(int a, int b); return a < b ? a : b; endfunction
  function u32_t pack_argb(u32_t a, u32_t r, u32_t g, u32_t b); return (a << 24) | (r << 16) | (g << 8) | b; endfunction
  function void from565(u32_t c, output u32_t r, output u32_t g, output u32_t b);
    r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31;
    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
  endfunction
  function u32_t level_bytes(int fmt, int w, int hh);
    if (fmt == 0) return u32_t'(w * hh * 4);
    if (fmt == 3) return u32_t'(imax(1, (w + 3) / 4) * imax(1, (hh + 3) / 4) * 8);
    if (fmt == 4 || fmt == 5) return u32_t'(imax(1, (w + 3) / 4) * imax(1, (hh + 3) / 4) * 16);
    if (fmt == 6 || fmt == 9) return u32_t'(w * hh * 2);
    if (fmt == 7 || fmt == 8 || fmt == 10) return u32_t'(w * hh);
    if (fmt == 11) return u32_t'(((w + 1) / 2) * hh);
    if (fmt == 12) return 2048 + u32_t'(imax(1, (w + 1) / 2) * imax(1, (hh + 1) / 2));
    if (fmt == 13) return u32_t'(imax(1, (w + 3) / 4) * imax(1, (hh + 3) / 4) * 8);
    return u32_t'(w * hh * 2);
  endfunction
  function bit dfunc(u32_t f, f32_t z, f32_t c);
    case (f) 0: return f32_lt(z, c); 1: return f32_le(z, c); 2: return f32_gt(z, c); 3: return f32_le(c, z);
             4: return f32_eq(z, c); 5: return !f32_eq(z, c); 6: return 1; default: return 0; endcase
  endfunction
  function bit sfunc(u32_t f, u32_t r, u32_t v);
    case (f) 0: return r < v; 1: return r <= v; 2: return r > v; 3: return r >= v; 4: return r == v; 5: return r != v; 6: return 1; default: return 0; endcase
  endfunction
  function byte unsigned stencil_op(byte unsigned sv, u32_t op, u32_t r);
    case (op) SOP_ZERO: return 0; SOP_REPLACE: return r[7:0]; SOP_INCR: return sv < 255 ? sv + 1 : sv;
              SOP_DECR: return sv > 0 ? sv - 1 : sv; SOP_INVERT: return ~sv; default: return sv; endcase
  endfunction

  // ---------------- Execução da lista ----------------
  function void gpu_execute(u32_t addr, u32_t nwords);   // resultado em 'last' (espelhado)
    u32_t p, e, w, op, np, pb, unit, pk, o, pa, n, ia, cnt, prim, first; tex_t t; logic [31:0] P[8];
    cur = '0; cur.bsrc = BF_ONE; cur.bdst = BF_ZERO; cur.basrc = BF_ONE; cur.badst = BF_ZERO; cur.fog_end = F32_1;
    cur.sfunc = 6; cur.smask = 32'hFF; cur.uvscale[0] = F32_1; cur.uvscale[1] = F32_1; cur.tex0.nlevels = 1; cur.tex1.nlevels = 1;
    for (int u = 0; u < 2; u++) for (int i = 0; i < 256; i++) cur_pal[u][i] = 0;
    state_dirty = 1; states.delete(); pal_store.delete(); tris.delete(); st = '0;
    rt_addr = 0; rt_pitch = 0; rt_w = 0; rt_h = 0; clr_argb = 0; clr_depth = F32_1; clr_flags = 0;
    vbase = 0; scx = 0; scy = 0; scw = 65535; sch = 65535; error = 0; fab_msg(h, dev, "");
    p = addr; e = addr + nwords * 4;
    if (!fab_ok(h, addr, nwords * 4)) begin fail("lista de comandos fora da memória"); last = st; obs(); return; end
    while (p < e && !error) begin
      w = fab_r32(h, p); op = w >> 24; np = w & 32'hFFFFFF;
      if (p + 4 + np * 4 > e) begin fail("comando truncado"); break; end
      pb = p;
      for (int i = 0; i < 8; i++) P[i] = u32_t'(i) < np ? fab_r32(h, pb + 4 + u32_t'(i) * 4) : 0;   // parâmetros (no máximo 6 usados)
      p += 4 + np * 4;
      case (op)
        GC_END: begin flush(); p = e; end
        GC_SET_RT: begin
          if (np < 3) begin fail("SET_RT"); break; end
          rt_addr = P[0]; rt_pitch = P[1]; rt_w = int'(P[2] & 32'hFFFF); rt_h = int'(P[2] >> 16);
          if (!fab_ok(h, rt_addr, rt_pitch * u32_t'(rt_h)) || rt_pitch < u32_t'(rt_w) * 4) fail("render target inválido");
        end
        GC_CLEAR: begin if (np < 3) begin fail("CLEAR"); break; end clr_argb = P[0]; clr_depth = P[1]; clr_flags = P[2]; end
        GC_VERTEX_BASE: begin if (np < 1) begin fail("VERTEX_BASE"); break; end vbase = P[0]; end
        GC_SET_TEX: begin
          if (np < 4) begin fail("SET_TEX"); break; end
          unit = P[0]; if (unit > 1) begin fail("unidade de textura"); break; end
          t = '0; t.addr = P[1]; pk = P[2]; t.lw = int'(pk & 255); t.lh = int'((pk >> 8) & 255); t.fmt = int'((pk >> 16) & 255); t.flags = int'(pk >> 24);
          t.nlevels = (t.flags & TFLAG_MIPS) != 0 ? clampi(int'(P[3]), 1, 12) : 1;
          if (t.lw > 11 || t.lh > 11 || t.fmt >= int'(TF_COUNT)) begin fail("textura inválida"); break; end
          o = 0;
          for (int l = 0; l < t.nlevels; l++) begin t.off[l] = o; o += level_bytes(t.fmt, imax(1, (1 << t.lw) >> l), imax(1, (1 << t.lh) >> l)); end
          if (!fab_ok(h, t.addr, o)) begin fail("textura fora da memória"); break; end
          t.valid = 1; if (unit == 0) cur.tex0 = t; else cur.tex1 = t; state_dirty = 1;
        end
        GC_SET_PALETTE: begin
          if (np < 3 || P[0] > 1 || P[2] > 256) begin fail("SET_PALETTE"); break; end
          unit = P[0]; pa = P[1]; n = P[2];
          if (!fab_ok(h, pa, n * 4)) begin fail("paleta fora da memória"); break; end
          for (u32_t i = 0; i < n; i++) cur_pal[unit][i] = fab_r32(h, pa + i * 4);
          state_dirty = 1;
        end
        GC_STATE: begin if (np < 1) begin fail("STATE"); break; end cur.flags = P[0]; state_dirty = 1; end
        GC_BLEND: begin
          if (np < 2) begin fail("BLEND"); break; end
          cur.bsrc = P[0]; cur.bdst = P[1]; cur.basrc = np >= 4 ? P[2] : P[0]; cur.badst = np >= 4 ? P[3] : P[1]; state_dirty = 1;   // 4 params: fatores do alfa
        end
        GC_FOG: begin if (np < 3) begin fail("FOG"); break; end cur.fog_color = P[0]; cur.fog_start = P[1]; cur.fog_end = P[2]; state_dirty = 1; end
        GC_STENCIL: begin
          if (np < 6) begin fail("STENCIL"); break; end
          cur.sfunc = P[0]; cur.sref = P[1]; cur.smask = P[2]; cur.sfail = P[3]; cur.zfail = P[4]; cur.zpass = P[5]; state_dirty = 1;
        end
        GC_TEXENV: begin
          if (np < 2 || P[0] > 1) begin fail("TEXENV"); break; end
          cur.texenv[P[0][0]] = P[1]; cur.uvscale[P[0][0]] = np >= 3 ? P[2] : F32_1; state_dirty = 1;
        end
        GC_SCISSOR: begin if (np < 4) begin fail("SCISSOR"); break; end scx = int'(P[0]); scy = int'(P[1]); scw = int'(P[2]); sch = int'(P[3]); end
        GC_DRAW_INDEXED: begin
          if (np < 3) begin fail("DRAW_INDEXED"); break; end
          ia = P[0]; cnt = P[1]; prim = P[2];
          if (!fab_ok(h, ia, cnt * 2)) begin fail("índices fora da memória"); break; end
          draw(prim, cnt, 1, ia);
        end
        GC_DRAW_ARRAYS: begin
          if (np < 3) begin fail("DRAW_ARRAYS"); break; end
          first = P[0]; cnt = P[1]; prim = P[2];
          draw(prim, cnt, 0, first);
        end
        GC_FLUSH: flush();
        default: fail($sformatf("comando desconhecido %0d", op));
      endcase
    end
    if (!error) flush();
    begin
      longint unsigned compute, membytes, mem;
      compute = st.tris_drawn * 12 + st.pix_shaded / 2 + st.pix_covered / 8 + st.tiles * 40;
      if (st.texfetch / 4 > compute) compute = st.texfetch / 4;   // 4 TMUs: 1 texel por ciclo cada
      membytes = st.vram_write + st.texfetch * 4 / 4;
      mem = membytes * 1000 / 26560;                                // 5,31 GB/s a 200 MHz ~ 26,56 B/ciclo
      st.cycles = compute > mem ? compute : mem;
    end
    last = st;
    total.tris_in += st.tris_in; total.tris_drawn += st.tris_drawn; total.pix_covered += st.pix_covered;
    total.pix_shaded += st.pix_shaded; total.tiles += st.tiles; total.cycles += st.cycles; total.texfetch += st.texfetch;
    total.vram_write += st.vram_write; total.bin_entries += st.bin_entries;
    obs();
  endfunction

  function int cur_state();
    rstate_t s;
    if (state_dirty) begin
      s = cur; s.pal_base = pal_store.size();
      for (int u = 0; u < 2; u++) for (int i = 0; i < 256; i++) pal_store.push_back(cur_pal[u][i]);
      states.push_back(s); state_dirty = 0;
    end
    return states.size() - 1;
  endfunction

  // índice i da primitiva: indexado (DRAW_INDEXED, u16 em base+2i) ou sequencial (DRAW_ARRAYS, base+i)
  function u32_t index_of(bit indexed, u32_t base, u32_t i);
    return indexed ? u32_t'(fab_r16(h, base + i * 2)) : base + i;
  endfunction

  // Vértice pós-transformação (48 B): pos(x,y,z,1/w), cor(r,g,b,a), tex(u,v,fog,v1); u1 = fog. vx[0..12]: x y z rhw r g b a u v fog u1 v1
  function void load_vertex(u32_t idx, output f32_t vx[13]);
    u32_t a;
    a = vbase + idx * VERTEX_SIZE;
    for (int i = 0; i < 13; i++) vx[i] = 0;
    if (!fab_ok(h, a, VERTEX_SIZE)) begin fail("vértice fora da memória"); return; end
    for (int i = 0; i < 11; i++) vx[i] = fab_r32(h, a + u32_t'(i) * 4);
    vx[11] = vx[10]; vx[12] = fab_r32(h, a + 44);
  endfunction

  function void draw(u32_t prim, u32_t count, bit indexed, u32_t base);
    f32_t a[13], b[13], c[13];
    if (prim == PRIM_TRIS) begin
      for (u32_t i = 0; i + 2 < count && !error; i += 3) begin
        load_vertex(index_of(indexed, base, i + 2), c); load_vertex(index_of(indexed, base, i + 1), b); load_vertex(index_of(indexed, base, i), a);
        emit(a, b, c);
      end
    end else if (prim == PRIM_STRIP) begin
      for (u32_t i = 0; i + 2 < count && !error; i++) begin
        load_vertex(index_of(indexed, base, i + 2), c);
        if ((i & 1) != 0) begin load_vertex(index_of(indexed, base, i), b); load_vertex(index_of(indexed, base, i + 1), a); end
        else begin load_vertex(index_of(indexed, base, i + 1), b); load_vertex(index_of(indexed, base, i), a); end
        emit(a, b, c);
      end
    end else if (prim == PRIM_FAN) begin
      for (u32_t i = 1; i + 1 < count && !error; i++) begin
        load_vertex(index_of(indexed, base, i + 1), c); load_vertex(index_of(indexed, base, i), b); load_vertex(index_of(indexed, base, 0), a);
        emit(a, b, c);
      end
    end else fail("primitiva inválida");
  endfunction

  function void emit(input f32_t a[13], input f32_t b[13], input f32_t c[13]);
    f32_t v[3][13], tmp[13]; int X[3], Y[3], ti, si, mnx, mxx, mny, mxy, level; longint area; rstate_t s; tri_t t; tex_t tx;
    f32_t sc, uvpar, pixpar, lod, lods[2]; real inv;
    st.tris_in++;
    v[0] = a; v[1] = b; v[2] = c;
    for (int k = 0; k < 3; k++) begin
      if (!f32_gt(v[k][3], F32_0) || !f32_finite(v[k][0]) || !f32_finite(v[k][1]) || !f32_finite(v[k][2])) return;   // atrás da câmera / inválido
      if (f32_gt(f32_abs(v[k][0]), F_8000) || f32_gt(f32_abs(v[k][1]), F_8000)) return;                            // guard-band
    end
    si = cur_state(); s = states[si];
    for (int k = 0; k < 3; k++) begin X[k] = f32_lround(f32_mul(v[k][0], F_16)); Y[k] = f32_lround(f32_mul(v[k][1], F_16)); end
    area = longint'(X[1] - X[0]) * longint'(Y[2] - Y[0]) - longint'(Y[1] - Y[0]) * longint'(X[2] - X[0]);
    if (area == 0) return;
    if (area > 0 && (s.flags & ST_CULL_CW) != 0) return;
    if (area < 0) begin
      if ((s.flags & ST_CULL_CCW) != 0) return;
      tmp = v[1]; v[1] = v[2]; v[2] = tmp; ti = X[1]; X[1] = X[2]; X[2] = ti; ti = Y[1]; Y[1] = Y[2]; Y[2] = ti; area = -area;
    end
    t = '0; t.state = si; t.area = area; t.x0 = X[0]; t.x1 = X[1]; t.x2 = X[2]; t.y0 = Y[0]; t.y1 = Y[1]; t.y2 = Y[2];
    mnx = imin(X[0], imin(X[1], X[2])); mxx = imax(X[0], imax(X[1], X[2])); mny = imin(Y[0], imin(Y[1], Y[2])); mxy = imax(Y[0], imax(Y[1], Y[2]));
    t.minx = imax(mnx >>> 4, imax(0, scx)); t.maxx = imin((mxx + 15) >>> 4, imin(rt_w - 1, scx + scw - 1));
    t.miny = imax(mny >>> 4, imax(0, scy)); t.maxy = imin((mxy + 15) >>> 4, imin(rt_h - 1, scy + sch - 1));
    if (t.minx > t.maxx || t.miny > t.maxy) return;
    // LOD por triângulo (nível de mip mais próximo)
    for (int u = 0; u < 2; u++) begin
      lods[u] = F32_0; tx = u == 0 ? s.tex0 : s.tex1;
      if (tx.valid && tx.nlevels > 1) begin
        sc = s.uvscale[u];
        uvpar = f32_mul(f32_mul(f32_mul(f32_mul(f32_abs(f32_sub(f32_mul(f32_sub(v[1][8], v[0][8]), f32_sub(v[2][9], v[0][9])),
                                                              f32_mul(f32_sub(v[2][8], v[0][8]), f32_sub(v[1][9], v[0][9])))), sc), sc),
                                        f32_from_i32(1 << tx.lw)), f32_from_i32(1 << tx.lh));
        pixpar = f32_div(f64_to_f32(real'(area)), F_256);
        lod = f32_gt(uvpar, F32_0) ? f32_mul(F32_HALF, f32_log2(f32_div(uvpar, pixpar))) : F32_0;
        lods[u] = clampf(lod, F32_0, f32_from_i32(tx.nlevels - 1));
      end
    end
    inv = 1.0 / real'(area); t.inv = $realtobits(inv);
    for (int k = 0; k < 3; k++) begin
      t.z[k] = v[k][2]; t.rhw[k] = v[k][3]; t.fog[k] = v[k][10];
      t.col[k * 4 + 0] = v[k][4]; t.col[k * 4 + 1] = v[k][5]; t.col[k * 4 + 2] = v[k][6]; t.col[k * 4 + 3] = v[k][7];
      t.uv[k * 4 + 0] = v[k][8]; t.uv[k * 4 + 1] = v[k][9]; t.uv[k * 4 + 2] = v[k][11]; t.uv[k * 4 + 3] = v[k][12];
    end
    for (int u = 0; u < 2; u++) begin
      tx = u == 0 ? s.tex0 : s.tex1;
      t.tbase[u] = 0; t.tw[u] = 1; t.th[u] = 1; t.twf[u] = F32_1; t.thf[u] = F32_1; t.tuvs[u] = F32_1;
      if (!tx.valid) continue;
      level = tx.nlevels > 1 ? clampi(f32_to_i32(f32_add(lods[u], F32_HALF)), 0, tx.nlevels - 1) : 0;
      t.tw[u] = imax(1, (1 << tx.lw) >> level); t.th[u] = imax(1, (1 << tx.lh) >> level);
      t.twf[u] = f32_from_i32(int'(t.tw[u])); t.thf[u] = f32_from_i32(int'(t.th[u]));
      t.tbase[u] = tx.addr + tx.off[level]; t.tuvs[u] = s.uvscale[u];
    end
    st.tris_drawn++;
    tris.push_back(t);
  endfunction

  // ---------------- Rasterização ----------------
  function bit cover_px(tri_t t, int px, int py, output f32_t l0, output f32_t l1, output f32_t l2);
    longint sx, sy, e0, e1, e2; real inv; int x0, x1, x2, y0, y1, y2;
    x0 = t.x0; x1 = t.x1; x2 = t.x2; y0 = t.y0; y1 = t.y1; y2 = t.y2;     // campos do struct empacotado -> int com sinal
    l0 = F32_0; l1 = F32_0; l2 = F32_0;
    sx = longint'(px) * 16 + 8; sy = longint'(py) * 16 + 8;
    e0 = longint'(x2 - x1) * (sy - longint'(y1)) - longint'(y2 - y1) * (sx - longint'(x1));
    e1 = longint'(x0 - x2) * (sy - longint'(y2)) - longint'(y0 - y2) * (sx - longint'(x2));
    e2 = longint'(x1 - x0) * (sy - longint'(y0)) - longint'(y1 - y0) * (sx - longint'(x0));
    if (e0 < 0 || (e0 == 0 && !topleft(x2 - x1, y2 - y1))) return 0;
    if (e1 < 0 || (e1 == 0 && !topleft(x0 - x2, y0 - y2))) return 0;
    if (e2 < 0 || (e2 == 0 && !topleft(x1 - x0, y1 - y0))) return 0;
    inv = $bitstoreal(t.inv);
    l0 = f64_to_f32(real'(e0) * inv); l1 = f64_to_f32(real'(e1) * inv); l2 = f64_to_f32(real'(e2) * inv);
    return 1;
  endfunction
  function bit topleft(int dx, int dy); return (dy == 0 && dx > 0) || dy < 0; endfunction

  function f32_t zof(tri_t t, f32_t l0, f32_t l1, f32_t l2);
    return f32_add(f32_add(f32_mul(l0, t.z[0]), f32_mul(l1, t.z[1])), f32_mul(l2, t.z[2]));
  endfunction

  // Percorre os pixels cobertos de [x0,x1]x[y0,y1]: modo 0 = imediato (imm_tri), 1 = passada de profundidade do HSR (bk = índice
  // do triângulo). Funções de aresta avançadas por incremento ao longo da linha; a linha termina ao sair do triângulo.
  function void raster(tri_t t, int x0, int x1, int y0, int y1, int mode, int bk, rstate_t s, inout longint unsigned covered,
                                 inout longint unsigned shaded);
    int ax[3], ay[3], bx[3], by[3]; bit tlf[3], was_in, inn; longint e[3], stp[3], ddx[3], ddy[3], sy, sx; real inv; f32_t l[3];
    ax = '{t.x1, t.x2, t.x0}; ay = '{t.y1, t.y2, t.y0}; bx = '{t.x2, t.x0, t.x1}; by = '{t.y2, t.y0, t.y1};
    for (int k = 0; k < 3; k++) begin
      ddx[k] = longint'(bx[k]) - longint'(ax[k]); ddy[k] = longint'(by[k]) - longint'(ay[k]);
      tlf[k] = (ddy[k] == 0 && ddx[k] > 0) || ddy[k] < 0; stp[k] = -ddy[k] * 16;
    end
    inv = $bitstoreal(t.inv);
    for (int py = y0; py <= y1; py++) begin
      sy = longint'(py) * 16 + 8; sx = longint'(x0) * 16 + 8; was_in = 0;
      for (int k = 0; k < 3; k++) e[k] = ddx[k] * (sy - longint'(ay[k])) - ddy[k] * (sx - longint'(ax[k]));
      for (int px = x0; px <= x1; px++) begin
        inn = (e[0] > 0 || (e[0] == 0 && tlf[0])) && (e[1] > 0 || (e[1] == 0 && tlf[1])) && (e[2] > 0 || (e[2] == 0 && tlf[2]));
        if (!inn) begin if (was_in) break; e[0] += stp[0]; e[1] += stp[1]; e[2] += stp[2]; continue; end
        was_in = 1;
        for (int k = 0; k < 3; k++) l[k] = f64_to_f32(real'(e[k]) * inv);
        if (mode == 0) pixel_imm(t, s, px, py, l[0], l[1], l[2], covered, shaded);
        else pixel_hsr(t, s, px, py, l[0], l[1], l[2], bk, covered);
        e[0] += stp[0]; e[1] += stp[1]; e[2] += stp[2];
      end
    end
  endfunction

  function void pixel_hsr(tri_t t, rstate_t s, int px, int py, f32_t l0, f32_t l1, f32_t l2, int bk, inout longint unsigned covered);
    int i; f32_t z;
    covered++;
    i = (py - toy) * T + (px - tox);
    z = zof(t, l0, l1, l2); if (f32_lt(z, F32_0) || f32_gt(z, F32_1)) return;
    if (dfunc((s.flags >> ST_DEPTH_FUNC_SHIFT) & 7, z, tz[i])) begin tz[i] = z; tvis[i] = bk; end
  endfunction

  function void pixel_imm(tri_t t, rstate_t s, int px, int py, f32_t l0, f32_t l1, f32_t l2, inout longint unsigned covered,
                                    inout longint unsigned shaded);
    int i; f32_t z; u32_t f, df, src; bit stencil, dtest, dwrite, blend, atest, dp;
    f = s.flags; df = (f >> ST_DEPTH_FUNC_SHIFT) & 7;
    stencil = (f & ST_STENCIL) != 0; dtest = (f & ST_DEPTH_TEST) != 0; dwrite = dtest && (f & ST_DEPTH_WRITE) != 0;
    blend = (f & ST_BLEND) != 0; atest = (f & ST_ATEST) != 0;
    covered++;
    i = (py - toy) * T + (px - tox);
    z = zof(t, l0, l1, l2); if (f32_lt(z, F32_0) || f32_gt(z, F32_1)) return;
    if (stencil && !sfunc(s.sfunc, s.sref & s.smask, u32_t'(tst[i]) & s.smask)) begin tst[i] = stencil_op(tst[i], s.sfail, s.sref); return; end
    dp = !dtest || dfunc(df, z, tz[i]);
    if (!dp) begin if (stencil) tst[i] = stencil_op(tst[i], s.zfail, s.sref); return; end
    src = shade(t, s, l0, l1, l2); shaded++;
    if (atest && (src >> 24) < 128) return;          // alpha test: descartado antes de Z/stencil/cor
    if (stencil) tst[i] = stencil_op(tst[i], s.zpass, s.sref);
    if (dwrite) tz[i] = z;
    tcol[i] = blend ? blend_px(src, tcol[i], s.bsrc, s.bdst, s.basrc, s.badst) : src;
  endfunction

  // ---------------- Texturas ----------------
  function u32_t etc1_texel(u32_t b, int x, int y);
    int tbl[8][2]; u32_t hi, lo; bit diff, flip; int cw[2], c[2][3], sh, b1, d, b2, sub, k, msb, lsb, md; u32_t o;
    tbl = '{'{2, 8}, '{5, 17}, '{9, 29}, '{13, 42}, '{18, 60}, '{24, 80}, '{33, 106}, '{47, 183}};
    hi = fab_r32(h, b); lo = fab_r32(h, b + 4); diff = hi[1]; flip = hi[0]; cw[0] = int'((hi >> 5) & 7); cw[1] = int'((hi >> 2) & 7);
    for (int q = 0; q < 3; q++) begin
      if (!diff) begin sh = 28 - 8 * q; c[0][q] = int'((hi >> sh) & 15) * 17; c[1][q] = int'((hi >> (sh - 4)) & 15) * 17; end
      else begin
        sh = 27 - 8 * q; b1 = int'((hi >> sh) & 31); d = int'((hi >> (sh - 3)) & 7); if ((d & 4) != 0) d -= 8; b2 = imin(31, imax(0, b1 + d));
        c[0][q] = (b1 << 3) | (b1 >> 2); c[1][q] = (b2 << 3) | (b2 >> 2);
      end
    end
    sub = flip ? int'(y >= 2) : int'(x >= 2); k = x * 4 + y; msb = int'((lo >> (16 + k)) & 1); lsb = int'((lo >> k) & 1);
    md = tbl[cw[sub]][lsb]; if (msb != 0) md = -md;
    o = 32'hFF000000;
    for (int q = 0; q < 3; q++) o |= u32_t'(clampi(c[sub][q] + md, 0, 255)) << (16 - 8 * q);
    return o;
  endfunction

  function u32_t texel(tex_t t, u32_t base, int w, int hh, int pal, int x, int y);
    u32_t c, r, g, b, alpha, a0, a1, idx, c0, c1, blk, r0, g0, b0, r1, g1, b1; longint unsigned bits; int bw, code, l; bit two;
    if ((t.flags & TFLAG_CLAMP) != 0) begin x = clampi(x, 0, w - 1); y = clampi(y, 0, hh - 1); end else begin x &= w - 1; y &= hh - 1; end
    case (t.fmt)
      TF_ARGB8888: return fab_r32(h, base + u32_t'(y * w + x) * 4);
      TF_RGB565: begin c = 32'(fab_r16(h, base + u32_t'(y * w + x) * 2)); from565(c, r, g, b); return pack_argb(255, r, g, b); end
      TF_ARGB4444: begin c = 32'(fab_r16(h, base + u32_t'(y * w + x) * 2)); return pack_argb(((c >> 12) & 15) * 17, ((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17); end
      TF_RGBA5551: begin
        c = 32'(fab_r16(h, base + u32_t'(y * w + x) * 2)); r = (c >> 11) & 31; g = (c >> 6) & 31; b = (c >> 1) & 31;
        return pack_argb((c & 1) * 255, (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2));
      end
      TF_L8: begin l = int'(fab_r8(h, base + u32_t'(y * w + x))); return pack_argb(255, u32_t'(l), u32_t'(l), u32_t'(l)); end
      TF_A8: return pack_argb(32'(fab_r8(h, base + u32_t'(y * w + x))), 255, 255, 255);
      TF_LA88: begin c = 32'(fab_r16(h, base + u32_t'(y * w + x) * 2)); return pack_argb(c >> 8, c & 255, c & 255, c & 255); end
      TF_CI8: begin l = pal + int'(fab_r8(h, base + u32_t'(y * w + x))); return pal_store[l]; end
      TF_CI4: begin c = 32'(fab_r8(h, base + u32_t'(y * ((w + 1) / 2) + (x >>> 1)))); l = pal + int'((x & 1) != 0 ? (c & 15) : (c >> 4)); return pal_store[l]; end
      TF_VQ: begin
        bw = imax(1, (w + 1) / 2); idx = 32'(fab_r8(h, base + 2048 + u32_t'((y >>> 1) * bw + (x >>> 1))));
        c = 32'(fab_r16(h, base + idx * 8 + u32_t'(((y & 1) * 2) + (x & 1)) * 2)); from565(c, r, g, b); return pack_argb(255, r, g, b);
      end
      TF_ETC1: begin bw = imax(1, (w + 3) / 4); return etc1_texel(base + u32_t'((y >>> 2) * bw + (x >>> 2)) * 8, x & 3, y & 3); end
      default: begin                                  // DXT1 (8 B/bloco), DXT3/DXT5 (16 B/bloco: alfa + cor)
        bw = imax(1, (w + 3) / 4); two = t.fmt != int'(TF_DXT1);
        blk = base + u32_t'((y >>> 2) * bw + (x >>> 2)) * (two ? 16 : 8);
        alpha = 255;
        if (t.fmt == int'(TF_DXT3)) alpha = ((32'(fab_r16(h, blk + u32_t'(2 * (y & 3)))) >> (4 * (x & 3))) & 15) * 17;
        else if (t.fmt == int'(TF_DXT5)) begin
          a0 = 32'(fab_r8(h, blk)); a1 = 32'(fab_r8(h, blk + 1)); bits = 0;
          for (int k = 0; k < 6; k++) bits = (bits << 8) | 64'(fab_r8(h, blk + 2 + u32_t'(k)));
          idx = u32_t'(bits >> (3 * ((y & 3) * 4 + (x & 3)))) & 7;
          if (idx == 0) alpha = a0; else if (idx == 1) alpha = a1;
          else if (a0 > a1) alpha = ((8 - idx) * a0 + (idx - 1) * a1) / 7;
          else if (idx < 6) alpha = ((6 - idx) * a0 + (idx - 1) * a1) / 5;
          else alpha = idx == 6 ? 0 : 255;
        end
        if (two) blk += 8;
        c0 = 32'(fab_r16(h, blk)); c1 = 32'(fab_r16(h, blk + 2)); code = int'((fab_r8(h, blk + 4 + u32_t'(y & 3)) >> (2 * (x & 3))) & 3);
        from565(c0, r0, g0, b0); from565(c1, r1, g1, b1);
        if (c0 > c1 || two) begin                     // DXT3/5 sempre usam o modo de 4 cores
          case (code) 0: return pack_argb(alpha, r0, g0, b0); 1: return pack_argb(alpha, r1, g1, b1);
            2: return pack_argb(alpha, (2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3);
            default: return pack_argb(alpha, (r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3); endcase
        end
        case (code) 0: return pack_argb(255, r0, g0, b0); 1: return pack_argb(255, r1, g1, b1);
          2: return pack_argb(255, (r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2); default: return 0; endcase
      end
    endcase
  endfunction

  function void unpack4(u32_t c, output f32_t o[4]);
    o[0] = u8f[(c >> 16) & 255]; o[1] = u8f[(c >> 8) & 255]; o[2] = u8f[c & 255]; o[3] = u8f[c >> 24];
  endfunction
  function int ifloor(f32_t f); int i; i = f32_to_i32(f); return f32_gt(f32_from_i32(i), f) ? i - 1 : i; endfunction

  function void sample(tri_t tr, rstate_t s, int unit, f32_t u, f32_t v, output f32_t o[4]);
    tex_t t; f32_t fu, fv, tx, ty, ntx, nty, c00[4], c10[4], c01[4], c11[4]; int x0, y0, w, hh, pal;
    t = unit == 0 ? s.tex0 : s.tex1;
    if (!t.valid) begin o = '{F32_1, F32_1, F32_1, F32_1}; return; end
    w = int'(tr.tw[unit]); hh = int'(tr.th[unit]); pal = s.pal_base + 256 * unit;
    fu = f32_mul(f32_mul(u, tr.tuvs[unit]), tr.twf[unit]); fv = f32_mul(f32_mul(v, tr.tuvs[unit]), tr.thf[unit]);
    if ((s.flags & ST_BILINEAR) != 0) begin
      fu = f32_sub(fu, F32_HALF); fv = f32_sub(fv, F32_HALF); x0 = ifloor(fu); y0 = ifloor(fv);
      tx = f32_sub(fu, f32_from_i32(x0)); ty = f32_sub(fv, f32_from_i32(y0));
      unpack4(texel(t, tr.tbase[unit], w, hh, pal, x0, y0), c00); unpack4(texel(t, tr.tbase[unit], w, hh, pal, x0 + 1, y0), c10);
      unpack4(texel(t, tr.tbase[unit], w, hh, pal, x0, y0 + 1), c01); unpack4(texel(t, tr.tbase[unit], w, hh, pal, x0 + 1, y0 + 1), c11);
      st.texfetch += 4;
      ntx = f32_sub(F32_1, tx); nty = f32_sub(F32_1, ty);
      for (int i = 0; i < 4; i++)
        o[i] = f32_add(f32_mul(f32_add(f32_mul(c00[i], ntx), f32_mul(c10[i], tx)), nty), f32_mul(f32_add(f32_mul(c01[i], ntx), f32_mul(c11[i], tx)), ty));
      return;
    end
    st.texfetch += 1;
    unpack4(texel(t, tr.tbase[unit], w, hh, pal, ifloor(fu), ifloor(fv)), o);
  endfunction

  // grampo [0,1], *255, +0,5 e truncamento em cada canal (r,g,b,a)
  function u32_t pack8(f32_t col[4]);
    f32_t t; int q[4];
    for (int i = 0; i < 4; i++) begin
      t = f32_lt(col[i], F32_0) ? F32_0 : col[i]; t = f32_gt(t, F32_1) ? F32_1 : t;
      q[i] = f32_to_i32(f32_add(f32_mul(t, F32_255), F32_HALF));
    end
    return (u32_t'(q[3] & 255) << 24) | (u32_t'(q[0] & 255) << 16) | (u32_t'(q[1] & 255) << 8) | u32_t'(q[2] & 255);
  endfunction

  function u32_t shade(tri_t t, rstate_t s, f32_t l0, f32_t l1, f32_t l2);
    f32_t pr[3], P[3], rhw, col[4], TT[4], tx[4], d, a, fogc, den, fg, nfg, fc[4]; u32_t f;
    pr[0] = f32_mul(l0, t.rhw[0]); pr[1] = f32_mul(l1, t.rhw[1]); pr[2] = f32_mul(l2, t.rhw[2]);   // l_k * rhw_k
    rhw = f32_add(f32_add(pr[0], pr[1]), pr[2]);
    for (int k = 0; k < 3; k++) P[k] = f32_div(pr[k], rhw);                                   // (l_k * rhw_k) / rhw
    for (int i = 0; i < 4; i++) col[i] = f32_add(f32_add(f32_mul(P[0], t.col[i]), f32_mul(P[1], t.col[4 + i])), f32_mul(P[2], t.col[8 + i]));
    f = s.flags;
    if ((f & (ST_TEX0 | ST_TEX1)) != 0) begin
      for (int i = 0; i < 4; i++) TT[i] = f32_add(f32_add(f32_mul(P[0], t.uv[i]), f32_mul(P[1], t.uv[4 + i])), f32_mul(P[2], t.uv[8 + i]));   // u, v, u1, v1
      for (int unit = 0; unit < 2; unit++) begin
        if ((f & (unit != 0 ? ST_TEX1 : ST_TEX0)) == 0) continue;
        if (unit == 1 && (f & ST_UV1) != 0) sample(t, s, unit, TT[2], TT[3], tx); else sample(t, s, unit, TT[0], TT[1], tx);
        case (s.texenv[unit])
          TE_REPLACE: col = tx;
          TE_DECAL: for (int i = 0; i < 3; i++) col[i] = f32_add(f32_mul(tx[i], tx[3]), f32_mul(col[i], f32_sub(F32_1, tx[3])));
          TE_ADD: begin a = f32_mul(col[3], tx[3]); for (int i = 0; i < 4; i++) col[i] = f32_add(col[i], tx[i]); col[3] = a; end
          default: for (int i = 0; i < 4; i++) col[i] = f32_mul(col[i], tx[i]);
        endcase
      end
    end
    if ((f & ST_FOG) != 0 && (f & ST_UV1) == 0) begin
      fogc = f32_add(f32_add(f32_mul(P[0], t.fog[0]), f32_mul(P[1], t.fog[1])), f32_mul(P[2], t.fog[2]));
      den = f32_sub(s.fog_end, s.fog_start);
      fg = !f32_eq(den, F32_0) ? clampf(f32_div(f32_sub(fogc, s.fog_start), den), F32_0, F32_1) : F32_1;
      fc[0] = u8f[(s.fog_color >> 16) & 255]; fc[1] = u8f[(s.fog_color >> 8) & 255]; fc[2] = u8f[s.fog_color & 255];
      nfg = f32_sub(F32_1, fg);
      for (int i = 0; i < 3; i++) col[i] = f32_add(f32_mul(col[i], nfg), f32_mul(fc[i], fg));
    end
    return pack8(col);
  endfunction

  function u32_t blend_px(u32_t s, u32_t d, u32_t sf, u32_t df, u32_t saf, u32_t daf);
    int sa, da, fs, fd, c; u32_t o;
    sa = int'(s >> 24); da = int'(d >> 24); o = 0;
    fs = fac(sf, sa, da); fd = fac(df, sa, da);
    for (int sh = 0; sh < 32; sh += 8) begin
      if (sh == 24) begin fs = fac(saf, sa, da); fd = fac(daf, sa, da); end      // canal alfa: fatores próprios
      c = (int'((s >> sh) & 255) * fs + int'((d >> sh) & 255) * fd + 127) / 255; if (c > 255) c = 255;
      o |= u32_t'(c) << sh;
    end
    return o;
  endfunction
  function int fac(u32_t k, int sa, int da);
    case (k) BF_ZERO: return 0; BF_ONE: return 255; BF_SRC_ALPHA: return sa; BF_INV_SRC_ALPHA: return 255 - sa; BF_DST_ALPHA: return da; default: return 255 - da; endcase
  endfunction

  function bit eligible(tri_t t);
    u32_t f, df; f = states[t.state].flags; df = (f >> ST_DEPTH_FUNC_SHIFT) & 7;
    return (f & ST_DEPTH_TEST) != 0 && (f & ST_DEPTH_WRITE) != 0 && (f & ST_BLEND) == 0 && (f & ST_STENCIL) == 0 && (f & ST_ATEST) == 0 && (df == 0 || df == 1);
  endfunction

  function void render_tile(int tx, int ty, int b0, int b1);
    int i, j, py, n; u32_t row; tri_t t; f32_t l0, l1, l2; longint unsigned covered, shaded;
    tox = tx * T; toy = ty * T;
    for (int y = 0; y < T; y++) begin
      py = toy + y;
      if ((clr_flags & CLR_COLOR) != 0) for (int x = 0; x < T; x++) tcol[y * T + x] = clr_argb;
      else begin
        n = py < rt_h ? clampi(rt_w - tox, 0, T) : 0; row = rt_addr + u32_t'(py) * rt_pitch + u32_t'(tox) * 4;
        for (int x = 0; x < T; x++) tcol[y * T + x] = x < n ? fab_r32(h, row + u32_t'(x) * 4) : 0;
      end
    end
    for (int k = 0; k < T * T; k++) begin tz[k] = clr_depth; tst[k] = 0; end
    i = b0;
    while (i < b1) begin
      t = tris[bin_items[i]];
      if (hsr() && eligible(t)) begin
        j = i; while (j < b1 && eligible(tris[bin_items[j]])) j++;
        for (int k = 0; k < T * T; k++) tvis[k] = -1;
        for (int k = i; k < j; k++) begin
          t = tris[bin_items[k]]; covered = 0; shaded = 0;
          raster(t, imax(t.minx, tox), imin(t.maxx, tox + T - 1), imax(t.miny, toy), imin(t.maxy, toy + T - 1), 1, bin_items[k], states[t.state], covered, shaded);
          st.pix_covered += covered;
        end
        for (int q = 0; q < T * T; q++) if (tvis[q] >= 0) begin          // sombreia apenas o pixel visível, uma vez
          t = tris[tvis[q]];
          void'(cover_px(t, tox + (q % T), toy + (q / T), l0, l1, l2));
          tcol[q] = shade(t, states[t.state], l0, l1, l2); st.pix_shaded++;
        end
        i = j;
      end else begin
        covered = 0; shaded = 0;
        raster(t, imax(t.minx, tox), imin(t.maxx, tox + T - 1), imax(t.miny, toy), imin(t.maxy, toy + T - 1), 0, 0, states[t.state], covered, shaded);
        st.pix_covered += covered; st.pix_shaded += shaded;
        i++;
      end
    end
    for (int y = 0; y < T; y++) begin
      py = toy + y;
      if (py >= rt_h) break;
      n = clampi(rt_w - tox, 0, T); row = rt_addr + u32_t'(py) * rt_pitch + u32_t'(tox) * 4;
      for (int x = 0; x < n; x++) fab_w32(h, row + u32_t'(x) * 4, tcol[y * T + x]);
      st.vram_write += 64'(4 * n);
    end
    st.tiles++;
  endfunction

  function void flush();
    int tw, th, nb, cnt[], start[], fillp[], k;
    if (error) begin tris.delete(); return; end
    if (rt_w == 0) begin
      if (tris.size() != 0 || clr_flags != 0) fail("FLUSH sem render target");
      tris.delete(); return;
    end
    tw = (rt_w + T - 1) / T; th = (rt_h + T - 1) / T; nb = tw * th;
    // listas por bloco (na ordem de submissão dos triângulos)
    cnt = new[nb]; start = new[nb + 1]; fillp = new[nb];
    for (int b = 0; b < nb; b++) cnt[b] = 0;
    foreach (tris[i]) for (int ty = tris[i].miny / T; ty <= tris[i].maxy / T; ty++) for (int tx = tris[i].minx / T; tx <= tris[i].maxx / T; tx++) begin
      cnt[ty * tw + tx]++; st.bin_entries++;
    end
    start[0] = 0; for (int b = 0; b < nb; b++) begin start[b + 1] = start[b] + cnt[b]; fillp[b] = start[b]; end
    bin_items = new[start[nb] > 0 ? start[nb] : 1];
    foreach (tris[i]) for (int ty = tris[i].miny / T; ty <= tris[i].maxy / T; ty++) for (int tx = tris[i].minx / T; tx <= tris[i].maxx / T; tx++) begin
      k = ty * tw + tx; bin_items[fillp[k]] = i; fillp[k]++;
    end
    for (int b = 0; b < nb; b++)
      if (cnt[b] != 0 || (clr_flags & CLR_COLOR) != 0) render_tile(b % tw, b / tw, start[b], start[b + 1]);
    tris.delete(); states.delete(); pal_store.delete(); state_dirty = 1; clr_flags = 0;
  endfunction
  endclass
endpackage

module sf_flamecore;
  import fabric::*; import sf_flamecore_pkg::*;
  fc_core core;
  export "DPI-C" function gpu_bind;
  export "DPI-C" function gpu_read;
  export "DPI-C" function gpu_write;
  export "DPI-C" function gpu_event;
  export "DPI-C" function gpu_submit;
  export "DPI-C" function gpu_execute;
  function automatic void gpu_bind(chandle hh, int d); core = new; core.gpu_bind(hh, d); endfunction
  function automatic u32_t gpu_read(u32_t off); return core.gpu_read(off); endfunction
  function automatic void gpu_write(u32_t off, u32_t v); core.gpu_write(off, v); endfunction
  function automatic void gpu_event(int tag, longint unsigned irq, longint unsigned unused); core.gpu_event(tag, irq, unused); endfunction
  function automatic bit gpu_submit(u32_t addr, u32_t n, bit irq); return core.gpu_submit(addr, n, irq); endfunction
  function automatic void gpu_execute(u32_t addr, u32_t nwords); core.gpu_execute(addr, nwords); endfunction
endmodule
