// SF-100 ABI v0.1 - mapa de endereços, registradores e formatos de comando.
// Contrato entre software e hardware. Veja docs/SF100-ABI.md.
#pragma once
#include "common.h"

namespace sf { namespace abi {

// ---------------- Mapa de memória (big-endian) ----------------
constexpr u32 DDR_BASE  = 0x00000000, DDR_SIZE  = 64u << 20;   // 64 MB DDR principal
constexpr u32 VRAM_BASE = 0x10000000, VRAM_SIZE = 32u << 20;   // 32 MB VRAM FlameCore
constexpr u32 ARAM_BASE = 0x20000000, ARAM_SIZE = 4u << 20;    // 4 MB RAM de áudio
constexpr u32 SRAM_BASE = 0x30000000, SRAM_SIZE = 32u << 10;   // SRAM interna do SoC
constexpr u32 MMIO_BASE = 0xF0000000;

constexpr u32 BRIDGE_BASE = 0xF0000000;
constexpr u32 VPUA_BASE   = 0xF0010000;
constexpr u32 VPUB_BASE   = 0xF0020000;
constexpr u32 GPU_BASE    = 0xF0030000;
constexpr u32 BLT_BASE    = 0xF0040000;
constexpr u32 COP_BASE    = 0xF0050000;
constexpr u32 PAULA_BASE  = 0xF0060000;
constexpr u32 VIDEO_BASE  = 0xF0070000;
constexpr u32 TIMER_BASE  = 0xF0080000;
constexpr u32 DVD_BASE    = 0xF0090000;
constexpr u32 MCARD_BASE  = 0xF00A0000;
constexpr u32 PAD_BASE    = 0xF00B0000;
constexpr u32 MMU_BASE    = 0xF00C0000;
constexpr u32 CLASSIC_BASE = 0xF00D0000;   // chipset de compatibilidade Amiga (OCS/ECS/AGA)
constexpr u32 DBG_BASE    = 0xF00E0000;    // console de depuração (saída de caracteres)
constexpr u32 HID_BASE    = 0xF00F0000;    // teclado e mouse (HID)
// Janelas de memória local das VPUs (acessíveis por CPU e DMA)
constexpr u32 VPUA_IMEM = 0xF0100000, VPUA_DMEM = 0xF0110000;
constexpr u32 VPUB_IMEM = 0xF0120000, VPUB_DMEM = 0xF0130000;
constexpr u32 VPU_IMEM_SIZE = 8u << 10, VPU_DMEM_SIZE = 64u << 10;

// ---------------- SnowBridge ----------------
enum : u32 {
  BR_ID = 0x000,        // RO: 'SF10'
  BR_VERSION = 0x004,   // RO: 0x0001 (v0.1)
  BR_TIME_LO = 0x008,   // RO: tempo simulado em ns (baixo; leitura de LO trava HI)
  BR_TIME_HI = 0x00C,
  BR_IRQ_STATUS = 0x010,// R: pendentes; W1C
  BR_IRQ_MASK = 0x014,
  BR_CACHE_OP = 0x020,  // W: executa operação de cache
  BR_CACHE_ADDR = 0x024,
  BR_CACHE_LEN = 0x028,
  BR_CACHE_CTRL = 0x02C,// bit0: cache de dados habilitada
  BR_SEM0 = 0x040,      // 16 semáforos: leitura = P (decrementa se >0, devolve valor antigo); escrita = V (soma valor, mín 1)
  BR_DMA0 = 0x100,      // 4 canais x 0x20
};
enum : u32 { CACHE_FLUSH_ALL = 1, CACHE_INVAL_ALL = 2, CACHE_FLUSH_INVAL_ALL = 3,
             CACHE_FLUSH_RANGE = 4, CACHE_INVAL_RANGE = 5, CACHE_FLUSH_INVAL_RANGE = 6,
             ICACHE_INVAL_ALL = 7, ICACHE_INVAL_RANGE = 8 };
// Canal DMA: offsets relativos a BR_DMA0 + 0x20*ch
enum : u32 { DMA_SRC = 0x00, DMA_DST = 0x04, DMA_LEN = 0x08, DMA_FLAGS = 0x0C, DMA_DESC = 0x10,
             DMA_STATUS = 0x14, DMA_CTRL = 0x18 };
enum : u32 { DMAF_FILL = 1, DMAF_IRQ = 2, DMAF_BSWAP32 = 4, DMAF_BSWAP16 = 8 };
enum : u32 { DMAS_BUSY = 1, DMAS_DONE = 2, DMAS_ERROR = 4 };
enum : u32 { DMAC_START_REGS = 1, DMAC_START_CHAIN = 2 };
// Descritor DMA em memória (20 bytes, big-endian): src, dst, len, flags, next (0 = fim)
constexpr u32 DMA_DESC_SIZE = 20;

// Bits de IRQ
enum : u32 { IRQ_DMA0 = 1u << 0, IRQ_DMA1 = 1u << 1, IRQ_DMA2 = 1u << 2, IRQ_DMA3 = 1u << 3,
             IRQ_VPUA = 1u << 4, IRQ_VPUB = 1u << 5, IRQ_GPU = 1u << 6, IRQ_BLT = 1u << 7,
             IRQ_VBLANK = 1u << 8, IRQ_AUDIO = 1u << 9,
             IRQ_TIMER = 1u << 10, IRQ_DVD = 1u << 11, IRQ_MCARD = 1u << 12, IRQ_PAD = 1u << 13,
             IRQ_COPPER = 1u << 14, IRQ_HID = 1u << 15 };
constexpr int IRQ_LEVEL = 4;       // autovetor nível 4 -> vetor 28
constexpr int VEC_IRQ4 = 28;

// ---------------- SnowVector ----------------
enum : u32 {
  VPU_CTRL = 0x00,   // W: bit0 START (enfileira job), bit1 RESET, bit2 IRQ ao concluir
  VPU_STATUS = 0x04, // bit0 BUSY, bit1 DONE, bit2 FAULT, bits 8..15 = jobs na fila
  VPU_PC = 0x08,     // PC inicial do próximo job (em instruções)
  VPU_CYCLES = 0x0C, // RO: ciclos do último job
  VPU_FLOPS = 0x10,  // RO: FLOPs do último job
  VPU_S0 = 0x20,     // 8 registradores escalares iniciais do próximo job (s0..s7)
  VPU_FAULTPC = 0x40,
  VPU_TOTAL_CYCLES = 0x44,
};
enum : u32 { VPUC_START = 1, VPUC_RESET = 2, VPUC_IRQ = 4 };
enum : u32 { VPUS_BUSY = 1, VPUS_DONE = 2, VPUS_FAULT = 4 };
constexpr int VPU_QUEUE_DEPTH = 8;

// ---------------- FlameCore ----------------
enum : u32 {
  GPU_CMD_ADDR = 0x00, GPU_CMD_LEN = 0x04 /*palavras*/, GPU_CTRL = 0x08 /*bit0 START, bit1 IRQ*/,
  GPU_STATUS = 0x0C, GPU_HSR = 0x10 /*1 = eliminação de superfícies ocultas ativa*/,
  GPU_STAT_TRIS_IN = 0x20, GPU_STAT_TRIS_DRAWN = 0x24, GPU_STAT_PIX_COVERED = 0x28,
  GPU_STAT_PIX_SHADED = 0x2C, GPU_STAT_TILES = 0x30, GPU_STAT_CYCLES = 0x34,
  GPU_STAT_TEXFETCH = 0x38, GPU_STAT_VRAM_WRITE = 0x3C,
};
enum : u32 { GPUS_BUSY = 1, GPUS_DONE = 2, GPUS_ERROR = 4 };
constexpr int GPU_QUEUE_DEPTH = 4;
constexpr int TILE = 32;
// Comandos gráficos: palavra 0 = (opcode << 24) | (nparams), seguida de nparams palavras
enum : u32 {
  GC_END = 0x00, GC_SET_RT = 0x01 /*addr,pitch,w|h<<16*/, GC_CLEAR = 0x02 /*argb,depth(f32),flags*/,
  GC_VERTEX_BASE = 0x03 /*addr*/, GC_SET_TEX = 0x04 /*unit,addr,log2w|log2h<<8|fmt<<16|flags<<24, nlevels*/,
  GC_STATE = 0x05 /*flags*/, GC_BLEND = 0x06 /*src,dst*/, GC_FOG = 0x07 /*argb,start,end*/,
  GC_STENCIL = 0x08 /*func,ref,mask,sfail,zfail,zpass*/, GC_TEXENV = 0x09 /*unit,mode*/,
  GC_SCISSOR = 0x0A /*x,y,w,h*/, GC_SET_PALETTE = 0x0B /*unit,addr,count(<=256): copia entradas ARGB8888 da memória*/, GC_DRAW_INDEXED = 0x10 /*idxaddr,count,prim*/,
  GC_DRAW_ARRAYS = 0x11 /*first,count,prim*/, GC_FLUSH = 0x1E /*renderiza o que foi acumulado*/,
};
enum : u32 { ST_DEPTH_TEST = 1u << 0, ST_DEPTH_WRITE = 1u << 1, ST_BLEND = 1u << 2,
             ST_CULL_CW = 1u << 3, ST_CULL_CCW = 1u << 4, ST_FOG = 1u << 5, ST_TEX0 = 1u << 6,
             ST_TEX1 = 1u << 7, ST_STENCIL = 1u << 8, ST_BILINEAR = 1u << 9,
             ST_DEPTH_FUNC_SHIFT = 12 /*3 bits: 0 LESS,1 LEQUAL,2 GREATER,3 GEQUAL,4 EQUAL,5 NOTEQUAL,6 ALWAYS,7 NEVER*/ };
enum : u32 { CLR_COLOR = 1, CLR_DEPTH = 2 };
enum : u32 { BF_ZERO = 0, BF_ONE, BF_SRC_ALPHA, BF_INV_SRC_ALPHA, BF_DST_ALPHA, BF_INV_DST_ALPHA };
enum : u32 { TF_ARGB8888 = 0, TF_RGB565 = 1, TF_ARGB4444 = 2, TF_DXT1 = 3,
             TF_DXT3 = 4 /*SFTC3: alfa explícito de 4 bits, bloco de 16 B*/, TF_DXT5 = 5 /*SFTC5: alfa interpolado, bloco de 16 B*/,
             TF_RGBA5551 = 6, TF_L8 = 7 /*luminância*/, TF_A8 = 8 /*só alfa (RGB = 1)*/, TF_LA88 = 9 /*alfa no byte alto*/,
             TF_CI8 = 10 /*índice de 8 bits na paleta da unidade (GC_SET_PALETTE)*/, TF_CI4 = 11 /*4 bits, nibble alto = texel par*/,
             TF_VQ = 12 /*por mip: livro de 256 x (2x2 RGB565: TL,TR,BL,BR) = 2048 B + 1 índice por bloco 2x2, linha a linha*/,
             TF_ETC1 = 13 /*ETC1 (Khronos), 4 bpp, RGB opaco; blocos de 8 B*/ };
constexpr u32 TF_COUNT = 14;
enum : u32 { TFLAG_CLAMP = 1, TFLAG_MIPS = 2 };
enum : u32 { TE_MODULATE = 0, TE_REPLACE = 1, TE_DECAL = 2, TE_ADD = 3 };
enum : u32 { PRIM_TRIS = 0, PRIM_STRIP = 1, PRIM_FAN = 2 };
enum : u32 { SOP_KEEP = 0, SOP_ZERO, SOP_REPLACE, SOP_INCR, SOP_DECR, SOP_INVERT };
// Vértice pós-transformação (48 bytes, 3 x vec4 big-endian float): pos(x,y,z,1/w), cor(r,g,b,a), tex(u,v,fog,-)
constexpr u32 VERTEX_SIZE = 48;

// ---------------- SnowBlitter ----------------
enum : u32 {
  BLT_OP = 0x00, BLT_SRC = 0x04, BLT_SRC_PITCH = 0x08, BLT_DST = 0x0C, BLT_DST_PITCH = 0x10,
  BLT_W = 0x14, BLT_H = 0x18, BLT_COLOR = 0x1C /*fill / color key*/, BLT_MASK = 0x20, BLT_MASK_PITCH = 0x24,
  BLT_MINTERM = 0x28, BLT_CTRL = 0x2C /*bit0 START bit1 IRQ*/, BLT_STATUS = 0x30,
};
enum : u32 { BOP_COPY = 0, BOP_FILL = 1, BOP_COLORKEY = 2, BOP_ALPHA = 3, BOP_MINTERM = 4, BOP_CONV565 = 5,
              BOP_SPRITES = 6 /*SRC = lista, COLOR = nº de sprites (<= 1024), DST/DST_PITCH/W/H = superfície ARGB e seu recorte*/ };
// Entrada da lista de sprites (16 bytes, big-endian): src(u32, ARGB8888), src_pitch(u16), flags(u16), x(i16), y(i16), w(u16), h(u16).
constexpr u32 SPRITE_ENTRY_SIZE = 16, SPRITE_MAX = 1024;
enum : u32 { SPF_HFLIP = 1, SPF_VFLIP = 2, SPF_OPAQUE = 4 /*copia sem mistura alfa*/ };

// ---------------- SnowCopper ----------------
enum : u32 { COP_LIST = 0x00, COP_CTRL = 0x04 /*bit0 enable*/ };
enum : u32 { COPOP_END = 0, COPOP_WAIT = 1, COPOP_MOVE = 2, COPOP_JUMP = 3,
             COPOP_SKIP = 4 /*pula a próxima instrução se linha >= y*/, COPOP_IRQ = 5 /*levanta IRQ_COPPER*/ };
inline u32 cop_skip(u32 y) { return (COPOP_SKIP << 28) | (y << 12); }
inline u32 cop_irq() { return COPOP_IRQ << 28; }
inline u32 cop_wait(u32 y, u32 x = 0) { return (COPOP_WAIT << 28) | (y << 12) | (x & 0xFFF); }
inline u32 cop_move(u32 addr) { return (COPOP_MOVE << 28) | (addr & 0x0FFFFFFF); }

// ---------------- Video ----------------
enum : u32 {
  VID_MODE = 0x00 /*0=240p 1=480i 2=480p 3=720p*/, VID_BG = 0x04, VID_P0_ADDR = 0x08, VID_P0_PITCH = 0x0C,
  VID_P1_ADDR = 0x10, VID_P1_PITCH = 0x14, VID_CTRL = 0x18 /*bit0 P0 en, bit1 P1 en, bit2 vblank irq*/,
  VID_FRAME = 0x1C, VID_LINE = 0x20,
  VID_PALETTE = 0x400 /*256 entradas ARGB (0x400..0x7FF); CTRL bit3/bit4 = plano 0/1 indexado 8bpp*/,
};
// Camadas (4): ADDR/PITCH/scroll/alfa/janela/flags. Os registradores P0/P1 acima são apelidos das camadas 0 e 1.
// Pixel da tela (x,y) lê a camada em (x+SCX, y+SCY); com LF_WRAP o mapa dá a volta no tamanho SIZE (w | h<<16; 0 = tamanho da tela).
// WINX = x0 | x1<<16 e WINY = y0 | y1<<16 recortam a camada (x1 == 0: sem recorte). ALPHA 0..255 multiplica o alfa do pixel.
// VID_ORDER: nibbles com a ordem de desenho de baixo para cima (padrão 0x3210). O Copper pode alterar tudo isso por scanline.
constexpr int VID_LAYERS = 4;
enum : u32 { VID_ORDER = 0x24, VID_LAYER0 = 0x40, VID_LAYER_STRIDE = 0x40 };
enum : u32 { LR_ADDR = 0x00, LR_PITCH = 0x04, LR_SCX = 0x08, LR_SCY = 0x0C, LR_ALPHA = 0x10, LR_WINX = 0x14, LR_WINY = 0x18, LR_FLAGS = 0x1C, LR_SIZE = 0x20 };
enum : u32 { LF_ENABLE = 1, LF_INDEXED = 2, LF_WRAP = 4, LF_ADD = 8 /*mistura aditiva*/ };
enum : u32 { VCTRL_P0 = 1, VCTRL_P1 = 2, VCTRL_VBLIRQ = 4, VCTRL_P0_IDX = 8, VCTRL_P1_IDX = 16 };
enum : u32 { VMODE_240P = 0, VMODE_480I = 1, VMODE_480P = 2, VMODE_720P = 3 };

// ---------------- MMU (SnowFire) ----------------
// TLB de 64 entradas, páginas de 4 KB, preenchida por software (como a MMU do ColdFire V4e).
// Endereços >= 0xF0000000 (MMIO) nunca são traduzidos. Falha -> exceção de erro de acesso (vetor 2).
enum : u32 { MMU_CTRL = 0x00 /*bit0 enable, bit1 supervisor sem tradução (só o modo usuário passa pela TLB)*/, MMU_FAR = 0x04 /*endereço virtual da última falha*/,
             MMU_FSR = 0x08 /*1 = miss, 2 = proteção; bits 4..6 = acesso (R=1,W=2,X=4)*/,
             MMU_AR = 0x10, MMU_TR = 0x14 /*PA | perm[3:0]*/, MMU_OP = 0x18 /*1 insere, 2 inval. tudo, 3 inval. página AR, 4 sonda AR->TR (bit31 = achou)*/ };
enum : u32 { MP_R = 1, MP_W = 2, MP_X = 4, MP_SUPER = 8 };

// ---------------- Timers / DVD-ROM / Cartão de memória / Controles ----------------
enum : u32 { TMR_LOAD = 0x00 /*us*/, TMR_CTRL = 0x04 /*bit0 enable, bit1 periódico, bit2 IRQ*/, TMR_COUNT = 0x08 /*us restantes (RO)*/ };  // 2 timers x 0x10
enum : u32 { DVD_CMD = 0x00 /*1 = ler*/, DVD_LBA = 0x04, DVD_COUNT = 0x08 /*setores de 2048 B*/, DVD_DST = 0x0C, DVD_STATUS = 0x10 /*bit0 busy, bit1 done, bit2 erro, bit3 disco*/,
             DVD_CTRL = 0x14 /*bit0 IRQ*/, DVD_CAPACITY = 0x18 /*setores*/ };
constexpr u32 DVD_SECTOR = 2048;
enum : u32 { MC_CMD = 0x00 /*1 ler, 2 programar (AND), 3 apagar bloco de 4 KB*/, MC_SECTOR = 0x04, MC_COUNT = 0x08, MC_ADDR = 0x0C /*DDR*/,
             MC_STATUS = 0x10 /*bit0 busy, bit1 done, bit2 erro, bit3 cartão*/, MC_CTRL = 0x14 /*bit0 IRQ*/, MC_CAPACITY = 0x18 };
constexpr u32 MC_SECTOR_SIZE = 512, MC_ERASE_SECTORS = 8;
enum : u32 { PAD_CTRL = 0x00 /*bit0 IRQ a cada leitura latched*/, PAD_PORT0 = 0x10 /*4 portas x 0x10: BUTTONS, AXES(lx,ly,rx,ry), TRIGGERS(l,r), STATUS(bit0 conectado) ; W em +8 = rumble*/ };
enum : u32 { BTN_UP = 1, BTN_DOWN = 2, BTN_LEFT = 4, BTN_RIGHT = 8, BTN_A = 16, BTN_B = 32, BTN_X = 64, BTN_Y = 128, BTN_L = 256, BTN_R = 512, BTN_START = 1024, BTN_SELECT = 2048 };

// ---------------- Teclado e mouse (HID) ----------------
// Teclado: FIFO de 64 eventos; código = USB HID usage ID (0x04 = A ... 0x2C = espaço, 0xE0..0xE7 = modificadores).
// Mouse: deslocamentos acumulados (a leitura zera) e estado dos botões.
enum : u32 { HID_CTRL = 0x00 /*bit0 IRQ a cada evento*/, HID_KEY_COUNT = 0x04 /*eventos na FIFO (RO)*/,
             HID_KEY_DATA = 0x08 /*R: retira um evento = código | soltou<<8 | mods<<16; 0xFFFFFFFF se vazia*/,
             HID_MOUSE_DX = 0x10 /*i32, leitura zera*/, HID_MOUSE_DY = 0x14 /*i32, leitura zera*/, HID_MOUSE_BTN = 0x18 /*bit0 esq, bit1 dir, bit2 meio*/,
             HID_MOUSE_WHEEL = 0x1C /*i32, leitura zera*/, HID_STATUS = 0x20 /*bit0 teclado, bit1 mouse, bit2 estouro da FIFO (W1C)*/ };
enum : u32 { HIDM_SHIFT = 1, HIDM_CTRL = 2, HIDM_ALT = 4, HIDM_META = 8 };
constexpr u32 HID_FIFO_DEPTH = 64, HID_KEY_UP = 0x100;

// ---------------- SnowFS (sistema de arquivos somente leitura do DVD) ----------------
// Setor 0: magic 'SFFS', versão, nº de arquivos, LBA do diretório, nº de setores do diretório, total de setores.
// Diretório: entradas de 64 B (nome[48] com NUL, LBA, tamanho, flags, reservado). Caminhos planos ("dir/arq"). Dados alinhados a setor.
constexpr u32 SFFS_MAGIC = 0x53464653, SFFS_VERSION = 1, SFFS_ENTRY_SIZE = 64, SFFS_NAME_MAX = 48;

// ---------------- Console de depuração ----------------
enum : u32 { DBG_PUTC = 0x00 /*W: caractere*/, DBG_STATUS = 0x04 /*R: 1 = pronto*/ };

// ---------------- SnowPaula ----------------
constexpr int PAULA_VOICES = 64;
enum : u32 {
  PA_MASTER = 0x000 /*volume 0..256*/, PA_REVERB_WET = 0x004 /*0..256*/, PA_SEQ_ADDR = 0x008,
  PA_SEQ_LEN = 0x00C /*eventos*/, PA_SEQ_CTRL = 0x010 /*bit0 start, bit1 loop*/, PA_SEQ_STATUS = 0x014,
  PA_DSP_LOAD = 0x018 /*pico de carga do DSP em permil. do orçamento (RO)*/, PA_DSP_DROPS = 0x01C /*vozes-quadro descartadas por estouro (W limpa)*/,
  PA_LEG0 = 0x040 /*4 canais legados x 0x10: ADDR,LEN(palavras),PERIOD,VOL(0..64)|ctrl bit31=on bit30=loop*/,
  PA_VOICE0 = 0x100 /*64 vozes x 0x40*/,
};
enum : u32 {
  PV_ADDR = 0x00 /*offset em ARAM*/, PV_LEN = 0x04 /*amostras*/, PV_LOOP_START = 0x08, PV_LOOP_LEN = 0x0C,
  PV_STEP = 0x10 /*Q16.16 relativo a 48 kHz*/, PV_VOL_L = 0x14 /*0..256*/, PV_VOL_R = 0x18,
  PV_CTRL = 0x1C /*bit0 on, bit1 loop, bits2-3 fmt (0 PCM8, 1 PCM16, 2 ADPCM), bit4 3D, bit5 lowpass*/,
  PV_REV_SEND = 0x20 /*0..256*/, PV_LP_COEF = 0x24 /*0..256*/, PV_X = 0x28, PV_Y = 0x2C, PV_Z = 0x30 /*f32 bits*/,
  PV_ADPCM_PRED = 0x34, PV_ADPCM_IDX = 0x38,
};
// Evento do sequenciador (16 bytes): tempo (amostras @48k), voz, registrador (offset PV_*), valor
constexpr u32 PA_SEQ_EVENT_SIZE = 16;
constexpr u32 PAULA_RATE = 48000;

}}  // namespace sf::abi
