// Frontend dos dispositivos realizados em SystemVerilog: a API C++ de src/ (bridge, periféricos, Blitter, Copper, vídeo)
// passa a ser um invólucro que entra no escopo da instância SV e chama a exportação correspondente. Estado e regras do
// hardware ficam no backend (rtl/); aqui só há a ponte e os espelhos que o frontend lê.
#include "fabric.h"
#include "Vsf100__Dpi.h"

namespace sf {
using namespace abi;

#define SV(dev) fabric_of(bus).enter(dev)

// ---------------- SnowBridge ----------------
u32 Bridge::read(u32 off) { SV(FD_BRIDGE); return bridge_read(off); }
void Bridge::write(u32 off, u32 v) { SV(FD_BRIDGE); bridge_write(off, v); }

// ---------------- Periféricos ----------------
u32 MmuDev::read(u32 off) { SV(FD_MMU); return mmu_read(off); }
void MmuDev::write(u32 off, u32 v) { SV(FD_MMU); mmu_write(off, v); }
u32 Timers::read(u32 off) { SV(FD_TIMERS); return timers_read(off); }
void Timers::write(u32 off, u32 v) { SV(FD_TIMERS); timers_write(off, v); }
u32 Dvd::read(u32 off) { SV(FD_DVD); return dvd_read(off); }
void Dvd::write(u32 off, u32 v) { SV(FD_DVD); dvd_write(off, v); }
void Dvd::insert(std::vector<u8> image) {
  disc = std::move(image); disc.resize((disc.size() + DVD_SECTOR - 1) / DVD_SECTOR * DVD_SECTOR);   // a mídia é do hospedeiro
  SV(FD_DVD); dvd_insert();
}
u32 MemCard::read(u32 off) { SV(FD_MCARD); return mcard_read(off); }
void MemCard::write(u32 off, u32 v) { SV(FD_MCARD); mcard_write(off, v); }
void Pads::latch() { SV(FD_PADS); pads_latch(); }
u32 Pads::read(u32 off) { SV(FD_PADS); return pads_read(off); }
void Pads::write(u32 off, u32 v) { SV(FD_PADS); pads_write(off, v); }
void Hid::key_event(u8 usage, bool down) { SV(FD_HID); hid_key_event(usage, down); }
void Hid::mouse_move(int x, int y) { SV(FD_HID); hid_mouse_move(x, y); }
void Hid::mouse_button(int b, bool down) { SV(FD_HID); hid_mouse_button(b, down); }
void Hid::mouse_wheel(int d) { SV(FD_HID); hid_mouse_wheel(d); }
u32 Hid::read(u32 off) { SV(FD_HID); return hid_read(off); }
void Hid::write(u32 off, u32 v) { SV(FD_HID); hid_write(off, v); }
u32 DebugConsole::read(u32 off) { SV(FD_DBG); return dbg_read(off); }
void DebugConsole::write(u32 off, u32 v) { SV(FD_DBG); dbg_write(off, v); }

// ---------------- SnowVector (VPU A/B) ----------------
// IMEM/DMEM são memória do hospedeiro (janelas do mapa): o carregador e o RESET mexem nelas daqui.
#define SVV() fabric_of(bus).enter(irq_bit == IRQ_VPUA ? FD_VPUA : FD_VPUB)
Vpu::Vpu(Bus& b, u32 bit) : bus(b), irq_bit(bit) {
  std::memset(imem, 0, sizeof imem); std::memset(dmem, 0, sizeof dmem); std::memset(v, 0, sizeof v); std::memset(s, 0, sizeof s);
}
void Vpu::reset_state() {
  std::memset(imem, 0, sizeof imem); std::memset(dmem, 0, sizeof dmem); std::memset(v, 0, sizeof v); std::memset(s, 0, sizeof s);
  SVV(); vpu_reset();
}
void Vpu::load_program(const std::vector<u32>& code, u32 at) {
  if ((at + code.size()) * 4 > VPU_IMEM_SIZE) throw std::runtime_error("programa VPU excede 8 KiB");
  for (size_t i = 0; i < code.size(); i++) wbe32(imem + (at + i) * 4, code[i]);
}
bool Vpu::submit(u32 pc, const u32 r[8], bool irq) { SVV(); return vpu_submit(pc, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], irq); }
u32 Vpu::status() const { SVV(); return vpu_status(); }
Vpu::Result Vpu::run_now(u32 pc, const u32 r[8]) {
  SVV(); vpu_run_now(pc, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
  Result res; res.cycles = vpu_result(0); res.flops = vpu_result(1); res.dma_bytes = vpu_result(2); res.dma_xfers = vpu_result(3);
  res.fault = vpu_result(4) != 0; res.pc = (u32)vpu_result(5);
  for (int i = 0, n = (int)vpu_result(6); i < n; i++) res.sigs.push_back({vpu_result(16 + 2 * i), (u32)vpu_result(17 + 2 * i)});
  for (int i = 0; i < 32; i++) for (int l = 0; l < 4; l++) v[i][l] = u2f(vpu_vreg(i, l));   // espelho dos registradores
  for (int i = 0; i < 8; i++) s[i] = vpu_sreg(i);
  return res;
}
u32 Vpu::read(u32 off) { SVV(); return vpu_read(off); }
void Vpu::write(u32 off, u32 val) { SVV(); vpu_write(off, val); }

// ---------------- FlameCore ----------------
FlameCore::FlameCore(Bus& b) : bus(b) {}
FlameCore::~FlameCore() {}
bool FlameCore::submit(u32 addr, u32 n, bool irq) { SV(FD_GPU); return gpu_submit(addr, n, irq); }
FlameCore::Stats FlameCore::execute(u32 addr, u32 n) { SV(FD_GPU); gpu_execute(addr, n); return last; }   // 'last' espelhado pelo backend
u32 FlameCore::read(u32 off) { SV(FD_GPU); return gpu_read(off); }
void FlameCore::write(u32 off, u32 v) { SV(FD_GPU); gpu_write(off, v); }

// ---------------- SnowBlitter ----------------
u32 Blitter::read(u32 off) { SV(FD_BLT); return blt_read(off); }
void Blitter::write(u32 off, u32 v) { SV(FD_BLT); blt_write(off, v); }

// ---------------- SnowCopper ----------------
u32 Copper::read(u32 off) { SV(FD_COP); return cop_read(off); }
void Copper::write(u32 off, u32 v) {
  SV(FD_COP); cop_write(off, v);
  list = cop_read(COP_LIST); enabled = cop_read(COP_CTRL) & 1;   // espelhos públicos
}

// ---------------- SnowPaula ----------------
Paula::Paula(Bus& b) : bus(b) {}
u32 Paula::read(u32 off) { SV(FD_PAULA); return paula_read(off); }
void Paula::write(u32 off, u32 v) { SV(FD_PAULA); paula_write(off, v); }
void Paula::render_until(Ps t) { SV(FD_PAULA); paula_render_until(t); }
void Paula::render(int frames) { SV(FD_PAULA); paula_render(frames); }

// ---------------- Vídeo ----------------
// Chamado pelo construtor (antes do fabric existir) e pelo aviso de troca de modo do backend: só ajusta o quadro do frontend.
void Video::set_mode(u32 m) {
  mode = m;
  switch (m) { case VMODE_240P: W = 320; H = 240; break; case VMODE_480I: case VMODE_480P: W = 640; H = 480; break; default: W = 1280; H = 720; break; }
  frame.assign((size_t)W * H, 0);
}
u32 Video::read(u32 off) { SV(FD_VIDEO); return video_read(off); }
void Video::write(u32 off, u32 v) {
  SV(FD_VIDEO); video_write(off, v);
  // espelhos públicos (camadas, paleta, fundo, controle, ordem) relidos do backend
  auto rd = [](u32 o) { return video_read(o); };
  if (off >= VID_PALETTE && off < VID_PALETTE + 0x400) { palette[(off - VID_PALETTE) / 4] = rd(off); return; }
  if (off >= VID_LAYER0 && off < VID_LAYER0 + VID_LAYERS * VID_LAYER_STRIDE) {
    Layer& l = layer[(off - VID_LAYER0) / VID_LAYER_STRIDE]; u32 b = off - (off - VID_LAYER0) % VID_LAYER_STRIDE;
    l.addr = rd(b + LR_ADDR); l.pitch = rd(b + LR_PITCH); l.scx = (i32)rd(b + LR_SCX); l.scy = (i32)rd(b + LR_SCY); l.alpha = rd(b + LR_ALPHA);
    l.winx = rd(b + LR_WINX); l.winy = rd(b + LR_WINY); l.flags = rd(b + LR_FLAGS); l.size = rd(b + LR_SIZE);
    return;
  }
  bg = rd(VID_BG); ctrl = rd(VID_CTRL); order = rd(VID_ORDER);
  layer[0].addr = rd(VID_P0_ADDR); layer[0].pitch = rd(VID_P0_PITCH); layer[1].addr = rd(VID_P1_ADDR); layer[1].pitch = rd(VID_P1_PITCH);
}
void Video::start() { SV(FD_VIDEO); video_start(); started = true; }
void Video::vblank() { SV(FD_VIDEO); video_vblank(); }

}  // namespace sf
