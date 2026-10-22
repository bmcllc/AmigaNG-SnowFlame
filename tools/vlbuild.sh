#!/bin/sh
# Compila o C++ gerado pelo Verilator (build/sv/vl) em libVsf100.a + libverilated.a, sem o makefile do Verilator
# (o verilated.mk recusa diretórios com espaços no caminho, como o deste projeto).
#   uso: tools/vlbuild.sh <dir-vl> <CXX> <VERILATOR_ROOT>
set -e
VL="$1"; CXX="$2"; ROOT="$3"
JOBS=$(nproc 2>/dev/null || echo 4)
FLAGS="-O3 -I$ROOT/include -I$ROOT/include/vltstd -DVM_COVERAGE=0 -DVM_SC=0 -DVM_TIMING=0 -DVM_TRACE=0 -DVM_TRACE_FST=0 -DVM_TRACE_VCD=0 \
-faligned-new -fcf-protection=none -Wno-bool-operation -Wno-shadow -Wno-sign-compare -Wno-tautological-compare -Wno-uninitialized \
-Wno-unused-but-set-parameter -Wno-unused-but-set-variable -Wno-unused-parameter -Wno-unused-variable"
export CXX FLAGS
cd "$VL"
rm -f ./*.o libVsf100.a libverilated.a
# modelo gerado
ls Vsf100*.cpp | xargs -P "$JOBS" -I{} sh -c '$CXX $FLAGS -I. -c "{}" -o "$(basename "{}" .cpp).o"'
ar rcs libVsf100.a Vsf100*.o
# runtime do Verilator
for f in verilated verilated_dpi verilated_threads; do echo "$f"; done | xargs -P "$JOBS" -I{} sh -c '$CXX $FLAGS -c "'"$ROOT"'/include/{}.cpp" -o "rt_{}.o"'
ar rcs libverilated.a rt_*.o
