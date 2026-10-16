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
