#include "classic.h"

namespace sf {
using namespace abi;

// Deslocamentos dos registradores (como em $DFFxxx)
enum : u32 {
  R_DSKPTH = 0x020, R_DSKPTL = 0x022, R_DSKLEN = 0x024, R_DSKSYNC = 0x07E, R_DSKBYTR = 0x01A, R_DMACONR = 0x002, R_VPOSR = 0x004, R_VHPOSR = 0x006, R_JOY0DAT = 0x00A, R_JOY1DAT = 0x00C, R_ADKCONR = 0x010, R_POTGOR = 0x016, R_INTENAR = 0x01C, R_INTREQR = 0x01E,
  R_BLTCON0 = 0x040, R_BLTCON1 = 0x042, R_BLTAFWM = 0x044, R_BLTALWM = 0x046, R_BLTCPT = 0x048, R_BLTSIZE = 0x058, R_BLTSIZV = 0x05C, R_BLTSIZH = 0x05E,
  R_BLTCMOD = 0x060, R_BLTBMOD = 0x062, R_BLTAMOD = 0x064, R_BLTDMOD = 0x066, R_BLTCDAT = 0x070, R_BLTBDAT = 0x072, R_BLTADAT = 0x074,
  R_COP1LC = 0x080, R_COP2LC = 0x084, R_COPJMP1 = 0x088, R_COPJMP2 = 0x08A, R_DIWSTRT = 0x08E, R_DIWSTOP = 0x090, R_DDFSTRT = 0x092, R_DDFSTOP = 0x094,
  R_DMACON = 0x096, R_INTENA = 0x09A, R_INTREQ = 0x09C, R_ADKCON = 0x09E, R_AUD0 = 0x0A0, R_BPL1PT = 0x0E0,
  R_BPLCON0 = 0x100, R_BPLCON1 = 0x102, R_BPLCON2 = 0x104, R_BPLCON3 = 0x106, R_BPL1MOD = 0x108, R_BPL2MOD = 0x10A, R_BPLCON4 = 0x10C,
  R_SPR0PT = 0x120, R_SPR0POS = 0x140, R_COLOR00 = 0x180, R_DIWHIGH = 0x1E4, R_FMODE = 0x1FC,
};
enum : u16 { DMA_AUD0 = 1, DMA_BPLEN = 0x100, DMA_COPEN = 0x80, DMA_BLTEN = 0x40, DMA_SPREN = 0x20, DMA_DMAEN = 0x200, DMA_BLTPRI = 0x400, DMA_BZERO = 0x2000, DMA_BBUSY = 0x4000 };
enum : u16 { INT_COPER = 0x10, INT_VERTB = 0x20, INT_BLIT = 0x40, INT_DSKBLK = 0x2, INT_DSKSYN = 0x1000, INT_PORTS = 0x8, INT_EXTER = 0x2000, INT_INTEN = 0x4000 };

void AmigaChipset::reset() {
  std::memset(reg, 0, sizeof reg); intena = intreq = dmacon = adkcon = 0; overlay = true; cop_halt = true; cop_waiting = false; cop_pc = 0;
  for (auto& x : bpl_ptr) x = 0; for (auto& x : spr_ptr) x = 0; for (auto& x : blt_ptr) x = 0; for (int i = 0; i < 4; i++) { aud_lc[i] = 0; aud_len[i] = 0; aud_per[i] = 0; aud_vol[i] = 0; }
  for (int i = 0; i < 256; i++) pal24[i] = 0xFF000000u; cop1lc = cop2lc = 0;
  for (auto& c : cia) { c = Cia(); }
  reg[R_DDFSTRT / 2] = 0x38; reg[R_DDFSTOP / 2] = 0xD0; reg[R_DIWSTRT / 2] = 0x2C81; reg[R_DIWSTOP / 2] = 0xF4C1;
}

void AmigaChipset::start() {
  if (started) return; started = true; enabled = true; frame_start = soc.sched.now;
  soc.sched.at(frame_ps(), [this]() { vblank_event(); });
}
void AmigaChipset::vblank_event() {
  frame_start = soc.sched.now; lof = !lof;
  render_frame(); frames++;
  if (use_hid) pump_hid();
  set_intreq(INT_VERTB); cia[0].tod++; cia[1].tod += (u32)lines();
  if (on_frame) on_frame();
  soc.sched.at(frame_ps(), [this]() { vblank_event(); });
}
u32 AmigaChipset::beam_vpos() const { Ps t = soc.sched.now - frame_start; return (u32)(t * (u64)lines() / frame_ps()) % (u32)lines(); }
u32 AmigaChipset::beam_hpos() const { Ps t = soc.sched.now - frame_start; Ps line = frame_ps() / (u64)lines(); return (u32)((t % line) * 228 / line); }

int AmigaChipset::irq_level() const {
  if (!(intena & INT_INTEN)) return 0;
  u16 p = intena & intreq & 0x3FFF;
  if (p & 0x2000) return 6;
  if (p & 0x1800) return 5;
  if (p & 0x0780) return 4;
  if (p & 0x0070) return 3;
  if (p & 0x0008) return 2;
  if (p & 0x0007) return 1;
  return 0;
}

u32 AmigaChipset::read(u32 off) { return ((u32)rreg(off & 0x1FC) << 16) | rreg((off & 0x1FC) + 2); }
void AmigaChipset::write(u32 off, u32 v) { wreg(off & 0x1FC, (u16)(v >> 16)); wreg((off & 0x1FC) + 2, (u16)v); }

u16 AmigaChipset::rreg(u32 off) {
  switch (off) {
    case R_DMACONR: return (u16)((dmacon & 0x07FF) | ((dmacon & DMA_BBUSY)) | ((dmacon & DMA_BZERO)));
    case R_VPOSR: return (u16)((lof ? 0x8000 : 0) | (level == AGA ? 0x2200 : level == ECS ? 0x2000 : 0) | ((beam_vpos() >> 8) & 1));
    case R_VHPOSR: return (u16)(((beam_vpos() & 0xFF) << 8) | (beam_hpos() & 0xFF));
    case R_JOY0DAT: return (u16)((mouse_v << 8) | mouse_h);
    case R_JOY1DAT: {                                          // joystick da porta 2 a partir do controle 0: direita = bit1, esquerda = bit9, baixo = bit0^bit1, cima = bit8^bit9
      u32 b = soc.pads.pad[0].buttons; int right = !!(b & BTN_RIGHT), left = !!(b & BTN_LEFT), up = !!(b & BTN_UP), down = !!(b & BTN_DOWN);
      int bit1 = right, bit9 = left, bit0 = bit1 ^ down, bit8 = bit9 ^ up;
      return (u16)((bit9 << 9) | (bit8 << 8) | (bit1 << 1) | bit0);
    }
    case R_ADKCONR: return adkcon;
    case R_POTGOR: return (u16)(0xFF00 & ~((mouse_btn & 2 ? 0x0400 : 0) | (mouse_btn & 4 ? 0x0100 : 0)));
    case R_INTENAR: return intena; case R_INTREQR: return intreq;
    default: return reg[(off >> 1) & 255];
  }
}

void AmigaChipset::set_color(int idx, u16 v) {
  int bank = level == AGA ? (reg[R_BPLCON3 / 2] >> 13) & 7 : 0; bool loct = level == AGA && (reg[R_BPLCON3 / 2] & 0x200);
  int i = bank * 32 + idx; u32 c = pal24[i]; int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
  int rn = (v >> 8) & 15, gn = (v >> 4) & 15, bn = v & 15;
  if (!loct) { r = rn * 17; g = gn * 17; b = bn * 17; } else { r = (r & 0xF0) | rn; g = (g & 0xF0) | gn; b = (b & 0xF0) | bn; }
  pal24[i] = 0xFF000000u | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

void AmigaChipset::wreg(u32 off, u16 v) {
  auto setclr = [&](u16& r, u16 val, u16 mask) { if (val & 0x8000) r |= (val & mask); else r &= (u16)~(val & mask); };
  if (off >= R_COLOR00 && off < R_COLOR00 + 64) { reg[off / 2] = v; set_color((int)((off - R_COLOR00) / 2), v); return; }
  if (off >= R_BPL1PT && off < R_BPL1PT + 32) { int n = (int)((off - R_BPL1PT) / 4); u32& p = bpl_ptr[n]; if (off & 2) p = (p & 0xFFFF0000u) | (v & 0xFFFE); else p = (p & 0xFFFFu) | ((u32)v << 16); return; }
  if (off >= R_SPR0PT && off < R_SPR0PT + 32) { int n = (int)((off - R_SPR0PT) / 4); u32& p = spr_ptr[n]; if (off & 2) p = (p & 0xFFFF0000u) | (v & 0xFFFE); else p = (p & 0xFFFFu) | ((u32)v << 16); return; }
  if (off >= R_BLTCPT && off < R_BLTCPT + 16) { int n = (int)((off - R_BLTCPT) / 4); u32& p = blt_ptr[n]; if (off & 2) p = (p & 0xFFFF0000u) | (v & 0xFFFE); else p = (p & 0xFFFFu) | ((u32)v << 16); return; }
  if (off >= R_AUD0 && off < R_AUD0 + 0x40) {
    int ch = (int)((off - R_AUD0) / 0x10); int r = (int)((off - R_AUD0) % 0x10);
    switch (r) {
      case 0: aud_lc[ch] = (aud_lc[ch] & 0xFFFFu) | ((u32)v << 16); break; case 2: aud_lc[ch] = (aud_lc[ch] & 0xFFFF0000u) | (v & 0xFFFE); break;
      case 4: aud_len[ch] = v; break; case 6: aud_per[ch] = v; if (dmacon & (1u << ch)) soc.paula.write(PA_LEG0 + ch * 0x10 + 8, v); break;
      case 8: aud_vol[ch] = v & 0x7F; if (dmacon & (1u << ch)) soc.paula.write(PA_LEG0 + ch * 0x10 + 12, 0xC0000000u | std::min<u32>(aud_vol[ch], 64)); break;
      default: break;
    }
    return;
  }
  switch (off) {
    case R_DMACON: {
      u16 old = dmacon; setclr(dmacon, v, 0x07FF);
      for (int ch = 0; ch < 4; ch++) {
        bool was = (old & DMA_DMAEN) && (old & (1u << ch)), now = (dmacon & DMA_DMAEN) && (dmacon & (1u << ch));
        if (now && !was) aud_start(ch); else if (!now && was) soc.paula.write(PA_LEG0 + ch * 0x10 + 12, 0);
      }
      return;
    }
    case R_DSKPTH: dsk_pt = (dsk_pt & 0xFFFFu) | ((u32)v << 16); return;
    case R_DSKPTL: dsk_pt = (dsk_pt & 0xFFFF0000u) | (v & 0xFFFE); return;
    case R_DSKLEN: dsk_len = v; if ((v & 0x8000) && dsk_armed) { dsk_armed = false; disk_start(); } else { dsk_armed = (v & 0x8000) != 0; if (!dsk_armed) dsk_gen++; } return;
    case R_INTENA: setclr(intena, v, 0x7FFF); return;
    case R_INTREQ: setclr(intreq, v, 0x7FFF); return;
    case R_ADKCON: setclr(adkcon, v, 0x7FFF); return;
    case R_COP1LC: cop1lc = (cop1lc & 0xFFFFu) | ((u32)v << 16); return; case R_COP1LC + 2: cop1lc = (cop1lc & 0xFFFF0000u) | (v & 0xFFFE); return;
    case R_COP2LC: cop2lc = (cop2lc & 0xFFFFu) | ((u32)v << 16); return; case R_COP2LC + 2: cop2lc = (cop2lc & 0xFFFF0000u) | (v & 0xFFFE); return;
    case R_COPJMP1: cop_pc = cop1lc; cop_halt = false; cop_waiting = false; return;
    case R_COPJMP2: cop_pc = cop2lc; cop_halt = false; cop_waiting = false; return;
    case R_BLTSIZE: reg[R_BLTSIZE / 2] = v; do_blit(); return;
    case R_BLTSIZH: reg[R_BLTSIZH / 2] = v; do_blit(); return;
    default: reg[(off >> 1) & 255] = v;
  }
}

// ---------------- áudio: canais Amiga sobre os canais legados da SnowPaula ----------------
void AmigaChipset::aud_start(int ch) {
  u32 words = aud_len[ch] ? aud_len[ch] : 65536u, bytes = words * 2, dst = ARAM_BASE + (u32)ch * 0x80000;
  u8* d = soc.bus.raw(dst, std::min<u32>(bytes, 0x80000)); const u8* s = soc.bus.raw(aud_lc[ch], bytes);
  if (d && s) std::memcpy(d, s, std::min<u32>(bytes, 0x80000));
  soc.paula.write(PA_LEG0 + ch * 0x10, (u32)ch * 0x80000); soc.paula.write(PA_LEG0 + ch * 0x10 + 4, std::min<u32>(words, 0x40000));
  soc.paula.write(PA_LEG0 + ch * 0x10 + 8, aud_per[ch] ? aud_per[ch] : 428);
  soc.paula.write(PA_LEG0 + ch * 0x10 + 12, 0xC0000000u | std::min<u32>(aud_vol[ch], 64));
}

// ---------------- blitter ----------------
void AmigaChipset::do_blit() {
  u16 con0 = reg[R_BLTCON0 / 2], con1 = reg[R_BLTCON1 / 2], size = reg[R_BLTSIZE / 2];
  int w = size & 0x3F; if (!w) w = 64; int h = size >> 6; if (!h) h = 1024;
  if (level >= ECS && reg[R_BLTSIZH / 2]) { w = reg[R_BLTSIZH / 2] & 0x7FF; if (!w) w = 2048; h = reg[R_BLTSIZV / 2] & 0x7FFF; if (!h) h = 32768; }
  bool ua = con0 & 0x800, ub = con0 & 0x400, uc = con0 & 0x200, ud = con0 & 0x100; int lf = con0 & 0xFF, ash = con0 >> 12, bsh = con1 >> 12;
  auto mem16 = [&](u32 a) -> u16 { const u8* p = soc.bus.raw(a & ~1u, 2); return p ? rbe16(p) : 0; };
  auto put16 = [&](u32 a, u16 v) { u8* p = soc.bus.raw(a & ~1u, 2); if (p) wbe16(p, v); };
  auto minterm = [&](u16 a, u16 b, u16 c) { u16 d = 0; for (int k = 0; k < 8; k++) if (lf & (1 << k)) { u16 ma = (k & 4) ? a : (u16)~a, mb = (k & 2) ? b : (u16)~b, mc = (k & 1) ? c : (u16)~c; d |= ma & mb & mc; } return d; };
  u16 afwm = reg[R_BLTAFWM / 2], alwm = reg[R_BLTALWM / 2], adat = reg[R_BLTADAT / 2], bdat = reg[R_BLTBDAT / 2], cdat = reg[R_BLTCDAT / 2];
  i16 cmod = (i16)reg[R_BLTCMOD / 2], bmod = (i16)reg[R_BLTBMOD / 2], amod = (i16)reg[R_BLTAMOD / 2], dmod = (i16)reg[R_BLTDMOD / 2];
  u32 pc_ = blt_ptr[0], pb = blt_ptr[1], pa = blt_ptr[2], pd = blt_ptr[3]; bool zero = true;
  Ps dur;
  if (con1 & 1) {                                                // modo linha
    int octant = (con1 >> 2) & 7; bool sud = octant & 4, sul = octant & 2, aul = octant & 1; bool sing = con1 & 2; bool sign = con1 & 0x40;
    i32 err = (i16)(pa & 0xFFFF); if (blt_ptr[2] & 0x10000) err = (i32)(i16)blt_ptr[2]; err = (i32)(i16)(blt_ptr[2] & 0xFFFF);
    int steps = h; int x = ash & 15; u32 ptr = pc_; bool plotted_row = false;
    for (int i = 0; i < steps; i++) {
      u16 a = (u16)(0x8000 >> x), b = ((bdat >> (15 - (bsh & 15))) & 1) ? 0xFFFF : 0, c = mem16(ptr);
      if (!(sing && plotted_row)) { u16 d = minterm(a, b, c); put16(ptr, d); if (d) zero = false; plotted_row = true; }
      bdat = (u16)((bdat << 1) | (bdat >> 15));                  // textura gira a cada passo
      bool minor_step = !sign;
      if (sign) err += bmod; else err += amod; sign = err < 0;
      auto step_x = [&](bool left) { if (left) { x--; if (x < 0) { x = 15; ptr -= 2; } } else { x++; if (x > 15) { x = 0; ptr += 2; } } };
      auto step_y = [&](bool up) { ptr += up ? -(i32)cmod : (i32)cmod; plotted_row = false; };
      if (!sud) { step_x(aul); if (minor_step) step_y(sul); }     // X é o eixo principal: AUL = esquerda, SUL = cima
      else { step_y(aul); if (minor_step) step_x(sul); }          // Y é o eixo principal: AUL = cima, SUL = esquerda
    }
    blt_ptr[0] = ptr; blt_ptr[3] = ptr; blt_ptr[2] = (u32)(u16)err;
    dur = (Ps)steps * 4 * 281900;
  } else {
    bool desc = con1 & 2; bool fill_i = (con1 & 8), fill_e = (con1 & 0x10); bool fill = (fill_i || fill_e) && desc; int step = desc ? -2 : 2; int bytes_per_row_words = w;
    (void)bytes_per_row_words;
    for (int y = 0; y < h; y++) {
      u16 preva = 0, prevb = 0; int fc = (con1 & 4) ? 1 : 0;
      for (int x = 0; x < w; x++) {
        u16 a = ua ? mem16(pa) : adat; if (x == 0) a &= afwm; if (x == w - 1) a &= alwm;
        u16 b = ub ? mem16(pb) : bdat, c = uc ? mem16(pc_) : cdat;
        u16 as, bs;
        if (!desc) { as = (u16)((((u32)preva << 16) | a) >> ash); bs = (u16)((((u32)prevb << 16) | b) >> bsh); }
        else { as = (u16)(((((u32)a << 16) | preva) << ash) >> 16); bs = (u16)(((((u32)b << 16) | prevb) << bsh) >> 16); }
        preva = a; prevb = b;
        u16 d = minterm(as, bs, c);
        if (fill) { u16 o = 0; for (int i = 0; i < 16; i++) { int bit = (d >> i) & 1; int ob = fill_e ? (bit ^ fc) : (bit | fc); o |= (u16)(ob << i); fc ^= bit; } d = o; }
        if (ud) put16(pd, d); if (d) zero = false;
        if (ua) pa += step; if (ub) pb += step; if (uc) pc_ += step; if (ud) pd += step;
      }
      if (ua) pa += amod; if (ub) pb += bmod; if (uc) pc_ += cmod; if (ud) pd += dmod;
    }
    blt_ptr[0] = pc_; blt_ptr[1] = pb; blt_ptr[2] = pa; blt_ptr[3] = pd;
    int nch = (ua ? 1 : 0) + (ub ? 1 : 0) + (uc ? 1 : 0) + (ud ? 1 : 0); dur = (Ps)w * (Ps)h * (Ps)std::max(1, nch) * 281900;
  }
  blits++; dmacon |= DMA_BBUSY; if (zero) dmacon |= DMA_BZERO; else dmacon &= (u16)~DMA_BZERO;
  soc.sched.at(dur, [this]() { dmacon &= (u16)~DMA_BBUSY; set_intreq(INT_BLIT); });
}

// ---------------- Copper (formato Amiga: MOVE, WAIT, SKIP) ----------------
void AmigaChipset::cop_write(u32 regoff, u16 v) {
  if (regoff < 0x20 && level == OCS) return;                       // sem copper danger: registradores baixos protegidos
  if (regoff < 0x40) return;                                       // (CDANG não implementado: protege 000-03E)
  wreg(regoff, v);
}
void AmigaChipset::cop_step(int vpos, int cc) {
  int guard = 0;
  while (!cop_halt && guard++ < 64) {
    if (cop_waiting) {
      u16 bv = (u16)vpos, bh = (u16)cc;
      u16 wv = cop_wait_v & cop_mask_v, wh = cop_wait_h & cop_mask_h; u16 cv = bv & cop_mask_v, ch = bh & cop_mask_h;
      bool reached = cv > wv || (cv == wv && ch >= wh);
      if (!reached) return;                                        // (o bit BFD/espera do blitter é ignorado: o blitter é instantâneo)
      cop_waiting = false;
    }
    const u8* p = soc.bus.raw(cop_pc, 4); if (!p) { cop_halt = true; return; }
    u16 ir1 = rbe16(p), ir2 = rbe16(p + 2); cop_pc += 4;
    if (!(ir1 & 1)) { cop_write(ir1 & 0x1FE, ir2); continue; }     // MOVE
    // WAIT / SKIP
    u16 vp = (ir1 >> 8) & 0xFF, hp = ir1 & 0xFE; u16 vm = 0x80 | ((ir2 >> 8) & 0x7F), hm = ir2 & 0xFE;
    if (vp == 0xFF && hp == 0xFE && ir2 == 0xFFFE) { cop_halt = true; return; }   // fim da lista: WAIT $FFFF,$FFFE
    if (ir2 & 1) {                                                 // SKIP: pula a próxima instrução se o feixe já passou
      u16 cv = (u16)(vpos & vm), ch = (u16)(cc & hm), wv = vp & vm, wh = hp & hm;
      if (cv > wv || (cv == wv && ch >= wh)) cop_pc += 4;
      continue;
    }
    cop_waiting = true; cop_wait_v = vp; cop_wait_h = hp; cop_mask_v = vm; cop_mask_h = hm;
  }
}

// ---------------- sprites ----------------
void AmigaChipset::load_sprites(std::vector<Sprite>& out) {
  out.clear();
  if (!((dmacon & DMA_DMAEN) && (dmacon & DMA_SPREN))) return;
  for (int n = 0; n < 8; n++) {
    u32 p = spr_ptr[n]; int guard = 0;
    while (guard++ < 64) {
      const u8* h = soc.bus.raw(p, 4); if (!h) break;
      u16 pos = rbe16(h), ctl = rbe16(h + 2); p += 4;
      if (pos == 0 && ctl == 0) break;
      Sprite s; s.num = n; s.vstart = (pos >> 8) | ((ctl & 4) ? 0x100 : 0); s.vstop = (ctl >> 8) | ((ctl & 2) ? 0x100 : 0);
      s.hstart = ((pos & 0xFF) << 1) | (ctl & 1);                               // 9 bits (SH8..SH0), mesma unidade do DIWSTRT: 0x81 = borda esquerda padrão
      s.attach = ctl & 0x80;
      for (int y = s.vstart; y < s.vstop; y++) { const u8* d = soc.bus.raw(p, 4); if (!d) break; s.a.push_back(rbe16(d)); s.b.push_back(rbe16(d + 2)); p += 4; }
      out.push_back(s);
    }
  }
}

// ---------------- renderização por quadro ----------------
void AmigaChipset::render_frame() {
  cop_pc = cop1lc; cop_halt = !((dmacon & DMA_DMAEN) && (dmacon & DMA_COPEN)); cop_waiting = false;
  std::vector<Sprite> sprites; load_sprites(sprites);
  u8* fb = soc.bus.raw(fb_addr, CLASSIC_FB_H * CLASSIC_FB_PITCH);
  int nlines = lines();
  for (int vpos = 0; vpos < nlines; vpos++) {
    bool fetched = false; int words = 0; int ham_r = 0, ham_g = 0, ham_b = 0; bool ham_init = false;
    for (int cc = 0; cc < 228; cc++) {
      cop_step(vpos, cc);
      u16 bc0 = reg[R_BPLCON0 / 2]; bool hires = bc0 & 0x8000, ham = bc0 & 0x800, dpf = bc0 & 0x400, lace = bc0 & 4;
      int bpu = (bc0 >> 12) & 7; int np = bpu; if ((bc0 & 0x10) && level == AGA) np = 8; if (level != AGA && np > 6) np = 6;
      u16 diws = reg[R_DIWSTRT / 2], diwp = reg[R_DIWSTOP / 2]; int vstart = diws >> 8, hstart = diws & 0xFF, vstop = diwp >> 8, hstop = (diwp & 0xFF) | 0x100;
      if (!(vstop & 0x80)) vstop |= 0x100;
      int ddfs = reg[R_DDFSTRT / 2] & 0xFE, ddfp = reg[R_DDFSTOP / 2] & 0xFE;
      bool disp_line = (dmacon & DMA_DMAEN) && (dmacon & DMA_BPLEN) && np > 0 && vpos >= vstart && vpos < vstop;
      if (disp_line && !fetched && cc == std::max(ddfs, 0)) {
        words = hires ? ((ddfp - ddfs) / 4 + 2) : ((ddfp - ddfs) / 8 + 1); words = std::max(0, std::min(words, 90));
        for (int n = 0; n < np; n++) { u32 p = bpl_ptr[n]; for (int wd = 0; wd < words; wd++) { const u8* q = soc.bus.raw(p + wd * 2, 2); line_data[n][wd] = q ? rbe16(q) : 0; } }
        fetched = true; line_words = words; ham_init = false;
      }
      // saída: 4 pixels de saída por ciclo de cor
      int out_row = (vpos - 0x2C) * 2 + (lace ? (lof ? 0 : 1) : 0);
      if (!fb || out_row < -1 || out_row >= (int)CLASSIC_FB_H) continue;
      u16 bc1 = reg[R_BPLCON1 / 2], bc2 = reg[R_BPLCON2 / 2];
      for (int j = 0; j < 4; j++) {
        int col = 4 * cc + j - 0x102; if (col < 0 || col >= (int)CLASSIC_FB_W) continue;
        int hx = 2 * cc + (j >> 1); u32 color = pal24[0];
        if (fetched && hx >= hstart && hx < hstop && vpos >= vstart && vpos < vstop) {
          int sub = hires ? (j & 1) : 0; int hxs = hx * 2 + (hires ? (j & 1) : 0);          // posição em unidades da resolução da tela
          int first = hires ? 2 * (2 * ddfs + 17) : (2 * ddfs + 17);
          int pix = hires ? hxs - first : hx - first; (void)sub;
          auto plane_bit = [&](int n, int delay) -> int { int i = pix - delay; if (i < 0 || (i >> 4) >= line_words) return 0; return (line_data[n][i >> 4] >> (15 - (i & 15))) & 1; };
          int d1 = bc1 & 15, d2 = (bc1 >> 4) & 15;
          u32 idx = 0, idx1 = 0, idx2 = 0;
          if (!dpf) { for (int n = 0; n < np; n++) idx |= (u32)plane_bit(n, (n & 1) ? d2 : d1) << n; }
          else { for (int n = 0; n < np; n++) { int b = plane_bit(n, (n & 1) ? d2 : d1); if (n & 1) idx2 |= (u32)b << (n >> 1); else idx1 |= (u32)b << (n >> 1); } }
          u32 pf_color = pal24[0]; int pf_prio_pf = 0; bool pf_opaque = false; bool second_opaque = false;
          int pf1p = bc2 & 7, pf2p = (bc2 >> 3) & 7; bool pf2pri = bc2 & 0x40; int front_p = pf1p, back_p = pf1p; (void)pf_prio_pf;
          if (dpf) {
            u32 c1 = pal24[idx1 & 7], c2 = pal24[8 + (idx2 & 7)];
            bool o1 = idx1 != 0, o2 = idx2 != 0;
            if (pf2pri) { if (o2) { pf_color = c2; pf_opaque = true; front_p = pf2p; if (o1) { second_opaque = true; back_p = pf1p; } } else if (o1) { pf_color = c1; pf_opaque = true; front_p = pf1p; } }
            else { if (o1) { pf_color = c1; pf_opaque = true; front_p = pf1p; if (o2) { second_opaque = true; back_p = pf2p; } } else if (o2) { pf_color = c2; pf_opaque = true; front_p = pf2p; } }
          } else if (ham && (np == 6 || np == 8)) {
            if (!ham_init) { ham_init = true; u32 c0 = pal24[0]; ham_r = (c0 >> 16) & 255; ham_g = (c0 >> 8) & 255; ham_b = c0 & 255; }
            if (hires || (j & 1) == 0) {
              int ctrl = np == 6 ? (idx >> 4) & 3 : (idx >> 6) & 3, data = np == 6 ? (int)(idx & 15) : (int)(idx & 63);
              if (ctrl == 0) { u32 c = pal24[data]; ham_r = (c >> 16) & 255; ham_g = (c >> 8) & 255; ham_b = c & 255; }
              else { int v = np == 6 ? data * 17 : ((data << 2) | (data >> 4)); if (np == 8) { int& comp = ctrl == 1 ? ham_b : ctrl == 2 ? ham_r : ham_g; comp = (comp & 3) | (data << 2); } else { if (ctrl == 1) ham_b = v; else if (ctrl == 2) ham_r = v; else ham_g = v; } }
            }
            pf_color = 0xFF000000u | ((u32)ham_r << 16) | ((u32)ham_g << 8) | (u32)ham_b; pf_opaque = idx != 0 || true; front_p = pf1p;
          } else {
            u32 ix = idx; bool is_ehb = np == 6 && !ham && !dpf && (bc0 & 0x80 || true);
            if (level != AGA && np <= 5) ix &= 31;
            if (is_ehb && np == 6 && ix >= 32) { u32 c = pal24[ix - 32]; pf_color = 0xFF000000u | ((((c >> 16) & 255) >> 1) << 16) | ((((c >> 8) & 255) >> 1) << 8) | ((c & 255) >> 1); }
            else pf_color = pal24[ix & 255];
            pf_opaque = ix != 0; front_p = pf1p;
          }
          color = pf_opaque ? pf_color : pal24[0];
          // sprites
          int best = -1; u32 scolor = 0; int spair = 0;
          for (auto& s : sprites) {
            if (vpos < s.vstart || vpos >= s.vstop || hx < s.hstart || hx >= s.hstart + 16) continue;
            int row = vpos - s.vstart, bit = 15 - (hx - s.hstart); int pa = (s.a[row] >> bit) & 1, pb = (s.b[row] >> bit) & 1; int px = pa | (pb << 1);
            int n = s.num; u32 sc = 0; bool vis = false;
            if (s.attach && (n & 1)) {                                     // sprite ímpar anexado ao par: 4 bits
              int even_n = n - 1; int pa2 = 0, pb2 = 0; for (auto& e : sprites) if (e.num == even_n && vpos >= e.vstart && vpos < e.vstop && hx >= e.hstart && hx < e.hstart + 16) { int r2 = vpos - e.vstart; pa2 = (e.a[r2] >> bit) & 1; pb2 = (e.b[r2] >> bit) & 1; }
              int v4 = px << 2 | (pb2 << 1) | pa2; if (v4) { sc = pal24[16 + v4]; vis = true; }
            } else if (s.attach && !(n & 1)) { vis = false; }             // metade par é desenhada junto com a ímpar
            else if (px) { sc = pal24[16 + 4 * (n >> 1) + px]; vis = true; }
            if (vis && (best < 0 || n < best)) { best = n; scolor = sc; spair = n >> 1; }
          }
          if (best >= 0) {
            if (pf_opaque) { if (spair < front_p) color = scolor; else if (second_opaque && false) color = pf_color; }
            else if (second_opaque) { if (spair < back_p) color = scolor; }
            else color = scolor;
          }
        }
        u32 argb = color | 0xFF000000u;
        for (int rr = 0; rr < (lace ? 1 : 2); rr++) { int row = out_row + rr; if (row >= 0 && row < (int)CLASSIC_FB_H) wbe32(fb + (size_t)row * CLASSIC_FB_PITCH + col * 4, argb); }
      }
    }
    if (fetched) {
      u16 bc0 = reg[R_BPLCON0 / 2]; int bpu = (bc0 >> 12) & 7; int np = bpu; if ((bc0 & 0x10) && level == AGA) np = 8; if (level != AGA && np > 6) np = 6;
      i16 m1 = (i16)reg[R_BPL1MOD / 2], m2 = (i16)reg[R_BPL2MOD / 2];
      for (int n = 0; n < np; n++) bpl_ptr[n] += (u32)(line_words * 2) + (u32)(i32)((n & 1) ? m2 : m1);
    }
  }
}

// ---------------- CIAs ----------------
u16 AmigaChipset::cia_count(int w, int t) {
  Cia& c = cia[w]; bool run = t == 0 ? c.a_run : c.b_run; u16 latch = t == 0 ? c.ta_latch : c.tb_latch; Ps start = t == 0 ? c.ta_start : c.tb_start;
  if (!run) return latch;                                         // parado: mostra o último valor (aproximado: o latch)
  u64 ticks = (u64)((soc.sched.now - start) / 1409700ull);         // período do clock E: 10 ciclos de 7,09 MHz ~ 1,41 us
  u32 period = (u32)latch + 1u; u32 rem = (u32)(ticks % period); return (u16)(latch - rem);
}
void AmigaChipset::cia_timer_arm(int w, int t) {
  Cia& c = cia[w]; u64& gen = t == 0 ? c.gena : c.genb; gen++; u64 g = gen; Ps start = soc.sched.now;
  if (t == 0) c.ta_start = start; else c.tb_start = start;
  u16 latch = t == 0 ? c.ta_latch : c.tb_latch; Ps delay = ((Ps)latch + 1) * 1409700ull;
  soc.sched.at(delay, [this, w, t, g]() {
    Cia& cc = cia[w]; u64& gn = t == 0 ? cc.gena : cc.genb; if (gn != g) return;
    cc.icr |= t == 0 ? 1 : 2; if (cc.mask & (t == 0 ? 1 : 2)) { cc.icr |= 0x80; set_intreq(w == 0 ? INT_PORTS : INT_EXTER); }
    bool oneshot = (t == 0 ? cc.cra : cc.crb) & 8;
    if (oneshot) { (t == 0 ? cc.a_run : cc.b_run) = false; (t == 0 ? cc.cra : cc.crb) &= ~1; } else cia_timer_arm(w, t);
  });
}
u8 AmigaChipset::cia_read(int w, int r) {
  Cia& c = cia[w];
  switch (r) {
    case 0: {
      u8 in = 0xFF;
      if (w == 0) { u32 b = soc.pads.pad[0].buttons; if (b & BTN_A) in &= (u8)~0x80; if ((b & BTN_B) || (mouse_btn & 1)) in &= (u8)~0x40; in &= (u8)(disk_status_bits() | 0xC3); }
      return (u8)((c.pra & c.ddra) | (in & ~c.ddra));
    }
    case 1: return (u8)((c.prb & c.ddrb) | (0xFF & ~c.ddrb));
    case 2: return c.ddra; case 3: return c.ddrb;
    case 4: return (u8)cia_count(w, 0); case 5: return (u8)(cia_count(w, 0) >> 8);
    case 6: return (u8)cia_count(w, 1); case 7: return (u8)(cia_count(w, 1) >> 8);
    case 8: return (u8)c.tod; case 9: return (u8)(c.tod >> 8); case 10: return (u8)(c.tod >> 16);
    case 12: return c.sdr;
    case 13: { u8 v = c.icr; c.icr = 0; return v; }                  // ler o ICR limpa
    case 14: return c.cra; case 15: return c.crb;
    default: return 0;
  }
}
void AmigaChipset::cia_write(int w, int r, u8 v) {
  Cia& c = cia[w];
  switch (r) {
    case 0: c.pra = v; if (w == 0) overlay = !(c.ddra & 1) || (c.pra & 1); break;
    case 1: c.prb = v; if (w == 1) disk_prb_write(v); break;
    case 2: c.ddra = v; if (w == 0) overlay = !(c.ddra & 1) || (c.pra & 1); break;
    case 3: c.ddrb = v; break;
    case 4: c.ta_latch = (u16)((c.ta_latch & 0xFF00) | v); break; case 5: c.ta_latch = (u16)((c.ta_latch & 0xFF) | (v << 8)); if (!(c.cra & 1)) { /* one-shot: carrega ao escrever o byte alto */ } break;
    case 6: c.tb_latch = (u16)((c.tb_latch & 0xFF00) | v); break; case 7: c.tb_latch = (u16)((c.tb_latch & 0xFF) | (v << 8)); break;
    case 12: c.sdr = v; break;
    case 13: if (v & 0x80) c.mask |= (v & 0x1F); else c.mask &= (u8)~(v & 0x1F); break;
    case 14: { bool was = c.cra & 1; if (w == 0 && (c.cra & 0x40) && !(v & 0x40)) { key_pending = false; key_try_send(); } c.cra = v & ~0x10; if ((v & 1) && !was) { c.a_run = true; cia_timer_arm(w, 0); } else if (!(v & 1)) { c.a_run = false; c.gena++; } if (v & 0x10) cia_timer_arm(w, 0); break; }
    case 15: { bool was = c.crb & 1; c.crb = v & ~0x10; if ((v & 1) && !was) { c.b_run = true; cia_timer_arm(w, 1); } else if (!(v & 1)) { c.b_run = false; c.genb++; } if (v & 0x10) cia_timer_arm(w, 1); break; }
    default: break;
  }
}

// ---------------- teclado, mouse e disquete ----------------
void AmigaChipset::key_try_send() {
  if (key_pending || keyq.empty()) return;
  u8 raw = keyq.front(); keyq.pop_front();
  cia[0].sdr = (u8)~(u8)((raw << 1) | (raw >> 7));               // o teclado envia o código girado 1 bit à esquerda e invertido
  cia[0].icr |= 8; key_pending = true;
  if (cia[0].mask & 8) { cia[0].icr |= 0x80; set_intreq(INT_PORTS); }
}
void AmigaChipset::key_raw(u8 raw, bool down) { keyq.push_back((u8)((raw & 0x7F) | (down ? 0 : 0x80))); if (keyq.size() > 64) keyq.pop_front(); key_try_send(); }

int AmigaChipset::hid_to_amiga(u8 u) {
  static const u8 letters[26] = {0x20, 0x35, 0x33, 0x22, 0x12, 0x23, 0x24, 0x25, 0x17, 0x26, 0x27, 0x28, 0x37, 0x36, 0x18, 0x19, 0x10, 0x13, 0x21, 0x14, 0x16, 0x34, 0x11, 0x32, 0x15, 0x31};
  if (u >= 0x04 && u <= 0x1D) return letters[u - 4];
  if (u >= 0x1E && u <= 0x26) return u - 0x1E + 1;               // 1..9
  if (u >= 0x3A && u <= 0x43) return 0x50 + (u - 0x3A);          // F1..F10
  switch (u) {
    case 0x27: return 0x0A; case 0x28: return 0x44; case 0x29: return 0x45; case 0x2A: return 0x41; case 0x2B: return 0x42; case 0x2C: return 0x40;
    case 0x2D: return 0x0B; case 0x2E: return 0x0C; case 0x2F: return 0x1A; case 0x30: return 0x1B; case 0x31: return 0x0D; case 0x33: return 0x29;
    case 0x34: return 0x2A; case 0x35: return 0x00; case 0x36: return 0x38; case 0x37: return 0x39; case 0x38: return 0x3A; case 0x39: return 0x62;
    case 0x49: return 0x5F; case 0x4C: return 0x46; case 0x4F: return 0x4E; case 0x50: return 0x4F; case 0x51: return 0x4D; case 0x52: return 0x4C;
    case 0xE0: case 0xE4: return 0x63; case 0xE1: return 0x60; case 0xE5: return 0x61; case 0xE2: return 0x64; case 0xE6: return 0x65; case 0xE3: return 0x66; case 0xE7: return 0x67;
    default: return -1;
  }
}
void AmigaChipset::pump_hid() {
  for (;;) {
    u32 ev = soc.hid.read(HID_KEY_DATA); if (ev == 0xFFFFFFFFu) break;
    int raw = hid_to_amiga((u8)ev); if (raw >= 0) key_raw((u8)raw, !(ev & HID_KEY_UP));
  }
  mouse_h = (u8)(mouse_h + (int)soc.hid.read(HID_MOUSE_DX)); mouse_v = (u8)(mouse_v + (int)soc.hid.read(HID_MOUSE_DY)); (void)soc.hid.read(HID_MOUSE_WHEEL);
  mouse_btn = (u8)soc.hid.read(HID_MOUSE_BTN);
}

// Disquete: status em CIA-A PRA (bit5 /RDY, bit4 /TK0, bit3 /PROT, bit2 /CHNG); controle em CIA-B PRB (bit7 /MTR, bit6..3 /SELx, bit2 /SIDE, bit1 DIR, bit0 /STEP).
u8 AmigaChipset::disk_status_bits() {
  u8 prb = (u8)((cia[1].prb & cia[1].ddrb) | ~cia[1].ddrb);
  if (prb & 0x08) return 0xFF;                                    // DF0 não selecionado
  u8 v = 0xFF;
  bool ready = df0.present() && df0.motor && soc.sched.now - df0.motor_on >= 500000000000ull;   // 500 ms de rotação até estabilizar
  if (ready) v &= (u8)~0x20;
  if (df0.cyl == 0) v &= (u8)~0x10;
  v &= (u8)~0x08;                                                 // imagens são somente leitura: protegido contra escrita
  if (!df0.present() || df0.changed) v &= (u8)~0x04;
  return v;
}
void AmigaChipset::disk_prb_write(u8 prb) {
  if (prb & 0x08) return;                                         // sem DF0 selecionado
  bool mtr = !(prb & 0x80);
  if (mtr && !df0.motor) df0.motor_on = soc.sched.now;
  df0.motor = mtr;
  bool step_hi = prb & 1;
  if (df0.step_hi && !step_hi) {                                  // borda de descida do /STEP
    if (prb & 2) { if (df0.cyl > 0) df0.cyl--; } else if (df0.cyl < AmigaDrive::CYLS - 1) df0.cyl++;
    if (df0.present()) df0.changed = false;
  }
  df0.step_hi = step_hi;
}
void AmigaChipset::disk_start() {
  u32 words = dsk_len & 0x3FFF;
  if (!(dmacon & DMA_DMAEN) || !(dmacon & 0x10) || (dsk_len & 0x4000) || !df0.present() || !df0.motor || words == 0) return;   // sem DMA de disco / escrita não suportada
  int side = (cia[1].prb & 4) ? 0 : 1;
  std::vector<u16> trk = df0.mfm_track(df0.cyl * 2 + side);
  size_t start = 0; bool sync = adkcon & 0x0400;
  if (sync) {
    u16 sw = reg[R_DSKSYNC / 2]; size_t i = 0; for (; i < trk.size(); i++) if (trk[i] == sw) break;
    if (i == trk.size()) return;                                  // sem sincronismo: nunca conclui (como no hardware)
    start = i; set_intreq(INT_DSKSYN);
  }
  for (u32 i = 0; i < words; i++) { u16 w = trk[(start + i) % trk.size()]; u8 b[2] = {(u8)(w >> 8), (u8)w}; soc.bus.raw_write((dsk_pt + 2 * i) & 0x1FFFFE, b, 2); }
  u64 g = ++dsk_gen;
  soc.sched.at((Ps)words * 32000000ull, [this, g]() { if (g == dsk_gen) set_intreq(INT_DSKBLK); });   // 32 us por palavra MFM (500 kbit/s)
  dsk_pt += 2 * words;
}

// ======================= ClassicSystem =======================
ClassicSystem::ClassicSystem(Soc& s) : soc(s), chip(s), cpu(*this) {
  soc.bus.map_device(CLASSIC_BASE, 0x1000, &chip);
  cpu.m68020 = false;
}

void ClassicSystem::load(u32 addr, const std::vector<u8>& bytes) { soc.bus.raw_write(addr, bytes.data(), (u32)bytes.size()); }
void ClassicSystem::boot(u32 pc, u32 sp) { chip.overlay = false; cpu.reset_regs(); cpu.pc = pc; cpu.a[7] = sp; cpu.ssp = sp; }
void ClassicSystem::boot_rom() { chip.overlay = true; cpu.reset_regs(); cpu.a[7] = read16(0) * 65536u + read16(2); cpu.ssp = cpu.a[7]; cpu.pc = (u32)read16(4) * 65536u + read16(6); }

// mapa de memória Amiga sobre o SF-100
u8 ClassicSystem::read8(u32 a) {
  a &= 0xFFFFFF;
  if (chip.overlay && a < 8 && !rom.empty()) return rom[a];
  if (a < 0xA00000) { const u8* p = soc.bus.raw(a, 1); return p ? *p : 0; }
  if (a >= 0xBFD000 && a < 0xBFF000) { if ((a & 0xF000) == 0xE000 && (a & 1)) return chip.cia_read(0, (a >> 8) & 15); if ((a & 0xF000) == 0xD000 && !(a & 1)) return chip.cia_read(1, (a >> 8) & 15); return 0xFF; }
  if (a >= 0xDFF000 && a < 0xE00000) { u16 v = chip.rd16(a & 0xFFE); return (a & 1) ? (u8)v : (u8)(v >> 8); }
  if (a >= 0xF80000 && !rom.empty()) { u32 o = (a - 0xF80000) % rom.size(); return rom[o]; }
  return 0;
}
u16 ClassicSystem::read16(u32 a) {
  a &= 0xFFFFFF;
  if (a >= 0xDFF000 && a < 0xE00000) return chip.rd16(a & 0xFFE);
  return (u16)((read8(a) << 8) | read8(a + 1));
}
void ClassicSystem::write8(u32 a, u8 v) {
  a &= 0xFFFFFF;
  if (a < 0xA00000) { u8* p = soc.bus.raw(a, 1); if (p) *p = v; return; }
  if (a >= 0xBFD000 && a < 0xBFF000) { if ((a & 0xF000) == 0xE000 && (a & 1)) chip.cia_write(0, (a >> 8) & 15, v); else if ((a & 0xF000) == 0xD000 && !(a & 1)) chip.cia_write(1, (a >> 8) & 15, v); return; }
  if (a >= 0xDFF000 && a < 0xE00000) { u16 cur = chip.rd16(a & 0xFFE); chip.write16(a & 0xFFE, (a & 1) ? (u16)((cur & 0xFF00) | v) : (u16)((cur & 0x00FF) | (v << 8))); return; }
}
void ClassicSystem::write16(u32 a, u16 v) {
  a &= 0xFFFFFF;
  if (a >= 0xDFF000 && a < 0xE00000) { chip.write16(a & 0xFFE, v); return; }
  write8(a, (u8)(v >> 8)); write8(a + 1, (u8)v);
}

u64 ClassicSystem::run(Ps limit) {
  Ps end = soc.sched.now + limit; u64 n = 0;
  while (soc.sched.now < end && !cpu.halted) {
    if (cpu.stopped && chip.irq_level() <= (int)((cpu.sr >> 8) & 7)) { Ps nt = soc.sched.next_time(); if (nt == ~0ull || nt > end) { soc.sched.advance_to(end); break; } soc.sched.advance_to(nt); continue; }
    int c = cpu.step(); n++;
    soc.sched.advance_to(soc.sched.now + (Ps)(c ? c : 4) * cycle_ps());
  }
  return n;
}
bool ClassicSystem::run_until_stop(Ps limit) {
  Ps end = soc.sched.now + limit;
  while (soc.sched.now < end && !cpu.halted) {
    if (cpu.stopped && ((cpu.sr >> 8) & 7) == 7) return true;
    if (cpu.stopped && chip.irq_level() <= (int)((cpu.sr >> 8) & 7)) { Ps nt = soc.sched.next_time(); if (nt == ~0ull) return true; soc.sched.advance_to(std::min(nt, end)); continue; }
    int c = cpu.step(); soc.sched.advance_to(soc.sched.now + (Ps)(c ? c : 4) * cycle_ps());
  }
  return cpu.halted;
}

}  // namespace sf
