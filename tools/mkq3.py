#!/usr/bin/env python3
"""Reempacota os pk3 do Quake 3 (pak0..pak8 de baseq3) num pk3 só para o DVD do SF-100, com os arquivos na ordem em que o motor os lê.
No DVD cada salto da cabeça custa 20-140 ms; nos pk3 originais as texturas de um mapa ficam espalhadas pelos 457 MB do pak0, e a carga
passa a maior parte do tempo em seek. Aqui os arquivos das listas de ordem vêm primeiro (na ordem da primeira leitura) e o resto depois,
em ordem alfabética: o conteúdo do jogo é o mesmo (o último pak que define um arquivo vence, como no ioq3).
Uso: tools/mkq3.py <baseq3> saida.pk3 [ordem.log ...]
ordem.log: console do motor com "+set fs_debug 1" (linhas "FS_FOpenFileRead: nome (found in ...)"), ex. do sf100_run, ou um nome por linha."""
import sys, os, re, glob, zipfile

base, out, logs = sys.argv[1], sys.argv[2], sys.argv[3:]
src = {}   # nome(minúsculo) -> (zipfile, info)
for p in sorted(glob.glob(os.path.join(base, "*.pk3"))):
    z = zipfile.ZipFile(p)
    for i in z.infolist():
        if not i.filename.endswith("/"): src[i.filename.lower()] = (z, i)

order, seen = [], set()
for lg in logs:
    for ln in open(lg, encoding="latin1"):
        m = re.search(r"FS_FOpenFileRead: (\S+) \(found in", ln)
        n = (m.group(1) if m else ln.strip()).lower()   # linha de log do motor ou um nome por linha (port/ioq3/q3order.txt)
        if n in src and n not in seen: seen.add(n); order.append(n)
names = order + sorted(n for n in src if n not in seen)

with zipfile.ZipFile(out, "w") as zo:
    for n in names:
        z, i = src[n]
        zi = zipfile.ZipInfo(i.filename, date_time=i.date_time); zi.compress_type = i.compress_type; zi.external_attr = i.external_attr
        zo.writestr(zi, z.read(i))
print("%s: %d arquivos (%d na ordem de leitura), %.1f MB" % (out, len(names), len(order), os.path.getsize(out) / 1e6))
