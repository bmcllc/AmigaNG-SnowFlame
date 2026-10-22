#!/bin/bash
# make_oa.sh [pasta_de_saida]: compila o motor do ioquake3 (oa.bin) e monta o pk3 enxuto do OpenArena (pak0.pk3) para o DVD do SF-100.
# Padrão: out/oa. Variáveis: SF_M68K_CROSS (toolchain, padrão ~/.local/opt/m68k-cross se não houver m68k-linux-gnu-gcc-14 no PATH),
# OA_BASE (pasta baseoa do OpenArena 0.8.8), OA_PLAYERS (personagens, padrão sarge,grism). Depois: make dvd-oa / make play-oa.
set -e
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
OUT=${1:-$ROOT/out/oa}; mkdir -p "$OUT"
unset CC CXX                                                   # o ambiente pode apontar CC/CXX para um compilador do host
OA_BASE=${OA_BASE:-"$ROOT/../Games-To-Port/openarena/x/openarena-0.8.8/baseoa"}
[ -d "$OA_BASE" ] || { echo "make_oa: não achei $OA_BASE (defina OA_BASE=pasta baseoa do OpenArena 0.8.8)" >&2; exit 1; }
OBJ=${OBJ:-$OUT/obj} "$HERE/build_ioq3.sh" "$OUT/oa.bin"
python3 "$ROOT/tools/mkoa.py" "$OA_BASE" oa_dm1 "$OUT/pak0.pk3" --players "${OA_PLAYERS:-sarge,grism}"
