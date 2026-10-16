#include "cpu68k.h"

namespace sf {

static inline u32 smask(int s) { return s == 1 ? 0xFFu : s == 2 ? 0xFFFFu : 0xFFFFFFFFu; }
static inline u32 smsb(int s) { return s == 1 ? 0x80u : s == 2 ? 0x8000u : 0x80000000u; }
static inline u32 sext(u32 v, int s) { return s == 1 ? (u32)(i32)(i8)v : s == 2 ? (u32)(i32)(i16)v : v; }

void Cpu68k::reset_regs() { for (int i = 0; i < 8; i++) d[i] = a[i] = 0; sr = 0x2700; usp = ssp = 0; stopped = halted = false; }
void Cpu68k::reset() { reset_regs(); a[7] = read32(vbr); ssp = a[7]; pc = read32(vbr + 4); }

void Cpu68k::set_sr(u32 v) {
  v &= 0xA71F;
  if ((v ^ sr) & 0x2000) { if (sr & 0x2000) { ssp = a[7]; a[7] = usp; } else { usp = a[7]; a[7] = ssp; } }
  sr = (u16)v;
}

// ---------------- acesso à memória (4 ciclos por acesso de 16 bits) ----------------
u16 Cpu68k::rd16(u32 ad) { ad &= addr_mask(); if ((ad & 1) && !m68020) throw AddrErr{ad, false, false}; cyc += 4; return mem.read16(ad); }
u8 Cpu68k::rd8(u32 ad) { ad &= addr_mask(); cyc += 4; return mem.read8(ad); }
void Cpu68k::wr16(u32 ad, u16 v) { ad &= addr_mask(); if ((ad & 1) && !m68020) throw AddrErr{ad, true, false}; cyc += 4; mem.write16(ad, v); }
void Cpu68k::wr8(u32 ad, u8 v) { ad &= addr_mask(); cyc += 4; mem.write8(ad, v); }

// ---------------- endereçamento ----------------
u32 Cpu68k::index_ext(u32 base) {
  u16 e = fetch(); int r = (e >> 12) & 15; u32 idx = r < 8 ? d[r] : a[r - 8];
  if (!(e & 0x800)) idx = (u32)(i32)(i16)idx;
  if (m68020) idx <<= (e >> 9) & 3;
  if (!m68020 || !(e & 0x100)) return base + (u32)(i32)(i8)e + idx;
  bool bs = e & 0x80, is = e & 0x40; int bds = (e >> 4) & 3, iis = e & 7;
  if (bds == 0) throw IllegalI();
  u32 bd = bds == 1 ? 0 : bds == 2 ? (u32)(i32)(i16)fetch() : fetch32();
  u32 b = bs ? 0 : base; if (is) idx = 0;
  int ods = iis & 3; u32 od = ods == 2 ? (u32)(i32)(i16)fetch() : ods == 3 ? fetch32() : 0;
  if (!is) {
    if (iis == 0) return b + bd + idx;
    if (iis == 4) throw IllegalI();
    if (!(iis & 4)) return read32(b + bd + idx) + od;
    return read32(b + bd) + idx + od;
  }
  if (iis == 0) return b + bd;
  if (iis > 3) throw IllegalI();
  return read32(b + bd) + od;
}

Cpu68k::EA Cpu68k::ea(int mode, int reg, int size) {
  EA e; e.reg = reg;
  switch (mode) {
    case 0: e.kind = 0; break;
    case 1: e.kind = 1; break;
    case 2: e.kind = 2; e.addr = a[reg]; break;
    case 3: e.kind = 2; e.addr = a[reg]; a[reg] += (reg == 7 && size == 1) ? 2 : size; break;
    case 4: a[reg] -= (reg == 7 && size == 1) ? 2 : size; e.kind = 2; e.addr = a[reg]; cyc += 2; break;
    case 5: { i16 dd = (i16)fetch(); e.kind = 2; e.addr = a[reg] + (u32)(i32)dd; break; }
    case 6: e.kind = 2; e.addr = index_ext(a[reg]); cyc += 2; break;
    case 7:
      switch (reg) {
        case 0: e.kind = 2; e.addr = (u32)(i32)(i16)fetch(); break;
        case 1: e.kind = 2; e.addr = fetch32(); break;
        case 2: { u32 base = pc; i16 dd = (i16)fetch(); e.kind = 2; e.addr = base + (u32)(i32)dd; break; }
        case 3: { u32 base = pc; e.kind = 2; e.addr = index_ext(base); cyc += 2; break; }
        case 4: e.kind = 3; if (size == 4) e.val = fetch32(); else { u16 w = fetch(); e.val = size == 1 ? (w & 0xFF) : w; } break;
        default: throw IllegalI();
      }
      break;
    default: throw IllegalI();
  }
  return e;
}
u32 Cpu68k::ea_read(EA& e, int size) {
  switch (e.kind) {
    case 0: return d[e.reg] & smask(size);
    case 1: return a[e.reg] & smask(size);
    case 2: return rd(e.addr, size);
    default: return e.val;
  }
}
void Cpu68k::ea_write(EA& e, int size, u32 v) {
  switch (e.kind) {
    case 0: d[e.reg] = size == 4 ? v : (d[e.reg] & ~smask(size)) | (v & smask(size)); break;
    case 1: a[e.reg] = v; break;
    case 2: wr(e.addr, size, v); break;
    default: throw IllegalI();
  }
}

bool Cpu68k::cond(int cc) const {
  bool c = sr & C, v = sr & V, z = sr & Z, n = sr & N;
  switch (cc) {
    case 0: return true; case 1: return false; case 2: return !c && !z; case 3: return c || z;
    case 4: return !c; case 5: return c; case 6: return !z; case 7: return z;
    case 8: return !v; case 9: return v; case 10: return !n; case 11: return n;
    case 12: return n == v; case 13: return n != v; case 14: return !z && (n == v); default: return z || (n != v);
  }
}

// ---------------- aritmética e flags ----------------
void Cpu68k::setnz(u32 r, int size) { r &= smask(size); ccr_set((r == 0 ? Z : 0) | ((r & smsb(size)) ? N : 0), N | Z | V | C); }

u32 Cpu68k::do_add(u32 s, u32 dst, int size, bool x) {
  u32 m = smask(size); s &= m; dst &= m; u32 xin = (x && (sr & X)) ? 1 : 0;
  u64 r = (u64)s + dst + xin; u32 res = (u32)r & m; u32 f = 0;
  if (r > m) f |= C | X; if (((s ^ res) & (dst ^ res)) & smsb(size)) f |= V; if (res & smsb(size)) f |= N;
  if (res == 0) { f |= Z; if (x) f = (f & ~Z) | (sr & Z); } else if (x) f &= ~Z;
  ccr_set(f, C | V | N | X | Z);
  return res;
}
u32 Cpu68k::do_sub(u32 s, u32 dst, int size, bool x, bool cmp) {
  u32 m = smask(size); s &= m; dst &= m; u32 xin = (x && (sr & X)) ? 1 : 0;
  u32 res = (dst - s - xin) & m; u32 f = 0;
  if ((u64)dst < (u64)s + xin) f |= C | X; if (((dst ^ s) & (dst ^ res)) & smsb(size)) f |= V; if (res & smsb(size)) f |= N;
  if (res == 0) { f |= Z; if (x) f = (f & ~Z) | (sr & Z); } else if (x) f &= ~Z;
  if (cmp) { f &= ~X; ccr_set(f, C | V | N | Z); }                 // CMP não altera X
  else ccr_set(f, C | V | N | X | Z);
  return res;
}

u32 Cpu68k::do_shift(int type, bool left, u32 val, int cnt, int size) {
  u32 m = smask(size), sg = smsb(size); val &= m; u32 c = 0, xf = (sr & X) ? 1 : 0; bool v = false;
  if (cnt == 0) { ccr_set((val == 0 ? Z : 0) | ((val & sg) ? N : 0) | ((type == 2 && xf) ? C : 0), N | Z | V | C); return val; }   // X intacto
  for (int i = 0; i < cnt; i++) {
    if (left) {
      c = (val & sg) != 0;
      if (type == 2) { u32 nv = ((val << 1) | xf) & m; xf = c; val = nv; }
      else if (type == 3) val = ((val << 1) | c) & m;
      else { u32 nv = (val << 1) & m; if (type == 0 && ((nv ^ val) & sg)) v = true; val = nv; }
    } else {
      c = val & 1;
      if (type == 0) val = (val >> 1) | (val & sg);
      else if (type == 1) val >>= 1;
      else if (type == 2) { val = (val >> 1) | (xf ? sg : 0); xf = c; }
      else val = (val >> 1) | (c ? sg : 0);
    }
  }
  u32 f = (val == 0 ? Z : 0) | ((val & sg) ? N : 0) | (c ? C : 0) | (v ? V : 0) | ((type != 3 && c) ? X : 0);
  ccr_set(f, N | Z | V | C | (type == 3 ? 0 : X));
  return val;
}

u32 Cpu68k::bcd_add(u32 s, u32 dst) {
  u32 xin = (sr & X) ? 1 : 0; u32 lo = (dst & 0xF) + (s & 0xF) + xin, res = (dst & 0xF0) + (s & 0xF0) + lo; bool carry = false;
  if (lo > 9) res += 6;
  if ((res & 0x1F0) > 0x90) { res += 0x60; carry = true; }
  res &= 0xFF; ccr_set(carry ? (C | X) : 0, C | X); if (res != 0) ccr_set(0, Z);
  return res;
}
u32 Cpu68k::bcd_sub(u32 s, u32 dst) {
  u32 xin = (sr & X) ? 1 : 0; bool lo_borrow = (dst & 0xF) < ((s & 0xF) + xin); int res = (int)dst - (int)s - (int)xin; bool borrow = false;
  if (lo_borrow) res -= 6;
  if (res < 0) { res -= 0x60; borrow = true; }
  u32 r = (u32)res & 0xFF; ccr_set(borrow ? (C | X) : 0, C | X); if (r != 0) ccr_set(0, Z);
  return r;
}

// ---------------- exceções ----------------
void Cpu68k::exception(int vec, u32 retpc, int newmask) {
  u16 old = sr; set_sr((sr | 0x2000) & ~0x8000u);
  if (newmask >= 0) sr = (u16)((sr & ~0x700) | (newmask << 8));
  push32(retpc); push16(old);
  u32 target = read32(vbr + (u32)vec * 4);
  if (target == 0 && vec != 25) { /* vetor não inicializado: segue assim mesmo (software Amiga costuma zerar) */ }
  pc = target; cyc += 34;
}
void Cpu68k::address_error(const AddrErr& e, u32 start_pc, u16 ir) {
  u16 old = sr; set_sr((sr | 0x2000) & ~0x8000u);
  push32(start_pc); push16(old); push16(ir); push32(e.addr); push16((u16)((e.write ? 0 : 0x10) | (e.instr ? 6 : 5)));
  pc = read32(vbr + 3 * 4); cyc += 50;
}

int Cpu68k::step() {
  if (halted) return 0;
  int lvl = mem.irq_level(), mask = (sr >> 8) & 7;
  if (lvl > mask) {
    stopped = false; cyc = 0; mem.irq_ack(lvl);
    try { exception(24 + lvl, pc, lvl); } catch (AddrErr&) { halted = true; return 0; }
    cycles += cyc; return cyc;
  }
  if (stopped) return 0;
  u32 start = pc; cyc = 0; bool trace = sr & 0x8000; u16 ir = 0;
  try { ir = rd16(pc); exec(); }
  catch (IllegalI&) { pc = start; try { u16 op = rd16(start); exception((op >> 12) == 0xA ? 10 : (op >> 12) == 0xF ? 11 : 4, start); } catch (AddrErr&) { halted = true; } }
  catch (PrivI&) { pc = start; exception(8, start); }
  catch (AddrErr& e) { try { address_error(e, start, ir); } catch (AddrErr&) { halted = true; } }
  if (trace && !halted && !stopped) exception(9, pc);
  instrs++; cycles += cyc; return cyc;
}

// ---------------- execução ----------------
void Cpu68k::exec() {
  u16 op = fetch(); int grp = op >> 12, mode = (op >> 3) & 7, reg = op & 7, dn = (op >> 9) & 7;
  switch (grp) {
    case 0x0: {
      if ((op & 0x138) == 0x108) {                                      // MOVEP
        i16 dd = (i16)fetch(); u32 ad = a[reg] + (u32)(i32)dd; int opm = (op >> 6) & 7; bool lng = opm & 1;
        if (!(opm & 2)) { u32 v = 0; int n = lng ? 4 : 2; for (int i = 0; i < n; i++) v = (v << 8) | rd8(ad + 2 * i); if (lng) d[dn] = v; else d[dn] = (d[dn] & 0xFFFF0000u) | (v & 0xFFFF); }
        else { u32 v = d[dn]; int n = lng ? 4 : 2; for (int i = 0; i < n; i++) wr8(ad + 2 * i, (u8)(v >> (8 * (n - 1 - i)))); }
        return;
      }
      if ((op & 0x100) || (op & 0xF00) == 0x800) {                      // BTST/BCHG/BCLR/BSET
        int tt = (op >> 6) & 3; u32 bit;
        if (op & 0x100) bit = d[dn]; else bit = fetch() & 0xFF;
        if (mode == 1) throw IllegalI();
        EA e = ea(mode, reg, 1); u32 v;
        if (e.kind == 0) { bit &= 31; v = d[e.reg]; } else { if (e.kind == 3 && tt) throw IllegalI(); bit &= 7; v = e.kind == 3 ? e.val : rd8(e.addr); }
        ccr_set(((v >> bit) & 1) ? 0 : Z, Z);
        if (tt == 1) v ^= 1u << bit; else if (tt == 2) v &= ~(1u << bit); else if (tt == 3) v |= 1u << bit;
        if (tt) { if (e.kind == 0) { d[e.reg] = v; cyc += 2; } else wr8(e.addr, (u8)v); }
        return;
      }
      int sub = (op >> 8) & 0xF;
      if ((op & 0xFF) == 0x3C || (op & 0xFF) == 0x7C) {                 // xxxI to CCR / SR
        bool is_sr = (op & 0xFF) == 0x7C; u16 imm = fetch(); if (is_sr) priv();
        u16 cur = is_sr ? sr : (sr & 0xFF); u16 r = sub == 0 ? (cur | imm) : sub == 2 ? (cur & imm) : sub == 10 ? (cur ^ imm) : (throw IllegalI(), 0);
        if (is_sr) set_sr(r); else sr = (u16)((sr & 0xFF00) | (r & 0x1F));
        cyc += 8; return;
      }
      int size = (op >> 6) & 3; if (size == 3) throw IllegalI();
      int sz = 1 << size; u32 imm; { u16 w = 0; if (sz == 4) imm = fetch32(); else { w = fetch(); imm = sz == 1 ? (w & 0xFF) : w; } }
      EA e = ea(mode, reg, sz); if (e.kind == 1 || e.kind == 3) throw IllegalI();
      u32 x = ea_read(e, sz), r;
      switch (sub) {
        case 0x0: r = x | imm; setnz(r, sz); ea_write(e, sz, r); break;
        case 0x2: r = x & imm; setnz(r, sz); ea_write(e, sz, r); break;
        case 0x4: r = do_sub(imm, x, sz, false); ea_write(e, sz, r); break;
        case 0x6: r = do_add(imm, x, sz, false); ea_write(e, sz, r); break;
        case 0xA: r = x ^ imm; setnz(r, sz); ea_write(e, sz, r); break;
        case 0xC: do_sub(imm, x, sz, false, true); break;
        default: throw IllegalI();
      }
      if (e.kind == 0 && sz == 4) cyc += 4;
      return;
    }
    case 0x1: case 0x2: case 0x3: {
      int sz = grp == 1 ? 1 : grp == 3 ? 2 : 4;
      EA s = ea(mode, reg, sz); u32 v = ea_read(s, sz);
      int dm = (op >> 6) & 7, dr = dn;
      if (dm == 1) { if (sz == 1) throw IllegalI(); a[dr] = sext(v, sz); return; }
      EA t = ea(dm, dr, sz); if (t.kind == 3 || (t.kind == 1)) throw IllegalI();
      ea_write(t, sz, v); setnz(v, sz); return;
    }
    case 0x4: {
      if (op == 0x4E71) return; if (op == 0x4E75) { pc = pop32(); cyc += 4; return; }
      if (op == 0x4E73) { priv(); u16 s = pop16(); pc = pop32(); set_sr(s); cyc += 8; return; }
      if (op == 0x4E77) { u16 c = pop16(); sr = (u16)((sr & 0xFF00) | (c & 0x1F)); pc = pop32(); return; }
      if (op == 0x4E76) { if (sr & V) exception(7, pc); return; }
      if (op == 0x4E72) { priv(); set_sr(fetch()); stopped = true; return; }
      if (op == 0x4E70) { priv(); mem.reset_signal(); cyc += 128; return; }
      if (op == 0x4E74 && m68020) { i16 dd = (i16)fetch(); pc = pop32(); a[7] += (u32)(i32)dd; return; }
      if (op == 0x4AFC) throw IllegalI();
      if ((op & 0xFFF0) == 0x4E40) { exception(32 + (op & 15), pc); return; }
      if ((op & 0xFFF8) == 0x4E50) { i16 dd = (i16)fetch(); push32(a[reg]); a[reg] = a[7]; a[7] += (u32)(i32)dd; return; }
      if ((op & 0xFFF8) == 0x4808 && m68020) { i32 dd = (i32)fetch32(); push32(a[reg]); a[reg] = a[7]; a[7] += (u32)dd; return; }
      if ((op & 0xFFF8) == 0x4E58) { a[7] = a[reg]; a[reg] = pop32(); return; }
      if ((op & 0xFFF0) == 0x4E60) { priv(); if (op & 8) a[reg] = (sr & 0x2000) ? usp : a[7]; else usp = a[reg]; return; }
      if ((op & 0xFFF8) == 0x49C0 && m68020) { d[reg] = (u32)(i32)(i8)d[reg]; setnz(d[reg], 4); return; }                        // EXTB.L (antes do LEA: mesmo padrão com modo 0)
      if ((op & 0xF1C0) == 0x41C0) { EA e = ea(mode, reg, 4); if (e.kind != 2) throw IllegalI(); a[dn] = e.addr; return; }     // LEA
      if ((op & 0xF1C0) == 0x4180) {                                                                                                 // CHK.W
        EA e = ea(mode, reg, 2); i32 bound = (i16)ea_read(e, 2), v = (i16)d[dn];
        if (v < 0) { ccr_set(N, N); exception(6, pc); } else if (v > bound) { ccr_set(0, N); exception(6, pc); }
        return;
      }
      if ((op & 0xFFF8) == 0x4840) { u32 v = d[reg]; d[reg] = (v << 16) | (v >> 16); setnz(d[reg], 4); return; }                // SWAP
      if ((op & 0xFFC0) == 0x4840) { EA e = ea(mode, reg, 4); if (e.kind != 2) throw IllegalI(); push32(e.addr); return; }      // PEA
      if ((op & 0xFFF8) == 0x4880) { d[reg] = (d[reg] & 0xFFFF0000u) | (u16)(i16)(i8)d[reg]; setnz(d[reg], 2); return; }       // EXT.W
      if ((op & 0xFFF8) == 0x48C0) { d[reg] = (u32)(i32)(i16)d[reg]; setnz(d[reg], 4); return; }                                // EXT.L
      if ((op & 0xFB80) == 0x4880) {                                                                                             // MOVEM
        bool toreg = op & 0x400; int sz = (op & 0x40) ? 4 : 2; u16 mask = fetch();
        if (!toreg) {
          if (mode == 4) {                                                                                                       // predecremento: máscara invertida
            u32 ad = a[reg];
            for (int i = 0; i < 16; i++) if (mask & (1u << i)) { int r = 15 - i; u32 v = r < 8 ? d[r] : a[r - 8]; ad -= sz; wr(ad, sz, v); }
            a[reg] = ad; return;
          }
          if (mode == 0 || mode == 1 || mode == 3) throw IllegalI();
          EA e = ea(mode, reg, sz); if (e.kind != 2) throw IllegalI(); u32 ad = e.addr;
          for (int i = 0; i < 16; i++) if (mask & (1u << i)) { u32 v = i < 8 ? d[i] : a[i - 8]; wr(ad, sz, v); ad += sz; }
        } else {
          if (mode == 0 || mode == 1 || mode == 4) throw IllegalI();
          EA e = ea(mode == 3 ? 2 : mode, reg, sz); if (e.kind != 2) throw IllegalI(); u32 ad = e.addr;
          for (int i = 0; i < 16; i++) if (mask & (1u << i)) { u32 v = sz == 2 ? sext(rd(ad, 2), 2) : rd(ad, 4); (i < 8 ? d[i] : a[i - 8]) = v; ad += sz; }
          if (mode == 3) a[reg] = ad;
        }
        return;
      }
      if ((op & 0xFFC0) == 0x4C00 || (op & 0xFFC0) == 0x4C40) {                                                                  // 68020: MULx.L / DIVx.L
        if (!m68020) throw IllegalI();
        u16 x = fetch(); EA e = ea(mode, reg, 4); u32 src = ea_read(e, 4); int dl = (x >> 12) & 7, dh = x & 7; bool sg = x & 0x800, w64 = x & 0x400;
        if (op & 0x40) {
          if (src == 0) { exception(5, pc); return; }
          if (w64) {
            if (sg) { i64 num = (i64)(((u64)d[dh] << 32) | d[dl]); i64 q = num / (i32)src, r = num % (i32)src; if (q != (i64)(i32)q) { ccr_set(V, V); return; } d[dl] = (u32)q; d[dh] = (u32)r; setnz((u32)q, 4); }
            else { u64 num = ((u64)d[dh] << 32) | d[dl]; u64 q = num / src, r = num % src; if (q >> 32) { ccr_set(V, V); return; } d[dl] = (u32)q; d[dh] = (u32)r; setnz((u32)q, 4); }
          } else {
            u32 q, r;
            if (sg) { i32 n = (i32)d[dl], dv = (i32)src; if (n == INT32_MIN && dv == -1) { ccr_set(V, V); return; } q = (u32)(n / dv); r = (u32)(n % dv); } else { q = d[dl] / src; r = d[dl] % src; }
            if (dh != dl) d[dh] = r; d[dl] = q; setnz(q, 4);
          }
          cyc += 76; return;
        }
        u64 pr = sg ? (u64)((i64)(i32)d[dl] * (i64)(i32)src) : (u64)d[dl] * src; d[dl] = (u32)pr;
        if (w64) { d[dh] = (u32)(pr >> 32); ccr_set((pr == 0 ? Z : 0) | ((pr >> 63) ? N : 0), N | Z | V | C); }
        else { bool ovf = sg ? ((i64)pr != (i64)(i32)pr) : (pr >> 32) != 0; ccr_set(((u32)pr == 0 ? Z : 0) | (((u32)pr & 0x80000000u) ? N : 0) | (ovf ? V : 0), N | Z | V | C); }
        cyc += 40; return;
      }
      if ((op & 0xFFC0) == 0x4E80) { EA e = ea(mode, reg, 4); if (e.kind != 2) throw IllegalI(); push32(pc); pc = e.addr; cyc += 4; return; }     // JSR
      if ((op & 0xFFC0) == 0x4EC0) { EA e = ea(mode, reg, 4); if (e.kind != 2) throw IllegalI(); pc = e.addr; cyc += 4; return; }              // JMP
      if ((op & 0xFFC0) == 0x40C0) { if (mode == 1) throw IllegalI(); EA e = ea(mode, reg, 2); ea_write(e, 2, sr); return; }               // MOVE from SR
      if ((op & 0xFFC0) == 0x42C0 && m68020) { EA e = ea(mode, reg, 2); ea_write(e, 2, sr & 0xFF); return; }                                 // MOVE from CCR (68010+)
      if ((op & 0xFFC0) == 0x44C0) { if (mode == 1) throw IllegalI(); EA e = ea(mode, reg, 2); sr = (u16)((sr & 0xFF00) | (ea_read(e, 2) & 0x1F)); return; }   // MOVE to CCR
      if ((op & 0xFFC0) == 0x46C0) { priv(); if (mode == 1) throw IllegalI(); EA e = ea(mode, reg, 2); set_sr(ea_read(e, 2)); return; }       // MOVE to SR
      if ((op & 0xFFC0) == 0x4800) {                                                                                                         // NBCD
        EA e = ea(mode, reg, 1); if (e.kind == 1 || e.kind == 3) throw IllegalI(); u32 v = ea_read(e, 1); u32 r = bcd_sub(v, 0); ea_write(e, 1, r); return;
      }
      if ((op & 0xFFC0) == 0x4AC0) {                                                                                                         // TAS
        EA e = ea(mode, reg, 1); if (e.kind == 1 || e.kind == 3) throw IllegalI(); u32 v = ea_read(e, 1); setnz(v, 1); ea_write(e, 1, v | 0x80); return;
      }
      int size = (op >> 6) & 3;
      if (size != 3) {
        int sz = 1 << size, sub = (op >> 8) & 0xF;
        if (sub == 0x0 || sub == 0x2 || sub == 0x4 || sub == 0x6 || sub == 0xA) {                                                            // NEGX, CLR, NEG, NOT, TST
          EA e = ea(mode, reg, sz); if ((sub != 0xA && (e.kind == 1 || e.kind == 3)) || (sub == 0xA && e.kind == 1 && sz == 1)) throw IllegalI();
          if (sub == 0xA) { setnz(ea_read(e, sz), sz); return; }
          if (sub == 0x2) { if (e.kind == 2) rd(e.addr, sz); ea_write(e, sz, 0); ccr_set(Z, N | Z | V | C); return; }
          u32 v = ea_read(e, sz);
          if (sub == 0x0) { u32 r = do_sub(v, 0, sz, true); ea_write(e, sz, r); }
          else if (sub == 0x4) { u32 r = do_sub(v, 0, sz, false); ea_write(e, sz, r); }
          else { u32 r = ~v; setnz(r, sz); ea_write(e, sz, r); }
          return;
        }
      }
      throw IllegalI();
    }
    case 0x5: {
      int size = (op >> 6) & 3;
      if (size == 3) {
        int cc = (op >> 8) & 15;
        if (mode == 1) {                                                                                                                      // DBcc
          u32 base = pc; i16 dd = (i16)fetch();
          if (!cond(cc)) { u16 c = (u16)(d[reg] - 1); d[reg] = (d[reg] & 0xFFFF0000u) | c; if (c != 0xFFFF) { pc = base + (u32)(i32)dd; cyc += 2; } else cyc += 4; } else cyc += 2;
          return;
        }
        if (mode == 7 && reg >= 2 && reg <= 4 && m68020) { if (reg == 3) fetch(); if (reg == 4) fetch32(); if (reg == 2) fetch(); if (cond(cc)) exception(7, pc); return; }   // TRAPcc
        EA e = ea(mode, reg, 1); if (e.kind == 1 || e.kind == 3) throw IllegalI();
        ea_write(e, 1, cond(cc) ? 0xFF : 0); if (e.kind == 0) cyc += cond(cc) ? 2 : 0; return;
      }
      int sz = 1 << size; u32 q = dn; if (!q) q = 8; bool sub = op & 0x100;
      if (mode == 1) { a[reg] += sub ? -q : q; cyc += 4; return; }
      EA e = ea(mode, reg, sz); if (e.kind == 3) throw IllegalI();
      u32 x = ea_read(e, sz); u32 r = sub ? do_sub(q, x, sz, false) : do_add(q, x, sz, false);
      ea_write(e, sz, r); if (e.kind == 0 && sz == 4) cyc += 4; return;
    }
    case 0x6: {
      int cc = (op >> 8) & 15; u32 base = pc; i32 disp = (i8)op;
      if ((op & 0xFF) == 0) disp = (i16)fetch(); else if ((op & 0xFF) == 0xFF) { if (!m68020) throw IllegalI(); disp = (i32)fetch32(); }
      if (cc == 1) { push32(pc); pc = base + (u32)disp; cyc += 4; }
      else if (cond(cc)) { pc = base + (u32)disp; cyc += 2; }
      else cyc += 4;
      return;
    }
    case 0x7: { if (op & 0x100) throw IllegalI(); u32 v = (u32)(i32)(i8)op; d[dn] = v; setnz(v, 4); return; }
    case 0x8: case 0xC: {
      int opm = (op >> 6) & 7;
      if (opm == 3 || opm == 7) {                                                                                                             // DIVU/DIVS / MULU/MULS
        EA e = ea(mode, reg, 2); if (e.kind == 1) throw IllegalI(); u32 src = ea_read(e, 2); bool sg = opm == 7;
        if (grp == 0xC) { u32 r = sg ? (u32)((i32)(i16)d[dn] * (i32)(i16)src) : (d[dn] & 0xFFFF) * (src & 0xFFFF); d[dn] = r; setnz(r, 4); cyc += 66; return; }
        if ((src & 0xFFFF) == 0) { exception(5, pc); return; }
        if (sg) { i32 dv = (i32)d[dn], s_ = (i16)src; i64 q = (i64)dv / s_, r = (i64)dv % s_; if (q > 32767 || q < -32768) { ccr_set(V, V | C); cyc += 10; return; } d[dn] = ((u32)(r & 0xFFFF) << 16) | (u16)q; ccr_set((q == 0 ? Z : 0) | (q < 0 ? N : 0), N | Z | V | C); cyc += 150; }
        else { u32 dv = d[dn], s_ = src & 0xFFFF; u32 q = dv / s_, r = dv % s_; if (q > 0xFFFF) { ccr_set(V, V | C); cyc += 10; return; } d[dn] = (r << 16) | q; ccr_set((q == 0 ? Z : 0) | ((q & 0x8000) ? N : 0), N | Z | V | C); cyc += 136; }
        return;
      }
      if (grp == 0x8 && (op & 0x1F0) == 0x100) {                                                                                              // SBCD
        if (op & 8) { a[reg] -= 1; u32 s_ = rd8(a[reg]); a[dn] -= 1; u32 dd = rd8(a[dn]); wr8(a[dn], (u8)bcd_sub(s_, dd)); }
        else { u32 r = bcd_sub(d[reg] & 0xFF, d[dn] & 0xFF); d[dn] = (d[dn] & ~0xFFu) | r; }
        return;
      }
      if (grp == 0xC && (op & 0x1F0) == 0x100) {                                                                                              // ABCD
        if (op & 8) { a[reg] -= 1; u32 s_ = rd8(a[reg]); a[dn] -= 1; u32 dd = rd8(a[dn]); wr8(a[dn], (u8)bcd_add(s_, dd)); }
        else { u32 r = bcd_add(d[reg] & 0xFF, d[dn] & 0xFF); d[dn] = (d[dn] & ~0xFFu) | r; }
        return;
      }
      if (grp == 0xC && (op & 0x1F8) == 0x140) { std::swap(d[dn], d[reg]); return; }                                                          // EXG Dx,Dy
      if (grp == 0xC && (op & 0x1F8) == 0x148) { std::swap(a[dn], a[reg]); return; }                                                          // EXG Ax,Ay
      if (grp == 0xC && (op & 0x1F8) == 0x188) { std::swap(d[dn], a[reg]); return; }                                                          // EXG Dx,Ay
      int size = opm & 3; if (size == 3) throw IllegalI(); int sz = 1 << size;
      EA e = ea(mode, reg, sz);
      if (opm < 4) { if (e.kind == 1) throw IllegalI(); u32 x = ea_read(e, sz); u32 r = grp == 8 ? (d[dn] | x) : (d[dn] & x); setnz(r, sz); EA t; t.kind = 0; t.reg = dn; ea_write(t, sz, r); if (sz == 4 && (e.kind == 0 || e.kind == 3)) cyc += 4; }
      else { if (e.kind == 0 || e.kind == 1 || e.kind == 3) throw IllegalI(); u32 x = ea_read(e, sz); u32 r = grp == 8 ? (d[dn] | x) : (d[dn] & x); setnz(r, sz); ea_write(e, sz, r); }
      return;
    }
    case 0x9: case 0xD: case 0xB: {
      int opm = (op >> 6) & 7;
      if (opm == 3 || opm == 7) {                                                                                                             // ADDA/SUBA/CMPA
        int sz = opm == 3 ? 2 : 4; EA e = ea(mode, reg, sz); u32 s_ = sext(ea_read(e, sz), sz);
        if (grp == 0xD) a[dn] += s_; else if (grp == 0x9) a[dn] -= s_; else do_sub(s_, a[dn], 4, false, true);
        cyc += 4; return;
      }
      int size = opm & 3; if (size == 3) throw IllegalI(); int sz = 1 << size;
      if (grp == 0xB) {
        if (opm < 4) { EA e = ea(mode, reg, sz); do_sub(ea_read(e, sz), d[dn], sz, false, true); return; }                                        // CMP
        if (mode == 1) {                                                                                                                      // CMPM (Ay)+,(Ax)+
          EA s_ = ea(3, reg, sz); u32 sv = rd(s_.addr, sz); EA t = ea(3, dn, sz); u32 dv = rd(t.addr, sz); do_sub(sv, dv, sz, false, true); return;
        }
        EA e = ea(mode, reg, sz); if (e.kind == 1 || e.kind == 3) throw IllegalI();                                                           // EOR
        u32 r = ea_read(e, sz) ^ d[dn]; setnz(r, sz); ea_write(e, sz, r); return;
      }
      if (opm >= 4 && (mode == 0 || mode == 1)) {                                                                                             // ADDX/SUBX
        if (mode == 0) { u32 r = grp == 0xD ? do_add(d[reg], d[dn], sz, true) : do_sub(d[reg], d[dn], sz, true); EA t; t.kind = 0; t.reg = dn; ea_write(t, sz, r); }
        else { EA s_ = ea(4, reg, sz); u32 sv = rd(s_.addr, sz); EA t = ea(4, dn, sz); u32 dv = rd(t.addr, sz); u32 r = grp == 0xD ? do_add(sv, dv, sz, true) : do_sub(sv, dv, sz, true); wr(t.addr, sz, r); }
        return;
      }
      EA e = ea(mode, reg, sz);
      if (opm < 4) { u32 x = ea_read(e, sz); u32 r = grp == 0xD ? do_add(x, d[dn], sz, false) : do_sub(x, d[dn], sz, false); EA t; t.kind = 0; t.reg = dn; ea_write(t, sz, r); if (sz == 4 && (e.kind == 0 || e.kind == 1 || e.kind == 3)) cyc += 4; }
      else { if (e.kind == 0 || e.kind == 1 || e.kind == 3) throw IllegalI(); u32 x = ea_read(e, sz); u32 r = grp == 0xD ? do_add(d[dn], x, sz, false) : do_sub(d[dn], x, sz, false); ea_write(e, sz, r); }
      return;
    }
    case 0xE: {
      int size = (op >> 6) & 3;
      if (size == 3) {                                                                                                                       // deslocamento de memória (1 posição, word)
        if ((op & 0x0800)) throw IllegalI();                                                                                                  // campos de bits 68020: não suportados
        EA e = ea(mode, reg, 2); if (e.kind != 2) throw IllegalI();
        u32 v = rd(e.addr, 2); u32 r = do_shift((op >> 9) & 3, op & 0x100, v, 1, 2); wr(e.addr, 2, r); return;
      }
      int sz = 1 << size, type = (op >> 3) & 3; int cnt = dn; if (op & 0x20) cnt = (int)(d[cnt] & 63); else if (!cnt) cnt = 8;
      u32 r = do_shift(type, op & 0x100, d[reg], cnt, sz); d[reg] = sz == 4 ? r : (d[reg] & ~smask(sz)) | (r & smask(sz)); cyc += 2 * cnt + (sz == 4 ? 4 : 2); return;
    }
    default: throw IllegalI();                                                                                                                // linhas A e F
  }
}

}  // namespace sf
