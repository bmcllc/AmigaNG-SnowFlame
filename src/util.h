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
// PNG sem compressão (blocos "stored" do zlib): grande, mas dispensa dependências. argb = 0xAARRGGBB (o alfa é ignorado).
inline u32 png_crc32(const u8* d, size_t n, u32 c = 0xFFFFFFFFu) {
  static u32 tab[256]; static bool init = false;
  if (!init) { for (u32 i = 0; i < 256; i++) { u32 x = i; for (int k = 0; k < 8; k++) x = (x & 1) ? 0xEDB88320u ^ (x >> 1) : x >> 1; tab[i] = x; } init = true; }
  for (size_t i = 0; i < n; i++) c = tab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
  return c;
}
// PNG sem compressão (blocos "stored" do zlib): grande, mas dispensa dependências.
inline bool write_png(const std::string& path, int w, int h, const std::vector<u32>& argb) {
  std::vector<u8> raw; raw.reserve((size_t)(w * 3 + 1) * h);
  for (int y = 0; y < h; y++) { raw.push_back(0); for (int x = 0; x < w; x++) { u32 p = argb[(size_t)y * w + x]; raw.push_back((u8)(p >> 16)); raw.push_back((u8)(p >> 8)); raw.push_back((u8)p); } }
  std::vector<u8> z = {0x78, 0x01}; u32 a = 1, b = 0;
  for (u8 c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  for (size_t pos = 0; pos < raw.size(); pos += 65535) {
    size_t n = std::min<size_t>(65535, raw.size() - pos); bool last = pos + n >= raw.size();
    z.push_back(last ? 1 : 0); z.push_back((u8)n); z.push_back((u8)(n >> 8)); z.push_back((u8)~n); z.push_back((u8)(~n >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
  }
  for (int k = 3; k >= 0; k--) z.push_back((u8)(((b << 16) | a) >> (8 * k)));
  FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return false;
  auto be32 = [&](u32 v) { u8 t[4] = {(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v}; std::fwrite(t, 1, 4, f); };
  auto chunk = [&](const char* type, const std::vector<u8>& d) {
    be32((u32)d.size()); std::vector<u8> t(type, type + 4); t.insert(t.end(), d.begin(), d.end());
    std::fwrite(t.data(), 1, t.size(), f); be32(png_crc32(t.data(), t.size()) ^ 0xFFFFFFFFu);
  };
  std::fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
  std::vector<u8> ihdr = {(u8)(w >> 24), (u8)(w >> 16), (u8)(w >> 8), (u8)w, (u8)(h >> 24), (u8)(h >> 16), (u8)(h >> 8), (u8)h, 8, 2, 0, 0, 0};
  chunk("IHDR", ihdr); chunk("IDAT", z); chunk("IEND", {});
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
