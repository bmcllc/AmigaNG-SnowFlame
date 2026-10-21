#!/bin/bash
# build_rtest.sh - compila o harness do renderer (sem o motor):  port/ioq3/build_rtest.sh saida.bin
source "$(dirname "$0")/modules.sh"
OUT=$1; H=$(cd "$(dirname "$0")/.." && pwd); T=$(mktemp -d); objs=()
b() { local o="$T/$(basename "$1").$RANDOM.o"; $CC "${CFLAGS_COMMON[@]}" "-I$CODE/renderercommon" "-I$CODE/thirdparty/jpeg-9f" "-I$H/ioq3" "-I$H/ioq3/sfr" -DSFR_STANDALONE $2 -c "$1" -o "$o" || exit 1; objs+=("$o"); }
"$(dirname "$0")/apply_patches.sh" >/dev/null
for f in "$H/rt/crt0.S" "$H/rt/libc.c" "$H/rt/libc2.c" "$H/rt/setjmp.S" "$H/rt/libm.c" "$H/rt/libgcc.c"; do b "$f"; done
for f in "${SFR[@]}"; do b "$H/ioq3/$f"; done
for f in "${RCOMMON[@]}"; do b "$CODE/$f"; done
for j in "${JPEG[@]}"; do b "$CODE/thirdparty/jpeg-9f/$j.c" "-w"; done
b "$H/ioq3/rtest.c" "$RT_DEFS"; b "$CODE/qcommon/q_shared.c"; b "$CODE/qcommon/q_math.c"
$CC "${CFLAGS_COMMON[@]}" -nostdlib -nostartfiles -nodefaultlibs -static -Wl,--no-warn-rwx-segments -Wl,--build-id=none -Wl,--defsym,__stack_top=0x03F00000 -Wl,-Map,"${OUT%.bin}.map" -T "$H/rt/link.ld" -o "${OUT%.bin}.elf" "${objs[@]}" || exit 1
${OBJCOPY:-m68k-linux-gnu-objcopy} -O binary "${OUT%.bin}.elf" "$OUT"; rm -rf "$T"; echo "$OUT: $(wc -c < "$OUT") bytes"
