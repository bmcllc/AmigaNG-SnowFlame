// sf100_mkdvd: monta a imagem de DVD do sistema (SnowFS): menu em boot/menu.bin, recursos em sys/ e jogos em games/.
//   sf100_mkdvd -o out/snowflame.dvd [--game nome=arquivo.c]... [--data caminho_no_disco=arquivo]...
#include "snowcc.h"
#include "sffs.h"
#include "texutil.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace sf;
using namespace sf::abi;

static std::vector<u8> slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); if (!f) { std::fprintf(stderr, "não abri %s\n", p.c_str()); std::exit(1); } return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
static std::string slurp_s(const std::string& p) { auto v = slurp(p); return std::string(v.begin(), v.end()); }

// ---- ícones do menu: 5 de 64x64 numa faixa ARGB4444 de 512x64 (supersampling 4x4) ----
struct Px { float r, g, b, a; };
static float sdBox(float x, float y, float cx, float cy, float hw, float hh, float rad) {
  float dx = std::fabs(x - cx) - hw + rad, dy = std::fabs(y - cy) - hh + rad;
  return std::hypot(std::max(dx, 0.f), std::max(dy, 0.f)) + std::min(std::max(dx, dy), 0.f) - rad;
}
static float sdCirc(float x, float y, float cx, float cy, float r) { return std::hypot(x - cx, y - cy) - r; }
static float cov(float d) { return std::clamp(0.5f - d, 0.f, 1.f); }          // d em "pixels de 64 unidades"; 0,5 = meio pixel
static Px icon_px(int id, float x, float y) {                                  // x,y em [0,64)
  float a = 0, r = 1, g = 1, b = 1; float t = y / 64.f;
  auto over = [&](float cvg, float rr, float gg, float bb) { r = r * (1 - cvg) + rr * cvg; g = g * (1 - cvg) + gg * cvg; b = b * (1 - cvg) + bb * cvg; a = std::max(a, cvg); };
  float br = 0.75f + 0.25f * (1 - t);                                              // brilho vertical
  switch (id) {
    case 0: {                                                                      // controle
      float d = std::min({sdBox(x, y, 32, 34, 22, 11, 8), sdCirc(x, y, 15, 42, 11), sdCirc(x, y, 49, 42, 11)});
      float c = cov(d); r = g = b = br; a = c;
      float cross = std::min(sdBox(x, y, 19, 33, 7, 2.5f, 1), sdBox(x, y, 19, 33, 2.5f, 7, 1)); over(cov(cross), 0.1f, 0.2f, 0.5f);
      over(cov(sdCirc(x, y, 43, 30, 3.2f)), 0.9f, 0.3f, 0.3f); over(cov(sdCirc(x, y, 50, 35, 3.2f)), 0.3f, 0.8f, 0.4f);
      over(cov(sdBox(x, y, 32, 31, 4, 1.5f, 0.8f)), 0.1f, 0.2f, 0.5f); a = c; break;
    }
    case 1: {                                                                      // cartão de memória
      float d = sdBox(x, y, 32, 32, 19, 25, 4);
      float c = cov(d); if (x + (64 - y) > 62 + 13 && y < 17) c = 0; r = 0.6f * br + 0.2f; g = 0.8f * br + 0.1f; b = 1.0f; a = c;
      over(cov(sdBox(x, y, 32, 36, 9, 8, 2)), 0.1f, 0.15f, 0.4f); over(cov(sdBox(x, y, 32, 36, 6.5f, 5.5f, 1)), 0.9f, 0.8f, 0.3f);
      for (int k = 0; k < 4; k++) over(cov(sdBox(x, y, 23 + k * 6, 14, 2, 4, 0.5f)), 0.95f, 0.8f, 0.3f);
      a = c; break;
    }
    case 2: {                                                                      // chip / sistema
      float c = cov(sdBox(x, y, 32, 32, 15, 15, 3)); r = 0.55f * br; g = 0.85f * br; b = 1.0f; a = c;
      for (int k = 0; k < 5; k++) { float p = 22 + k * 5; c = std::max(c, cov(sdBox(x, y, p, 12, 1.4f, 5, 0.3f))); c = std::max(c, cov(sdBox(x, y, p, 52, 1.4f, 5, 0.3f))); c = std::max(c, cov(sdBox(x, y, 12, p, 5, 1.4f, 0.3f))); c = std::max(c, cov(sdBox(x, y, 52, p, 5, 1.4f, 0.3f))); }
      a = std::max(a, c); over(cov(sdBox(x, y, 32, 32, 9, 9, 2)), 0.05f, 0.1f, 0.35f); over(cov(sdBox(x, y, 32, 32, 5, 5, 1)), 0.6f, 0.9f, 1.f); break;
    }
    case 3: {                                                                      // disco
      float R = std::hypot(x - 32, y - 32); float c = cov(R - 24) * (1 - cov(R - 6)); float ang = std::atan2(y - 32, x - 32);
      float sh = 0.5f + 0.5f * std::sin(ang * 2.f + 0.6f); r = 0.5f + 0.5f * sh; g = 0.7f + 0.3f * sh; b = 1.0f; a = c;
      over(cov(std::fabs(R - 16) - 0.7f) * 0.5f, 1, 1, 1); over(cov(R - 9) * (1 - cov(R - 8)) * 0.7f, 0.2f, 0.3f, 0.6f); a = std::max(a, c); break;
    }
    default: {                                                                     // opções: três sliders
      float c = 0; for (int k = 0; k < 3; k++) { float yy = 18 + k * 14; c = std::max(c, cov(sdBox(x, y, 32, yy, 22, 2.2f, 2.2f))); }
      r = 0.7f; g = 0.85f; b = 1.0f; a = c;
      for (int k = 0; k < 3; k++) { float yy = 18 + k * 14, kx = 20 + (k * 17 + 11) % 28; over(cov(sdCirc(x, y, kx, yy, 6.5f)), 1.f, 0.85f - 0.2f * k, 0.3f + 0.3f * k); over(cov(sdCirc(x, y, kx, yy, 2.5f)), 0.15f, 0.2f, 0.45f); a = std::max(a, cov(sdCirc(x, y, kx, yy, 6.5f))); }
    }
  }
  return {r, g, b, a};
}
static std::vector<u8> make_icons() {
  const int W = 512, H = 64; std::vector<u32> img((size_t)W * H);
  for (int id = 0; id < 5; id++) for (int py = 0; py < 64; py++) for (int px = 0; px < 64; px++) {
    Px acc{0, 0, 0, 0}; for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
      Px p = icon_px(id, px + (sx + 0.5f) / 4, py + (sy + 0.5f) / 4); acc.r += p.r * p.a; acc.g += p.g * p.a; acc.b += p.b * p.a; acc.a += p.a; }
    float al = acc.a / 16; float inv = acc.a > 0 ? 1.f / acc.a : 0;
    auto q = [](float v) { return (int)std::clamp(v * 255.f + 0.5f, 0.f, 255.f); };
    img[(size_t)py * W + id * 64 + px] = pack_argb(q(al), q(acc.r * inv), q(acc.g * inv), q(acc.b * inv));
  }
  return build_texture(img, W, H, TF_ARGB4444, false);
}

