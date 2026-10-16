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

## Roteiro do ioquake3
1. (feito) Runtime freestanding e hello world em ColdFire real.
2. libc ampliada para o que o ioq3 usa (`stdio` com arquivos sobre o SnowFS, `setjmp`, `time`, `errno`, `ctype`, `assert`) e ferramenta para empacotar `.pk3`/dados no DVD.
3. Camada de plataforma (`sys_*`): tempo, entrada (HID/controles), arquivos (SnowFS), memória.
4. Renderer próprio sobre o FlameCore (BSP + lightmaps em multitextura), som sobre a SnowPaula.
5. Dados livres do OpenArena.
