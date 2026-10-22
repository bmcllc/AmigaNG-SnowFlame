#!/usr/bin/env python3
# Gera rtl/gen/sf100_abi_pkg.sv a partir de src/abi.h: o abi.h continua a fonte de verdade do mapa de memória, registradores,
# opcodes e bits de IRQ; o backend SystemVerilog só consome o pacote gerado (nunca editar à mão).
#   uso: tools/abi2sv.py src/abi.h rtl/gen/sf100_abi_pkg.sv
import re
import sys


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    return re.sub(r"//[^\n]*", " ", s)


def sv_expr(e):
    e = e.strip()
    e = re.sub(r"\b(0x[0-9A-Fa-f]+|\d+)[uU]?[lL]*\b", lambda m: num(m.group(1)), e)
    return e


def num(t):
    if t.lower().startswith("0x"):
        return "32'h" + t[2:].upper()
    return t


def main():
    src, dst = sys.argv[1], sys.argv[2]
    text = strip_comments(open(src, encoding="utf-8").read())
    out = []
    seen = set()

    def emit(name, expr):
        if name in seen:
            return
        seen.add(name)
        out.append("  localparam int unsigned %s = %s;" % (name, sv_expr(expr)))

    # constexpr <tipo> A = x, B = y;
    for m in re.finditer(r"constexpr\s+(?:u32|int|u64)\s+([^;]+);", text):
        for part in split_top(m.group(1)):
            k, _, v = part.partition("=")
            emit(k.strip(), v)
    # enum : u32 { A = 0, B, C = 4 };  (valores implícitos seguem o anterior + 1)
    for m in re.finditer(r"enum\s*:\s*u32\s*\{([^}]*)\}", text):
        prev = None
        for part in split_top(m.group(1)):
            part = part.strip()
            if not part:
                continue
            if "=" in part:
                k, _, v = part.partition("=")
                k = k.strip(); emit(k, v); prev = k
            else:
                emit(part, "%s + 1" % prev if prev else "0"); prev = part
    hdr = ("// GERADO por tools/abi2sv.py a partir de src/abi.h - não editar.\n"
           "// Constantes do ABI do SF-100 (mapa de memória, registradores, opcodes, IRQs) para o backend SystemVerilog.\n"
           "package sf100_abi;\n")
    open(dst, "w", encoding="utf-8").write(hdr + "\n".join(out) + "\nendpackage\n")


def split_top(s):
    parts, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur); cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    return parts


if __name__ == "__main__":
    main()
