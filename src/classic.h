// SnowOS Classic: camada de compatibilidade Amiga (OCS/ECS/AGA) do SF-100.
//  - AmigaChipset: registradores $DFF000 (mapeados em CLASSIC_BASE), Copper estilo Amiga, bitplanes (1-8 planos, lores/hires, HAM6/HAM8,
//    EHB, dual playfield, rolagem), 8 sprites, blitter clássico (A/B/C/D, minterms, deslocamento, máscaras, descendente, fill), INTENA/INTREQ,
//    áudio de 4 canais (sobre os canais legados da SnowPaula) e dois CIAs (timers, TOD, joystick/botões).
//  - ClassicSystem: CPU 68000/68020 (Cpu68k) com o mapa de memória Amiga sobre o barramento do SF-100 (chip RAM = DDR baixa).
// A emulação do chipset é por quadro (50/60 Hz): lê os registradores/memória no instante do VBLANK e simula o feixe linha a linha,
// com o Copper agindo na posição exata (linha, ciclo de cor); o resultado vai para um framebuffer ARGB 640x512 na VRAM.
#pragma once
#include "soc.h"
#include "cpu68k.h"
#include "amigadisk.h"

namespace sf {

constexpr u32 CLASSIC_FB_ADDR = abi::VRAM_BASE + 0x01E00000, CLASSIC_FB_PITCH = 640 * 4, CLASSIC_FB_W = 640, CLASSIC_FB_H = 512;

class AmigaChipset : public Device {
 public:
  explicit AmigaChipset(Soc& s) : soc(s) { reset(); }
  enum Level { OCS = 0, ECS = 1, AGA = 2 };
  Level level = AGA;
  bool pal = true;                                   // PAL: 312 linhas a 50 Hz; NTSC: 262 linhas a 59,94 Hz
  bool enabled = false;                              // liga a geração de VBLANK/quadros próprios (start())
  u16 reg[256];                                      // registradores (índice = deslocamento/2)
  u32 fb_addr = CLASSIC_FB_ADDR;                     // framebuffer de saída (ARGB, 640x512)
  u64 frames = 0; u64 blits = 0;
  std::function<void()> on_frame;                    // chamado depois de cada quadro renderizado

  void reset();
  void start();                                      // agenda o VBLANK periódico
  void render_frame();                               // simula um quadro agora (também usado pelos testes)
  int irq_level() const;                             // nível 0..6 da interrupção pendente
  u32 read(u32 off) override;
  void write(u32 off, u32 v) override;
  u16 rd16(u32 off) { return rreg(off & 0x1FE); }
  void write16(u32 off, u16 v) override { wreg(off & 0x1FE, v); }
  u16 read16(u32 off) override { return rreg(off & 0x1FE); }
  bool native16() const override { return true; }

  // CIAs: A (odd bytes em $BFExxx) e B (even bytes em $BFDxxx); 'r' = número do registrador 0..15
  u8 cia_read(int which, int r);
  void cia_write(int which, int r, u8 v);
  // Teclado e mouse (via CIA-A: SDR/ICR bit 3 para o teclado; JOY0DAT, PRA bit 6 e POTGOR para o mouse) e disquete DF0 (ADF)
  bool use_hid = true;                               // a cada VBLANK consome os eventos do dispositivo HID do SF-100
  void key_raw(u8 raw, bool down);                   // código bruto de tecla do Amiga (0..0x7F)
  void pump_hid();                                   // traduz teclas (USB HID -> código Amiga) e movimento/botões do mouse
  static int hid_to_amiga(u8 usage);                 // -1 se não há equivalente
  AmigaDrive df0;
  void insert_adf(std::vector<u8> adf) { df0.adf = std::move(adf); df0.changed = true; }
  void eject_disk() { df0.adf.clear(); df0.changed = true; }
  bool overlay = true;                               // ROM sobreposta em 0 (limpa pelo bit 0 do PRA do CIA A)

