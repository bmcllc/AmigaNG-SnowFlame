// Microprogramas SnowVector-128 de referência (geometria e dinâmica de partículas) e utilitários de matriz.
#pragma once
#include "common.h"
#include <string>

namespace sf { namespace kern {

// Layout da DMEM do kernel de geometria (slots de 16 bytes, base das constantes em s3; s0/s1/s2 = entrada/saída/fim)
//  0-3 MVP (colunas) | 4-6 matriz de normais (colunas) | 7 direção da luz | 8 escala viewport | 9 deslocamento viewport
//  10 (0,0,0,1) | 11 cor da luz | 12 zeros | 13 ambiente | 14 uns | vértices de entrada a partir de GEO_IN
constexpr u32 GEO_CONST_SLOTS = 15, GEO_IN = 0x100;

inline const char* geometry_src() {
  return R"(
    VLD v0,  [s3+0]
    VLD v1,  [s3+1]
    VLD v2,  [s3+2]
    VLD v3,  [s3+3]
    VLD v4,  [s3+4]
    VLD v5,  [s3+5]
    VLD v6,  [s3+6]
    VLD v7,  [s3+7]
    VLD v8,  [s3+8]
    VLD v9,  [s3+9]
    VLD v10, [s3+10]
    VLD v11, [s3+11]
    VLD v12, [s3+12]
    VLD v13, [s3+13]
    VLD v14, [s3+14]
  loop:
    VLD v16, [s0+0]          ; posição
    VLD v17, [s0+1]          ; normal
    VLD v18, [s0+2]          ; u, v, fog
    VMUL v20, v0, v16.xxxx   ; clip = M * pos
    VMAC v20, v1, v16.yyyy
    VMAC v20, v2, v16.zzzz
    VMAC v20, v3, v16.wwww
    VRCP v21, v20.wwww       ; 1/w
    VMUL v23, v20, v21       ; NDC
    VMUL v22, v23, v8        ; viewport
    VADD v22, v22, v9
    VMAC v22, v21, v10       ; w <- 1/w
    VMUL v24, v4, v17.xxxx   ; normal rotacionada
    VMAC v24, v5, v17.yyyy
    VMAC v24, v6, v17.zzzz
    VDOT3 v25, v24, v7       ; N.L
    VMAX v25, v25, v12       ; >= 0
    VMUL v26, v11, v25
    VADD v26, v26, v13       ; + ambiente
    VMIN v26, v26, v14       ; <= 1
    VST [s1+0], v22
    VST [s1+1], v26
    VST [s1+2], v18
    SADD s0, s0, 48
    SADD s1, s1, 48
    SCMP s0, s2
    BRLT loop
    HALT
  )";
}

// Partículas (SnowVector B): registro = pos(vec4) + vel(vec4). Constantes: 0 dt | 1 g | 2 zeros | 3 máscara Y | 4 (0,-2,0,0) | 5 grande | 6 -1 | 7 restituição
constexpr u32 PART_IN = 0x100;
inline const char* particles_src() {
  return R"(
    SETA s3, 0
    VLD v1, [s3+0]
    VLD v2, [s3+1]
    VLD v3, [s3+2]
    VLD v4, [s3+3]
    VLD v5, [s3+4]
    VLD v6, [s3+5]
    VLD v7, [s3+6]
    VLD v8, [s3+7]
  loop:
    VLD v10, [s0+0]
    VLD v11, [s0+1]
    VMAC v10, v11, v1        ; p += v*dt
    VMAC v11, v2, v1         ; v += g*dt
    VMIN v12, v10, v3        ; min(p,0)
    VMUL v12, v12, v4        ; só o eixo Y
    VMAC v10, v12, v5        ; reflete: y <- -y
    VMUL v13, v12, v6        ; * grande
    VMAX v13, v13, v7        ; s em [-1,0]
    VABS v14, v11
    VMUL v14, v14, v8        ; e*|v|
    VSUB v14, v11, v14       ; v - e|v|
    VMAC v11, v13, v14       ; v += s*(v - e|v|)
    VST [s0+0], v10
    VST [s0+1], v11
    SADD s0, s0, 32
    SCMP s0, s2
    BRLT loop
    HALT
  )";
}

struct Mat4 { double m[4][4]; };   // m[linha][coluna], convenção coluna-vetor
inline Mat4 identity() { Mat4 r{}; for (int i = 0; i < 4; i++) r.m[i][i] = 1; return r; }
inline Mat4 mul(const Mat4& a, const Mat4& b) { Mat4 r{}; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) for (int k = 0; k < 4; k++) r.m[i][j] += a.m[i][k] * b.m[k][j]; return r; }
inline Mat4 translate(double x, double y, double z) { Mat4 r = identity(); r.m[0][3] = x; r.m[1][3] = y; r.m[2][3] = z; return r; }
inline Mat4 rotate_y(double a) { Mat4 r = identity(); r.m[0][0] = std::cos(a); r.m[0][2] = std::sin(a); r.m[2][0] = -std::sin(a); r.m[2][2] = std::cos(a); return r; }
inline Mat4 rotate_x(double a) { Mat4 r = identity(); r.m[1][1] = std::cos(a); r.m[1][2] = -std::sin(a); r.m[2][1] = std::sin(a); r.m[2][2] = std::cos(a); return r; }
inline Mat4 perspective(double fovy, double aspect, double n, double f) {
  Mat4 r{}; double t = 1.0 / std::tan(fovy / 2);
  r.m[0][0] = t / aspect; r.m[1][1] = t; r.m[2][2] = (f + n) / (n - f); r.m[2][3] = 2 * f * n / (n - f); r.m[3][2] = -1; return r;
}

}}  // namespace sf::kern
