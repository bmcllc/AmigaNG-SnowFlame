// FlameCore 3D: GPU de renderização em blocos (TBR) com eliminação de superfícies ocultas (HSR) por bloco.
// Implementação funcional em software: rasterização com precisão sub-pixel 28.4, regra top-left,
// interpolação com correção de perspectiva, 2 unidades de textura, fog, blend, stencil.
#pragma once
#include "bus.h"
#include "hostpool.h"
#include <memory>

namespace sf {

typedef float f4 __attribute__((vector_size(16)));   // 4 floats por instrução SIMD do host: cada lane faz exatamente a conta escalar (sem FMA)

class FlameCore : public Device {
 public:
  explicit FlameCore(Bus& b);
  ~FlameCore();
  struct Stats { u64 tris_in = 0, tris_drawn = 0, pix_covered = 0, pix_shaded = 0, tiles = 0, cycles = 0, texfetch = 0, vram_write = 0, bin_entries = 0; };
  Stats last, total;
  bool hsr = true;
  // Threads do host para renderizar blocos em paralelo (só velocidade: cada bloco é independente e o resultado é idêntico
  // ao serial). 0 = automático (HostPool::auto_threads: núcleos do host ou SF_HOST_THREADS); 1 = serial.
  int host_threads = 0;
  bool error = false; std::string error_msg;
  u64 lists_done = 0;

  bool submit(u32 addr, u32 nwords, bool irq);       // enfileira lista de comandos
  Stats execute(u32 addr, u32 nwords);               // execução funcional síncrona
  bool busy() const { return running; }
  u32 read(u32 off) override;
  void write(u32 off, u32 v) override;

  struct Vtx { float x, y, z, rhw, r, g, b, a, u, v, fog, u1, v1; };

 private:
  friend class Fabric;                              // backend SystemVerilog (fabric/)
  struct Tex { bool valid = false; const u8* ptr = nullptr; u32 addr = 0; int lw = 0, lh = 0, fmt = 0, flags = 0, nlevels = 1; u32 off[16] = {0}; };
  struct RState {
    u32 flags = 0; u32 bsrc = abi::BF_ONE, bdst = abi::BF_ZERO, basrc = abi::BF_ONE, badst = abi::BF_ZERO; u32 fog_color = 0; float fog_start = 0, fog_end = 1;
    u32 sfunc = 6, sref = 0, smask = 0xFF, sfail = 0, zfail = 0, zpass = 0;
    Tex tex[2]; u32 texenv[2] = {0, 0}; float uvscale[2] = {1, 1}; u32 pal[2][256] = {{0}};   // paletas (CI4/CI8) por unidade
  };
  // Amostrador de uma unidade de textura, resolvido uma vez por triângulo (nível de mip, base, dimensões)
  struct TexU { const u8* base = nullptr; int w = 1, h = 1; float wf = 1, hf = 1, uvs = 1; };
  // Atributos dos 3 vértices em vetores (cor rgba, uv/uv1, rhw, fog) para interpolar os quatro canais de uma vez
  struct Tri { Vtx v[3]; int state; i32 x[3], y[3]; i64 area; double inv; float lod[2]; int minx, miny, maxx, maxy; f4 col[3], uv[3], rhwv; float fog[3]; TexU tu[2]; };
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
  bool cover(const Tri& t, int px, int py, float& l0, float& l1, float& l2) const;
  // Contexto de um bloco em renderização (um por thread do host): buffers do bloco, origem e estatísticas parciais.
  struct TileCtx {
    u32 tcol[abi::TILE * abi::TILE]; float tz[abi::TILE * abi::TILE]; u8 tst[abi::TILE * abi::TILE]; int tvis[abi::TILE * abi::TILE];
    int tox = 0, toy = 0; Stats st;
  };
  u32 shade(TileCtx& c, const Tri& t, const RState& s, float l0, float l1, float l2) const;
  f4 sample(TileCtx& c, const RState& s, const TexU& q, int unit, float u, float v) const;
  static u32 texel(const Tex& t, const TexU& q, const u32* pal, int x, int y);
  template <class F> static void raster(const Tri& t, int x0, int x1, int y0, int y1, F&& fn);
  void render_tile(TileCtx& c, int tx, int ty, const std::vector<u32>& bin) const;
  void imm_tri(TileCtx& c, const Tri& t) const;
  bool rt_is_texture() const;
  std::unique_ptr<HostPool> pool; std::vector<std::unique_ptr<TileCtx>> ctx;   // um contexto por thread do pool
};

}  // namespace sf
