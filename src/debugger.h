// Depurador do SF-100: pontos de parada, watchpoints (acessos de dados da CPU), passo a passo, backtrace por A6 (LINK/UNLK),
// símbolos e uma interface textual no estilo gdb usada por tools/sfdbg e pelos testes.
#pragma once
#include "soc.h"
#include <map>
#include <set>

namespace sf {

class Debugger {
 public:
  explicit Debugger(Soc& s);
  ~Debugger();
  std::map<std::string, u32> symbols;
  void add_symbols(const std::map<std::string, u32>& m) { for (auto& kv : m) symbols[kv.first] = kv.second; }
  std::string symbol_at(u32 addr) const;                 // "nome" ou "nome+0x10"; vazio se não houver símbolo antes
  bool eval(const std::string& expr, u32& out) const;    // números ($hex, 0xhex, decimal), símbolos, registradores, *endereço, soma/subtração

  enum Why { W_NONE, W_BREAKPOINT, W_WATCHPOINT, W_HALTED, W_STEPPED, W_TIME, W_IDLE, W_FINISHED };
  struct Stop { Why why = W_NONE; int id = 0; u32 pc = 0, addr = 0; bool write = false; };

  int add_breakpoint(u32 addr, bool temporary = false);
  int add_watch(u32 addr, u32 len, bool rd, bool wr);
  bool remove(int id);
  Stop step(int n = 1);                                  // n instruções
  Stop next();                                           // passa por cima de BSR/JSR
  Stop finish();                                         // roda até a função atual retornar
  Stop cont(Ps max_ps = 10ull * 1000000000000ull);       // até parada, HALT ou limite de tempo simulado
  std::vector<u32> backtrace(int max_frames = 16) const; // PCs: atual, depois endereços de retorno seguindo a cadeia de A6

  // Interface textual: devolve o texto de saída (várias linhas). Comandos: help, regs, step [n], next, finish, cont [ps], break X,
  // tbreak X, watch X [len] [r|w|rw], delete N, info break|sym, x[/Nbwl|s] X, dis [X] [n], bt, print E, set REG|*ADDR VALOR, time.
  std::string exec(const std::string& line);
  std::string describe(const Stop& s) const;

 private:
  struct Bp { int id; u32 addr; bool temp; };
  struct Wp { int id; u32 addr, len; bool rd, wr; };
  Soc& soc; std::vector<Bp> bps; std::vector<Wp> wps; int next_id = 1;
  bool wp_hit = false; Stop wp_info; u16 last_op_ = 0;
  std::function<void(u32, int, bool)> prev_hook; bool hooked = false;
  void install_hook();
  Stop run_loop(const std::function<bool()>& stop_after_step, Ps max_ps, u64 max_steps);
  std::string disline(u32 addr, int* len = nullptr) const;
  std::string regs_text() const;
  u16 rd16(u32 a) const { u16 v = 0; soc.bus.peek16(a, v); return v; }
};

}  // namespace sf
