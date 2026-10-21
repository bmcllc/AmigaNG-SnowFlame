#!/usr/bin/env python3
"""Monta um pk3 enxuto do OpenArena para o SF-100: um mapa + tudo de que ele precisa para rodar (QVMs, scripts de shader/bots/arenas,
botfiles, UI/menus, gfx, modelos de armas/itens/objetos do mapa, os personagens pedidos e as texturas que tudo isso referencia).
Sons e músicas ficam de fora (o SF-100 roda o ioq3 com som desligado por padrão; o motor segue sem eles).
Uso: tools/mkoa.py <baseoa> <mapa> saida.pk3 [--players sarge,grism,...]
(os paks são lidos em ordem alfabética; o último que define o arquivo vence, como no ioq3)"""
import sys, os, re, struct, zipfile, glob

args = sys.argv[1:]
players = ["sarge"]
if "--players" in args:
    k = args.index("--players"); players = [p for p in args[k + 1].split(",") if p]; del args[k:k + 2]
base, mapa, out = args[:3]

src = {}   # nome(minúsculo) -> (zipfile, info)
for p in sorted(glob.glob(os.path.join(base, "*.pk3"))):
    z = zipfile.ZipFile(p)
    for i in z.infolist():
        if not i.filename.endswith("/"): src[i.filename.lower()] = (z, i)
def read(n): z, i = src[n]; return z.read(i)

bspname = "maps/%s.bsp" % mapa
data = read(bspname)
def lump(k): off, ln = struct.unpack_from("<ii", data, 8 + 8 * k); return data[off:off + ln]
shaders = {lump(1)[k:k + 64].split(b"\0")[0].decode().lower() for k in range(0, len(lump(1)), 72)}
ents = lump(0).decode("latin1")

want = {bspname}
def add_prefix(*pre):
    for n in src:
        if n.startswith(pre): want.add(n)
add_prefix("scripts/", "botfiles/", "vm/", "ui/", "menu/", "gfx/", "sprites/", "fonts/", "models/weapons2/", "models/weapons/", "models/weaphits/",
           "models/powerups/", "models/ammo/", "models/flags/", "models/gibs/", "models/misc/")
want = {n for n in want if not n.endswith((".wav", ".ogg", ".mp3"))}
want.add("default.cfg")
for n in src:
    if n.startswith("maps/%s." % mapa) or n == "maps/%s.arena" % mapa: want.add(n)       # .aas/.arena do mapa, se houver
# modelos citados pelas entidades do BSP (model "models/...md3")
for m in re.findall(r'"model"\s+"(models/[^"]+)"', ents, re.I):
    if m.lower() in src: want.add(m.lower())
for p in players:
    for n in src:
        if n.startswith("models/players/%s/" % p) and not n.endswith((".wav", ".txt", ".obj")): want.add(n)

# nomes de shader/textura referenciados: BSP, MD3 (por superfície), arquivos .skin e os próprios arquivos de gfx/sprites/menu
refs = set(shaders)
def md3_shaders(b):
    if b[:4] != b"IDP3": return
    nsurf = struct.unpack_from("<i", b, 84)[0]; ofs_surf = struct.unpack_from("<i", b, 100)[0]
    o = ofs_surf
    for _ in range(nsurf):
        nsh = struct.unpack_from("<i", b, o + 76)[0]; osh = struct.unpack_from("<i", b, o + 92)[0]; oend = struct.unpack_from("<i", b, o + 104)[0]
        for k in range(nsh):
            refs.add(b[o + osh + 68 * k:o + osh + 68 * k + 64].split(b"\0")[0].decode("latin1").lower())
        o += oend
for n in list(want):
    if n.endswith(".md3"): md3_shaders(read(n))
    elif n.endswith(".skin"):
        for ln in read(n).decode("latin1").splitlines():
            if "," in ln: refs.add(ln.split(",", 1)[1].strip().lower())
    elif n.startswith(("gfx/", "sprites/", "menu/")) and n.endswith((".tga", ".jpg")): refs.add(os.path.splitext(n)[0])
refs = {r.replace("\\", "/") for r in refs if r and not r.startswith("*")}

text = "".join(read(n).decode("latin1") for n in want if n.endswith(".shader"))
blocks = {}
for blk in re.finditer(r"^([^\s/{][^\n]*?)\s*\{(.*?)^\}", text, re.S | re.M):
    blocks.setdefault(blk.group(1).strip().lower(), blk.group(2))
need, seen, todo = set(), set(), list(refs)
while todo:
    r = todo.pop()
    if r in seen: continue
    seen.add(r)
    for ext in ("", ".tga", ".jpg"):
        if r + ext in src and (ext or r.endswith((".tga", ".jpg"))): need.add(r + ext)
    if r in blocks:
        for m in re.findall(r"(?:map|clampmap)\s+(\S+)|animmap\s+[\d.]+\s+([^\n]+)", blocks[r], re.I):
            for t in (x for g in m for x in g.split()):
                t = t.lower().replace("\\", "/")
                if t.startswith("$"): continue
                todo.append(os.path.splitext(t)[0]); need.add(t) if t in src else None
want |= need
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as o:
    for n in sorted(want):
        z, i = src[n]; o.writestr(i.filename, z.read(i))
print(out, len(want), "arquivos,", os.path.getsize(out), "bytes;", len(shaders), "shaders no BSP;", "personagens:", ",".join(players))
