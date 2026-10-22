#include "periph.h"

namespace sf {
using namespace abi;

// ---------------- MMU ----------------
u32 MmuDev::read(u32 off) {
  switch (off) {
    case MMU_CTRL: return (u32)bus.mmu_on | ((u32)bus.mmu_super_bypass << 1); case MMU_FAR: return bus.mmu_far; case MMU_FSR: return bus.mmu_fsr;
    case MMU_AR: return ar; case MMU_TR: return tr;
  }
  return 0;
}
void MmuDev::write(u32 off, u32 v) {
  switch (off) {
    case MMU_CTRL: bus.mmu_on = v & 1; bus.mmu_super_bypass = v & 2; break;
    case MMU_FSR: bus.mmu_fsr = 0; break;
    case MMU_AR: ar = v; break;
    case MMU_TR: tr = v; break;
    case MMU_OP: {
      u32 vpn = ar >> 12;
      if (v == 1) {
        for (auto& e : bus.tlb) if (e.v && e.vpn == vpn) e.v = false;     // sem duplicatas
        auto& e = bus.tlb[bus.tlb_rr++ & 63]; e.v = true; e.vpn = vpn; e.ppn = tr >> 12; e.perm = tr & 15;
      } else if (v == 2) { for (auto& e : bus.tlb) e.v = false; }
      else if (v == 3) { for (auto& e : bus.tlb) if (e.v && e.vpn == vpn) e.v = false; }
      else if (v == 4) {
        tr = 0; for (auto& e : bus.tlb) if (e.v && e.vpn == vpn) { tr = 0x80000000u | (e.ppn << 12) | e.perm; break; }
      }
      break;
    }
  }
}

// ---------------- Timers (1 MHz) ----------------
u32 Timers::read(u32 off) {
  int i = off / 0x10; if (i > 1) return 0; T& x = t[i];
  switch (off % 0x10) {
    case TMR_LOAD: return x.load; case TMR_CTRL: return x.ctrl;
    case TMR_COUNT: {
      if (!(x.ctrl & 1) || !x.load) return 0;
      u64 el = (bus.sched->now - x.start) / 1000000ull;
      if (x.ctrl & 2) return (u32)(x.load - el % x.load);
      return el >= x.load ? 0 : (u32)(x.load - el);
    }
  }
  return 0;
}
void Timers::write(u32 off, u32 v) {
  int i = off / 0x10; if (i > 1) return; T& x = t[i];
  switch (off % 0x10) {
    case TMR_LOAD: x.load = v; break;
    case TMR_CTRL: x.ctrl = v & 7; x.gen++; if ((v & 1) && x.load) { x.start = bus.sched->now; arm(i); } break;
  }
}
void Timers::arm(int i) {
  T& x = t[i]; u64 g = x.gen;
  bus.sched->at((Ps)x.load * 1000000ull, [this, i, g]() {
    T& y = t[i]; if (g != y.gen) return;
    fired[i]++; if (y.ctrl & 4) bus.raise_irq(IRQ_TIMER);
    if (y.ctrl & 2) arm(i); else y.ctrl &= ~1u;
  });
}

// ---------------- DVD ----------------
void Dvd::insert(std::vector<u8> image) { disc = std::move(image); disc.resize((disc.size() + DVD_SECTOR - 1) / DVD_SECTOR * DVD_SECTOR); spinning = false; head = 0; }
u32 Dvd::read(u32 off) {
  switch (off) {
    case DVD_LBA: return lba; case DVD_COUNT: return count; case DVD_DST: return dst;
    case DVD_STATUS: return status | (disc.empty() ? 0 : 8); case DVD_CTRL: return ctrl;
    case DVD_CAPACITY: return (u32)(disc.size() / DVD_SECTOR);
  }
  return 0;
}
void Dvd::write(u32 off, u32 v) {
  switch (off) {
    case DVD_LBA: lba = v; break; case DVD_COUNT: count = v; break; case DVD_DST: dst = v; break; case DVD_CTRL: ctrl = v; break;
    case DVD_STATUS: status &= ~(v & 6); break;                       // W1C de done/erro
    case DVD_CMD: if (v == 1) start(); break;
  }
}
void Dvd::start() {
  if (status & 1) return;                                           // ocupado: comando ignorado
  u32 cap = (u32)(disc.size() / DVD_SECTOR);
  if (disc.empty() || count == 0 || (u64)lba + count > cap || !bus.raw_ok(dst, count * DVD_SECTOR) || dst >= DDR_SIZE) { status |= 4; if (ctrl & 1) bus.raise_irq(IRQ_DVD); return; }
  status = 1;
  Ps now = bus.sched->now, t = 0;
  if (!spinning || now - last_io > 3000000000000ull) t += 800000000000ull;                // spin-up (800 ms) se parado
  u32 dist = lba > head ? lba - head : head - lba;
  if (dist > 64) t += 20000000000ull + (Ps)(120000000000.0 * dist / std::max<u32>(cap, 1));   // seek: 20 ms + até 120 ms
  Ps xfer = (Ps)((double)count * DVD_SECTOR / (RATE_MBPS * 1e6) * 1e12);
  Ps xstart = t;                                                   // a transferência só disputa a DDR depois do spin-up/seek
  u64 g = ++gen; u32 l = lba, n = count, d = dst;
  bus.sched->at(xstart, [this, g, l, n, d, xfer]() {
    if (g != gen) return;
    Ps fin = std::max(bus.sched->now + xfer, bus.reserve(d, (u64)n * DVD_SECTOR, bus.sched->now));
    bus.sched->at(fin - bus.sched->now, [this, g, l, n, d]() {
      if (g != gen) return;
      if (!disc.empty()) { std::memcpy(bus.raw(d, n * DVD_SECTOR), &disc[(size_t)l * DVD_SECTOR], (size_t)n * DVD_SECTOR); bus.cache_inval_range(d, n * DVD_SECTOR); }
      sectors_read += n; head = l + n; spinning = true; last_io = bus.sched->now;
      status = 2; if (ctrl & 1) bus.raise_irq(IRQ_DVD);
    });
  });
}

// ---------------- Cartão de memória ----------------
u32 MemCard::read(u32 off) {
  switch (off) {
    case MC_SECTOR: return sector; case MC_COUNT: return count; case MC_ADDR: return addr;
    case MC_STATUS: return status | (present ? 8 : 0); case MC_CTRL: return ctrl; case MC_CAPACITY: return (u32)(data.size() / MC_SECTOR_SIZE);
  }
  return 0;
}
void MemCard::write(u32 off, u32 v) {
  switch (off) {
    case MC_SECTOR: sector = v; break; case MC_COUNT: count = v; break; case MC_ADDR: addr = v; break; case MC_CTRL: ctrl = v; break;
    case MC_STATUS: status &= ~(v & 6); break;
    case MC_CMD: start(v); break;
  }
}
void MemCard::start(u32 cmd) {
  if (status & 1) return;
  u32 cap = (u32)(data.size() / MC_SECTOR_SIZE); u32 n = cmd == 3 ? MC_ERASE_SECTORS : count;
  bool bad = !present || cmd < 1 || cmd > 3 || n == 0 || (u64)sector + n > cap || (cmd == 3 && sector % MC_ERASE_SECTORS) ||
             (cmd != 3 && (addr >= DDR_SIZE || !bus.raw_ok(addr, n * MC_SECTOR_SIZE)));
  if (bad) { status |= 4; if (ctrl & 1) bus.raise_irq(IRQ_MCARD); return; }
  status = 1;
  Ps per = cmd == 1 ? 100000000ull : cmd == 2 ? 400000000ull : 2000000000ull;          // ps por setor/bloco: 100 us / 400 us / 2 ms
  Ps dur = per * (cmd == 3 ? 1 : n);
  Ps fin = bus.sched->now + dur;
  if (cmd != 3) fin = std::max(fin, bus.reserve(addr, (u64)n * MC_SECTOR_SIZE, bus.sched->now));
  u32 s = sector, a = addr;
  if (cmd == 2) bus.cache_flush_range(a, n * MC_SECTOR_SIZE, false);
  bus.sched->at(fin - bus.sched->now, [this, cmd, s, n, a]() {
    if (cmd == 1) { std::memcpy(bus.raw(a, n * MC_SECTOR_SIZE), &data[(size_t)s * MC_SECTOR_SIZE], (size_t)n * MC_SECTOR_SIZE); bus.cache_inval_range(a, n * MC_SECTOR_SIZE); }
    else if (cmd == 2) { const u8* src = bus.raw(a, n * MC_SECTOR_SIZE); for (size_t i = 0; i < (size_t)n * MC_SECTOR_SIZE; i++) data[(size_t)s * MC_SECTOR_SIZE + i] &= src[i]; }   // flash: só 1 -> 0
    else std::memset(&data[(size_t)s * MC_SECTOR_SIZE], 0xFF, (size_t)MC_ERASE_SECTORS * MC_SECTOR_SIZE);
    status = 2; if (ctrl & 1) bus.raise_irq(IRQ_MCARD);
  });
}

// ---------------- Controles ----------------
void Pads::latch() { for (int i = 0; i < 4; i++) { lat[i] = pad[i]; } if (ctrl & 1) bus.raise_irq(IRQ_PAD); }
u32 Pads::read(u32 off) {
  if (off == PAD_CTRL) return ctrl;
  if (off >= PAD_PORT0 && off < PAD_PORT0 + 0x40) {
    const PadState& p = lat[(off - PAD_PORT0) / 0x10];
    switch ((off - PAD_PORT0) % 0x10) {
      case 0: return p.connected ? p.buttons : 0;
      case 4: return ((u32)p.lx << 24) | ((u32)p.ly << 16) | ((u32)p.rx << 8) | p.ry;
      case 8: return ((u32)p.lt << 8) | p.rt;
      default: return p.connected ? 1 : 0;
    }
  }
  return 0;
}
void Pads::write(u32 off, u32 v) {
  if (off == PAD_CTRL) { ctrl = v; return; }
  if (off >= PAD_PORT0 && off < PAD_PORT0 + 0x40 && (off - PAD_PORT0) % 0x10 == 8) pad[(off - PAD_PORT0) / 0x10].rumble = v;
}

// ---------------- Teclado e mouse ----------------
void Hid::key_event(u8 usage, bool down) {
  if (!kbd_present) return;
  if (usage >= 0xE0 && usage <= 0xE7) {
    u32 m = (usage == 0xE0 || usage == 0xE4) ? abi::HIDM_CTRL : (usage == 0xE1 || usage == 0xE5) ? abi::HIDM_SHIFT : (usage == 0xE2 || usage == 0xE6) ? abi::HIDM_ALT : abi::HIDM_META;
    if (down) mods |= m; else mods &= ~m;      // simplificação: soltar um dos pares limpa o modificador
  }
  if (fifo.size() >= abi::HID_FIFO_DEPTH) { status_ovf = 1; return; }
  fifo.push_back(usage | (down ? 0u : abi::HID_KEY_UP) | (mods << 16)); note();
}
void Hid::mouse_move(int x, int y) { if (!mouse_present) return; dx += x; dy += y; note(); }
void Hid::mouse_button(int bit, bool down) { if (!mouse_present) return; if (down) btn |= 1u << bit; else btn &= ~(1u << bit); note(); }
void Hid::mouse_wheel(int d) { if (!mouse_present) return; wheel += d; note(); }
u32 Hid::read(u32 off) {
  using namespace abi;
  switch (off) {
    case HID_CTRL: return ctrl;
    case HID_KEY_COUNT: return (u32)fifo.size();
    case HID_KEY_DATA: { if (fifo.empty()) return 0xFFFFFFFFu; u32 v = fifo.front(); fifo.pop_front(); return v; }
    case HID_MOUSE_DX: { u32 v = (u32)dx; dx = 0; return v; }
    case HID_MOUSE_DY: { u32 v = (u32)dy; dy = 0; return v; }
    case HID_MOUSE_BTN: return btn;
    case HID_MOUSE_WHEEL: { u32 v = (u32)wheel; wheel = 0; return v; }
    case HID_STATUS: return (kbd_present ? 1u : 0u) | (mouse_present ? 2u : 0u) | (status_ovf << 2);
    default: return 0;
  }
}
void Hid::write(u32 off, u32 v) {
  if (off == abi::HID_CTRL) ctrl = v;
  else if (off == abi::HID_STATUS && (v & 4)) status_ovf = 0;
}

// ---------------- Console de depuração ----------------
u32 DebugConsole::read(u32 off) { return off == DBG_STATUS ? 1u : 0u; }
void DebugConsole::write(u32 off, u32 v) { if (off == DBG_PUTC) { out += (char)v; if (on_char) on_char((char)v); } }

}  // namespace sf
