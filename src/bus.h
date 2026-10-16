// SnowBridge: interconexão, memórias, cache de dados do CPU (não coerente com DMA),
// semáforos, IRQ e engine DMA com descritores encadeados.
#pragma once
#include "common.h"
#include "abi.h"

namespace sf {

class Device {
 public:
  virtual ~Device() {}
  virtual u32 read(u32 off) = 0;
  virtual void write(u32 off, u32 v) = 0;
  // Dispositivos com registradores de 16 bits independentes (efeitos colaterais por registrador, como o chipset Amiga) devolvem true:
  // acessos de 16 bits chamam read16/write16 e os de 32 bits viram dois acessos de 16 bits. Os demais usam leitura-modificação-escrita.
  virtual bool native16() const { return false; }
  virtual u16 read16(u32 off) { u32 v = read(off & ~3u); return (off & 2) ? (u16)v : (u16)(v >> 16); }
  virtual void write16(u32 off, u16 v) { u32 cur = read(off & ~3u); int sh = (off & 2) ? 0 : 16; write(off & ~3u, (cur & ~(0xFFFFu << sh)) | ((u32)v << sh)); }
};

// Cache de dados 32 KB, 4 vias, linhas de 16 B, write-back/write-allocate. Apenas DDR é cacheável.
// NÃO é coerente com DMA/VPU/GPU: o software deve fazer flush/invalidate (veja BR_CACHE_OP).
class DCache {
 public:
  struct Line { bool v = false, d = false; u32 tag = 0; u8 data[16] = {0}; };
  static constexpr int SETS = 128, WAYS = 4, LINE = 16;
  Line lines[SETS][WAYS];
  u8 rr[SETS] = {0};
  u64 hits = 0, misses = 0, writebacks = 0;
};

class Bus {
 public:
  Sched* sched = nullptr;
  std::vector<u8> ddr, vram, aram, sram;
  bool cache_enabled = true;
  DCache dc;
  bool fault = false; u32 fault_addr = 0;     // erro de barramento sticky (CPU consulta)
  u32 irq_status = 0, irq_mask = 0;
  u32 sem[16] = {0};

  // Árbitro de memória: cada região (DDR, VRAM, ARAM, outras) atende um mestre por vez.
  // reserve() devolve o instante absoluto (ps) em que a transferência termina, serializando mestres concorrentes.
  enum Region { R_DDR = 0, R_VRAM, R_ARAM, R_OTHER, R_COUNT };
  static Region region_of(u32 a);
  static u32 region_mbps(Region r);
  Ps arb_busy[R_COUNT] = {0, 0, 0, 0}; Ps arb_wait_ps = 0; u64 arb_bytes[R_COUNT] = {0, 0, 0, 0};
  bool arb_enabled = true;                         // false = modelo antigo, sem contenção
  Ps reserve(u32 addr, u64 bytes, Ps earliest);
  Ps cpu_stall_ps = 0;                             // penalidade acumulada de misses da cache (consumida pelo Soc)

  // MMU de software (TLB de 64 entradas, 4 KB). Registradores em MMU_BASE (classe Mmu).
  struct TlbEntry { bool v = false; u32 vpn = 0, ppn = 0, perm = 0; };
  TlbEntry tlb[64]; int tlb_rr = 0; bool mmu_on = false, mmu_super_bypass = false; u32 mmu_far = 0, mmu_fsr = 0;
  bool cpu_super = true;            // modo supervisor corrente da CPU (atualizado pelo núcleo)
  bool mmu_bypass = false;          // acessos de exceção da CPU (pilha/vetores) são físicos
  bool xlate(u32 va, u32 acc, u32& pa);
  u64 tlb_hits = 0, tlb_misses = 0;
  // Cache de instruções (somente temporização): 32 KB, 4 vias, linhas de 16 B.
  // A I-cache guarda os bytes das linhas: sem snoop, uma instrução alterada em memória só é vista depois de ICACHE_INVAL_*
  // (o carregador do host, Bus::raw_write/Soc::load, invalida sozinho). Busca de instrução não passa pela D-cache.
  u32 itag[128][4] = {{0}}; bool ivalid[128][4] = {{false}}; u8 irr[128] = {0}; u8 idata[128][4][16]; u64 ihits = 0, imisses = 0;
  bool icache_enabled = true; u64 ifetches = 0;   // palavras de instrução buscadas (para ferramentas/testes)
  void icache_inval_all() { std::memset(ivalid, 0, sizeof ivalid); }
  void icache_inval_range(u32 a, u32 len);

