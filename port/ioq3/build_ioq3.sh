#!/bin/bash
# build_ioq3.sh - compila e liga o motor do ioquake3 para o SF-100 (cliente+servidor, renderer nulo):  port/ioq3/build_ioq3.sh saida.bin
# Objetos ficam em $OBJ (padrão /tmp/sf100_ioq3_obj) e só são refeitos quando o fonte muda.
source "$(dirname "$0")/modules.sh"
OUT=$1; OBJ=${OBJ:-/tmp/sf100_ioq3_obj}; mkdir -p "$OBJ"; H=$(cd "$(dirname "$0")/.." && pwd)
"$(dirname "$0")/apply_patches.sh" >/dev/null
objs=()
build() {   # build defines grupo caminho_completo
  local o="$OBJ/$2-$(basename "$3").o"
  if [ ! -f "$o" ] || [ "$3" -nt "$o" ] || [ -n "$FORCE" ]; then $CC "${CFLAGS_COMMON[@]}" $1 -c "$3" -o "$o" || exit 1; fi
  objs+=("$o")
}
for f in "${QCOMMON[@]}"; do build "" qcommon "$CODE/$f"; done
for f in "${SERVER[@]}"; do build "" server "$CODE/$f"; done
for f in "${BOTLIB[@]}"; do build "" botlib "$CODE/$f"; done
for f in "${CLIENT[@]}"; do build "" client "$CODE/$f"; done
for f in "${ZLIB[@]}"; do build "-DZ_SOLO" zlib "$CODE/$f"; done
for f in "$H/ioq3/sys_snowflame.c" "$H/ioq3/tr_stub.c"; do build "" plat "$f"; done
for f in "$H/rt/crt0.S" "$H/rt/libc.c" "$H/rt/libc2.c" "$H/rt/setjmp.S" "$H/rt/libm.c" "$H/rt/libgcc.c"; do build "" rt "$f"; done
$CC "${CFLAGS_COMMON[@]}" -nostdlib -nostartfiles -nodefaultlibs -static -Wl,--no-warn-rwx-segments -Wl,--build-id=none -Wl,--defsym,__stack_top=0x03F00000 -Wl,-Map,"${OUT%.bin}.map" -T "$H/rt/link.ld" -o "${OUT%.bin}.elf" "${objs[@]}" || exit 1
${OBJCOPY:-m68k-linux-gnu-objcopy} -O binary "${OUT%.bin}.elf" "$OUT"
echo "$OUT: $(wc -c < "$OUT") bytes; $(m68k-linux-gnu-size "${OUT%.bin}.elf" | tail -1)"
