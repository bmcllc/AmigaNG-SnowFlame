#!/bin/bash
# check.sh - compila todos os módulos do ioq3 para ColdFire e resume os erros por arquivo.   port/ioq3/check.sh [grupo...]
source "$(dirname "$0")/modules.sh"
OUT=${OUT:-/tmp/sf100_ioq3_obj}; mkdir -p "$OUT"
ok=0; bad=0
run_group() { local name=$1 defs=$2; shift 2; for f in "$@"; do
  o="$OUT/$name-$(echo "$f" | tr / _).o"
  if err=$(compile_one "$defs" "$f" "$o" 2>&1); then ok=$((ok+1)); else bad=$((bad+1)); echo "FALHA [$name] $f: $(echo "$err" | grep -E 'error|fatal' | head -${ERRS:-1} | tr '\n' ' ')"; fi
done; }
groups=${@:-qcommon server botlib client game cgame ui zlib}
for g in $groups; do case $g in
  qcommon) run_group qcommon "" "${QCOMMON[@]}"; if err=$($CC "${CFLAGS_COMMON[@]}" -c "$HERE/ioq3/$VMSF" -o "$OUT/vmsf.o" 2>&1); then ok=$((ok+1)); else bad=$((bad+1)); echo "FALHA [qcommon] $VMSF: $(echo "$err" | grep -E "error|fatal" | head -1)"; fi;; server) run_group server "" "${SERVER[@]}";; botlib) run_group botlib "" "${BOTLIB[@]}";;
  cgame) run_group cgame "-DCGAME" "${CGAME[@]}";; ui) run_group ui "-DUI" "${UI[@]}";;
  zlib) run_group zlib "-DZ_SOLO" "${ZLIB[@]}";;
  client) run_group client "" "${CLIENT[@]}";; game) run_group game "-DQAGAME" "${GAME[@]}";;
esac; done
echo "compilados: $ok   com erro: $bad"
