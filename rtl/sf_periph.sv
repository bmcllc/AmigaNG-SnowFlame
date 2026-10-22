// Periféricos do SF-100 em SystemVerilog (nível de transação): registradores da MMU, timers, DVD-ROM, cartão de memória,
// controles, teclado/mouse e console de depuração. Mesmo comportamento de src/periph.cpp (modelo de referência).
//
// Convenção dos módulos do backend: cada um exporta a sua API de barramento (<dev>_read/<dev>_write), a entrada dos eventos
// que agendou (<dev>_event) e as operações do frontend (inserir disco, injetar tecla...). Não há relógio: o modelo de tempo
// é "funcional no kick" (efeito imediato + evento de conclusão agendado no Time Host).

// ---------------- Registradores da MMU (a TLB fica no hospedeiro: a CPU a consulta a cada acesso) ----------------
module sf_mmu;
  import fabric::*; import sf100_abi::*;
  chandle h; int dev;
  u32_t ar, tr;
  export "DPI-C" function mmu_bind;
  export "DPI-C" function mmu_read;
  export "DPI-C" function mmu_write;

  function automatic void mmu_bind(chandle hh, int d); h = hh; dev = d; ar = 0; tr = 0; endfunction

  function automatic u32_t mmu_read(u32_t off);
    case (off)
      MMU_CTRL: return fab_busreg(h, BUS_MMU_ON) | (fab_busreg(h, BUS_MMU_SBYP) << 1);
      MMU_FAR: return fab_busreg(h, BUS_MMU_FAR);
      MMU_FSR: return fab_busreg(h, BUS_MMU_FSR);
      MMU_AR: return ar;
      MMU_TR: return tr;
      default: return 0;
    endcase
  endfunction

  // Entrada da TLB empacotada: [19:0] vpn, [39:20] ppn, [43:40] perm, [63] válida
  function automatic bit hit(longint unsigned e, u32_t vpn); return e[63] && u32_t'(e[19:0]) == vpn; endfunction

  function automatic void mmu_write(u32_t off, u32_t v);
    u32_t vpn, rr; longint unsigned e;
    case (off)
      MMU_CTRL: begin fab_set_busreg(h, BUS_MMU_ON, v & 1); fab_set_busreg(h, BUS_MMU_SBYP, (v >> 1) & 1); end
      MMU_FSR: fab_set_busreg(h, BUS_MMU_FSR, 0);
      MMU_AR: ar = v;
      MMU_TR: tr = v;
      MMU_OP: begin
        vpn = ar >> 12;
        if (v == 1) begin
          for (int i = 0; i < 64; i++) begin e = fab_tlb_get(h, i); if (hit(e, vpn)) fab_tlb_set(h, i, {1'b0, e[62:0]}); end   // sem duplicatas
          rr = fab_busreg(h, BUS_TLB_RR); fab_set_busreg(h, BUS_TLB_RR, rr + 1);
          fab_tlb_set(h, int'(rr & 63), {1'b1, 19'd0, 4'(tr & 15), 20'(tr >> 12), 20'(vpn)});
        end else if (v == 2) begin
          for (int i = 0; i < 64; i++) begin e = fab_tlb_get(h, i); fab_tlb_set(h, i, {1'b0, e[62:0]}); end
        end else if (v == 3) begin
          for (int i = 0; i < 64; i++) begin e = fab_tlb_get(h, i); if (hit(e, vpn)) fab_tlb_set(h, i, {1'b0, e[62:0]}); end
        end else if (v == 4) begin
          tr = 0;
          for (int i = 0; i < 64; i++) begin
            e = fab_tlb_get(h, i);
            if (hit(e, vpn)) begin tr = 32'h80000000 | (u32_t'(e[39:20]) << 12) | u32_t'(e[43:40]); break; end
          end
        end
      end
      default: ;
    endcase
  endfunction
endmodule

// ---------------- Timers (2 canais, 1 MHz) ----------------
module sf_timers;
  import fabric::*; import sf100_abi::*;
  chandle h; int dev;
  u32_t load[2], ctrl[2]; ps_t start[2]; longint unsigned gen[2], fired[2];
  export "DPI-C" function timers_bind;
  export "DPI-C" function timers_read;
  export "DPI-C" function timers_write;
  export "DPI-C" function timers_event;

  function automatic void timers_bind(chandle hh, int d);
    h = hh; dev = d;
    for (int i = 0; i < 2; i++) begin load[i] = 0; ctrl[i] = 0; start[i] = 0; gen[i] = 0; fired[i] = 0; end
  endfunction

  function automatic u32_t timers_read(u32_t off);
    int i; longint unsigned el;
    i = int'(off / 16); if (off / 16 > 1) return 0;
    case (off % 16)
      TMR_LOAD: return load[i];
      TMR_CTRL: return ctrl[i];
      TMR_COUNT: begin
        if ((ctrl[i] & 1) == 0 || load[i] == 0) return 0;
        el = (fab_now(h) - start[i]) / 1000000;
        if ((ctrl[i] & 2) != 0) return u32_t'(64'(load[i]) - el % 64'(load[i]));
        return el >= 64'(load[i]) ? 0 : u32_t'(64'(load[i]) - el);
      end
      default: return 0;
    endcase
  endfunction

  function automatic void arm(int i); fab_at(h, dev, 64'(load[i]) * 1000000, i, gen[i], 0); endfunction

  function automatic void timers_write(u32_t off, u32_t v);
    int i;
    i = int'(off / 16); if (off / 16 > 1) return;
    case (off % 16)
      TMR_LOAD: load[i] = v;
      TMR_CTRL: begin ctrl[i] = v & 7; gen[i]++; if ((v & 1) != 0 && load[i] != 0) begin start[i] = fab_now(h); arm(i); end end
      default: ;
    endcase
  endfunction

  function automatic void timers_event(int tag, longint unsigned g, longint unsigned unused);
    if (g != gen[tag]) return;
    fired[tag]++; fab_obs(h, dev, tag, fired[tag]);
    if ((ctrl[tag] & 4) != 0) fab_irq(h, IRQ_TIMER);
    if ((ctrl[tag] & 2) != 0) arm(tag); else ctrl[tag] &= ~32'd1;
  endfunction
endmodule

// ---------------- Leitor de DVD-ROM (setores de 2048 B, spin-up/seek/transferência, DMA para a DDR) ----------------
module sf_dvd;
  import fabric::*; import sf100_abi::*;
  localparam real RATE_MBPS = 5.54;                 // ~4x DVD
  localparam int MEDIA = 0;
  chandle h; int dev;
  u32_t lba, count, dst, status, ctrl, head; bit spinning; ps_t last_io; longint unsigned gen, sectors_read;
  u32_t p_lba, p_n, p_dst; ps_t p_xfer;             // transferência em andamento (vale para o evento de geração 'gen')
  export "DPI-C" function dvd_bind;
  export "DPI-C" function dvd_read;
  export "DPI-C" function dvd_write;
  export "DPI-C" function dvd_event;
  export "DPI-C" function dvd_insert;

  function automatic void dvd_bind(chandle hh, int d);
    h = hh; dev = d; lba = 0; count = 0; dst = 0; status = 0; ctrl = 0; head = 0; spinning = 0; last_io = 0; gen = 0; sectors_read = 0;
  endfunction
  function automatic void dvd_insert(); spinning = 0; head = 0; endfunction   // o frontend trocou a mídia
  function automatic u32_t capacity(); return u32_t'(fab_media_size(h, MEDIA) / 64'(DVD_SECTOR)); endfunction
  function automatic bit empty(); return fab_media_size(h, MEDIA) == 0; endfunction

  function automatic u32_t dvd_read(u32_t off);
    case (off)
      DVD_LBA: return lba; DVD_COUNT: return count; DVD_DST: return dst;
      DVD_STATUS: return status | (empty() ? 0 : 8); DVD_CTRL: return ctrl;
      DVD_CAPACITY: return capacity();
      default: return 0;
    endcase
  endfunction
  function automatic void dvd_write(u32_t off, u32_t v);
    case (off)
      DVD_LBA: lba = v; DVD_COUNT: count = v; DVD_DST: dst = v; DVD_CTRL: ctrl = v;
      DVD_STATUS: status &= ~(v & 6);               // W1C de done/erro
      DVD_CMD: if (v == 1) start();
      default: ;
    endcase
  endfunction

  function automatic void start();
    u32_t cap, seekd; ps_t now, t;
    if ((status & 1) != 0) return;                  // ocupado: comando ignorado
    cap = capacity();
    if (empty() || count == 0 || 64'(lba) + 64'(count) > 64'(cap) || !fab_ok(h, dst, count * DVD_SECTOR) || dst >= DDR_SIZE) begin
      status |= 4; if ((ctrl & 1) != 0) fab_irq(h, IRQ_DVD); return;
    end
    status = 1;
    now = fab_now(h); t = 0;
    if (!spinning || now - last_io > 64'd3000000000000) t += 64'd800000000000;           // spin-up (800 ms) se parado
    seekd = lba > head ? lba - head : head - lba;
    if (seekd > 64) t += 64'd20000000000 + ps_t'($floor(120000000000.0 * real'(seekd) / real'(cap > 1 ? cap : 1)));   // seek
    p_xfer = ps_t'($floor(real'(count) * real'(DVD_SECTOR) / (RATE_MBPS * 1e6) * 1e12));
    gen++; p_lba = lba; p_n = count; p_dst = dst;
    fab_at(h, dev, t, 0, gen, 0);                   // a transferência só disputa a DDR depois do spin-up/seek
  endfunction

  function automatic void dvd_event(int tag, longint unsigned g, longint unsigned unused);
    ps_t now, fin, r;
    if (g != gen) return;
    now = fab_now(h);
    if (tag == 0) begin
      fin = now + p_xfer; r = fab_reserve(h, p_dst, 64'(p_n) * DVD_SECTOR, now); if (r > fin) fin = r;
      fab_at(h, dev, fin - now, 1, g, 0);
      return;
    end
    if (!empty()) begin
      fab_media_read(h, MEDIA, 64'(p_lba) * DVD_SECTOR, p_dst, p_n * DVD_SECTOR);
      fab_cache(h, CACHE_INVAL_RANGE, p_dst, p_n * DVD_SECTOR);
    end
    sectors_read += 64'(p_n); fab_obs(h, dev, 0, sectors_read);
    head = p_lba + p_n; spinning = 1; last_io = now;
    status = 2; if ((ctrl & 1) != 0) fab_irq(h, IRQ_DVD);
  endfunction
endmodule

// ---------------- Cartão de memória (flash de 1 MB: setores de 512 B, apagamento em blocos de 4 KB) ----------------
module sf_mcard;
  import fabric::*; import sf100_abi::*;
  localparam int MEDIA = 1;
  chandle h; int dev;
  u32_t sector, count, addr, status, ctrl;
  u32_t p_cmd, p_s, p_n, p_a;                       // operação em andamento
  export "DPI-C" function mcard_bind;
  export "DPI-C" function mcard_read;
  export "DPI-C" function mcard_write;
  export "DPI-C" function mcard_event;

  function automatic void mcard_bind(chandle hh, int d); h = hh; dev = d; sector = 0; count = 0; addr = 0; status = 0; ctrl = 0; endfunction
  function automatic bit present(); return fab_in(h, dev, 0) != 0; endfunction
  function automatic u32_t capacity(); return u32_t'(fab_media_size(h, MEDIA) / 64'(MC_SECTOR_SIZE)); endfunction

  function automatic u32_t mcard_read(u32_t off);
    case (off)
      MC_SECTOR: return sector; MC_COUNT: return count; MC_ADDR: return addr;
      MC_STATUS: return status | (present() ? 8 : 0); MC_CTRL: return ctrl; MC_CAPACITY: return capacity();
      default: return 0;
    endcase
  endfunction
  function automatic void mcard_write(u32_t off, u32_t v);
    case (off)
      MC_SECTOR: sector = v; MC_COUNT: count = v; MC_ADDR: addr = v; MC_CTRL: ctrl = v;
      MC_STATUS: status &= ~(v & 6);
      MC_CMD: start(v);
      default: ;
    endcase
  endfunction

  function automatic void start(u32_t cmd);
    u32_t cap, n; bit bad; ps_t per, dur, now, fin, r;
    if ((status & 1) != 0) return;
    cap = capacity(); n = cmd == 3 ? MC_ERASE_SECTORS : count;
    bad = !present() || cmd < 1 || cmd > 3 || n == 0 || 64'(sector) + 64'(n) > 64'(cap) || (cmd == 3 && sector % MC_ERASE_SECTORS != 0) ||
          (cmd != 3 && (addr >= DDR_SIZE || !fab_ok(h, addr, n * MC_SECTOR_SIZE)));
    if (bad) begin status |= 4; if ((ctrl & 1) != 0) fab_irq(h, IRQ_MCARD); return; end
    status = 1;
    per = cmd == 1 ? 64'd100000000 : cmd == 2 ? 64'd400000000 : 64'd2000000000;   // ps por setor/bloco: 100 us / 400 us / 2 ms
    dur = per * (cmd == 3 ? 64'd1 : 64'(n));
    now = fab_now(h); fin = now + dur;
    if (cmd != 3) begin r = fab_reserve(h, addr, 64'(n) * MC_SECTOR_SIZE, now); if (r > fin) fin = r; end
    p_cmd = cmd; p_s = sector; p_n = n; p_a = addr;
    if (cmd == 2) fab_cache(h, CACHE_FLUSH_RANGE, addr, n * MC_SECTOR_SIZE);
    fab_at(h, dev, fin - now, 0, 0, 0);
  endfunction

  function automatic void mcard_event(int tag, longint unsigned a0, longint unsigned a1);
    if (p_cmd == 1) begin
      fab_media_read(h, MEDIA, 64'(p_s) * MC_SECTOR_SIZE, p_a, p_n * MC_SECTOR_SIZE); fab_cache(h, CACHE_INVAL_RANGE, p_a, p_n * MC_SECTOR_SIZE);
    end else if (p_cmd == 2) fab_media_and(h, MEDIA, 64'(p_s) * MC_SECTOR_SIZE, p_a, p_n * MC_SECTOR_SIZE);   // flash: só 1 -> 0
    else fab_media_fill(h, MEDIA, 64'(p_s) * MC_SECTOR_SIZE, 8'hFF, MC_ERASE_SECTORS * MC_SECTOR_SIZE);
    status = 2; if ((ctrl & 1) != 0) fab_irq(h, IRQ_MCARD);
  endfunction
endmodule

// ---------------- Controles (4 portas, travadas a cada vblank) ----------------
module sf_pads;
  import fabric::*; import sf100_abi::*;
  chandle h; int dev;
  u32_t ctrl;
  bit l_conn[4]; u32_t l_buttons[4], l_axes[4], l_trig[4];   // estado travado (o físico é do frontend: fab_in)
  export "DPI-C" function pads_bind;
  export "DPI-C" function pads_read;
  export "DPI-C" function pads_write;
  export "DPI-C" function pads_latch;

  function automatic void pads_bind(chandle hh, int d);
    h = hh; dev = d; ctrl = 0;
    for (int i = 0; i < 4; i++) begin l_conn[i] = 0; l_buttons[i] = 0; l_axes[i] = 32'h80808080; l_trig[i] = 0; end
  endfunction
  // chave do frontend por porta: 4*i + {0 conectado, 1 botões, 2 eixos lx|ly|rx|ry, 3 gatilhos lt|rt}
  function automatic void pads_latch();
    for (int i = 0; i < 4; i++) begin
      l_conn[i] = fab_in(h, dev, 4 * i) != 0; l_buttons[i] = u32_t'(fab_in(h, dev, 4 * i + 1));
      l_axes[i] = u32_t'(fab_in(h, dev, 4 * i + 2)); l_trig[i] = u32_t'(fab_in(h, dev, 4 * i + 3));
    end
    if ((ctrl & 1) != 0) fab_irq(h, IRQ_PAD);
  endfunction
  function automatic u32_t pads_read(u32_t off);
    int p;
    if (off == PAD_CTRL) return ctrl;
    if (off >= PAD_PORT0 && off < PAD_PORT0 + 32'h40) begin
      p = int'((off - PAD_PORT0) / 16);
      case ((off - PAD_PORT0) % 16)
        0: return l_conn[p] ? l_buttons[p] : 0;
        4: return l_axes[p];
        8: return l_trig[p];
        default: return l_conn[p] ? 1 : 0;
      endcase
    end
    return 0;
  endfunction
  function automatic void pads_write(u32_t off, u32_t v);
    if (off == PAD_CTRL) begin ctrl = v; return; end
    if (off >= PAD_PORT0 && off < PAD_PORT0 + 32'h40 && (off - PAD_PORT0) % 16 == 8) fab_out(h, dev, int'((off - PAD_PORT0) / 16), 64'(v));   // rumble
  endfunction
endmodule

// ---------------- Teclado + mouse ----------------
module sf_hid;
  import fabric::*; import sf100_abi::*;
  chandle h; int dev;
  u32_t ctrl, status_ovf, mods, btn; u32_t fifo[$]; int dx, dy, wheel;
  export "DPI-C" function hid_bind;
  export "DPI-C" function hid_read;
  export "DPI-C" function hid_write;
  export "DPI-C" function hid_key_event;
  export "DPI-C" function hid_mouse_move;
  export "DPI-C" function hid_mouse_button;
  export "DPI-C" function hid_mouse_wheel;

  function automatic void hid_bind(chandle hh, int d);
    h = hh; dev = d; ctrl = 0; status_ovf = 0; mods = 0; btn = 0; fifo.delete(); dx = 0; dy = 0; wheel = 0;
  endfunction
  function automatic bit kbd(); return fab_in(h, dev, 0) != 0; endfunction
  function automatic bit mouse(); return fab_in(h, dev, 1) != 0; endfunction
  function automatic void note(); if ((ctrl & 1) != 0) fab_irq(h, IRQ_HID); endfunction

  function automatic void hid_key_event(byte unsigned usage, bit down);
    u32_t m;
    if (!kbd()) return;
    if (usage >= 8'hE0 && usage <= 8'hE7) begin
      m = (usage == 8'hE0 || usage == 8'hE4) ? HIDM_CTRL : (usage == 8'hE1 || usage == 8'hE5) ? HIDM_SHIFT :
          (usage == 8'hE2 || usage == 8'hE6) ? HIDM_ALT : HIDM_META;
      if (down) mods |= m; else mods &= ~m;         // simplificação: soltar um dos pares limpa o modificador
    end
    if (fifo.size() >= HID_FIFO_DEPTH) begin status_ovf = 1; return; end
    fifo.push_back(32'(usage) | (down ? 0 : HID_KEY_UP) | (mods << 16)); note();
  endfunction
  function automatic void hid_mouse_move(int x, int y); if (!mouse()) return; dx += x; dy += y; note(); endfunction
  function automatic void hid_mouse_button(int b, bit down);
    if (!mouse()) return; if (down) btn |= 32'd1 << b; else btn &= ~(32'd1 << b); note();
  endfunction
  function automatic void hid_mouse_wheel(int d); if (!mouse()) return; wheel += d; note(); endfunction

  function automatic u32_t hid_read(u32_t off);
    u32_t v;
    case (off)
      HID_CTRL: return ctrl;
      HID_KEY_COUNT: return fifo.size();
      HID_KEY_DATA: begin if (fifo.size() == 0) return 32'hFFFFFFFF; v = fifo.pop_front(); return v; end
      HID_MOUSE_DX: begin v = dx; dx = 0; return v; end
      HID_MOUSE_DY: begin v = dy; dy = 0; return v; end
      HID_MOUSE_BTN: return btn;
      HID_MOUSE_WHEEL: begin v = wheel; wheel = 0; return v; end
      HID_STATUS: return (kbd() ? 1 : 0) | (mouse() ? 2 : 0) | (status_ovf << 2);
      default: return 0;
    endcase
  endfunction
  function automatic void hid_write(u32_t off, u32_t v);
    if (off == HID_CTRL) ctrl = v;
    else if (off == HID_STATUS && (v & 4) != 0) status_ovf = 0;
  endfunction
endmodule

// ---------------- Console de depuração ----------------
module sf_dbgcon;
  import fabric::*; import sf100_abi::*;
  chandle h; int dev;
  export "DPI-C" function dbg_bind;
  export "DPI-C" function dbg_read;
  export "DPI-C" function dbg_write;
  function automatic void dbg_bind(chandle hh, int d); h = hh; dev = d; endfunction
  function automatic u32_t dbg_read(u32_t off); return off == DBG_STATUS ? 1 : 0; endfunction
  function automatic void dbg_write(u32_t off, u32_t v); if (off == DBG_PUTC) fab_out(h, dev, 0, 64'(v)); endfunction
endmodule
