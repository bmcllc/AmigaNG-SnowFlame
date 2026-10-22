# Port de software C real para o SF-100 (GCC + runtime freestanding)

O `snowcc` é um subconjunto de C pensado para o SnowOS (sem `float`, `long long`, structs por valor...). Para portar programas grandes usamos o **GCC cross de verdade**
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
- Ampliado para o ioquake3: `libc2.c` (ctype, errno, strings extras, tempo `time/clock/gmtime/mktime/strftime`, `strtod`, `sscanf`, `bsearch`), **stdio completo sobre o SnowFS** (`fopen/fread/fseek/fgets/fprintf...`; arquivos do DVD lidos por DMA com cache de setor, arquivos gravados em RAM; `sf_fs.h` expõe a listagem), `setjmp.S`, `endian.h` big-endian, `limits.h`, stubs de `SDL.h`/`dlfcn.h`/`netinet/in.h`. O build usa `-nostdinc` (o `/usr/include` do host fica de fora).

## Uso
```
port/build.sh out.bin programa.c [mais.c ...]     # imagem plana carregável em 0x1000
./build/sf100_run out.bin                         # executa até HALT; imprime o console e D0
```
`port/hello/hello.c` exercita FPU, 64 bits, structs por valor, `qsort`, `malloc` e `printf`; o teste `gcc_coldfire_hello_runs_with_correct_fpu_int64_and_printf` compara a saída com valores conferidos no host (ignorado sem toolchain).

`sf100_run`: `--data caminho=arquivo` (SnowFS em memória como DVD) ou `--dvd imagem`, `--cmdline` (texto em `SRAM_BASE+0x200`), `--time s`, `--shot quadro:arq.png`, `--watch-code lo:hi`, `--profile` (histograma de PCs; `SF_PROFILE_FROM`=segundos, `SF_PROFILE_FILE`, `SF_PROFILE_MISSES=1` conta misses da D-cache). Variáveis: `SF_TS=1` (tempo simulado em cada linha do console), `SF_DVDSTAT=1` (comandos/setores/saltos do DVD e quem chama `BR_SLEEP_US`), `SF_CALLERS=endereço` (quem entra numa função), `SF_BREAK`, `SF_GUARD`. O resumo final traz instruções, ciclos e misses das caches; o tempo simulado que não vira ciclo é espera (`BR_SLEEP_US`) ou miss de cache.

## ioquake3 (Quake 3 / OpenArena)
O port do ioquake3 (motor, renderer sobre o FlameCore, módulos de jogo nativos, ferramentas de dados e seus testes) fica no repositório
**snowflame-ioq3**, ao lado deste (`../snowflame-ioq3`), que usa este como dependência (`SF100_DIR`): runtime, `sf100_run`, `sf100_mkdvd`
(jogos no DVD pelo `MORE_GAMES`), `sf100_play` e a `libsnowflame.a` para os testes. Medidas, fases e roteiro estão no README de lá.