int main(int argc, char** argv) {
  std::string out = "out/snowflame.dvd"; std::vector<std::pair<std::string, std::string>> games, data;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto kv = [&](std::vector<std::pair<std::string, std::string>>& v) { std::string s = argv[++i]; size_t e = s.find('='); if (e == std::string::npos) { std::fprintf(stderr, "esperava nome=arquivo\n"); std::exit(2); } v.push_back({s.substr(0, e), s.substr(e + 1)}); };
    if (a == "-o" && i + 1 < argc) out = argv[++i]; else if (a == "--game" && i + 1 < argc) kv(games); else if (a == "--data" && i + 1 < argc) kv(data);
    else { std::fprintf(stderr, "uso: sf100_mkdvd -o saida.dvd [--game nome=arquivo.c]... [--data caminho=arquivo]...\n"); return 2; }
  }
  std::vector<SffsFile> files; std::map<std::string, u32> flags;
  auto compile = [&](const std::string& disc_path, const std::string& src) {
    CcOptions opt; opt.include_dirs = {"os", "os/include", "os/apps"}; std::string code = slurp_s(src);
    CcResult r = cc_compile(code, src, opt); if (!r.ok) { std::fprintf(stderr, "%s: %s\n", src.c_str(), r.error.c_str()); std::exit(1); }
    if (r.base != abi::IPL_LOAD_ADDR) { std::fprintf(stderr, "%s: base inesperada\n", src.c_str()); std::exit(1); }
    files.push_back({disc_path, r.image}); flags[disc_path] = r.entry;
    std::printf("  %-26s %7zu bytes  entrada %#x   (%s)\n", disc_path.c_str(), r.image.size(), r.entry, src.c_str());
  };
  compile("boot/menu.bin", "os/apps/menu.c");
  for (auto& g : games) compile("games/" + g.first + ".bin", g.second);
  for (const char* f : {"font.raw", "font.adv", "logo.raw"}) { files.push_back({std::string("sys/") + f, slurp(std::string("assets/") + f)}); std::printf("  sys/%-22s %7zu bytes\n", f, files.back().second.size()); }
  files.push_back({"sys/icons.raw", make_icons()}); std::printf("  %-26s %7zu bytes\n", "sys/icons.raw", files.back().second.size());
  for (auto& d : data) { files.push_back({d.first, slurp(d.second)}); std::printf("  %-26s %7zu bytes  (%s)\n", d.first.c_str(), files.back().second.size(), d.second.c_str()); }
  std::vector<u8> disc; std::string err; if (!sffs_build(files, disc, &err, &flags)) { std::fprintf(stderr, "SnowFS: %s\n", err.c_str()); return 1; }
  std::ofstream f(out, std::ios::binary); f.write((const char*)disc.data(), (std::streamsize)disc.size());
  std::printf("%s: %zu arquivos, %.1f KB (%zu setores)\n", out.c_str(), files.size(), disc.size() / 1024.0, disc.size() / abi::DVD_SECTOR);
  return 0;
}
