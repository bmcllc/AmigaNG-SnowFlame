/* snowflame.h - definições de hardware do SF-100 (espelho de src/abi.h) e intrínsecos do snowcc */
#ifndef SNOWFLAME_H
#define SNOWFLAME_H

#define REG(a) (*(volatile unsigned *)(a))
#define REG16(a) (*(volatile unsigned short *)(a))
#define REG8(a) (*(volatile unsigned char *)(a))
#define NULL ((void *)0)

/* ---- mapa de memória ---- */
#define DDR_BASE   0x00000000
#define VRAM_BASE  0x10000000
#define ARAM_BASE  0x20000000
#define SRAM_BASE  0x30000000
#define MMIO_BASE  0xF0000000

#define BRIDGE_BASE  0xF0000000
#define VPUA_BASE    0xF0010000
#define VPUB_BASE    0xF0020000
#define GPU_BASE     0xF0030000
#define BLT_BASE     0xF0040000
#define COP_BASE     0xF0050000
#define PAULA_BASE   0xF0060000
#define VIDEO_BASE   0xF0070000
#define TIMER_BASE   0xF0080000
#define DVD_BASE     0xF0090000
#define MCARD_BASE   0xF00A0000
#define PAD_BASE     0xF00B0000
#define MMU_BASE     0xF00C0000
#define MMU_CTRL     0x00
#define MMU_FAR      0x04
#define MMU_FSR      0x08
#define MMU_AR       0x10
#define MMU_TR       0x14
#define MMU_OP       0x18
#define MP_R 1
#define MP_W 2
#define MP_X 4
#define MP_SUPER 8
#define VPUA_IMEM  0xF0100000
#define VPUA_DMEM  0xF0110000
#define VPUB_IMEM  0xF0120000
#define VPUB_DMEM  0xF0130000
#define DBG_BASE   0xF00E0000
#define HID_BASE   0xF00F0000
#define CLASSIC_BASE 0xF00D0000

/* ---- IRQ ---- */
#define IRQ_DMA0     (1u << 0)
#define IRQ_DMA1     (1u << 1)
#define IRQ_DMA2     (1u << 2)
#define IRQ_DMA3     (1u << 3)
#define IRQ_VPUA     (1u << 4)
#define IRQ_VPUB     (1u << 5)
#define IRQ_GPU      (1u << 6)
#define IRQ_BLT      (1u << 7)
#define IRQ_VBLANK   (1u << 8)
#define IRQ_AUDIO    (1u << 9)
#define IRQ_TIMER    (1u << 10)
#define IRQ_DVD      (1u << 11)
#define IRQ_MCARD    (1u << 12)
#define IRQ_PAD      (1u << 13)
#define IRQ_COPPER   (1u << 14)
#define IRQ_HID      (1u << 15)
/* ---- DVD-ROM ---- */
#define DVD_CMD      0x00
#define DVD_LBA      0x04
#define DVD_COUNT    0x08
#define DVD_DST      0x0C
#define DVD_STATUS   0x10
#define DVD_CTRL     0x14
#define DVD_CAPACITY 0x18
#define DVD_SECTOR   2048

/* ---- cartão de memória ---- */
#define MC_CMD       0x00
#define MC_SECTOR    0x04
#define MC_COUNT     0x08
#define MC_ADDR      0x0C
#define MC_STATUS    0x10
#define MC_CTRL      0x14
#define MC_CAPACITY  0x18

/* ---- teclado e mouse (HID) ---- */
#define HID_CTRL        0x00
#define HID_KEY_COUNT   0x04
#define HID_KEY_DATA    0x08
#define HID_MOUSE_DX    0x10
#define HID_MOUSE_DY    0x14
#define HID_MOUSE_BTN   0x18
#define HID_MOUSE_WHEEL 0x1C
#define HID_STATUS      0x20

#define IRQ_BIT(mask) irq_bit_of(mask)

