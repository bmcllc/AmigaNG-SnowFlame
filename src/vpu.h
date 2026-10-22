// SnowVector-128: unidade SIMD FP32 (4 vias), 32 registradores vetoriais, 8 escalares,
// 64 KiB de DMEM, 8 KiB de IMEM, palavras de instrução de 32 bits em big-endian.
#pragma once
#include "bus.h"

namespace sf {

namespace vop {
enum : u32 {
  HALT = 0x00, VADD = 0x01, VSUB = 0x02, VMUL = 0x03, VMAC = 0x04, VDOT3 = 0x05, VMIN = 0x06, VMAX = 0x07,
  VLD = 0x08, VST = 0x09, SETA = 0x0A, SADD = 0x0B, SCMP = 0x0C, BRLT = 0x0D, BRNE = 0x0E, BRA = 0x0F,
  VMOV = 0x10, VRCP = 0x11, VRSQ = 0x12, VABS = 0x13, VDOT4 = 0x14, NOP = 0x15,
  // Motor DMA local (DMEM <-> barramento, sem a CPU) e sincronização: DMAIN sDmem,sEnd,sLen | DMAOUT sEnd,sDmem,sLen | DMAWAIT | SIGNAL n
  DMAIN = 0x16, DMAOUT = 0x17, DMAWAIT = 0x18, SIGNAL = 0x19,
  VFRC = 0x1A,   // parte fracionária (x - floor(x)) por via: laços periódicos e partículas que dão a volta
};
constexpr u32 SWZ_IDENT = 0xE4;   // x,y,z,w
}

// Codificação: op[31:26] a[25:21] b[20:16] c[15:11] low[10:0]; I-type: imm16 em [15:0]
inline u32 vpu_enc_r(u32 op, u32 a, u32 b, u32 c, u32 swz = vop::SWZ_IDENT) {
  return (op << 26) | (a << 21) | (b << 16) | (c << 11) | (swz & 0xFF);
}
inline u32 vpu_enc_i(u32 op, u32 a, u32 b, i32 imm) { return (op << 26) | (a << 21) | (b << 16) | ((u32)imm & 0xFFFF); }

// Assembler textual:  VADD v1, v2, v3.xxxx | VLD v0,[s1+3] | VST [s2],v4 | SETA s1,100 | SADD s1,s1,4 |
// SCMP s1,s2 | BRLT label | DMAIN s0,s1,s2 | DMAOUT s1,s0,s2 | DMAWAIT | SIGNAL 3 | HALT | "label:" | comentários com ';' ou '//'
bool vpu_assemble(const std::string& src, std::vector<u32>& out, std::string& err);
std::string vpu_disasm(u32 w);

class Vpu : public Device {
 public:
  Vpu(Bus& b, u32 irq_bit);
  u8 imem[abi::VPU_IMEM_SIZE];
  u8 dmem[abi::VPU_DMEM_SIZE];
  float v[32][4];
  u32 s[8];

  void load_program(const std::vector<u32>& code, u32 at_word = 0);
  // Enfileira um job (equivale a escrever VPU_S0.., VPU_PC e VPU_CTRL.START).
  bool submit(u32 pc, const u32 sregs[8], bool irq = false);

  u32 read(u32 off) override;
  void write(u32 off, u32 val) override;

  bool busy() const { return running; }
  u32 status() const;
  u64 last_cycles = 0, last_flops = 0, total_cycles = 0, total_flops = 0, jobs_done = 0, dma_bytes = 0, dma_xfers = 0, signals = 0;
  u32 fault_pc = 0;

  struct Result { u64 cycles = 0, flops = 0, dma_bytes = 0, dma_xfers = 0; bool fault = false; u32 pc = 0;
                  std::vector<std::pair<u64, u32>> sigs; };   // SIGNAL: (ciclo do job, semáforo)
  Result run_now(u32 pc, const u32 sregs[8]);   // execução funcional síncrona (usada pelos jobs e testes)
  void reset_state();

 private:
  friend class Fabric;                              // backend SystemVerilog (fabric/)
  struct Job { u32 pc; u32 s[8]; bool irq; };
  Bus& bus; u32 irq_bit;
  std::deque<Job> queue; bool running = false, done = false, faulted = false; u64 gen = 0;
  u32 next_pc = 0, next_s[8] = {0};
  void kick();
};

}  // namespace sf
