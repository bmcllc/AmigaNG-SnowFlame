#!/usr/bin/env python3
"""Gera dados sintéticos para testar o renderer do SF-100 sem os dados de nenhum jogo: um BSP Q3 v46 (sala com lightmaps, um patch curvo, uma pirâmide
com cor de vértice), texturas TGA/JPG, um script de shader e um MD3 simples.  Uso: tools/mkmap.py pasta_de_saida"""
import sys, os, struct, math, zlib
from PIL import Image, ImageDraw
out = sys.argv[1] if len(sys.argv) > 1 else "renderdata"
def path(p):
    q = os.path.join(out, p); os.makedirs(os.path.dirname(q), exist_ok=True); return q

# ---------------- texturas ----------------
def checker(n, c1, c2, cell):
    im = Image.new("RGB", (n, n))
    d = ImageDraw.Draw(im)
    for y in range(0, n, cell):
        for x in range(0, n, cell):
            d.rectangle([x, y, x + cell - 1, y + cell - 1], fill=c1 if ((x // cell) + (y // cell)) % 2 == 0 else c2)
    return im
checker(64, (190, 70, 50), (230, 200, 150), 8).save(path("textures/test/wall.tga"))
im = checker(128, (70, 90, 70), (110, 130, 100), 16)
d = ImageDraw.Draw(im)
for i in range(0, 128, 8): d.line([(i, 0), (127 - i, 127)], fill=(60, 80, 60))
im.save(path("textures/test/floor.jpg"), quality=90)
fence = Image.new("RGBA", (64, 64), (0, 0, 0, 0)); d = ImageDraw.Draw(fence)
for x in range(4, 64, 16): d.rectangle([x, 0, x + 5, 63], fill=(200, 160, 60, 255))
d.rectangle([0, 28, 63, 34], fill=(200, 160, 60, 255)); fence.save(path("textures/test/fence.tga"))
glow = Image.new("RGB", (64, 64), (0, 0, 0)); d = ImageDraw.Draw(glow)
for r in range(30, 0, -1): c = int(255 * (1 - r / 30.0) ** 1.5); d.ellipse([32 - r, 32 - r, 32 + r, 32 + r], fill=(c, int(c * 0.6), int(c * 0.2)))
glow.save(path("textures/test/glow.tga"))
bar = Image.new("RGBA", (64, 16), (30, 30, 60, 200)); ImageDraw.Draw(bar).rectangle([0, 0, 63, 1], fill=(255, 255, 255, 255)); bar.save(path("gfx/2d/bar.tga"))
open(path("scripts/test.shader"), "w").write("""// shaders sinteticos de teste
textures/test/fence
{
	cull none
	surfaceparm trans
	{
		map textures/test/fence.tga
		alphaFunc GE128
		rgbGen identity
	}
	{
		map $lightmap
		blendFunc filter
		depthFunc equal
	}
}

textures/test/glowpad
{
	surfaceparm nolightmap
	{
		map textures/test/glow.tga
		blendFunc add
		rgbGen identity
		tcMod rotate 40
	}
}

textures/test/lavafloor
{
	{
		map $lightmap
		rgbGen identity
	}
	{
		map textures/test/floor.jpg
		blendFunc filter
		tcMod scroll 0.1 0.05
	}
}
""")

# ---------------- BSP ----------------
verts = []; indexes = []; surfaces = []; shaders = []
def shader_index(name):
    if name not in shaders: shaders.append(name)
    return shaders.index(name)
def dv(xyz, st, lm, n, col=(255, 255, 255, 255)): return struct.pack("<3f2f2f3f4B", *xyz, *st, *lm, *n, *col)
def add_surface(shader, stype, vs, idx, lmnum, patch=(0, 0), n=(0, 0, 1)):   # n: normal da face planar (lightmapVecs[2], como o q3map)
    surfaces.append(struct.pack("<12i3f9f2i", shader_index(shader), -1, stype, len(verts), len(vs), len(indexes), len(idx), lmnum, 0, 0, 128, 128,
                                0, 0, 0, 1, 0, 0, 0, 1, 0, *n, patch[0], patch[1]))
    verts.extend(vs); indexes.extend(idx)
def quad(shader, p0, p1, p2, p3, n, uv_scale, lm_rect, lm=0):
    (lx0, ly0, lx1, ly1) = lm_rect
    vs = [dv(p0, (0, 0), (lx0, ly0), n), dv(p1, (uv_scale, 0), (lx1, ly0), n), dv(p2, (uv_scale, uv_scale), (lx1, ly1), n), dv(p3, (0, uv_scale), (lx0, ly1), n)]
    base = 0
    add_surface(shader, 1, vs, [0, 2, 1, 0, 3, 2], lm, n=n)   # enrolamento do Q3 (horário visto da frente): o renderer descarta faces de costas
R = 256; H = 192
# lightmap 0: gradiente quente no chão/paredes; lightmap 1: tom azulado
lm = []
for k, (r0, g0, b0) in enumerate([(120, 110, 100), (80, 100, 140)]):
    img = bytearray()
    for y in range(128):
        for x in range(128):
            f = 0.35 + 0.65 * (1 - math.hypot(x - 64, y - 64) / 90.0); f = max(0.15, min(1.0, f))
            img += bytes((int(r0 * f * 1.6) & 255 if r0 * f * 1.6 < 256 else 255, int(g0 * f * 1.6) if g0 * f * 1.6 < 256 else 255, int(b0 * f * 1.6) if b0 * f * 1.6 < 256 else 255))
    lm.append(bytes(img))
# chão, teto, quatro paredes (normais para dentro), cada um em sua região do lightmap
quad("textures/test/lavafloor", (-R, -R, 0), (R, -R, 0), (R, R, 0), (-R, R, 0), (0, 0, 1), 4, (0, 0, 1, 1), 0)
quad("textures/test/wall", (-R, R, H), (R, R, H), (R, -R, H), (-R, -R, H), (0, 0, -1), 4, (0, 0, 1, 1), 1)
quad("textures/test/wall", (-R, -R, 0), (-R, -R, H), (R, -R, H), (R, -R, 0), (0, 1, 0), 4, (0, 0, 1, 1), 0)
quad("textures/test/wall", (R, R, 0), (R, R, H), (-R, R, H), (-R, R, 0), (0, -1, 0), 4, (0, 0, 1, 1), 1)
quad("textures/test/wall", (-R, R, 0), (-R, R, H), (-R, -R, H), (-R, -R, 0), (1, 0, 0), 4, (0, 0, 1, 1), 0)
quad("textures/test/wall", (R, -R, 0), (R, -R, H), (R, R, H), (R, R, 0), (-1, 0, 0), 4, (0, 0, 1, 1), 1)
# cerca (alpha test) no meio da sala e uma "poça" aditiva no chão
quad("textures/test/fence", (-80, 40, 0), (-80, 40, 120), (80, 40, 120), (80, 40, 0), (0, -1, 0), 2, (0.2, 0.2, 0.8, 0.8), 0)
quad("textures/test/glowpad", (-60, -140, 1), (60, -140, 1), (60, -20, 1), (-60, -20, 1), (0, 0, 1), 1, (0, 0, 1, 1), -1)
# patch de Bezier 3x5: meio cilindro "arco" (3 pontos de controle por seção, 5 seções)
cp = []
for j in range(5):
    y0 = 100 + j * 20
    for i, (x, z) in enumerate([(-100, 0), (-100, 120), (0, 120)]):
        pass
pts = []
for j in range(3):
    for i in range(5):
        a = math.pi * i / 4.0
        x = 150 + 60 * math.cos(a); z = 60 * math.sin(a) + 2
        # controles quadráticos de um arco: usa pontos do arco (aproximação)
        pts.append((x, 120 + j * 80, z))
vs = []
for j in range(3):
    for i in range(5):
        x, y, z = pts[j * 5 + i]
        vs.append(dv((x, y, z), (i / 4.0 * 2, j / 2.0 * 2), (0.3, 0.3), (0, 0, 1)))
add_surface("textures/test/wall", 2, vs, [], 0, patch=(5, 3))
# pirâmide com cor de vértice (soup), sem lightmap
apex = (-170, -150, 110); b = [(-220, -200, 0), (-120, -200, 0), (-120, -100, 0), (-220, -100, 0)]
cols = [(255, 80, 60, 255), (80, 255, 90, 255), (80, 120, 255, 255), (255, 230, 80, 255)]
vs = []; idx = []
for k in range(4):
    p1, p2 = b[k], b[(k + 1) % 4]
    base = len(vs)
    vs += [dv(apex, (0.5, 0), (0, 0), (0, 0, 1), (255, 255, 255, 255)), dv(p1, (0, 1), (0, 0), (0, 0, 1), cols[k]), dv(p2, (1, 1), (0, 0), (0, 0, 1), cols[(k + 1) % 4])]
    idx += [base, base + 1, base + 2]
add_surface("textures/test/wall", 3, vs, idx, -1)

def lump(data): return data
ents = b'{\n"classname" "worldspawn"\n"message" "teste sintetico do SF-100"\n}\n{\n"classname" "info_player_start"\n"origin" "0 -200 40"\n"angle" "90"\n}\n\x00'
shdr = b"".join(struct.pack("<64s2i", n.encode(), 0, 0) for n in shaders)
planes = struct.pack("<4f", 1, 0, 0, 0)
nodes = struct.pack("<3i3i3i", 0, -1, -2, -400, -400, -50, 400, 400, 300)
leaf = lambda cl, a, b_: struct.pack("<2i3i3i4i", cl, 0, -400, -400, -50, 400, 400, 300, a, b_, 0, 0)
nsurf = len(surfaces)
leafs = leaf(0, 0, nsurf) + leaf(1, 0, nsurf)
leafsurf = struct.pack("<%di" % nsurf, *range(nsurf))
model = struct.pack("<6f4i", -R, -R, 0, R, R, H, 0, nsurf, 0, 0)
visdata = struct.pack("<2i", 2, 1) + bytes([3, 3])
lumps = [ents, shdr, planes, nodes, leafs, leafsurf, b"", model, b"", b"", b"".join(verts), struct.pack("<%di" % len(indexes), *indexes), b"", b"".join(surfaces), b"".join(lm), b"", visdata]
off = 8 + 17 * 8; table = b""; body = b""
for l in lumps:
    pad = (-len(l)) % 4
    table += struct.pack("<2i", off + len(body), len(l)); body += l + b"\0" * pad
open(path("maps/test.bsp"), "wb").write(b"IBSP" + struct.pack("<i", 46) + table + body)

# ---------------- MD3: cubo com 2 frames (o segundo maior) e uma tag ----------------
def md3():
    # cubo de 24 vértices (4 por face, UV 0..1 por face), 12 triângulos, 2 frames (o segundo maior) e uma tag
    faces = [((-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)), ((-1, 1, -1), (1, 1, -1), (1, -1, -1), (-1, -1, -1)),
             ((-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)), ((1, 1, -1), (-1, 1, -1), (-1, 1, 1), (1, 1, 1)),
             ((1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1)), ((-1, 1, -1), (-1, -1, -1), (-1, -1, 1), (-1, 1, 1))]
    uvs = [(0, 1), (1, 1), (1, 0), (0, 0)]
    cubev = [v for f in faces for v in f]; sts = uvs * 6
    tris = []
    for f in range(6): b0 = f * 4; tris += [(b0, b0 + 2, b0 + 1), (b0, b0 + 3, b0 + 2)]   # enrolamento do MD3 do Q3 (renderer descarta costas)
    nframes = 2; scale = [20, 28]; nv = len(cubev)
    frames = b"".join(struct.pack("<3f3f3ff16s", -sc, -sc, -sc, sc, sc, sc, 0, 0, 0, sc * 1.8, ("f%d" % i).encode()) for i, sc in enumerate(scale))
    tag = lambda name, org: struct.pack("<64s3f9f", name.encode(), *org, 1, 0, 0, 0, 1, 0, 0, 0, 1)
    tags = tag("tag_top", (0, 0, 20)) + tag("tag_top", (0, 0, 28))
    xyz = b""
    for f in range(nframes):
        for v in cubev: xyz += struct.pack("<3hH", int(v[0] * scale[f] * 64), int(v[1] * scale[f] * 64), int(v[2] * scale[f] * 64), 0)
    shader = struct.pack("<64si", b"textures/test/wall", 0)
    header = 108; surf_hdr = 108
    ofs_tris = surf_hdr + len(shader); ofs_st = ofs_tris + len(tris) * 12; ofs_xyz = ofs_st + len(sts) * 8; ofs_end = ofs_xyz + len(xyz)
    surf = struct.pack("<i64siiiiiiiiii", 0x33504449, b"cube", 0, nframes, 1, nv, len(tris), ofs_tris, surf_hdr, ofs_st, ofs_xyz, ofs_end)
    surf += shader + b"".join(struct.pack("<3i", *t) for t in tris) + b"".join(struct.pack("<2f", *s_) for s_ in sts) + xyz
    ofs_frames = header; ofs_tags = ofs_frames + len(frames); ofs_surf = ofs_tags + len(tags); ofs_end2 = ofs_surf + len(surf)
    hd = struct.pack("<2i64s9i", 0x33504449, 15, b"models/test/cube.md3", 0, nframes, 1, 1, 0, ofs_frames, ofs_tags, ofs_surf, ofs_end2)
    return hd + frames + tags + surf
open(path("models/test/cube.md3"), "wb").write(md3())
print(out, "pronto")
