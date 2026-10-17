# SnowOS

## SnowOS Native (`os/`)
Kernel preemptivo escrito em C e compilado com o snowcc, mais as bibliotecas SnowGX, SnowVX e SnowAudio. Uma aplicação inclui tudo como uma só unidade:

```c
#include "snowos_all.c"          /* kernel + bibliotecas (use -I os -I os/include) */
void app_main(void) { ... }      /* roda como tarefa de prioridade 5 */
```
- **Modelo:** as tarefas rodam em **modo usuário** (pilha própria, alocada do heap); o kernel roda em supervisor, com uma pilha de kernel de 1 KB por tarefa. Escalonador por prioridade (maior = mais urgente; 0 = ociosa)
  com rodízio entre iguais, fatia de 1 ms vinda do Timer 0 (IRQ nível 4). Chamadas de sistema por `TRAP #0` (D0 = número, D1/D2 = argumentos, resultado em D0); os manipuladores usam o estágio `__interrupt` do snowcc
  (recebem o contexto salvo e devolvem o contexto que deve continuar). Uma exceção em tarefa de usuário (instrução ilegal, privilégio, divisão por zero, acesso inválido) **mata só a tarefa**; no kernel, é pânico.
- **API (`os/include/snowos.h`):** tarefas (`task_create/exit/self/yield/sleep/state/kill`, `os_ticks`), semáforos (`sem_*`, com timeout), filas (`queue_*`), `os_malloc/os_free` (primeiro-que-couber com fusão),
  `os_irq_attach(bit, sem)` (IRQ de dispositivo → `sem_post`), console (`os_putc/puts/printf`), cache (`os_cache_flush/invalidate`), `dma_copy`, `os_exit(code)`.
- **SnowGX:** listas de comandos do FlameCore (`gx_begin/set_rt/clear/state/blend/blend_sep/set_tex/set_palette/draw_*`; `gx_blend_sep` dá fatores próprios ao canal alfa, `gx_submit` espera pela IRQ da GPU), conversão inteiro/16.16 → float IEEE (`gx_f32`, `gx_put_vertex`),
  Blitter (`gx_blit`, `gx_blit_sprites`), vídeo (`gx_layer`, `gx_layer_scroll`, `gx_wait_vblank`).
- **SnowVX:** síncrono — `vx_load_program/vx_load_data/vx_run/vx_read_data` (IRQ + semáforo); **assíncrono** (como pede a arquitetura: a CPU publica trabalho e segue) —
  `vx_load_file` (microprograma `.vx` do disco → IMEM por DMA), `vx_dma_to_dmem`, `vx_submit` (enfileira um job com s0..s7, não bloqueia; fila de 8),
  `vx_busy/vx_wait`, e `vx_sem_take/vx_sem_wait/vx_sem_reset` sobre os semáforos do SnowBridge que os microprogramas sinalizam com `SIGNAL n`.
  Microprogramas padrão em `os/vx/` (montados pelo `sf100_mkdvd` em `sys/vx/*.vx`): **`geometry.vsm`** (VPU-A: transforma, ilumina com difusa + especular + borda e projeta
  lotes de até 448 vértices de 64 B em vértices do FlameCore; DMA de entrada/saída próprio; `SIGNAL 0`) e **`dynamics.vsm`** (VPU-B: partículas residentes na DMEM — integra, oscilador
  procedural, volta na caixa com `VFRC`, fade nas bordas — e emite 2 triângulos por partícula; `SIGNAL 1`). Layouts de job/constantes no cabeçalho de cada arquivo; testes em `tests/test_vxlib.cpp`. **SnowAudio:** `snd_load` (DMA para ARAM), `snd_voice_play`, canais legados, sequenciador, `snd_sine`.
- **SnowFS** (`os/snowfs.c`, construtor de imagens no host em `src/sffs.h`): sistema de arquivos somente leitura sobre o DVD (superbloco no setor 0, diretório plano de entradas de 64 B, dados alinhados a setor).
  `fs_mount/fs_open/fs_read/fs_seek/fs_tell/fs_size/fs_close/fs_load/fs_count/fs_name`; leituras por DMA do DVD com IRQ, seguras entre tarefas (semáforo), até 8 arquivos abertos, nomes de até 47 caracteres.
