// Chipset 2D de tradição Amiga: SnowBlitter (DMA 2D + minterms), SnowCopper (lista sincronizada ao vídeo)
// e controlador de vídeo (2 planos ARGB sobre cor de fundo, com efeitos por scanline via Copper).
#pragma once
#include "bus.h"
#include "hostpool.h"
#include <memory>

namespace sf {

class Blitter : public Device {
 public:
  explicit Blitter(Bus& b) : bus(b) {}
  u32 read(u32 off) override;
  void write(u32 off, u32 v) override;
  struct Job { u32 op, src, spitch, dst, dpitch, w, h, color, mask, mpitch, minterm; bool irq; };
  bool error = false; u64 jobs = 0, pixels = 0, sprites_drawn = 0;
  bool submit(const Job& j);
  bool busy() const { return running; }
  // execução funcional síncrona; retorna ciclos (0 em erro)
  u64 execute(const Job& j);
 private:
  Bus& bus; bool running = false; std::deque<Job> q; u32 r[16] = {0};
  u64 spr_px = 0, spr_n = 0; Ps spr_fin = 0;               // tráfego da última lista de sprites (pixels visíveis, sprites)
  u64 exec_sprites(const Job& j);
  void kick();
};

class Copper : public Device {
 public:
  explicit Copper(Bus& b) : bus(b) {}
  u32 read(u32 off) override { return off == abi::COP_LIST ? list : off == abi::COP_CTRL ? (enabled ? 1 : 0) : 0; }
  void write(u32 off, u32 v) override { if (off == abi::COP_LIST) list = v; else if (off == abi::COP_CTRL) enabled = v & 1; }
  u32 list = 0; bool enabled = false;
  void restart() { pc = list; ended = !enabled; }
  // Executa instruções do Copper até um WAIT posterior à linha atual.
  void run_line(int line);
  u64 ops_executed = 0;
  std::function<void(u32 addr)> before_move;   // chamado antes de cada MOVE (o vídeo compõe as linhas pendentes se o alvo tiver efeitos fora dele)
 private:
  Bus& bus; u32 pc = 0; bool ended = true;
};

class Video : public Device {
 public:
  Video(Bus& b, Copper& c) : bus(b), cop(c) { set_mode(abi::VMODE_240P); }
  u32 read(u32 off) override;
  void write(u32 off, u32 v) override;
  void start();                          // agenda vblanks periódicos (60 Hz)
  void vblank();                         // compõe um quadro e dispara IRQ
  int width() const { return W; } int height() const { return H; }
  std::vector<u32> frame;                // 0x00RRGGBB, W*H
  u32 frames = 0; std::function<void(Video&)> on_frame;
  std::function<void()> on_vblank_hook;  // chamado antes da composição (ex.: Paula.render_until)
  struct Layer { u32 addr = 0, pitch = 0; i32 scx = 0, scy = 0; u32 alpha = 255, winx = 0, winy = 0, flags = 0, size = 0; };
  Layer layer[abi::VID_LAYERS];
  u32 bg = 0xFF000000, ctrl = 0, mode = 0, order = 0x3210;
  int cur_line = 0;
  u32 palette[256] = {0};                // CLUT ARGB para planos indexados (o Copper pode reescrevê-la por scanline)
 private:
  Bus& bus; Copper& cop; int W = 320, H = 240; bool started = false;
  void set_mode(u32 m);
  void compose();
  // Composição: o Copper roda em série linha a linha e cada linha guarda o seu estado (camadas, fundo, paleta); os pixels das
  // linhas pendentes saem em paralelo no host antes de qualquer MOVE do Copper com efeito fora dos registradores do vídeo.
  struct Row { const u8* p; int w, bpp; bool idx, wrap, add, pre; i32 scx; u32 alpha; int x0, x1; };
  struct Line { int y, nrows, pal; u32 bg; Row rows[abi::VID_LAYERS]; };
  std::vector<Line> pend; std::vector<u32> pend_pal; std::unique_ptr<HostPool> pool;
  void compose_line(const Line& ln);
  void compose_flush();
  bool enabled(int i) const { return (i < 2 && (ctrl & (1u << i))) || (layer[i].flags & abi::LF_ENABLE); }
  bool indexed(int i) const { return (i < 2 && (ctrl & (8u << i))) || (layer[i].flags & abi::LF_INDEXED); }
};

}  // namespace sf
