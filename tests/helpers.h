#pragma once
#include "framework.h"
#include "soc.h"
#include "cfasm.h"

using namespace sf;
using namespace sf::abi;
using A = sf::CfAsm;

constexpr u32 CODE_BASE = 0x1000;

// Carrega e executa um programa ColdFire até HALT. Faz flush da cache para os testes poderem inspecionar a DDR.
inline bool run_prog(Soc& s, A& a, Ps limit = 2000000000ull, u32 sp = 0x00F00000) {
  auto code = a.finish(); s.load(a.base(), code);
  s.cpu.reset_regs(); s.cpu.a[7] = sp; s.cpu.set_pc(a.base());
  bool h = s.run_until_halt(limit);
  s.bus.cache_flush_all(false);
  return h;
}
inline void mmio_w(Soc& s, u32 addr, u32 v) { s.bus.cpu_write(addr, 4, v); }
inline u32 mmio_r(Soc& s, u32 addr) { return s.bus.cpu_read(addr, 4); }
// Avança o relógio simulado (sem CPU) em ns.
inline void advance_ns(Soc& s, u64 ns) { s.sched.advance_to(s.sched.now + ns * 1000); }
inline void put_f(Soc& s, u32 addr, float f) { s.bus.raw_w32(addr, f2u(f)); }
inline void put_vec(Soc& s, u32 addr, float x, float y, float z, float w) { put_f(s, addr, x); put_f(s, addr + 4, y); put_f(s, addr + 8, z); put_f(s, addr + 12, w); }
