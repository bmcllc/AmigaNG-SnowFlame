// B.A.S.E. Host Fabric — serviços que o hospedeiro fornece ao backend SystemVerilog do SF-100.
//
// O backend descreve o COMPORTAMENTO da máquina (registradores, máquinas de estado, regras de validação, cálculo de latência,
// decodificação e execução). O hospedeiro realiza os recursos físicos: memória (Memory Host), relógio/eventos (Time Host),
// mídias (DVD, cartão), FPU IEEE (as contas FP32 são feitas pela FPU do host, com o mesmo arredondamento do modelo de
// referência) e as saídas (quadro, áudio, console). Todo acesso passa por estas funções DPI-C, implementadas em fabric/fabric.cpp.
//
// Cada módulo de dispositivo guarda o 'chandle' do seu hospedeiro (recebido em <dev>_bind) e o identificador do dispositivo
// no fabric (FD_*); eventos agendados voltam ao dispositivo pela exportação <dev>_event(tag, a0, a1).
package fabric;
  typedef longint unsigned ps_t;     // tempo simulado em picossegundos
  typedef int unsigned u32_t;
  typedef int unsigned f32_t;        // float IEEE-754 de 32 bits (padrão de bits)

  // Identificadores dos dispositivos no fabric (iguais a sf::FabDev em fabric/fabric.h)
  localparam int FD_BRIDGE = 0, FD_VPUA = 1, FD_VPUB = 2, FD_GPU = 3, FD_BLT = 4, FD_COP = 5, FD_PAULA = 6, FD_VIDEO = 7,
                 FD_TIMERS = 8, FD_DVD = 9, FD_MCARD = 10, FD_PADS = 11, FD_MMU = 12, FD_DBG = 13, FD_HID = 14, FD_CPU = 15;

  // Registradores de estado do barramento que o hospedeiro mantém (o núcleo da CPU e a MMU os consultam a cada acesso)
  localparam int BUS_IRQ_STATUS = 0, BUS_IRQ_MASK = 1, BUS_MMU_ON = 2, BUS_MMU_SBYP = 3, BUS_MMU_FAR = 4, BUS_MMU_FSR = 5,
                 BUS_DCACHE_EN = 6, BUS_ICACHE_EN = 7, BUS_TLB_RR = 8, BUS_FAULT = 9, BUS_FAULT_ADDR = 10, BUS_MMU_BYPASS = 11,
                 BUS_SEM0 = 16;   // BUS_SEM0 + n: semáforo n (0..15)

  // ---------------- Memory Host ----------------
  // Acesso físico cru (sem D-cache, sem MMIO) a DDR/VRAM/ARAM/SRAM e às janelas locais (IMEM/DMEM das VPUs); big-endian.
  import "DPI-C" function bit fab_ok(chandle h, u32_t a, u32_t len);
  import "DPI-C" function byte unsigned fab_r8(chandle h, u32_t a);
  import "DPI-C" function shortint unsigned fab_r16(chandle h, u32_t a);
  import "DPI-C" function u32_t fab_r32(chandle h, u32_t a);
  import "DPI-C" function void fab_w8(chandle h, u32_t a, byte unsigned v);
  import "DPI-C" function void fab_w16(chandle h, u32_t a, shortint unsigned v);
  import "DPI-C" function void fab_w32(chandle h, u32_t a, u32_t v);
  // Transferência de bloco (motor de cópia do hospedeiro): modo 0 = cópia com semântica de memmove, 1 = troca de bytes em
  // palavras de 32 bits, 2 = em palavras de 16 bits (as duas também tolerantes a sobreposição).
  import "DPI-C" function void fab_copy(chandle h, u32_t dst, u32_t src, u32_t len, int mode);
  import "DPI-C" function void fab_fill(chandle h, u32_t dst, u32_t pattern, u32_t len);   // repete o padrão big-endian de 4 B
  // Acesso de dados como o da CPU (D-cache + MMIO de outros dispositivos): usado pelo Copper (MOVE).
  import "DPI-C" context function void fab_cpu_write(chandle h, u32_t a, int size, u32_t v);
  // Porta da CPU na hierarquia de memória (I-cache/D-cache, MMU/TLB, MMIO): busca de instrução, leitura e escrita de dados.
  // super: modo supervisor do acesso (0/1); 2 = mantém o do último acesso (pilha/vetores de exceção, sempre físicos).
  import "DPI-C" context function shortint unsigned fab_fetch16(chandle h, u32_t a, bit sup);
  import "DPI-C" context function u32_t fab_cpu_rd(chandle h, u32_t a, int size, int sup);
  import "DPI-C" context function void fab_cpu_wr(chandle h, u32_t a, int size, u32_t v, int sup);
  import "DPI-C" function u32_t fab_cpu_poll(chandle h);      // bit0 IRQ pendente, bit1 MMU ligada, bit2 supervisor sem tradução
  // Estado arquitetural da CPU publicado para o frontend ao fim de cada passo
  import "DPI-C" function void fab_cpu_state(chandle h, u32_t d0, u32_t d1, u32_t d2, u32_t d3, u32_t d4, u32_t d5, u32_t d6, u32_t d7,
      u32_t a0, u32_t a1, u32_t a2, u32_t a3, u32_t a4, u32_t a5, u32_t a6, u32_t a7, u32_t pc, u32_t sr, u32_t vbr,
      real f0, real f1, real f2, real f3, real f4, real f5, real f6, real f7, bit fn, bit fz, bit fnan, u32_t fpcr, u32_t fpiar,
      longint c0, longint c1, longint c2, longint c3, u32_t macsr, u32_t mask, u32_t cacr, u32_t osp, bit halted, bit stopped, int reason,
      int exc_vec, u32_t exc_pc, u32_t exc_addr, longint unsigned cycles, longint unsigned instrs);
  // Coerência com a D-cache da CPU (operações CACHE_* do ABI) e penalidade de espera da CPU
  import "DPI-C" function void fab_cache(chandle h, u32_t op, u32_t a, u32_t len);
  import "DPI-C" function void fab_stall(chandle h, ps_t t);
  // Árbitro de memória por região: devolve o instante absoluto em que a transferência termina
  import "DPI-C" function ps_t fab_reserve(chandle h, u32_t a, longint unsigned bytes, ps_t earliest);
  // Estado do barramento mantido pelo hospedeiro (BR_*)
  import "DPI-C" function u32_t fab_busreg(chandle h, int key);
  import "DPI-C" function void fab_set_busreg(chandle h, int key, u32_t v);
  import "DPI-C" function void fab_irq(chandle h, u32_t bits);
  // TLB da MMU (64 entradas): {v, perm[3:0], ppn[19:0], vpn[19:0]} empacotados em 64 bits
  import "DPI-C" function longint unsigned fab_tlb_get(chandle h, int i);
  import "DPI-C" function void fab_tlb_set(chandle h, int i, longint unsigned e);

  // ---------------- Time Host ----------------
  import "DPI-C" function ps_t fab_now(chandle h);
  // Agenda fab -> <dev>_event(tag, a0, a1) daqui a 'delay' ps (ordem estável entre eventos do mesmo instante)
  import "DPI-C" function void fab_at(chandle h, int dev, ps_t delay, int tag, longint unsigned a0, longint unsigned a1);

  // ---------------- I/O Host ----------------
  // Mídias (0 = disco do DVD, 1 = flash do cartão de memória): tamanho, cópia mídia -> memória, programação (AND) e apagamento.
  import "DPI-C" function longint unsigned fab_media_size(chandle h, int m);
  import "DPI-C" function void fab_media_read(chandle h, int m, longint unsigned off, u32_t dst, u32_t len);
  import "DPI-C" function void fab_media_and(chandle h, int m, longint unsigned off, u32_t src, u32_t len);
  import "DPI-C" function void fab_media_fill(chandle h, int m, longint unsigned off, byte unsigned v, u32_t len);
  // Entradas do frontend (estado físico de controles, presença de periféricos...) e saídas para ele (console, quadro, áudio)
  import "DPI-C" function longint unsigned fab_in(chandle h, int dev, int key);
  import "DPI-C" context function void fab_out(chandle h, int dev, int key, longint unsigned v);
  // Quadro de vídeo de saída (Display Host): pixel 0x00RRGGBB no índice y*W + x do quadro corrente
  import "DPI-C" function void fab_frame_w(chandle h, u32_t idx, u32_t rgb);
  // Áudio de saída (Audio Host): um quadro estéreo de 16 bits
  import "DPI-C" function void fab_pcm(chandle h, shortint l, shortint r);
  // Mensagem de diagnóstico do dispositivo para o frontend (ex.: motivo do erro de uma lista de comandos da GPU)
  import "DPI-C" function void fab_msg(chandle h, int dev, string m);
  // Observabilidade: espelha contadores/estado do dispositivo nos campos públicos do frontend (trace/estatísticas)
  import "DPI-C" function void fab_obs(chandle h, int dev, int key, longint unsigned v);

  // ---------------- FPU do hospedeiro (FP32 IEEE, arredondamento ao mais próximo, sem FMA) ----------------
  import "DPI-C" pure function f32_t f32_add(f32_t a, f32_t b);
  import "DPI-C" pure function f32_t f32_sub(f32_t a, f32_t b);
  import "DPI-C" pure function f32_t f32_mul(f32_t a, f32_t b);
  import "DPI-C" pure function f32_t f32_div(f32_t a, f32_t b);
  import "DPI-C" pure function f32_t f32_sqrt(f32_t a);
  import "DPI-C" pure function f32_t f32_floor(f32_t a);
  import "DPI-C" pure function f32_t f32_abs(f32_t a);
  import "DPI-C" pure function f32_t f32_cos(f32_t a);
  import "DPI-C" pure function f32_t f32_sin(f32_t a);
  import "DPI-C" pure function f32_t f32_log2(f32_t a);
  import "DPI-C" pure function bit f32_lt(f32_t a, f32_t b);
  import "DPI-C" pure function bit f32_le(f32_t a, f32_t b);
  import "DPI-C" pure function bit f32_eq(f32_t a, f32_t b);
  import "DPI-C" pure function bit f32_finite(f32_t a);
  import "DPI-C" pure function f32_t f32_from_i32(int a);
  import "DPI-C" pure function int f32_to_i32(f32_t a);          // truncamento (cast C)
  import "DPI-C" pure function int f32_lround(f32_t a);          // std::lround
  import "DPI-C" pure function real f32_to_f64(f32_t a);
  import "DPI-C" pure function f32_t f64_to_f32(real a);
  import "DPI-C" pure function f32_t f32_clamp_i16(f32_t a);      // grampo em [-32768, 32767]
  import "DPI-C" pure function shortint f32_to_i16(f32_t a);

  // FPU double do hospedeiro: 0 trunc, 1 floor, 2 ceil, 3 nearbyint (ao par mais próximo), 4 sqrt, 5 fabs
  import "DPI-C" pure function real f64_op(int op, real a);

  localparam f32_t F32_0 = 32'h00000000, F32_1 = 32'h3F800000, F32_HALF = 32'h3F000000, F32_255 = 32'h437F0000;

  // conveniências
  function automatic bit f32_gt(f32_t a, f32_t b); return f32_lt(b, a); endfunction
  function automatic f32_t f32_min(f32_t a, f32_t b); return f32_lt(a, b) ? a : b; endfunction   // a < b ? a : b
  function automatic f32_t f32_max(f32_t a, f32_t b); return f32_gt(a, b) ? a : b; endfunction   // a > b ? a : b
  function automatic f32_t f32_from_u32(u32_t a); return f64_to_f32(real'(a)); endfunction        // (float)u32 = arredondamento único
endpackage
