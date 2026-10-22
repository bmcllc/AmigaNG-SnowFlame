// B.A.S.E. Host Fabric do SF-100: liga o backend SystemVerilog (rtl/, compilado pelo Verilator) ao frontend em C++.
//
//   Machine Profile (rtl/sf100.sv + src/abi.h)  ->  backend SV (comportamento dos dispositivos)
//                                                ->  Host Fabric (este módulo: memória, tempo, mídias, FPU, saídas)
//                                                ->  hospedeiro (PC; Switch/PS4 com o mesmo código)
//
// O frontend continua usando as mesmas classes de src/ (Soc, Timers, Video...): no build com o backend SV os métodos delas
// são "wrappers" (fabric/sv_*.cpp) que entram no escopo da instância SV do dispositivo e chamam a API exportada dele. Os
// campos públicos de estatística/estado das classes viram espelhos atualizados pelo canal de observabilidade (fab_obs).
#pragma once
#include "soc.h"

class VerilatedContext;
class Vsf100;

namespace sf {

// Identificadores dos dispositivos no fabric (iguais aos FD_* de rtl/fabric_pkg.sv)
enum FabDev : int { FD_BRIDGE = 0, FD_VPUA, FD_VPUB, FD_GPU, FD_BLT, FD_COP, FD_PAULA, FD_VIDEO, FD_TIMERS, FD_DVD, FD_MCARD,
                    FD_PADS, FD_MMU, FD_DBG, FD_HID, FD_CPU, FD_COUNT };
// Registradores do barramento mantidos pelo hospedeiro (BR_* de rtl/fabric_pkg.sv)
enum FabBusReg : int { FB_IRQ_STATUS = 0, FB_IRQ_MASK, FB_MMU_ON, FB_MMU_SBYP, FB_MMU_FAR, FB_MMU_FSR, FB_DCACHE_EN, FB_ICACHE_EN,
                       FB_TLB_RR, FB_FAULT, FB_FAULT_ADDR, FB_MMU_BYPASS, FB_SEM0 = 16 };

class Fabric {
 public:
  explicit Fabric(Soc& s);
  ~Fabric();
  Fabric(const Fabric&) = delete;
  Fabric& operator=(const Fabric&) = delete;

  Soc& soc; Bus& bus;
  // Torna 'd' o escopo corrente das exportações DPI (toda chamada a <dev>_* é precedida disto).
  void enter(int d) const;
  // Evento agendado por um dispositivo SV que venceu: volta para <dev>_event no escopo dele.
  void dispatch(int dev, int tag, u64 a0, u64 a1);
  // Ligações frontend <-> backend (registradas em fabric.cpp para cada dispositivo)
  std::function<void(int key, u64 v)> obs[FD_COUNT], out[FD_COUNT];
  std::function<u64(int key)> in[FD_COUNT];
  std::function<void(const char*)> msg[FD_COUNT];
  std::vector<u8>* media[2] = {nullptr, nullptr};   // 0 = disco do DVD, 1 = flash do cartão de memória

  // Espelho da CPU: o frontend lê/escreve os campos de soc.cpu; antes de cada passo as alterações dele voltam ao backend.
  void cpu_push();
  void cpu_pull_done();                              // o backend acabou de publicar o estado (soc.cpu == espelho)

  // Contadores do próprio fabric (diagnóstico)
  u64 calls = 0, events = 0;

 private:
  std::unique_ptr<VerilatedContext> ctx; std::unique_ptr<Vsf100> top;
  void* scope[FD_COUNT] = {};
  std::vector<u8> cpu_shadow;                        // estado da CPU na última publicação do backend (serializado)
};

// O Fabric ligado ao barramento (criado por fabric_attach na construção do Soc).
inline Fabric& fabric_of(Bus& b) { return *static_cast<Fabric*>(b.fabric.get()); }

}  // namespace sf