/* ---- SnowBridge ---- */
#define BR_ID              0x000
#define BR_VERSION         0x004
#define BR_TIME_LO         0x008
#define BR_TIME_HI         0x00C
#define BR_IRQ_STATUS      0x010
#define BR_IRQ_MASK        0x014
#define BR_CACHE_OP        0x020
#define BR_CACHE_ADDR      0x024
#define BR_CACHE_LEN       0x028
#define BR_CACHE_CTRL      0x02C
#define BR_SYS_CTRL        0x0F0
#define BR_SEM0            0x040
#define BR_DMA0            0x100
#define CACHE_FLUSH_ALL    1
#define CACHE_INVAL_ALL    2
#define CACHE_FLUSH_INVAL_ALL 3
#define CACHE_FLUSH_RANGE  4
#define CACHE_INVAL_RANGE  5
#define CACHE_FLUSH_INVAL_RANGE 6
#define ICACHE_INVAL_ALL   7
#define ICACHE_INVAL_RANGE 8
#define DMA_SRC            0x00
#define DMA_DST            0x04
#define DMA_LEN            0x08
#define DMA_FLAGS          0x0C
#define DMA_DESC           0x10
#define DMA_STATUS         0x14
#define DMA_CTRL           0x18

#define DMAF_FILL 1
#define DMAF_IRQ 2
#define DMAF_BSWAP32 4
#define DMAF_BSWAP16 8
#define DMAS_BUSY 1
#define DMAS_DONE 2
#define DMAS_ERROR 4
#define DMAC_START_REGS 1
#define DMAC_START_CHAIN 2

/* ---- Timers ---- */
#define TMR_LOAD           0x00
#define TMR_CTRL           0x04
#define TMR_COUNT          0x08

/* ---- SnowVector ---- */
#define VPU_CTRL           0x00
#define VPU_STATUS         0x04
#define VPU_PC             0x08
#define VPU_CYCLES         0x0C
#define VPU_FLOPS          0x10
#define VPU_S0             0x20
#define VPU_FAULTPC        0x40
#define VPU_TOTAL_CYCLES   0x44

#define VPUC_START 1
#define VPUC_RESET 2
#define VPUC_IRQ 4
#define VPUS_BUSY 1
#define VPUS_DONE 2
#define VPUS_FAULT 4

/* ---- FlameCore ---- */
#define GPU_CMD_ADDR       0x00
#define GPU_CMD_LEN        0x04
#define GPU_CTRL           0x08
#define GPU_STATUS         0x0C
#define GPU_HSR            0x10
#define GC_END             0x00
#define GC_SET_RT          0x01
#define GC_CLEAR           0x02
#define GC_VERTEX_BASE     0x03
#define GC_SET_TEX         0x04
#define GC_STATE           0x05
#define GC_BLEND           0x06
#define GC_FOG             0x07
#define GC_STENCIL         0x08
#define GC_TEXENV          0x09
#define GC_SCISSOR         0x0A
#define GC_SET_PALETTE     0x0B
#define GC_DRAW_INDEXED    0x10
#define GC_DRAW_ARRAYS     0x11
#define GC_FLUSH           0x1E

#define GPUS_BUSY 1
#define GPUS_DONE 2
#define GPUS_ERROR 4
#define ST_DEPTH_TEST 1
#define ST_DEPTH_WRITE 2
#define ST_BLEND 4
#define ST_CULL_CW 8
#define ST_CULL_CCW 16
#define ST_FOG 32
#define ST_TEX0 64
#define ST_TEX1 128
#define ST_STENCIL 256
#define ST_BILINEAR 512
#define CLR_COLOR 1
#define CLR_DEPTH 2
#define BF_ZERO 0
#define BF_ONE 1
#define BF_SRC_ALPHA 2
#define BF_INV_SRC_ALPHA 3
#define TFLAG_CLAMP 1
#define TFLAG_MIPS 2
#define TE_MODULATE 0
#define TE_REPLACE 1
#define TE_DECAL 2
#define TE_ADD 3
#define PRIM_TRIS 0
#define PRIM_STRIP 1
#define PRIM_FAN 2
#define TF_ARGB8888 0
#define TF_RGB565 1
#define TF_ARGB4444 2
#define TF_DXT1 3
#define TF_DXT3 4
#define TF_DXT5 5
#define TF_RGBA5551 6
#define TF_L8 7
#define TF_A8 8
#define TF_LA88 9
#define TF_CI8 10
#define TF_CI4 11
#define TF_VQ 12
#define TF_ETC1 13
#define VERTEX_SIZE 48

