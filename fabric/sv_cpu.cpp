// Frontend da CPU realizada em SystemVerilog (rtl/sf_cpu.sv). Os campos públicos de Cpu (registradores, flags, contadores) são o
// espelho do estado arquitetural do backend: ele os publica ao fim de cada passo (fab_cpu_state) e, se o frontend os alterou
// desde então (depurador, carregador, testes), as alterações voltam ao backend antes do passo seguinte (cpu_set_state).
#include "fabric.h"
#include "Vsf100__Dpi.h"

namespace sf {

// Estado serializado na ordem dos argumentos de cpu_set_state/fab_cpu_state (sem bytes de preenchimento: comparação por memcmp).
static std::vector<u8> serialize(const Cpu& c) {
  std::vector<u8> b; b.reserve(256);
  auto put = [&](const void* p, size_t n) { const u8* q = (const u8*)p; b.insert(b.end(), q, q + n); };
  u8 fl[5] = {c.fp_n, c.fp_z, c.fp_nan, c.halted, c.stopped}; i32 rs = c.reason;
  put(c.d, sizeof c.d); put(c.a, sizeof c.a); put(&c.pc, 4); put(&c.sr, 4); put(&c.vbr, 4); put(c.fp, sizeof c.fp); put(fl, 5);
  put(&c.fpcr, 4); put(&c.fpiar, 4); put(c.acc, sizeof c.acc); put(&c.macsr, 4); put(&c.mask, 4); put(&c.cacr, 4); put(&c.other_sp, 4);
  put(&rs, 4); put(&c.exc_vec, 4); put(&c.exc_pc, 4); put(&c.exc_addr, 4); put(&c.cycles, 8); put(&c.instrs, 8);
  return b;
}

void Fabric::cpu_pull_done() { cpu_shadow = serialize(soc.cpu); }

void Fabric::cpu_push() {
  const Cpu& c = soc.cpu;
  if (!cpu_shadow.empty() && serialize(c) == cpu_shadow) return;   // nada mudou do lado do frontend
  enter(FD_CPU);
  cpu_set_state(c.d[0], c.d[1], c.d[2], c.d[3], c.d[4], c.d[5], c.d[6], c.d[7], c.a[0], c.a[1], c.a[2], c.a[3], c.a[4], c.a[5], c.a[6], c.a[7],
                c.pc, c.sr, c.vbr, c.fp[0], c.fp[1], c.fp[2], c.fp[3], c.fp[4], c.fp[5], c.fp[6], c.fp[7], c.fp_n, c.fp_z, c.fp_nan, c.fpcr, c.fpiar,
                c.acc[0], c.acc[1], c.acc[2], c.acc[3], c.macsr, c.mask, c.cacr, c.other_sp, c.halted, c.stopped, (int)c.reason,
                c.exc_vec, c.exc_pc, c.exc_addr, c.cycles, c.instrs);
  cpu_pull_done();
}

// Estado de reset dos registradores (lado do frontend: chega ao backend no próximo passo)
void Cpu::reset_regs() {
  for (int i = 0; i < 8; i++) { d[i] = a[i] = 0; fp[i] = 0; }
  sr = 0x2700; vbr = 0; for (auto& x : acc) x = 0; macsr = 0; mask = 0xFFFFu; cacr = 0; other_sp = 0; fpcr = fpiar = 0; fp_n = fp_z = fp_nan = false; halted = stopped = false; reason = RUNNING;
}
void Cpu::reset() { reset_regs(); Fabric& f = fabric_of(bus); f.cpu_push(); f.enter(FD_CPU); cpu_reset(); }
int Cpu::step() { Fabric& f = fabric_of(bus); f.cpu_push(); f.enter(FD_CPU); return cpu_step(); }

}  // namespace sf
