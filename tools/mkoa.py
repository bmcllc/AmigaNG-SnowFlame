#!/usr/bin/env python3
"""Monta um pk3 enxuto do OpenArena para o SF-100: um mapa + as texturas que o BSP referencia + scripts de shader + QVMs + UI/gfx básicos.
Uso: tools/mkoa.py <baseoa> <mapa> saida.pk3   (os paks são lidos em ordem alfabética; o último que define o arquivo vence, como no ioq3)"""
import sys, os, re, struct, zipfile, glob
base, mapa, out = sys.argv[1:4]
src = {}   # nome(minúsculo) -> (zipfile, info)
for p in sorted(glob.glob(os.path.join(base, "*.pk3"))):
    z = zipfile.ZipFile(p)
    for i in z.infolist():
        if not i.filename.endswith("/"): src[i.filename.lower()] = (z, i)
bsp = src["maps/%s.bsp" % mapa]; data = bsp[0].read(bsp[1])
off, ln = struct.unpack_from("<ii", data, 8 + 8 * 1)          # lump 1 = shaders (64 bytes de nome + 8)
shaders = {data[off + k:off + k + 64].split(b"\0")[0].decode().lower() for k in range(0, ln, 72)}
want = {"maps/%s.bsp" % mapa}
for n in list(src):
    if n.startswith("scripts/") and n.endswith((".shader", ".arena")): want.add(n)
    if n.startswith("vm/") or n in ("default.cfg", "ui/menus.txt"): want.add(n)
    if n.startswith(("ui/", "menu/", "gfx/2d/", "fonts/", "sound/misc/")) and not n.startswith("sound/"): want.add(n)
# texturas/shaders citados pelo BSP e por scripts de shader (mapas de textura por 'map'/'clampmap')
text = "".join(src[n][0].read(src[n][1]).decode("latin1") for n in want if n.endswith(".shader"))
refs = set(shaders) | {m.lower().replace("\\", "/") for m in re.findall(r"(?:map|clampmap|animmap\s+[\d.]+)\s+(\S[^\n]*)", text)}
need = set()
for r in shaders:                                               # só os shaders do BSP -> arquivo direto
    for ext in (".tga", ".jpg"):
        if r + ext in src: need.add(r + ext)
for blk in re.finditer(r"^([^\s/][^\n]*?)\s*\{(.*?)^\}", text, re.S | re.M):   # blocos de shader do BSP
    if blk.group(1).strip().lower() in shaders:
        for m in re.findall(r"(?:map|clampmap)\s+(\S+)|animmap\s+[\d.]+\s+([^\n]+)", blk.group(2), re.I):
            for t in (x for g in m for x in g.split()):
                t = t.lower().replace("\\", "/")
                if t.startswith("$"): continue
                for cand in (t, os.path.splitext(t)[0] + ".tga", os.path.splitext(t)[0] + ".jpg"):
                    if cand in src: need.add(cand)
want |= need
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as o:
    for n in sorted(want):
        z, i = src[n]; o.writestr(i.filename, z.read(i))
print(out, len(want), "arquivos,", os.path.getsize(out), "bytes;", len(shaders), "shaders no BSP")
