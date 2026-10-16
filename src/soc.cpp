#include "soc.h"

namespace sf {
using namespace abi;

Soc::Soc() : bridge(bus), cpu(bus), vpuA(bus, IRQ_VPUA), vpuB(bus, IRQ_VPUB), gpu(bus), blt(bus), cop(bus), paula(bus), video(bus, cop), mmu(bus), timers(bus), dvd(bus), mcard(bus), pads(bus), hid(bus) {
  bus.sched = &sched;
  bus.map_device(BRIDGE_BASE, 0x1000, &bridge);
  bus.map_device(VPUA_BASE, 0x1000, &vpuA);   bus.map_device(VPUB_BASE, 0x1000, &vpuB);
  bus.map_device(GPU_BASE, 0x1000, &gpu);     bus.map_device(BLT_BASE, 0x1000, &blt);
  bus.map_device(COP_BASE, 0x1000, &cop);     bus.map_device(PAULA_BASE, 0x2000, &paula);
  bus.map_device(VIDEO_BASE, 0x1000, &video);
  bus.add_window(VPUA_IMEM, VPU_IMEM_SIZE, vpuA.imem); bus.add_window(VPUA_DMEM, VPU_DMEM_SIZE, vpuA.dmem);
  bus.add_window(VPUB_IMEM, VPU_IMEM_SIZE, vpuB.imem); bus.add_window(VPUB_DMEM, VPU_DMEM_SIZE, vpuB.dmem);
  bus.map_device(MMU_BASE, 0x1000, &mmu);     bus.map_device(TIMER_BASE, 0x1000, &timers);
  bus.map_device(DVD_BASE, 0x1000, &dvd);     bus.map_device(MCARD_BASE, 0x1000, &mcard);
  bus.map_device(PAD_BASE, 0x1000, &pads);
  bus.map_device(HID_BASE, 0x1000, &hid);
  bus.map_device(DBG_BASE, 0x1000, &console);
  video.on_vblank_hook = [this]() { paula.render_until(sched.now); pads.latch(); };
}

bool Soc::step_one() {
  if (cpu.halted && sched.empty()) return false;
  if (cpu.idle()) { Ps nt = sched.next_time(); if (nt == ~0ull) return false; sched.advance_to(nt); return true; }
  u32 pc0 = cpu.pc; int c = cpu.step();
  if (c) sched.advance_to(sched.now + (Ps)c * 1000000ull / 266 + take_stall());
  if (on_step) on_step(pc0, c);
  return true;
}

Ps Soc::take_stall() { Ps s = bus.cpu_stall_ps; bus.cpu_stall_ps = 0; return s; }

Ps Soc::run(Ps limit) {
  Ps start = sched.now, end = start + limit;
  while (sched.now < end) {
    if (cpu.idle()) {
      Ps nt = sched.next_time();
      if (nt == ~0ull) break;                       // nada mais a acontecer
      sched.advance_to(std::min(nt, end));
      if (cpu.halted) { if (sched.empty() || sched.next_time() > end) break; }
      continue;
    }
    u32 pc0 = cpu.pc; int c = cpu.step();
    if (on_step) on_step(pc0, c);
    if (c == 0) continue;                            // virou idle neste passo
    sched.advance_to(sched.now + (Ps)c * 1000000ull / 266 + take_stall());
  }
  if (sched.now < end && !cpu.halted && !cpu.stopped) sched.advance_to(end);
  return sched.now - start;
}

bool Soc::run_until_halt(Ps limit) {
  Ps end = sched.now + limit;
  while (!cpu.halted && sched.now < end) {
    if (cpu.stopped) { Ps nt = sched.next_time(); if (nt == ~0ull || nt > end) break; sched.advance_to(nt); }
    u32 pc0 = cpu.pc; int c = cpu.step();
    if (on_step) on_step(pc0, c);
    if (c) sched.advance_to(sched.now + (Ps)c * 1000000ull / 266 + take_stall());
  }
  return cpu.halted && cpu.reason == Cpu::HALT_INSN;
}

void Soc::run_frames(int n) {
  video.start();
  u32 target = video.frames + n;
  Ps guard = sched.now + (Ps)(n + 2) * FRAME_PS;
  while (video.frames < target && sched.now < guard) {
    Ps nt = std::min(sched.next_time(), guard);
    if (!cpu.idle()) run(std::min<Ps>(nt - sched.now + 1, 1000000ull)); else sched.advance_to(nt);
  }
}

bool Soc::wait_until(const std::function<bool()>& pred, Ps limit) {
  Ps end = sched.now + limit;
  while (!pred()) {
    Ps nt = sched.next_time(); if (nt == ~0ull || nt > end) return false;
    sched.advance_to(nt);
  }
  return true;
}

}  // namespace sf
