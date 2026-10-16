// Profiler do SF-100: amostragem exata por instrução (ciclos por PC), tempo exclusivo/inclusivo por função (pilha de chamadas via
// BSR/JSR/RTS), uso dos dispositivos (VPUs, GPU, Blitter, DMA, árbitro de memória) e taxas de acerto das caches.
#pragma once
#include "soc.h"
#include <map>
#include <unordered_map>

namespace sf {

class Profiler {
 public:
  explicit Profiler(Soc& s) : soc(s) {}
  ~Profiler() { stop(); }
  std::map<std::string, u32> symbols;
  void start();                          // instala o gancho Soc::on_step
  void stop();
  void reset();
  struct Func { std::string name; u32 addr = 0; u64 excl = 0, incl = 0, calls = 0, instrs = 0; };
  std::vector<Func> functions() const;   // ordenado por ciclos exclusivos (decrescente)
  u64 total_cycles() const { return total; }
  u64 cycles_at(u32 pc) const { auto it = pc_cycles.find(pc); return it == pc_cycles.end() ? 0 : it->second; }
  std::string report(int top = 12) const;
  std::string function_of(u32 pc) const;

 private:
  Soc& soc; bool on = false; std::function<void(u32, int)> prev;
  std::unordered_map<u32, u64> pc_cycles, pc_hits; u64 total = 0, instrs = 0;
  struct Frame { std::string fn; u64 cycles; };
  std::vector<Frame> stack; std::map<std::string, Func> fn;
  Ps t0 = 0; u64 cyc0 = 0, ins0 = 0;
  void hook(u32 pc, int cycles);
};

}  // namespace sf
