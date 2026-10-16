# Port de software C real para o SF-100 (GCC + runtime freestanding)

O `snowcc` é um subconjunto de C pensado para o SnowOS (sem `float`, `long long`, structs por valor...). Para portar programas grandes (ex.: ioquake3) usamos o **GCC cross de verdade**
(`m68k-linux-gnu-gcc-14 -mcpu=5475` = ColdFire V4e com FPU, 64-bit, structs, varargs) e um runtime próprio em `port/rt/`.

## Toolchain
Debian/Ubuntu: `sudo apt install gcc-m68k-linux-gnu binutils-m68k-linux-gnu`. Sem sudo: `apt-get download gcc-14-m68k-linux-gnu cpp-14-m68k-linux-gnu binutils-m68k-linux-gnu libgcc-14-dev-m68k-cross`,
`dpkg -x` cada `.deb` numa pasta e `export SF_M68K_CROSS=<pasta>` (o `port/build.sh` ajusta PATH e LD_LIBRARY_PATH). O GCC da distribuição é compilado sem multilib: o `libgcc.a` é de 68020 e **não** serve ao ColdFire; por isso `port/rt/libgcc.c` reimplementa as rotinas que o GCC chama.

## Runtime (`port/rt/`)
- `crt0.S`: pilha em `IPL_STACK`, zera o BSS, chama `__rt_init` e `main`; o código de retorno vai para `__exit_code` e para D0 antes do `halt`.
- `link.ld`: código em 0x1000 (`IPL_LOAD_ADDR`), `.rodata`/`.data`/`.bss`, `_end` (heap começa depois), `__code_end`.
- `libc.c`: string/memória, `malloc` (primeiro-que-couber, 60 MB), `qsort`, `rand`, `strtol/atof`, `printf` completo (flags, largura, precisão, `ll`, `%f/%e/%g`) para o console de depuração.
- `libm.c`: `sqrt/fabs` pela FPU; `sin/cos/tan/atan/atan2/asin/acos/exp/log/pow/floor/ceil/fmod/ldexp/frexp...` por série (a FPU do V4e não tem transcendentais); variantes `*f`.
- `libgcc.c`: `__muldi3`, divisões/módulos de 64 bits, deslocamentos de 64 bits, conversões inteiro64 <-> float/double.
- Cabeçalhos mínimos em `rt/include/`.

## Uso
```
port/build.sh out.bin programa.c [mais.c ...]     # imagem plana carregável em 0x1000
./build/sf100_run out.bin                         # executa até HALT; imprime o console e D0
```
`port/hello/hello.c` exercita FPU, 64 bits, structs por valor, `qsort`, `malloc` e `printf`; o teste `gcc_coldfire_hello_runs_with_correct_fpu_int64_and_printf` compara a saída com valores conferidos no host (ignorado sem toolchain).

## Estado do ioquake3 (Fase 2 concluída)
Fontes em `Games-To-Port/ioq3` (clone raso de github.com/ioquake/ioq3). `port/ioq3/check.sh` compila para ColdFire **186 arquivos sem erro**: `qcommon` (BSP/colisão, cvar/cmd, arquivos+unzip, VM interpretada, msg/net_chan), `server`, `botlib`, `client` (som DMA, cinemáticas, consoles; sem SDL/OpenAL/HTTP), `game` (qagame), `cgame`, `q3_ui` e `zlib` (inflate, `Z_SOLO`).
Para isso o runtime ganhou (`port/rt/`): `libc2.c` (ctype, errno, strings extras, tempo `time/clock/gmtime/mktime/strftime`, `strtod`, `sscanf`, `bsearch`), **stdio completo sobre o SnowFS** (`fopen/fread/fseek/fgets/fprintf...`; arquivos do DVD lidos por DMA com cache de setor, arquivos gravados em RAM; `sf_fs.h` expõe a listagem), `setjmp.S`, `endian.h` big-endian, `limits.h`, stubs de `SDL.h`/`dlfcn.h`/`netinet/in.h`. O build usa `-nostdinc` (o `/usr/include` do host fica de fora).
`port/ioq3/q3smoke.c` roda código real do ioq3 (`q_shared.c`, `q_math.c`: vetores, ângulos, matrizes, tokenizer, infostrings, `va`) no emulador e a saída é **idêntica byte a byte** à do mesmo fonte no host x86 (`q3smoke.expected`; teste `ioq3_real_code_on_coldfire_matches_x86_host_output`). `SF_PORT_SLOW=1` ativa o teste que compila os 186 arquivos.

