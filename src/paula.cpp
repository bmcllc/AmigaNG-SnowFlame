#include "paula.h"

namespace sf {
using namespace abi;

Paula::Paula(Bus& b) : bus(b) {
  const int dl[4] = {1557, 1617, 1491, 1422}; const int da[2] = {556, 441};
  for (int i = 0; i < 4; i++) { combL[i].b.assign(dl[i], 0); combR[i].b.assign(dl[i] + 23, 0); }
  for (int i = 0; i < 2; i++) { apL[i].b.assign(da[i], 0); apR[i].b.assign(da[i] + 23, 0); }
}

// ---------------- Registradores ----------------
void Paula::voice_write(Voice& v, u32 reg, u32 val) {
  v.dirty = true;
  switch (reg) {
    case PV_ADDR: v.addr = val; break; case PV_LEN: v.len = val; break; case PV_LOOP_START: v.loop_start = val; break;
    case PV_LOOP_LEN: v.loop_len = val; break; case PV_STEP: v.step = val; break; case PV_VOL_L: v.vol_l = val; break;
    case PV_VOL_R: v.vol_r = val; break; case PV_REV_SEND: v.rev_send = val; break; case PV_LP_COEF: v.lp_coef = val; break;
    case PV_X: v.x = u2f(val); break; case PV_Y: v.y = u2f(val); break; case PV_Z: v.z = u2f(val); break;
    case PV_ADPCM_PRED: v.adpcm_pred = (i32)val; break; case PV_ADPCM_IDX: v.adpcm_idx = (i32)val; break;
    case PV_CTRL: { bool was = v.ctrl & 1; v.ctrl = val; if ((val & 1) && !was) start_voice(v); break; }
  }
}
u32 Paula::voice_read(const Voice& v, u32 reg) const {
  switch (reg) {
    case PV_ADDR: return v.addr; case PV_LEN: return v.len; case PV_LOOP_START: return v.loop_start; case PV_LOOP_LEN: return v.loop_len;
    case PV_STEP: return v.step; case PV_VOL_L: return v.vol_l; case PV_VOL_R: return v.vol_r; case PV_CTRL: return v.ctrl;
    case PV_REV_SEND: return v.rev_send; case PV_LP_COEF: return v.lp_coef; case PV_X: return f2u(v.x); case PV_Y: return f2u(v.y);
    case PV_Z: return f2u(v.z); case PV_ADPCM_PRED: return (u32)v.adpcm_pred; case PV_ADPCM_IDX: return (u32)v.adpcm_idx;
  }
  return 0;
}
void Paula::start_voice(Voice& v) { v.pos = 0; v.lp = 0; v.ad_i = -1; }

u32 Paula::read(u32 off) {
  if (off == PA_MASTER) return master; if (off == PA_REVERB_WET) return rev_wet;
  if (off == PA_SEQ_ADDR) return seq_addr; if (off == PA_SEQ_LEN) return seq_len;
  if (off == PA_SEQ_STATUS) return seq_running ? 1 : 0;
  if (off == PA_DSP_LOAD) return dsp_peak; if (off == PA_DSP_DROPS) return (u32)dsp_drops;
  if (off >= PA_LEG0 && off < PA_LEG0 + 0x40) {
    const Legacy& l = leg[(off - PA_LEG0) / 0x10]; switch ((off - PA_LEG0) % 0x10) { case 0: return l.addr; case 4: return l.len; case 8: return l.period; default: return l.ctrl; }
  }
  if (off >= PA_VOICE0 && off < PA_VOICE0 + PAULA_VOICES * 0x40) return voice_read(voices[(off - PA_VOICE0) / 0x40], (off - PA_VOICE0) % 0x40);
  return 0;
}
void Paula::write(u32 off, u32 v) {
  if (off == PA_MASTER) { master = v; return; } if (off == PA_REVERB_WET) { rev_wet = v; return; }
  if (off == PA_DSP_DROPS) { dsp_drops = 0; dsp_peak = 0; return; }
  if (off == PA_SEQ_ADDR) { seq_addr = v; return; } if (off == PA_SEQ_LEN) { seq_len = v; return; }
  if (off == PA_SEQ_CTRL) { if (v & 1) { seq_running = true; seq_idx = 0; seq_pos = 0; } else seq_running = false; seq_loop = v & 2; return; }
  if (off >= PA_LEG0 && off < PA_LEG0 + 0x40) {
    Legacy& l = leg[(off - PA_LEG0) / 0x10];
    switch ((off - PA_LEG0) % 0x10) { case 0: l.addr = v; break; case 4: l.len = v; break; case 8: l.period = v ? v : 1; break;
      default: { bool was = l.ctrl & 0x80000000u; l.ctrl = v; if ((v & 0x80000000u) && !was) l.pos = 0; } }
    return;
  }
  if (off >= PA_VOICE0 && off < PA_VOICE0 + PAULA_VOICES * 0x40) voice_write(voices[(off - PA_VOICE0) / 0x40], (off - PA_VOICE0) % 0x40, v);
}
void Paula::seq_apply(u32 voice, u32 reg, u32 val) {
  if (voice == 0xFFFFFFFFu) write(reg, val); else if (voice < PAULA_VOICES) voice_write(voices[voice], reg, val);
}

// ---------------- Amostras ----------------
static float nib(const u8* aram, u32 base, u32 n) { u8 b = aram[base + n / 2]; return (float)((n & 1) ? (b & 15) : (b >> 4)); }

float Paula::get_sample(Voice& v, u32 idx) {
  if (idx >= v.len) return 0;
  u32 fmt = (v.ctrl >> 2) & 3;
  if (fmt == 0) { u32 a = v.addr + idx; return a < ARAM_SIZE ? (float)(i8)bus.aram[a] / 128.0f : 0; }
  u32 a = v.addr + idx * 2; return a + 1 < ARAM_SIZE ? (float)(i16)rbe16(&bus.aram[a]) / 32768.0f : 0;
}

// Busca s[idx] e s[idx+1] (com tratamento de laço para PCM); ADPCM decodifica sequencialmente.
bool Paula::fetch_pair(Voice& v, u32 idx, float& s0, float& s1) {
  u32 fmt = (v.ctrl >> 2) & 3;
  if (fmt == 2) {
    if (v.ad_i < 0) {
      v.ad = adpcm::State{v.adpcm_pred, std::max(0, std::min(88, v.adpcm_idx))};
      if (v.addr + (v.len + 1) / 2 > ARAM_SIZE) return false;
      v.a_cur = adpcm::decode(v.ad, (int)nib(bus.aram.data(), v.addr, 0)); v.ad_i = 0;
      v.a_next = v.len > 1 ? adpcm::decode(v.ad, (int)nib(bus.aram.data(), v.addr, 1)) : v.a_cur;
    }
    while ((u32)v.ad_i < idx) {
      v.a_cur = v.a_next; v.ad_i++;
      v.a_next = ((u32)v.ad_i + 1 < v.len) ? adpcm::decode(v.ad, (int)nib(bus.aram.data(), v.addr, v.ad_i + 1)) : v.a_cur;
    }
    s0 = v.a_cur / 32768.0f; s1 = v.a_next / 32768.0f; return true;
  }
  s0 = get_sample(v, idx);
  u32 n1 = idx + 1; bool loop = v.ctrl & 2;
  u32 lend = v.loop_len ? v.loop_start + v.loop_len : v.len;
  if (loop && n1 >= lend) n1 = v.loop_start + (n1 - lend);
  s1 = n1 < v.len ? get_sample(v, n1) : s0;
  return true;
}

// ---------------- Mixagem ----------------
void Paula::render_until(Ps t) {
  u64 target = (u64)((__uint128_t)t * PAULA_RATE / 1000000000000ull);
  if (target > frames_rendered) render((int)(target - frames_rendered));
}

void Paula::render(int frames) {
  size_t base = pcm.size(); pcm.resize(base + (size_t)frames * 2);
  for (int n = 0; n < frames; n++) {
    // sequenciador por eventos
    while (seq_running) {
      if (seq_idx >= seq_len) {
        if (seq_loop && seq_len) { seq_idx = 0; seq_pos = 0; } else { seq_running = false; bus.raise_irq(IRQ_AUDIO); }
        break;
      }
      u32 ea = seq_addr + seq_idx * PA_SEQ_EVENT_SIZE;
      const u8* e = bus.raw(ea, PA_SEQ_EVENT_SIZE); if (!e) { seq_running = false; break; }
      if (rbe32(e) > seq_pos) break;
      seq_apply(rbe32(e + 4), rbe32(e + 8), rbe32(e + 12)); seq_idx++;
    }
    if (seq_running) seq_pos++;
    const float mscale = master / 256.0f, wet = rev_wet / 256.0f;   // por quadro: eventos do sequenciador podem alterá-los
    float L = 0, R = 0, send = 0; u32 dsp_cost = 40 + (wet > 0 ? 180 : 0);
    for (auto& v : voices) {
      if (!(v.ctrl & 1)) continue;
      u32 vcost = 24 + (((v.ctrl >> 2) & 3) == 2 ? 10 : 0) + ((v.ctrl & 0x20) ? 6 : 0) + ((v.ctrl & 0x10) ? 12 : 0);
      if (dsp_cost + vcost > DSP_BUDGET) { v.pos += v.step; dsp_drops++; continue; }   // DSP estourou: a voz é silenciada neste quadro
      dsp_cost += vcost;
      if (v.dirty) {
        float gl = v.vol_l / 256.0f, gr = v.vol_r / 256.0f;
        if (v.ctrl & 0x10) {
          float d = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z), att = 1.0f / (1.0f + d), pan = d > 1e-4f ? std::max(-1.0f, std::min(1.0f, v.x / d)) : 0.0f;
          float ang = (pan + 1.0f) * 0.78539816f; gl *= att * std::cos(ang) * 1.41421356f; gr *= att * std::sin(ang) * 1.41421356f;
        }
        v.gl = gl; v.gr = gr; v.dirty = false;
      }
      u32 idx = (u32)(v.pos >> 16); u32 frac = (u32)(v.pos & 0xFFFF);
      bool loop = v.ctrl & 2; u32 fmt = (v.ctrl >> 2) & 3;
      u32 lend = (fmt == 2) ? v.len : (v.loop_len ? v.loop_start + v.loop_len : v.len);
      if (idx >= lend || idx >= v.len) {
        if (loop && lend > 0) {
          u32 lstart = fmt == 2 ? 0 : v.loop_start;
          u32 span = lend - lstart; if (!span) { v.ctrl &= ~1u; continue; }
          idx = lstart + (idx - lend) % span; v.pos = ((u64)idx << 16) | frac;
          if (fmt == 2) v.ad_i = -1;
        } else { v.ctrl &= ~1u; continue; }
      }
      float s0, s1; if (!fetch_pair(v, idx, s0, s1)) { v.ctrl &= ~1u; continue; }
      float x = s0 + (s1 - s0) * (frac / 65536.0f);
      if (v.ctrl & 0x20) { v.lp += (v.lp_coef / 256.0f) * (x - v.lp); x = v.lp; }
      L += x * v.gl; R += x * v.gr; send += x * v.rev_send / 256.0f * (0.5f * (v.gl + v.gr));
      v.pos += v.step; active_voice_samples++;
    }
    for (int c = 0; c < 4; c++) {                         // canais legados
      // canais legados (Paula clássico): 0,3 esquerda; 1,2 direita
      if (leg[c].ctrl & 0x80000000u) dsp_cost += 8;
      Legacy& l = leg[c]; if (!(l.ctrl & 0x80000000u)) continue;
      u32 bytes = l.len * 2, idx = (u32)(l.pos >> 16);
      if (idx >= bytes) { if (l.ctrl & 0x40000000u) { idx %= std::max(1u, bytes); l.pos = (u64)idx << 16; } else { l.ctrl &= ~0x80000000u; continue; } }
      u32 a = l.addr + idx; float x = a < ARAM_SIZE ? (float)(i8)bus.aram[a] / 128.0f : 0;
      x *= (l.ctrl & 0x7F) / 64.0f * 0.5f; if (c == 0 || c == 3) L += x; else R += x;
      l.pos += (u64)((3546895ull * 65536ull) / ((u64)l.period * PAULA_RATE));
    }
    dsp_peak = std::max(dsp_peak, dsp_cost * 1000 / DSP_BUDGET);
    if (wet > 0) {
      float in = send * 0.25f, oL = 0, oR = 0;
      for (int k = 0; k < 4; k++) {
        Comb& a = combL[k]; float y = a.b[a.i]; a.lp = y * 0.8f + a.lp * 0.2f; a.b[a.i] = in + a.lp * 0.84f; a.i = (a.i + 1) % a.b.size(); oL += y;
        Comb& b = combR[k]; y = b.b[b.i]; b.lp = y * 0.8f + b.lp * 0.2f; b.b[b.i] = in + b.lp * 0.84f; b.i = (b.i + 1) % b.b.size(); oR += y;
      }
      for (int k = 0; k < 2; k++) {
        Ap& a = apL[k]; float bo = a.b[a.i]; float yy = -oL + bo; a.b[a.i] = oL + bo * 0.5f; a.i = (a.i + 1) % a.b.size(); oL = yy;
        Ap& b = apR[k]; bo = b.b[b.i]; yy = -oR + bo; b.b[b.i] = oR + bo * 0.5f; b.i = (b.i + 1) % b.b.size(); oR = yy;
      }
      L += oL * wet; R += oR * wet;
    }
    L *= mscale * 32767.0f; R *= mscale * 32767.0f;
    pcm[base + n * 2] = (i16)std::max(-32768.0f, std::min(32767.0f, L));
    pcm[base + n * 2 + 1] = (i16)std::max(-32768.0f, std::min(32767.0f, R));
  }
  frames_rendered += frames;
}

}  // namespace sf
