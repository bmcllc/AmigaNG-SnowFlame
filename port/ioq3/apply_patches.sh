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
  ("void MSG_initHuffman( void ) {\n	int i,j;\n\n	msgInit = qtrue;\n	Huff_Init(&msgHuff);\n", "#ifdef SF100\n#include \"sf_huff.h\"\n#endif\nvoid MSG_initHuffman( void ) {\n	int i,j;\n\n	msgInit = qtrue;\n	Huff_Init(&msgHuff);\n#ifdef SF100\n	// SF-100: a árvore inicial exata vem pronta (sf_huff.h, gerada no host por port/ioq3/huffgen.c): ~6,6 s a menos no boot\n	sf_huff_restore(&msgHuff.compressor);\n	sf_huff_restore(&msgHuff.decompressor);\n	return;\n#endif\n"),
])
patch(code + "/qcommon/vm.c", [
  ("	do\n	{\n		retval = FS_FindVM(", "#ifdef SF100\n	// SF-100: módulos nativos ficam ligados no motor (sem dlopen nem arquivo .so); ver Sys_LoadGameDll em sys_snowflame.c\n	if(interpret == VMI_NATIVE && (vm->dllHandle = Sys_LoadGameDll(module, &vm->entryPoint, VM_DllSyscall)))\n	{\n		vm->systemCall = systemCalls;\n		return vm;\n	}\n#endif\n	do\n	{\n		retval = FS_FindVM("),
])
patch(code + "/qcommon/ioapi.c", [
  ('#include "ioapi.h"\n', '''#include "ioapi.h"
#ifdef SF100
/* SF-100: o diretório central de cada zip aberto para leitura fica em RAM. No DVD o diretório (no fim do pk3) e os dados ficam a
   centenas de MB de distância: sem isto, cada arquivo aberto custa dois saltos longos da cabeça (até 140 ms cada). */
static struct { FILE *f; unsigned long off, len; unsigned char *cd; } sfz[16];
static void sfz_cache_cd(FILE *f) {
    unsigned char e[22]; unsigned long off, len; int i;
    for (i = 0; i < 16 && sfz[i].f; i++) ;
    if (i < 16 && !fseek(f, -22, SEEK_END) && fread(e, 1, 22, f) == 22 && e[0] == 0x50 && e[1] == 0x4b && e[2] == 5 && e[3] == 6) {   /* pk3 sem comentário */
        len = e[12] | e[13] << 8 | (unsigned long)e[14] << 16 | (unsigned long)e[15] << 24;
        off = e[16] | e[17] << 8 | (unsigned long)e[18] << 16 | (unsigned long)e[19] << 24;
        if ((sfz[i].cd = malloc(len ? len : 1)) && !fseek(f, (long)off, SEEK_SET) && fread(sfz[i].cd, 1, len, f) == len) { sfz[i].f = f; sfz[i].off = off; sfz[i].len = len; }
        else { free(sfz[i].cd); sfz[i].cd = NULL; }
    }
    fseek(f, 0, SEEK_SET);
}
static uLong sfz_read_cd(FILE *f, void *buf, uLong size) {   /* 0: fora do diretório (lê do disco) */
    long p; int i;
    for (i = 0; i < 16; i++) if (sfz[i].f == f) {
        p = ftell(f);
        if (p < (long)sfz[i].off || (unsigned long)p + size > sfz[i].off + sfz[i].len) return 0;
        memcpy(buf, sfz[i].cd + (p - sfz[i].off), size); fseek(f, p + (long)size, SEEK_SET); return size;
    }
    return 0;
}
static void sfz_drop(FILE *f) { int i; for (i = 0; i < 16; i++) if (sfz[i].f == f) { free(sfz[i].cd); sfz[i].cd = NULL; sfz[i].f = NULL; } }
#endif
'''),
  ("        file = fopen(filename, mode_fopen);\n", '        file = fopen(filename, mode_fopen);\n#ifdef SF100\n    if (file && !strcmp(mode_fopen, "rb")) sfz_cache_cd(file);\n#endif\n'),
  ("    uLong ret;\n    ret = (uLong)fread(", "    uLong ret;\n#ifdef SF100\n    if ((ret = sfz_read_cd((FILE *)stream, buf, size)) != 0) return ret;\n#endif\n    ret = (uLong)fread("),
  ("    int ret;\n    ret = fclose((FILE *)stream);", "    int ret;\n#ifdef SF100\n    sfz_drop((FILE *)stream);\n#endif\n    ret = fclose((FILE *)stream);"),
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