- **exec/dos** (`os/amigaos.c`): camada mínima com os nomes do AmigaOS 3.x — listas (`NewList/AddHead/AddTail/Remove/RemHead/RemTail/Insert/FindName`), memória (`AllocMem/FreeMem/AllocVec/FreeVec/AvailMem`, `MEMF_CLEAR`; CHIP/FAST ignorados), tarefas e sinais (`FindTask/CreateTask/DeleteTask/AllocSignal/FreeSignal/Wait/Signal/SetSignal`, `Forbid/Permit` cooperativos), portas de mensagem (`CreateMsgPort/PutMsg/GetMsg/WaitPort/ReplyMsg`) e dos (`Open/Close/Read/Seek/Write(Output)/PutStr/Delay/FileSize/IoErr`, arquivos do SnowFS somente leitura). Sem `OpenLibrary`/tabelas de salto, sem `Lock/Examine`, sem `CreateNewProc`. Mensagens devem ficar em heap/globais (a MMU esconde a pilha de cada tarefa das outras).
- **SnowIn** (`os/snowin.c`): `in_key_poll/in_key_wait/in_key_ascii` (eventos USB HID com modificadores, ASCII US) e `in_mouse/in_mouse_buttons`, sobre o dispositivo HID (`HID_BASE`, IRQ bit 15).
- **Proteção de memória (MMU):** com `MMU_CTRL` = enable | supervisor sem tradução, só o modo usuário passa pela TLB (mapa identidade, preenchida por demanda no vetor 2 pela política `user_perm` do kernel).
  Código somente leitura/execução, dados e heap RW, DDR a partir de 16 MB, VRAM, ARAM e SRAM RW, **pilha própria RW com página de guarda**; página zero, pilhas de outras tarefas e pilhas do kernel são proibidas e a tarefa que as tocar é encerrada
  (`[os] tarefa N encerrada: acesso inválido a ...`). A TLB não tem ASID: as páginas da pilha de quem sai do processador são invalidadas. O preenchimento da TLB roda com IRQs mascaradas (o erro de acesso não eleva o nível): sem isso, uma troca de tarefa entre `MMU_TR` e `MMU_OP` regravava `MMU_AR` (`mmu_drop_stack`) e a página da pilha ganhava a tradução física da falta. Para o erro de acesso ser reiniciável, a CPU restaura D0-D7/A0-A7/CCR da instrução que falhou.
- Limites: o kernel e seus dados dividem a imagem com a aplicação (uma tarefa ainda pode corromper `tasks[]`), MMIO nunca é traduzido (as bibliotecas de usuário acessam os dispositivos direto) e as chamadas de sistema não validam ponteiros; sem ponto flutuante no compilador (as bibliotecas montam floats a partir de inteiros), uma aplicação por imagem.

## SnowOS Classic (`src/classic.*`, `src/cpu68k.*`, `src/m68k2cf.*`)
Ambiente de compatibilidade com software Amiga: **Cpu68k** (68000 completo + extensões 68020 de uso comum) sobre o mapa de memória Amiga, o **chipset OCS/ECS/AGA** emulado por quadro e um **tradutor estático** 68k → ColdFire.

### Cpu68k
ISA 68000 completa (BCD, MOVEP, CHK, TAS, trace, erro de endereço com quadro de 14 bytes, linhas A/F, TRAP, autovetores) e do 68020: EXTB.L, MULx.L/DIVx.L (64 bits), LINK.L, Bcc/BRA/BSR.L, RTD, TRAPcc, EA com formato completo.
Validado por teste diferencial contra o núcleo ColdFire (subconjunto comum, ~12 mil instruções aleatórias, CCR a cada passo) e por testes específicos. Ciclos por contagem de acessos de 16 bits (4 ciclos cada) + extras de MUL/DIV/deslocamentos.
Não há MMU/cache de 68020/68030, FPU 68881/68882 nem bit-field (BFxxx).

### ClassicSystem e AmigaChipset
- Mapa: chip RAM = DDR baixa (2 MB), fast RAM até $9FFFFF, ROM em $F80000 (com sobreposição em 0 até o OVL cair), CIAs em $BFE001/$BFD000, registradores em $DFF000 (→ `CLASSIC_BASE` 0xF00D0000 no SF-100).
  A CPU 68k acessa a DDR sem cache (como a chip RAM do Amiga); código ColdFire traduzido deve rodar com a D-cache desligada.
