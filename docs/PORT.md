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

### Símbolos ainda indefinidos do motor (= a camada de plataforma da Fase 3)
`main`, `Sys_*` (Init, Error, Print, Quit, Milliseconds, ListFiles/FreeFileList, FOpen, Mkdir, LoadGameDll/UnloadDll, DefaultInstallPath/HomePath..., ConsoleInput, GetClipboardData, RandomBytes, SetEnv...), `NET_*` e `Sys_SendPacket/StringToAdr/IsLANAddress/ShowIP` (rede nula), `IN_*` (entrada: HID/controles), `SNDDMA_*` (som sobre a SnowPaula), `GetRefAPI` (renderer próprio sobre o FlameCore), `S_AL_Init` (stub) e `crc32/inflate*` (zlib já compila).

## Roteiro
1. (feito) Runtime freestanding e hello world em ColdFire real.
2. (feito) libc ampliada e compilação de todos os módulos do ioq3.
3. Camada de plataforma (`sys_snowflame.c`, rede nula, `IN_*`, `SNDDMA_*`) e ligação do motor; módulos de jogo ligados estaticamente (sem `dlopen`) ou QVM da pk3.
4. Renderer próprio sobre o FlameCore (BSP + lightmaps em multitextura, shaders simples) e carga de `.pk3` do DVD (zip do SnowFS).
5. Dados livres do OpenArena e menu/IPL para iniciar o jogo.
