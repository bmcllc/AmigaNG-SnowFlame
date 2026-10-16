#include "m68k2cf.h"
#include "cpu68k.h"
#include "cfasm.h"

namespace sf {
namespace {

using Op = CfAsm::Op;
using A_ = CfAsm;

struct Scratch : M68kMem {
  std::vector<u8> m = std::vector<u8>(0x10000, 0);
  u8 read8(u32 a) override { return m[a & 0xFFFF]; } u16 read16(u32 a) override { return (u16)((m[a & 0xFFFF] << 8) | m[(a + 1) & 0xFFFF]); }
  void write8(u32 a, u8 v) override { m[a & 0xFFFF] = v; } void write16(u32 a, u16 v) override { m[a & 0xFFFF] = (u8)(v >> 8); m[(a + 1) & 0xFFFF] = (u8)v; }
};
// tamanho em bytes da instrução 68000 no início de 'p' (mede as palavras buscadas executando-a num CPU descartável)
int insn_len(const u8* p, size_t avail) {
  Scratch s; for (size_t i = 0; i < 24 && i < avail; i++) s.m[0x2000 + i] = p[i];
  Cpu68k c(s); c.reset_regs(); for (int i = 0; i < 8; i++) { c.d[i] = 0x4000; c.a[i] = 0x4000; } c.a[7] = 0x6000; c.ssp = 0x6000; c.pc = 0x2000;
  u64 f0 = c.ifetch_words; c.step(); int n = (int)(c.ifetch_words - f0); return std::max(1, n) * 2;
}

int ea_ext_count(int mode, int reg, int size) {
  switch (mode) {
    case 5: case 6: return 1;
    case 7: return reg == 0 ? 1 : reg == 1 ? 2 : reg == 2 ? 1 : reg == 3 ? 1 : reg == 4 ? (size == 4 ? 2 : 1) : 0;
    default: return 0;
  }
}

struct EaRef { int shift; int mode, reg, size; int ext_idx, ext_cnt; };       // shift: posição do campo modo/registro no opcode (3 = baixo, 6 = destino do MOVE)
struct Info { std::vector<EaRef> eas; bool unsupported = false; std::string why; int fixed_after_op = 0; };

Info analyze(const std::vector<u16>& w) {
  Info in; u16 op = w[0]; int grp = op >> 12, mode = (op >> 3) & 7, reg = op & 7, size = 1 << ((op >> 6) & 3);
  int idx = 1;
  auto add = [&](int shift, int m, int r, int sz) { EaRef e{shift, m, r, sz, idx, ea_ext_count(m, r, sz)}; idx += e.ext_cnt; in.eas.push_back(e); };
  switch (grp) {
    case 1: case 2: case 3: { int sz = grp == 1 ? 1 : grp == 3 ? 2 : 4; add(0, mode, reg, sz); add(6, (op >> 6) & 7, (op >> 9) & 7, sz); in.eas[0].shift = 0; in.eas[1].shift = 6; break; }
    case 0:
      if ((op & 0x138) == 0x108) { in.unsupported = true; in.why = "MOVEP"; }
      else if (op == 0x003C || op == 0x007C || op == 0x023C || op == 0x027C || op == 0x0A3C || op == 0x0A7C) { idx = 2; }
      else if (op & 0x100) add(0, mode, reg, 1);
      else if ((op & 0xF00) == 0x800) { idx = 2; add(0, mode, reg, 1); }
      else if (((op >> 6) & 3) != 3) { idx = size == 4 ? 3 : 2; add(0, mode, reg, size); }
      break;
    case 4:
      if ((op & 0xF1C0) == 0x41C0 || (op & 0xFFC0) == 0x4E80 || (op & 0xFFC0) == 0x4EC0 || ((op & 0xFFC0) == 0x4840 && mode != 0)) add(0, mode, reg, 4);
      else if ((op & 0xFFC0) == 0x40C0 || (op & 0xFFC0) == 0x44C0 || (op & 0xFFC0) == 0x46C0) add(0, mode, reg, 2);
      else if ((op & 0xFFC0) == 0x4AC0 && mode != 1) add(0, mode, reg, 1);
      else if ((op & 0xFB80) == 0x4880 && mode != 0 && (op & 0xFFF8) != 0x48C0 && (op & 0xFFF8) != 0x4880 && (op & 0xFFF8) != 0x49C0) { idx = 2; add(0, mode, reg, (op & 0x40) ? 4 : 2); }
      else if ((op & 0xF1C0) == 0x4180) { in.unsupported = true; in.why = "CHK"; }
      else if ((op & 0xFFC0) == 0x4800) { in.unsupported = true; in.why = "NBCD"; }
      else if (((op >> 6) & 3) != 3 && ((op & 0xF00) == 0x000 || (op & 0xF00) == 0x200 || (op & 0xF00) == 0x400 || (op & 0xF00) == 0x600 || (op & 0xF00) == 0xA00)) add(0, mode, reg, size);
      break;
    case 5: if (((op >> 6) & 3) == 3) { if (mode != 1) add(0, mode, reg, 1); } else add(0, mode, reg, size); break;
    case 8: case 9: case 0xB: case 0xC: case 0xD: {
      int opm = (op >> 6) & 7;
      if (opm == 3 || opm == 7) { if (!(grp == 8 || grp == 0xC) || true) add(0, mode, reg, (grp == 0xD || grp == 0x9 || grp == 0xB) ? (opm == 3 ? 2 : 4) : 2); }
      else if (opm < 4) add(0, mode, reg, size);
      else if (grp == 0xB && mode != 1) add(0, mode, reg, size);
      else if ((grp == 9 || grp == 0xD || grp == 8 || grp == 0xC) && mode > 1 && !((grp == 8 || grp == 0xC) && (op & 0x1F0) == 0x100)) add(0, mode, reg, size);
      break;
    }
    case 0xE: if (((op >> 6) & 3) == 3) add(0, mode, reg, 2); break;
    default: break;
  }
  return in;
}

struct Ctx {
  const std::vector<u8>& code68; const XlatOptions& opt; std::map<u32, u32>& map; bool pass2;
  std::vector<std::string>& notes;
  std::function<u32(u32)> data_map;                                  // endereço 68k dentro de data_ranges -> ColdFire (ou 0)
};

u32 map_addr(Ctx& c, u32 t, bool* inside = nullptr) {
  u32 end = c.opt.base68 + (u32)c.code68.size(); bool in = t >= c.opt.base68 && t < end;
  if (inside) *inside = in;
  if (!in) return t;
  auto it = c.map.find(t); if (it != c.map.end()) return it->second;
  u32 d = c.data_map(t); if (d) return d;
  return c.pass2 ? 0xFFFFFFFFu : 0x1000;                              // endereço interno que não é início de instrução/dado
}

[[noreturn]] void bad(const std::string& m) { throw std::runtime_error(m); }

int popcount16(u16 m) { int n = 0; for (int i = 0; i < 16; i++) if (m & (1u << i)) n++; return n; }
u16 rev16(u16 m) { u16 r = 0; for (int i = 0; i < 16; i++) if (m & (1u << i)) r |= (u16)(1u << (15 - i)); return r; }

std::vector<u8> translate_one(Ctx& c, u32 addr68, const std::vector<u16>& w, u32 cf_addr, bool& expanded) {
  A_ a(cf_addr); u16 op = w[0]; int grp = op >> 12, mode = (op >> 3) & 7, reg = op & 7, dn = (op >> 9) & 7;
  expanded = false;
  auto next68 = [&]() { return addr68 + (u32)w.size() * 2; };
  auto raw = [&](std::vector<u16> ws) { for (u16 x : ws) a.w(x); };
  Info in = analyze(w);
  if (in.unsupported) bad(in.why + " em 0x" + [&] { char b[16]; std::snprintf(b, sizeof b, "%x", addr68); return std::string(b); }() + " não é suportado pelo tradutor");

  // ---- instruções que viram sequências ----
  if (grp == 0x5 && ((op >> 6) & 3) == 3 && mode == 1) {                          // DBcc Dn,disp
    int cc = (op >> 8) & 15; u32 base = addr68 + 2; i16 d16 = (i16)w[1]; u32 target68 = base + (u32)(i32)d16;
    if (cc == 0) { expanded = true; a.nop(); return a.finish(); }                 // DBT: nunca repete
    u32 target = map_addr(c, target68), exitaddr = map_addr(c, next68()); int t = reg == 7 ? 6 : 7; expanded = true;
    if (cc != 1) { a.w((u16)(0x6000 | (cc << 8))); a.w((u16)((i32)exitaddr - (i32)(cf_addr + 2))); }   // condição verdadeira: segue para a próxima instrução
    a.lea(A_::Disp(7, -12), 7); a.movem_to((u16)(1u << t), A_::Disp(7, 4));        // [a7+0] = flags, [a7+4] = temp salvo, [a7+8] = destino (rts)
    a.move_from_ccr(t); a.move(2, A_::D(t), A_::Ind(7));
    a.movel(A_::Imm(exitaddr), A_::D(t)); a.movem_to((u16)(1u << t), A_::Disp(7, 8));
    a.subq(2, 1, A_::D(reg)); a.cmpi(2, 0xFFFF, A_::D(reg)); a.beq("noloop");
    a.movel(A_::Imm(target), A_::D(t)); a.movem_to((u16)(1u << t), A_::Disp(7, 8));
    a.label("noloop"); a.move(2, A_::Ind(7), A_::D(t)); a.move_to_ccr(A_::D(t)); a.movem_from(A_::Disp(7, 4), (u16)(1u << t)); a.lea(A_::Disp(7, 8), 7); a.rts();
    return a.finish();
  }
  if (grp == 0xC && ((op & 0x1F8) == 0x140 || (op & 0x1F8) == 0x148 || (op & 0x1F8) == 0x188)) {   // EXG
    int x = (op & 0x1F8) == 0x148 ? 8 + dn : dn, y = (op & 0x1F8) == 0x140 ? reg : 8 + reg; expanded = true;
    if (x == 15 || y == 15) bad("EXG com A7 não é suportado");
    if (x == y) { a.nop(); return a.finish(); }
    int lo = std::min(x, y), hi = std::max(x, y);
    a.lea(A_::Disp(7, -8), 7); a.movem_to((u16)((1u << lo) | (1u << hi)), A_::Ind(7));
    a.movem_from(A_::Disp(7, 4), (u16)(1u << lo)); a.movem_from(A_::Ind(7), (u16)(1u << hi)); a.lea(A_::Disp(7, 8), 7);
    return a.finish();
  }
  if (grp == 0xB && ((op >> 6) & 3) != 3 && (op & 0x100) && mode == 1) {          // CMPM (Ay)+,(Ax)+
    int sz = 1 << ((op >> 6) & 3), ax = dn, ay = reg; if (ax == 7 || ay == 7) bad("CMPM com A7 não é suportado"); expanded = true;
    a.lea(A_::Disp(7, -4), 7); a.movem_to(1u << 7, A_::Ind(7)); a.move(sz, A_::Post(ax), A_::D(7)); a.cmp(sz, A_::Post(ay), 7); a.movem_from(A_::Ind(7), 1u << 7); a.lea(A_::Disp(7, 4), 7);
    return a.finish();
  }
  if (op == 0x003C || op == 0x023C || op == 0x0A3C || op == 0x007C || op == 0x027C || op == 0x0A7C) {   // ORI/ANDI/EORI to CCR/SR
    bool sr = (op & 0xFF) == 0x7C; u16 imm = w[1]; expanded = true;
    a.lea(A_::Disp(7, -4), 7); a.movem_to(1u << 7, A_::Ind(7));
    if (sr) a.move_from_sr(7); else a.move_from_ccr(7);
    if ((op >> 8) == 0x00) a.ori(4, imm, A_::D(7)); else if ((op >> 8) == 0x02) a.andi(4, imm, A_::D(7)); else a.eori(4, imm, A_::D(7));
    if (sr) a.move_to_sr(A_::D(7)); else a.move_to_ccr(A_::D(7));
    a.movem_from(A_::Ind(7), 1u << 7); a.lea(A_::Disp(7, 4), 7);
    return a.finish();
  }
  if (op == 0x4E77) { expanded = true; raw({0x44DF, 0x4E75}); return a.finish(); }   // RTR: move.w (a7)+,ccr ; rts
  if ((op & 0xFB80) == 0x4880 && mode != 0 && (op & 0xFFF8) != 0x48C0 && (op & 0xFFF8) != 0x4880 && (op & 0xFFF8) != 0x49C0) {   // MOVEM
    bool toreg = op & 0x400; bool lng = op & 0x40; u16 mask = w[1];
    if (!lng) bad("MOVEM.W não é suportado");
    if (mode == 4 && !toreg) {                                                    // movem.l regs,-(An)
      u16 m = rev16(mask); int n = popcount16(m); expanded = true;
      a.lea(A_::Disp(reg, -4 * n), reg); a.movem_to(m, A_::Ind(reg)); return a.finish();
    }
    if (mode == 3 && toreg) {                                                     // movem.l (An)+,regs
      int n = popcount16(mask); expanded = true; a.movem_from(A_::Ind(reg), mask); a.lea(A_::Disp(reg, 4 * n), reg); return a.finish();
    }
    if (mode != 2 && mode != 5) bad("MOVEM com esse modo de endereçamento não é suportado");
  }
  if (grp == 0xE && ((op >> 6) & 3) == 3) {                                        // deslocamento de memória de 1 bit
    int type = (op >> 9) & 3; bool left = op & 0x100;
    if (type == 2) bad("ROXL/ROXR em memória não é suportado");
    if (mode == 0 || mode == 1 || mode == 6 || (mode == 7 && (reg == 2 || reg == 3 || reg == 4)) || ((mode == 3 || mode == 4 || mode == 2 || mode == 5) && reg == 7)) bad("modo de endereçamento do deslocamento em memória não é suportado");
    expanded = true;
    Op src, dst; std::vector<u16> ext(w.begin() + 1, w.end());
    auto mk = [&](int m, int r, int dsp) -> Op { Op o; o.mode = m; o.reg = r; if (m == 5) o.ext.push_back((u16)dsp); else if (m == 7) o.ext = ext; return o; };
    if (mode == 5) { src = mk(5, reg, (i16)ext[0]); dst = src; }
    else if (mode == 7) { src = mk(7, reg, 0); dst = src; }
    else { src = mk(mode, reg, 0); dst = mode == 3 ? mk(5, reg, -2) : mk(2, reg, 0); }
    a.lea(A_::Disp(7, -8), 7); a.movem_to((1u << 6) | (1u << 7), A_::Ind(7));
    a.move(2, src, A_::D(7)); a.shift_i(type, left, 1, 7, 2); a.move_from_ccr(6); a.move(2, A_::D(7), dst); a.move_to_ccr(A_::D(6));
    a.movem_from(A_::Ind(7), (1u << 6) | (1u << 7)); a.lea(A_::Disp(7, 8), 7);
    return a.finish();
  }
  if (grp == 0xE && ((op >> 6) & 3) != 3 && (((op >> 3) & 3) == 2)) {             // ROXL/ROXR com registrador
    int sz = 1 << ((op >> 6) & 3); bool left = op & 0x100; bool regcount = op & 0x20; int cnt = dn ? dn : 8;
    if (sz != 4 || !left || regcount) bad("ROXR/ROX.B/.W/contagem em registrador não são suportados (só ROXL.L #n,Dn)");
    expanded = true;
    int t1 = 7, t2 = 6; if (reg == 7) { t1 = 5; } else if (reg == 6) { t2 = 5; }
    a.lea(A_::Disp(7, -8), 7); a.movem_to((u16)((1u << t1) | (1u << t2)), A_::Ind(7));
    for (int i = 0; i < cnt; i++) a.addx_l(reg, reg);                           // gira por X (ADDX Dn,Dn): X/C = bit que saiu
    a.tst(4, A_::D(reg));                                                       // N e Z do resultado; V = 0 (como o ROXL); C = 0 por enquanto
    a.move_from_ccr(t1); a.movel(A_::D(t1), A_::D(t2)); a.lsr(4, t2); a.andi(4, 1, A_::D(t2)); a.or_(4, A_::D(t2), t1); a.move_to_ccr(A_::D(t1));   // C := X
    a.movem_from(A_::Ind(7), (u16)((1u << t1) | (1u << t2))); a.lea(A_::Disp(7, 8), 7);
    return a.finish();
  }
  if ((grp == 0x8 && (op & 0x1F0) == 0x100) || (grp == 0xC && (op & 0x1F0) == 0x100)) bad("ABCD/SBCD não são suportados");
  if ((grp == 0x9 || grp == 0xD) && ((op >> 6) & 3) != 3 && (op & 0x100) && mode == 1) bad("ADDX/SUBX em memória não são suportados");
  if (grp == 0x4 && (op & 0xFF00) == 0x4000 && ((op >> 6) & 3) != 3 && mode != 0) bad("NEGX em memória não é suportado");
  if (grp == 0xA || grp == 0xF) {                                                  // linhas A/F: o ColdFire usa outros significados -> ILLEGAL
    c.notes.push_back("linha A/F convertida em ILLEGAL"); expanded = true; a.illegal(); return a.finish();
  }
  if (op == 0x4E76) bad("TRAPV não é suportado");

  // ---- desvios e chamadas relativos ao PC ----
  if (grp == 0x6) {
    int cc = (op >> 8) & 15; i32 disp = (i8)op; u32 base = addr68 + 2;
    if ((op & 0xFF) == 0) disp = (i16)w[1]; else if ((op & 0xFF) == 0xFF) disp = (i32)(((u32)w[1] << 16) | w[2]);
    u32 target = map_addr(c, base + (u32)disp); i32 d = (i32)target - (i32)(cf_addr + 2);
    if (c.pass2 && (d < -32768 || d > 32767)) bad("desvio fora de alcance de 16 bits após a tradução");
    a.w((u16)(0x6000 | (cc << 8))); a.w((u16)d); return a.finish();
  }

  // ---- cópia com ajuste de endereçamento relativo ao PC / absolutos que apontam para código ----
  std::vector<u16> out(w.begin(), w.end());
  std::vector<u16> rebuilt; rebuilt.push_back(op);
  // reconstrói palavra a palavra: palavras antes do primeiro EA e entre EAs são copiadas; cada EA relevante é reescrito
  size_t pos = 1; u16 newop = op;
  bool control = (op & 0xF1C0) == 0x41C0 || (op & 0xFFC0) == 0x4E80 || (op & 0xFFC0) == 0x4EC0 || (op & 0xFFC0) == 0x4840;
  for (auto& e : in.eas) {
    while (pos < (size_t)e.ext_idx) rebuilt.push_back(w[pos++]);
    int m = e.mode, r = e.reg;
    if (m == 7 && r == 2) {                                                        // d16(PC) -> abs.l com o endereço traduzido
      u32 ext_addr = addr68 + 2 * (u32)e.ext_idx; u32 t = ext_addr + (u32)(i32)(i16)w[e.ext_idx]; u32 ct = map_addr(c, t);
      newop = (u16)((newop & ~(0x3F << e.shift)) | (((7 << 3) | 1) << e.shift)); rebuilt.push_back((u16)(ct >> 16)); rebuilt.push_back((u16)ct); pos += 1; expanded = expanded || false;
    } else if (m == 7 && r == 3) { bad("endereçamento PC+índice não é suportado"); }
    else if (m == 7 && r == 1 && c.opt.remap_chipset && ((((u32)w[e.ext_idx] << 16) | w[e.ext_idx + 1]) - 0xDFF000u) < 0x1000u) {   // registradores do chipset: $DFFxxx -> CLASSIC_BASE+xxx
      u32 t = (((u32)w[e.ext_idx] << 16) | w[e.ext_idx + 1]) - 0xDFF000u + 0xF00D0000u; rebuilt.push_back((u16)(t >> 16)); rebuilt.push_back((u16)t); pos += 2;
    }
    else if (m == 7 && (r == 0 || r == 1) && control) {                            // JSR/JMP/LEA/PEA com endereço absoluto: se cair no código traduzido, remapeia
      u32 t = r == 0 ? (u32)(i32)(i16)w[e.ext_idx] : (((u32)w[e.ext_idx] << 16) | w[e.ext_idx + 1]); bool inside; u32 ct = map_addr(c, t, &inside);
      if (inside) { newop = (u16)((newop & ~(0x3F << e.shift)) | (((7 << 3) | 1) << e.shift)); rebuilt.push_back((u16)(ct >> 16)); rebuilt.push_back((u16)ct); pos += e.ext_cnt; }
    }
  }
  while (pos < w.size()) rebuilt.push_back(w[pos++]);
  rebuilt[0] = newop;
  for (u16 x : rebuilt) a.w(x);
  return a.finish();
}

}  // namespace

XlatResult translate_68k(const std::vector<u8>& code68, const XlatOptions& opt) {
  XlatResult r; r.base_cf = opt.base_cf;
  try {
    auto in_data = [&](u32 a, u32* end) { for (auto& d : opt.data_ranges) if (a >= d.first && a < d.second) { if (end) *end = d.second; return true; } return false; };
    std::map<u32, u32> map, dmap_start; std::vector<std::string> notes;
    Ctx c{code68, opt, map, false, notes, nullptr};
    c.data_map = [&](u32 t) -> u32 { for (auto& d : opt.data_ranges) if (t >= d.first && t < d.second) { auto it = dmap_start.find(d.first); if (it != dmap_start.end()) return it->second + (t - d.first); } return 0; };
    struct Item { u32 addr68; bool is_data; std::vector<u16> words; u32 data_end; };
    std::vector<Item> items; u32 end68 = opt.base68 + (u32)code68.size();
    for (size_t p = 0; p < code68.size();) {                                     // decodifica instruções e separa os dados
      u32 a = opt.base68 + (u32)p, dend = 0;
      if (in_data(a, &dend)) { u32 e = std::min(dend, end68); items.push_back({a, true, {}, e}); p = e - opt.base68; continue; }
      int len = insn_len(&code68[p], code68.size() - p);
      if (p + (size_t)len > code68.size()) bad("instrução truncada no fim do código");
      Item it{a, false, {}, 0}; for (int i = 0; i < len; i += 2) it.words.push_back(rbe16(&code68[p + i])); items.push_back(it); p += (size_t)len; r.source_insns++;
    }
    std::vector<std::vector<u8>> out(items.size());
    // passo 1: mede os tamanhos (endereços de destino ainda desconhecidos: valores provisórios não mudam o tamanho) e monta o mapa
    u32 cf = opt.base_cf;
    for (size_t i = 0; i < items.size(); i++) {
      Item& it = items[i];
      if (it.is_data) { dmap_start[it.addr68] = cf; size_t off = it.addr68 - opt.base68, n = it.data_end - it.addr68; out[i].assign(code68.begin() + (long)off, code68.begin() + (long)(off + n)); if (out[i].size() & 1) out[i].push_back(0); }
      else { map[it.addr68] = cf; bool ex = false; out[i] = translate_one(c, it.addr68, it.words, cf, ex); }
      cf += (u32)out[i].size();
    }
    // passo 2: refaz com o mapa completo
    c.pass2 = true; cf = opt.base_cf; notes.clear();
    for (size_t i = 0; i < items.size(); i++) {
      Item& it = items[i];
      if (!it.is_data) {
        bool ex = false; std::vector<u8> b = translate_one(c, it.addr68, it.words, cf, ex);
        if (b.size() != out[i].size()) bad("o tamanho da tradução mudou entre os passos");
        out[i] = b; if (ex) r.expanded++;
      }
      cf += (u32)out[i].size();
    }
    for (auto& o : out) r.code.insert(r.code.end(), o.begin(), o.end());
    r.map = map; for (auto& d : dmap_start) r.map[d.first] = d.second;
    r.cf_bytes = (int)r.code.size(); r.notes = notes; r.ok = true;
  } catch (std::runtime_error& e) { r.error = e.what(); }
  return r;
}

}  // namespace sf
