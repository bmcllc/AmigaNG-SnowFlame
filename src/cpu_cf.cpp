#include "cpu_cf.h"

namespace sf {
using namespace abi;

static inline u32 smask(int s) { return s == 1 ? 0xFFu : s == 2 ? 0xFFFFu : 0xFFFFFFFFu; }
static inline u32 ssign(int s) { return s == 1 ? 0x80u : s == 2 ? 0x8000u : 0x80000000u; }
static inline u32 sext(u32 v, int s) { return s == 1 ? (u32)(i32)(i8)v : s == 2 ? (u32)(i32)(i16)v : v; }
static inline i64 sext48(i64 v) { return (i64)((u64)v << 16) >> 16; }   // acumulador eMAC de 48 bits

void Cpu::reset_regs() {
  for (int i = 0; i < 8; i++) { d[i] = a[i] = 0; fp[i] = 0; }
  sr = 0x2700; vbr = 0; for (auto& x : acc) x = 0; macsr = 0; mask = 0xFFFFu; cacr = 0; other_sp = 0; fpcr = fpiar = 0; fp_n = fp_z = fp_nan = false; halted = stopped = false; reason = RUNNING;
}
void Cpu::reset() {
  reset_regs();
  a[7] = bus.cpu_read(vbr, 4); pc = bus.cpu_read(vbr + 4, 4);
  if (bus.fault) { bus.fault = false; halted = true; reason = DOUBLE_FAULT; }
}

// ---------------- Endereçamento ----------------
u32 Cpu::brief(u32 base) {
  u16 e = fetch();
  int r = (e >> 12) & 15; u32 idx = r < 8 ? d[r] : a[r - 8];
  if (!(e & 0x800)) idx = (u32)(i32)(i16)idx;
  idx <<= (e >> 9) & 3;
  if (!(e & 0x100)) return base + (u32)(i32)(i8)e + idx;            // formato curto: (d8,An,Xn*SF)
  // formato completo (68020): deslocamento de base, supressão de base/índice e indireção de memória
  bool bs = e & 0x80, is = e & 0x40; int bds = (e >> 4) & 3, iis = e & 7;
  if (bds == 0 || (e & 8)) throw Illegal();
  u32 bd = bds == 1 ? 0 : bds == 2 ? (u32)(i32)(i16)fetch() : fetch32();
  u32 b = bs ? 0 : base; if (is) idx = 0;
  int ods = iis & 3; u32 od = ods == 2 ? (u32)(i32)(i16)fetch() : ods == 3 ? fetch32() : 0;
  cyc += 1;
  if (!is) {
    if (iis == 0) return b + bd + idx;
    if (iis == 4) throw Illegal();
    if (!(iis & 4)) return rd(b + bd + idx, 4) + od;               // pré-indexado
    return rd(b + bd, 4) + idx + od;                               // pós-indexado
  }
  if (iis == 0) return b + bd;
  if (iis > 3) throw Illegal();
  return rd(b + bd, 4) + od;
}
Cpu::EA Cpu::ea(int mode, int reg, int size) {
  EA e{}; e.reg = reg;
  switch (mode) {
    case 0: e.k = 0; break;
    case 1: e.k = 1; break;
    case 2: agu_use(reg); e.k = 2; e.addr = a[reg]; break;
    case 3: agu_use(reg); e.k = 2; e.addr = a[reg]; a[reg] += (reg == 7 && size == 1) ? 2 : size; break;
    case 4: agu_use(reg); a[reg] -= (reg == 7 && size == 1) ? 2 : size; e.k = 2; e.addr = a[reg]; break;
    case 5: { agu_use(reg); i16 dd = (i16)fetch(); e.k = 2; e.addr = a[reg] + (u32)(i32)dd; break; }
    case 6: agu_use(reg); e.k = 2; e.addr = brief(a[reg]); break;
    case 7:
      switch (reg) {
        case 0: e.k = 2; e.addr = (u32)(i32)(i16)fetch(); break;
        case 1: e.k = 2; e.addr = fetch32(); break;
        case 2: { u32 base = pc; i16 dd = (i16)fetch(); e.k = 2; e.addr = base + (u32)(i32)dd; break; }
        case 3: { u32 base = pc; e.k = 2; e.addr = brief(base); break; }
        case 4: e.k = 3; if (size == 4) e.imm = fetch32(); else { u16 w = fetch(); e.imm = size == 1 ? (w & 0xFF) : w; } break;
        default: throw Illegal();
      }
      break;
    default: throw Illegal();
  }
  return e;
}
u32 Cpu::ea_read(const EA& e, int size) {
  switch (e.k) {
    case 0: return d[e.reg] & smask(size);
    case 1: return size == 2 ? (u32)(i32)(i16)a[e.reg] : a[e.reg];
    case 2: return rd(e.addr, size);
    default: return e.imm;
  }
}
void Cpu::ea_write(const EA& e, int size, u32 v) {
  switch (e.k) {
    case 0: d[e.reg] = size == 4 ? v : (d[e.reg] & ~smask(size)) | (v & smask(size)); break;
    case 1: a[e.reg] = v; agu_set(e.reg); break;
    case 2: wr(e.addr, size, v); break;
    default: throw Illegal();
  }
}

// ---------------- Flags ----------------
void Cpu::setnz(u32 r, int s) {
  r &= smask(s); u32 f = 0; if (r == 0) f |= Z; if (r & ssign(s)) f |= N;
  setccr(f, N | Z | V | C);
}
u32 Cpu::do_add(u32 x, u32 y, int s, bool setx) {
  u64 r = (u64)(x & smask(s)) + (y & smask(s)); u32 res = (u32)r & smask(s); u32 f = 0;
  if (r > smask(s)) f |= C; if (((x ^ res) & (y ^ res)) & ssign(s)) f |= V;
  if (res == 0) f |= Z; if (res & ssign(s)) f |= N; if ((f & C) && setx) f |= X;
  setccr(f, setx ? (N | Z | V | C | X) : (N | Z | V | C));
  return res;
}
u32 Cpu::do_sub(u32 dst, u32 src, int s, bool setx) {
  u32 m = smask(s); u32 res = (dst - src) & m; u32 f = 0;
  if ((dst & m) < (src & m)) f |= C; if (((dst ^ src) & (dst ^ res)) & ssign(s)) f |= V;
  if (res == 0) f |= Z; if (res & ssign(s)) f |= N; if ((f & C) && setx) f |= X;
  setccr(f, setx ? (N | Z | V | C | X) : (N | Z | V | C));
  return res;
}
u32 Cpu::do_addx(u32 x, u32 y, int s) {
  u32 m = smask(s), cin = (sr & X) ? 1 : 0; u64 r = (u64)(x & m) + (y & m) + cin; u32 res = (u32)r & m; u32 f = 0;
  if (r > m) f |= C | X; if (((x ^ res) & (y ^ res)) & ssign(s)) f |= V; if (res & ssign(s)) f |= N;
  setccr(f, N | V | C | X); if (res != 0) setccr(0, Z);        // Z só é limpo, nunca ligado
  return res;
}
u32 Cpu::do_subx(u32 dst, u32 src, int s) {
  u32 m = smask(s), cin = (sr & X) ? 1 : 0; u32 res = (dst - src - cin) & m; u32 f = 0;
  if ((u64)(dst & m) < (u64)(src & m) + cin) f |= C | X; if (((dst ^ src) & (dst ^ res)) & ssign(s)) f |= V; if (res & ssign(s)) f |= N;
  setccr(f, N | V | C | X); if (res != 0) setccr(0, Z);
  return res;
}
u32 Cpu::do_shift(u32 val, int type, bool left, int cnt, int s) {
  u32 m = smask(s), sg = ssign(s); val &= m; u32 c = 0, v = 0, x = (sr & X) != 0;
  for (int i = 0; i < cnt; i++) {
    if (left) {
      if (type == 3) { c = (val & sg) != 0; val = ((val << 1) | c) & m; }
      else if (type == 2) { c = (val & sg) != 0; val = ((val << 1) | x) & m; x = c; }
      else { c = (val & sg) != 0; u32 nv = (val << 1) & m; if (type == 0 && ((nv ^ val) & sg)) v = 1; val = nv; }
    } else {
      c = val & 1;
      if (type == 0) val = (val >> 1) | (val & sg);
      else if (type == 1) val >>= 1;
      else if (type == 2) { val = (val >> 1) | (x ? sg : 0); x = c; }
      else val = (val >> 1) | (c ? sg : 0);
    }
  }
  u32 f = 0; if (val == 0) f |= Z; if (val & sg) f |= N;
  if (cnt > 0) { if (c) f |= C; if (v) f |= V; if (c && type != 3) f |= X; }
  else if (type == 2 && x) f |= C;                        // ROX com contagem 0: C = X
  setccr(f, (cnt > 0 && type != 3) ? (N | Z | V | C | X) : (N | Z | V | C));
  return val;
}
bool Cpu::cond(int cc) const {
  bool c = sr & C, v = sr & V, z = sr & Z, n = sr & N;
  switch (cc) {
    case 0: return true; case 1: return false; case 2: return !c && !z; case 3: return c || z;
    case 4: return !c; case 5: return c; case 6: return !z; case 7: return z;
    case 8: return !v; case 9: return v; case 10: return !n; case 11: return n;
    case 12: return n == v; case 13: return n != v; case 14: return !z && (n == v); default: return z || (n != v);
  }
}

// ---------------- Exceções ----------------
void Cpu::set_sr(u32 v) {
  v &= 0xFFFF; if ((v ^ sr) & 0x2000) std::swap(a[7], other_sp);   // troca USP/SSP ao mudar de modo
  sr = v;
}

void Cpu::exception(int vec, u32 retpc, int newmask) {
  u32 oldsr = sr; set_sr(sr | 0x2000); bus.mmu_bypass = true;   // pilha/vetores de exceção são acessos físicos
  if (newmask >= 0) sr = (sr & ~0x700u) | ((u32)newmask << 8);
  a[7] -= 8;
  bus.cpu_write(a[7] + 4, 4, retpc);
  bus.cpu_write(a[7], 4, (4u << 28) | ((u32)vec << 18) | (oldsr & 0xFFFF));
  u32 target = bus.cpu_read(vbr + (u32)vec * 4, 4);
  bus.mmu_bypass = false;
  if (bus.fault) { bus.fault = false; halted = true; reason = DOUBLE_FAULT; return; }
  if (target == 0) { halted = true; reason = NO_VECTOR; return; }
  pc = target;
}

int Cpu::step() {
  if (halted) return 0;
  if (bus.irq_pending() && ((sr >> 8) & 7) < (u32)IRQ_LEVEL) {
    stopped = false; cyc = 10; exception(VEC_IRQ4, pc, IRQ_LEVEL); cycles += cyc; return cyc;
  }
  if (stopped) return 0;
  u32 start = pc; cyc = 1; agu_prev = agu_cur; agu_cur = 0;
  // Com a MMU ligada o erro de acesso precisa ser reiniciável: guarda os registradores (a instrução pode ter gravado lixo lido da falha).
  u32 sd[8], sa[8], ssr = sr; bool snap = bus.mmu_on && !(bus.mmu_super_bypass && (sr & 0x2000));
  if (snap) { std::memcpy(sd, d, sizeof sd); std::memcpy(sa, a, sizeof sa); }
  try { exec(); }
  catch (Illegal&) { exception(4, start, -1); }
  catch (DivZero&) { exception(5, start, -1); }
  catch (Priv&) { exception(8, start, -1); }
  if (bus.fault) {
    bus.fault = false;
    if (snap && !((sr ^ ssr) & 0x2000)) { std::memcpy(d, sd, sizeof sd); std::memcpy(a, sa, sizeof sa); sr = (sr & ~0xFFu) | (ssr & 0xFF); }
    if (!halted) exception(2, start, -1);
  }
  instrs++; cycles += cyc; return cyc;
}

// ---------------- Execução ----------------
void Cpu::exec() {
  u16 op = fetch();
  int grp = op >> 12;
  switch (grp) {
    case 0x0: {
      int mode = (op >> 3) & 7, reg = op & 7;
      if ((op & 0x100) || (op & 0xF00) == 0x800) {          // BTST/BCHG/BCLR/BSET
        int tt = (op >> 6) & 3; u32 bit;
        if (op & 0x100) bit = d[(op >> 9) & 7]; else bit = fetch() & 0xFF;
        EA e = ea(mode, reg, 1);
        if (e.k == 1) throw Illegal();
        u32 v; if (e.k == 0) { bit &= 31; v = d[e.reg]; } else { bit &= 7; v = ea_read(e, 1); }
        setccr((v >> bit) & 1 ? 0 : Z, Z);
        if (tt == 1) v ^= 1u << bit; else if (tt == 2) v &= ~(1u << bit); else if (tt == 3) v |= 1u << bit;
        if (tt) { if (e.k == 0) d[e.reg] = v; else ea_write(e, 1, v); }
        break;
      }
      if ((op & 0xFFF8) == 0x00C0) { u32 v = d[reg], r = 0; for (int i = 0; i < 32; i++) if (v & (1u << i)) r |= 0x80000000u >> i; d[reg] = r; break; }   // BITREV
      if ((op & 0xFFF8) == 0x02C0) { u32 v = d[reg]; d[reg] = (v << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24); break; }              // BYTEREV
      if ((op & 0xFFF8) == 0x04C0) {                                                                                                                   // FF1
        u32 v = d[reg], r = 0; while (r < 32 && !(v & (0x80000000u >> r))) r++;
        d[reg] = r; setccr((v == 0 ? Z : 0) | ((v & 0x80000000u) ? N : 0), N | Z | V | C); break;
      }
      int size = (op >> 6) & 3; if (size == 3) throw Illegal();
      int sz = 1 << size; int sub = (op >> 8) & 0xF;
      u32 imm; { u16 w = 0; if (sz == 4) imm = fetch32(); else { w = fetch(); imm = sz == 1 ? (w & 0xFF) : w; } }
      EA e = ea(mode, reg, sz);
      if (e.k == 1 || e.k == 3) throw Illegal();
      u32 x = ea_read(e, sz), r;
      switch (sub) {
        case 0x0: r = x | imm; setnz(r, sz); ea_write(e, sz, r); break;
        case 0x2: r = x & imm; setnz(r, sz); ea_write(e, sz, r); break;
        case 0x4: r = do_sub(x, imm, sz); ea_write(e, sz, r); break;
        case 0x6: r = do_add(x, imm, sz); ea_write(e, sz, r); break;
        case 0xA: r = x ^ imm; setnz(r, sz); ea_write(e, sz, r); break;
        case 0xC: do_sub(x, imm, sz, false); break;
        default: throw Illegal();
      }
      break;
    }
    case 0x1: case 0x2: case 0x3: {
      int sz = grp == 1 ? 1 : grp == 3 ? 2 : 4;
      EA s = ea((op >> 3) & 7, op & 7, sz);
      u32 v = ea_read(s, sz);
      int dm = (op >> 6) & 7, dr = (op >> 9) & 7;
      if (dm == 1) { if (sz == 1) throw Illegal(); a[dr] = sz == 2 ? (u32)(i32)(i16)v : v; agu_set(dr); break; }
      EA t = ea(dm, dr, sz);
      if (t.k == 3) throw Illegal();
      ea_write(t, sz, v); setnz(v, sz);
      break;
    }
    case 0x4: {
      int mode = (op >> 3) & 7, reg = op & 7;
      if (op == 0x4E71) break;
      if (op == 0x4E75) { pc = pop32(); cyc += 3; break; }
      if (op == 0x4E73) { priv(); u32 f = pop32(); pc = pop32(); set_sr(f); break; }
      if (op == 0x4E72) { priv(); set_sr(fetch()); stopped = true; break; }
      if (op == 0x4AC8) { priv(); halted = true; reason = HALT_INSN; break; }
      if (op == 0x4AFC) throw Illegal();
      if ((op & 0xFFC0) == 0x4AC0 && mode != 1) {              // TAS
        EA e = ea(mode, reg, 1); if (e.k == 3) throw Illegal();
        u32 v = ea_read(e, 1); setnz(v, 1); ea_write(e, 1, v | 0x80); break;
      }
      if ((op & 0xFFF8) == 0x4C80) {                           // SATS.L
        if (sr & V) d[reg] = (d[reg] & 0x80000000u) ? 0x7FFFFFFFu : 0x80000000u;
        setnz(d[reg], 4); break;
      }
      if ((op & 0xFFF8) == 0x42C0) { d[reg] = (d[reg] & 0xFFFF0000u) | (sr & 0x1F); break; }   // MOVE CCR,Dn
      if ((op & 0xFFF0) == 0x4E40) { exception(32 + (op & 15), pc, -1); break; }
      if ((op & 0xFFF8) == 0x4E50) { i16 dd = (i16)fetch(); push32(a[reg]); a[reg] = a[7]; a[7] += (u32)(i32)dd; break; }
      if ((op & 0xFFF8) == 0x4E58) { a[7] = a[reg]; a[reg] = pop32(); break; }
      if (op == 0x4E7B || op == 0x4E7A) {                   // MOVEC Rn,Rc / Rc,Rn
        priv(); u16 x = fetch(); int r = (x >> 12) & 7; bool isa = x & 0x8000; u32 rc = x & 0xFFF;
        u32& rn = isa ? a[r] : d[r];
        if (op == 0x4E7B) { if (rc == 0x801) vbr = rn; else if (rc == 0x800) other_sp = rn; else if (rc == 0x002) cacr = rn; }   // demais: ignorados
        else rn = rc == 0x801 ? vbr : rc == 0x800 ? other_sp : rc == 0x002 ? cacr : 0;
        break;
      }
      if ((op & 0xFFF0) == 0x4E60) { priv(); int r = op & 7; if (op & 8) a[r] = other_sp; else other_sp = a[r]; break; }   // MOVE USP
      if ((op & 0xFFF8) == 0x40C0) { priv(); d[reg] = (d[reg] & 0xFFFF0000u) | (sr & 0xFFFF); break; }
      if ((op & 0xFFC0) == 0x46C0) { priv(); EA e = ea(mode, reg, 2); set_sr(ea_read(e, 2)); break; }
      if ((op & 0xFFC0) == 0x44C0) { EA e = ea(mode, reg, 2); setccr(ea_read(e, 2), 0x1F); break; }
      if ((op & 0xF1C0) == 0x41C0 && mode >= 2) { EA e = ea(mode, reg, 4); if (e.k != 2) throw Illegal(); a[(op >> 9) & 7] = e.addr; agu_set((op >> 9) & 7); break; }
      if ((op & 0xFFF8) == 0x4840) { u32 v = d[reg]; d[reg] = (v << 16) | (v >> 16); setnz(d[reg], 4); break; }
      if ((op & 0xFFC0) == 0x4840) { EA e = ea(mode, reg, 4); if (e.k != 2) throw Illegal(); push32(e.addr); break; }
      if ((op & 0xFFF8) == 0x4880) { d[reg] = (d[reg] & 0xFFFF0000u) | (u16)(i16)(i8)d[reg]; setnz(d[reg], 2); break; }
      if ((op & 0xFFF8) == 0x48C0) { d[reg] = (u32)(i32)(i16)d[reg]; setnz(d[reg], 4); break; }
      if ((op & 0xFFF8) == 0x49C0) { d[reg] = (u32)(i32)(i8)d[reg]; setnz(d[reg], 4); break; }
      if ((op & 0xFFC0) == 0x48C0 || (op & 0xFFC0) == 0x4CC0) {   // MOVEM.L
        bool toreg = (op & 0x400); u16 mask = fetch();
        if (mode != 2 && mode != 5) throw Illegal();
        EA e = ea(mode, reg, 4); u32 ad = e.addr;
        for (int i = 0; i < 16; i++) if (mask & (1u << i)) {
          u32& r = i < 8 ? d[i] : a[i - 8];
          if (toreg) r = rd(ad, 4); else wr(ad, 4, r);
          ad += 4;
        }
        break;
      }
      if ((op & 0xFFC0) == 0x4C00 || (op & 0xFFC0) == 0x4C40) {   // MULx.L / DIVx.L
        u16 x = fetch(); EA e = ea(mode, reg, 4); u32 src = ea_read(e, 4);
        int dl = (x >> 12) & 7; bool sg = x & 0x800;
        if (op & 0x40) {
          if (src == 0) throw DivZero();
          int dr = x & 7; u32 q, r; u32 f = 0;
          if (sg) { i32 a_ = (i32)d[dl], b_ = (i32)src; if (a_ == INT32_MIN && b_ == -1) { q = (u32)a_; r = 0; f |= V; } else { q = (u32)(a_ / b_); r = (u32)(a_ % b_); } }
          else { q = d[dl] / src; r = d[dl] % src; }
          if (dr != dl) d[dr] = r; d[dl] = q;
          if (q == 0) f |= Z; if (q & 0x80000000u) f |= N; setccr(f, N | Z | V | C); cyc += 19;
        } else {
          bool w64 = x & 0x400; int dh = x & 7; u64 pr; u32 f = 0;
          if (sg) pr = (u64)((i64)(i32)d[dl] * (i64)(i32)src); else pr = (u64)d[dl] * (u64)src;
          d[dl] = (u32)pr;
          if (w64) { d[dh] = (u32)(pr >> 32); if (pr == 0) f |= Z; if (pr >> 63) f |= N; }
          else {
            bool ovf = sg ? ((i64)pr != (i64)(i32)pr) : (pr >> 32) != 0;
            if (ovf) f |= V; if ((u32)pr == 0) f |= Z; if ((u32)pr & 0x80000000u) f |= N;
          }
          setccr(f, N | Z | V | C); cyc += 3;
        }
        break;
      }
      if ((op & 0xFFC0) == 0x4E80) { EA e = ea(mode, reg, 4); if (e.k != 2) throw Illegal(); push32(pc); pc = e.addr; cyc += 2; break; }
      if ((op & 0xFFC0) == 0x4EC0) { EA e = ea(mode, reg, 4); if (e.k != 2) throw Illegal(); pc = e.addr; cyc += 2; break; }
      int size = (op >> 6) & 3;
      if (size != 3) {
        int sz = 1 << size; int sub = (op >> 8) & 0xF;
        if (sub == 0x0) {                                    // NEGX (só Dn)
          if (mode != 0) throw Illegal();
          ea_write(EA{0, reg, 0, 0}, sz, do_subx(0, d[reg], sz)); break;
        }
        if (sub == 0x2 || sub == 0x4 || sub == 0x6 || sub == 0xA) {   // CLR, NEG, NOT, TST
          EA e = ea(mode, reg, sz);
          if (sub == 0xA) { setnz(ea_read(e, sz), sz); break; }
          if (e.k == 1 || e.k == 3) throw Illegal();
          if (sub == 0x2) { ea_write(e, sz, 0); setccr(Z, N | Z | V | C); }
          else if (sub == 0x4) { u32 r = do_sub(0, ea_read(e, sz), sz); ea_write(e, sz, r); }
          else { u32 r = ~ea_read(e, sz); setnz(r, sz); ea_write(e, sz, r); }
          break;
        }
      }
      throw Illegal();
    }
    case 0x5: {
      int mode = (op >> 3) & 7, reg = op & 7; int size = (op >> 6) & 3;
      if (op == 0x51F8) break; if (op == 0x51FA) { fetch(); break; } if (op == 0x51FB) { fetch32(); break; }   // TPF
      if (size == 3) {                                       // Scc
        EA e = ea(mode, reg, 1); if (e.k == 1) throw Illegal();
        ea_write(e, 1, cond((op >> 8) & 15) ? 0xFF : 0); break;
      }
      int sz = 1 << size; u32 q = (op >> 9) & 7; if (q == 0) q = 8; bool sub = op & 0x100;
      if (mode == 1) { a[reg] += sub ? -q : q; agu_set(reg); break; }
      EA e = ea(mode, reg, sz); if (e.k == 3) throw Illegal();
      u32 x = ea_read(e, sz); u32 r = sub ? do_sub(x, q, sz) : do_add(x, q, sz);
      ea_write(e, sz, r);
      break;
    }
    case 0x6: {
      int cc = (op >> 8) & 15; u32 base = pc; i32 disp = (i8)op;
      if ((op & 0xFF) == 0) disp = (i16)fetch(); else if ((op & 0xFF) == 0xFF) disp = (i32)fetch32();
      if (cc <= 1) { if (cc == 1) push32(pc); pc = base + (u32)disp; cyc += 2; }   // BRA/BSR: alvo vem do BTB
      else {                                                  // Bcc: predição estática (para trás = tomado); erro custa o esvaziamento do pipeline
        bool pred = disp < 0, taken = cond(cc);
        if (taken) { pc = base + (u32)disp; cyc += pred ? 1 : 5; } else cyc += pred ? 5 : 0;
      }
      break;
    }
    case 0x7: {
      if (op & 0x100) {                                      // MVS / MVZ
        int opm = (op >> 6) & 7; if (opm < 4) throw Illegal();
        int ss = (opm & 1) ? 2 : 1; EA e = ea((op >> 3) & 7, op & 7, ss); u32 v = ea_read(e, ss) & smask(ss);
        if (opm < 6) v = sext(v, ss);
        d[(op >> 9) & 7] = v; setnz(v, 4); break;
      }
      u32 v = (u32)(i32)(i8)op; d[(op >> 9) & 7] = v; setnz(v, 4); break;
    }
    case 0x8: case 0xC: {
      int opm = (op >> 6) & 7, dn = (op >> 9) & 7, mode = (op >> 3) & 7, reg = op & 7;
      if (opm == 3 || opm == 7) {
        EA e = ea(mode, reg, 2); if (e.k == 1) throw Illegal(); u32 src = ea_read(e, 2); bool sg = opm == 7;
        if (grp == 0xC) {                                    // MULU.W / MULS.W
          u32 r = sg ? (u32)((i32)(i16)d[dn] * (i32)(i16)src) : (d[dn] & 0xFFFF) * (src & 0xFFFF);
          d[dn] = r; setnz(r, 4); cyc += 2;
        } else {                                             // DIVU.W / DIVS.W
          if ((src & 0xFFFF) == 0) throw DivZero();
          u32 f = 0;
          if (sg) { i32 dv = (i32)d[dn], s_ = (i16)src; i64 q = (i64)dv / s_, r = (i64)dv % s_;
            if (q > 32767 || q < -32768) f |= V; else { d[dn] = ((u32)(r & 0xFFFF) << 16) | (u16)q; if (q == 0) f |= Z; if (q < 0) f |= N; } }
          else { u32 dv = d[dn], s_ = src & 0xFFFF; u32 q = dv / s_, r = dv % s_;
            if (q > 0xFFFF) f |= V; else { d[dn] = (r << 16) | q; if (q == 0) f |= Z; if (q & 0x8000) f |= N; } }
          setccr(f, N | Z | V | C); cyc += 18;
        }
        break;
      }
      int size = opm & 3; if (size == 3) throw Illegal(); int sz = 1 << size;
      if (opm < 4) {
        EA e = ea(mode, reg, sz); u32 x = ea_read(e, sz);
        u32 r = grp == 8 ? (d[dn] | x) : (d[dn] & x); setnz(r, sz);
        ea_write(EA{0, dn, 0, 0}, sz, r);
      } else {
        EA e = ea(mode, reg, sz); if (e.k == 0 || e.k == 1 || e.k == 3) throw Illegal();
        u32 x = ea_read(e, sz); u32 r = grp == 8 ? (d[dn] | x) : (d[dn] & x); setnz(r, sz); ea_write(e, sz, r);
      }
      break;
    }
    case 0x9: case 0xD: case 0xB: {
      int opm = (op >> 6) & 7, dn = (op >> 9) & 7, mode = (op >> 3) & 7, reg = op & 7;
      if (opm == 3 || opm == 7) {                            // ADDA / SUBA / CMPA
        int sz = opm == 3 ? 2 : 4; EA e = ea(mode, reg, sz); u32 s_ = sext(ea_read(e, sz), sz);
        if (grp == 0xD) { a[dn] += s_; agu_set(dn); } else if (grp == 0x9) { a[dn] -= s_; agu_set(dn); } else do_sub(a[dn], s_, 4, false);
        break;
      }
      int size = opm & 3; int sz = 1 << size; if (size == 3) throw Illegal();
      if (grp != 0xB && opm >= 4 && mode == 0) {             // ADDX / SUBX Dy,Dx
        u32 r = grp == 0xD ? do_addx(d[dn], d[reg], sz) : do_subx(d[dn], d[reg], sz);
        ea_write(EA{0, dn, 0, 0}, sz, r); break;
      }
      if (grp == 0xB) {
        if (opm < 4) { EA e = ea(mode, reg, sz); do_sub(d[dn], ea_read(e, sz), sz, false); }
        else {                                               // EOR Dn,<ea>
          EA e = ea(mode, reg, sz); if (e.k == 1 || e.k == 3) throw Illegal();
          u32 r = ea_read(e, sz) ^ d[dn]; setnz(r, sz); ea_write(e, sz, r);
        }
        break;
      }
      if (opm < 4) {
        EA e = ea(mode, reg, sz); u32 x = ea_read(e, sz);
        u32 r = grp == 0xD ? do_add(d[dn], x, sz) : do_sub(d[dn], x, sz);
        ea_write(EA{0, dn, 0, 0}, sz, r);
      } else {
        EA e = ea(mode, reg, sz); if (e.k == 0 || e.k == 1 || e.k == 3) throw Illegal();
        u32 x = ea_read(e, sz);
        u32 r = grp == 0xD ? do_add(x, d[dn], sz) : do_sub(x, d[dn], sz);
        ea_write(e, sz, r);
      }
      break;
    }
    case 0xA: {
      if ((op & 0x01C0) == 0x0140) {                         // MOV3Q.L #imm3,<ea>
        int q = (op >> 9) & 7; u32 v = q == 0 ? 0xFFFFFFFFu : (u32)q;
        EA e = ea((op >> 3) & 7, op & 7, 4); if (e.k == 3) throw Illegal();
        ea_write(e, 4, v); if (e.k != 1) setnz(v, 4); break;
      }
      // eMAC (CFPRM rev. 3). Bit 8 = 0: MAC/MSAC/MAAAC...; bit 8 = 1: MOV3Q, MOVE ACC/ACCext/MACSR/MASK.
      if (!(op & 0x0100)) { emac_mac(op); break; }
      if ((op & 0x01C0) == 0x0140) {                       // MOV3Q.L #imm3,<ea>
        int q = (op >> 9) & 7; u32 v = q == 0 ? 0xFFFFFFFFu : (u32)q;
        EA e = ea((op >> 3) & 7, op & 7, 4); if (e.k == 3) throw Illegal();
        ea_write(e, 4, v); if (e.k != 1) setnz(v, 4); break;
      }
      if (op & 0x0800) {                                   // bits 10-9: 0 MACSR, 1 ACCext01, 2 MASK, 3 ACCext23
        int g = (op >> 9) & 3, sub = (op >> 6) & 3;
        if (g == 0) {
          if (sub == 0) { EA e = ea((op >> 3) & 7, op & 7, 4); macsr = ea_read(e, 4) & 0xFFFu; }
          else if (sub == 2) gw(op & 15, macsr);
          else if (sub == 3 && op == 0xA9C0) setccr(((macsr >> 3) & 1) << 3 | ((macsr >> 2) & 1) << 2 | ((macsr >> 1) & 1) << 1 | (macsr & 1), 0x1F);   // MOVE MACSR,CCR: X=0, N, Z, V, C=EV
          else throw Illegal();
        } else if (g == 2) {
          if (sub == 0) { EA e = ea((op >> 3) & 7, op & 7, 4); mask = ea_read(e, 4) & 0xFFFFu; }
          else if (sub == 2) gw(op & 15, 0xFFFF0000u | mask);
          else throw Illegal();
        } else {
          int p = g == 1 ? 0 : 2;                          // ACCext01 = ACC1[47:32]:ACC0[47:32]; ACCext23 = ACC3:ACC2
          if (sub == 0) {
            EA e = ea((op >> 3) & 7, op & 7, 4); u32 v = ea_read(e, 4);
            acc[p] = sext48((acc[p] & 0xFFFFFFFFll) | ((i64)(v & 0xFFFF) << 32));
            acc[p + 1] = sext48((acc[p + 1] & 0xFFFFFFFFll) | ((i64)(v >> 16) << 32));
          } else if (sub == 2) {
            gw(op & 15, (u32)((acc[p + 1] >> 32) & 0xFFFF) << 16 | (u32)((acc[p] >> 32) & 0xFFFF));
          } else throw Illegal();
        }
        break;
      }
      {                                                    // ACCn (bits 10-9): MOVE to/from, MOVCLR
        int n = (op >> 9) & 3, sub = (op >> 6) & 3;
        if (sub == 0 && (op & 0xFC) == 0x10) { emac_copy(n, op & 3); break; }    // MOVE.L ACCy,ACCx
        if (sub == 0) { EA e = ea((op >> 3) & 7, op & 7, 4); emac_set(n, ea_read(e, 4)); break; }   // MOVE.L <ea>,ACCn
        if (sub == 2) { gw(op & 15, emac_store(n)); break; }                    // MOVE.L ACCn,Rx
        if (sub == 3) { gw(op & 15, emac_store(n)); acc[n] = 0; macsr &= ~(0x100u << n); break; }   // MOVCLR
        throw Illegal();
      }
    }
    case 0xE: {
      if (((op >> 6) & 3) == 3) {                            // deslocamento/rotação de 1 bit em memória (palavra): extensão do SF-100
        if (op & 0x0800) throw Illegal();                    // bit 11 = 1 seria bit-field (68020), fora do escopo
        EA e = ea((op >> 3) & 7, op & 7, 2); if (e.k == 0 || e.k == 1 || e.k == 3) throw Illegal();
        u32 r = do_shift(ea_read(e, 2), (op >> 9) & 3, op & 0x100, 1, 2);
        ea_write(e, 2, r);
        break;
      }
      int size = (op >> 6) & 3, sz = 1 << size, reg = op & 7, type = (op >> 3) & 3;
      int cnt = (op >> 9) & 7;
      if (op & 0x20) cnt = d[cnt] & 63; else if (cnt == 0) cnt = 8;
      u32 r = do_shift(d[reg], type, op & 0x100, cnt, sz);
      d[reg] = sz == 4 ? r : (d[reg] & ~smask(sz)) | r;
      break;
    }
    case 0xF: exec_fpu(op); break;
    default: throw Illegal();
  }
}

// ---------------- eMAC (CFPRM rev. 3: EMAC_B) ----------------
// Acumuladores de 48 bits: ACC[31:0] + extensão de 16 bits (ACCext), guardados como i64 estendido por sinal de bit 47.
// Inteiro (F/I=0): o produto tem 32 bits (estendido a 40); fracionário (F/I=1): o produto é alinhado com 2^0 no bit 39
// (operandos .W: (x*y)<<9; .L: (x*y)>>23), e a leitura do acumulador usa ACC[39:8].

bool Cpu::emac_fits(i64 v) const {
  bool uns = macsr & 0x40, frac = macsr & 0x20;
  int bits = frac ? 40 : 32;
  if (uns) return v >= 0 && v < ((i64)1 << bits);
  return v >= -((i64)1 << (bits - 1)) && v < ((i64)1 << (bits - 1));
}

// Operandos já escolhidos (metade alta/baixa para .W). Produto de 16 ou 32 bits com sinal (S/U=0) ou sem sinal (S/U=1).
Cpu::Prod Cpu::emac_product(u32 xv, u32 yv, bool word, int scale) const {
  bool uns = macsr & 0x40, frac = macsr & 0x20;
  __int128 a_, b_, p;
  if (uns) { a_ = word ? (__int128)(u16)xv : (__int128)xv; b_ = word ? (__int128)(u16)yv : (__int128)yv; }
  else { a_ = word ? (__int128)(i16)xv : (__int128)(i32)xv; b_ = word ? (__int128)(i16)yv : (__int128)(i32)yv; }
  p = a_ * b_;
  Prod r;
  if (frac) {
    p = word ? p * 512 : p / ((__int128)1 << 23);       // scale factor é ignorado no modo fracionário (>>23 com arredondamento para -inf)
    r.ovf = uns ? (p < 0 || p >= ((__int128)1 << 40)) : (p < -((__int128)1 << 39) || p >= ((__int128)1 << 39));
  } else {
    if (scale == 1) p <<= 1; else if (scale == 3) p >>= 1;
    r.ovf = uns ? (p < 0 || p >= ((__int128)1 << 32)) : (p < -((__int128)1 << 31) || p >= ((__int128)1 << 31));
    p = uns ? (__int128)(u32)(u64)p : (__int128)(i32)(u32)(u64)p;   // produto de 32 bits truncado e estendido
  }
  r.v = (i64)p;
  return r;
}

// Acumula (ou subtrai) o produto. Devolve EV (overflow do acumulador). PAVn é sticky; setflags grava N/Z/V/EV.
bool Cpu::emac_add(int n, const Prod& p, bool sub, bool setflags) {
  i64 s = acc[n] + (sub ? -p.v : p.v);
  bool ev = !emac_fits(s);
  if (p.ovf || ev) macsr |= 0x100u << n;
  acc[n] = sext48(s);
  if (setflags) emac_flags(n, ev);
  return ev;
}
void Cpu::emac_flags(int n, bool ev) {
  macsr &= ~0xFu;
  if (acc[n] == 0) macsr |= 4;
  if (acc[n] < 0) macsr |= 8;
  if (macsr & (0x100u << n)) macsr |= 2;
  if (ev) macsr |= 1;
}

// MOVE.L Ry/#imm,ACCn: a extensão recebe o sinal (inteiro com sinal, fracionário) ou zero (inteiro sem sinal); PAVn e flags limpos.
void Cpu::emac_set(int n, u32 v) {
  bool uns = macsr & 0x40, frac = macsr & 0x20;
  i64 raw;
  if (frac) raw = (i64)((u64)v | ((v & 0x80000000u) ? (0xFFull << 40) : 0));
  else if (uns) raw = (i64)(u32)v;
  else raw = (i64)(i32)v;
  acc[n] = sext48(raw);
  macsr &= ~(0x100u << n);
  emac_flags(n, false);
  macsr &= ~0x2u;
}

// MOVE.L ACCn,Rx (e MOVCLR): leitura com saturação/arredondamento conforme OMC, S/U, F/I e R/T (pseudocódigo do manual).
u32 Cpu::emac_store(int n) const {
  i64 v = acc[n];
  bool uns = macsr & 0x40, frac = macsr & 0x20, sat = macsr & 0x80, rnd = macsr & 0x10;
  auto fits = [](i64 t, int bits) { return t >= -((i64)1 << (bits - 1)) && t < ((i64)1 << (bits - 1)); };
  if (!frac) {
    if (!uns) {
      if (!sat || fits(v, 32)) return (u32)v;
      return v >= 0 ? 0x7FFFFFFFu : 0x80000000u;
    }
    if (!sat || (v >= 0 && v < ((i64)1 << 32))) return (u32)v;
    return 0xFFFFFFFFu;
  }
  if (uns && !sat) {                                     // 16 bits: parte alta zerada, ACC[39:24] arredondado por [23:0]
    return (u32)(((v + (1 << 23)) >> 24) & 0xFFFF);
  }
  if (uns) {                                             // saturação com arredondamento de 16 bits
    i64 t = (v + (1 << 23)) >> 24;
    if (fits(t, 16)) return (u32)(t & 0xFFFF);
    return t >= 0 ? 0x7FFFu : 0x8000u;
  }
  if (!sat) return rnd ? (u32)((v + 0x80) >> 8) : (u32)(v >> 8);
  if (!rnd) return fits(v, 40) ? (u32)(v >> 8) : (v >= 0 ? 0x7FFFFFFFu : 0x80000000u);
  i64 t = (v + 0x80) >> 8;                               // Temp[47:8] arredondado por [7:0]
  if (fits(t, 32)) return (u32)t;
  return t >= 0 ? 0x7FFFFFFFu : 0x80000000u;
}

// MAC/MSAC (EMAC) e MAAAC/MSAAC/MASAC/MSSAC: operação com bit 8 do opcode = 0.
// Sem carga: Rx = op[11-9,6], Ry = op[3-0], ext[11]=sz, ext[10-9]=escala, ext[8]=subtrai, ext[7]/[6]=U/L de Rx/Ry,
//   ext[4]/op[7]=ACCx, ext[3-2]=ACCw, ext[1-0]=0 (MAC/MSAC) ou 01/11 (duplas).
// Com carga (op[5-4]!=0): Rw = op[11-9,6], EA = op[5-0] (memória), Rx = ext[15-12], Ry = ext[3-0], ext[5]=MASK, ACCx = ext[4] e !op[7].
void Cpu::emac_mac(u16 op) {
  bool load = (op & 0x30) != 0;
  u16 e = fetch();
  int rwr = ((op >> 9) & 7) | (((op >> 6) & 1) << 3);
  int rxr = load ? (e >> 12) & 15 : rwr;
  int ryr = load ? e & 15 : op & 15;
  int accx = (((e >> 4) & 1) << 1) | (load ? ((op >> 7) & 1) ^ 1 : (op >> 7) & 1);
  int dual = e & 3, accw = (e >> 2) & 3, scale = (e >> 9) & 3;
  bool sub = e & 0x100, word = !(e & 0x800);
  if (scale == 2 || dual == 2 || (load && dual)) throw Illegal();
  u32 xv = gr(rxr), yv = gr(ryr);
  if (word) {
    xv = (e & 0x80) ? xv >> 16 : xv & 0xFFFF;
    yv = (e & 0x40) ? yv >> 16 : yv & 0xFFFF;
  }
  Prod p = emac_product(xv, yv, word, scale);
  emac_add(accx, p, sub, true);
  if (dual) emac_add(accw, p, dual == 3, false);
  cyc += 2;
  if (load) {                                            // a carga em paralelo: operando de memória, opcionalmente ANDed com MASK
    EA m = ea((op >> 3) & 7, op & 7, 4);
    if (m.k != 2) throw Illegal();
    if (e & 0x20) m.addr &= 0xFFFF0000u | mask;
    gw(rwr, rd(m.addr, 4));
  }
}

// ACCy -> ACCx: copia o acumulador de 48 bits e o PAV; N/Z do resultado, V = PAVy, EV = overflow do acumulador de origem.
void Cpu::emac_copy(int x, int y) {
  bool pav = macsr & (0x100u << y);
  bool ev = !emac_fits(acc[y]);
  acc[x] = acc[y];
  macsr &= ~(0x100u << x);
  if (pav) macsr |= 0x100u << x;
  emac_flags(x, ev);
}

// ---------------- FPU (subconjunto IEEE double) ----------------
void Cpu::fp_cc(double v) { fp_nan = std::isnan(v); fp_n = !fp_nan && std::signbit(v) && v != 0; fp_z = v == 0; }

double Cpu::fp_load(int fmt, const EA& e) {
  switch (fmt) {
    case 0: return (double)(i32)ea_read(e, 4);
    case 1: return (double)u2f(ea_read(e, 4));
    case 4: return (double)(i16)ea_read(e, 2);
    case 6: return (double)(i8)ea_read(e, 1);
    case 5: if (e.k != 2) throw Illegal(); { u64 hi = rd(e.addr, 4), lo = rd(e.addr + 4, 4); return u2d((hi << 32) | lo); }
    default: throw Illegal();
  }
}
void Cpu::fp_store(int fmt, const EA& e, double v) {
  auto toint = [&](double x, double lo, double hi) { if (std::isnan(x)) return (i64)0; x = fp_round(x); return (i64)std::min(std::max(x, lo), hi); };
  switch (fmt) {
    case 0: ea_write(e, 4, (u32)toint(v, -2147483648.0, 2147483647.0)); break;
    case 1: ea_write(e, 4, f2u((float)v)); break;
    case 4: ea_write(e, 2, (u32)toint(v, -32768, 32767) & 0xFFFF); break;
    case 6: ea_write(e, 1, (u32)toint(v, -128, 127) & 0xFF); break;
    case 5: if (e.k != 2) throw Illegal(); { u64 b = d2u(v); wr(e.addr, 4, (u32)(b >> 32)); wr(e.addr + 4, 4, (u32)b); } break;
    default: throw Illegal();
  }
}
bool Cpu::fbcond(int c) const {
  bool N_ = fp_n, Z_ = fp_z, U = fp_nan;
  switch (c & 0x0F) {                                        // 0x10-0x1F: mesmas condições (variantes que sinalizam BSUN, ignorado)
    case 0x0: return false;              case 0x1: return Z_;
    case 0x2: return !(U || Z_ || N_);   case 0x3: return Z_ || !(U || N_);
    case 0x4: return N_ && !(U || Z_);   case 0x5: return Z_ || (N_ && !U);
    case 0x6: return !(U || Z_);         case 0x7: return !U;
    case 0x8: return U;                  case 0x9: return U || Z_;
    case 0xA: return U || !(N_ || Z_);   case 0xB: return U || Z_ || !N_;
    case 0xC: return U || (N_ && !Z_);   case 0xD: return U || Z_ || N_;
    case 0xE: return !Z_;                default: return true;
  }
}

double Cpu::fp_round(double v) const {
  switch ((fpcr >> 4) & 3) {
    case 1: return std::trunc(v); case 2: return std::floor(v); case 3: return std::ceil(v);
    default: return std::nearbyint(v);
  }
}

void Cpu::exec_fpu(u16 op) {
  if ((op & 0xFE00) != 0xF200) throw Illegal();
  int type = (op >> 6) & 7, mode = (op >> 3) & 7, reg = op & 7;
  if (type == 2 || type == 3) {                              // FBcc
    u32 base = pc; i32 disp = type == 2 ? (i32)(i16)fetch() : (i32)fetch32();
    bool pred = disp < 0, taken = fbcond(op & 0x3F);
    if (taken) { pc = base + (u32)disp; cyc += pred ? 1 : 5; } else cyc += pred ? 5 : 0;
    return;
  }
  if (type == 1) {                                           // FScc <ea>
    if (mode == 1) throw Illegal();
    u16 cw = fetch(); EA e = ea(mode, reg, 1); if (e.k == 3) throw Illegal();
    ea_write(e, 1, fbcond(cw & 0x3F) ? 0xFF : 0); return;
  }
  if (type == 4 || type == 5) {                              // FSAVE / FRESTORE: quadro de 16 bytes {0x41000000, FPCR, FPSR, FPIAR}; quadro nulo (0) = reset da FPU
    priv(); EA e = ea(mode, reg, 16); if (e.k != 2) throw Illegal();
    if (type == 4) { wr(e.addr, 4, 0x41000000u); wr(e.addr + 4, 4, fpcr); wr(e.addr + 8, 4, fpsr()); wr(e.addr + 12, 4, fpiar); }
    else if (rd(e.addr, 4) == 0) { fpcr = fpiar = 0; fp_n = fp_z = fp_nan = false; for (auto& f : fp) f = 0; }
    else { fpcr = rd(e.addr + 4, 4) & 0xFF; u32 st = rd(e.addr + 8, 4); fp_n = st & (1u << 27); fp_z = st & (1u << 26); fp_nan = st & (1u << 24); fpiar = rd(e.addr + 12, 4); }
    return;
  }
  if (type != 0) throw Illegal();
  u16 x = fetch(); int kind = x >> 13;
  if (kind == 3) {                                           // FMOVE FPn,<ea>
    EA e = ea(mode, reg, ((x >> 10) & 7) == 5 ? 8 : ((x >> 10) & 7) == 4 ? 2 : ((x >> 10) & 7) == 6 ? 1 : 4);
    fp_store((x >> 10) & 7, e, fp[(x >> 7) & 7]); return;
  }
  if (kind == 4 || kind == 5) {                              // FMOVE <ea>,FPCR/FPSR/FPIAR e o inverso
    int sel = (x >> 10) & 7; if (sel != 4 && sel != 2 && sel != 1) throw Illegal();
    EA e = ea(mode, reg, 4);
    if (kind == 4) {
      u32 v = ea_read(e, 4);
      if (sel == 4) fpcr = v & 0xFF; else if (sel == 1) fpiar = v;
      else { fp_n = v & (1u << 27); fp_z = v & (1u << 26); fp_nan = v & (1u << 24); }
    } else {
      if (e.k == 3) throw Illegal();
      ea_write(e, 4, sel == 4 ? fpcr : sel == 1 ? fpiar : fpsr());
    }
    return;
  }
  if (kind == 6 || kind == 7) {                              // FMOVEM lista estática (FP0 = bit 7), 8 bytes por registro
    if (((x >> 11) & 3) != 2) throw Illegal();
    EA e = ea(mode, reg, 8); if (e.k != 2) throw Illegal();
    u32 ad = e.addr;
    for (int i = 0; i < 8; i++) if (x & (0x80u >> i)) {
      if (kind == 6) { u64 hi = rd(ad, 4), lo = rd(ad + 4, 4); fp[i] = u2d((hi << 32) | lo); }
      else { u64 b = d2u(fp[i]); wr(ad, 4, (u32)(b >> 32)); wr(ad + 4, 4, (u32)b); }
      ad += 8;
    }
    return;
  }
  if (kind != 0 && kind != 2) throw Illegal();
  int dst = (x >> 7) & 7, opm = x & 0x7F; double src;
  if (kind == 0) src = fp[(x >> 10) & 7];
  else {
    int fmt = (x >> 10) & 7; int sz = fmt == 5 ? 8 : fmt == 4 ? 2 : fmt == 6 ? 1 : 4;
    EA e = ea(mode, reg, sz); src = fp_load(fmt, e);
  }
  // Variantes de precisão explícita: FSxxx (0x40|b, 0x41 p/ sqrt) arredonda para simples, FDxxx para dupla.
  int base = opm; bool single = (fpcr & 0xC0) == 0x40;
  if (opm >= 0x40) {
    if (opm == 0x41 || opm == 0x45) { single = opm == 0x41; base = 0x04; }
    else { single = !(opm & 4); base = opm & ~0x44; }
  }
  double r; bool store = true, rounds = true;
  switch (base) {
    case 0x00: r = src; break;
    case 0x01: r = fp_round(src); rounds = false; cyc += 2; break;       // FINT
    case 0x03: r = std::trunc(src); rounds = false; break;     // FINTRZ
    case 0x04: r = std::sqrt(src); cyc += 27; break;
    case 0x18: r = std::fabs(src); break;
    case 0x1A: r = -src; break;
    case 0x20: r = fp[dst] / src; cyc += 22; break;
    case 0x22: r = fp[dst] + src; cyc += 2; break;
    case 0x23: r = fp[dst] * src; cyc += 3; break;
    case 0x28: r = fp[dst] - src; cyc += 2; break;
    case 0x38: r = fp[dst] - src; store = false; cyc += 2; break;        // FCMP
    case 0x3A: r = src; store = false; break;                  // FTST
    default: throw Illegal();
  }
  if (rounds && single && store) r = (double)(float)r;
  if (store) fp[dst] = r;
  fp_cc(r);
}

}  // namespace sf
