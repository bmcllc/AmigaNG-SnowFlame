#include "vpu.h"

namespace sf {
using namespace abi;

Vpu::Vpu(Bus& b, u32 bit) : bus(b), irq_bit(bit) { reset_state(); }

void Vpu::reset_state() {
  std::memset(imem, 0, sizeof imem); std::memset(dmem, 0, sizeof dmem);
  std::memset(v, 0, sizeof v); std::memset(s, 0, sizeof s);
  queue.clear(); running = done = faulted = false; gen++;
}

void Vpu::load_program(const std::vector<u32>& code, u32 at) {
  if ((at + code.size()) * 4 > VPU_IMEM_SIZE) throw std::runtime_error("programa VPU excede 8 KiB");
  for (size_t i = 0; i < code.size(); i++) wbe32(imem + (at + i) * 4, code[i]);
}

u32 Vpu::status() const {
  return (running ? (u32)VPUS_BUSY : 0u) | (done ? (u32)VPUS_DONE : 0u) | (faulted ? (u32)VPUS_FAULT : 0u) | ((u32)queue.size() << 8);
}

// ---------------- Núcleo de execução ----------------
static inline void swz4(const float* in, u32 swz, float* out) {
  for (int i = 0; i < 4; i++) out[i] = in[(swz >> (2 * i)) & 3];
}

Vpu::Result Vpu::run_now(u32 pc, const u32 sregs[8]) {
  Result r; for (int i = 0; i < 8; i++) s[i] = sregs[i];
  bool lt = false, eq = false;
  const u32 nwords = VPU_IMEM_SIZE / 4;
  const u64 WATCHDOG = 4000000;
  auto fault = [&](u32 at) { r.fault = true; r.pc = at; };
  // DMA local: copia funcional no instante da instrução; o término (em ciclos do job) vem do árbitro de memória.
  const Ps t0 = bus.sched ? bus.sched->now : 0;
  Ps dma_end = t0; u64 dma_done = 0;
  auto dma = [&](u32 dm, u32 ext, u32 len, bool in) -> bool {
    if ((dm | ext | len) & 3 || (u64)dm + len > VPU_DMEM_SIZE) return false;
    u8* p = bus.raw(ext, len); if (len && !p) return false;
    if (in) std::memcpy(dmem + dm, p, len); else if (len) std::memcpy(p, dmem + dm, len);
    Ps start = std::max(t0 + cyc2ps(r.cycles + VPU_DMA_SETUP, VPU_HZ), dma_end);
    dma_end = bus.reserve(ext, len, start);
    dma_done = (u64)(((__uint128_t)(dma_end - t0) * VPU_HZ + 999999999999ull) / 1000000000000ull);
    r.dma_bytes += len; r.dma_xfers++;
    return true;
  };
  while (true) {
    if (pc >= nwords) { fault(pc); break; }
    if (r.cycles >= WATCHDOG) { fault(pc); break; }
    u32 w = rbe32(imem + pc * 4);
    u32 op = w >> 26, a = (w >> 21) & 31, b = (w >> 16) & 31, c = (w >> 11) & 31, swz = w & 0xFF;
    i32 imm = (i16)(w & 0xFFFF);
    u32 cur = pc++;
    r.cycles++;
    float vb[4];
    switch (op) {
      case vop::HALT: r.cycles = std::max(r.cycles, dma_done); return r;     // o job só termina com o DMA concluído
      case vop::NOP: break;
      case vop::VADD: case vop::VSUB: case vop::VMUL: case vop::VMAC: case vop::VMIN: case vop::VMAX: {
        swz4(v[c], swz, vb); float* d = v[a]; const float* x = v[b]; float t[4];
        for (int i = 0; i < 4; i++) {
          switch (op) {
            case vop::VADD: t[i] = x[i] + vb[i]; break;
            case vop::VSUB: t[i] = x[i] - vb[i]; break;
            case vop::VMUL: t[i] = x[i] * vb[i]; break;
            case vop::VMAC: t[i] = d[i] + x[i] * vb[i]; break;
            case vop::VMIN: t[i] = x[i] < vb[i] ? x[i] : vb[i]; break;
            default:        t[i] = x[i] > vb[i] ? x[i] : vb[i]; break;
          }
        }
        std::memcpy(d, t, 16); r.flops += (op == vop::VMAC) ? 8 : 4; break;
      }
      case vop::VDOT3: case vop::VDOT4: {
        swz4(v[c], swz, vb); const float* x = v[b];
        float d = x[0] * vb[0] + x[1] * vb[1] + x[2] * vb[2];
        if (op == vop::VDOT4) { d += x[3] * vb[3]; r.flops += 7; } else r.flops += 5;
        for (int i = 0; i < 4; i++) v[a][i] = d; break;
      }
      case vop::VMOV: case vop::VRCP: case vop::VRSQ: case vop::VABS: case vop::VFRC: {
        swz4(v[c], swz, vb); float t[4];
        for (int i = 0; i < 4; i++) {
          switch (op) {
            case vop::VMOV: t[i] = vb[i]; break;
            case vop::VRCP: t[i] = 1.0f / vb[i]; break;
            case vop::VRSQ: t[i] = 1.0f / std::sqrt(vb[i]); break;
            case vop::VFRC: t[i] = vb[i] - std::floor(vb[i]); break;
            default:        t[i] = std::fabs(vb[i]); break;
          }
        }
        std::memcpy(v[a], t, 16); if (op != vop::VMOV) r.flops += 4; break;
      }
      case vop::VLD: case vop::VST: {
        u32 addr = s[b & 7] + (u32)(imm * 16);
        if ((addr & 15) || addr + 16 > VPU_DMEM_SIZE) { fault(cur); return r; }
        if (op == vop::VLD) for (int i = 0; i < 4; i++) v[a][i] = rbef(dmem + addr + 4 * i);
        else for (int i = 0; i < 4; i++) wbef(dmem + addr + 4 * i, v[a][i]);
        break;
      }
      case vop::DMAIN:  if (!dma(s[a & 7], s[b & 7], s[c & 7], true)) { fault(cur); return r; } break;
      case vop::DMAOUT: if (!dma(s[b & 7], s[a & 7], s[c & 7], false)) { fault(cur); return r; } break;
      case vop::DMAWAIT: r.cycles = std::max(r.cycles, dma_done); break;
      case vop::SIGNAL: r.sigs.push_back({r.cycles, a & 15}); break;
      case vop::SETA: s[a & 7] = (u32)imm; break;
      case vop::SADD: s[a & 7] = s[b & 7] + (u32)imm; break;
      case vop::SCMP: lt = (i32)s[a & 7] < (i32)s[b & 7]; eq = s[a & 7] == s[b & 7]; break;
      case vop::BRLT: if (lt) pc = (u32)((i32)pc + imm); break;
      case vop::BRNE: if (!eq) pc = (u32)((i32)pc + imm); break;
      case vop::BRA: pc = (u32)((i32)pc + imm); break;
      default: fault(cur); return r;
    }
  }
  return r;
}

// ---------------- Fila de jobs / MMIO ----------------
bool Vpu::submit(u32 pc, const u32 sregs[8], bool irq) {
  if ((int)queue.size() + (running ? 1 : 0) >= VPU_QUEUE_DEPTH) return false;
  Job j; j.pc = pc; for (int i = 0; i < 8; i++) j.s[i] = sregs[i]; j.irq = irq;
  queue.push_back(j); done = false;
  if (!running) kick();
  return true;
}

void Vpu::kick() {
  if (queue.empty()) { running = false; return; }
  Job j = queue.front(); queue.pop_front();
  running = true;
  Result r = run_now(j.pc, j.s);
  last_cycles = r.cycles; last_flops = r.flops; total_cycles += r.cycles; total_flops += r.flops;
  dma_bytes += r.dma_bytes; dma_xfers += r.dma_xfers;
  if (r.fault) { faulted = true; fault_pc = r.pc; }
  u64 g = gen; bool irq = j.irq;
  for (auto& sg : r.sigs) {
    u32 n = sg.second;
    bus.sched->at(cyc2ps(sg.first, VPU_HZ), [this, g, n]() { if (g != gen) return; bus.sem[n] += 1; signals++; });
  }
  bus.sched->at(cyc2ps(std::max<u64>(r.cycles, 1), VPU_HZ), [this, g, irq]() {
    if (g != gen) return;       // job cancelado por RESET
    jobs_done++; running = false;
    if (irq) bus.raise_irq(irq_bit);
    if (!queue.empty()) kick(); else done = true;
  });
}

u32 Vpu::read(u32 off) {
  switch (off) {
    case VPU_STATUS: return status();
    case VPU_PC: return next_pc;
    case VPU_CYCLES: return (u32)last_cycles;
    case VPU_FLOPS: return (u32)last_flops;
    case VPU_FAULTPC: return fault_pc;
    case VPU_TOTAL_CYCLES: return (u32)total_cycles;
    case VPU_DMA_BYTES: return (u32)dma_bytes;
    case VPU_JOBS_DONE: return (u32)jobs_done;
  }
  if (off >= VPU_S0 && off < VPU_S0 + 32) return next_s[(off - VPU_S0) / 4];
  return 0;
}
void Vpu::write(u32 off, u32 val) {
  if (off == VPU_PC) { next_pc = val; return; }
  if (off >= VPU_S0 && off < VPU_S0 + 32) { next_s[(off - VPU_S0) / 4] = val; return; }
  if (off == VPU_STATUS) { if (val & VPUS_FAULT) faulted = false; return; }
  if (off == VPU_CTRL) {
    if (val & VPUC_RESET) { queue.clear(); running = false; done = false; faulted = false; gen++; }
    if (val & VPUC_START) { if (!submit(next_pc, next_s, val & VPUC_IRQ)) faulted = true; }
  }
}

}  // namespace sf
