// Periféricos do SF-100: registradores da MMU, timers, DVD-ROM, cartão de memória e controles.
#pragma once
#include "bus.h"

namespace sf {

class MmuDev : public Device {
 public:
  explicit MmuDev(Bus& b) : bus(b) {}
  u32 read(u32 off) override;
  void write(u32 off, u32 v) override;
 private:
  Bus& bus; u32 ar = 0, tr = 0;
};

class Timers : public Device {
 public:
  explicit Timers(Bus& b) : bus(b) {}
  u32 read(u32 off) override;
  void write(u32 off, u32 v) override;
  u64 fired[2] = {0, 0};
 private:
  struct T { u32 load = 0, ctrl = 0; Ps start = 0; u64 gen = 0; } t[2];
  Bus& bus;
  void arm(int i);
};

// Leitor de DVD-ROM: setores de 2048 B, latências de rotação/seek/transferência, DMA para a DDR.
class Dvd : public Device {
 public:
  explicit Dvd(Bus& b) : bus(b) {}
  u32 read(u32 off) override;
  void write(u32 off, u32 v) override;
  void insert(std::vector<u8> image) { disc = std::move(image); disc.resize((disc.size() + abi::DVD_SECTOR - 1) / abi::DVD_SECTOR * abi::DVD_SECTOR); spinning = false; head = 0; }
  void eject() { disc.clear(); }
  std::vector<u8> disc; u64 sectors_read = 0;
  static constexpr double RATE_MBPS = 5.54;       // ~4x DVD
 private:
  Bus& bus; u32 lba = 0, count = 0, dst = 0, status = 0, ctrl = 0, head = 0; bool spinning = false; Ps last_io = 0; u64 gen = 0;
  void start();
};

class MemCard : public Device {
 public:
  explicit MemCard(Bus& b) : data(1u << 20, 0xFF), bus(b) {}
  u32 read(u32 off) override;
  void write(u32 off, u32 v) override;
  std::vector<u8> data; bool present = true;
 private:
  Bus& bus; u32 sector = 0, count = 0, addr = 0, status = 0, ctrl = 0;
  void start(u32 cmd);
};

struct PadState { bool connected = false; u32 buttons = 0; u8 lx = 128, ly = 128, rx = 128, ry = 128, lt = 0, rt = 0; u32 rumble = 0; };
class Pads : public Device {
 public:
  explicit Pads(Bus& b) : bus(b) {}
  u32 read(u32 off) override;
  void write(u32 off, u32 v) override;
  PadState pad[4];                  // estado "físico" (o host altera)
  void latch();                     // chamado a cada vblank: copia estado físico para os registradores
 private:
  Bus& bus; PadState lat[4]; u32 ctrl = 0;
};

// Teclado + mouse. O host injeta eventos (key_event/mouse_*); o software lê pela FIFO/registradores.
class Hid : public Device {
 public:
  explicit Hid(Bus& b) : bus(b) {}
  u32 read(u32 off) override;
  void write(u32 off, u32 v) override;
  bool kbd_present = true, mouse_present = true;
  void key_event(u8 usage, bool down);
  void mouse_move(int dx, int dy);
  void mouse_button(int bit, bool down);        // bit 0 esq, 1 dir, 2 meio
  void mouse_wheel(int d);
 private:
  Bus& bus; u32 ctrl = 0, status_ovf = 0, mods = 0, btn = 0; std::deque<u32> fifo; i32 dx = 0, dy = 0, wheel = 0;
  void note() { if (ctrl & 1) bus.raise_irq(abi::IRQ_HID); }
};

// Console de depuração: caracteres escritos em DBG_PUTC vão para 'out' (e para on_char, se definido).
class DebugConsole : public Device {
 public:
  std::string out; std::function<void(char)> on_char;
  u32 read(u32 off) override { return off == abi::DBG_STATUS ? 1u : 0u; }
  void write(u32 off, u32 v) override { if (off == abi::DBG_PUTC) { out += (char)v; if (on_char) on_char((char)v); } }
};

}  // namespace sf
