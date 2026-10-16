// snowcc: compilador de um subconjunto de C para o SnowFire-CF266 (ColdFire V4e). Veja docs/SNOWCC.md.
// Gera uma imagem binária big-endian carregável em 'base', com tabela de símbolos e listagem em assembly.
#pragma once
#include "common.h"
#include <map>

namespace sf {

struct CcOptions {
  u32 base = 0x1000;                                // endereço de carga/execução
  u32 stack_top = 0x00F00000;                       // SP inicial do crt0
  bool crt0 = true;                                 // emite _start: SP, jsr main, halt
  std::map<std::string, std::string> defines;       // -DNOME=valor
  std::vector<std::string> include_dirs;            // diretórios para #include
  std::function<bool(const std::string& name, std::string& text)> include_loader;   // alternativa a include_dirs (devolve false se não achar)
};

struct CcResult {
  bool ok = false;
  std::string error;                                // "arquivo:linha: mensagem"
  std::vector<u8> image; u32 base = 0, entry = 0, code_end = 0;
  std::map<std::string, u32> symbols;               // funções e variáveis globais
  std::string listing;                              // assembly desmontado + dados
};

CcResult cc_compile(const std::string& source, const std::string& filename, const CcOptions& opt = CcOptions());
CcResult cc_compile_file(const std::string& path, CcOptions opt = CcOptions());   // também adiciona o diretório do arquivo a include_dirs

}  // namespace sf
