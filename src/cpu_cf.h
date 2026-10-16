// SnowFire-CF266: interpretador do núcleo Motorola ColdFire V4e (ISA_C + FPU + EMAC_B com as codificações do CFPRM rev. 3).
// Codificações de opcode reais do ColdFire/68k; veja docs/SF100-ABI.md para o subconjunto suportado.
#pragma once
#include "bus.h"

namespace sf {

class Cpu {
 public:
  explicit Cpu(Bus& b) : bus(b) { reset_regs(); }
  enum Reason { RUNNING = 0, HALT_INSN, DOUBLE_FAULT, NO_VECTOR };

  u32 d[8], a[8];            // a[7] = SP
  u32 pc = 0, sr = 0x2700, vbr = 0;
  double fp[8]; bool fp_n = false, fp_z = false, fp_nan = false;
  u32 fpcr = 0, fpiar = 0;   // FPCR: PREC [7:6] (1 = simples), RND [5:4] (0 próx., 1 zero, 2 -inf, 3 +inf)
  i64 acc[4] = {0, 0, 0, 0}; // acumuladores eMAC de 48 bits: ACC[47:0] (32 bits + extensão de 16) guardado com extensão de sinal
  // MACSR: bits 11-8 PAV3-PAV0, 7 OMC (satura na leitura), 6 S/U, 5 F/I, 4 R/T, 3 N, 2 Z, 1 V, 0 EV
  // MASK: 16 bits implementados (o acesso lê 0xFFFF nos 16 bits altos)
  u32 macsr = 0, mask = 0xFFFFu, cacr = 0;
  u32 other_sp = 0;          // SP inativo: USP em supervisor, SSP em usuário (troca ao mudar SR[S])
  bool halted = false, stopped = false; Reason reason = RUNNING;
  int exc_vec = -1; u32 exc_pc = 0, exc_addr = 0;      // última exceção: vetor, PC da instrução e (erro de acesso) endereço da falha
  u64 cycles = 0, instrs = 0;

  void reset_regs();
  // Reset de hardware: SP = [VBR+0], PC = [VBR+4]
  void reset();
  void set_pc(u32 p) { pc = p; halted = false; stopped = false; reason = RUNNING; }
  // Executa uma instrução (ou atende IRQ). Retorna ciclos consumidos (0 se parado em STOP/HALT).
  int step();
  bool idle() const { return halted || stopped; }
  enum { C = 1, V = 2, Z = 4, N = 8, X = 16 };

 private:
  Bus& bus; int cyc = 1;
  u32 agu_prev = 0, agu_cur = 0;   // Ax escritos pela instrução anterior/atual (interlock de geração de endereço: +1 ciclo ao usar Ax como base logo em seguida)
  void agu_use(int r) { if (agu_prev & (1u << r)) cyc++; }
  void agu_set(int r) { agu_cur |= 1u << r; }
  struct Illegal {}; struct DivZero {}; struct Priv {};
  struct EA { int k; int reg; u32 addr; u32 imm; };    // k: 0 Dn, 1 An, 2 mem, 3 imediato
  u16 fetch() { bus.cpu_super = sr & 0x2000; u16 w = bus.fetch16(pc); pc += 2; return w; }
  u32 fetch32() { u32 h = fetch(); return (h << 16) | fetch(); }
  u32 brief(u32 base);
  EA ea(int mode, int reg, int size);
  u32 ea_read(const EA& e, int size);
  void ea_write(const EA& e, int size, u32 v);
  u32 rd(u32 a_, int sz) { cyc++; bus.cpu_super = sr & 0x2000; return bus.cpu_read(a_, sz); }
  void wr(u32 a_, int sz, u32 v) { cyc++; bus.cpu_super = sr & 0x2000; bus.cpu_write(a_, sz, v); }
  void push32(u32 v) { a[7] -= 4; wr(a[7], 4, v); }
  u32 pop32() { u32 v = rd(a[7], 4); a[7] += 4; return v; }
  void exception(int vec, u32 retpc, int newmask);
  void set_sr(u32 v);
  void priv() { if (!(sr & 0x2000)) throw Priv(); }
  struct Prod { i64 v = 0; bool ovf = false; };             // produto alinhado ao acumulador; ovf: não coube no campo do modo
  Prod emac_product(u32 xv, u32 yv, bool word, int scale) const;
  bool emac_fits(i64 v) const;                              // v cabe em 32 bits (inteiro) ou 40 bits (fracionário), com ou sem sinal
  bool emac_add(int n, const Prod& p, bool sub, bool setflags);
  void emac_flags(int n, bool ev);
  void emac_set(int n, u32 v);
  u32 emac_store(int n) const;
  void emac_mac(u16 op);
  void emac_copy(int x, int y);
  u32 gr(int r) const { return r < 8 ? d[r] : a[r - 8]; }
  void gw(int r, u32 v) { if (r < 8) d[r] = v; else a[r - 8] = v; }
  bool cond(int cc) const;
  void setccr(u32 bits, u32 mask) { sr = (sr & ~mask) | (bits & mask); }
  void setnz(u32 r, int size);
  u32 do_add(u32 x, u32 y, int size, bool setx = true);
  u32 do_sub(u32 dst, u32 src, int size, bool setx = true);
  u32 do_shift(u32 val, int type, bool left, int cnt, int size);
  void exec();
  void exec_fpu(u16 op);
  double fp_load(int fmt, const EA& e);
  void fp_store(int fmt, const EA& e, double v);
  void fp_cc(double v);
  double fp_round(double v) const;
  u32 fpsr() const { return (fp_n ? 1u << 27 : 0) | (fp_z ? 1u << 26 : 0) | (fp_nan ? 1u << 24 : 0); }
  u32 do_addx(u32 x, u32 y, int size);
  u32 do_subx(u32 dst, u32 src, int size);
  bool fbcond(int c) const;
};

}  // namespace sf
