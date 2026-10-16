// Cpu68k: interpretador do Motorola 68000 (com extensões 68020 de uso comum) para o SnowOS Classic.
// Memória e interrupções vêm de uma interface abstrata (M68kMem); o Classic liga isso ao barramento do SF-100 e ao chipset Amiga.
// Cobertura: ISA 68000 completa (BCD, MOVEP, CHK, TAS, trace, exceções 3/4/5/6/7/8/9/10/11, TRAP, autovetores de IRQ) + 68020:
// EXTB.L, MULx.L/DIVx.L (64 bits), LINK.L, Bcc/BRA/BSR.L, RTD, TRAPcc, EA com formato completo (bd, índice escalado, indireção de memória).
#pragma once
#include "common.h"

namespace sf {

class M68kMem {
 public:
  virtual ~M68kMem() {}
  virtual u8 read8(u32 a) = 0;
  virtual u16 read16(u32 a) = 0;
  virtual void write8(u32 a, u8 v) = 0;
  virtual void write16(u32 a, u16 v) = 0;
  virtual int irq_level() { return 0; }        // nível de interrupção pendente (0..7)
  virtual void irq_ack(int level) {}           // CPU atendeu a interrupção
  virtual void reset_signal() {}               // instrução RESET
};

class Cpu68k {
 public:
  explicit Cpu68k(M68kMem& m) : mem(m) { reset_regs(); }
  enum { C = 1, V = 2, Z = 4, N = 8, X = 16 };
  u32 d[8], a[8];            // a[7] = SP ativo (USP ou SSP conforme SR.S)
  u32 pc = 0, usp = 0, ssp = 0, vbr = 0; u16 sr = 0x2700;
  bool stopped = false, halted = false, m68020 = false;      // m68020: endereços de 32 bits, sem erro de endereço em acessos ímpares
  u64 cycles = 0, instrs = 0, ifetch_words = 0;     // ifetch_words: palavras de instrução buscadas (usado pelo tradutor para medir o tamanho das instruções)

  void reset_regs();
  void reset();                                              // SP = [0], PC = [4]
  int step();                                                // devolve ciclos (0 se parada em STOP/halt)
  void set_sr(u32 v);
  u32 addr_mask() const { return m68020 ? 0xFFFFFFFFu : 0x00FFFFFFu; }
  u32 read32(u32 ad) { return ((u32)rd16(ad) << 16) | rd16(ad + 2); }

 private:
  M68kMem& mem; int cyc = 0;
  struct Fault {}; struct IllegalI {}; struct PrivI {}; struct AddrErr { u32 addr; bool write; bool instr; };
  struct EA { int kind = 0; int reg = 0; u32 addr = 0, val = 0; };   // kind: 0 Dn, 1 An, 2 memória, 3 imediato
  u16 rd16(u32 ad); u8 rd8(u32 ad); void wr16(u32 ad, u16 v); void wr8(u32 ad, u8 v);
  u32 rd(u32 ad, int size) { return size == 1 ? rd8(ad) : size == 2 ? rd16(ad) : read32(ad); }
  void wr(u32 ad, int size, u32 v) { if (size == 1) wr8(ad, (u8)v); else if (size == 2) wr16(ad, (u16)v); else { wr16(ad, (u16)(v >> 16)); wr16(ad + 2, (u16)v); } }
  u16 fetch() { u16 w = rd16(pc); pc += 2; ifetch_words++; return w; }
  u32 fetch32() { u32 h = fetch(); return (h << 16) | fetch(); }
  void push16(u16 v) { a[7] -= 2; wr16(a[7], v); }
  void push32(u32 v) { a[7] -= 4; wr16(a[7], (u16)(v >> 16)); wr16(a[7] + 2, (u16)v); }
  u16 pop16() { u16 v = rd16(a[7]); a[7] += 2; return v; }
  u32 pop32() { u32 v = read32(a[7]); a[7] += 4; return v; }

  EA ea(int mode, int reg, int size);
  u32 ea_read(EA& e, int size);
  void ea_write(EA& e, int size, u32 v);
  u32 index_ext(u32 base);
  bool cond(int cc) const;
  void exception(int vec, u32 retpc, int newmask = -1);
  void address_error(const AddrErr& e, u32 start_pc, u16 ir);
  void exec();
  void setnz(u32 r, int size);
  u32 do_add(u32 s, u32 dst, int size, bool x);
  u32 do_sub(u32 s, u32 dst, int size, bool x, bool store_z_sticky = false);
  u32 do_shift(int type, bool left, u32 val, int cnt, int size);
  u32 bcd_add(u32 s, u32 d); u32 bcd_sub(u32 s, u32 d);
  void priv() { if (!(sr & 0x2000)) throw PrivI(); }
  void ccr_set(u32 bits, u32 mask) { sr = (u16)((sr & ~mask) | (bits & mask)); }
};

}  // namespace sf
