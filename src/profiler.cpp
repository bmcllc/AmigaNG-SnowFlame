#include "profiler.h"
#include "cfdis.h"

namespace sf {

void Profiler::start() {
  if (on) return; on = true; prev = soc.on_step; t0 = soc.sched.now; cyc0 = soc.cpu.cycles; ins0 = soc.cpu.instrs;
  soc.on_step = [this](u32 pc, int c) { if (prev) prev(pc, c); hook(pc, c); };
}
void Profiler::stop() { if (!on) return; on = false; soc.on_step = prev; }
void Profiler::reset() { pc_cycles.clear(); pc_hits.clear(); total = instrs = 0; stack.clear(); fn.clear(); t0 = soc.sched.now; cyc0 = soc.cpu.cycles; ins0 = soc.cpu.instrs; }

std::string Profiler::function_of(u32 pc) const {
  const std::string* best = nullptr; u32 ba = 0;
  for (auto& kv : symbols) if (kv.second <= pc && (!best || kv.second > ba)) { best = &kv.first; ba = kv.second; }
  if (best) return *best;
  char b[24]; std::snprintf(b, sizeof b, "0x%08x", pc & ~0xFFu); return b;
}

void Profiler::hook(u32 pc, int c) {
  if (!c) return;
  total += c; instrs++; pc_cycles[pc] += c; pc_hits[pc]++;
  std::string f = function_of(pc);
  Func& F = fn[f]; if (F.name.empty()) { F.name = f; auto it = symbols.find(f); F.addr = it == symbols.end() ? 0 : it->second; }
  F.excl += c; F.instrs++;
  if (stack.empty()) stack.push_back({f, 0});
  stack.back().cycles += c;
  u16 op = 0; soc.bus.peek16(pc, op);
  bool call = (op & 0xFF00) == 0x6100 || (op & 0xFFC0) == 0x4E80;
  if (call && !soc.cpu.halted) { std::string callee = function_of(soc.cpu.pc); fn[callee].name = callee; fn[callee].calls++; stack.push_back({callee, 0}); }
  else if (op == 0x4E75 && stack.size() > 1) { Frame fr = stack.back(); stack.pop_back(); bool rec = false; for (auto& x : stack) if (x.fn == fr.fn) rec = true; if (!rec) fn[fr.fn].incl += fr.cycles; stack.back().cycles += fr.cycles; }   // recursão: só a ativação mais externa conta
}

std::vector<Profiler::Func> Profiler::functions() const {
  std::map<std::string, Func> m = fn;
  // frames ainda abertos: o inclusivo de cada um soma os ciclos dos frames mais profundos
  u64 acc = 0; for (size_t i = stack.size(); i-- > 0;) { acc += stack[i].cycles; bool rec = false; for (size_t j = 0; j < i; j++) if (stack[j].fn == stack[i].fn) rec = true; if (!rec) m[stack[i].fn].incl += acc; }
  std::vector<Func> v; for (auto& kv : m) v.push_back(kv.second);
  std::sort(v.begin(), v.end(), [](const Func& a, const Func& b) { return a.excl != b.excl ? a.excl > b.excl : a.name < b.name; });
  return v;
}

std::string Profiler::report(int top) const {
  std::string o; char b[256];
  u64 cyc = soc.cpu.cycles - cyc0; Ps dt = soc.sched.now - t0; double us = dt / 1e6;
  std::snprintf(b, sizeof b, "== Profiler SF-100: %.3f us simulados, %llu instruções (%llu amostradas), %llu ciclos de CPU ==\n", us, (unsigned long long)(soc.cpu.instrs - ins0), (unsigned long long)instrs, (unsigned long long)cyc); o += b;
  if (dt) { double busy = (double)cyc * 1e6 / 266.0 / (double)dt * 100.0; std::snprintf(b, sizeof b, "CPU: %.1f%% do tempo executando (IPC %.2f, CPI %.2f)\n", busy > 100 ? 100.0 : busy, instrs ? (double)instrs / (double)total : 0.0, instrs ? (double)total / (double)instrs : 0.0); o += b; }
  o += "\n-- funções (ciclos exclusivos / inclusivos / chamadas) --\n";
  int n = 0;
  for (auto& f : functions()) {
    if (n++ >= top) break; std::snprintf(b, sizeof b, "%-28s %10llu (%5.1f%%) %10llu %8llu\n", f.name.c_str(), (unsigned long long)f.excl, total ? 100.0 * f.excl / total : 0.0, (unsigned long long)f.incl, (unsigned long long)f.calls); o += b;
  }
  o += "\n-- instruções mais quentes --\n";
  std::vector<std::pair<u64, u32>> hot; for (auto& kv : pc_cycles) hot.push_back({kv.second, kv.first}); std::sort(hot.begin(), hot.end(), [](auto& a, auto& b2) { return a.first != b2.first ? a.first > b2.first : a.second < b2.second; });
  std::function<std::string(u32)> sf = [&](u32 a) { for (auto& kv : symbols) if (kv.second == a) return kv.first; return std::string(); };
  auto rd = [&](u32 a) { u16 v = 0; soc.bus.peek16(a, v); return v; };
  for (int i = 0; i < top && i < (int)hot.size(); i++) {
    CfInsn in = cf_disasm(rd, hot[i].second, &sf); auto h = pc_hits.find(hot[i].second);
    std::snprintf(b, sizeof b, "0x%08x %10llu (%5.1f%%) x%-8llu %s\n", hot[i].second, (unsigned long long)hot[i].first, total ? 100.0 * hot[i].first / total : 0.0, (unsigned long long)(h == pc_hits.end() ? 0 : h->second), in.text.c_str()); o += b;
  }
  o += "\n-- dispositivos --\n";
  auto pct = [&](u64 cycles, u64 hz) { return dt ? 100.0 * (double)cycles / hz * 1e12 / (double)dt : 0.0; };
  std::snprintf(b, sizeof b, "VPU-A: %llu ciclos (%.1f%% do tempo), %llu jobs   VPU-B: %llu ciclos (%.1f%%), %llu jobs\n", (unsigned long long)soc.vpuA.total_cycles, pct(soc.vpuA.total_cycles, VPU_HZ), (unsigned long long)soc.vpuA.jobs_done, (unsigned long long)soc.vpuB.total_cycles, pct(soc.vpuB.total_cycles, VPU_HZ), (unsigned long long)soc.vpuB.jobs_done); o += b;
  std::snprintf(b, sizeof b, "GPU: %llu ciclos (%.1f%%), %llu listas, %llu triângulos, %llu pixels sombreados\n", (unsigned long long)soc.gpu.total.cycles, pct(soc.gpu.total.cycles, GPU_HZ), (unsigned long long)soc.gpu.lists_done, (unsigned long long)soc.gpu.total.tris_drawn, (unsigned long long)soc.gpu.total.pix_shaded); o += b;
  std::snprintf(b, sizeof b, "Blitter: %llu jobs, %llu pixels, %llu sprites   DMA: %llu jobs, %llu bytes\n", (unsigned long long)soc.blt.jobs, (unsigned long long)soc.blt.pixels, (unsigned long long)soc.blt.sprites_drawn, (unsigned long long)soc.bridge.dma_jobs, (unsigned long long)soc.bridge.dma_bytes); o += b;
  const Bus& bu = soc.bus;
  std::snprintf(b, sizeof b, "Árbitro: DDR %llu B, VRAM %llu B, ARAM %llu B, espera acumulada %.3f us\n", (unsigned long long)bu.arb_bytes[Bus::R_DDR], (unsigned long long)bu.arb_bytes[Bus::R_VRAM], (unsigned long long)bu.arb_bytes[Bus::R_ARAM], bu.arb_wait_ps / 1e6); o += b;
  u64 dh = bu.dc.hits, dm = bu.dc.misses, ih = bu.ihits, im = bu.imisses;
  std::snprintf(b, sizeof b, "D-cache: %.1f%% acertos (%llu/%llu), %llu write-backs   I-cache: %.1f%% acertos (%llu/%llu)   TLB: %llu acertos, %llu faltas\n", dh + dm ? 100.0 * dh / (dh + dm) : 0.0, (unsigned long long)dh, (unsigned long long)(dh + dm), (unsigned long long)bu.dc.writebacks, ih + im ? 100.0 * ih / (ih + im) : 0.0, (unsigned long long)ih, (unsigned long long)(ih + im), (unsigned long long)bu.tlb_hits, (unsigned long long)bu.tlb_misses); o += b;
  std::snprintf(b, sizeof b, "SnowPaula: pico do DSP %u permil., %llu vozes-quadro descartadas\n", soc.paula.dsp_peak, (unsigned long long)soc.paula.dsp_drops); o += b;
  return o;
}

}  // namespace sf
