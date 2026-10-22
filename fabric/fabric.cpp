// Host Fabric do SF-100: cria o modelo SystemVerilog do SoC (Verilator), liga cada instância ao hospedeiro e implementa os
// serviços que o backend importa (rtl/fabric_pkg.sv): Memory Host, Time Host, I/O Host, FPU do hospedeiro e observabilidade.
#include "fabric.h"
#include "verilated.h"
#include "Vsf100.h"
#include "Vsf100__Dpi.h"
#include <atomic>

namespace sf {
using namespace abi;

static std::atomic<unsigned> model_seq{0};

Fabric::Fabric(Soc& s) : soc(s), bus(s.bus) {
  // Observabilidade: espelhos dos campos públicos do frontend
  obs[FD_TIMERS] = [this](int k, u64 v) { if (k >= 0 && k < 2) soc.timers.fired[k] = v; };
  obs[FD_DVD] = [this](int, u64 v) { soc.dvd.sectors_read = v; };
  obs[FD_BRIDGE] = [this](int k, u64 v) { if (k == 0) soc.bridge.dma_bytes = v; else soc.bridge.dma_jobs = v; };
  obs[FD_BLT] = [this](int k, u64 v) {
    Blitter& b = soc.blt;
    switch (k) { case 0: b.error = v; break; case 1: b.jobs = v; break; case 2: b.pixels = v; break; case 3: b.sprites_drawn = v; break; default: b.running = v; }
  };
  auto vpu_obs = [](Vpu& p, int k, u64 v) {
    switch (k) { case 0: p.last_cycles = v; break; case 1: p.last_flops = v; break; case 2: p.total_cycles = v; break; case 3: p.total_flops = v; break;
                 case 4: p.jobs_done = v; break; case 5: p.dma_bytes = v; break; case 6: p.dma_xfers = v; break; case 7: p.signals = v; break;
                 case 8: p.fault_pc = (u32)v; break; default: p.running = v; }
  };
  obs[FD_VPUA] = [this, vpu_obs](int k, u64 v) { vpu_obs(soc.vpuA, k, v); };
  obs[FD_VPUB] = [this, vpu_obs](int k, u64 v) { vpu_obs(soc.vpuB, k, v); };
  obs[FD_GPU] = [this](int k, u64 v) {
    FlameCore& g = soc.gpu;
    if (k < 32) {
      FlameCore::Stats& s = k < 16 ? g.last : g.total;
      switch (k & 15) { case 0: s.tris_in = v; break; case 1: s.tris_drawn = v; break; case 2: s.pix_covered = v; break; case 3: s.pix_shaded = v; break;
                        case 4: s.tiles = v; break; case 5: s.cycles = v; break; case 6: s.texfetch = v; break; case 7: s.vram_write = v; break; default: s.bin_entries = v; }
    } else if (k == 32) g.error = v; else if (k == 33) g.lists_done = v; else g.running = v;
  };
  in[FD_GPU] = [this](int) -> u64 { return soc.gpu.hsr; };
  out[FD_GPU] = [this](int, u64 v) { soc.gpu.hsr = v & 1; };
  msg[FD_GPU] = [this](const char* m) { soc.gpu.error_msg = m; };
  obs[FD_COP] = [this](int, u64 v) { soc.cop.ops_executed = v; };
  obs[FD_VIDEO] = [this](int, u64 v) { soc.video.frames = (u32)v; };
  obs[FD_PAULA] = [this](int k, u64 v) {
    Paula& p = soc.paula;
    switch (k) { case 0: p.frames_rendered = v; break; case 1: p.dsp_peak = (u32)v; break; case 2: p.dsp_drops = v; break; case 3: p.master = (u32)v; break;
                 case 4: p.rev_wet = (u32)v; break; case 5: p.seq_running = v; break; default: p.active_voice_samples = v; }
  };
  // Entradas do frontend
  in[FD_MCARD] = [this](int) -> u64 { return soc.mcard.present; };
  in[FD_HID] = [this](int k) -> u64 { return k == 0 ? soc.hid.kbd_present : soc.hid.mouse_present; };
  in[FD_PADS] = [this](int k) -> u64 {
    const PadState& p = soc.pads.pad[(k >> 2) & 3];
    switch (k & 3) { case 0: return p.connected; case 1: return p.buttons;
      case 2: return ((u32)p.lx << 24) | ((u32)p.ly << 16) | ((u32)p.rx << 8) | p.ry; default: return ((u32)p.lt << 8) | p.rt; }
  };
  // Saídas para o frontend
  out[FD_PADS] = [this](int k, u64 v) { soc.pads.pad[k & 3].rumble = (u32)v; };
  out[FD_DBG] = [this](int, u64 v) { DebugConsole& c = soc.console; c.out += (char)v; if (c.on_char) c.on_char((char)v); };
  out[FD_BRIDGE] = [this](int, u64) { if (soc.bridge.on_reboot) soc.bridge.on_reboot(); };
  out[FD_VIDEO] = [this](int k, u64 v) {
    Video& vd = soc.video;
    if (k == 0) { vd.W = (int)(u32)v; vd.H = (int)((v >> 32) & 0xFFFF); vd.mode = (u32)(v >> 48); vd.frame.assign((size_t)vd.W * vd.H, 0); }
    else if (k == 1) { if (vd.on_vblank_hook) vd.on_vblank_hook(); }
    else if (vd.on_frame) vd.on_frame(vd);
  };
  media[0] = &soc.dvd.disc; media[1] = &soc.mcard.data;

  // Modelo SV: um por SoC (cada instância tem o próprio contexto e nomes de escopo próprios)
  std::string name = "SF" + std::to_string(model_seq++);
  ctx.reset(new VerilatedContext);
  top.reset(new Vsf100(ctx.get(), name.c_str()));
  top->eval();
  Verilated::threadContextp(ctx.get());
  auto sc = [&](const char* inst) { void* p = svGetScopeFromName((name + ".sf100." + inst).c_str()); if (!p) throw std::runtime_error(std::string("fabric: escopo SV ausente: ") + inst); return p; };
  scope[FD_CPU] = sc("cpu"); scope[FD_BRIDGE] = sc("bridge"); scope[FD_VPUA] = sc("vpuA"); scope[FD_VPUB] = sc("vpuB"); scope[FD_GPU] = sc("gpu"); scope[FD_BLT] = sc("blt"); scope[FD_COP] = scope[FD_VIDEO] = sc("display");
  scope[FD_TIMERS] = sc("timers"); scope[FD_DVD] = sc("dvd"); scope[FD_MCARD] = sc("mcard"); scope[FD_PADS] = sc("pads");
  scope[FD_PAULA] = sc("paula"); scope[FD_MMU] = sc("mmu"); scope[FD_HID] = sc("hid"); scope[FD_DBG] = sc("dbg");
  enter(FD_CPU); cpu_bind(this, FD_CPU);
  enter(FD_BRIDGE); bridge_bind(this, FD_BRIDGE);
  enter(FD_VPUA); vpu_bind(this, FD_VPUA, VPUA_IMEM, VPUA_DMEM, IRQ_VPUA);
  enter(FD_VPUB); vpu_bind(this, FD_VPUB, VPUB_IMEM, VPUB_DMEM, IRQ_VPUB);
  enter(FD_GPU); gpu_bind(this, FD_GPU);
  enter(FD_BLT); blt_bind(this, FD_BLT);
  enter(FD_VIDEO); display_bind(this, FD_VIDEO, FD_COP);
  enter(FD_PAULA); paula_bind(this, FD_PAULA);
  enter(FD_TIMERS); timers_bind(this, FD_TIMERS);
  enter(FD_DVD); dvd_bind(this, FD_DVD);
  enter(FD_MCARD); mcard_bind(this, FD_MCARD);
  enter(FD_PADS); pads_bind(this, FD_PADS);
  enter(FD_MMU); mmu_bind(this, FD_MMU);
  enter(FD_HID); hid_bind(this, FD_HID);
  enter(FD_DBG); dbg_bind(this, FD_DBG);
}

// O Verilator localiza o contexto pelo ponteiro da thread (o último Fabric criado): o modelo deste SoC precisa ser destruído
// com o próprio contexto corrente.
Fabric::~Fabric() { Verilated::threadContextp(ctx.get()); if (top) top->final(); top.reset(); }

void Fabric::enter(int d) const { svSetScope(scope[d]); }

void Fabric::dispatch(int dev, int tag, u64 a0, u64 a1) {
  events++; enter(dev);
  switch (dev) {
    case FD_BRIDGE: bridge_event(tag, a0, a1); break;
    case FD_VPUA: case FD_VPUB: vpu_event(tag, a0, a1); break;
    case FD_GPU: gpu_event(tag, a0, a1); break;
    case FD_BLT: blt_event(tag, a0, a1); break;
    case FD_VIDEO: video_event(tag, a0, a1); break;
    case FD_TIMERS: timers_event(tag, a0, a1); break;
    case FD_DVD: dvd_event(tag, a0, a1); break;
    case FD_MCARD: mcard_event(tag, a0, a1); break;
    default: throw std::runtime_error("fabric: evento para dispositivo sem backend SV");
  }
}

void fabric_attach(Soc& s) { s.bus.fabric = std::make_shared<Fabric>(s); }
const char* fabric_backend() { return "sv"; }

}  // namespace sf