/* ---- SnowBlitter / SnowCopper ---- */
#define BLT_OP             0x00
#define BLT_SRC            0x04
#define BLT_SRC_PITCH      0x08
#define BLT_DST            0x0C
#define BLT_DST_PITCH      0x10
#define BLT_W              0x14
#define BLT_H              0x18
#define BLT_COLOR          0x1C
#define BLT_MASK           0x20
#define BLT_MASK_PITCH     0x24
#define BLT_MINTERM        0x28
#define BLT_CTRL           0x2C
#define BLT_STATUS         0x30
#define BOP_COPY           0
#define BOP_FILL           1
#define BOP_COLORKEY       2
#define BOP_ALPHA          3
#define BOP_MINTERM        4
#define BOP_CONV565        5
#define BOP_SPRITES        6
#define SPRITE_ENTRY_SIZE  16
#define SPF_HFLIP          1
#define SPF_VFLIP          2
#define SPF_OPAQUE         4
#define COP_LIST           0x00
#define COP_CTRL           0x04
#define COPOP_END          0
#define COPOP_WAIT         1
#define COPOP_MOVE         2
#define COPOP_JUMP         3
#define COPOP_SKIP         4
#define COPOP_IRQ          5

/* ---- Vídeo ---- */
#define VID_MODE           0x00
#define VID_BG             0x04
#define VID_P0_ADDR        0x08
#define VID_P0_PITCH       0x0C
#define VID_P1_ADDR        0x10
#define VID_P1_PITCH       0x14
#define VID_CTRL           0x18
#define VID_FRAME          0x1C
#define VID_LINE           0x20
#define VID_ORDER          0x24
#define VID_LAYER0         0x40
#define VID_LAYER_STRIDE   0x40
#define VID_PALETTE        0x400
#define LR_ADDR            0x00
#define LR_PITCH           0x04
#define LR_SCX             0x08
#define LR_SCY             0x0C
#define LR_ALPHA           0x10
#define LR_WINX            0x14
#define LR_WINY            0x18
#define LR_FLAGS           0x1C
#define LR_SIZE            0x20
#define LF_ENABLE          1
#define LF_INDEXED         2
#define LF_WRAP            4
#define LF_ADD             8
#define VCTRL_P0           1
#define VCTRL_P1           2
#define VCTRL_VBLIRQ       4
#define VCTRL_P0_IDX       8
#define VCTRL_P1_IDX       16
#define VMODE_240P         0
#define VMODE_480I         1
#define VMODE_480P         2
#define VMODE_720P         3

/* ---- Controles ---- */
#define PAD_CTRL           0x00
#define PAD_PORT0          0x10
#define BTN_UP             1
#define BTN_DOWN           2
#define BTN_LEFT           4
#define BTN_RIGHT          8
#define BTN_A              16
#define BTN_B              32
#define BTN_X              64
#define BTN_Y              128
#define BTN_L              256
#define BTN_R              512
#define BTN_START          1024
#define BTN_SELECT         2048

/* ---- SnowPaula ---- */
#define PA_MASTER          0x000
#define PA_REVERB_WET      0x004
#define PA_SEQ_ADDR        0x008
#define PA_SEQ_LEN         0x00C
#define PA_SEQ_CTRL        0x010
#define PA_SEQ_STATUS      0x014
#define PA_DSP_LOAD        0x018
#define PA_DSP_DROPS       0x01C
#define PA_LEG0            0x040
#define PA_VOICE0          0x100
#define PV_ADDR            0x00
#define PV_LEN             0x04
#define PV_LOOP_START      0x08
#define PV_LOOP_LEN        0x0C
#define PV_STEP            0x10
#define PV_VOL_L           0x14
#define PV_VOL_R           0x18
#define PV_CTRL            0x1C
#define PV_REV_SEND        0x20
#define PV_LP_COEF         0x24
#define PV_X               0x28
#define PV_Y               0x2C
#define PV_Z               0x30

/* macros de comandos do Copper */
#define COP_WAIT(y)  ((COPOP_WAIT << 28) | ((y) << 12))
#define COP_MOVE(a)  ((COPOP_MOVE << 28) | ((a) & 0x0FFFFFFF))
#define COP_SKIP(y)  ((COPOP_SKIP << 28) | ((y) << 12))

/* ---- intrínsecos do snowcc ---- */
void __halt(void);
void __nop(void);
void __stop(int sr);                       /* constante */
void __trap(int n);                        /* constante 0..15 */
int __trapcall(int n, int a, int b, int c);/* d0=a, d1=b, d2=c; trap #n; devolve d0 */
void __set_vbr(unsigned v);
void __set_cacr(unsigned v);
unsigned __get_sp(void);
void __set_sp(unsigned v);
unsigned __get_sr(void);
void __set_sr(unsigned v);
unsigned __get_usp(void);
void __set_usp(unsigned v);

#endif
