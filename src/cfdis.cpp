#include "cfdis.h"

namespace sf {
namespace {

struct Dis {
  const std::function<u16(u32)>& rd; const std::function<std::string(u32)>* syms; u32 pc0, pc; bool ok = true;
  Dis(const std::function<u16(u32)>& r, const std::function<std::string(u32)>* s, u32 p) : rd(r), syms(s), pc0(p), pc(p) {}
  u16 fetch() { u16 w = rd(pc); pc += 2; return w; }
  u32 fetch32() { u32 h = fetch(); return (h << 16) | fetch(); }
  static std::string hex(u32 v) { char b[16]; std::snprintf(b, sizeof b, "$%x", v); return b; }
  static std::string shex(i32 v) { char b[16]; std::snprintf(b, sizeof b, v < 0 ? "-$%x" : "$%x", v < 0 ? (u32)-v : (u32)v); return b; }
  std::string sym(u32 a) { if (syms) { std::string s = (*syms)(a); if (!s.empty()) return s; } return hex(a); }
  static const char* sz(int s) { return s == 1 ? "b" : s == 2 ? "w" : "l"; }
  static std::string dn(int n) { return "d" + std::to_string(n); }
  static std::string an(int n) { return n == 7 ? "a7" : "a" + std::to_string(n); }
  static std::string rn(int n) { return n < 8 ? dn(n) : an(n - 8); }

  // índice / formato completo: base já formatada ("a3" ou "pc")
  std::string index_ea(const std::string& base) {
    u16 e = fetch(); int r = (e >> 12) & 15; std::string x = rn(r) + ((e & 0x800) ? ".l" : ".w"); int sc = (e >> 9) & 3; if (sc) x += "*" + std::to_string(1 << sc);
    if (!(e & 0x100)) { i32 d8 = (i8)e; return "(" + (d8 ? shex(d8) + "," : std::string()) + base + "," + x + ")"; }
    bool bs = e & 0x80, is = e & 0x40; int bds = (e >> 4) & 3, iis = e & 7;
    if (bds == 0 || (e & 8)) { ok = false; return "?"; }
    i32 bd = bds == 1 ? 0 : bds == 2 ? (i32)(i16)fetch() : (i32)fetch32();
    int ods = iis & 3; i32 od = ods == 2 ? (i32)(i16)fetch() : ods == 3 ? (i32)fetch32() : 0;
    std::string b = bs ? "zb" : base, idx = is ? "" : x;
    std::string in = "[" + shex(bd) + "," + b;
    if (!iis) { std::string s = "(" + shex(bd) + "," + b; if (!is) s += "," + idx; return s + ")"; }
    if (iis == 4 || (is && iis > 3)) { ok = false; return "?"; }
    if (!is && !(iis & 4)) in += "," + idx + "]"; else in += "]";                       // pré-indexado: índice dentro dos colchetes
    std::string s = "(" + in; if (!is && (iis & 4)) s += "," + idx; if (od) s += "," + shex(od); return s + ")";
  }
  std::string ea(int mode, int reg, int size) {
    switch (mode) {
      case 0: return dn(reg); case 1: return an(reg); case 2: return "(" + an(reg) + ")"; case 3: return "(" + an(reg) + ")+"; case 4: return "-(" + an(reg) + ")";
      case 5: { i32 d = (i16)fetch(); return shex(d) + "(" + an(reg) + ")"; }
      case 6: return index_ea(an(reg));
      case 7:
        switch (reg) {
          case 0: return "(" + shex((i16)fetch()) + ").w";
          case 1: return "(" + sym(fetch32()) + ").l";
          case 2: { u32 base = pc; i32 d = (i16)fetch(); return shex(d) + "(pc)  ; " + sym(base + d); }
          case 3: return index_ea("pc");
          case 4: if (size == 4) return "#" + hex(fetch32()); else { u16 w = fetch(); return "#" + hex(size == 1 ? (w & 0xFF) : w); }
        }
    }
    ok = false; return "?";
  }
  std::string reglist(u16 m) {
    std::string s; int i = 0;
    while (i < 16) {
      if (!(m & (1u << i))) { i++; continue; }
      int j = i; while (j + 1 < 16 && (m & (1u << (j + 1))) && ((j + 1) / 8 == i / 8)) j++;
      if (!s.empty()) s += "/"; s += rn(i); if (j > i) s += "-" + rn(j); i = j + 1;
    }
    return s;
  }
  static const char* cc(int c) { static const char* n[16] = {"t", "f", "hi", "ls", "cc", "cs", "ne", "eq", "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"}; return n[c]; }
  static const char* fcc(int c) {
    static const char* n[32] = {"f", "eq", "ogt", "oge", "olt", "ole", "ogl", "or", "un", "ueq", "ugt", "uge", "ult", "ule", "ne", "t",
                                "sf", "seq", "gt", "ge", "lt", "le", "gl", "gle", "ngle", "ngl", "nle", "nlt", "nge", "ngt", "sne", "st"};
    return n[c & 31];
  }
  static std::string fmt(const char* m, const std::string& a, const std::string& b = "") {
    std::string s = m; while (s.size() < 8) s += ' '; if (s.size() == std::strlen(m) && s.size() >= 8) s += ' '; s += a; if (!b.empty()) s += "," + b; return s;
  }