// ======================= Serviços do hospedeiro (importados pelo backend SV) =======================
using namespace sf;
static inline Fabric& F(void* h) { return *static_cast<Fabric*>(h); }

// ---------------- Memory Host ----------------
svBit fab_ok(void* h, unsigned a, unsigned len) { return F(h).bus.raw(a, len) != nullptr; }
unsigned char fab_r8(void* h, unsigned a) { return F(h).bus.raw_r8(a); }
unsigned short fab_r16(void* h, unsigned a) { return F(h).bus.raw_r16(a); }
unsigned fab_r32(void* h, unsigned a) { return F(h).bus.raw_r32(a); }
void fab_w8(void* h, unsigned a, unsigned char v) { F(h).bus.raw_w8(a, v); }
void fab_w16(void* h, unsigned a, unsigned short v) { F(h).bus.raw_w16(a, v); }
void fab_w32(void* h, unsigned a, unsigned v) { F(h).bus.raw_w32(a, v); }
void fab_copy(void* h, unsigned dst, unsigned src, unsigned len, int mode) {
  Bus& b = F(h).bus; u8* dp = b.raw(dst, len); const u8* sp = b.raw(src, len); if (!dp || !sp || !len) return;
  std::vector<u8> tmp(sp, sp + len);                 // memmove: tolera sobreposição
  if (mode == 1) for (u32 i = 0; i + 3 < len; i += 4) { std::swap(tmp[i], tmp[i + 3]); std::swap(tmp[i + 1], tmp[i + 2]); }
  else if (mode == 2) for (u32 i = 0; i + 1 < len; i += 2) std::swap(tmp[i], tmp[i + 1]);
  std::memcpy(dp, tmp.data(), len);
}
void fab_fill(void* h, unsigned dst, unsigned pattern, unsigned len) {
  u8* dp = F(h).bus.raw(dst, len); if (!dp) return;
  u8 pat[4]; wbe32(pat, pattern); for (u32 i = 0; i < len; i++) dp[i] = pat[i & 3];
}
void fab_cpu_write(void* h, unsigned a, int size, unsigned v) { F(h).bus.cpu_write(a, size, v); }
void fab_cache(void* h, unsigned op, unsigned a, unsigned len) {
  Bus& b = F(h).bus;
  switch (op) {
    case CACHE_FLUSH_ALL: b.cache_flush_all(false); break;
    case CACHE_INVAL_ALL: b.cache_inval_all(); break;
    case CACHE_FLUSH_INVAL_ALL: b.cache_flush_all(true); break;
    case CACHE_FLUSH_RANGE: b.cache_flush_range(a, len, false); break;
    case CACHE_INVAL_RANGE: b.cache_inval_range(a, len); break;
    case CACHE_FLUSH_INVAL_RANGE: b.cache_flush_range(a, len, true); break;
    case ICACHE_INVAL_ALL: b.icache_inval_all(); break;
    case ICACHE_INVAL_RANGE: b.icache_inval_range(a, len); break;
  }
}
void fab_stall(void* h, unsigned long long t) { F(h).bus.cpu_stall_ps += t; }
unsigned long long fab_reserve(void* h, unsigned a, unsigned long long bytes, unsigned long long earliest) { return F(h).bus.reserve(a, bytes, earliest); }
unsigned fab_busreg(void* h, int key) {
  Bus& b = F(h).bus;
  switch (key) {
    case FB_IRQ_STATUS: return b.irq_status; case FB_IRQ_MASK: return b.irq_mask;
    case FB_MMU_ON: return b.mmu_on; case FB_MMU_SBYP: return b.mmu_super_bypass; case FB_MMU_FAR: return b.mmu_far; case FB_MMU_FSR: return b.mmu_fsr;
    case FB_DCACHE_EN: return b.cache_enabled; case FB_ICACHE_EN: return b.icache_enabled; case FB_TLB_RR: return (u32)b.tlb_rr;
    case FB_FAULT: return b.fault; case FB_FAULT_ADDR: return b.fault_addr; case FB_MMU_BYPASS: return b.mmu_bypass;
  }
  return key >= FB_SEM0 && key < FB_SEM0 + 16 ? b.sem[key - FB_SEM0] : 0;
}
void fab_set_busreg(void* h, int key, unsigned v) {
  Bus& b = F(h).bus;
  switch (key) {
    case FB_IRQ_STATUS: b.irq_status = v; return; case FB_IRQ_MASK: b.irq_mask = v; return;
    case FB_MMU_ON: b.mmu_on = v != 0; return; case FB_MMU_SBYP: b.mmu_super_bypass = v != 0; return;
    case FB_MMU_FAR: b.mmu_far = v; return; case FB_MMU_FSR: b.mmu_fsr = v; return;
    case FB_DCACHE_EN: b.cache_enabled = v != 0; return; case FB_ICACHE_EN: b.icache_enabled = v != 0; return; case FB_TLB_RR: b.tlb_rr = (int)v; return;
    case FB_FAULT: b.fault = v != 0; return; case FB_FAULT_ADDR: b.fault_addr = v; return; case FB_MMU_BYPASS: b.mmu_bypass = v != 0; return;
  }
  if (key >= FB_SEM0 && key < FB_SEM0 + 16) b.sem[key - FB_SEM0] = v;
}
void fab_irq(void* h, unsigned bits) { F(h).bus.raise_irq(bits); }
unsigned long long fab_tlb_get(void* h, int i) {
  const TlbEntry& e = std::as_const(F(h).bus.tlb)[i & 63];
  return ((u64)e.v << 63) | ((u64)(e.perm & 15) << 40) | ((u64)(e.ppn & 0xFFFFF) << 20) | (e.vpn & 0xFFFFF);
}
void fab_tlb_set(void* h, int i, unsigned long long e) {
  TlbEntry& t = F(h).bus.tlb[i & 63];                 // acesso de escrita: avança as épocas do caminho rápido
  t.v = (e >> 63) & 1; t.perm = (u32)(e >> 40) & 15; t.ppn = (u32)(e >> 20) & 0xFFFFF; t.vpn = (u32)e & 0xFFFFF;
}

