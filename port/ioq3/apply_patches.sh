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
patch(code + "/qcommon/q_shared.h", [
  ('  #define BASEGAME					"foobar"', '  #ifndef BASEGAME\n  #define BASEGAME					"foobar"\n  #endif'),
])
patch(code + "/client/snd_local.h", [
  ("#define MAX_RAW_STREAMS (MAX_CLIENTS * 2 + 1)", "#ifndef MAX_RAW_STREAMS\n#define MAX_RAW_STREAMS (MAX_CLIENTS * 2 + 1)\n#endif"),
])
PY
