#!/usr/bin/env python3
"""Gera os recursos binários do menu (assets/*.raw) a partir de uma fonte TrueType (Lato, licença OFL).
Formato: texturas TF_A8 (1 byte de alfa por texel, linha a linha).
  font.raw: 256x128, grade 16x8 de células 16x16 com os caracteres ASCII 0..127
  logo.raw: 512x64 com o texto "SNOWFLAME"
Uso: python3 tools/mkassets.py [pasta_das_fontes]"""
import sys
from PIL import Image, ImageDraw, ImageFont
d = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/lato"
reg = ImageFont.truetype(d + "/Lato-Medium.ttf", 14)
bold = ImageFont.truetype(d + "/Lato-Bold.ttf", 46)
atlas = Image.new("L", (256, 128), 0)
dr = ImageDraw.Draw(atlas)
adv = bytearray(128)
for c in range(32, 127):
    x, y = (c % 16) * 16, (c // 16) * 16
    w = dr.textlength(chr(c), font=reg)
    adv[c] = min(15, int(round(w)) if c != 32 else 4)
    dr.text((x + (16 - w) / 2, y + 0), chr(c), font=reg, fill=255)
atlas.save("assets/font.png")
open("assets/font.raw", "wb").write(atlas.tobytes())
open("assets/font.adv", "wb").write(bytes(adv))   # largura útil de cada caractere (pixels)
logo = Image.new("L", (512, 64), 0)
dl = ImageDraw.Draw(logo)
txt = "SNOWFLAME"
w = dl.textlength(txt, font=bold)
dl.text(((512 - w) / 2, 4), txt, font=bold, fill=255)
logo.save("assets/logo.png")
open("assets/logo.raw", "wb").write(logo.tobytes())
print("assets/font.raw", len(atlas.tobytes()), "assets/logo.raw", len(logo.tobytes()))
