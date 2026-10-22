#!/bin/bash
# build_ioq3.sh - compila e liga o motor do ioquake3 para o SF-100 (cliente+servidor, renderer nulo):  port/ioq3/build_ioq3.sh saida.bin
# Objetos ficam em $OBJ (padrão /tmp/sf100_ioq3_obj) e só são refeitos quando o fonte muda.
source "$(dirname "$0")/modules.sh"
OUT=$1; OBJ=${OBJ:-/tmp/sf100_ioq3_obj}; mkdir -p "$OBJ"; H=$(cd "$(dirname "$0")/.." && pwd)
"$(dirname "$0")/apply_patches.sh" >/dev/null || exit 1
gen_huff "$OBJ" || exit 1
[ -n "$NATIVE" ] && CFLAGS_COMMON+=(-DLEGACY_PROTOCOL)   # Quake 3: protocolo 68 (demos e servidores 1.32), ausente no modo STANDALONE
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
build "${NATIVE:+-DSF_NATIVE_GAME}" plat${NATIVE:+n} "$H/ioq3/sys_snowflame.c"   # com NATIVE=1, Sys_LoadGameDll entrega os módulos ligados
build "" plat "$H/ioq3/$VMSF"                                                       # interpretador da QVM com goto computado (no lugar de qcommon/vm_interpreted.c)
for f in "${SFR[@]}"; do build "" sfr "$H/ioq3/$f"; done                          # renderer sobre o FlameCore
for f in "${RCOMMON[@]}"; do build "" rcommon "$CODE/$f"; done                    # carregadores de imagem e fontes do ioq3
for j in "${JPEG[@]}"; do build "-w" jpeg "$CODE/thirdparty/jpeg-9f/$j.c"; done  # libjpeg (só decodificação)
# NATIVE=1: qagame/cgame/ui do Quake 3 nativos, ligados no motor. Cada módulo vira um objeto parcial com só vmMain/dllEntry globais
# (renomeados para <módulo>_*) e marcas <módulo>_data_*/_bss_* que o Sys_LoadGameDll usa para reiniciar as variáveis a cada carga.
native() {   # native módulo defines fontes...
  local m=$1 defs=$2; shift 2; local mo=() f o; for f in "$@"; do o="$OBJ/$m-$(basename "$f").o"
    if [ ! -f "$o" ] || [ "$CODE/$f" -nt "$o" ] || [ -n "$FORCE" ]; then $CC "${CFLAGS_COMMON[@]}" $defs -c "$CODE/$f" -o "$o" || exit 1; fi; mo+=("$o"); done
  ${LD:-m68k-linux-gnu-ld} -r -o "$OBJ/$m.r.o" "${mo[@]}" || exit 1
  sz() { m68k-linux-gnu-size -A "$OBJ/$m.r.o" | awk -v s=$1 '$1==s{print $2}'; }
  ${OBJCOPY:-m68k-linux-gnu-objcopy} -G vmMain -G dllEntry "$OBJ/$m.r.o" "$OBJ/$m.l.o" || exit 1
  ${OBJCOPY:-m68k-linux-gnu-objcopy} --redefine-sym vmMain=${m}_vmMain --redefine-sym dllEntry=${m}_dllEntry \
    --add-symbol ${m}_data_start=.data:0,global --add-symbol ${m}_data_end=.data:$(sz .data),global \
    --add-symbol ${m}_bss_start=.bss:0,global --add-symbol ${m}_bss_end=.bss:$(sz .bss),global "$OBJ/$m.l.o" "$OBJ/$m.mod.o" || exit 1
  objs+=("$OBJ/$m.mod.o")
}
if [ -n "$NATIVE" ]; then
  Q3='-UBASEGAME -DBASEGAME="baseq3"'   # nome do jogo no serverinfo (o motor segue o com_basegame)
  native qagame "-DQAGAME $Q3" "${GAME[@]}"; native cgame "-DCGAME $Q3" "${CGAME[@]}"; native ui "-DUI $Q3" "${UI[@]}"
fi
for f in "$H/rt/crt0.S" "$H/rt/libc.c" "$H/rt/libc2.c" "$H/rt/setjmp.S" "$H/rt/libm.c" "$H/rt/libgcc.c"; do build "" rt "$f"; done
$CC "${CFLAGS_COMMON[@]}" -nostdlib -nostartfiles -nodefaultlibs -static -Wl,--no-warn-rwx-segments -Wl,--build-id=none -Wl,--defsym,__stack_top=0x03F00000 -Wl,-Map,"${OUT%.bin}.map" -T "$H/rt/link.ld" -o "${OUT%.bin}.elf" "${objs[@]}" || exit 1
${OBJCOPY:-m68k-linux-gnu-objcopy} -O binary "${OUT%.bin}.elf" "$OUT"
echo "$OUT: $(wc -c < "$OUT") bytes; $(m68k-linux-gnu-size "${OUT%.bin}.elf" | tail -1)"
