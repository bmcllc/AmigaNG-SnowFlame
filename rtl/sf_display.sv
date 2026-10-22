// SnowCopper + controlador de vídeo em SystemVerilog (um módulo, duas janelas de registradores: COP_BASE e VIDEO_BASE).
// O Copper roda em série com a varredura: em cada scanline executa a lista até um WAIT posterior e, em seguida, a linha é
// composta com o estado corrente (4 camadas com rolagem/alfa/janela/ordem, paleta, fundo). Mesmo resultado de
// src/chipset.cpp (que compõe as linhas em paralelo no host, com o mesmo efeito da ordem serial).
// O quadro de saída pertence ao Display Host (fab_frame_w); trocar o modo avisa o frontend (fab_out) para redimensioná-lo.
module sf_display;
  import fabric::*; import sf100_abi::*;
  localparam longint unsigned FRAME_PS = 64'd16666666667;   // 60 Hz
  localparam int OUT_MODE = 0, OUT_VBLANK_HOOK = 1, OUT_FRAME = 2;
  chandle h; int dev, cdev;
  // Copper
  u32_t cop_list, cop_pc; bit cop_enabled, cop_ended; longint unsigned ops_executed;
  // Vídeo
  u32_t l_addr[4], l_pitch[4], l_alpha[4], l_winx[4], l_winy[4], l_flags[4], l_size[4]; int l_scx[4], l_scy[4];
  u32_t bg, ctrl, mode, order, palette[256], frames; int cur_line, W, H; bit started;
  export "DPI-C" function display_bind;
  export "DPI-C" function cop_read;
  export "DPI-C" function cop_write;
  export "DPI-C" function video_read;
  export "DPI-C" function video_write;
  export "DPI-C" function video_start;
  export "DPI-C" function video_vblank;
  export "DPI-C" function video_event;

  function automatic void display_bind(chandle hh, int d, int cd);
    h = hh; dev = d; cdev = cd;
    cop_list = 0; cop_pc = 0; cop_enabled = 0; cop_ended = 1; ops_executed = 0;
    for (int i = 0; i < 4; i++) begin
      l_addr[i] = 0; l_pitch[i] = 0; l_scx[i] = 0; l_scy[i] = 0; l_alpha[i] = 255; l_winx[i] = 0; l_winy[i] = 0; l_flags[i] = 0; l_size[i] = 0;
    end
    bg = 32'hFF000000; ctrl = 0; order = 32'h3210; cur_line = 0; frames = 0; started = 0;
    for (int i = 0; i < 256; i++) palette[i] = 0;
    set_mode(VMODE_240P);
  endfunction

  // ---------------- Copper ----------------
  function automatic u32_t cop_read(u32_t off); return off == COP_LIST ? cop_list : off == COP_CTRL ? (cop_enabled ? 1 : 0) : 0; endfunction
  function automatic void cop_write(u32_t off, u32_t v); if (off == COP_LIST) cop_list = v; else if (off == COP_CTRL) cop_enabled = v[0]; endfunction

  // Executa instruções do Copper até um WAIT posterior à linha atual.
  function automatic void cop_run_line(int line);
    int guard; u32_t w0, w1, a;
    guard = 0;
    while (!cop_ended) begin
      if (guard >= 4096) break; guard++;
      if (!fab_ok(h, cop_pc, 8)) begin cop_ended = 1; return; end
      w0 = fab_r32(h, cop_pc); w1 = fab_r32(h, cop_pc + 4); cop_pc += 8; ops_executed++;
      case (w0 >> 28)
        COPOP_END: begin cop_ended = 1; return; end
        COPOP_WAIT: if (int'((w0 >> 12) & 32'hFFFF) > line) begin cop_pc -= 8; ops_executed--; return; end
        COPOP_MOVE: begin
          a = MMIO_BASE | (w0 & 32'h0FFFFFFF);
          if (!(a >= COP_BASE && a < COP_BASE + 32'h10000)) fab_cpu_write(h, a, 4, w1);
        end
        COPOP_JUMP: cop_pc = w1;
        COPOP_SKIP: if (line >= int'((w0 >> 12) & 32'hFFFF)) cop_pc += 8;
        COPOP_IRQ: fab_irq(h, IRQ_COPPER);
        default: begin cop_ended = 1; return; end
      endcase
    end
  endfunction

  // ---------------- Vídeo ----------------
  function automatic void set_mode(u32_t m);
    mode = m;
    case (m) VMODE_240P: begin W = 320; H = 240; end VMODE_480I, VMODE_480P: begin W = 640; H = 480; end default: begin W = 1280; H = 720; end endcase
    fab_out(h, dev, OUT_MODE, {16'(m), 16'(H), 32'(W)});   // o frontend realoca o quadro (zerado)
  endfunction

  function automatic u32_t video_read(u32_t off);
    int i;
    if (off >= VID_PALETTE && off < VID_PALETTE + 32'h400) return palette[(off - VID_PALETTE) / 4];
    if (off >= VID_LAYER0 && off < VID_LAYER0 + VID_LAYERS * VID_LAYER_STRIDE) begin
      i = int'((off - VID_LAYER0) / VID_LAYER_STRIDE);
      case ((off - VID_LAYER0) % VID_LAYER_STRIDE)
        LR_ADDR: return l_addr[i]; LR_PITCH: return l_pitch[i]; LR_SCX: return u32_t'(l_scx[i]); LR_SCY: return u32_t'(l_scy[i]);
        LR_ALPHA: return l_alpha[i]; LR_WINX: return l_winx[i]; LR_WINY: return l_winy[i]; LR_FLAGS: return l_flags[i]; LR_SIZE: return l_size[i];
        default: return 0;
      endcase
    end
    case (off)
      VID_MODE: return mode; VID_BG: return bg; VID_P0_ADDR: return l_addr[0]; VID_P0_PITCH: return l_pitch[0];
      VID_P1_ADDR: return l_addr[1]; VID_P1_PITCH: return l_pitch[1]; VID_CTRL: return ctrl; VID_FRAME: return frames; VID_LINE: return u32_t'(cur_line);
      VID_ORDER: return order;
      default: return 0;
    endcase
  endfunction
  function automatic void video_write(u32_t off, u32_t v);
    int i;
    if (off >= VID_PALETTE && off < VID_PALETTE + 32'h400) begin palette[(off - VID_PALETTE) / 4] = v; return; end
    if (off >= VID_LAYER0 && off < VID_LAYER0 + VID_LAYERS * VID_LAYER_STRIDE) begin
      i = int'((off - VID_LAYER0) / VID_LAYER_STRIDE);
      case ((off - VID_LAYER0) % VID_LAYER_STRIDE)
        LR_ADDR: l_addr[i] = v; LR_PITCH: l_pitch[i] = v; LR_SCX: l_scx[i] = int'(v); LR_SCY: l_scy[i] = int'(v);
        LR_ALPHA: l_alpha[i] = v < 255 ? v : 255; LR_WINX: l_winx[i] = v; LR_WINY: l_winy[i] = v;
        LR_FLAGS: l_flags[i] = v & 31; LR_SIZE: l_size[i] = v;
        default: ;
      endcase
      return;
    end
    case (off)
      VID_MODE: set_mode(v & 3); VID_BG: bg = v; VID_P0_ADDR: l_addr[0] = v; VID_P0_PITCH: l_pitch[0] = v;
      VID_P1_ADDR: l_addr[1] = v; VID_P1_PITCH: l_pitch[1] = v; VID_CTRL: ctrl = v;
      VID_ORDER: order = v & 32'hFFFF;
      default: ;
    endcase
  endfunction

  function automatic void video_start();
    if (started) return; started = 1;
    fab_at(h, dev, FRAME_PS, 0, 0, 0);              // vblanks periódicos
  endfunction
  function automatic void video_event(int tag, longint unsigned a0, longint unsigned a1);
    video_vblank(); fab_at(h, dev, FRAME_PS, 0, 0, 0);
  endfunction

  function automatic void video_vblank();
    fab_out(h, dev, OUT_VBLANK_HOOK, 0);             // ex.: áudio renderizado até aqui, controles travados
    compose(); frames++; fab_obs(h, dev, 0, 64'(frames));
    if ((ctrl & 4) != 0) fab_irq(h, IRQ_VBLANK);
    fab_out(h, dev, OUT_FRAME, 0);
  endfunction

  function automatic bit enabled(int i); return (i < 2 && (ctrl & (32'd1 << i)) != 0) || (l_flags[i] & LF_ENABLE) != 0; endfunction
  function automatic bit indexed(int i); return (i < 2 && (ctrl & (32'd8 << i)) != 0) || (l_flags[i] & LF_INDEXED) != 0; endfunction
  function automatic u32_t umin(u32_t a, u32_t b); return a < b ? a : b; endfunction

  function automatic void compose();
    bit interlaced; int field;
    cop_pc = cop_list; cop_ended = !cop_enabled;    // restart
    interlaced = mode == VMODE_480I; field = int'(frames & 1);
    for (int y = 0; y < H; y++) begin
      cur_line = y;
      cop_run_line(y);                              // efeitos por scanline: o Copper altera registradores do vídeo
      if (interlaced && (y & 1) != field) continue;
      compose_line(y);
    end
    cur_line = 0;
    fab_obs(h, cdev, 0, ops_executed);
  endfunction

  function automatic void compose_line(int y);
    u32_t out[1280];
    int i, w, hh, wy0, wy1, x0, x1, bpp; longint sy, lo, hi, sx; u32_t ra, s, a, c, o, alpha; byte unsigned pi; bit idx, wrap, add, pre, full, premul;
    for (int x = 0; x < W; x++) out[x] = bg & 32'hFFFFFF;
    // Camada por camada (na ordem): o trecho visível e o índice de origem são calculados uma vez por linha.
    for (int k = 0; k < VID_LAYERS; k++) begin
      i = int'((order >> (4 * k)) & 15); if (i >= VID_LAYERS || !enabled(i)) continue;
      idx = indexed(i); wrap = (l_flags[i] & LF_WRAP) != 0; bpp = idx ? 1 : 4;
      w = (l_size[i] & 32'hFFFF) != 0 ? int'(l_size[i] & 32'hFFFF) : W; hh = (l_size[i] >> 16) != 0 ? int'(l_size[i] >> 16) : H;
      wy0 = int'(l_winy[i] & 32'hFFFF); wy1 = int'(l_winy[i] >> 16); if (wy1 != 0 && (y < wy0 || y >= wy1)) continue;
      sy = longint'(y) + longint'(l_scy[i]);
      if (wrap) begin sy = sy % longint'(hh); if (sy < 0) sy += longint'(hh); end else if (sy < 0 || sy >= longint'(hh)) continue;
      ra = l_addr[i] + u32_t'(sy) * l_pitch[i]; if (!fab_ok(h, ra, u32_t'(w) * u32_t'(bpp))) continue;
      x0 = int'(l_winx[i] & 32'hFFFF); x1 = int'(l_winx[i] >> 16); if (x1 == 0) begin x0 = 0; x1 = W; end
      if (x1 > W) x1 = W;
      add = (l_flags[i] & LF_ADD) != 0; pre = (l_flags[i] & LF_PREMUL) != 0; alpha = l_alpha[i];
      lo = x0 > 0 ? longint'(x0) : 0; hi = longint'(x1);
      if (wrap) begin sx = (lo + longint'(l_scx[i])) % longint'(w); if (sx < 0) sx += longint'(w); end
      else begin
        if (-longint'(l_scx[i]) > lo) lo = -longint'(l_scx[i]);
        if (longint'(w) - longint'(l_scx[i]) < hi) hi = longint'(w) - longint'(l_scx[i]);
        sx = lo + longint'(l_scx[i]);
      end
      if (lo >= hi) continue;
      full = alpha == 255; premul = pre && !add;
      for (int x = int'(lo); x < int'(hi); x++) begin
        if (idx) begin pi = fab_r8(h, ra + u32_t'(sx)); s = palette[pi]; end else s = fab_r32(h, ra + u32_t'(sx) * 4);
        sx++; if (sx == longint'(w)) sx = 0;
        a = full ? s >> 24 : (s >> 24) * alpha / 255; c = out[x];
        if (premul) begin                           // pré-multiplicado: brilhos aditivos (alfa 0) também aparecem
          if ((s & 32'hFFFFFF) == 0 && a == 0) continue;
          o = 0; for (int sh = 0; sh < 24; sh += 8) o |= umin(255, ((((s >> sh) & 255) * alpha + 127) / 255) + ((((c >> sh) & 255) * (255 - a) + 127) / 255)) << sh;
          out[x] = o; continue;
        end
        if (a == 0) continue;
        if (add) begin
          o = 0; for (int sh = 0; sh < 24; sh += 8) o |= umin(255, ((c >> sh) & 255) + ((((s >> sh) & 255) * a + 127) / 255)) << sh;
          out[x] = o; continue;
        end
        if (a == 255) begin out[x] = s & 32'hFFFFFF; continue; end
        o = 0; for (int sh = 0; sh < 24; sh += 8) o |= ((((s >> sh) & 255) * a + ((c >> sh) & 255) * (255 - a) + 127) / 255) << sh;
        out[x] = o;
      end
    end
    for (int x = 0; x < W; x++) fab_frame_w(h, u32_t'(y * W + x), out[x]);
  endfunction
endmodule