// ---------------- Porta da CPU ----------------
unsigned short fab_fetch16(void* h, unsigned a, svBit super) { Bus& b = F(h).bus; b.cpu_super = super != 0; return b.fetch16(a); }
unsigned fab_cpu_rd(void* h, unsigned a, int size, int super) { Bus& b = F(h).bus; if (super != 2) b.cpu_super = super != 0; return b.cpu_read(a, size); }
void fab_cpu_wr(void* h, unsigned a, int size, unsigned v, int super) { Bus& b = F(h).bus; if (super != 2) b.cpu_super = super != 0; b.cpu_write(a, size, v); }
unsigned fab_cpu_poll(void* h) { Bus& b = F(h).bus; return (b.irq_pending() ? 1u : 0u) | (b.mmu_on ? 2u : 0u) | (b.mmu_super_bypass ? 4u : 0u); }
void fab_cpu_state(void* h, unsigned d0, unsigned d1, unsigned d2, unsigned d3, unsigned d4, unsigned d5, unsigned d6, unsigned d7,
                   unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned a4, unsigned a5, unsigned a6, unsigned a7, unsigned pc, unsigned sr, unsigned vbr,
                   double f0, double f1, double f2, double f3, double f4, double f5, double f6, double f7, svBit fn, svBit fz, svBit fnan, unsigned fpcr, unsigned fpiar,
                   long long c0, long long c1, long long c2, long long c3, unsigned macsr, unsigned mask, unsigned cacr, unsigned osp, svBit halted, svBit stopped,
                   int reason, int exc_vec, unsigned exc_pc, unsigned exc_addr, unsigned long long cycles, unsigned long long instrs) {
  Cpu& c = F(h).soc.cpu;
  const u32 dd[8] = {d0, d1, d2, d3, d4, d5, d6, d7}, aa[8] = {a0, a1, a2, a3, a4, a5, a6, a7}; const double ff[8] = {f0, f1, f2, f3, f4, f5, f6, f7};
  const i64 cc[4] = {c0, c1, c2, c3};
  for (int i = 0; i < 8; i++) { c.d[i] = dd[i]; c.a[i] = aa[i]; c.fp[i] = ff[i]; }
  for (int i = 0; i < 4; i++) c.acc[i] = cc[i];
  c.pc = pc; c.sr = sr; c.vbr = vbr; c.fp_n = fn; c.fp_z = fz; c.fp_nan = fnan; c.fpcr = fpcr; c.fpiar = fpiar;
  c.macsr = macsr; c.mask = mask; c.cacr = cacr; c.other_sp = osp; c.halted = halted; c.stopped = stopped; c.reason = (Cpu::Reason)reason;
  c.exc_vec = exc_vec; c.exc_pc = exc_pc; c.exc_addr = exc_addr; c.cycles = cycles; c.instrs = instrs;
  F(h).cpu_pull_done();
}

