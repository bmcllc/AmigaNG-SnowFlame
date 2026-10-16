#!/usr/bin/env python3
"""Gera um pk3 (zip) mínimo para testar o motor do ioquake3 sem os dados do jogo: default.cfg + um arquivo de texto (compressão deflate, para exercitar o inflate do zlib).
Uso: tools/mkpk3.py saida.pk3"""
import sys, zipfile
out = sys.argv[1] if len(sys.argv) > 1 else "pak0.pk3"
cfg = """// default.cfg sintético do SF-100
seta sensitivity "5"
seta name "SnowPlayer"
seta com_blood "1"
echo "[default.cfg] executado pelo ioq3 a partir do pk3 no DVD"
"""
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("default.cfg", cfg)
    z.writestr("readme.txt", ("SnowFlame SF-100 - pak sintetico\n" * 40))
print(out)
