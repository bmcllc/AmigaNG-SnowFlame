# Ferramentas do SF-100

Todas são geradas por `make` em `build/`. Código-fonte em `tools/` e nas bibliotecas de `src/`.

| Ferramenta | Para quê |
|---|---|
| `snowcc` | compilador C → ColdFire (`docs/SNOWCC.md`) |
| `sfdbg` | depurador interativo |
| `sfprof` | profiler (ciclos por função/instrução, uso dos dispositivos) |
| `m68kxlat` | tradutor estático 68000 → ColdFire |
| `sfclassic` | roda um programa 68000 no SnowOS Classic e grava os quadros em PPM |
| `sf100_demo` | demo (programa ColdFire + VPUs + GPU + Blitter/Copper + áudio) |
| `sf100_techdemos` | cinco tech demos (GPU+VPU, vídeo+Copper, Classic, SnowOS, áudio); veja `docs/TECHDEMOS.md` |

## sfdbg
`sfdbg prog.c|prog.bin [--base A] [--entry A] [--map arq] [-x script] [-c 'cmd']…` (sem script lê comandos da entrada). Para `.c` compila com símbolos.
Comandos (classe `Debugger`, `src/debugger.h`): `regs`, `step [n]`, `next` (passa por cima de BSR/JSR), `finish`, `cont [ps]`, `break X` / `tbreak X`, `watch X [len] [r|w|rw]` (acessos de dados da CPU),
`delete N`, `info break|sym`, `x/N[b|w|l|s] X`, `dis [X] [n]`, `bt` (cadeia de A6), `print E`, `set REG|*ADDR V`, `time`. Expressões: `$hex`, `0xhex`, decimal, símbolos, `d0..d7/a0..a7/pc/sp/sr`, `*endereço`, `+`/`-`.
O depurador lê a memória sem efeitos colaterais (`Bus::peek` enxerga linhas sujas do D-cache). A saída do console de depuração (`DBG_PUTC`) é mostrada como `[console] …`.

## sfprof
`sfprof prog.c [-n top] [-I dir]`. Relatório (`Profiler::report`): ciclos exclusivos/inclusivos e chamadas por função (pilha de BSR/JSR/RTS; recursão conta só a ativação externa), instruções mais quentes (com desmontagem),
CPI, uso de VPU-A/B, GPU, Blitter, DMA, árbitro de memória (bytes e espera), taxas de acerto de D-cache/I-cache/TLB e carga da SnowPaula.

## Desassembador
`cf_disasm(rd16, pc, símbolos)` (`src/cfdis.h`) desmonta tudo o que o interpretador executa; o teste `disasm_length_agrees_with_the_interpreter_for_every_opcode` compara o tamanho com as palavras buscadas pela CPU nos 65 536 opcodes.

## m68kxlat / sfclassic
Veja `docs/SNOWOS.md` (SnowOS Classic).

## sf100_play — jogar numa janela (controle, teclado e som)
Frontend interativo **opcional** (usa SDL2; o núcleo continua sem dependências e `make` não o compila). `make play` monta o DVD e abre o SnowFlame Dashboard.

    make play
    ./build/sf100_play [disco.dvd] [--mc cartao.bin] [--scale N] [--boot caminho] [--mute]

Controle (qualquer um reconhecido pelo SDL_GameController): direcional ou analógico esquerdo, A = confirmar, B = voltar, Start, Back = Select, X/Y/LB/RB, gatilhos e analógicos vão para a porta 0.
Teclado: vai direto para o HID do console (setas, Enter/Espaço, Esc/Backspace...). F11 = tela cheia; F12 ou fechar a janela = sair.
O cartão de memória é lido e gravado em `out/card.bin`. O título da janela mostra os fps; o áudio pode picotar quando a emulação fica abaixo de 60 fps.

Desempenho: o FlameCore renderiza os blocos e o vídeo compõe as linhas em várias threads do host (`HostPool`; `SF_HOST_THREADS=N` fixa o número, 1 = serial; o resultado é idêntico em qualquer N). O limite é o interpretador ColdFire, que roda numa thread só: o Dashboard executa ~400 mil instruções por quadro (código do snowcc sem otimização emitindo vértices), e num i5-11260H fica em ~20 fps com o perfil de energia "power" (núcleo a ~1,6 GHz). Em notebooks, o modo de energia "desempenho" (turbo até 4,4 GHz) é o que mais aproxima dos 60 fps. Todas as ferramentas compilam com `-ffp-contract=off`: sem ele, o `-march=native` do sf100_play usaria FMA e a emulação deixaria de ser bit a bit igual à dos testes.