  Bus();
  void map_device(u32 base, u32 size, Device* d);
  void add_window(u32 base, u32 size, u8* ptr);   // memória linear (ex.: IMEM/DMEM das VPUs)

  // Acesso "cru" (sem cache, sem MMIO). Retorna nullptr se [addr, addr+len) não estiver numa região plana.
  u8* raw(u32 addr, u32 len);
  const u8* raw(u32 addr, u32 len) const { return const_cast<Bus*>(this)->raw(addr, len); }
  bool raw_ok(u32 addr, u32 len) { return raw(addr, len) != nullptr; }
  u32 raw_r32(u32 a);  void raw_w32(u32 a, u32 v);
  u16 raw_r16(u32 a);  void raw_w16(u32 a, u16 v);
  u8 raw_r8(u32 a);    void raw_w8(u32 a, u8 v);
  float raw_rf(u32 a) { return u2f(raw_r32(a)); }
  void raw_write(u32 a, const void* src, u32 n);   // utilitário de carga (lança exceção se inválido)
  void raw_read(u32 a, void* dst, u32 n);

  // Acesso do CPU (passa pela cache em DDR; MMIO despachado aos dispositivos)
  u32 cpu_read(u32 a, int size);
  void cpu_write(u32 a, int size, u32 v);
  // Leitura sem efeitos colaterais (não conta acertos/faltas nem altera a D-cache): enxerga linhas sujas da D-cache. Para depuradores.
  bool peek(u32 a, void* dst, u32 n) const;
  bool peek32(u32 a, u32& v) const { u8 b[4]; if (!peek(a, b, 4)) return false; v = rbe32(b); return true; }
  bool peek16(u32 a, u16& v) const { u8 b[2]; if (!peek(a, b, 2)) return false; v = rbe16(b); return true; }
  // Gancho de acessos de dados da CPU (depurador: watchpoints). Chamado com o endereço virtual, antes do acesso.
  std::function<void(u32 addr, int size, bool write)> on_access;
  u16 fetch16(u32 a);   // busca de instrução: sem cache de instruções modelada (lê DDR/SRAM direto)

  // Cache
  void cache_flush_range(u32 a, u32 len, bool inval);
  void cache_inval_range(u32 a, u32 len);
  void cache_flush_all(bool inval) { cache_flush_range(0, abi::DDR_SIZE, inval); }
  void cache_inval_all() { cache_inval_range(0, abi::DDR_SIZE); }

  void raise_irq(u32 bits) { irq_status |= bits; }
  bool irq_pending() const { return (irq_status & irq_mask) != 0; }

 private:
  struct Map { u32 base, size; Device* dev; };
  struct Win { u32 base, size; u8* ptr; };
  std::vector<Map> maps; std::vector<Win> wins;
  Device* find_dev(u32 a, u32& off);
  DCache::Line& cache_line(u32 a);
  const u8* iline(u32 pa);
  u8 fetch16_byte(u32 pa);
  bool xlate_span(u32 va, int n, u32 acc, u32* pa);
  void cache_bytes(u32 a, u8* buf, int n, bool wr);
  void wb(DCache::Line& l, int set);
};

// Dispositivo SnowBridge (registradores em BRIDGE_BASE): DMA, IRQ, semáforos, cache, relógio.
class Bridge : public Device {
 public:
  Bridge(Bus& b) : bus(b) {}
  u32 read(u32 off) override;
  void write(u32 off, u32 v) override;
  struct Ch { u32 src = 0, dst = 0, len = 0, flags = 0, desc = 0, status = 0; };
  Ch ch[4];
  u64 dma_bytes = 0, dma_jobs = 0;
  std::function<void()> on_reboot;                  // BR_SYS_CTRL bit0
 private:
  Bus& bus; u32 time_hi_latch = 0; u32 cache_addr = 0, cache_len = 0;
  void start(int c, bool chain);
  void exec(int c, u32 src, u32 dst, u32 len, u32 flags, u32 next);
  void fail(int c);
};

}  // namespace sf
