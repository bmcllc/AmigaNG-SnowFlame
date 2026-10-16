#!/bin/bash
# cc_one.sh - compila um único arquivo para o SF-100 com as mesmas opções do motor:  port/ioq3/cc_one.sh fonte.c saida.o [flags extras]
source "$(dirname "$0")/modules.sh"
src=$1; out=$2; shift 2
$CC "${CFLAGS_COMMON[@]}" "-I$CODE/renderercommon" "-I$CODE/thirdparty/jpeg-9f" "-I$HERE/ioq3" "$@" -c "$src" -o "$out"