// ---------------- Time Host ----------------
unsigned long long fab_now(void* h) { return F(h).bus.sched->now; }
void fab_at(void* h, int dev, unsigned long long delay, int tag, unsigned long long a0, unsigned long long a1) {
  Fabric* f = &F(h);
  f->bus.sched->at(delay, [f, dev, tag, a0, a1]() { f->dispatch(dev, tag, a0, a1); });
}

// ---------------- I/O Host ----------------
unsigned long long fab_media_size(void* h, int m) { return F(h).media[m & 1]->size(); }
void fab_media_read(void* h, int m, unsigned long long off, unsigned dst, unsigned len) {
  std::vector<u8>& md = *F(h).media[m & 1]; u8* dp = F(h).bus.raw(dst, len);
  if (dp && off + len <= md.size()) std::memcpy(dp, md.data() + off, len);
}
void fab_media_and(void* h, int m, unsigned long long off, unsigned src, unsigned len) {
  std::vector<u8>& md = *F(h).media[m & 1]; const u8* sp = F(h).bus.raw(src, len);
  if (sp && off + len <= md.size()) for (u32 i = 0; i < len; i++) md[off + i] &= sp[i];
}
void fab_media_fill(void* h, int m, unsigned long long off, unsigned char v, unsigned len) {
  std::vector<u8>& md = *F(h).media[m & 1]; if (off + len <= md.size()) std::memset(md.data() + off, v, len);
}
unsigned long long fab_in(void* h, int dev, int key) { auto& fn = F(h).in[dev]; return fn ? fn(key) : 0; }
void fab_out(void* h, int dev, int key, unsigned long long v) { auto& fn = F(h).out[dev]; if (fn) fn(key, v); }
void fab_msg(void* h, int dev, const char* m) { auto& fn = F(h).msg[dev]; if (fn) fn(m ? m : ""); }
void fab_obs(void* h, int dev, int key, unsigned long long v) { auto& fn = F(h).obs[dev]; if (fn) fn(key, v); }
void fab_frame_w(void* h, unsigned idx, unsigned rgb) { std::vector<u32>& fr = F(h).soc.video.frame; if (idx < fr.size()) fr[idx] = rgb; }
void fab_pcm(void* h, short l, short r) { std::vector<i16>& p = F(h).soc.paula.pcm; p.push_back(l); p.push_back(r); }

