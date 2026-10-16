// Unidade de disquete Amiga (DF0) sobre imagens ADF de 880 KB: gera as trilhas em MFM no formato AmigaDOS (11 setores por trilha).
#pragma once
#include "common.h"

namespace sf {

struct AmigaDrive {
  static constexpr int CYLS = 80, SIDES = 2, SECTORS = 11, SECTOR_BYTES = 512, ADF_SIZE = CYLS * SIDES * SECTORS * SECTOR_BYTES;
  static constexpr u32 TRACK_WORDS = 6400;                 // comprimento de uma volta em palavras MFM (12800 bytes)
  std::vector<u8> adf;                                     // vazio = sem disco
  bool motor = false, changed = true, step_hi = true; int cyl = 0; Ps motor_on = 0;
  bool present() const { return adf.size() == (size_t)ADF_SIZE; }

  // 'v' tem os bits de dado nas posições pares (máscara 0x55555555); devolve o longword MFM com os bits de relógio. prev = último bit do longword anterior.
  static u32 mfm_long(u32 d, u32 prev) {
    u32 clk = ~((d << 1) | (d >> 1) | (prev << 31)) & 0xAAAAAAAAu;
    return d | clk;
  }
  // Trilha MFM como palavras de 16 bits (ordem de transmissão). trk = cilindro*2 + lado.
  std::vector<u16> mfm_track(int trk) const {
    std::vector<u16> out; out.reserve(TRACK_WORDS);
    for (int s = 0; s < SECTORS; s++) {
      out.push_back(0xAAAA); out.push_back(0xAAAA); out.push_back(0x4489); out.push_back(0x4489);
      std::vector<u32> body;                              // longwords de dados (já mascarados), na ordem do fluxo
      auto odd_even = [](const std::vector<u32>& v, std::vector<u32>& o) {
        for (u32 x : v) o.push_back((x >> 1) & 0x55555555u);
        for (u32 x : v) o.push_back(x & 0x55555555u);
      };
      std::vector<u32> hdr = {0xFF000000u | ((u32)trk << 16) | ((u32)s << 8) | (u32)(SECTORS - s)}, hdr_labels(4, 0);
      std::vector<u32> hbits; odd_even(hdr, hbits);
      std::vector<u32> lbits; odd_even(hdr_labels, lbits);
      u32 hchk = 0; for (u32 x : hbits) hchk ^= x; for (u32 x : lbits) hchk ^= x; hchk &= 0x55555555u;
      std::vector<u32> data(128);
      const u8* src = &adf[(size_t)(trk * SECTORS + s) * SECTOR_BYTES];
      for (int i = 0; i < 128; i++) data[i] = ((u32)src[4 * i] << 24) | ((u32)src[4 * i + 1] << 16) | ((u32)src[4 * i + 2] << 8) | src[4 * i + 3];
      std::vector<u32> dbits; odd_even(data, dbits);
      u32 dchk = 0; for (u32 x : dbits) dchk ^= x; dchk &= 0x55555555u;
      body.insert(body.end(), hbits.begin(), hbits.end()); body.insert(body.end(), lbits.begin(), lbits.end());
      body.push_back(0); body.push_back(hchk); body.push_back(0); body.push_back(dchk);   // checksums: parte ímpar (0) + par
      body.insert(body.end(), dbits.begin(), dbits.end());
      u32 prev = 1;                                      // o sync 0x4489 termina em 1
      for (u32 d : body) { u32 m = mfm_long(d, prev); prev = d & 1; out.push_back((u16)(m >> 16)); out.push_back((u16)m); }
    }
    while (out.size() < TRACK_WORDS) out.push_back(0xAAAA);
    return out;
  }
};

// Decodificação (usada pelos testes e ferramentas): procura o setor 'sector' na trilha e devolve os 512 bytes; falso se o checksum não fecha.
inline bool amiga_mfm_decode_sector(const std::vector<u16>& trk, int sector, std::vector<u8>& out) {
  auto L = [&](size_t w) { return ((u32)trk[w % trk.size()] << 16) | trk[(w + 1) % trk.size()]; };
  auto dec = [&](u32 odd, u32 even) { return ((odd & 0x55555555u) << 1) | (even & 0x55555555u); };
  for (size_t w = 0; w + 1 < trk.size(); w++) {
    if (trk[w] != 0x4489 || trk[w + 1] != 0x4489) continue;
    size_t p = w + 2;
    u32 info = dec(L(p), L(p + 2));
    if (((info >> 8) & 0xFF) != (u32)sector) continue;
    u32 chk = 0; for (int i = 0; i < 10; i++) chk ^= L(p + 2 * i) & 0x55555555u;       // cabeçalho: info(2) + rótulo(8)
    if ((chk ^ (L(p + 20) & 0x55555555u) ^ (L(p + 22) & 0x55555555u)) != 0) return false;
    size_t d = p + 28; u32 dchk = 0;
    for (int i = 0; i < 256; i++) dchk ^= L(d + 2 * i) & 0x55555555u;
    if ((dchk ^ (L(p + 24) & 0x55555555u) ^ (L(p + 26) & 0x55555555u)) != 0) return false;
    out.resize(512);
    for (int i = 0; i < 128; i++) { u32 v = dec(L(d + 2 * i), L(d + 2 * (i + 128))); for (int k = 0; k < 4; k++) out[4 * i + k] = (u8)(v >> (24 - 8 * k)); }
    return true;
  }
  return false;
}

}  // namespace sf
