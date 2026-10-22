// SnowFlame SF-100: SoC completo (ColdFire V4e + 2x SnowVector + FlameCore + SnowBlitter/Copper + SnowPaula + vídeo).
#pragma once
#include "bus.h"
#include "cpu_cf.h"
#include "vpu.h"
#include "flamecore.h"
#include "chipset.h"
#include "paula.h"
#include "periph.h"

namespace sf {

class Soc;
// Liga o backend de hardware ao SoC recém-construído: no build com o backend SystemVerilog (fabric/) cria o modelo do SoC e
// conecta os serviços do hospedeiro (memória, tempo, mídias, saídas); no modelo de referência em C++ (src/fabric_ref.cpp) não faz nada.
void fabric_attach(Soc& s);
const char* fabric_backend();      // "ref" ou "sv"

class Soc {
 public:
  Soc();
  Sched sched;
  Bus bus;
  Bridge bridge;
  Cpu cpu;
  Vpu vpuA, vpuB;
  FlameCore gpu;
  Blitter blt;
  Copper cop;
  Paula paula;
  Video video;
  MmuDev mmu;
  Timers timers;
  Dvd dvd;
  MemCard mcard;
  Pads pads;
  Hid hid;
  DebugConsole console;

  void load(u32 addr, const std::vector<u8>& bytes) { bus.raw_write(addr, bytes.data(), (u32)bytes.size()); }
  // Gancho chamado após cada instrução da CPU (pc da instrução, ciclos gastos): profiler, rastreio.
  std::function<void(u32 pc, int cycles)> on_step;
  // Executa uma instrução (ou, com a CPU parada, avança até o próximo evento). false = nada mais pode acontecer.
  // IPL (firmware de boot): carrega 'path' do SnowFS do DVD em IPL_LOAD_ADDR e inicia a CPU na entrada gravada em 'flags'.
  // O programa também pode pedir um novo boot escrevendo o caminho em SRAM_BASE e 1 em BR_SYS_CTRL.
  bool boot_dvd(const std::string& path = "boot/menu.bin");
  void quiesce();                  // silencia os dispositivos (IRQs, timers, vídeo, áudio, MMU) antes de um novo boot
  u32 boots = 0; std::string last_boot;
  bool step_one();
  // Executa o sistema por até 'limit' ps de tempo simulado. A CPU roda enquanto não estiver parada;
  // eventos dos dispositivos (DMA, VPU, GPU...) avançam em paralelo. Retorna quando o tempo acaba,
  // quando a CPU dá HALT e não há mais eventos, ou quando a CPU para sem eventos pendentes.
  Ps run(Ps limit);
  Ps take_stall();
  bool cpu_idle() const;
  // Roda até a CPU executar HALT (ou limite de tempo). Retorna true se halted por instrução HALT.
  bool run_until_halt(Ps limit = 10ull * 1000000000000ull);
  void run_frames(int n);          // liga o vídeo e roda n quadros (60 Hz)
  void sync_audio() { paula.render_until(sched.now); }
  // Avança só os dispositivos (sem CPU) até o próximo instante em que o evento 'pred' fique verdadeiro.
  bool wait_until(const std::function<bool()>& pred, Ps limit = 1000000000000ull);
};

}  // namespace sf
