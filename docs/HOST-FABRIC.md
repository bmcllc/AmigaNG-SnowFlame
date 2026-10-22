# B.A.S.E. Host Fabric no SF-100 — backend SystemVerilog, frontend C/C++

> A arquitetura é definida pelo Machine Profile e realizada pelos recursos do hospedeiro.
> FPGA não é requisito: é, no máximo, um backend opcional futuro.

O hardware do SF-100 tem duas realizações do **mesmo comportamento**:

| Realização | Onde | Build | Papel |
|---|---|---|---|
| Referência (C++) | `src/*.cpp` dos dispositivos | `make` (`BACKEND=ref`, saída em `build/`) | rápida; base da verificação |
| Backend SystemVerilog | `rtl/*.sv` (compilado pelo Verilator) + `fabric/` | `make BACKEND=sv` (saída em `build/sv/`) | descrição da máquina; realização no host |

O **frontend** (Soc, escalonador, ferramentas, depurador, SnowOS, snowcc, SDL, testes) é o mesmo C/C++ nos dois casos: as classes
de `src/` mantêm a API pública; no build SV os métodos delas são invólucros (`fabric/sv_*.cpp`) que chamam a API exportada da
instância SV correspondente.

## Camadas

```
Machine Profile   src/abi.h (fonte de verdade) -> tools/abi2sv.py -> pacote sf100_abi (SV)   +   rtl/sf100.sv (topo)
Semântica         rtl/sf_*.sv: registradores, máquinas de estado, decodificação/execução, cálculo de latência
Host Fabric       fabric/fabric.cpp: Memory Host, Time Host, I/O Host, FPU do host, observabilidade (rtl/fabric_pkg.sv)
Hospedeiro        PC hoje; Switch/PS4 com o mesmo código (C++17 + C++ gerado pelo Verilator, sem dependências de SO)
```

### Modelo de realização: nível de transação

Os módulos SV não têm relógio: o modelo de tempo do SF-100 é "funcional no kick" (efeito imediato + evento de conclusão
agendado). Cada módulo exporta (DPI-C) a sua API de barramento — `<dev>_read/_write`, `<dev>_event` para os eventos que ele
agendou e as operações do frontend (`dvd_insert`, `hid_key_event`, `vpu_run_now`, `cpu_step`...). Uma instância do modelo
Verilator por `Soc` (contexto e nomes de escopo próprios); o invólucro faz `svSetScope` da instância antes de cada chamada.

### Serviços do hospedeiro (`rtl/fabric_pkg.sv`)

| Serviço | Funções | Realização |
|---|---|---|
| Memory Host | `fab_ok/r8/r16/r32/w*`, `fab_copy`, `fab_fill`, `fab_reserve`, `fab_cache` | memórias planas, árbitro e D-cache de `Bus` |
| Porta da CPU | `fab_fetch16`, `fab_cpu_rd/wr`, `fab_cpu_poll` | I-cache, D-cache, MMU/TLB e MMIO de `Bus` |
| Estado do barramento | `fab_busreg`, `fab_tlb_get/set`, `fab_irq` | IRQ, semáforos, modos da MMU/caches, TLB |
| Time Host | `fab_now`, `fab_at` | `Sched` (eventos em ps, ordem estável) |
| I/O Host | `fab_media_*`, `fab_in`, `fab_out`, `fab_frame_w`, `fab_pcm`, `fab_msg` | disco, cartão, controles, quadro, áudio, console |
| FPU do host | `f32_*` (FP32), `f64_op` | contas IEEE na mesma ordem do modelo de referência |
| Observabilidade | `fab_obs`, `fab_cpu_state` | espelhos dos campos públicos (estatísticas, registradores) |

As contas FP32 passam pela FPU do hospedeiro porque o Verilator promove `shortreal` a `real`; `real` (double) é usado
diretamente na FPU da CPU. O resultado é bit a bit igual ao do modelo de referência (`-ffp-contract=off`).

## Mapa dispositivo -> módulo

| Dispositivo (`src/`) | Módulo SV | Observações |
|---|---|---|
| `Cpu` (ColdFire V4e) | `rtl/sf_cpu.sv` | estado arquitetural espelhado em `soc.cpu` a cada passo; alterações do frontend voltam por `cpu_set_state` |
| `Bridge` | `rtl/sf_bridge.sv` | DMA 4 canais, semáforos, cache, relógio |
| `Vpu` ×2 | `rtl/sf_vpu.sv` | IMEM/DMEM são memória do host (janelas do mapa) |
| `FlameCore` | `rtl/sf_flamecore.sv` | TBR serial (a referência paraleliza blocos no host com o mesmo resultado) |
| `Blitter` | `rtl/sf_blitter.sv` | |
| `Copper` + `Video` | `rtl/sf_display.sv` | composição serial linha a linha; quadro no Display Host |
| `Paula` | `rtl/sf_paula.sv` | PCM no Audio Host |
| `Timers`, `Dvd`, `MemCard`, `Pads`, `Hid`, `MmuDev`, `DebugConsole` | `rtl/sf_periph.sv` | mídias e estado físico de controles ficam no host |

Ficam no Host Fabric (C++): a interconexão e a hierarquia de memória (`Bus`: memórias, I/D-cache, TLB, árbitro), o
escalonador e o SoC Classic (`classic.*`, `cpu68k.*` — outro perfil, ainda só na realização de referência).

## Verificação diferencial

```
make verify            # mesma suíte nos dois backends + demo bit a bit igual (quadros PPM e WAV)
make test BACKEND=sv   # só a suíte no backend SV
```

## Regras

1. Todo comportamento de hardware novo entra nas duas realizações (`src/` e `rtl/`) e passa por `make verify`.
2. `src/abi.h` continua a fonte de verdade; o pacote SV é gerado (`build/sv/sf100_abi_pkg.sv`), nunca editado.
3. O frontend só fala com o hardware pela API das classes (MMIO, `submit`, `insert`...): escrever direto em campos internos
   de um dispositivo não chega ao backend SV (os campos públicos de estado são espelhos).
4. Nada depende de FPGA. Um backend sintetizável seria uma terceira realização opcional do mesmo perfil.

## Limitações conhecidas

- O backend SV é mais lento que a referência (chamadas DPI por acesso e por operação FP); `make play` usa a referência.
- Nos módulos SV, identificadores que são palavras reservadas de SV (`dist`, `cover`, `inside`, `reg`, `super`) foram renomeados.
- O Verilator não aceita chamadas DPI dentro de índices de array: calcule o índice antes.
- O Verilator expande em linha toda função de módulo em cada ponto de chamada. Núcleos grandes (CPU, FlameCore) ficam
  numa `class` dentro de um pacote (`sf_cpu_pkg::cf_core`, `sf_flamecore_pkg::fc_core`): métodos de classe viram funções
  C++ de verdade (com funções de módulo o C++ gerado passava de 100 MB).
- `v != v` em `real` é dobrado para falso pelo Verilator: teste NaN pelos bits (`$realtobits`).
