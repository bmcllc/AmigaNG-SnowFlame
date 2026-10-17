// sf100_mkdvd: monta a imagem de DVD do sistema (SnowFS): menu em boot/menu.bin, recursos em sys/ e jogos em games/.
//   sf100_mkdvd -o out/snowflame.dvd [--game nome=arquivo.c]... [--data caminho_no_disco=arquivo]...
#include "snowcc.h"
#include "sffs.h"
#include "sysassets.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace sf;
using namespace sf::abi;

static std::vector<u8> slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); if (!f) { std::fprintf(stderr, "não abri %s\n", p.c_str()); std::exit(1); } return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
static std::string slurp_s(const std::string& p) { auto v = slurp(p); return std::string(v.begin(), v.end()); }

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
  for (auto& sf : sysassets::system_files()) { files.push_back(sf); std::printf("  %-26s %7zu bytes\n", sf.first.c_str(), sf.second.size()); }
  for (auto& d : data) { files.push_back({d.first, slurp(d.second)}); std::printf("  %-26s %7zu bytes  (%s)\n", d.first.c_str(), files.back().second.size(), d.second.c_str()); }
  std::vector<u8> disc; std::string err; if (!sffs_build(files, disc, &err, &flags)) { std::fprintf(stderr, "SnowFS: %s\n", err.c_str()); return 1; }
  std::ofstream f(out, std::ios::binary); f.write((const char*)disc.data(), (std::streamsize)disc.size());
  std::printf("%s: %zu arquivos, %.1f KB (%zu setores)\n", out.c_str(), files.size(), disc.size() / 1024.0, disc.size() / abi::DVD_SECTOR);
  return 0;
}
