#!/bin/bash
# build_smoke.sh - compila o q3smoke (código real do ioq3) para o SF-100: port/ioq3/build_smoke.sh saida.bin
source "$(dirname "$0")/modules.sh"
OUT=$1; TMP=$(mktemp -d); H=$(cd "$(dirname "$0")/.." && pwd)
for f in "$H/rt/crt0.S" "$H/rt/libc.c" "$H/rt/libc2.c" "$H/rt/setjmp.S" "$H/rt/libm.c" "$H/rt/libgcc.c" "$H/ioq3/q3smoke.c" "$CODE/qcommon/q_shared.c" "$CODE/qcommon/q_math.c"; do
  $CC "${CFLAGS_COMMON[@]}" "-I$CODE/qcommon" -c "$f" -o "$TMP/$(basename "$f").o"
done
$CC "${CFLAGS_COMMON[@]}" -nostdlib -nostartfiles -nodefaultlibs -static -Wl,--no-warn-rwx-segments -Wl,--build-id=none -T "$H/rt/link.ld" -o "${OUT%.bin}.elf" "$TMP"/*.o
${OBJCOPY:-m68k-linux-gnu-objcopy} -O binary "${OUT%.bin}.elf" "$OUT"; rm -rf "$TMP"; echo "$OUT: $(wc -c < "$OUT") bytes"