## Fase 3: o motor do ioq3 liga e roda no emulador
`port/ioq3/build_ioq3.sh saida.bin` compila (com cache de objetos) e liga o motor completo: `qcommon`, `server`, `botlib`, `client`, `zlib`, a camada de plataforma e o runtime. Imagem de ~690 KB (BSS ~10 MB).
- `sys_snowflame.c`: `Sys_*` (tempo pelo relógio do SnowBridge, `Sys_ListFiles` sobre o SnowFS + arquivos em RAM, `Sys_Sleep` por `BR_SLEEP_US`), **rede nula** (só loopback), **entrada** `IN_Frame` (teclado HID → teclas e `SE_CHAR`, mouse, botões do controle → `K_JOY*`/setas), **som** `SNDDMA_*` (o buffer DMA do Quake, 22 kHz estéreo, é separado em duas vozes mono da SnowPaula — esquerda e direita — por DMA para a ARAM; a posição de reprodução vem do relógio) e `main()` (linha de comando lida de `SRAM_BASE+0x200`).
- `tr_stub.c`: renderer nulo que cumpre a `refexport_t` (o renderer real sobre o FlameCore é a Fase 4).
- `apply_patches.sh`: ajustes mínimos e idempotentes no clone do ioq3 (macros `#ifndef` para `DEF_COMZONEMEGS`, `DEF_COMHUNKMEGS`, `MIN_COMHUNKMEGS`, `MAX_RAW_STREAMS`, `BASEGAME`), com os valores certos para 64 MB passados por `-D` em `modules.sh`. Compila-se com `-DSTANDALONE -DBASEGAME="baseoa"` (dados do OpenArena; sem a verificação de pak0 do Q3).
- Hardware novo: `BR_SLEEP_US` (SnowBridge): a CPU espera N µs sem queimar instruções (usado por `Sys_Sleep`, `NET_Sleep` e pela espera do DVD); `Cpu::exc_vec/exc_pc` registram a última exceção.
- `sf100_run` ganhou `--data caminho=arquivo` (SnowFS em memória), `--cmdline`, `--profile` (PCs mais quentes), `--watch-code lo:hi` (acha escrita no código); foi ele que achou um estouro de `malloc` em `SND_setup` (`com_soundMegs`).
Resultado (pk3 sintético de `tools/mkpk3.py`, `port/ioq3/fixture/pak0.pk3`): o motor lista `baseoa/pak0.pk3` pelo SnowFS, abre o zip e descomprime (zlib) `default.cfg`, inicializa cliente, renderer e **som sobre a SnowPaula**, completa a inicialização e sai por `+quit` com código 0 (~0,9 G instruções, ~8 s simulados; a maior parte do tempo de CPU é `Huff_Init`). Teste (lento): `SF_PORT_SLOW=1`.

## Fase 4: renderer do ioq3 sobre o FlameCore (`port/ioq3/sfr/`)
O renderer "SnowFlame" implementa a `refexport_t` inteira. A CPU (FPU do ColdFire) projeta e **recorta** (near/frustum) os triângulos, acumula-os já em espaço de tela no formato de vértice de 48 bytes do GPU e monta **uma lista de comandos por quadro**
(o FlameCore é por tiles: precisa ver o quadro todo). Buffer duplo de 640×480 na VRAM; `EndFrame` envia a lista, espera o GPU e troca a camada de vídeo 0.
- `sfr_image.c`: TGA/JPG/PNG/BMP/PCX (carregadores do renderercommon do ioq3 + libjpeg só de decodificação), redimensiona para potência de 2 (máx. 256), converte para RGB565/ARGB4444 com mipmaps na VRAM; lightmaps do BSP com x2 de "overbright".
- `sfr_shader.c`: scripts `.shader` do Q3 (map/clampMap/animMap, blendFunc, alphaFunc, rgbGen, alphaGen, tcMod scroll/scale/rotate, tcGen environment, cull, sort, surfaceparm, skyparms); shaders sem script ganham estágio padrão (lightmap+difuso, dinâmico, vértice, 2D).
- `sfr_world.c`: BSP v46 — planares, triangle soup, **patches de Bézier** (tesselados), lightmaps, **PVS por cluster**, culling por frustum em nós/folhas/superfícies, modelos inline (`*N`), tokens de entidade, `inPVS`.
- `sfr_model.c`: MD3 (frames com interpolação, tags, `LerpTag`, skins por superfície, iluminação por vértice simples). `sfr_scene.c`: entidades, sprites, vigas/relâmpagos, polígonos, ordenação por `sort`.
- Para suportar o Q3 foram **acrescentados dois recursos ao GPU** (`docs/SF100-ABI.md`): `ST_UV1` (a unidade de textura 1 lê um segundo par de UV do vértice: lightmap com UV próprio em multitextura) e `ST_ATEST` (alpha test: cercas, grades, folhagem).
- Harness sem motor: `port/ioq3/rtest.c` (`build_rtest.sh`) implementa `refimport_t` sobre stdio/SnowFS; dados sintéticos de `tools/mkmap.py` em `port/ioq3/fixture/renderdata` (BSP com lightmaps, patch, soup colorida, cerca com alpha test, shader aditivo rotativo, textura JPG, MD3 e HUD). Teste com CRC dos quadros (`SF_PORT_SLOW=1`).
- Correção importante: `endian.h` do runtime não definia `__FLOAT_WORD_ORDER`, e o ioq3 compilava em modo little-endian num CPU big-endian; agora o motor usa `Q3_BIG_ENDIAN` (e troca os bytes dos arquivos de dados, que são little-endian).
Ainda não implementado: skybox, névoa (fog), sombras, marcas de impacto (`MarkFragments`), DrawStretchRaw (cinemáticas), `deformVertexes`/`turb`, luz dinâmica e lightgrid (modelos usam um sombreamento por normal fixo).

## Roteiro
1. (feito) Runtime freestanding e hello world em ColdFire real.
2. (feito) libc ampliada e compilação de todos os módulos do ioq3.
3. (feito) Camada de plataforma (`sys_snowflame.c`, rede nula, `IN_*`, `SNDDMA_*`) e ligação do motor rodando; os módulos de jogo serão QVM da pk3 (sem `dlopen`).
4. (feito, com dados sintéticos) Renderer próprio sobre o FlameCore (BSP + lightmaps em multitextura, shaders, MD3, 2D). Falta validar com um mapa e dados reais do OpenArena.
5. Dados livres do OpenArena e menu/IPL para iniciar o jogo.
