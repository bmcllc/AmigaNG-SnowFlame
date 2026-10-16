// Utilitários de E/S de arquivo: PPM (quadros) e WAV (áudio).
#pragma once
#include "common.h"
#include <cstdio>

namespace sf {

inline bool write_ppm(const std::string& path, int w, int h, const std::vector<u32>& rgb) {
  FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int i = 0; i < w * h; i++) { u8 p[3] = {(u8)(rgb[i] >> 16), (u8)(rgb[i] >> 8), (u8)rgb[i]}; std::fwrite(p, 1, 3, f); }
  std::fclose(f); return true;
}
inline bool write_wav(const std::string& path, const std::vector<i16>& stereo, u32 rate = 48000) {
  FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return false;
  u32 dataBytes = (u32)stereo.size() * 2; auto le32 = [&](u32 v) { u8 b[4] = {(u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24)}; std::fwrite(b, 1, 4, f); };
  auto le16 = [&](u16 v) { u8 b[2] = {(u8)v, (u8)(v >> 8)}; std::fwrite(b, 1, 2, f); };
  std::fwrite("RIFF", 1, 4, f); le32(36 + dataBytes); std::fwrite("WAVEfmt ", 1, 8, f); le32(16); le16(1); le16(2); le32(rate); le32(rate * 4); le16(4); le16(16);
  std::fwrite("data", 1, 4, f); le32(dataBytes);
  for (i16 s : stereo) le16((u16)s);
  std::fclose(f); return true;
}

}  // namespace sf
