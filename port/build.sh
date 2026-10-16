#!/bin/bash
# build.sh - compila um programa C para o SF-100 com o GCC cross (m68k, ColdFire V4e) e gera a imagem plana para o IPL.
#   port/build.sh saida.bin arquivo.c [outros.c ...]
# Usa m68k-linux-gnu-gcc-14 do PATH, ou o de $SF_M68K_CROSS (raiz de um 'dpkg -x', ex.: ~/.local/opt/m68k-cross).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
if [ -n "$SF_M68K_CROSS" ]; then export PATH="$SF_M68K_CROSS/usr/bin:$PATH"; export LD_LIBRARY_PATH="$SF_M68K_CROSS/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH"; fi
CC=${CC:-m68k-linux-gnu-gcc-14}
OBJCOPY=${OBJCOPY:-m68k-linux-gnu-objcopy}
OUT=$1; shift
CFLAGS=(-mcpu=5475 -O2 -g0 -ffreestanding -fno-builtin-printf -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -mno-pcrel "-I$HERE/rt/include" -Wall -Wno-misleading-indentation)
TMP=$(mktemp -d)
for f in "$HERE/rt/crt0.S" "$HERE/rt/libc.c" "$HERE/rt/libm.c" "$HERE/rt/libgcc.c" "$@"; do
  $CC "${CFLAGS[@]}" $EXTRA_CFLAGS -c "$f" -o "$TMP/$(basename "$f").o"
done
$CC "${CFLAGS[@]}" -nostdlib -nostartfiles -nodefaultlibs -static -Wl,--no-warn-rwx-segments -Wl,--build-id=none -T "$HERE/rt/link.ld" -Wl,-Map,"${OUT%.bin}.map" -o "${OUT%.bin}.elf" "$TMP"/*.o
$OBJCOPY -O binary "${OUT%.bin}.elf" "$OUT"
rm -rf "$TMP"
echo "$OUT: $(wc -c < "$OUT") bytes"
