// FlameCore 3D: GPU de renderização em blocos (TBR) com eliminação de superfícies ocultas (HSR) por bloco.
// Implementação funcional em software: rasterização com precisão sub-pixel 28.4, regra top-left,
// interpolação com correção de perspectiva, 2 unidades de textura, fog, blend, stencil.
#pragma once
#include "bus.h"

namespace sf {

class FlameCore : public Device {
 public:
  explicit FlameCore(Bus& b) : bus(b) {}
  struct Stats { u64 tris_in = 0, tris_drawn = 0, pix_covered = 0, pix_shaded = 0, tiles = 0, cycles = 0, texfetch = 0, vram_write = 0, bin_entries = 0; };
  Stats last, total;
  bool hsr = true;
  bool error = false; std::string error_msg;
  u64 lists_done = 0;

  bool submit(u32 addr, u32 nwords, bool irq);       // enfileira lista de comandos
  Stats execute(u32 addr, u32 nwords);               // execução funcional síncrona
  bool busy() const { return running; }
  u32 read(u32 off) override;
  void write(u32 off, u32 v) override;

  struct Vtx { float x, y, z, rhw, r, g, b, a, u, v, fog; };

 private:
  struct Tex { bool valid = false; const u8* ptr = nullptr; u32 addr = 0; int lw = 0, lh = 0, fmt = 0, flags = 0, nlevels = 1; u32 off[16] = {0}; };
  struct RState {
    u32 flags = 0; u32 bsrc = abi::BF_ONE, bdst = abi::BF_ZERO; u32 fog_color = 0; float fog_start = 0, fog_end = 1;
    u32 sfunc = 6, sref = 0, smask = 0xFF, sfail = 0, zfail = 0, zpass = 0;
    Tex tex[2]; u32 texenv[2] = {0, 0}; float uvscale[2] = {1, 1}; u32 pal[2][256] = {{0}};   // paletas (CI4/CI8) por unidade
  };
  struct Tri { Vtx v[3]; int state; i32 x[3], y[3]; i64 area; float lod[2]; int minx, miny, maxx, maxy; };
  struct Fb { u32 addr = 0, pitch = 0; int w = 0, h = 0; } rt;
  struct ClearSt { u32 argb = 0; float depth = 1.0f; u32 flags = 0; } clr;

  u32 reg_addr = 0, reg_len = 0;
  Bus& bus; bool running = false; struct Job { u32 addr, n; bool irq; }; std::deque<Job> queue;
  // estado da lista de comandos
  RState cur; bool state_dirty = true; std::vector<RState> states; std::vector<Tri> tris;
  u32 vbase = 0; int scx = 0, scy = 0, scw = 65535, sch = 65535;
  Stats st;

  void kick();
  void flush();
  void draw(u32 prim, u32 count, const std::function<u32(u32)>& index);
  void emit(const Vtx& a, const Vtx& b, const Vtx& c);
  Vtx load_vertex(u32 idx);
  int cur_state();
  bool eligible(const Tri& t) const;
  bool cover(const Tri& t, int px, int py, float& l0, float& l1, float& l2);
  u32 shade(const Tri& t, float l0, float l1, float l2);
  void sample(const RState& s, int unit, float u, float v, float lod, float out[4]);
  u32 texel(const Tex& t, const u32* pal, int level, int x, int y);
  void render_tile(int tx, int ty, const std::vector<u32>& bin);
  void imm_tri(const Tri& t, int ti);

  // buffers do tile
  u32 tcol[abi::TILE * abi::TILE]; float tz[abi::TILE * abi::TILE]; u8 tst[abi::TILE * abi::TILE]; int tvis[abi::TILE * abi::TILE];
  int tox = 0, toy = 0;     // origem do tile
};

}  // namespace sf
