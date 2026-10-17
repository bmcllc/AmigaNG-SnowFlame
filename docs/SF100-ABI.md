# Amiga SnowFlame SF-100 — ABI v0.1 (emulador em C++)

Fonte de verdade: `src/abi.h`. Tudo é **big-endian** na memória. Tempo simulado em picossegundos; cada dispositivo executa
funcionalmente no instante do *kick* e o evento de conclusão (com IRQ opcional) é agendado após a latência em ciclos.

## Mapa de memória
| Região | Base | Tamanho |
|---|---|---|
| DDR | 0x00000000 | 64 MB |
| VRAM (FlameCore) | 0x10000000 | 32 MB |
| ARAM (áudio) | 0x20000000 | 4 MB |
| SRAM do SoC | 0x30000000 | 32 KB |
| MMIO | 0xF0000000 | Bridge F0000000, VPU-A F0010000, VPU-B F0020000, GPU F0030000, Blitter F0040000, Copper F0050000, Paula F0060000, Vídeo F0070000 |
| IMEM/DMEM VPU-A | F0100000 / F0110000 | 8 KB / 64 KB |
| IMEM/DMEM VPU-B | F0120000 / F0130000 | 8 KB / 64 KB |

## SnowBridge
- `ID`='SF10', `VERSION`, relógio em ns, `IRQ_STATUS` (W1C) / `IRQ_MASK`. IRQ nível 4 (autovetor 28).
  Bits: DMA0-3, VPU-A, VPU-B, GPU, BLT, VBLANK, AUDIO.
- 16 semáforos: leitura = P (decrementa se > 0, devolve o valor antigo); escrita = V.
- 4 canais DMA: registradores (src,dst,len,flags,desc,status,ctrl) ou cadeia de descritores de 20 bytes
  (src,dst,len,flags,next). Flags: FILL, IRQ, BSWAP32, BSWAP16. Modelo de banda: DDR 2128 MB/s, VRAM 5312, ARAM 800,
  outros 4000, mais 150 ns de setup.
- **Cache de dados da CPU não é coerente** (32 KB, 4 vias, linhas de 16 B, write-back, só DDR). Antes de DMA/VPU/GPU lerem
  dados escritos pela CPU: `CACHE_FLUSH_*`. Antes da CPU ler dados escritos por eles: `CACHE_INVAL_*`
  (registradores `CACHE_OP/ADDR/LEN`).

## CPU SnowFire (ColdFire V4e, 266 MHz)
Interpretador com codificações reais: move/aritmética/lógica/shifts/bits, MULx.L, DIVx.L, MOVEM.L, LINK/UNLK, Scc, TRAP,
RTE (frame ColdFire), STOP, MOVEC (VBR 0x801, OTHER_A7 0x800, CACR 0x002), IRQ autovetorado.
- **Endereçamento:** curto (d8,An,Xn*SF) e **formato completo** (bd de 0/16/32 bits, supressão de base/índice, indireção de memória pré/pós-indexada com od).
- **Usuário/supervisor:** SR[S] troca A7 com o SP inativo (`other_sp`; MOVE USP, MOVEC 0x800). RTE/STOP/MOVE to SR/MOVEC/MOVE USP/HALT/MOVE from SR/FSAVE/FRESTORE são
  privilegiadas (vetor 8 em modo usuário). TRAP/IRQ/exceções entram em supervisor; o quadro (formato/vetor/SR + PC) fica na pilha de supervisor.
- **Inteiros extras:** ADDX/SUBX/NEGX (registrador), MOV3Q, MVS/MVZ, SATS, FF1, BYTEREV, BITREV, TAS, MOVE CCR,Dn, TPF.
- **FPU (double):** FMOVE/FADD/FSUB/FMUL/FDIV/FSQRT/FABS/FNEG/FINT/FINTRZ, variantes de precisão FS*/FD*, FCMP/FTST, FBcc com as 16 condições IEEE,
  FScc, FMOVE de/para FPCR/FPSR/FPIAR, FMOVEM estático, FSAVE/FRESTORE (quadro de 16 B `{0x41000000, FPCR, FPSR, FPIAR}`; quadro nulo = reset). FPCR: PREC[7:6]=1 arredonda para
  simples; RND[5:4] (0 próx., 1 zero, 2 -inf, 3 +inf) vale para FINT e conversões para inteiro.
