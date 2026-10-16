// Utilitários de textura do lado do desenvolvedor: codificador SFTC1 (DXT1 com palavras de cor big-endian),
// geração de mipmaps e conversões de pixel. O hardware (FlameCore) só decodifica.
#pragma once
#include "common.h"
#include <array>
#include <set>

namespace sf {

inline u32 pack_argb(int a, int r, int g, int b) { return ((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | (u32)b; }
inline u16 to565(int r, int g, int b) { return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }
inline void from565(u16 c, int& r, int& g, int& b) {
  r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31;
  r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
}

// Decodificador ETC1 (Khronos): bloco de 8 bytes, texel (x,y) do bloco 4x4. Índice do pixel: bit = x*4+y; MSB = sinal, LSB = magnitude.
inline u32 etc1_texel(const u8* b, int x, int y) {
  static const int tbl[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
  u32 hi = rbe32(b), lo = rbe32(b + 4); bool diff = (hi >> 1) & 1, flip = hi & 1; int cw[2] = {(int)((hi >> 5) & 7), (int)((hi >> 2) & 7)};
  int c[2][3];
  if (!diff) {
    int sh[3] = {28, 20, 12};
    for (int k = 0; k < 3; k++) { c[0][k] = ((hi >> sh[k]) & 15) * 17; c[1][k] = ((hi >> (sh[k] - 4)) & 15) * 17; }
  } else {
    int sh[3] = {27, 19, 11};
    for (int k = 0; k < 3; k++) {
      int b1 = (hi >> sh[k]) & 31, d = (hi >> (sh[k] - 3)) & 7; if (d & 4) d -= 8; int b2 = std::min(31, std::max(0, b1 + d));
      c[0][k] = (b1 << 3) | (b1 >> 2); c[1][k] = (b2 << 3) | (b2 >> 2);
    }
  }
  int sub = flip ? (y >= 2) : (x >= 2), k = x * 4 + y; int msb = (lo >> (16 + k)) & 1, lsb = (lo >> k) & 1;
  int mod = tbl[cw[sub]][lsb]; if (msb) mod = -mod;
  auto cl = [](int v) { return v < 0 ? 0 : v > 255 ? 255 : v; };
  return 0xFF000000u | ((u32)cl(c[sub][0] + mod) << 16) | ((u32)cl(c[sub][1] + mod) << 8) | (u32)cl(c[sub][2] + mod);
}

// Bytes por mip nível para um formato (0 ARGB8888, 1 RGB565, 2 ARGB4444, 3 DXT1, 4 DXT3, 5 DXT5, 6 RGBA5551, 7 L8, 8 A8, 9 LA88, 10 CI8, 11 CI4, 12 VQ, 13 ETC1)
inline u32 tex_level_bytes(int fmt, int w, int h) {
  if (fmt == 0) return (u32)w * h * 4;
  if (fmt == 3) return (u32)std::max(1, (w + 3) / 4) * std::max(1, (h + 3) / 4) * 8;
  if (fmt == 4 || fmt == 5) return (u32)std::max(1, (w + 3) / 4) * std::max(1, (h + 3) / 4) * 16;
  if (fmt == 6 || fmt == 9) return (u32)w * h * 2;                                   // RGBA5551, LA88
  if (fmt == 7 || fmt == 8 || fmt == 10) return (u32)w * h;                          // L8, A8, CI8
  if (fmt == 11) return (u32)((w + 1) / 2) * h;                                      // CI4
  if (fmt == 12) return 2048 + (u32)std::max(1, (w + 1) / 2) * std::max(1, (h + 1) / 2);   // VQ
  if (fmt == 13) return (u32)std::max(1, (w + 3) / 4) * std::max(1, (h + 3) / 4) * 8;      // ETC1
  return (u32)w * h * 2;
}

// Reduz por filtro de caixa uma imagem ARGB.
inline std::vector<u32> box_down(const std::vector<u32>& src, int w, int h) {
  int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
  std::vector<u32> out((size_t)nw * nh);
  for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) {
    int a = 0, r = 0, g = 0, b = 0, n = 0;
    for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
      int sx = std::min(w - 1, x * 2 + dx), sy = std::min(h - 1, y * 2 + dy); u32 p = src[(size_t)sy * w + sx];
      a += p >> 24; r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; n++;
    }
    out[(size_t)y * nw + x] = pack_argb(a / n, r / n, g / n, b / n);
  }
  return out;
}

// Serializa pixels ARGB (w x h) em bytes de formato fmt (big-endian).
inline std::vector<u8> encode_level(const std::vector<u32>& px, int w, int h, int fmt, const std::vector<u32>* pal = nullptr);

// ---- quantização vetorial (LBG): dados N x D -> até K centroides; exato quando há <= K vetores distintos ----
template <int D> std::vector<std::array<float, D>> lbg(const std::vector<std::array<float, D>>& data, int K) {
  typedef std::array<float, D> V;
  std::set<V> uniq(data.begin(), data.end());
  if ((int)uniq.size() <= K) return std::vector<V>(uniq.begin(), uniq.end());
  V mean{}; for (auto& v : data) for (int i = 0; i < D; i++) mean[i] += v[i];
  for (int i = 0; i < D; i++) mean[i] /= (float)data.size();
  std::vector<V> cb{mean}; std::vector<int> asg(data.size());
  auto lloyd = [&](int iters) {
    for (int it = 0; it < iters; it++) {
      std::vector<V> sum(cb.size()); std::vector<int> cnt(cb.size(), 0);
      for (size_t n = 0; n < data.size(); n++) {
        int best = 0; float bd = 1e30f;
        for (size_t k = 0; k < cb.size(); k++) { float d = 0; for (int i = 0; i < D; i++) { float t = data[n][i] - cb[k][i]; d += t * t; } if (d < bd) { bd = d; best = (int)k; } }
        asg[n] = best; cnt[best]++; for (int i = 0; i < D; i++) sum[best][i] += data[n][i];
      }
      for (size_t k = 0; k < cb.size(); k++) {
        if (cnt[k]) for (int i = 0; i < D; i++) cb[k][i] = sum[k][i] / (float)cnt[k];
        else cb[k] = data[(k * 7919 + it * 104729) % data.size()];       // cluster vazio: reaproveita um vetor (determinístico)
      }
    }
  };
  std::fill(asg.begin(), asg.end(), 0);
  while ((int)cb.size() < K) {
    // divide cada centroide ao longo da dimensão de maior variância do seu cluster
    std::vector<V> var(cb.size()); std::vector<int> cnt(cb.size(), 0);
    for (size_t n = 0; n < data.size(); n++) { int k = asg[n]; cnt[k]++; for (int i = 0; i < D; i++) { float t = data[n][i] - cb[k][i]; var[k][i] += t * t; } }
    std::vector<V> nb;
    for (size_t k = 0; k < cb.size(); k++) {
      int dm = 0; for (int i = 1; i < D; i++) if (var[k][i] > var[k][dm]) dm = i;
      float off = cnt[k] ? std::max(0.5f, 0.5f * std::sqrt(var[k][dm] / (float)cnt[k])) : 1.0f;
      V a = cb[k], b = cb[k]; a[dm] += off; b[dm] -= off; nb.push_back(a); nb.push_back(b);
    }
    if ((int)nb.size() > K) nb.resize(K);
    cb = nb; lloyd(8);
  }
  lloyd(8);
  return cb;
}
// Paleta de até n entradas ARGB para a imagem.
inline std::vector<u32> quantize_palette(const std::vector<u32>& px, int n) {
  std::vector<std::array<float, 4>> d; d.reserve(px.size());
  for (u32 p : px) d.push_back({(float)(p >> 24), (float)((p >> 16) & 255), (float)((p >> 8) & 255), (float)(p & 255)});
  auto cb = lbg<4>(d, n); std::vector<u32> pal;
  for (auto& c : cb) { auto q = [](float v) { return (int)std::lround(std::min(255.0f, std::max(0.0f, v))); }; pal.push_back(pack_argb(q(c[0]), q(c[1]), q(c[2]), q(c[3]))); }
  return pal;
}
inline int nearest_pal(const std::vector<u32>& pal, u32 p) {
  int best = 0; long bd = 1L << 60;
  for (size_t k = 0; k < pal.size(); k++) {
    long d = 0; for (int sh = 0; sh < 32; sh += 8) { long t = (long)((p >> sh) & 255) - (long)((pal[k] >> sh) & 255); d += t * t; }
    if (d < bd) { bd = d; best = (int)k; }
  }
  return best;
}

// Codificador ETC1: testa as duas orientações e os modos individual (4+4 bits) e diferencial (5+3 bits); escolhe a tabela de cada metade por força bruta.
inline void etc1_encode_block(const u32 px[16], u8* out) {
  static const int tbl[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
  auto ch = [](u32 p, int k) { return (int)((p >> (16 - 8 * k)) & 255); };
  auto cl = [](int v) { return v < 0 ? 0 : v > 255 ? 255 : v; };
  struct Best { long err = 1L << 60; u32 hi = 0, lo = 0; } best;
  for (int flip = 0; flip < 2; flip++) {
    int sp[2][8], n[2] = {0, 0};
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { int sb = flip ? (y >= 2) : (x >= 2); sp[sb][n[sb]++] = x * 4 + y; }   // índice k = x*4+y
    int avg[2][3];
    for (int sb = 0; sb < 2; sb++) for (int k = 0; k < 3; k++) { int sum = 0; for (int i = 0; i < 8; i++) { int kk = sp[sb][i]; sum += ch(px[(kk % 4) * 4 + kk / 4], k); } avg[sb][k] = (sum + 4) / 8; }
    // pixel (x,y) está em px[y*4+x]; k = x*4+y  =>  x = k/4, y = k%4  => px index = (k%4)*4 + k/4
    for (int mode = 0; mode < 2; mode++) {
      int q[2][3], base[2][3]; bool ok = true;
      if (mode == 0) { for (int sb = 0; sb < 2; sb++) for (int k = 0; k < 3; k++) { q[sb][k] = (avg[sb][k] * 15 + 127) / 255; base[sb][k] = q[sb][k] * 17; } }
      else {
        for (int k = 0; k < 3; k++) {
          q[0][k] = (avg[0][k] * 31 + 127) / 255; int q2 = (avg[1][k] * 31 + 127) / 255; int d = q2 - q[0][k];
          if (d < -4 || d > 3) { ok = false; break; }
          q[1][k] = q2; base[0][k] = (q[0][k] << 3) | (q[0][k] >> 2); base[1][k] = (q2 << 3) | (q2 >> 2);
        }
        if (!ok) continue;
      }
      long total = 0; int cwsel[2]; u32 idxbits[2] = {0, 0};   // idxbits: bits (msb<<16 | lsb) por k
      for (int sb = 0; sb < 2; sb++) {
        long bsum = 1L << 60; int bcw = 0; u32 bbits = 0;
        for (int cw = 0; cw < 8; cw++) {
          long sum = 0; u32 bits = 0;
          for (int i = 0; i < 8; i++) {
            int kk = sp[sb][i]; u32 p = px[(kk % 4) * 4 + kk / 4]; long be = 1L << 60; int bi = 0;
            for (int idx = 0; idx < 4; idx++) {
              int mod = tbl[cw][idx & 1]; if (idx & 2) mod = -mod;
              long e = 0; for (int k = 0; k < 3; k++) { long t = cl(base[sb][k] + mod) - ch(p, k); e += t * t; }
              if (e < be) { be = e; bi = idx; }
            }
            sum += be; bits |= ((u32)(bi >> 1) << (16 + kk)) | ((u32)(bi & 1) << kk);
          }
          if (sum < bsum) { bsum = sum; bcw = cw; bbits = bits; }
        }
        total += bsum; cwsel[sb] = bcw; idxbits[sb] = bbits;
      }
      if (total < best.err) {
        u32 hi = 0;
        if (mode == 0) hi = ((u32)q[0][0] << 28) | ((u32)q[1][0] << 24) | ((u32)q[0][1] << 20) | ((u32)q[1][1] << 16) | ((u32)q[0][2] << 12) | ((u32)q[1][2] << 8);
        else hi = ((u32)q[0][0] << 27) | ((u32)((q[1][0] - q[0][0]) & 7) << 24) | ((u32)q[0][1] << 19) | ((u32)((q[1][1] - q[0][1]) & 7) << 16) | ((u32)q[0][2] << 11) | ((u32)((q[1][2] - q[0][2]) & 7) << 8);
        hi |= ((u32)cwsel[0] << 5) | ((u32)cwsel[1] << 2) | (mode ? 2u : 0u) | (flip ? 1u : 0u);
        best.err = total; best.hi = hi; best.lo = idxbits[0] | idxbits[1];
      }
    }
  }
  wbe32(out, best.hi); wbe32(out + 4, best.lo);
}

// Codificador VQ: livro de 256 blocos 2x2 (RGB565) + índices.
inline std::vector<u8> vq_encode(const std::vector<u32>& px, int w, int h) {
  int bw = std::max(1, (w + 1) / 2), bh = std::max(1, (h + 1) / 2);
  std::vector<std::array<float, 12>> d; d.reserve((size_t)bw * bh);
  auto at = [&](int x, int y) { return px[(size_t)std::min(h - 1, y) * w + std::min(w - 1, x)]; };
  for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
    std::array<float, 12> v; int i = 0;
    for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) { u32 p = at(bx * 2 + dx, by * 2 + dy); int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255; int rr, gg, bb; from565(to565(r, g, b), rr, gg, bb); v[i++] = (float)rr; v[i++] = (float)gg; v[i++] = (float)bb; }
    d.push_back(v);
  }
  auto cb = lbg<12>(d, 256);
  std::vector<u8> out(2048 + (size_t)bw * bh, 0);
  std::vector<std::array<int, 12>> cbi(cb.size());
  for (size_t k = 0; k < cb.size(); k++) {
    for (int i = 0; i < 12; i++) cbi[k][i] = (int)std::lround(std::min(255.0f, std::max(0.0f, cb[k][i])));
    for (int t = 0; t < 4; t++) wbe16(&out[k * 8 + t * 2], to565(cbi[k][t * 3], cbi[k][t * 3 + 1], cbi[k][t * 3 + 2]));
  }
  for (size_t n = 0; n < d.size(); n++) {
    int best = 0; float bd = 1e30f;
    for (size_t k = 0; k < cb.size(); k++) { float e = 0; for (int i = 0; i < 12; i++) { float t = d[n][i] - (float)cbi[k][i]; e += t * t; } if (e < bd) { bd = e; best = (int)k; } }
    out[2048 + n] = (u8)best;
  }
  return out;
}
// DXT3/DXT5 (SFTC3/SFTC5): 8 bytes de alfa + 8 bytes de cor (sempre no modo de 4 cores), palavras big-endian.
inline std::vector<u8> encode_dxt35(const std::vector<u32>& px, int w, int h, int fmt) {
  std::vector<u32> opaque(px); for (auto& p : opaque) p |= 0xFF000000u;
  auto col = encode_level(opaque, w, h, 3);
  int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
  std::vector<u8> out((size_t)bw * bh * 16, 0);
  for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
    u8* o = &out[((size_t)by * bw + bx) * 16]; int al[16];
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) al[y * 4 + x] = px[(size_t)std::min(h - 1, by * 4 + y) * w + std::min(w - 1, bx * 4 + x)] >> 24;
    if (fmt == 4) {
      for (int y = 0; y < 4; y++) { u16 row = 0; for (int x = 0; x < 4; x++) row |= (u16)((al[y * 4 + x] >> 4) << (4 * x)); wbe16(o + 2 * y, row); }
    } else {
      int mn = 255, mx = 0; for (int i = 0; i < 16; i++) { mn = std::min(mn, al[i]); mx = std::max(mx, al[i]); }
      o[0] = (u8)mx; o[1] = (u8)mn;                          // a0 > a1: 8 níveis; a0 == a1: 6 níveis + 0/255 (alfa constante)
      int lv[8]; lv[0] = mx; lv[1] = mn; if (mx > mn) for (int k = 1; k <= 6; k++) lv[1 + k] = ((7 - k) * mx + k * mn) / 7;
      u64 bits = 0;
      for (int i = 0; i < 16; i++) {
        int best = 0, bd = 1 << 30; for (int k = 0; k < (mx > mn ? 8 : 2); k++) { int d = std::abs(al[i] - lv[k]); if (d < bd) { bd = d; best = k; } }
        bits |= (u64)best << (3 * i);
      }
      for (int k = 0; k < 6; k++) o[2 + k] = (u8)(bits >> (8 * (5 - k)));
    }
    std::memcpy(o + 8, &col[((size_t)by * bw + bx) * 8], 8);
  }
  return out;
}
inline std::vector<u8> encode_level(const std::vector<u32>& px, int w, int h, int fmt, const std::vector<u32>* palp) {
  if (fmt == 4 || fmt == 5) return encode_dxt35(px, w, h, fmt);
  if (fmt >= 6 && fmt <= 13) {
    std::vector<u8> o(tex_level_bytes(fmt, w, h), 0);
    auto lum = [](u32 p) { return (int)((((p >> 16) & 255) * 77 + ((p >> 8) & 255) * 150 + (p & 255) * 29) >> 8); };
    if (fmt == 6) for (size_t i = 0; i < px.size(); i++) { u32 p = px[i]; wbe16(&o[i * 2], (u16)(((((p >> 16) & 255) >> 3) << 11) | ((((p >> 8) & 255) >> 3) << 6) | (((p & 255) >> 3) << 1) | ((p >> 24) >= 128 ? 1 : 0))); }
    else if (fmt == 7) for (size_t i = 0; i < px.size(); i++) o[i] = (u8)lum(px[i]);
    else if (fmt == 8) for (size_t i = 0; i < px.size(); i++) o[i] = (u8)(px[i] >> 24);
    else if (fmt == 9) for (size_t i = 0; i < px.size(); i++) wbe16(&o[i * 2], (u16)(((px[i] >> 24) << 8) | lum(px[i])));
    else if (fmt == 10 || fmt == 11) {
      std::vector<u32> pal = palp ? *palp : quantize_palette(px, fmt == 10 ? 256 : 16);
      for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int k = nearest_pal(pal, px[(size_t)y * w + x]);
        if (fmt == 10) o[(size_t)y * w + x] = (u8)k; else { u8& b = o[(size_t)y * ((w + 1) / 2) + (x >> 1)]; b |= (x & 1) ? (u8)k : (u8)(k << 4); }
      }
    } else if (fmt == 12) return vq_encode(px, w, h);
    else {
      int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
      for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
        u32 blk[16]; for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) blk[y * 4 + x] = px[(size_t)std::min(h - 1, by * 4 + y) * w + std::min(w - 1, bx * 4 + x)];
        etc1_encode_block(blk, &o[((size_t)by * bw + bx) * 8]);
      }
    }
    return o;
  }
  std::vector<u8> out(tex_level_bytes(fmt, w, h), 0);
  if (fmt == 0) { for (size_t i = 0; i < px.size(); i++) wbe32(&out[i * 4], px[i]); }
  else if (fmt == 1) { for (size_t i = 0; i < px.size(); i++) wbe16(&out[i * 2], to565((px[i] >> 16) & 255, (px[i] >> 8) & 255, px[i] & 255)); }
  else if (fmt == 2) {
    for (size_t i = 0; i < px.size(); i++) {
      u32 p = px[i]; wbe16(&out[i * 2], (u16)(((p >> 28) << 12) | ((((p >> 16) & 255) >> 4) << 8) | ((((p >> 8) & 255) >> 4) << 4) | ((p & 255) >> 4)));
    }
  } else if (fmt == 3) {
    int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
    for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
      bool anyT = false; u32 blk[16]; int ci[16], nci = 0;
      for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        int sx = std::min(w - 1, bx * 4 + x), sy = std::min(h - 1, by * 4 + y); u32 p = px[(size_t)sy * w + sx]; blk[y * 4 + x] = p;
        if ((p >> 24) < 128) anyT = true; else ci[nci++] = y * 4 + x;
      }
      // endpoints = par de cores opacas mais distantes do bloco
      int mnr = 0, mng = 0, mnb = 0, mxr = 0, mxg = 0, mxb = 0, bestd = -1;
      for (int i = 0; i < nci; i++) for (int j = i; j < nci; j++) {
        u32 a = blk[ci[i]], b = blk[ci[j]]; int dr = (int)((a >> 16) & 255) - (int)((b >> 16) & 255), dg = (int)((a >> 8) & 255) - (int)((b >> 8) & 255), db = (int)(a & 255) - (int)(b & 255);
        int d = dr * dr + dg * dg + db * db;
        if (d > bestd) { bestd = d; mxr = (a >> 16) & 255; mxg = (a >> 8) & 255; mxb = a & 255; mnr = (b >> 16) & 255; mng = (b >> 8) & 255; mnb = b & 255; }
      }
      u16 c0 = to565(mxr, mxg, mxb), c1 = to565(mnr, mng, mnb);
      if (anyT) { if (c0 > c1) std::swap(c0, c1); }          // modo 3 cores + transparente (c0 <= c1)
      else if (c0 < c1) std::swap(c0, c1);                    // modo 4 cores (c0 > c1); c0 == c1 -> tudo índice 0
      int pr[4], pg[4], pb[4]; from565(c0, pr[0], pg[0], pb[0]); from565(c1, pr[1], pg[1], pb[1]);
      if (c0 > c1) { for (int k = 0; k < 3; k++) { int a0[3] = {pr[0], pg[0], pb[0]}, a1[3] = {pr[1], pg[1], pb[1]};
          (k == 0 ? pr[2] : k == 1 ? pg[2] : pb[2]) = (2 * a0[k] + a1[k]) / 3; (k == 0 ? pr[3] : k == 1 ? pg[3] : pb[3]) = (a0[k] + 2 * a1[k]) / 3; } }
      else { pr[2] = (pr[0] + pr[1]) / 2; pg[2] = (pg[0] + pg[1]) / 2; pb[2] = (pb[0] + pb[1]) / 2; }
      u8* o = &out[((size_t)by * bw + bx) * 8]; wbe16(o, c0); wbe16(o + 2, c1);
      for (int y = 0; y < 4; y++) {
        u8 row = 0;
        for (int x = 0; x < 4; x++) {
          u32 p = blk[y * 4 + x]; int code;
          if ((p >> 24) < 128) code = 3;
          else {
            int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255, best = 1 << 30; code = 0;
            for (int k = 0; k < (c0 > c1 ? 4 : 3); k++) {
              int dd = (r - pr[k]) * (r - pr[k]) + (g - pg[k]) * (g - pg[k]) + (b - pb[k]) * (b - pb[k]);
              if (dd < best) { best = dd; code = k; }
            }
          }
          row |= (u8)(code << (2 * x));
        }
        o[4 + y] = row;
      }
    }
  }
  return out;
}

// Constrói uma textura completa com cadeia de mipmaps contígua (do maior para 1x1).
// Formatos com paleta (CI4/CI8): a mesma paleta vale para todos os mips e sai em *pal_out.
inline std::vector<u8> build_texture(const std::vector<u32>& base, int w, int h, int fmt, bool mips, int* nlevels = nullptr, std::vector<u32>* pal_out = nullptr) {
  std::vector<u8> out; std::vector<u32> cur = base; int cw = w, ch = h, n = 0;
  std::vector<u32> pal; if (fmt == 10 || fmt == 11) { pal = quantize_palette(base, fmt == 10 ? 256 : 16); if (pal_out) *pal_out = pal; }
  while (true) {
    auto lv = encode_level(cur, cw, ch, fmt, pal.empty() ? nullptr : &pal); out.insert(out.end(), lv.begin(), lv.end()); n++;
    if (!mips || (cw == 1 && ch == 1)) break;
    cur = box_down(cur, cw, ch); cw = std::max(1, cw / 2); ch = std::max(1, ch / 2);
  }
  if (nlevels) *nlevels = n;
  return out;
}

}  // namespace sf
