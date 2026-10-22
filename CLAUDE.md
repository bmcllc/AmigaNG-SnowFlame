# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Emulador C++17 do console conceitual "Amiga SnowFlame SF-100" (CPU ColdFire V4e, 2× SnowVector-128 VPU, GPU FlameCore, Blitter, Copper, vídeo, SnowPaula, SnowBridge). Sem dependências externas no backend de referência (o backend SystemVerilog opcional exige Verilator). README, comentários e docs estão em português; mantenha esse idioma.

## Commands

```
make                          # build/libsnowflame.a + build/sf100_tests + build/sf100_demo
make test                     # roda todos os testes
./build/sf100_tests <substr>  # roda só os testes cujo nome contém <substr> (filtro strstr em tests/test_main.cpp)
make demo                     # roda a demo; escreve out/frame_*.ppm e out/audio.wav
./build/sf100_demo out 60     # demo com diretório de saída e nº de quadros
make clean                    # remove build/ e out/
./build/snowcc app.c -I os -I os/include --run    # compila/executa C no emulador (os apps do SnowOS usam `#include "snowos_all.c"`)
./build/sfdbg app.c -I os -I os/include           # depurador;  ./build/sfprof ...  # profiler;  ./build/m68kxlat / sfclassic: Classic
```

```
make dvd                     # monta out/snowflame.dvd (menu + recursos + jogos; GAMES=--game nome=os/apps/games/x.c)
make boot                    # liga o SF-100 no Dashboard sem janela (PNGs em out/boot/)
make play                    # janela/som/controle; opcional, exige SDL2 e pkg-config (fora de `all`)
make play-oa                 # idem com o OpenArena (ioq3) no DVD; ver docs/PORT.md (make oa / dvd-oa)
make techdemos               # out/techdemos/ (ver docs/TECHDEMOS.md)
port/build.sh out.bin prog.c # C real via GCC cross m68k (ColdFire V4e) + runtime em port/rt/; ver docs/PORT.md (ioquake3 em port/ioq3/)
```

`make` compila tudo e é lento (os testes do kernel/compilador/68k são pesados): use `make -j12`; o tempo de `./build/sf100_tests` completo passa de 1 min.

O Makefile ignora o `CXX` do ambiente e usa g++ (com clang as imagens da demo divergem das do g++; objetos de compiladores diferentes não se misturam: um carimbo `build/.cc-*` refaz tudo ao trocar). Para outro compilador, passe `CXX=...` na linha de comando. Não há linter. O build usa `-Wall -Wextra`; mantenha-o sem warnings. O Makefile lista os `.cpp` de `src/` explicitamente em `SRC` — um novo arquivo em `src/` precisa ser adicionado lá (testes são pegos por wildcard `tests/*.cpp`; ferramentas ficam em `tools/` com alvo próprio).

## Backend SystemVerilog (B.A.S.E. Host Fabric) — ver docs/HOST-FABRIC.md
- O hardware tem duas realizações do mesmo comportamento: modelos C++ de referência (`src/`, `make`) e backend SystemVerilog (`rtl/*.sv`, Verilator, `make BACKEND=sv` -> `build/sv/`). O frontend (Soc, ferramentas, testes) é o mesmo; no build SV os métodos das classes de dispositivo ficam em `fabric/sv_*.cpp` e chamam a API DPI exportada pelos módulos.
- `fabric/fabric.cpp` implementa os serviços do hospedeiro importados por `rtl/fabric_pkg.sv` (memória/caches/TLB do `Bus`, eventos do `Sched`, mídias, FPU FP32, espelhos). O pacote de constantes SV é gerado de `src/abi.h` (`tools/abi2sv.py`).
- Mudou comportamento de hardware? Altere `src/` E `rtl/` e rode `make verify` (suíte nos dois backends + demo bit a bit igual). Campos públicos dos dispositivos são espelhos no backend SV: o frontend deve usar a API (MMIO, `write`, `insert`...).
- Verilator: evite palavras reservadas de SV como identificadores e chamadas DPI dentro de índices de array; contas FP32 só via `f32_*`.

## Architecture

- **`src/abi.h` é a fonte de verdade** do mapa de memória, registradores MMIO, opcodes (`GC_*`, VPU) e bits de IRQ; `docs/SF100-ABI.md` é a descrição legível (inclui desvios documentados e limitações conhecidas). Atualize os dois ao mudar o hardware. Tudo é big-endian na memória.
- **`Soc` (`src/soc.h`)** instancia e liga tudo: `Sched` (eventos em picossegundos), `Bus`, `Bridge`, `Cpu`, `vpuA/B`, `gpu`, `blt`, `cop`, `paula`, `video`, `mmu`, `timers`, `dvd`, `mcard`, `pads`. `Soc::run/run_until_halt/run_frames/wait_until` são os pontos de entrada de simulação.
- **Modelo de tempo: "funcional no kick"** — cada dispositivo executa seu efeito imediatamente quando disparado e agenda o evento de conclusão (com IRQ opcional) após a latência em ciclos. Não é ciclo a ciclo.
- **`Bus` (`bus.h/.cpp`)**: memórias planas (DDR/VRAM/ARAM/SRAM), despacho de MMIO via `Device::read/write` (`map_device`) e janelas lineares (`add_window`, ex.: IMEM/DMEM das VPUs). Acesso `raw_*` ignora cache/MMIO; `cpu_read/cpu_write` passam pela D-cache (32 KB, write-back, **não coerente** com DMA/VPU/GPU — software precisa de `CACHE_FLUSH/INVAL`), pela MMU de software (TLB) e contam stall. `Bus::reserve` é o árbitro de memória por região que serializa DMA/GPU. **Caminho rápido (só velocidade):** as linhas recentes de busca/leitura/escrita (`LineHint`) valem enquanto `Bus::ep.i`/`ep.d` não avançam; tudo que muda modo (`ModeFlag`), TLB (`TlbArray`: acesso não-const avança a época, o Bus lê pela interface const) ou validade/identidade das linhas das caches (preenchimento, `icache_inval_*`, `cache_*_range`) precisa avançar a época — ao adicionar um novo ponto que escreve nesse estado, avance-a.
- **CPU (`cpu_cf.*`)**: interpretador ColdFire com FPU double e EMAC_B com a codificação do CFPRM rev. 3 (`cpu_cf.cpp`: `emac_*`; `cfasm.h`: `mac_*`). `src/cfasm.h` é um montador in-process usado por testes e demo.
- **VPU (`vpu.*`)**: SIMD FP32×4 com ISA de 32 bits própria; `vpu_assemble`/`vpu_disasm` em texto.
- **GPU (`flamecore.*`, `texutil.h`)**: renderizador por tiles 32×32 acionado por lista de comandos (`GC_*`). DXT1 usa palavras big-endian (desvio documentado).
- **`chipset.*`** = Blitter, Copper, Video; **`paula.*`** = áudio (+`adpcm.h`); **`periph.*`** = MMU, Timers, DVD, MemCard, Pads.
- **`demo/`** (header-only: `kernels.h` programas VPU, `scene.h` cena) + `demo_main.cpp`; **`tests/`** usa o mini-framework de `tests/framework.h` (`TEST(nome)`, `CHECK`, `CHECK_EQ`, `CHECK_NEAR`; auto-registro). `tests/test_cpu_isa.cpp` cobre a ISA ColdFire e varre os 65536 opcodes (rode também com `-fsanitize=address,undefined` ao mexer no interpretador) e `helpers.h`. Os testes também incluem headers de `demo/`.

## Dois caminhos para compilar código para o SF-100
- **`snowcc`** (subconjunto de C: sem `float`, `long long`, structs por valor) para o SnowOS em `os/`.
- **`port/`**: GCC cross de verdade (`m68k-linux-gnu-gcc-14 -mcpu=5475`) com runtime próprio (`port/rt/`: libc, libm, libgcc reimplementada — o libgcc.a da distribuição é 68020 e não serve). Usado para portar programas grandes. Código carregado em 0x1000 (`IPL_LOAD_ADDR`).

## Software de sistema (resumo; detalhes em docs/)
- **Interpretador ColdFire** (`cpu_cf.*`): formato completo de EA, modo usuário/supervisor com troca de A7, FPU/eMAC estendidos, temporização por tabela; **I-cache com dados** (instrução reescrita só vale após `ICACHE_INVAL_*`; `Bus::raw_write`/`Soc::load` invalidam) e MMU que traduz acessos cruzando páginas. `src/cfdis.*` desmonta tudo que ele executa (teste varre os 65536 opcodes).
- **Vídeo** tem 4 camadas com rolagem/alfa/janela/ordem (registradores alteráveis pelo Copper por scanline); **FlameCore** aceita CI4/CI8 (`GC_SET_PALETTE`), VQ, ETC1, DXT1/3/5 etc. (`texutil.h` tem os codificadores).
- **snowcc** (`snowcc_*.cpp`): C → ColdFire; funções `__interrupt` viram estágios que trocam de contexto devolvendo um SP (base do escalonador do SnowOS). `os/` é compilado por ele em tempo de execução (os testes de SnowOS compilam o kernel a cada teste).
- **SnowOS Classic:** `classic.*` (chipset Amiga por quadro, CIAs, `ClassicSystem` com o mapa de memória), `cpu68k.*`, `m68k2cf.*`. O chipset é um `Device` com `native16()` (registradores de 16 bits independentes).
- **Depurador/profiler** usam `Soc::on_step`/`step_one`, `Bus::on_access` (watchpoints) e `Bus::peek` (leitura sem efeitos colaterais).
