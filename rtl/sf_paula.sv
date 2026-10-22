// SnowPaula em SystemVerilog: DSP de áudio de 64 vozes (PCM8/PCM16/ADPCM IMA), filtro passa-baixa, posicionamento 3D,
// reverb (4 combs + 2 all-pass por canal), 4 canais legados no estilo Paula (por período) e sequenciador por eventos.
// 48 kHz, 16 bits estéreo; modelo de carga de um DSP de 100 MHz (2083 ciclos por quadro). Mesmo comportamento de
// src/paula.cpp: as contas em ponto flutuante são FP32 feitas pela FPU do hospedeiro (f32_*), na mesma ordem.
// As amostras são lidas da ARAM (Memory Host); cada quadro produzido vai para o Audio Host (fab_pcm).
module sf_paula;
  import fabric::*; import sf100_abi::*;
  localparam int NV = 64;
  localparam u32_t DSP_BUDGET = 100000000 / 48000;
  localparam f32_t F_256 = 32'h43800000, F_128 = 32'h43000000, F_32768 = 32'h47000000, F_65536 = 32'h47800000, F_EPS = 32'h38d1b717,
                   F_PI4 = 32'h3f490fdb, F_SQRT2 = 32'h3fb504f3, F_025 = 32'h3e800000, F_08 = 32'h3f4ccccd, F_02 = 32'h3e4ccccd,
                   F_084 = 32'h3f570a3d, F_32767 = 32'h46fffe00, F_64 = 32'h42800000, F_M1 = 32'hbf800000;
  localparam int CL[4] = '{1557, 1617, 1491, 1422}, AL[2] = '{556, 441};
  localparam int K_STEP[89] = '{7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
  localparam int K_IDX[8] = '{-1, -1, -1, -1, 2, 4, 6, 8};

  chandle h; int dev;
  // vozes
  u32_t v_addr[NV], v_len[NV], v_lstart[NV], v_llen[NV], v_step[NV], v_voll[NV], v_volr[NV], v_ctrl[NV], v_rsend[NV], v_lpc[NV];
  f32_t v_x[NV], v_y[NV], v_z[NV], v_lp[NV], v_gl[NV], v_gr[NV]; int v_apred[NV], v_aidx[NV];
  longint unsigned v_pos[NV]; bit v_dirty[NV];
  int v_spred[NV], v_sidx[NV], v_adi[NV], v_acur[NV], v_anext[NV];        // estado do decodificador ADPCM
  // canais legados
  u32_t g_addr[4], g_len[4], g_period[4], g_ctrl[4]; longint unsigned g_pos[4];
  // global
  u32_t master, rev_wet, seq_addr, seq_len, seq_idx, dsp_peak; bit seq_running, seq_loop;
  longint unsigned seq_pos, frames_rendered, active_voice_samples, dsp_drops;
  // reverb
  f32_t cl_b[4][1640], cr_b[4][1640], al_b[2][600], ar_b[2][600], cl_lp[4], cr_lp[4]; int cl_i[4], cr_i[4], al_i[2], ar_i[2];

  export "DPI-C" function paula_bind;
  export "DPI-C" function paula_read;
  export "DPI-C" function paula_write;
  export "DPI-C" function paula_render;
  export "DPI-C" function paula_render_until;

  function automatic void paula_bind(chandle hh, int d);
    h = hh; dev = d;
    for (int i = 0; i < NV; i++) begin
      v_addr[i] = 0; v_len[i] = 0; v_lstart[i] = 0; v_llen[i] = 0; v_step[i] = 32'h10000; v_voll[i] = 256; v_volr[i] = 256; v_ctrl[i] = 0;
      v_rsend[i] = 0; v_lpc[i] = 256; v_x[i] = 0; v_y[i] = 0; v_z[i] = 0; v_apred[i] = 0; v_aidx[i] = 0; v_pos[i] = 0; v_lp[i] = 0;
      v_dirty[i] = 1; v_gl[i] = F32_1; v_gr[i] = F32_1; v_spred[i] = 0; v_sidx[i] = 0; v_adi[i] = -1; v_acur[i] = 0; v_anext[i] = 0;
    end
    for (int c = 0; c < 4; c++) begin g_addr[c] = 0; g_len[c] = 0; g_period[c] = 428; g_ctrl[c] = 0; g_pos[c] = 0; end
    master = 256; rev_wet = 0; seq_addr = 0; seq_len = 0; seq_idx = 0; dsp_peak = 0; seq_running = 0; seq_loop = 0;
    seq_pos = 0; frames_rendered = 0; active_voice_samples = 0; dsp_drops = 0;
    for (int k = 0; k < 4; k++) begin cl_i[k] = 0; cr_i[k] = 0; cl_lp[k] = 0; cr_lp[k] = 0; for (int n = 0; n < 1640; n++) begin cl_b[k][n] = 0; cr_b[k][n] = 0; end end
    for (int k = 0; k < 2; k++) begin al_i[k] = 0; ar_i[k] = 0; for (int n = 0; n < 600; n++) begin al_b[k][n] = 0; ar_b[k][n] = 0; end end
  endfunction

  // Espelhos do frontend: 0 frames_rendered, 1 dsp_peak, 2 dsp_drops, 3 master, 4 rev_wet, 5 seq_running, 6 active_voice_samples
  function automatic void obs();
    fab_obs(h, dev, 0, frames_rendered); fab_obs(h, dev, 1, 64'(dsp_peak)); fab_obs(h, dev, 2, dsp_drops); fab_obs(h, dev, 3, 64'(master));
    fab_obs(h, dev, 4, 64'(rev_wet)); fab_obs(h, dev, 5, 64'(seq_running)); fab_obs(h, dev, 6, active_voice_samples);
  endfunction

  // ---------------- Registradores ----------------
  function automatic void voice_write(int v, u32_t r, u32_t val);
    bit was;
    v_dirty[v] = 1;
    case (r)
      PV_ADDR: v_addr[v] = val; PV_LEN: v_len[v] = val; PV_LOOP_START: v_lstart[v] = val; PV_LOOP_LEN: v_llen[v] = val;
      PV_STEP: v_step[v] = val; PV_VOL_L: v_voll[v] = val; PV_VOL_R: v_volr[v] = val; PV_REV_SEND: v_rsend[v] = val; PV_LP_COEF: v_lpc[v] = val;
      PV_X: v_x[v] = val; PV_Y: v_y[v] = val; PV_Z: v_z[v] = val;
      PV_ADPCM_PRED: v_apred[v] = int'(val); PV_ADPCM_IDX: v_aidx[v] = int'(val);
      PV_CTRL: begin was = v_ctrl[v][0]; v_ctrl[v] = val; if (val[0] && !was) begin v_pos[v] = 0; v_lp[v] = 0; v_adi[v] = -1; end end
      default: ;
    endcase
  endfunction
  function automatic u32_t voice_read(int v, u32_t r);
    case (r)
      PV_ADDR: return v_addr[v]; PV_LEN: return v_len[v]; PV_LOOP_START: return v_lstart[v]; PV_LOOP_LEN: return v_llen[v];
      PV_STEP: return v_step[v]; PV_VOL_L: return v_voll[v]; PV_VOL_R: return v_volr[v]; PV_CTRL: return v_ctrl[v];
      PV_REV_SEND: return v_rsend[v]; PV_LP_COEF: return v_lpc[v]; PV_X: return v_x[v]; PV_Y: return v_y[v]; PV_Z: return v_z[v];
      PV_ADPCM_PRED: return u32_t'(v_apred[v]); PV_ADPCM_IDX: return u32_t'(v_aidx[v]);
      default: return 0;
    endcase
  endfunction

  function automatic u32_t paula_read(u32_t off);
    int c;
    if (off == PA_MASTER) return master; if (off == PA_REVERB_WET) return rev_wet;
    if (off == PA_SEQ_ADDR) return seq_addr; if (off == PA_SEQ_LEN) return seq_len;
    if (off == PA_SEQ_STATUS) return seq_running ? 1 : 0;
    if (off == PA_DSP_LOAD) return dsp_peak; if (off == PA_DSP_DROPS) return u32_t'(dsp_drops);
    if (off >= PA_LEG0 && off < PA_LEG0 + 32'h40) begin
      c = int'((off - PA_LEG0) / 16);
      case ((off - PA_LEG0) % 16) 0: return g_addr[c]; 4: return g_len[c]; 8: return g_period[c]; default: return g_ctrl[c]; endcase
    end
    if (off >= PA_VOICE0 && off < PA_VOICE0 + NV * 32'h40) return voice_read(int'((off - PA_VOICE0) / 32'h40), (off - PA_VOICE0) % 32'h40);
    return 0;
  endfunction
  function automatic void write_reg(u32_t off, u32_t v);
    int c; bit was;
    if (off == PA_MASTER) begin master = v; return; end if (off == PA_REVERB_WET) begin rev_wet = v; return; end
    if (off == PA_DSP_DROPS) begin dsp_drops = 0; dsp_peak = 0; return; end
    if (off == PA_SEQ_ADDR) begin seq_addr = v; return; end if (off == PA_SEQ_LEN) begin seq_len = v; return; end
    if (off == PA_SEQ_CTRL) begin if (v[0]) begin seq_running = 1; seq_idx = 0; seq_pos = 0; end else seq_running = 0; seq_loop = v[1]; return; end
    if (off >= PA_LEG0 && off < PA_LEG0 + 32'h40) begin
      c = int'((off - PA_LEG0) / 16);
      case ((off - PA_LEG0) % 16)
        0: g_addr[c] = v; 4: g_len[c] = v; 8: g_period[c] = v != 0 ? v : 1;
        default: begin was = g_ctrl[c][31]; g_ctrl[c] = v; if (v[31] && !was) g_pos[c] = 0; end
      endcase
      return;
    end
    if (off >= PA_VOICE0 && off < PA_VOICE0 + NV * 32'h40) voice_write(int'((off - PA_VOICE0) / 32'h40), (off - PA_VOICE0) % 32'h40, v);
  endfunction
  function automatic void paula_write(u32_t off, u32_t v); write_reg(off, v); obs(); endfunction
  function automatic void seq_apply(u32_t voice, u32_t r, u32_t val);
    if (voice == 32'hFFFFFFFF) write_reg(r, val); else if (voice < NV) voice_write(int'(voice), r, val);
  endfunction

  // ---------------- Amostras ----------------
  function automatic int adpcm_decode(int v, int code);
    int st, d;
    st = K_STEP[v_sidx[v]]; d = st >>> 3;
    if ((code & 4) != 0) d += st; if ((code & 2) != 0) d += st >>> 1; if ((code & 1) != 0) d += st >>> 2;
    v_spred[v] += (code & 8) != 0 ? -d : d;
    v_spred[v] = v_spred[v] < -32768 ? -32768 : v_spred[v] > 32767 ? 32767 : v_spred[v];
    v_sidx[v] = v_sidx[v] + K_IDX[code & 7]; v_sidx[v] = v_sidx[v] < 0 ? 0 : v_sidx[v] > 88 ? 88 : v_sidx[v];
    return v_spred[v];
  endfunction
  function automatic int nib(u32_t base, u32_t n);
    byte unsigned b; b = fab_r8(h, ARAM_BASE + base + n / 2); return (n & 1) != 0 ? int'(b & 8'd15) : int'(b >> 4);
  endfunction
  function automatic f32_t get_sample(int v, u32_t idx);
    u32_t a; byte unsigned b;
    if (idx >= v_len[v]) return F32_0;
    if (((v_ctrl[v] >> 2) & 3) == 0) begin
      a = v_addr[v] + idx; if (a >= ARAM_SIZE) return F32_0;
      b = fab_r8(h, ARAM_BASE + a); return f32_div(f32_from_i32(int'(byte'(b))), F_128);
    end
    a = v_addr[v] + idx * 2; if (!(a + 1 < ARAM_SIZE)) return F32_0;
    return f32_div(f32_from_i32(int'(shortint'(fab_r16(h, ARAM_BASE + a)))), F_32768);
  endfunction
  // Busca s[idx] e s[idx+1] (com tratamento de laço para PCM); ADPCM decodifica sequencialmente. 0 = falha.
  function automatic bit fetch_pair(int v, u32_t idx, output f32_t s0, output f32_t s1);
    u32_t n1, lend;
    s0 = F32_0; s1 = F32_0;
    if (((v_ctrl[v] >> 2) & 3) == 2) begin
      if (v_adi[v] < 0) begin
        v_spred[v] = v_apred[v]; v_sidx[v] = v_aidx[v] < 0 ? 0 : v_aidx[v] > 88 ? 88 : v_aidx[v];
        if (v_addr[v] + (v_len[v] + 1) / 2 > ARAM_SIZE) return 0;
        v_acur[v] = adpcm_decode(v, nib(v_addr[v], 0)); v_adi[v] = 0;
        v_anext[v] = v_len[v] > 1 ? adpcm_decode(v, nib(v_addr[v], 1)) : v_acur[v];
      end
      while (u32_t'(v_adi[v]) < idx) begin
        v_acur[v] = v_anext[v]; v_adi[v]++;
        v_anext[v] = (u32_t'(v_adi[v]) + 1 < v_len[v]) ? adpcm_decode(v, nib(v_addr[v], u32_t'(v_adi[v]) + 1)) : v_acur[v];
      end
      s0 = f32_div(f32_from_i32(v_acur[v]), F_32768); s1 = f32_div(f32_from_i32(v_anext[v]), F_32768); return 1;
    end
    s0 = get_sample(v, idx);
    n1 = idx + 1;
    lend = v_llen[v] != 0 ? v_lstart[v] + v_llen[v] : v_len[v];
    if (v_ctrl[v][1] && n1 >= lend) n1 = v_lstart[v] + (n1 - lend);
    s1 = n1 < v_len[v] ? get_sample(v, n1) : s0;
    return 1;
  endfunction

  // ---------------- Mixagem ----------------
  function automatic void paula_render_until(ps_t t);
    longint unsigned target;
    target = 64'((128'(t) * 128'(48000)) / 128'(64'd1000000000000));
    if (target > frames_rendered) paula_render(int'(target - frames_rendered));
  endfunction

  function automatic void paula_render(int frames);
    u32_t ea, dsp_cost, vcost, idx, frac, fmt, lend, lstart, span, bytes, a, ld;
    f32_t mscale, wet, L, R, send, gl, gr, d, att, pan, ang, s0, s1, x, yv, bo, yy, inp, oL, oR;
    byte unsigned b; bit loop;
    for (int n = 0; n < frames; n++) begin
      // sequenciador por eventos
      while (seq_running) begin
        if (seq_idx >= seq_len) begin
          if (seq_loop && seq_len != 0) begin seq_idx = 0; seq_pos = 0; end else begin seq_running = 0; fab_irq(h, IRQ_AUDIO); end
          break;
        end
        ea = seq_addr + seq_idx * PA_SEQ_EVENT_SIZE;
        if (!fab_ok(h, ea, PA_SEQ_EVENT_SIZE)) begin seq_running = 0; break; end
        if (64'(fab_r32(h, ea)) > seq_pos) break;
        seq_apply(fab_r32(h, ea + 4), fab_r32(h, ea + 8), fab_r32(h, ea + 12)); seq_idx++;
      end
      if (seq_running) seq_pos++;
      mscale = f32_div(f32_from_u32(master), F_256); wet = f32_div(f32_from_u32(rev_wet), F_256);   // por quadro: eventos podem alterá-los
      L = F32_0; R = F32_0; send = F32_0; dsp_cost = 40 + (f32_gt(wet, F32_0) ? 180 : 0);
      for (int v = 0; v < NV; v++) begin
        if (!v_ctrl[v][0]) continue;
        fmt = (v_ctrl[v] >> 2) & 3;
        vcost = 24 + (fmt == 2 ? 10 : 0) + (v_ctrl[v][5] ? 6 : 0) + (v_ctrl[v][4] ? 12 : 0);
        if (dsp_cost + vcost > DSP_BUDGET) begin v_pos[v] += 64'(v_step[v]); dsp_drops++; continue; end   // DSP estourou: voz silenciada no quadro
        dsp_cost += vcost;
        if (v_dirty[v]) begin
          gl = f32_div(f32_from_u32(v_voll[v]), F_256); gr = f32_div(f32_from_u32(v_volr[v]), F_256);
          if (v_ctrl[v][4]) begin
            d = f32_sqrt(f32_add(f32_add(f32_mul(v_x[v], v_x[v]), f32_mul(v_y[v], v_y[v])), f32_mul(v_z[v], v_z[v])));
            att = f32_div(F32_1, f32_add(F32_1, d));
            pan = f32_gt(d, F_EPS) ? f32_max(F_M1, f32_min(F32_1, f32_div(v_x[v], d))) : F32_0;
            ang = f32_mul(f32_add(pan, F32_1), F_PI4);
            gl = f32_mul(gl, f32_mul(f32_mul(att, f32_cos(ang)), F_SQRT2)); gr = f32_mul(gr, f32_mul(f32_mul(att, f32_sin(ang)), F_SQRT2));
          end
          v_gl[v] = gl; v_gr[v] = gr; v_dirty[v] = 0;
        end
        idx = u32_t'(v_pos[v] >> 16); frac = u32_t'(v_pos[v] & 64'hFFFF);
        loop = v_ctrl[v][1];
        lend = fmt == 2 ? v_len[v] : (v_llen[v] != 0 ? v_lstart[v] + v_llen[v] : v_len[v]);
        if (idx >= lend || idx >= v_len[v]) begin
          if (loop && lend > 0) begin
            lstart = fmt == 2 ? 0 : v_lstart[v];
            span = lend - lstart; if (span == 0) begin v_ctrl[v][0] = 0; continue; end
            idx = lstart + (idx - lend) % span; v_pos[v] = (64'(idx) << 16) | 64'(frac);
            if (fmt == 2) v_adi[v] = -1;
          end else begin v_ctrl[v][0] = 0; continue; end
        end
        if (!fetch_pair(v, idx, s0, s1)) begin v_ctrl[v][0] = 0; continue; end
        x = f32_add(s0, f32_mul(f32_sub(s1, s0), f32_div(f32_from_u32(frac), F_65536)));
        if (v_ctrl[v][5]) begin v_lp[v] = f32_add(v_lp[v], f32_mul(f32_div(f32_from_u32(v_lpc[v]), F_256), f32_sub(x, v_lp[v]))); x = v_lp[v]; end
        L = f32_add(L, f32_mul(x, v_gl[v])); R = f32_add(R, f32_mul(x, v_gr[v]));
        send = f32_add(send, f32_mul(f32_div(f32_mul(x, f32_from_u32(v_rsend[v])), F_256), f32_mul(F32_HALF, f32_add(v_gl[v], v_gr[v]))));
        v_pos[v] += 64'(v_step[v]); active_voice_samples++;
      end
      for (int c = 0; c < 4; c++) begin                 // canais legados (Paula clássico): 0,3 esquerda; 1,2 direita
        if (g_ctrl[c][31]) dsp_cost += 8;
        if (!g_ctrl[c][31]) continue;
        bytes = g_len[c] * 2; idx = u32_t'(g_pos[c] >> 16);
        if (idx >= bytes) begin
          if (g_ctrl[c][30]) begin idx = idx % (bytes > 1 ? bytes : 1); g_pos[c] = 64'(idx) << 16; end
          else begin g_ctrl[c][31] = 0; continue; end
        end
        a = g_addr[c] + idx;
        if (a < ARAM_SIZE) begin b = fab_r8(h, ARAM_BASE + a); x = f32_div(f32_from_i32(int'(byte'(b))), F_128); end else x = F32_0;
        x = f32_mul(x, f32_mul(f32_div(f32_from_u32(g_ctrl[c] & 32'h7F), F_64), F32_HALF));
        if (c == 0 || c == 3) L = f32_add(L, x); else R = f32_add(R, x);
        g_pos[c] += (64'd3546895 * 64'd65536) / (64'(g_period[c]) * 64'd48000);
      end
      ld = dsp_cost * 1000 / DSP_BUDGET; if (ld > dsp_peak) dsp_peak = ld;
      if (f32_gt(wet, F32_0)) begin
        inp = f32_mul(send, F_025); oL = F32_0; oR = F32_0;
        for (int k = 0; k < 4; k++) begin
          yv = cl_b[k][cl_i[k]]; cl_lp[k] = f32_add(f32_mul(yv, F_08), f32_mul(cl_lp[k], F_02));
          cl_b[k][cl_i[k]] = f32_add(inp, f32_mul(cl_lp[k], F_084)); cl_i[k] = (cl_i[k] + 1) % CL[k]; oL = f32_add(oL, yv);
          yv = cr_b[k][cr_i[k]]; cr_lp[k] = f32_add(f32_mul(yv, F_08), f32_mul(cr_lp[k], F_02));
          cr_b[k][cr_i[k]] = f32_add(inp, f32_mul(cr_lp[k], F_084)); cr_i[k] = (cr_i[k] + 1) % (CL[k] + 23); oR = f32_add(oR, yv);
        end
        for (int k = 0; k < 2; k++) begin
          bo = al_b[k][al_i[k]]; yy = f32_add(oL ^ 32'h80000000, bo); al_b[k][al_i[k]] = f32_add(oL, f32_mul(bo, F32_HALF)); al_i[k] = (al_i[k] + 1) % AL[k]; oL = yy;
          bo = ar_b[k][ar_i[k]]; yy = f32_add(oR ^ 32'h80000000, bo); ar_b[k][ar_i[k]] = f32_add(oR, f32_mul(bo, F32_HALF)); ar_i[k] = (ar_i[k] + 1) % (AL[k] + 23); oR = yy;
        end
        L = f32_add(L, f32_mul(oL, wet)); R = f32_add(R, f32_mul(oR, wet));
      end
      L = f32_mul(L, f32_mul(mscale, F_32767)); R = f32_mul(R, f32_mul(mscale, F_32767));
      fab_pcm(h, f32_to_i16(f32_clamp_i16(L)), f32_to_i16(f32_clamp_i16(R)));
    end
    frames_rendered += 64'(frames);
    obs();
  endfunction
endmodule
