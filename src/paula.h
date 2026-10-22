// SnowPaula: DSP de áudio de 64 vozes (PCM8/PCM16/ADPCM), filtro, posicionamento 3D, reverb,
// 4 canais legados no estilo Paula (por período) e sequenciador por eventos. 48 kHz, 16 bits estéreo.
#pragma once
#include "bus.h"
#include "adpcm.h"

namespace sf {

class Paula : public Device {
 public:
  explicit Paula(Bus& b);
  u32 read(u32 off) override;
  void write(u32 off, u32 v) override;

  // Gera quadros de áudio até o instante t (ps); acumula em pcm (L,R intercalados).
  void render_until(Ps t);
  void render(int frames);
  std::vector<i16> pcm; u64 frames_rendered = 0;

  struct Voice {
    u32 addr = 0, len = 0, loop_start = 0, loop_len = 0, step = 0x10000, vol_l = 256, vol_r = 256, ctrl = 0, rev_send = 0, lp_coef = 256;
    float x = 0, y = 0, z = 0; i32 adpcm_pred = 0, adpcm_idx = 0;
    u64 pos = 0; float lp = 0; bool dirty = true; float gl = 1, gr = 1;
    adpcm::State ad; int ad_i = -1, a_cur = 0, a_next = 0;
  };
  struct Legacy { u32 addr = 0, len = 0, period = 428, ctrl = 0; u64 pos = 0; };
  Voice voices[abi::PAULA_VOICES]; Legacy leg[4];
  u32 master = 256, rev_wet = 0;
  bool seq_running = false, seq_loop = false;
  u64 active_voice_samples = 0;
  // Modelo de carga do DSP de 100 MHz: orçamento de 2083 ciclos por quadro de áudio a 48 kHz.
  static constexpr u32 DSP_BUDGET = 100000000u / 48000u;
  u32 dsp_peak = 0; u64 dsp_drops = 0;

  struct Comb { std::vector<float> b; size_t i = 0; float lp = 0; };
  struct Ap { std::vector<float> b; size_t i = 0; };

 private:
  friend class Fabric;                              // backend SystemVerilog (fabric/)
  Bus& bus;
  u32 seq_addr = 0, seq_len = 0, seq_idx = 0; u64 seq_pos = 0;
  Comb combL[4], combR[4]; Ap apL[2], apR[2];
  void voice_write(Voice& v, u32 reg, u32 val);
  u32 voice_read(const Voice& v, u32 reg) const;
  void seq_apply(u32 voice, u32 reg, u32 val);
  float get_sample(Voice& v, u32 idx);
  bool fetch_pair(Voice& v, u32 idx, float& s0, float& s1);
  void start_voice(Voice& v);
};

}  // namespace sf
