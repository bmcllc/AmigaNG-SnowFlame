// SnowFS: construtor (lado do host) de imagens de DVD com o sistema de arquivos somente leitura do SnowOS.
#pragma once
#include "abi.h"
#include <algorithm>
#include <map>
#include <string>
#include <utility>

namespace sf {

using SffsFile = std::pair<std::string, std::vector<u8>>;

// Monta a imagem: setor 0 = superbloco, diretório logo depois, depois os dados (alinhados a setor).
// Devolve falso (e preenche 'err') se algum nome for vazio/longo demais ou repetido.
inline bool sffs_build(const std::vector<SffsFile>& files, std::vector<u8>& image, std::string* err = nullptr, const std::map<std::string, u32>* flags = nullptr) {
  using namespace abi;
  auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
  for (size_t i = 0; i < files.size(); i++) {
    const std::string& n = files[i].first;
    if (n.empty() || n.size() >= SFFS_NAME_MAX) return fail("nome inválido: " + n);
    for (size_t j = 0; j < i; j++) if (files[j].first == n) return fail("nome repetido: " + n);
  }
  auto sectors = [](size_t bytes) { return (u32)((bytes + DVD_SECTOR - 1) / DVD_SECTOR); };
  u32 dir_sectors = std::max<u32>(1, sectors(files.size() * SFFS_ENTRY_SIZE)), lba = 1 + dir_sectors;
  std::vector<u32> lbas;
  for (auto& f : files) { lbas.push_back(lba); lba += sectors(f.second.size()); }
  image.assign((size_t)lba * DVD_SECTOR, 0);
  auto put32 = [&](size_t off, u32 v) { for (int k = 0; k < 4; k++) image[off + k] = (u8)(v >> (24 - 8 * k)); };
  put32(0, SFFS_MAGIC); put32(4, SFFS_VERSION); put32(8, (u32)files.size()); put32(12, 1); put32(16, dir_sectors); put32(20, lba);
  for (size_t i = 0; i < files.size(); i++) {
    size_t e = (size_t)DVD_SECTOR + i * SFFS_ENTRY_SIZE;
    std::memcpy(&image[e], files[i].first.c_str(), files[i].first.size());
    put32(e + 48, lbas[i]); put32(e + 52, (u32)files[i].second.size());
    if (flags) { auto it = flags->find(files[i].first); if (it != flags->end()) put32(e + 56, it->second); }
    if (!files[i].second.empty()) std::memcpy(&image[(size_t)lbas[i] * DVD_SECTOR], files[i].second.data(), files[i].second.size());
  }
  return true;
}

// Procura um arquivo numa imagem SnowFS (lado do host / firmware).
struct SffsEntry { u32 lba = 0, size = 0, flags = 0; };
inline bool sffs_find(const std::vector<u8>& disc, const std::string& name, SffsEntry& out) {
  using namespace abi;
  auto g32 = [&](size_t o) { return o + 4 <= disc.size() ? ((u32)disc[o] << 24) | ((u32)disc[o + 1] << 16) | ((u32)disc[o + 2] << 8) | disc[o + 3] : 0u; };
  if (g32(0) != SFFS_MAGIC || g32(4) != SFFS_VERSION) return false;
  u32 n = g32(8), dir = g32(12);
  for (u32 i = 0; i < n; i++) {
    size_t e = (size_t)dir * DVD_SECTOR + (size_t)i * SFFS_ENTRY_SIZE; if (e + SFFS_ENTRY_SIZE > disc.size()) return false;
    if (std::strncmp((const char*)&disc[e], name.c_str(), SFFS_NAME_MAX) == 0) { out.lba = g32(e + 48); out.size = g32(e + 52); out.flags = g32(e + 56); return true; }
  }
  return false;
}

}  // namespace sf
