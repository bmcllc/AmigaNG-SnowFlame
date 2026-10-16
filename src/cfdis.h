// Desassembador do SnowFire-CF266 (mesmo subconjunto que o interpretador executa). Usado pelo depurador, pelo profiler e nos testes.
#pragma once
#include "common.h"

namespace sf {

struct CfInsn { std::string text; int len = 2; bool known = true; };
// rd16 devolve a palavra de 16 bits no endereço dado. 'syms' (opcional) resolve endereços absolutos/alvos de desvio para nomes.
CfInsn cf_disasm(const std::function<u16(u32)>& rd16, u32 pc, const std::function<std::string(u32)>* syms = nullptr);

}  // namespace sf