- **Bitplanes:** 1–8 planos, lores/hires, HAM6/HAM8, EHB, dual playfield (com PF2PRI), rolagem (BPLCON1), módulos, DDFSTRT/STOP e DIWSTRT/STOP, paleta de 12 bits (OCS/ECS) e **24 bits com bancos + LOCT (AGA)**.
  **Sprites:** 8, listas DMA (vários blocos por sprite), pares anexados de 4 bits, prioridade por BPLCON2 (PF1P/PF2P). **Copper** Amiga (MOVE/WAIT/SKIP/COPJMP, máscara de WAIT) na posição exata (linha, ciclo de cor): barras de raster
  no meio da linha funcionam. **Blitter clássico:** canais A/B/C/D, minterms, deslocamento de barril, máscaras da primeira/última palavra, módulos, ascendente/descendente, **preenchimento** inclusivo/exclusivo, **modo de linha** (conferido
  contra Bresenham), BBUSY/BZERO e IRQ após o tempo da operação. **INTENA/INTREQ** com os níveis reais (1–6) e `irq_level()` para a CPU; **áudio** de 4 canais sobre os canais legados da SnowPaula; **dois CIAs** (timers A/B com o clock E,
  ICR, TOD por quadro, PRA com botões de fogo e OVL); joystick (JOY1DAT) e fogo vindos do controle do SF-100.
- Saída: framebuffer ARGB 640×512 em `CLASSIC_FB_ADDR` (VRAM) — cada pixel lores vira 2 colunas, linhas duplicadas fora do entrelaçado — que uma camada de vídeo do SF-100 exibe (`VMODE_480P`). `start()` agenda o VBLANK a 50 Hz (PAL) ou 59,94 Hz.
- **Teclado e mouse:** `AmigaChipset::key_raw` envia o código pelo CIA-A (SDR girado e invertido, ICR bit 3, fila de 64 teclas liberada pelo handshake do CRA bit 6); `pump_hid()` (a cada VBLANK, com `use_hid`) traduz os eventos do dispositivo HID do SF-100
  (USB usage → código Amiga, `hid_to_amiga`) e o mouse (contadores JOY0DAT, botão esquerdo em PRA bit 6, direito/meio em POTGOR).
- **Disquete DF0 (ADF 880 KB):** `insert_adf/eject_disk`; controle por CIA-B PRB (motor, seleção, lado, direção, /STEP em borda de descida) e status em CIA-A PRA (/RDY após 500 ms, /TK0, /PROT sempre ativo, /CHNG). A trilha é gerada em MFM AmigaDOS (11 setores, 6400 palavras);
  `DSKPT/DSKLEN` (duas escritas com bit 15) fazem o DMA, com `ADKCON` WORDSYNC + `DSKSYNC` (começa no sincronismo) e IRQ `DSKBLK` após 32 µs/palavra. `src/amigadisk.h` tem o gerador e um decodificador usado nos testes. Sem escrita em disco, sem DF1–DF3, sem `DSKBYTR`.
- **Limitações:** emulação por quadro (registradores lidos no VBLANK; só o Copper age no meio do quadro, não a CPU), sem escrita em disco/serial/RTC, sem colisão de sprites, FMODE/captura larga do AGA ignorado,
  entrelaçado aproximado (renderiza o campo corrente), BPLxPT são contadores vivos (o software reaponta a cada quadro, como no hardware), Copper sem bit CDANG, bit BFD de WAIT ignorado (o blitter é síncrono).

### Tradutor 68k → ColdFire (`m68kxlat`, `translate_68k`)
O ColdFire usa um subconjunto da codificação 68k: o que tem a mesma semântica é **copiado**; o resto é **expandido** com pilha + MOVEM/LEA para não perturbar flags/registradores:
`DBcc`, `EXG`, `MOVEM.L` com pré/pós-incremento, deslocamentos/rotações em memória, `ROXL.L #n,Dn` (flags exatos), `CMPM`, `ORI/ANDI/EORI` para CCR/SR e `RTR`. Endereçamento relativo ao PC e desvios são refeitos (o código é realocado;
a tabela `map` dá o endereço novo de cada instrução), dados no meio do código vão em `data_ranges`, endereços absolutos `$DFFxxx` viram `CLASSIC_BASE+xxx`. Linhas A/F viram `ILLEGAL` (com aviso).
Validação: teste diferencial (150 programas aleatórios + estruturados) comparando registradores, CCR a cada fronteira de instrução de origem e memória com o `Cpu68k`; um programa 68k que configura o vídeo e desenha produz o mesmo quadro por tradução.
**Não suportado (erro claro):** ABCD/SBCD/NBCD, MOVEP, CHK, TRAPV, MOVEM.W, ROXR/ROX.B/.W, EXG com A7, ADDX/SUBX/NEGX em memória, PC+índice, saltos computados para dentro do código traduzido. O tradutor mantém a ABI de exceções do ColdFire
(quadro de 8 bytes com formato/vetor), então manipuladores que lêem o quadro de exceção do 68000 precisam rodar no `Cpu68k`.
