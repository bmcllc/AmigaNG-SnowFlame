# snowcc — compilador C para o SnowFire-CF266

`snowcc` compila **um subconjunto de C** para código ColdFire V4e do SF-100. É um compilador de uma passada sobre uma única unidade de
compilação (use `#include` para juntar arquivos), sem otimizador além de atalhos locais (constantes, potências de dois, comparações diretas).
Código: `src/snowcc_pp.cpp` (pré-processador + léxico), `src/snowcc_parse.cpp` (sintaxe + tipos), `src/snowcc_gen.cpp` (gerador + `cc_compile`).

```
build/snowcc prog.c -o prog.bin -S prog.lst --map prog.map [-I dir] [-Dnome[=valor]] [--base 0x1000] [--stack 0xF00000] [--run]
```
`--run` executa no emulador e imprime o console de depuração, o resultado de `main` (D0) e as instruções/tempo simulados.
API em C++: `cc_compile(source, filename, CcOptions)` → `CcResult {image, symbols, listing, error}` (`src/snowcc.h`).

## Linguagem aceita
- **Tipos:** `void`, `char`, `short`, `int`, `long` (= int, 32 bits), `signed`/`unsigned`, ponteiros, arrays (qualquer dimensão), `struct`/`union`, `enum`, `typedef`.
  Alinhamento natural (char 1, short 2, resto 4). `const`, `volatile`, `static`, `extern`, `register`, `inline` são aceitos (a maioria é ignorada).
- **Comandos:** expressões, `if/else`, `while`, `do/while`, `for` (com declaração), `switch/case/default` (cadeia de comparações), `break`, `continue`, `return`; declarações em qualquer ponto do bloco.
- **Operadores:** todos os de C exceto os de ponto flutuante; ponteiros com aritmética escalada, `sizeof`, casts, `?:`, `,`, atribuições compostas, `++/--`.
- **Funções:** protótipos e definições; ponteiros para função (`int (*f)(int)`, arrays deles, casts `(void (*)(void))addr`); **variádicas** com `...` (os argumentos extras ocupam slots de 32 bits;
  as macros `va_start/va_arg` de `os/include/snowos.h` leem `int`/ponteiros).
- **Globais e inicializadores:** constantes, endereços (`&g`, nomes de funções/arrays, strings), listas com chaves aninhadas para arrays e estruturas, `char s[] = "..."`, locais `static`.
  Locais aceitam inicializadores com chaves (até 64 elementos zerados implicitamente; use globais para arrays grandes).
- **Pré-processador:** `#define` (objeto e função), `#undef`, `#include "x"`/`<x>`, `#if/#ifdef/#ifndef/#elif/#else/#endif` (com `defined`, aritmética, `&&`, `||`), `#error`, `#pragma` (ignorado), `__LINE__`.
- **Não suportado (erro de compilação claro):** `float`/`double`/`long long`, campos de bits, estruturas passadas/devolvidas por valor, K&R, literais compostos. (`goto`/rótulos e `#`/`##` em macros agora são suportados.)

## Convenção de chamada (ABI do código gerado)
- Argumentos empilhados da **direita para a esquerda**, cada um num slot de 32 bits; resultado em **D0**; quem chama remove os argumentos. `char/short` ficam na parte baixa do slot (big-endian).
- Quadro com `LINK A6` / `UNLK A6`; parâmetros em `8(A6)…`, locais em `-n(A6)`. **Nenhum registrador é preservado entre chamadas** (D0-D2/A0-A1 são temporários; locais vivem na pilha).
- `crt0` (`_start`): `movea.l #stack_top,a7; jsr main; halt`.

## Extensões e intrínsecos
- Qualificador **`__interrupt`**: `__interrupt unsigned h(unsigned sp)`. O compilador gera um estágio (símbolo `h`) que salva D0-D7/A0-A6 (64 B) na pilha, chama o corpo `h__body(sp)` com `sp` = ponteiro para o
  contexto salvo e **adota o valor devolvido como novo SP** antes de restaurar os registradores e executar `RTE`. Devolver `sp` retorna ao mesmo contexto; devolver outro troca de contexto (é assim que o SnowOS faz o escalonamento).
  Layout do contexto: `d[8], a[7], pad, formato|vetor|SR, PC` (`struct Ctx` em `os/kernel.c`).
- Intrínsecos (declarados em `os/include/snowflame.h`): `__halt()`, `__nop()`, `__stop(sr)`, `__trap(n)`, `__trapcall(n, a, b, c)` (D0=a, D1=b, D2=c; `TRAP #n`; devolve D0), `__set_vbr`, `__set_cacr`,
  `__get_sp/__set_sp`, `__get_sr/__set_sr`, `__get_usp/__set_usp`.
- `extern char _end;` dá o fim da imagem (início do heap do SnowOS).

## Listagem e símbolos
`-S` grava a desmontagem (com rótulos `.Ln` locais) e os dados; `--map` grava `endereço nome` de funções e globais. O depurador (`sfdbg prog.c`) e o profiler (`sfprof prog.c`) compilam e carregam os símbolos sozinhos.
