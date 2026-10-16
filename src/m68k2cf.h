// m68k2cf: tradutor estático de código 68000 para o ColdFire do SF-100 (SnowOS Classic, "tradução ou recompilação").
// O ColdFire é um subconjunto da codificação 68k: o que tem a mesma semântica é copiado; o resto é expandido (DBcc, EXG, MOVEM com
// pré/pós-incremento, deslocamentos em memória, ROXL.L, CMPM, xxxI para CCR/SR, RTR) usando só instruções que preservam os flags
// quando preciso (pilha + MOVEM/LEA). Endereçamento relativo ao PC e desvios são refeitos (o código é realocado). Dados misturados com código
// são declarados em data_ranges e copiados sem tradução. Detalhes e limites em docs/SNOWOS-CLASSIC.md.
#pragma once
#include "common.h"
#include <map>

namespace sf {

struct XlatOptions {
  u32 base68 = 0x1000;                                  // endereço do primeiro byte do código 68k
  u32 base_cf = 0x1000;                                 // endereço de carga do código traduzido
  std::vector<std::pair<u32, u32>> data_ranges;         // [início, fim) em endereços 68k: copiados como dados
  u32 entry68 = 0;                                      // (informativo) entrada em endereço 68k
  bool remap_chipset = true;                            // endereços absolutos $DFF000-$DFFFFF (chipset) -> CLASSIC_BASE (0xF00D0000) do SF-100
};
struct XlatResult {
  bool ok = false; std::string error;
  std::vector<u8> code; u32 base_cf = 0;
  std::map<u32, u32> map;                               // endereço 68k de cada instrução/dado -> endereço ColdFire
  int source_insns = 0, expanded = 0, cf_bytes = 0;
  std::vector<std::string> notes;                       // avisos (ex.: linha A/F convertida em ILLEGAL; Z/V de ROXL aproximados)
};
XlatResult translate_68k(const std::vector<u8>& code68, const XlatOptions& opt);

}  // namespace sf
