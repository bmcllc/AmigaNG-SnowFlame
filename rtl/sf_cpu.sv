// SnowFire-CF266 em SystemVerilog: núcleo Motorola ColdFire V4e (ISA_C + FPU double + EMAC_B com as codificações do
// CFPRM rev. 3), modo usuário/supervisor com troca de A7, exceções/IRQ autovetoradas, erro de acesso reiniciável (MMU) e
// temporização por tabela (interlock de geração de endereço, predição estática de desvios). Mesmo comportamento de
// src/cpu_cf.cpp (o interpretador de referência, com despacho especializado só por velocidade).
//
// A hierarquia de memória (I-cache/D-cache, TLB/MMU, árbitro, MMIO) é a do Memory Host: busca, leitura e escrita passam por
// fab_fetch16 / fab_cpu_rd / fab_cpu_wr com o modo supervisor corrente. O estado arquitetural é espelhado no frontend
// (fab_cpu_state) ao fim de cada passo; o frontend devolve alterações dele por cpu_set_state antes do passo seguinte.
// As exceções de decodificação (ilegal, divisão por zero, privilégio) viram 'trap' e encerram a instrução no ponto em que
// ocorreram, como o 'throw' do modelo de referência.
package sf_cpu_pkg;
  import fabric::*; import sf100_abi::*;
  localparam int TRAP_ILLEGAL = 4, TRAP_DIVZERO = 5, TRAP_PRIV = 8;
  localparam int RUNNING = 0, HALT_INSN = 1, DOUBLE_FAULT = 2, NO_VECTOR = 3;
  localparam u32_t FC = 1, FV = 2, FZ = 4, FN = 8, FX = 16;
  typedef struct packed { int k; int rg; u32_t addr; u32_t imm; } ea_t;   // k: 0 Dn, 1 An, 2 memória, 3 imediato
  typedef struct packed { longint v; bit ovf; } prod_t;

  // O núcleo é uma classe: os métodos viram funções C++ de verdade (o Verilator expandiria em linha cada chamada de função
  // de módulo, e o decodificador chama ea()/brief()/rd() em dezenas de pontos).
  class cf_core;

  chandle h; int dev;
  u32_t d[8], a[8], pc, sr, vbr, fpcr, fpiar, macsr, mask, cacr, other_sp, exc_pc, exc_addr;
  real fp[8]; bit fp_n, fp_z, fp_nan, halted, stopped; longint acc[4]; int reason, exc_vec;
  longint unsigned cycles, instrs;
  int cyc, trap; u32_t agu_prev, agu_cur;


  function new(); endfunction
  function void attach(chandle hh, int dv); h = hh; dev = dv; agu_prev = 0; agu_cur = 0; cyc = 1; trap = 0; endfunction

  // ---------------- Espelho do estado arquitetural ----------------
  function void set_state(u32_t d0, u32_t d1, u32_t d2, u32_t d3, u32_t d4, u32_t d5, u32_t d6, u32_t d7,
      u32_t a0, u32_t a1, u32_t a2, u32_t a3, u32_t a4, u32_t a5, u32_t a6, u32_t a7, u32_t pc_, u32_t sr_, u32_t vbr_,
      real f0, real f1, real f2, real f3, real f4, real f5, real f6, real f7, bit fn, bit fz, bit fnan, u32_t fpcr_, u32_t fpiar_,
      longint c0, longint c1, longint c2, longint c3, u32_t macsr_, u32_t mask_, u32_t cacr_, u32_t osp, bit hl, bit stp, int rsn,
      int ev, u32_t epc, u32_t eaddr, longint unsigned cy, longint unsigned ins);
    d = '{d0, d1, d2, d3, d4, d5, d6, d7}; a = '{a0, a1, a2, a3, a4, a5, a6, a7}; pc = pc_; sr = sr_; vbr = vbr_;
    fp = '{f0, f1, f2, f3, f4, f5, f6, f7}; fp_n = fn; fp_z = fz; fp_nan = fnan; fpcr = fpcr_; fpiar = fpiar_;
    acc = '{c0, c1, c2, c3}; macsr = macsr_; mask = mask_; cacr = cacr_; other_sp = osp; halted = hl; stopped = stp; reason = rsn;
    exc_vec = ev; exc_pc = epc; exc_addr = eaddr; cycles = cy; instrs = ins;
  endfunction
  function void publish();
    fab_cpu_state(h, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], pc, sr, vbr,
                  fp[0], fp[1], fp[2], fp[3], fp[4], fp[5], fp[6], fp[7], fp_n, fp_z, fp_nan, fpcr, fpiar, acc[0], acc[1], acc[2], acc[3],
                  macsr, mask, cacr, other_sp, halted, stopped, reason, exc_vec, exc_pc, exc_addr, cycles, instrs);
  endfunction

  // ---------------- Utilitários ----------------
  function u32_t smask(int s); return s == 1 ? 32'hFF : s == 2 ? 32'hFFFF : 32'hFFFFFFFF; endfunction
  function u32_t ssign(int s); return s == 1 ? 32'h80 : s == 2 ? 32'h8000 : 32'h80000000; endfunction
  function u32_t sext(u32_t v, int s); return s == 1 ? u32_t'(int'(byte'(v[7:0]))) : s == 2 ? u32_t'(int'(shortint'(v[15:0]))) : v; endfunction
  function u32_t sx16(u32_t v); return u32_t'(int'(shortint'(v[15:0]))); endfunction
  function u32_t sx8(u32_t v); return u32_t'(int'(byte'(v[7:0]))); endfunction
  function longint sext48(longint v); return longint'(64'(v) << 16) >>> 16; endfunction
  function bit sup(); return sr[13]; endfunction
  function u32_t gr(int r); return r < 8 ? d[r] : a[r - 8]; endfunction
  function void gw(int r, u32_t v); if (r < 8) d[r] = v; else a[r - 8] = v; endfunction
  function void setccr(u32_t bits, u32_t m); sr = (sr & ~m) | (bits & m); endfunction
  function void agu_use(int r); if ((agu_prev & (32'd1 << r)) != 0) cyc++; endfunction
  function void agu_set(int r); agu_cur |= 32'd1 << r; endfunction
  function void illegal(); trap = TRAP_ILLEGAL; endfunction
  function bit priv(); if (!sup()) begin trap = TRAP_PRIV; return 0; end return 1; endfunction

  // ---------------- Barramento (Memory Host) ----------------
  function u32_t fetch(); u32_t w; w = 32'(fab_fetch16(h, pc, sup())); pc += 2; return w; endfunction
  function u32_t fetch32(); u32_t hi; hi = fetch(); return (hi << 16) | fetch(); endfunction
  function u32_t rd(u32_t ad, int sz); cyc++; return fab_cpu_rd(h, ad, sz, int'(sup())); endfunction
  function void wr(u32_t ad, int sz, u32_t v); cyc++; fab_cpu_wr(h, ad, sz, v, int'(sup())); endfunction
  function void push32(u32_t v); a[7] -= 4; wr(a[7], 4, v); endfunction
  function u32_t pop32(); u32_t v; v = rd(a[7], 4); a[7] += 4; return v; endfunction

  // ---------------- Endereçamento ----------------
  function u32_t brief(u32_t base);
    u32_t e, idx, bd, b, od; int r, bds, iis, ods; bit bs, is_;
    e = fetch();
    r = int'((e >> 12) & 15); idx = r < 8 ? d[r] : a[r - 8];
    if ((e & 32'h800) == 0) idx = sx16(idx);
    idx <<= (e >> 9) & 3;
    if ((e & 32'h100) == 0) return base + sx8(e) + idx;                 // formato curto: (d8,An,Xn*SF)
    // formato completo (68020): deslocamento de base, supressão de base/índice e indireção de memória
    bs = e[7]; is_ = e[6]; bds = int'((e >> 4) & 3); iis = int'(e & 7);
    if (bds == 0 || (e & 8) != 0) begin illegal(); return 0; end
    bd = bds == 1 ? 0 : bds == 2 ? sx16(fetch()) : fetch32();
    b = bs ? 0 : base; if (is_) idx = 0;
    ods = iis & 3; od = ods == 2 ? sx16(fetch()) : ods == 3 ? fetch32() : 0;
    cyc += 1;
    if (!is_) begin
      if (iis == 0) return b + bd + idx;
      if (iis == 4) begin illegal(); return 0; end
      if ((iis & 4) == 0) return rd(b + bd + idx, 4) + od;              // pré-indexado
      return rd(b + bd, 4) + idx + od;                                  // pós-indexado
    end
    if (iis == 0) return b + bd;
    if (iis > 3) begin illegal(); return 0; end
    return rd(b + bd, 4) + od;
  endfunction

  function ea_t ea(int mode, int rg, int size);
    ea_t e; u32_t base, w;
    e = '0; e.rg = rg;
    case (mode)
      0: e.k = 0;
      1: e.k = 1;
      2: begin agu_use(rg); e.k = 2; e.addr = a[rg]; end
      3: begin agu_use(rg); e.k = 2; e.addr = a[rg]; a[rg] += (rg == 7 && size == 1) ? 2 : u32_t'(size); end
      4: begin agu_use(rg); a[rg] -= (rg == 7 && size == 1) ? 2 : u32_t'(size); e.k = 2; e.addr = a[rg]; end
      5: begin agu_use(rg); w = fetch(); e.k = 2; e.addr = a[rg] + sx16(w); end
      6: begin agu_use(rg); e.k = 2; e.addr = brief(a[rg]); end
      7: case (rg)
           0: begin e.k = 2; e.addr = sx16(fetch()); end
           1: begin e.k = 2; e.addr = fetch32(); end
           2: begin base = pc; w = fetch(); e.k = 2; e.addr = base + sx16(w); end
           3: begin base = pc; e.k = 2; e.addr = brief(base); end
           4: begin e.k = 3; if (size == 4) e.imm = fetch32(); else begin w = fetch(); e.imm = size == 1 ? (w & 32'hFF) : w; end end
           default: illegal();
         endcase
      default: illegal();
    endcase
    return e;
  endfunction
  function u32_t ea_read(ea_t e, int size);
    case (e.k)
      0: return d[e.rg] & smask(size);
      1: return size == 2 ? sx16(a[e.rg]) : a[e.rg];
      2: return rd(e.addr, size);
      default: return e.imm;
    endcase
  endfunction
  function void ea_write(ea_t e, int size, u32_t v);
    case (e.k)
      0: d[e.rg] = size == 4 ? v : (d[e.rg] & ~smask(size)) | (v & smask(size));
      1: begin a[e.rg] = v; agu_set(e.rg); end
      2: wr(e.addr, size, v);
      default: illegal();
    endcase
  endfunction
  function ea_t dreg(int r); ea_t e; e = '0; e.k = 0; e.rg = r; return e; endfunction

  // ---------------- Flags ----------------
  function void setnz(u32_t r, int s);
    u32_t f; r &= smask(s); f = 0; if (r == 0) f |= FZ; if ((r & ssign(s)) != 0) f |= FN;
    setccr(f, FN | FZ | FV | FC);
  endfunction
  function u32_t do_add(u32_t x, u32_t y, int s, bit setx = 1);
    longint unsigned r; u32_t res, f;
    r = 64'(x & smask(s)) + 64'(y & smask(s)); res = u32_t'(r) & smask(s); f = 0;
    if (r > 64'(smask(s))) f |= FC; if ((((x ^ res) & (y ^ res)) & ssign(s)) != 0) f |= FV;
    if (res == 0) f |= FZ; if ((res & ssign(s)) != 0) f |= FN; if ((f & FC) != 0 && setx) f |= FX;
    setccr(f, setx ? (FN | FZ | FV | FC | FX) : (FN | FZ | FV | FC));
    return res;
  endfunction
  function u32_t do_sub(u32_t dst, u32_t src, int s, bit setx = 1);
    u32_t m, res, f;
    m = smask(s); res = (dst - src) & m; f = 0;
    if ((dst & m) < (src & m)) f |= FC; if ((((dst ^ src) & (dst ^ res)) & ssign(s)) != 0) f |= FV;
    if (res == 0) f |= FZ; if ((res & ssign(s)) != 0) f |= FN; if ((f & FC) != 0 && setx) f |= FX;
    setccr(f, setx ? (FN | FZ | FV | FC | FX) : (FN | FZ | FV | FC));
    return res;
  endfunction
  function u32_t do_addx(u32_t x, u32_t y, int s);
    u32_t m, cin, res, f; longint unsigned r;
    m = smask(s); cin = sr[4] ? 1 : 0; r = 64'(x & m) + 64'(y & m) + 64'(cin); res = u32_t'(r) & m; f = 0;
    if (r > 64'(m)) f |= FC | FX; if ((((x ^ res) & (y ^ res)) & ssign(s)) != 0) f |= FV; if ((res & ssign(s)) != 0) f |= FN;
    setccr(f, FN | FV | FC | FX); if (res != 0) setccr(0, FZ);        // Z só é limpo, nunca ligado
    return res;
  endfunction
  function u32_t do_subx(u32_t dst, u32_t src, int s);
    u32_t m, cin, res, f;
    m = smask(s); cin = sr[4] ? 1 : 0; res = (dst - src - cin) & m; f = 0;
    if (64'(dst & m) < 64'(src & m) + 64'(cin)) f |= FC | FX; if ((((dst ^ src) & (dst ^ res)) & ssign(s)) != 0) f |= FV; if ((res & ssign(s)) != 0) f |= FN;
    setccr(f, FN | FV | FC | FX); if (res != 0) setccr(0, FZ);
    return res;
  endfunction
  function u32_t do_shift(u32_t val, int typ, bit left, int cnt, int s);
    u32_t m, sg, c, v, x, nv, f;
    m = smask(s); sg = ssign(s); val &= m; c = 0; v = 0; x = sr[4] ? 1 : 0;
    for (int i = 0; i < cnt; i++) begin
      if (left) begin
        if (typ == 3) begin c = (val & sg) != 0 ? 1 : 0; val = ((val << 1) | c) & m; end
        else if (typ == 2) begin c = (val & sg) != 0 ? 1 : 0; val = ((val << 1) | x) & m; x = c; end
        else begin c = (val & sg) != 0 ? 1 : 0; nv = (val << 1) & m; if (typ == 0 && ((nv ^ val) & sg) != 0) v = 1; val = nv; end
      end else begin
        c = val & 1;
        if (typ == 0) val = (val >> 1) | (val & sg);
        else if (typ == 1) val >>= 1;
        else if (typ == 2) begin val = (val >> 1) | (x != 0 ? sg : 0); x = c; end
        else val = (val >> 1) | (c != 0 ? sg : 0);
      end
    end
    f = 0; if (val == 0) f |= FZ; if ((val & sg) != 0) f |= FN;
    if (cnt > 0) begin if (c != 0) f |= FC; if (v != 0) f |= FV; if (c != 0 && typ != 3) f |= FX; end
    else if (typ == 2 && x != 0) f |= FC;                               // ROX com contagem 0: C = X
    setccr(f, (cnt > 0 && typ != 3) ? (FN | FZ | FV | FC | FX) : (FN | FZ | FV | FC));
    return val;
  endfunction
  function bit cond(int cc);
    bit c, v, z, n; c = sr[0]; v = sr[1]; z = sr[2]; n = sr[3];
    case (cc)
      0: return 1; 1: return 0; 2: return !c && !z; 3: return c || z;
      4: return !c; 5: return c; 6: return !z; 7: return z;
      8: return !v; 9: return v; 10: return !n; 11: return n;
      12: return n == v; 13: return n != v; 14: return !z && (n == v); default: return z || (n != v);
    endcase
  endfunction

  // ---------------- Exceções ----------------
  function void set_sr(u32_t v);
    u32_t t;
    v &= 32'hFFFF; if (((v ^ sr) & 32'h2000) != 0) begin t = a[7]; a[7] = other_sp; other_sp = t; end   // troca USP/SSP
    sr = v;
  endfunction
  function void exception(int vec, u32_t retpc, int newmask);
    u32_t oldsr, target;
    exc_vec = vec; exc_pc = retpc; exc_addr = fab_busreg(h, BUS_FAULT_ADDR);
    oldsr = sr; set_sr(sr | 32'h2000); fab_set_busreg(h, BUS_MMU_BYPASS, 1);   // pilha/vetores de exceção são acessos físicos
    if (newmask >= 0) sr = (sr & ~32'h700) | (u32_t'(newmask) << 8);
    a[7] -= 8;
    fab_cpu_wr(h, a[7] + 4, 4, retpc, 2);
    fab_cpu_wr(h, a[7], 4, (32'd4 << 28) | (u32_t'(vec) << 18) | (oldsr & 32'hFFFF), 2);
    target = fab_cpu_rd(h, vbr + u32_t'(vec) * 4, 4, 2);
    fab_set_busreg(h, BUS_MMU_BYPASS, 0);
    if (fab_busreg(h, BUS_FAULT) != 0) begin fab_set_busreg(h, BUS_FAULT, 0); halted = 1; reason = DOUBLE_FAULT; return; end
    if (target == 0) begin halted = 1; reason = NO_VECTOR; return; end
    pc = target;
  endfunction

  // Reset de hardware: SP = [VBR+0], PC = [VBR+4] (registradores já zerados pelo frontend: reset_regs)
  function void reset();
    a[7] = fab_cpu_rd(h, vbr, 4, 2); pc = fab_cpu_rd(h, vbr + 4, 4, 2);
    if (fab_busreg(h, BUS_FAULT) != 0) begin fab_set_busreg(h, BUS_FAULT, 0); halted = 1; reason = DOUBLE_FAULT; end
    publish();
  endfunction

  // Executa uma instrução (ou atende IRQ). Devolve os ciclos consumidos (0 se parada em STOP/HALT).
  function int step();
    u32_t start, ssr, poll, sd[8], sa[8]; bit snap;
    if (halted) return 0;
    poll = fab_cpu_poll(h);                                           // bit0 IRQ pendente, bit1 MMU ligada, bit2 supervisor sem tradução
    if (poll[0] && ((sr >> 8) & 7) < u32_t'(IRQ_LEVEL)) begin
      stopped = 0; cyc = 10; exception(VEC_IRQ4, pc, IRQ_LEVEL); cycles += 64'(cyc); publish(); return cyc;
    end
    if (stopped) return 0;
    start = pc; cyc = 1; agu_prev = agu_cur; agu_cur = 0;
    // Com a MMU ligada o erro de acesso precisa ser reiniciável: guarda os registradores
    ssr = sr; snap = poll[1] && !(poll[2] && sr[13]);
    if (snap) begin sd = d; sa = a; end
    trap = 0;
    exec();
    if (trap != 0) begin int t; t = trap; trap = 0; exception(t, start, -1); end
    if (fab_busreg(h, BUS_FAULT) != 0) begin
      fab_set_busreg(h, BUS_FAULT, 0);
      if (snap && ((sr ^ ssr) & 32'h2000) == 0) begin d = sd; a = sa; sr = (sr & ~32'hFF) | (ssr & 32'hFF); end
      if (!halted) exception(2, start, -1);
    end
    instrs++; cycles += 64'(cyc);
    publish();
    return cyc;
  endfunction

  // ---------------- Execução ----------------
  function void exec();
    u32_t op; int grp, mode, rg;
    op = fetch(); grp = int'(op >> 12); mode = int'((op >> 3) & 7); rg = int'(op & 7);
    case (grp)
      0: exec_0(op, mode, rg);
      1, 2, 3: exec_move(op, grp, mode);
      4: exec_4(op, mode, rg);
      5: exec_5(op, mode, rg);
      6: exec_6(op);
      7: exec_7(op, mode);
      8, 12: exec_logic(op, grp, mode, rg);
      9, 11, 13: exec_arith(op, grp, mode, rg);
      10: exec_a(op, mode);
      14: exec_e(op, mode);
      default: exec_fpu(op);
    endcase
  endfunction

  function void exec_0(u32_t op, int mode, int rg);
    int tt, size, sz, sub; u32_t bt, v, rr, imm, w, x, r; ea_t e;
    if ((op & 32'h100) != 0 || (op & 32'hF00) == 32'h800) begin           // BTST/BCHG/BCLR/BSET
      tt = int'((op >> 6) & 3);
      if ((op & 32'h100) != 0) bt = d[(op >> 9) & 7]; else bt = fetch() & 32'hFF;
      e = ea(mode, rg, 1); if (trap != 0) return;
      if (e.k == 1) begin illegal(); return; end
      if (e.k == 0) begin bt &= 31; v = d[e.rg]; end else begin bt &= 7; v = ea_read(e, 1); end
      setccr(((v >> bt) & 1) != 0 ? 0 : FZ, FZ);
      if (tt == 1) v ^= 32'd1 << bt; else if (tt == 2) v &= ~(32'd1 << bt); else if (tt == 3) v |= 32'd1 << bt;
      if (tt != 0) begin if (e.k == 0) d[e.rg] = v; else ea_write(e, 1, v); end
      return;
    end
    if ((op & 32'hFFF8) == 32'h00C0) begin v = d[rg]; rr = 0; for (int i = 0; i < 32; i++) if ((v & (32'd1 << i)) != 0) rr |= 32'h80000000 >> i; d[rg] = rr; return; end   // BITREV
    if ((op & 32'hFFF8) == 32'h02C0) begin v = d[rg]; d[rg] = (v << 24) | ((v & 32'hFF00) << 8) | ((v >> 8) & 32'hFF00) | (v >> 24); return; end        // BYTEREV
    if ((op & 32'hFFF8) == 32'h04C0) begin                                                                                                       // FF1
      v = d[rg]; rr = 0; while (rr < 32 && (v & (32'h80000000 >> rr)) == 0) rr++;
      d[rg] = rr; setccr((v == 0 ? FZ : 0) | ((v & 32'h80000000) != 0 ? FN : 0), FN | FZ | FV | FC); return;
    end
    size = int'((op >> 6) & 3); if (size == 3) begin illegal(); return; end
    sz = 1 << size; sub = int'((op >> 8) & 15);
    if (sz == 4) imm = fetch32(); else begin w = fetch(); imm = sz == 1 ? (w & 32'hFF) : w; end
    e = ea(mode, rg, sz); if (trap != 0) return;
    if (e.k == 1 || e.k == 3) begin illegal(); return; end
    x = ea_read(e, sz);
    case (sub)
      0: begin r = x | imm; setnz(r, sz); ea_write(e, sz, r); end
      2: begin r = x & imm; setnz(r, sz); ea_write(e, sz, r); end
      4: begin r = do_sub(x, imm, sz); ea_write(e, sz, r); end
      6: begin r = do_add(x, imm, sz); ea_write(e, sz, r); end
      10: begin r = x ^ imm; setnz(r, sz); ea_write(e, sz, r); end
      12: void'(do_sub(x, imm, sz, 0));
      default: illegal();
    endcase
  endfunction

  function void exec_move(u32_t op, int grp, int mode);
    int sz, dm, dr; u32_t v; ea_t s, t;
    sz = grp == 1 ? 1 : grp == 3 ? 2 : 4;
    s = ea(mode, int'(op & 7), sz); if (trap != 0) return;
    v = ea_read(s, sz);
    dm = int'((op >> 6) & 7); dr = int'((op >> 9) & 7);
    if (dm == 1) begin if (sz == 1) begin illegal(); return; end a[dr] = sz == 2 ? sx16(v) : v; agu_set(dr); return; end
    t = ea(dm, dr, sz); if (trap != 0) return;
    if (t.k == 3) begin illegal(); return; end
    ea_write(t, sz, v); setnz(v, sz);
  endfunction

  function void exec_4(u32_t op, int mode, int rg);
    u32_t f, v, x, rc, src, q, rr, ad, msk, lw; int r, dl, dr, dh, size, sz, sub; bit isa, toreg, sg, w64, ovf; ea_t e;
    longint unsigned pr; longint dv, qq, rm; int a_, b_;
    if (op == 32'h4E71) return;
    if (op == 32'h4E75) begin pc = pop32(); cyc += 3; return; end
    if (op == 32'h4E73) begin if (!priv()) return; f = pop32(); pc = pop32(); set_sr(f); return; end
    if (op == 32'h4E72) begin if (!priv()) return; set_sr(fetch()); stopped = 1; return; end
    if (op == 32'h4AC8) begin if (!priv()) return; halted = 1; reason = HALT_INSN; return; end
    if (op == 32'h4AFC) begin illegal(); return; end
    if ((op & 32'hFFC0) == 32'h4AC0 && mode != 1) begin                    // TAS
      e = ea(mode, rg, 1); if (trap != 0) return; if (e.k == 3) begin illegal(); return; end
      v = ea_read(e, 1); setnz(v, 1); ea_write(e, 1, v | 32'h80); return;
    end
    if ((op & 32'hFFF8) == 32'h4C80) begin                                 // SATS.L
      if (sr[1]) d[rg] = (d[rg] & 32'h80000000) != 0 ? 32'h7FFFFFFF : 32'h80000000;
      setnz(d[rg], 4); return;
    end
    if ((op & 32'hFFF8) == 32'h42C0) begin d[rg] = (d[rg] & 32'hFFFF0000) | (sr & 32'h1F); return; end   // MOVE CCR,Dn
    if ((op & 32'hFFF0) == 32'h4E40) begin exception(32 + int'(op & 15), pc, -1); return; end
    if ((op & 32'hFFF8) == 32'h4E50) begin x = sx16(fetch()); push32(a[rg]); a[rg] = a[7]; a[7] += x; return; end
    if ((op & 32'hFFF8) == 32'h4E58) begin a[7] = a[rg]; a[rg] = pop32(); return; end
    if (op == 32'h4E7B || op == 32'h4E7A) begin                            // MOVEC Rn,Rc / Rc,Rn
      if (!priv()) return; x = fetch(); r = int'((x >> 12) & 7); isa = x[15]; rc = x & 32'hFFF;
      if (op == 32'h4E7B) begin
        v = isa ? a[r] : d[r];
        if (rc == 32'h801) vbr = v; else if (rc == 32'h800) other_sp = v; else if (rc == 32'h002) cacr = v;   // demais: ignorados
      end else begin
        v = rc == 32'h801 ? vbr : rc == 32'h800 ? other_sp : rc == 32'h002 ? cacr : 0;
        if (isa) a[r] = v; else d[r] = v;
      end
      return;
    end
    if ((op & 32'hFFF0) == 32'h4E60) begin if (!priv()) return; r = int'(op & 7); if ((op & 8) != 0) a[r] = other_sp; else other_sp = a[r]; return; end   // MOVE USP
    if ((op & 32'hFFF8) == 32'h40C0) begin if (!priv()) return; d[rg] = (d[rg] & 32'hFFFF0000) | (sr & 32'hFFFF); return; end
    if ((op & 32'hFFC0) == 32'h46C0) begin if (!priv()) return; e = ea(mode, rg, 2); if (trap != 0) return; set_sr(ea_read(e, 2)); return; end
    if ((op & 32'hFFC0) == 32'h44C0) begin e = ea(mode, rg, 2); if (trap != 0) return; setccr(ea_read(e, 2), 32'h1F); return; end
    if ((op & 32'hF1C0) == 32'h41C0 && mode >= 2) begin
      e = ea(mode, rg, 4); if (trap != 0) return; if (e.k != 2) begin illegal(); return; end
      a[(op >> 9) & 7] = e.addr; agu_set(int'((op >> 9) & 7)); return;
    end
    if ((op & 32'hFFF8) == 32'h4840) begin v = d[rg]; d[rg] = (v << 16) | (v >> 16); setnz(d[rg], 4); return; end
    if ((op & 32'hFFC0) == 32'h4840) begin e = ea(mode, rg, 4); if (trap != 0) return; if (e.k != 2) begin illegal(); return; end push32(e.addr); return; end
    if ((op & 32'hFFF8) == 32'h4880) begin d[rg] = (d[rg] & 32'hFFFF0000) | (sx8(d[rg]) & 32'hFFFF); setnz(d[rg], 2); return; end
    if ((op & 32'hFFF8) == 32'h48C0) begin d[rg] = sx16(d[rg]); setnz(d[rg], 4); return; end
    if ((op & 32'hFFF8) == 32'h49C0) begin d[rg] = sx8(d[rg]); setnz(d[rg], 4); return; end
    if ((op & 32'hFFC0) == 32'h48C0 || (op & 32'hFFC0) == 32'h4CC0) begin   // MOVEM.L
      toreg = op[10]; msk = fetch();
      if (mode != 2 && mode != 5) begin illegal(); return; end
      e = ea(mode, rg, 4); if (trap != 0) return; ad = e.addr;
      for (int i = 0; i < 16; i++) if ((msk & (32'd1 << i)) != 0) begin
        if (toreg) begin lw = rd(ad, 4); if (i < 8) d[i] = lw; else a[i - 8] = lw; end
        else wr(ad, 4, i < 8 ? d[i] : a[i - 8]);
        ad += 4;
      end
      return;
    end
    if ((op & 32'hFFC0) == 32'h4C00 || (op & 32'hFFC0) == 32'h4C40) begin   // MULx.L / DIVx.L
      x = fetch(); e = ea(mode, rg, 4); if (trap != 0) return; src = ea_read(e, 4);
      dl = int'((x >> 12) & 7); sg = x[11];
      if ((op & 32'h40) != 0) begin
        if (src == 0) begin trap = TRAP_DIVZERO; return; end
        dr = int'(x & 7); f = 0;
        if (sg) begin
          a_ = int'(d[dl]); b_ = int'(src);
          if (a_ == -2147483648 && b_ == -1) begin q = u32_t'(a_); rr = 0; f |= FV; end else begin q = u32_t'(a_ / b_); rr = u32_t'(a_ % b_); end
        end else begin q = d[dl] / src; rr = d[dl] % src; end
        if (dr != dl) d[dr] = rr; else d[dl] = q;                       // REMS/REMU.L guardam só o resto; DIVS/DIVU.L o quociente
        if (q == 0) f |= FZ; if ((q & 32'h80000000) != 0) f |= FN; setccr(f, FN | FZ | FV | FC); cyc += 19;
      end else begin
        w64 = x[10]; dh = int'(x & 7); f = 0;
        if (sg) pr = 64'(longint'(int'(d[dl])) * longint'(int'(src))); else pr = 64'(d[dl]) * 64'(src);
        d[dl] = u32_t'(pr);
        if (w64) begin d[dh] = u32_t'(pr >> 32); if (pr == 0) f |= FZ; if (pr[63]) f |= FN; end
        else begin
          ovf = sg ? (longint'(pr) != longint'(int'(u32_t'(pr)))) : (pr >> 32) != 0;
          if (ovf) f |= FV; if (u32_t'(pr) == 0) f |= FZ; if (pr[31]) f |= FN;
        end
        setccr(f, FN | FZ | FV | FC); cyc += 3;
      end
      return;
    end
    if ((op & 32'hFFC0) == 32'h4E80) begin e = ea(mode, rg, 4); if (trap != 0) return; if (e.k != 2) begin illegal(); return; end push32(pc); pc = e.addr; cyc += 2; return; end
    if ((op & 32'hFFC0) == 32'h4EC0) begin e = ea(mode, rg, 4); if (trap != 0) return; if (e.k != 2) begin illegal(); return; end pc = e.addr; cyc += 2; return; end
    size = int'((op >> 6) & 3);
    if (size != 3) begin
      sz = 1 << size; sub = int'((op >> 8) & 15);
      if (sub == 0) begin                                                 // NEGX (só Dn)
        if (mode != 0) begin illegal(); return; end
        ea_write(dreg(rg), sz, do_subx(0, d[rg], sz)); return;
      end
      if (sub == 2 || sub == 4 || sub == 6 || sub == 10) begin           // CLR, NEG, NOT, TST
        e = ea(mode, rg, sz); if (trap != 0) return;
        if (sub == 10) begin setnz(ea_read(e, sz), sz); return; end
        if (e.k == 1 || e.k == 3) begin illegal(); return; end
        if (sub == 2) begin ea_write(e, sz, 0); setccr(FZ, FN | FZ | FV | FC); end
        else if (sub == 4) begin v = do_sub(0, ea_read(e, sz), sz); ea_write(e, sz, v); end
        else begin v = ~ea_read(e, sz); setnz(v, sz); ea_write(e, sz, v); end
        return;
      end
    end
    illegal();
  endfunction

  function void exec_5(u32_t op, int mode, int rg);
    int size, sz; u32_t q, x, r; bit sub; ea_t e;
    size = int'((op >> 6) & 3);
    if (op == 32'h51F8) return; if (op == 32'h51FA) begin void'(fetch()); return; end if (op == 32'h51FB) begin void'(fetch32()); return; end   // TPF
    if (size == 3) begin                                                  // Scc
      e = ea(mode, rg, 1); if (trap != 0) return; if (e.k == 1) begin illegal(); return; end
      ea_write(e, 1, cond(int'((op >> 8) & 15)) ? 32'hFF : 0); return;
    end
    sz = 1 << size; q = (op >> 9) & 7; if (q == 0) q = 8; sub = op[8];
    if (mode == 1) begin a[rg] += sub ? -q : q; agu_set(rg); return; end
    e = ea(mode, rg, sz); if (trap != 0) return; if (e.k == 3) begin illegal(); return; end
    x = ea_read(e, sz); r = sub ? do_sub(x, q, sz) : do_add(x, q, sz);
    ea_write(e, sz, r);
  endfunction

  function void exec_6(u32_t op);
    int cc; u32_t base; int disp; bit pred, taken;
    cc = int'((op >> 8) & 15); base = pc; disp = int'(byte'(op[7:0]));
    if ((op & 32'hFF) == 0) disp = int'(shortint'(fetch())); else if ((op & 32'hFF) == 32'hFF) disp = int'(fetch32());
    if (cc <= 1) begin if (cc == 1) push32(pc); pc = base + u32_t'(disp); cyc += 2; end   // BRA/BSR
    else begin                                                            // Bcc: predição estática (para trás = tomado)
      pred = disp < 0; taken = cond(cc);
      if (taken) begin pc = base + u32_t'(disp); cyc += pred ? 1 : 5; end else cyc += pred ? 5 : 0;
    end
  endfunction

  function void exec_7(u32_t op, int mode);
    int opm, ss; u32_t v; ea_t e;
    if ((op & 32'h100) != 0) begin                                       // MVS / MVZ
      opm = int'((op >> 6) & 7); if (opm < 4) begin illegal(); return; end
      ss = (opm & 1) != 0 ? 2 : 1; e = ea(mode, int'(op & 7), ss); if (trap != 0) return; v = ea_read(e, ss) & smask(ss);
      if (opm < 6) v = sext(v, ss);
      d[(op >> 9) & 7] = v; setnz(v, 4); return;
    end
    v = sx8(op); d[(op >> 9) & 7] = v; setnz(v, 4);
  endfunction

  function void exec_logic(u32_t op, int grp, int mode, int rg);
    int opm, dn, size, sz; u32_t src, r, f, x, sv; bit sg; ea_t e; int dvs, s16; longint q, rm; u32_t dvu, su, qu, ru;
    opm = int'((op >> 6) & 7); dn = int'((op >> 9) & 7);
    if (opm == 3 || opm == 7) begin
      e = ea(mode, rg, 2); if (trap != 0) return; if (e.k == 1) begin illegal(); return; end src = ea_read(e, 2); sg = opm == 7;
      if (grp == 12) begin                                                // MULU.W / MULS.W
        r = sg ? u32_t'(int'(shortint'(d[dn][15:0])) * int'(shortint'(src[15:0]))) : (d[dn] & 32'hFFFF) * (src & 32'hFFFF);
        d[dn] = r; setnz(r, 4); cyc += 2;
      end else begin                                                      // DIVU.W / DIVS.W
        if ((src & 32'hFFFF) == 0) begin trap = TRAP_DIVZERO; return; end
        f = 0;
        if (sg) begin
          dvs = int'(d[dn]); s16 = int'(shortint'(src[15:0])); q = longint'(dvs) / longint'(s16); rm = longint'(dvs) % longint'(s16);
          if (q > 32767 || q < -32768) f |= FV;
          else begin d[dn] = (u32_t'(rm & 64'hFFFF) << 16) | (u32_t'(q) & 32'hFFFF); if (q == 0) f |= FZ; if (q < 0) f |= FN; end
        end else begin
          dvu = d[dn]; su = src & 32'hFFFF; qu = dvu / su; ru = dvu % su;
          if (qu > 32'hFFFF) f |= FV; else begin d[dn] = (ru << 16) | qu; if (qu == 0) f |= FZ; if ((qu & 32'h8000) != 0) f |= FN; end
        end
        setccr(f, FN | FZ | FV | FC); cyc += 18;
      end
      return;
    end
    size = opm & 3; if (size == 3) begin illegal(); return; end sz = 1 << size;
    if (opm < 4) begin
      e = ea(mode, rg, sz); if (trap != 0) return; x = ea_read(e, sz);
      r = grp == 8 ? (d[dn] | x) : (d[dn] & x); setnz(r, sz);
      ea_write(dreg(dn), sz, r);
    end else begin
      e = ea(mode, rg, sz); if (trap != 0) return; if (e.k == 0 || e.k == 1 || e.k == 3) begin illegal(); return; end
      x = ea_read(e, sz); r = grp == 8 ? (d[dn] | x) : (d[dn] & x); setnz(r, sz); ea_write(e, sz, r);
    end
  endfunction

  function void exec_arith(u32_t op, int grp, int mode, int rg);
    int opm, dn, sz, size; u32_t s_, r, x; ea_t e;
    opm = int'((op >> 6) & 7); dn = int'((op >> 9) & 7);
    if (opm == 3 || opm == 7) begin                                       // ADDA / SUBA / CMPA
      sz = opm == 3 ? 2 : 4; e = ea(mode, rg, sz); if (trap != 0) return; s_ = sext(ea_read(e, sz), sz);
      if (grp == 13) begin a[dn] += s_; agu_set(dn); end else if (grp == 9) begin a[dn] -= s_; agu_set(dn); end else void'(do_sub(a[dn], s_, 4, 0));
      return;
    end
    size = opm & 3; sz = 1 << size; if (size == 3) begin illegal(); return; end
    if (grp != 11 && opm >= 4 && mode == 0) begin                         // ADDX / SUBX Dy,Dx
      r = grp == 13 ? do_addx(d[dn], d[rg], sz) : do_subx(d[dn], d[rg], sz);
      ea_write(dreg(dn), sz, r); return;
    end
    if (grp == 11) begin
      if (opm < 4) begin e = ea(mode, rg, sz); if (trap != 0) return; void'(do_sub(d[dn], ea_read(e, sz), sz, 0)); end
      else begin                                                          // EOR Dn,<ea>
        e = ea(mode, rg, sz); if (trap != 0) return; if (e.k == 1 || e.k == 3) begin illegal(); return; end
        r = ea_read(e, sz) ^ d[dn]; setnz(r, sz); ea_write(e, sz, r);
      end
      return;
    end
    if (opm < 4) begin
      e = ea(mode, rg, sz); if (trap != 0) return; x = ea_read(e, sz);
      r = grp == 13 ? do_add(d[dn], x, sz) : do_sub(d[dn], x, sz);
      ea_write(dreg(dn), sz, r);
    end else begin
      e = ea(mode, rg, sz); if (trap != 0) return; if (e.k == 0 || e.k == 1 || e.k == 3) begin illegal(); return; end
      x = ea_read(e, sz);
      r = grp == 13 ? do_add(x, d[dn], sz) : do_sub(x, d[dn], sz);
      ea_write(e, sz, r);
    end
  endfunction

  function void exec_a(u32_t op, int mode);
    int q, g, sub, p, n; u32_t v; ea_t e;
    if ((op & 32'h01C0) == 32'h0140) begin                                // MOV3Q.L #imm3,<ea>
      q = int'((op >> 9) & 7); v = q == 0 ? 32'hFFFFFFFF : u32_t'(q);
      e = ea(mode, int'(op & 7), 4); if (trap != 0) return; if (e.k == 3) begin illegal(); return; end
      ea_write(e, 4, v); if (e.k != 1) setnz(v, 4); return;
    end
    // eMAC (CFPRM rev. 3). Bit 8 = 0: MAC/MSAC/MAAAC...; bit 8 = 1: MOVE ACC/ACCext/MACSR/MASK.
    if ((op & 32'h0100) == 0) begin emac_mac(op); return; end
    if ((op & 32'h0800) != 0) begin                                       // bits 10-9: 0 MACSR, 1 ACCext01, 2 MASK, 3 ACCext23
      g = int'((op >> 9) & 3); sub = int'((op >> 6) & 3);
      if (g == 0) begin
        if (sub == 0) begin e = ea(mode, int'(op & 7), 4); if (trap != 0) return; macsr = ea_read(e, 4) & 32'hFFF; end
        else if (sub == 2) gw(int'(op & 15), macsr);
        else if (sub == 3 && op == 32'hA9C0) setccr((((macsr >> 3) & 1) << 3) | (((macsr >> 2) & 1) << 2) | (((macsr >> 1) & 1) << 1) | (macsr & 1), 32'h1F);   // MOVE MACSR,CCR
        else illegal();
      end else if (g == 2) begin
        if (sub == 0) begin e = ea(mode, int'(op & 7), 4); if (trap != 0) return; mask = ea_read(e, 4) & 32'hFFFF; end
        else if (sub == 2) gw(int'(op & 15), 32'hFFFF0000 | mask);
        else illegal();
      end else begin
        p = g == 1 ? 0 : 2;                                               // ACCext01 = ACC1[47:32]:ACC0[47:32]; ACCext23 = ACC3:ACC2
        if (sub == 0) begin
          e = ea(mode, int'(op & 7), 4); if (trap != 0) return; v = ea_read(e, 4);
          acc[p] = sext48((acc[p] & 64'hFFFFFFFF) | (longint'(64'(v & 32'hFFFF)) << 32));
          acc[p + 1] = sext48((acc[p + 1] & 64'hFFFFFFFF) | (longint'(64'(v >> 16)) << 32));
        end else if (sub == 2) gw(int'(op & 15), (u32_t'((acc[p + 1] >>> 32) & 64'hFFFF) << 16) | u32_t'((acc[p] >>> 32) & 64'hFFFF));
        else illegal();
      end
      return;
    end
    n = int'((op >> 9) & 3); sub = int'((op >> 6) & 3);                   // ACCn: MOVE to/from, MOVCLR
    if (sub == 0 && (op & 32'hFC) == 32'h10) begin emac_copy(n, int'(op & 3)); return; end       // MOVE.L ACCy,ACCx
    if (sub == 0) begin e = ea(mode, int'(op & 7), 4); if (trap != 0) return; emac_set(n, ea_read(e, 4)); return; end   // MOVE.L <ea>,ACCn
    if (sub == 2) begin gw(int'(op & 15), emac_store(n)); return; end                           // MOVE.L ACCn,Rx
    if (sub == 3) begin gw(int'(op & 15), emac_store(n)); acc[n] = 0; macsr &= ~(32'h100 << n); return; end   // MOVCLR
    illegal();
  endfunction

  function void exec_e(u32_t op, int mode);
    int size, sz, rg, typ, cnt; u32_t r; ea_t e;
    if (((op >> 6) & 3) == 3) begin                                       // deslocamento/rotação de 1 bit em memória (palavra)
      if ((op & 32'h0800) != 0) begin illegal(); return; end
      e = ea(mode, int'(op & 7), 2); if (trap != 0) return; if (e.k == 0 || e.k == 1 || e.k == 3) begin illegal(); return; end
      r = do_shift(ea_read(e, 2), int'((op >> 9) & 3), op[8], 1, 2);
      ea_write(e, 2, r);
      return;
    end
    size = int'((op >> 6) & 3); sz = 1 << size; rg = int'(op & 7); typ = int'((op >> 3) & 3);
    cnt = int'((op >> 9) & 7);
    if ((op & 32'h20) != 0) cnt = int'(d[cnt] & 63); else if (cnt == 0) cnt = 8;
    r = do_shift(d[rg], typ, op[8], cnt, sz);
    d[rg] = sz == 4 ? r : (d[rg] & ~smask(sz)) | r;
  endfunction

  // ---------------- eMAC (EMAC_B) ----------------
  // Acumuladores de 48 bits guardados como inteiro de 64 bits estendido por sinal do bit 47 (ver src/cpu_cf.cpp).
  function bit emac_fits(longint v);
    bit uns, frac; int bits;
    uns = macsr[6]; frac = macsr[5]; bits = frac ? 40 : 32;
    if (uns) return v >= 0 && v < (64'sd1 <<< bits);
    return v >= -(64'sd1 <<< (bits - 1)) && v < (64'sd1 <<< (bits - 1));
  endfunction
  function prod_t emac_product(u32_t xv, u32_t yv, bit word, int scale);
    bit uns, frac; logic signed [127:0] a_, b_, p; prod_t r;
    uns = macsr[6]; frac = macsr[5];
    if (uns) begin a_ = word ? 128'(xv[15:0]) : 128'(xv); b_ = word ? 128'(yv[15:0]) : 128'(yv); end
    else begin
      a_ = word ? 128'(signed'(xv[15:0])) : 128'(signed'(xv)); b_ = word ? 128'(signed'(yv[15:0])) : 128'(signed'(yv));
    end
    p = a_ * b_;
    if (frac) begin
      p = word ? p * 512 : p / (128'sd1 <<< 23);                        // fator de escala ignorado no modo fracionário
      r.ovf = uns ? (p < 0 || p >= (128'sd1 <<< 40)) : (p < -(128'sd1 <<< 39) || p >= (128'sd1 <<< 39));
    end else begin
      if (scale == 1) p = p <<< 1; else if (scale == 3) p = p >>> 1;
      r.ovf = uns ? (p < 0 || p >= (128'sd1 <<< 32)) : (p < -(128'sd1 <<< 31) || p >= (128'sd1 <<< 31));
      p = uns ? 128'(p[31:0]) : 128'(signed'(p[31:0]));                 // produto de 32 bits truncado e estendido
    end
    r.v = longint'(p[63:0]);
    return r;
  endfunction
  function bit emac_add(int n, prod_t p, bit sub, bit setflags);
    longint s; bit ev;
    s = acc[n] + (sub ? -p.v : p.v);
    ev = !emac_fits(s);
    if (p.ovf || ev) macsr |= 32'h100 << n;
    acc[n] = sext48(s);
    if (setflags) emac_flags(n, ev);
    return ev;
  endfunction
  function void emac_flags(int n, bit ev);
    macsr &= ~32'hF;
    if (acc[n] == 0) macsr |= 4;
    if (acc[n] < 0) macsr |= 8;
    if ((macsr & (32'h100 << n)) != 0) macsr |= 2;
    if (ev) macsr |= 1;
  endfunction
  function void emac_set(int n, u32_t v);
    longint raw;
    if (macsr[5]) raw = longint'(64'(v) | (v[31] ? (64'hFF << 40) : 64'd0));
    else if (macsr[6]) raw = longint'(64'(v));
    else raw = longint'(int'(v));
    acc[n] = sext48(raw);
    macsr &= ~(32'h100 << n);
    emac_flags(n, 0);
    macsr &= ~32'h2;
  endfunction
  function bit fitsb(longint t, int bits); return t >= -(64'sd1 <<< (bits - 1)) && t < (64'sd1 <<< (bits - 1)); endfunction
  function u32_t emac_store(int n);
    longint v, t; bit uns, frac, sat, rnd;
    v = acc[n]; uns = macsr[6]; frac = macsr[5]; sat = macsr[7]; rnd = macsr[4];
    if (!frac) begin
      if (!uns) begin
        if (!sat || fitsb(v, 32)) return u32_t'(v);
        return v >= 0 ? 32'h7FFFFFFF : 32'h80000000;
      end
      if (!sat || (v >= 0 && v < (64'sd1 <<< 32))) return u32_t'(v);
      return 32'hFFFFFFFF;
    end
    if (uns && !sat) return u32_t'(((v + (64'sd1 <<< 23)) >>> 24) & 64'hFFFF);   // 16 bits: ACC[39:24] arredondado
    if (uns) begin
      t = (v + (64'sd1 <<< 23)) >>> 24;
      if (fitsb(t, 16)) return u32_t'(t & 64'hFFFF);
      return t >= 0 ? 32'h7FFF : 32'h8000;
    end
    if (!sat) return rnd ? u32_t'((v + 64'sh80) >>> 8) : u32_t'(v >>> 8);
    if (!rnd) return fitsb(v, 40) ? u32_t'(v >>> 8) : (v >= 0 ? 32'h7FFFFFFF : 32'h80000000);
    t = (v + 64'sh80) >>> 8;                                                    // Temp[47:8] arredondado por [7:0]
    if (fitsb(t, 32)) return u32_t'(t);
    return t >= 0 ? 32'h7FFFFFFF : 32'h80000000;
  endfunction
  function void emac_mac(u32_t op);
    bit load, sub, word; u32_t e, xv, yv; int rwr, rxr, ryr, accx, dual, accw, scale; prod_t p; ea_t m;
    load = (op & 32'h30) != 0;
    e = fetch();
    rwr = int'(((op >> 9) & 7) | (((op >> 6) & 1) << 3));
    rxr = load ? int'((e >> 12) & 15) : rwr;
    ryr = load ? int'(e & 15) : int'(op & 15);
    accx = int'((((e >> 4) & 1) << 1) | (load ? (((op >> 7) & 1) ^ 1) : ((op >> 7) & 1)));
    dual = int'(e & 3); accw = int'((e >> 2) & 3); scale = int'((e >> 9) & 3);
    sub = e[8]; word = !e[11];
    if (scale == 2 || dual == 2 || (load && dual != 0)) begin illegal(); return; end
    xv = gr(rxr); yv = gr(ryr);
    if (word) begin
      xv = e[7] ? xv >> 16 : xv & 32'hFFFF;
      yv = e[6] ? yv >> 16 : yv & 32'hFFFF;
    end
    p = emac_product(xv, yv, word, scale);
    void'(emac_add(accx, p, sub, 1));
    if (dual != 0) void'(emac_add(accw, p, dual == 3, 0));
    cyc += 2;
    if (load) begin                                                       // carga em paralelo, opcionalmente ANDed com MASK
      m = ea(int'((op >> 3) & 7), int'(op & 7), 4); if (trap != 0) return;
      if (m.k != 2) begin illegal(); return; end
      if (e[5]) m.addr &= 32'hFFFF0000 | mask;
      gw(rwr, rd(m.addr, 4));
    end
  endfunction
  function void emac_copy(int x, int y);
    bit pav, ev;
    pav = (macsr & (32'h100 << y)) != 0;
    ev = !emac_fits(acc[y]);
    acc[x] = acc[y];
    macsr &= ~(32'h100 << x);
    if (pav) macsr |= 32'h100 << x;
    emac_flags(x, ev);
  endfunction

  // ---------------- FPU (subconjunto IEEE double; as contas real são double IEEE do hospedeiro) ----------------
  function bit isnan(real v); logic [63:0] b; b = $realtobits(v); return b[62:52] == 11'h7FF && b[51:0] != 0; endfunction   // (v != v é dobrado para 0 pelo Verilator)
  function void fp_cc(real v); fp_nan = isnan(v); fp_n = !fp_nan && $realtobits(v) >> 63 != 0 && v != 0; fp_z = v == 0; endfunction
  function u32_t fpsr(); return (fp_n ? 32'd1 << 27 : 0) | (fp_z ? 32'd1 << 26 : 0) | (fp_nan ? 32'd1 << 24 : 0); endfunction
  function real fp_round(real v);
    case ((fpcr >> 4) & 3) 1: return f64_op(0, v); 2: return f64_op(1, v); 3: return f64_op(2, v); default: return f64_op(3, v); endcase
  endfunction
  function real fp_load(int fmt, ea_t e);
    longint unsigned hi, lo;
    case (fmt)
      0: return real'(int'(ea_read(e, 4)));
      1: return f32_to_f64(ea_read(e, 4));
      4: return real'(int'(shortint'(ea_read(e, 2))));
      6: return real'(int'(byte'(ea_read(e, 1))));
      5: begin if (e.k != 2) begin illegal(); return 0; end hi = 64'(rd(e.addr, 4)); lo = 64'(rd(e.addr + 4, 4)); return $bitstoreal((hi << 32) | lo); end
      default: begin illegal(); return 0; end
    endcase
  endfunction
  function longint toint(real x, real lo, real hi);
    if (isnan(x)) return 0;
    x = fp_round(x); if (x < lo) x = lo; if (hi < x) x = hi;           // std::min(std::max(x, lo), hi): x já é inteiro
    return longint'(x);
  endfunction
  function void fp_store(int fmt, ea_t e, real v);
    longint unsigned b;
    case (fmt)
      0: ea_write(e, 4, u32_t'(toint(v, -2147483648.0, 2147483647.0)));
      1: ea_write(e, 4, f64_to_f32(v));
      4: ea_write(e, 2, u32_t'(toint(v, -32768.0, 32767.0)) & 32'hFFFF);
      6: ea_write(e, 1, u32_t'(toint(v, -128.0, 127.0)) & 32'hFF);
      5: begin if (e.k != 2) begin illegal(); return; end b = $realtobits(v); wr(e.addr, 4, u32_t'(b >> 32)); wr(e.addr + 4, 4, u32_t'(b)); end
      default: illegal();
    endcase
  endfunction
  function bit fbcond(int c);
    bit n_, z_, u;
    n_ = fp_n; z_ = fp_z; u = fp_nan;
    case (c & 15)                                                         // 0x10-0x1F: mesmas condições (BSUN ignorado)
      0: return 0;               1: return z_;
      2: return !(u || z_ || n_); 3: return z_ || !(u || n_);
      4: return n_ && !(u || z_); 5: return z_ || (n_ && !u);
      6: return !(u || z_);       7: return !u;
      8: return u;                9: return u || z_;
      10: return u || !(n_ || z_); 11: return u || z_ || !n_;
      12: return u || (n_ && !z_); 13: return u || z_ || n_;
      14: return !z_;             default: return 1;
    endcase
  endfunction

  function void exec_fpu(u32_t op);
    int typ, mode, rg, kind, sel, fmt, sz, dst, opm, base; u32_t base_pc, cw, x, v, st_, ad; int disp; bit pred, taken, single, store, rounds;
    real src, r; longint unsigned hi, lo, b; ea_t e;
    if ((op & 32'hFE00) != 32'hF200) begin illegal(); return; end
    typ = int'((op >> 6) & 7); mode = int'((op >> 3) & 7); rg = int'(op & 7);
    if (typ == 2 || typ == 3) begin                                       // FBcc
      base_pc = pc; disp = typ == 2 ? int'(shortint'(fetch())) : int'(fetch32());
      pred = disp < 0; taken = fbcond(int'(op & 32'h3F));
      if (taken) begin pc = base_pc + u32_t'(disp); cyc += pred ? 1 : 5; end else cyc += pred ? 5 : 0;
      return;
    end
    if (typ == 1) begin                                                   // FScc <ea>
      if (mode == 1) begin illegal(); return; end
      cw = fetch(); e = ea(mode, rg, 1); if (trap != 0) return; if (e.k == 3) begin illegal(); return; end
      ea_write(e, 1, fbcond(int'(cw & 32'h3F)) ? 32'hFF : 0); return;
    end
    if (typ == 4 || typ == 5) begin                                       // FSAVE / FRESTORE (quadro de 16 bytes; nulo = reset)
      if (!priv()) return; e = ea(mode, rg, 16); if (trap != 0) return; if (e.k != 2) begin illegal(); return; end
      if (typ == 4) begin wr(e.addr, 4, 32'h41000000); wr(e.addr + 4, 4, fpcr); wr(e.addr + 8, 4, fpsr()); wr(e.addr + 12, 4, fpiar); end
      else if (rd(e.addr, 4) == 0) begin fpcr = 0; fpiar = 0; fp_n = 0; fp_z = 0; fp_nan = 0; for (int i = 0; i < 8; i++) fp[i] = 0; end
      else begin fpcr = rd(e.addr + 4, 4) & 32'hFF; st_ = rd(e.addr + 8, 4); fp_n = st_[27]; fp_z = st_[26]; fp_nan = st_[24]; fpiar = rd(e.addr + 12, 4); end
      return;
    end
    if (typ != 0) begin illegal(); return; end
    x = fetch(); kind = int'(x >> 13);
    if (kind == 3) begin                                                  // FMOVE FPn,<ea>
      fmt = int'((x >> 10) & 7);
      e = ea(mode, rg, fmt == 5 ? 8 : fmt == 4 ? 2 : fmt == 6 ? 1 : 4); if (trap != 0) return;
      fp_store(fmt, e, fp[(x >> 7) & 7]); return;
    end
    if (kind == 4 || kind == 5) begin                                     // FMOVE <ea>,FPCR/FPSR/FPIAR e o inverso
      sel = int'((x >> 10) & 7); if (sel != 4 && sel != 2 && sel != 1) begin illegal(); return; end
      e = ea(mode, rg, 4); if (trap != 0) return;
      if (kind == 4) begin
        v = ea_read(e, 4);
        if (sel == 4) fpcr = v & 32'hFF; else if (sel == 1) fpiar = v;
        else begin fp_n = v[27]; fp_z = v[26]; fp_nan = v[24]; end
      end else begin
        if (e.k == 3) begin illegal(); return; end
        ea_write(e, 4, sel == 4 ? fpcr : sel == 1 ? fpiar : fpsr());
      end
      return;
    end
    if (kind == 6 || kind == 7) begin                                     // FMOVEM lista estática (FP0 = bit 7)
      if (((x >> 11) & 3) != 2) begin illegal(); return; end
      e = ea(mode, rg, 8); if (trap != 0) return; if (e.k != 2) begin illegal(); return; end
      ad = e.addr;
      for (int i = 0; i < 8; i++) if ((x & (32'h80 >> i)) != 0) begin
        if (kind == 6) begin hi = 64'(rd(ad, 4)); lo = 64'(rd(ad + 4, 4)); fp[i] = $bitstoreal((hi << 32) | lo); end
        else begin b = $realtobits(fp[i]); wr(ad, 4, u32_t'(b >> 32)); wr(ad + 4, 4, u32_t'(b)); end
        ad += 8;
      end
      return;
    end
    if (kind != 0 && kind != 2) begin illegal(); return; end
    dst = int'((x >> 7) & 7); opm = int'(x & 32'h7F);
    if (kind == 0) src = fp[(x >> 10) & 7];
    else begin
      fmt = int'((x >> 10) & 7); sz = fmt == 5 ? 8 : fmt == 4 ? 2 : fmt == 6 ? 1 : 4;
      e = ea(mode, rg, sz); if (trap != 0) return; src = fp_load(fmt, e); if (trap != 0) return;
    end
    // Variantes de precisão explícita: FSxxx arredonda para simples, FDxxx para dupla.
    base = opm; single = (fpcr & 32'hC0) == 32'h40;
    if (opm >= 32'h40) begin
      if (opm == 32'h41 || opm == 32'h45) begin single = opm == 32'h41; base = 4; end
      else begin single = (opm & 4) == 0; base = opm & ~32'h44; end
    end
    store = 1; rounds = 1;
    case (base)
      32'h00: r = src;
      32'h01: begin r = fp_round(src); rounds = 0; cyc += 2; end                     // FINT
      32'h03: begin r = f64_op(0, src); rounds = 0; end                               // FINTRZ
      32'h04: begin r = f64_op(4, src); cyc += 27; end
      32'h18: r = f64_op(5, src);
      32'h1A: r = -src;
      32'h20: begin r = fp[dst] / src; cyc += 22; end
      32'h22: begin r = fp[dst] + src; cyc += 2; end
      32'h23: begin r = fp[dst] * src; cyc += 3; end
      32'h28: begin r = fp[dst] - src; cyc += 2; end
      32'h38: begin r = fp[dst] - src; store = 0; cyc += 2; end                       // FCMP
      32'h3A: begin r = src; store = 0; end                                           // FTST
      default: begin illegal(); return; end
    endcase
    if (rounds && single && store) r = f32_to_f64(f64_to_f32(r));
    if (store) fp[dst] = r;
    fp_cc(r);
  endfunction
  endclass
endpackage

module sf_cpu;
  import fabric::*; import sf_cpu_pkg::*;
  cf_core core;
  export "DPI-C" function cpu_bind;
  export "DPI-C" function cpu_set_state;
  export "DPI-C" function cpu_step;
  export "DPI-C" function cpu_reset;
  function automatic void cpu_bind(chandle hh, int dv); core = new; core.attach(hh, dv); endfunction
  function automatic void cpu_set_state(u32_t d0, u32_t d1, u32_t d2, u32_t d3, u32_t d4, u32_t d5, u32_t d6, u32_t d7,
      u32_t a0, u32_t a1, u32_t a2, u32_t a3, u32_t a4, u32_t a5, u32_t a6, u32_t a7, u32_t pc_, u32_t sr_, u32_t vbr_,
      real f0, real f1, real f2, real f3, real f4, real f5, real f6, real f7, bit fn, bit fz, bit fnan, u32_t fpcr_, u32_t fpiar_,
      longint c0, longint c1, longint c2, longint c3, u32_t macsr_, u32_t mask_, u32_t cacr_, u32_t osp, bit hl, bit stp, int rsn,
      int ev, u32_t epc, u32_t eaddr, longint unsigned cy, longint unsigned ins);
    core.set_state(d0, d1, d2, d3, d4, d5, d6, d7, a0, a1, a2, a3, a4, a5, a6, a7, pc_, sr_, vbr_, f0, f1, f2, f3, f4, f5, f6, f7,
                   fn, fz, fnan, fpcr_, fpiar_, c0, c1, c2, c3, macsr_, mask_, cacr_, osp, hl, stp, rsn, ev, epc, eaddr, cy, ins);
  endfunction
  function automatic int cpu_step(); return core.step(); endfunction
  function automatic void cpu_reset(); core.reset(); endfunction
endmodule