  // saídas para o sistema
  u32 beam_vpos() const; u32 beam_hpos() const;
  Ps frame_ps() const { return pal ? 20000000000ull : 16683350000ull; }
  int lines() const { return pal ? 312 : 262; }
  struct Sprite { int vstart = 0, vstop = 0, hstart = 0; bool attach = false; std::vector<u16> a, b; int num = 0; };

 private:
  Soc& soc; Ps frame_start = 0; bool started = false;
  // estado interno
  std::deque<u8> keyq; bool key_pending = false;
  u8 mouse_h = 0, mouse_v = 0, mouse_btn = 0;
  u32 dsk_pt = 0; u16 dsk_len = 0; bool dsk_armed = false; u64 dsk_gen = 0;
  void key_try_send();
  void disk_start();
  u8 disk_status_bits();
  void disk_prb_write(u8 prb);
  u16 intena = 0, intreq = 0, dmacon = 0, adkcon = 0; bool lof = true;
  u32 bpl_ptr[8]; u32 cop1lc = 0, cop2lc = 0, spr_ptr[8]; u32 blt_ptr[4]; u32 aud_lc[4]; u16 aud_len[4], aud_per[4], aud_vol[4];
  u32 pal24[256];                                    // paleta ARGB (OCS/ECS: 32 entradas; AGA: 256)
  // Copper
  u32 cop_pc = 0; bool cop_waiting = false; u16 cop_wait_v = 0, cop_wait_h = 0, cop_mask_v = 0, cop_mask_h = 0; bool cop_halt = false; u32 cop_ir[2];
  struct Cia { u8 sdr = 0; u8 pra = 0, prb = 0, ddra = 0, ddrb = 0, cra = 0, crb = 0, icr = 0, mask = 0; u16 ta_latch = 0xFFFF, tb_latch = 0xFFFF; Ps ta_start = 0, tb_start = 0; u32 tod = 0; u64 gena = 0, genb = 0; bool a_run = false, b_run = false; } cia[2];

  u16 rreg(u32 off); void wreg(u32 off, u16 v);
  void set_intreq(u16 bits) { intreq |= bits; }
  void do_blit();
  void cop_step(int vpos, int cc);
  void cop_write(u32 regoff, u16 v);
  void vblank_event();
  void render_line(int vpos);
  void aud_start(int ch);
  void cia_timer_arm(int w, int t);
  u16 cia_count(int w, int t);
  void load_sprites(std::vector<Sprite>& out);
  void set_color(int idx, u16 v);

  // estado de renderização
  u32 line_data[8][96]; int line_words = 0; u32 pf_ptr[8];
};

class ClassicSystem : public M68kMem {
 public:
  explicit ClassicSystem(Soc& s);
  Soc& soc; AmigaChipset chip; Cpu68k cpu;
  std::vector<u8> rom;                               // imagem da ROM (mapeada em $F80000; vetores de reset sobrepostos em 0 até o OVL cair)
  u32 chip_size = 0x200000;                          // 2 MB de chip RAM (DDR baixa); acima disso (até 0x9FFFFF) é fast RAM
  void load(u32 addr, const std::vector<u8>& bytes);
  void boot(u32 pc, u32 sp);                         // inicia a CPU direto num endereço (sem ROM)
  void boot_rom();                                   // inicia pelos vetores da ROM
  u64 run(Ps limit);                                 // executa a CPU 68k pelo tempo simulado dado; devolve instruções
  bool run_until_stop(Ps limit);                     // até STOP com interrupções mascaradas, halt ou limite
  Ps cycle_ps() const { return 140970; }             // 1 ciclo de 7,09 MHz (PAL)

  u8 read8(u32 a) override; u16 read16(u32 a) override; void write8(u32 a, u8 v) override; void write16(u32 a, u16 v) override;
  int irq_level() override { return chip.irq_level(); }
  void reset_signal() override { chip.reset(); }
};

}  // namespace sf