  std::string run();
  std::string fpu(u16 op);
};

std::string Dis::fpu(u16 op) {
  if ((op & 0xFE00) != 0xF200) { ok = false; return "?"; }
  int type = (op >> 6) & 7, mode = (op >> 3) & 7, reg = op & 7;
  if (type == 2 || type == 3) {
    u32 base = pc; i32 d = type == 2 ? (i32)(i16)fetch() : (i32)fetch32();
    return fmt((std::string("fb") + fcc(op & 0x3F) + (type == 2 ? ".w" : ".l")).c_str(), sym(base + d));
  }
  if (type == 1) { u16 cw = fetch(); if (mode == 1) { ok = false; return "?"; } return fmt((std::string("fs") + fcc(cw & 0x3F)).c_str(), ea(mode, reg, 1)); }
  if (type == 4 || type == 5) { if (mode < 2) { ok = false; return "?"; } return fmt(type == 4 ? "fsave" : "frestore", ea(mode, reg, 16)); }
  if (type != 0) { ok = false; return "?"; }
  u16 x = fetch(); int kind = x >> 13;
  static const char fl[8] = {'l', 's', 'x', 'p', 'w', 'd', 'b', '?'};
  auto fsz = [](int f) { return f == 5 ? 8 : f == 4 ? 2 : f == 6 ? 1 : 4; };
  if (kind == 3) { int f = (x >> 10) & 7; if (f == 2 || f == 3 || f == 7) { ok = false; return "?"; } return fmt((std::string("fmove.") + fl[f]).c_str(), "fp" + std::to_string((x >> 7) & 7), ea(mode, reg, fsz(f))); }
  if (kind == 4 || kind == 5) {
    int sel = (x >> 10) & 7; if (sel != 4 && sel != 2 && sel != 1) { ok = false; return "?"; }
    const char* cr = sel == 4 ? "fpcr" : sel == 2 ? "fpsr" : "fpiar"; std::string e = ea(mode, reg, 4);
    return kind == 4 ? fmt("fmove.l", e, cr) : fmt("fmove.l", cr, e);
  }
  if (kind == 6 || kind == 7) {
    if (((x >> 11) & 3) != 2) { ok = false; return "?"; }
    std::string l; for (int i = 0; i < 8; i++) if (x & (0x80u >> i)) { if (!l.empty()) l += "/"; l += "fp" + std::to_string(i); }
    std::string e = ea(mode, reg, 8); return kind == 6 ? fmt("fmovem.d", e, l) : fmt("fmovem.d", l, e);
  }
  if (kind != 0 && kind != 2) { ok = false; return "?"; }
  int dst = (x >> 7) & 7, opm = x & 0x7F; int base = opm; char pre = 0;
  if (opm >= 0x40) { if (opm == 0x41 || opm == 0x45) { pre = opm == 0x41 ? 's' : 'd'; base = 0x04; } else { pre = (opm & 4) ? 'd' : 's'; base = opm & ~0x44; } }
  const char* nm = nullptr;
  switch (base) { case 0x00: nm = "move"; break; case 0x01: nm = "int"; break; case 0x03: nm = "intrz"; break; case 0x04: nm = "sqrt"; break; case 0x18: nm = "abs"; break;
    case 0x1A: nm = "neg"; break; case 0x20: nm = "div"; break; case 0x22: nm = "add"; break; case 0x23: nm = "mul"; break; case 0x28: nm = "sub"; break;
    case 0x38: nm = "cmp"; break; case 0x3A: nm = "tst"; break; }
  if (!nm) { ok = false; return "?"; }
  std::string m = "f"; if (pre) m += pre; m += nm;
  std::string d = "fp" + std::to_string(dst);
  if (kind == 0) { std::string s = "fp" + std::to_string((x >> 10) & 7); return base == 0x3A ? fmt(m.c_str(), s) : fmt(m.c_str(), s, d); }
  int f = (x >> 10) & 7; if (f == 2 || f == 3 || f == 7) { ok = false; return "?"; }
  m += std::string(".") + fl[f]; std::string e = ea(mode, reg, fsz(f));
  return base == 0x3A ? fmt(m.c_str(), e) : fmt(m.c_str(), e, d);
}

std::string Dis::run() {
  u16 op = fetch(); int grp = op >> 12, mode = (op >> 3) & 7, reg = op & 7;
  switch (grp) {
    case 0x0: {
      if ((op & 0x100) || (op & 0xF00) == 0x800) {
        static const char* nm[4] = {"btst", "bchg", "bclr", "bset"}; int tt = (op >> 6) & 3; std::string b;
        if (op & 0x100) b = dn((op >> 9) & 7); else b = "#" + std::to_string(fetch() & 0xFF);
        return fmt(nm[tt], b, ea(mode, reg, 1));
      }
      if ((op & 0xFFF8) == 0x00C0) return fmt("bitrev", dn(reg));
      if ((op & 0xFFF8) == 0x02C0) return fmt("byterev", dn(reg));
      if ((op & 0xFFF8) == 0x04C0) return fmt("ff1", dn(reg));
      int size = (op >> 6) & 3; if (size == 3) { ok = false; return "?"; }
      int s = 1 << size, sub = (op >> 8) & 0xF; const char* nm = sub == 0 ? "ori" : sub == 2 ? "andi" : sub == 4 ? "subi" : sub == 6 ? "addi" : sub == 10 ? "eori" : sub == 12 ? "cmpi" : nullptr;
      if (!nm) { ok = false; return "?"; }
      std::string im; if (s == 4) im = "#" + hex(fetch32()); else { u16 w = fetch(); im = "#" + hex(s == 1 ? (w & 0xFF) : w); }
      return fmt((std::string(nm) + "." + sz(s)).c_str(), im, ea(mode, reg, s));
    }
    case 0x1: case 0x2: case 0x3: {
      int s = grp == 1 ? 1 : grp == 3 ? 2 : 4; std::string src = ea(mode, reg, s); int dm = (op >> 6) & 7, dr = (op >> 9) & 7;
      if (dm == 1) return fmt((std::string("movea.") + sz(s)).c_str(), src, an(dr));
      return fmt((std::string("move.") + sz(s)).c_str(), src, ea(dm, dr, s));
    }
    case 0x4: {
      if (op == 0x4E71) return "nop"; if (op == 0x4E75) return "rts"; if (op == 0x4E73) return "rte";
      if (op == 0x4E72) return fmt("stop", "#" + hex(fetch())); if (op == 0x4AC8) return "halt"; if (op == 0x4AFC) return "illegal";
      if ((op & 0xFFF0) == 0x4E40) return fmt("trap", "#" + std::to_string(op & 15));
      if ((op & 0xFFF8) == 0x4E50) return fmt("link", an(reg), "#" + shex((i16)fetch())); if ((op & 0xFFF8) == 0x4E58) return fmt("unlk", an(reg));
      if (op == 0x4E7B || op == 0x4E7A) { u16 x = fetch(); std::string r = ((x & 0x8000) ? an((x >> 12) & 7) : dn((x >> 12) & 7)); u32 rc = x & 0xFFF;
        std::string c = rc == 0x801 ? "vbr" : rc == 0x800 ? "other_a7" : rc == 0x002 ? "cacr" : hex(rc); return op == 0x4E7B ? fmt("movec", r, c) : fmt("movec", c, r); }
      if ((op & 0xFFF0) == 0x4E60) return (op & 8) ? fmt("move.l", "usp", an(reg)) : fmt("move.l", an(reg), "usp");
      if ((op & 0xFFC0) == 0x4AC0 && mode != 1) return fmt("tas", ea(mode, reg, 1));
      if ((op & 0xFFF8) == 0x4C80) return fmt("sats.l", dn(reg));
      if ((op & 0xFFF8) == 0x42C0) return fmt("move.w", "ccr", dn(reg));
      if ((op & 0xFFF8) == 0x40C0) return fmt("move.w", "sr", dn(reg));
      if ((op & 0xFFC0) == 0x46C0) return fmt("move.w", ea(mode, reg, 2), "sr");
      if ((op & 0xFFC0) == 0x44C0) return fmt("move.w", ea(mode, reg, 2), "ccr");
      if ((op & 0xF1C0) == 0x41C0 && mode >= 2) return fmt("lea", ea(mode, reg, 4), an((op >> 9) & 7));
      if ((op & 0xFFF8) == 0x4840) return fmt("swap", dn(reg));
      if ((op & 0xFFC0) == 0x4840) return fmt("pea", ea(mode, reg, 4));
      if ((op & 0xFFF8) == 0x4880) return fmt("ext.w", dn(reg)); if ((op & 0xFFF8) == 0x48C0) return fmt("ext.l", dn(reg)); if ((op & 0xFFF8) == 0x49C0) return fmt("extb.l", dn(reg));
      if ((op & 0xFFC0) == 0x48C0 || (op & 0xFFC0) == 0x4CC0) {
        bool toreg = op & 0x400; u16 m = fetch(); std::string e = ea(mode, reg, 4); return toreg ? fmt("movem.l", e, reglist(m)) : fmt("movem.l", reglist(m), e);
      }
      if ((op & 0xFFC0) == 0x4C00 || (op & 0xFFC0) == 0x4C40) {
        u16 x = fetch(); std::string e = ea(mode, reg, 4); int dl = (x >> 12) & 7; bool sg = x & 0x800;
        if (op & 0x40) { int dr = x & 7; std::string n = sg ? (dr != dl ? "rems.l" : "divs.l") : (dr != dl ? "remu.l" : "divu.l"); return dr != dl ? fmt(n.c_str(), e, dn(dr) + ":" + dn(dl)) : fmt(n.c_str(), e, dn(dl)); }
        std::string n = sg ? "muls.l" : "mulu.l"; if (x & 0x400) return fmt(n.c_str(), e, dn(x & 7) + ":" + dn(dl)); return fmt(n.c_str(), e, dn(dl));
      }
      if ((op & 0xFFC0) == 0x4E80) return fmt("jsr", ea(mode, reg, 4)); if ((op & 0xFFC0) == 0x4EC0) return fmt("jmp", ea(mode, reg, 4));
      int size = (op >> 6) & 3;
      if (size != 3) {
        int s = 1 << size, sub = (op >> 8) & 0xF;
        if (sub == 0x0) { if (mode != 0) { ok = false; return "?"; } return fmt((std::string("negx.") + sz(s)).c_str(), dn(reg)); }
        const char* nm = sub == 2 ? "clr" : sub == 4 ? "neg" : sub == 6 ? "not" : sub == 10 ? "tst" : nullptr;
        if (nm) return fmt((std::string(nm) + "." + sz(s)).c_str(), ea(mode, reg, s));
      }
      ok = false; return "?";
    }
    case 0x5: {
      int size = (op >> 6) & 3;
      if (op == 0x51F8) return "tpf"; if (op == 0x51FA) return fmt("tpf.w", "#" + hex(fetch())); if (op == 0x51FB) return fmt("tpf.l", "#" + hex(fetch32()));
      if (size == 3) return fmt((std::string("s") + cc((op >> 8) & 15)).c_str(), ea(mode, reg, 1));
      int q = (op >> 9) & 7; if (!q) q = 8;
      return fmt(((op & 0x100) ? std::string("subq.") : std::string("addq.")).append(sz(1 << size)).c_str(), "#" + std::to_string(q), ea(mode, reg, 1 << size));
    }
    case 0x6: {
      int c = (op >> 8) & 15; u32 base = pc; i32 d = (i8)op; const char* suf = ".s";
      if ((op & 0xFF) == 0) { d = (i16)fetch(); suf = ".w"; } else if ((op & 0xFF) == 0xFF) { d = (i32)fetch32(); suf = ".l"; }
      std::string n = c == 0 ? "bra" : c == 1 ? "bsr" : std::string("b") + cc(c);
      return fmt((n + suf).c_str(), sym(base + d));
    }
    case 0x7: {
      if (op & 0x100) { int opm = (op >> 6) & 7; if (opm < 4) { ok = false; return "?"; } int s = (opm & 1) ? 2 : 1;
        return fmt(((opm < 6) ? std::string("mvs.") : std::string("mvz.")).append(sz(s)).c_str(), ea(mode, reg, s), dn((op >> 9) & 7)); }
      return fmt("moveq", "#" + std::to_string((i8)op), dn((op >> 9) & 7));
    }
    case 0x8: case 0xC: {
      int opm = (op >> 6) & 7, d = (op >> 9) & 7;
      if (opm == 3 || opm == 7) { std::string n = grp == 0xC ? (opm == 7 ? "muls.w" : "mulu.w") : (opm == 7 ? "divs.w" : "divu.w"); return fmt(n.c_str(), ea(mode, reg, 2), dn(d)); }
      int s = 1 << (opm & 3); if ((opm & 3) == 3) { ok = false; return "?"; }
      std::string n = std::string(grp == 8 ? "or." : "and.") + sz(s);
      return opm < 4 ? fmt(n.c_str(), ea(mode, reg, s), dn(d)) : fmt(n.c_str(), dn(d), ea(mode, reg, s));
    }
    case 0x9: case 0xD: case 0xB: {
      int opm = (op >> 6) & 7, d = (op >> 9) & 7;
      if (opm == 3 || opm == 7) { int s = opm == 3 ? 2 : 4; const char* n = grp == 0xD ? "adda." : grp == 9 ? "suba." : "cmpa."; return fmt((std::string(n) + sz(s)).c_str(), ea(mode, reg, s), an(d)); }
      int s = 1 << (opm & 3); if ((opm & 3) == 3) { ok = false; return "?"; }
      if (grp != 0xB && opm >= 4 && mode == 0) return fmt((std::string(grp == 0xD ? "addx." : "subx.") + sz(s)).c_str(), dn(reg), dn(d));
      if (grp == 0xB) return opm < 4 ? fmt((std::string("cmp.") + sz(s)).c_str(), ea(mode, reg, s), dn(d)) : fmt((std::string("eor.") + sz(s)).c_str(), dn(d), ea(mode, reg, s));
      std::string n = std::string(grp == 0xD ? "add." : "sub.") + sz(s);
      return opm < 4 ? fmt(n.c_str(), ea(mode, reg, s), dn(d)) : fmt(n.c_str(), dn(d), ea(mode, reg, s));
    }
    case 0xA: {
      if (!(op & 0x0100)) {                                // MAC / MSAC / MAAAC / MASAC / MSAAC / MSSAC (EMAC), com ou sem carga
        bool load = (op & 0x30) != 0;
        int rwr = ((op >> 9) & 7) | (((op >> 6) & 1) << 3), accl = (op >> 7) & 1;
        u16 e = fetch(); int sc = (e >> 9) & 3, dual = e & 3; bool word = !(e & 0x800), sub = e & 0x100;
        if (sc == 2 || dual == 2 || (load && dual)) { ok = false; return "?"; }
        int rxr = load ? (e >> 12) & 15 : rwr, ryr = load ? e & 15 : op & 15;
        int accx = (((e >> 4) & 1) << 1) | (load ? accl ^ 1 : accl), accw = (e >> 2) & 3;
        std::string n = dual == 0 ? (sub ? "msac" : "mac") : dual == 1 ? (sub ? "msaac" : "maaac") : (sub ? "mssac" : "masac");
        n += word ? ".w" : ".l";
        std::string x = rn(rxr) + (word ? ((e & 0x80) ? ".u" : ".l") : ""), y = rn(ryr) + (word ? ((e & 0x40) ? ".u" : ".l") : "");
        if (sc == 1) x += "<<1"; else if (sc == 3) x += ">>1";
        if (load) {
          std::string m = ea((op >> 3) & 7, op & 7, 4) + ((e & 0x20) ? "&" : "");
          return fmt(n.c_str(), y + "," + x + "," + m, rn(rwr) + ",acc" + std::to_string(accx));
        }
        std::string accs = "acc" + std::to_string(accx) + (dual ? ",acc" + std::to_string(accw) : "");
        return fmt(n.c_str(), y + "," + x, accs);
      }
      if ((op & 0x01C0) == 0x0140) { int q = (op >> 9) & 7; return fmt("mov3q.l", "#" + std::to_string(q ? (int)q : -1), ea(mode, reg, 4)); }
      if (op & 0x0800) {                                   // MACSR / MASK / ACCext01 / ACCext23
        int g = (op >> 9) & 3, sub = (op >> 6) & 3;
        if (g == 0) {
          if (sub == 0) return fmt("move.l", ea((op >> 3) & 7, op & 7, 4), "macsr");
          if (sub == 2) return fmt("move.l", "macsr", rn(op & 15));
          if (sub == 3 && op == 0xA9C0) return "move.l  macsr,ccr";
        } else if (g == 2) {
          if (sub == 0) return fmt("move.l", ea((op >> 3) & 7, op & 7, 4), "mask");
          if (sub == 2) return fmt("move.l", "mask", rn(op & 15));
        } else {
          std::string r = g == 1 ? "accext01" : "accext23";
          if (sub == 0) return fmt("move.l", ea((op >> 3) & 7, op & 7, 4), r.c_str());
          if (sub == 2) return fmt("move.l", r.c_str(), rn(op & 15));
        }
        ok = false; return "?";
      }
      int n = (op >> 9) & 3, sub = (op >> 6) & 3; std::string an = "acc" + std::to_string(n);
      if (sub == 0 && (op & 0xFC) == 0x10) return fmt("move.l", "acc" + std::to_string(op & 3), an.c_str());
      if (sub == 0) return fmt("move.l", ea((op >> 3) & 7, op & 7, 4), an.c_str());
      if (sub == 2) return fmt("move.l", an.c_str(), rn(op & 15));
      if (sub == 3) return fmt("movclr.l", an.c_str(), rn(op & 15));
      ok = false; return "?";
    }
    case 0xE: {
      int size = (op >> 6) & 3; static const char* nm[4] = {"as", "ls", "rox", "ro"};
      if (size == 3) {
        int mode = (op >> 3) & 7; if (mode <= 1 || (mode == 7 && (op & 7) > 3) || (op & 0x0800)) { ok = false; return "?"; }
        return fmt((std::string(nm[(op >> 9) & 3]) + ((op & 0x100) ? "l.w" : "r.w")).c_str(), ea(mode, op & 7, 2));
      }
      int type = (op >> 3) & 3; std::string n = std::string(nm[type]) + ((op & 0x100) ? "l." : "r.") + sz(1 << size); int cnt = (op >> 9) & 7;
      return fmt(n.c_str(), (op & 0x20) ? dn(cnt) : "#" + std::to_string(cnt ? cnt : 8), dn(reg));
    }
    case 0xF: return fpu(op);
  }
  ok = false; return "?";
}

}  // namespace

CfInsn cf_disasm(const std::function<u16(u32)>& rd16, u32 pc, const std::function<std::string(u32)>* syms) {
  Dis d(rd16, syms, pc); CfInsn r; r.text = d.run(); r.known = d.ok; r.len = (int)(d.pc - pc);
  if (!r.known) { char b[48]; u16 w = rd16(pc); std::snprintf(b, sizeof b, ".word   $%04x", w); r.text = b; r.len = 2; }
  return r;
}

}  // namespace sf