- **eMAC (EMAC_B, codificação do ColdFire Family Programmer's Reference Manual rev. 3):** MAC/MSAC .W/.L com metades (.U/.L), escala <<1/>>1, forma com carga (MAC com <ea>&MASK → Rw), MAAAC/MSAAC/MASAC/MSSAC, MOVE/MOVCLR ACCn, ACCy→ACCx, MOVE ACCext01/23, MOVE MACSR/MASK/MACSR→CCR. Acumuladores de 48 bits (ACC + 16 de extensão); PAV0-3 em MACSR[11:8] (sticky); EV/V/N/Z conforme o manual. Inteiro: produto de 32 bits (overflow se não couber); fracionário: produto alinhado com 2^0 no bit 39 do acumulador, MOVE ACC lê ACC[39:8]. Saturação e arredondamento (OMC, S/U, R/T) só na leitura (MOVE ACC/MOVCLR), como no pseudocódigo do manual. Interpretações: arredondamento = meio para cima; MASK aplica-se só ao endereço do operando de memória.
- **Temporização (tabela, não ciclo a ciclo):** 1 ciclo base + 1 por acesso a dados; Bcc com predição estática (para trás = tomado; acerto 1-2 ciclos, erro 6); BRA/BSR/JMP/JSR 2; RTS 3;
  interlock de geração de endereço (+1 ao usar An como base logo depois de escrevê-lo); MUL.L 3, DIV.L 19, MAC 2; FADD/FSUB 2, FMUL 3, FDIV 22, FSQRT 27, FINT 2 ciclos extras;
  miss de D-cache/I-cache 60 ns + linha. Não há superescalar nem pipeline detalhado.
- **Caches:** D-cache de 32 KB write-back, não coerente. **I-cache de 32 KB guarda os bytes das linhas** (sem snoop): instruções reescritas só valem depois de `CACHE_FLUSH_RANGE` (D-cache) +
  `ICACHE_INVAL_RANGE/ALL` (o carregador do host, `Bus::raw_write`/`Soc::load`, invalida sozinho). `BR_CACHE_CTRL`: bit0 D-cache, bit1 desliga a I-cache.
- **MMU:** acessos de dados e buscas que cruzam página traduzem cada página (páginas físicas podem ser não contíguas); escrita que cruza para página sem tradução/permissão falha sem escrita parcial.

## SnowVector-128 (2 unidades, 150 MHz)
SIMD FP32×4, 32 registradores vetoriais, 8 escalares, IMEM 8 KB, DMEM 64 KB. Palavra de 32 bits:
`op[31:26] a[25:21] b[20:16] c[15:11] low[10:0]` (tipo I: imm16 em [15:0]); swizzle de 8 bits no operando b.
Opcodes: HALT VADD VSUB VMUL VMAC VDOT3 VMIN VMAX VLD VST SETA SADD SCMP BRLT BRNE BRA VMOV VRCP VRSQ VABS VDOT4 NOP VFRC DMAIN DMAOUT DMAWAIT SIGNAL.
Fila de 8 jobs, watchdog de 4M ciclos, falta em VLD/VST desalinhado ou fora da DMEM. Pico 8 FLOP/ciclo (1,2 GFLOPS/unidade).
- **DMA local** (a VPU busca e entrega dados sem a CPU): `DMAIN sDmem, sEnd, sLen` (barramento → DMEM), `DMAOUT sEnd, sDmem, sLen` (DMEM → barramento), `DMAWAIT`.
  Endereços e tamanho múltiplos de 4; lado externo em DDR/VRAM/ARAM/SRAM/janelas; a cópia é funcional na instrução e o término vem do árbitro de memória
  (16 ciclos de preparo + banda da região, transferências em série). `HALT` só conclui o job depois do DMA pendente. **Não enxerga a D-cache da CPU**
  (flush antes de DMAIN de dados escritos pela CPU). Falta (`VPUS_FAULT`) fora da DMEM, desalinhado ou endereço externo inválido.
- **`SIGNAL n`** faz V() no semáforo n (0..15) do SnowBridge no instante da instrução: encadeia VPU → CPU sem IRQ (a CPU faz P lendo `BR_SEM0+4n`).
- **`VFRC vd, vb`**: parte fracionária por via (x − floor(x)).
- Registradores extras: `VPU_DMA_BYTES` (0x48, acumulado) e `VPU_JOBS_DONE` (0x4C).
Montador/desmontador de texto: `vpu_assemble`, `vpu_disasm`.

## FlameCore (GPU de tiles, 200 MHz)
Lista de comandos: palavra 0 = `(opcode<<24)|nparams`, seguida de nparams palavras (ver `GC_*`). `GC_BLEND` aceita 2 parâmetros (os mesmos fatores para cor e alfa) ou 4 (`src, dst, src_alfa, dst_alfa`): com `ONE, INV_SRC_ALPHA` no alfa e um alvo limpo com alfa 0, o resultado sai pré-multiplicado para uma camada `LF_PREMUL`. Vértice pós-transformação de
48 bytes (pos x,y,z,1/w; cor r,g,b,a; tex u,v,fog). Binning por tile 32×32, funções de aresta 28.4 com regra top-left,
interpolação com correção de perspectiva, HSR (sombreamento adiado — saída idêntica ao modo imediato), Z, stencil, blend,
fog, scissor, 2 TMUs, mipmaps, bilinear, clamp/repeat. Formatos: ARGB8888, RGB565, ARGB4444, DXT1, DXT3, DXT5, RGBA5551, L8, A8, LA88, CI4/CI8 (paleta da unidade via `GC_SET_PALETTE unit,addr,count`), VQ (livro de 256 blocos 2x2 RGB565 de 2048 B por mip + 1 índice por bloco) e ETC1 (Khronos, 4 bpp).
**Desvio:** DXT1/3/5 usam palavras big-endian (SFTC1/3/5). DXT3 = alfa de 4 bits (2 B por linha, texel x no nibble x), DXT5 = 2 bytes de alfa + 48 bits de índices (texel i nos bits 3i..3i+2, lidos como inteiro big-endian), blocos de 16 B com o alfa antes da cor; DXT3/5 sempre decodificam a cor em 4 cores. Tempo = max(ciclos de cálculo, ciclos de memória). Tiles não tocados
não são escritos.

## Blitter / Copper / Vídeo
- Blitter: COPY, FILL, COLORKEY, ALPHA, MINTERM (ex.: 0xCA cookie-cut), CONV565; semântica memmove; fila e IRQ.
  **SPRITES** (`BOP_SPRITES`): `BLT_SRC` = lista, `BLT_COLOR` = nº de sprites (<= 1024), `BLT_DST/DST_PITCH/W/H` = superfície ARGB e recorte.
  Entrada de 16 B: `src u32, src_pitch u16, flags u16, x i16, y i16, w u16, h u16` (flags: `SPF_HFLIP`, `SPF_VFLIP`, `SPF_OPAQUE`).
  Mistura alfa na ordem da lista, recorte nas bordas; custo 8 ciclos/sprite + 3 ciclos/pixel visível.
- Copper: `END/WAIT/MOVE/JUMP/SKIP/IRQ` por scanline (`cop_wait`, `cop_move`, `cop_skip(y)` = pula a próxima instrução se linha >= y, `cop_irq()` = IRQ_COPPER); MOVE vale para qualquer registrador MMIO.
- Vídeo: 240p/480i/480p/720p, **4 camadas** (P0/P1 são apelidos das camadas 0/1) sobre a cor de fundo, vblank a cada 16 666 667 ps. Por camada (base `VID_LAYER0 + i*0x40`):
  `ADDR, PITCH, SCX, SCY` (rolagem com `LF_WRAP` em torno de `SIZE = w | h<<16`), `ALPHA` 0..255, `WINX/WINY` (recorte), `FLAGS` (`LF_ENABLE`, `LF_INDEXED` 8 bpp com a CLUT, `LF_WRAP`, `LF_ADD` mistura aditiva, `LF_PREMUL` pixels pré-multiplicados: saída = cor·ALPHA + fundo·(1−a), o que deixa brilhos aditivos de alfa 0 aparecerem sobre o céu do Copper).
  `VID_ORDER` define a ordem de desenho (padrão 0x3210 = camada 3 por cima). O Copper altera tudo isso por scanline (paralaxe, janelas, paletas, fade).

## SnowPaula (48 kHz estéreo 16 bits)
64 vozes: PCM8/PCM16/IMA-ADPCM (nibble alto primeiro), interpolação linear, passa-baixa, posição 3D (ganho 1/(1+d)),
reverb tipo Freeverb. 4 canais legados Amiga (taxa 3546895/período; canais 0 e 3 à esquerda, 1 e 2 à direita).
Sequenciador por eventos de 16 bytes (tempo em amostras, voz, registrador, valor), preciso por amostra.

## Hardware complementar (v0.2)
| Bloco | Base | Resumo |
|---|---|---|
| MMU | F00C0000 | TLB de 64 entradas, páginas de 4 KB, preenchida por software (CTRL/FAR/FSR/AR/TR/OP). Perm: R, W, X, SUPER. MMIO (>= F0000000) nunca é traduzido. Falha -> erro de acesso, vetor 2; pilha/vetores de exceção são físicos. |
| I-cache | — | 32 KB, 4 vias, linhas de 16 B, com dados (stale até `ICACHE_INVAL_ALL/RANGE`); miss = 60 ns + linha. |
| Timers | F0080000 | 2 timers de 1 MHz: one-shot ou periódico, IRQ_TIMER. |
| DVD-ROM | F0090000 | Setores de 2048 B, ~5,5 MB/s (4x), spin-up 800 ms, seek 20–140 ms, leitura por DMA à DDR (disputa o árbitro), IRQ_DVD. |
| Cartão de memória | F00A0000 | 1 MB, setores de 512 B, flash real: programar só limpa bits (AND), apagar em blocos de 4 KB (2 ms); IRQ_MCARD. |
| Controles | F00B0000 | 4 portas: botões, 2 analógicos, 2 gatilhos, rumble; estado latched a cada vblank (IRQ_PAD opcional). |
| Console de depuração | F00E0000 | `DBG_PUTC` (escrita de caractere vai para `Soc::console.out`), `DBG_STATUS`. |
| Chipset Amiga | F00D0000 | registradores $DFF000-$DFFFFF do OCS/ECS/AGA (16 bits independentes: acessos de 32 bits viram dois de 16). Veja docs/SNOWOS.md (Classic). |
| Vídeo | F0070000 | CLUT de 256 entradas ARGB em +0x400; planos indexados 8 bpp (`VCTRL_P0_IDX/P1_IDX`); o Copper pode trocar a paleta por scanline. |
| SnowPaula DSP | — | Orçamento de 100 MHz / 48 kHz = 2083 ciclos por quadro; voz custa 24 (+10 ADPCM, +6 filtro, +12 3D), reverb 180; excedente silencia vozes (PA_DSP_DROPS). |
| FlameCore | — | 4 TMUs (2 por pipeline): piso de 1 texel/ciclo/TMU no tempo de cálculo. |
| Blitter | — | Participa do árbitro de memória: FILL escreve; COPY/CONV565 leem+escrevem; ALPHA/COLORKEY/MINTERM fazem RMW do destino (MINTERM lê também a máscara); CONV565 lê 16 bpp. |

## Teclado/mouse (HID) e SnowFS
`HID_BASE = 0xF00F0000`, IRQ bit 15. Teclado: FIFO de 64 eventos (`HID_KEY_DATA` = usage USB | `HID_KEY_UP` | mods<<16, `0xFFFFFFFF` se vazia); mouse: `HID_MOUSE_DX/DY/WHEEL` acumulam e zeram na leitura, `HID_MOUSE_BTN`.
O host injeta eventos com `Soc::hid.key_event/mouse_move/mouse_button/mouse_wheel`. SnowFS: formato do DVD descrito em `src/abi.h` (`SFFS_*`); `sffs_build` (`src/sffs.h`) gera a imagem.

`MMU_CTRL` bit 1 = supervisor sem tradução (kernel não precisa de páginas mapeadas); com a MMU ligada, a CPU restaura os registradores ao tomar erro de acesso, tornando a instrução reiniciável.

`ST_ATEST` (GPU): descarta pixels com alfa < 128 depois do sombreamento (antes de Z/cor), para cercas, grades e folhagem sem ordenação.

`ST_UV1` (GPU): a unidade de textura 1 lê o **segundo par de UV** do vértice (os campos `fog` e `pad`, bytes 40..47), com mapeamento independente do difuso — é o que permite lightmaps com UV próprio em multitextura; o fog fica indisponível nesse modo.

## Software de sistema e ferramentas (neste repositório)
- **snowcc** (`docs/SNOWCC.md`): compilador de um subconjunto de C para o ColdFire; **SnowOS Native** + SnowGX/SnowVX/SnowAudio (`os/`, `docs/SNOWOS.md`); **SnowOS Classic**: Cpu68k + chipset Amiga + tradutor 68k -> ColdFire.
- Depurador (`sfdbg`), profiler (`sfprof`), desassembador (`src/cfdis.h`): `docs/TOOLS.md`.

## Limitações conhecidas
- Temporização: modelo por tabela de ciclos (acima), não pipeline/ciclo a ciclo; dispositivos executam "funcionalmente no kick" e concluem após a latência, com árbitro de memória por região (DDR/VRAM/ARAM)
  que serializa DMA, GPU e Blitter (`Bus::reserve`).
- DXT1/3/5 usam palavras big-endian (desvio); FPU em double (sem formato estendido).
- ColdFire: deslocamentos/rotações de 1 bit em memória (palavra) e ROXd são extensões do SF-100 (`cpu_cf.cpp`); sem FPU de 96 bits, sem cache de instruções por instrução (só por linha), sem modo de depuração (BDM).
- Chipset Amiga: veja as limitações em `docs/SNOWOS.md`.
