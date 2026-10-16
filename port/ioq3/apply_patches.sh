#!/bin/bash
# apply_patches.sh - ajustes mínimos no clone do ioq3 (idempotente): tornam sobrescrevíveis por -D os tamanhos de memória que não cabem nos 64 MB do SF-100.
source "$(dirname "$0")/modules.sh"
python3 - "$CODE" <<'PY'
import sys, re
code = sys.argv[1]
def patch(path, subs):
    s = open(path).read(); o = s
    for old, new in subs:
        if new in s: continue
        assert old in s, (path, old)
        s = s.replace(old, new, 1)
    if s != o: open(path, 'w').write(s); print("patch:", path)
patch(code + "/qcommon/common.c", [
  ("#define DEF_COMHUNKMEGS 	128", "#ifndef DEF_COMHUNKMEGS\n#define DEF_COMHUNKMEGS 	128\n#endif"),
  ("#define MIN_COMHUNKMEGS		DEF_COMHUNKMEGS", "#ifndef MIN_COMHUNKMEGS\n#define MIN_COMHUNKMEGS		DEF_COMHUNKMEGS\n#endif"),
  ("#define DEF_COMZONEMEGS		48", "#ifndef DEF_COMZONEMEGS\n#define DEF_COMZONEMEGS		48\n#endif"),
])
patch(code + "/qcommon/msg.c", [
  ("void MSG_initHuffman( void ) {\n	int i,j;", "#ifdef SF100\n// SF-100: a árvore inicial de Huffman usa as frequências divididas por 64 (mesmo resultado estrutural, ~64x menos instruções no boot);\n// cliente e servidor rodam no mesmo binário, então a codificação continua consistente.\n#define HUFF_N(v) (((v) + 63) >> 6)\n#else\n#define HUFF_N(v) (v)\n#endif\nvoid MSG_initHuffman( void ) {\n	int i,j;"),
  ("for (j=0;j<msg_hData[i];j++) {", "for (j=0;j<HUFF_N(msg_hData[i]);j++) {"),
])
patch(code + "/qcommon/q_shared.h", [
  ('  #define BASEGAME					"foobar"', '  #ifndef BASEGAME\n  #define BASEGAME					"foobar"\n  #endif'),
])
patch(code + "/client/snd_local.h", [
  ("#define MAX_RAW_STREAMS (MAX_CLIENTS * 2 + 1)", "#ifndef MAX_RAW_STREAMS\n#define MAX_RAW_STREAMS (MAX_CLIENTS * 2 + 1)\n#endif"),
])
patch(code + "/qcommon/unzip.c", [
  ('#include "unzip.h"', '#include "unzip.h"\n#ifdef SF100\n// zlib compilado com Z_SOLO exige zalloc/zfree do chamador\nstatic voidpf sf_zalloc(voidpf o, uInt n, uInt s) { (void)o; return calloc(n, s); }\nstatic void sf_zfree(voidpf o, voidpf p) { (void)o; free(p); }\n#define SF_ZALLOC ((alloc_func)sf_zalloc)\n#define SF_ZFREE ((free_func)sf_zfree)\n#else\n#define SF_ZALLOC ((alloc_func)0)\n#define SF_ZFREE ((free_func)0)\n#endif'),
  ("stream.zalloc = (alloc_func)0;", "stream.zalloc = SF_ZALLOC;"),
  ("stream.zfree = (free_func)0;", "stream.zfree = SF_ZFREE;"),
])
PY