// ---------------- FPU do hospedeiro (FP32) ----------------
static inline float fu(unsigned a) { return u2f(a); }
static inline unsigned uf(float f) { return f2u(f); }
unsigned f32_add(unsigned a, unsigned b) { return uf(fu(a) + fu(b)); }
unsigned f32_sub(unsigned a, unsigned b) { return uf(fu(a) - fu(b)); }
unsigned f32_mul(unsigned a, unsigned b) { return uf(fu(a) * fu(b)); }
unsigned f32_div(unsigned a, unsigned b) { return uf(fu(a) / fu(b)); }
unsigned f32_sqrt(unsigned a) { return uf(std::sqrt(fu(a))); }
unsigned f32_floor(unsigned a) { return uf(std::floor(fu(a))); }
unsigned f32_abs(unsigned a) { return uf(std::fabs(fu(a))); }
unsigned f32_cos(unsigned a) { return uf(std::cos(fu(a))); }
unsigned f32_sin(unsigned a) { return uf(std::sin(fu(a))); }
unsigned f32_log2(unsigned a) { return uf(std::log2(fu(a))); }
svBit f32_lt(unsigned a, unsigned b) { return fu(a) < fu(b); }
svBit f32_le(unsigned a, unsigned b) { return fu(a) <= fu(b); }
svBit f32_eq(unsigned a, unsigned b) { return fu(a) == fu(b); }
svBit f32_finite(unsigned a) { return std::isfinite(fu(a)); }
double f64_op(int op, double a) {
  switch (op) { case 0: return std::trunc(a); case 1: return std::floor(a); case 2: return std::ceil(a); case 3: return std::nearbyint(a);
                case 4: return std::sqrt(a); default: return std::fabs(a); }
}
unsigned f32_from_i32(int a) { return uf((float)a); }
int f32_to_i32(unsigned a) { return (int)fu(a); }
int f32_lround(unsigned a) { return (int)std::lround(fu(a)); }
double f32_to_f64(unsigned a) { return (double)fu(a); }
unsigned f64_to_f32(double a) { return uf((float)a); }
unsigned f32_clamp_i16(unsigned a) { return uf(std::max(-32768.0f, std::min(32767.0f, fu(a)))); }
short f32_to_i16(unsigned a) { return (i16)fu(a); }
