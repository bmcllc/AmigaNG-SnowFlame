// SnowBridge: interconexão, memórias, cache de dados do CPU (não coerente com DMA),
// semáforos, IRQ e engine DMA com descritores encadeados.
#pragma once
#include "common.h"
#include "abi.h"
#include <memory>

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

// Épocas de validade das linhas do caminho rápido do Bus: avançam sempre que muda algo de que uma linha gravada dependia.
// i = busca de instrução (modos, TLB, conteúdo/validade da I-cache); d = dados (modos, TLB, linhas da D-cache).
struct Epochs { u64 i = 0, d = 0; void all() { ++i; ++d; } };

// Flag do modo de acesso (MMU, supervisor, caches): um bool que avança as épocas quando o valor muda, invalidando de uma vez
// as linhas do caminho rápido do Bus (que só comparam a época em vez de reler cada flag).
class ModeFlag {
 public:
  ModeFlag(bool x, Epochs* e) : v(x), ep(e) {}
  ModeFlag(const ModeFlag&) = delete;
  ModeFlag& operator=(const ModeFlag&) = delete;
  ModeFlag& operator=(bool x) { if (v != x) { v = x; ep->all(); } return *this; }
  operator bool() const { return v; }
 private:
  bool v; Epochs* ep;
};

struct TlbEntry { bool v = false; u32 vpn = 0, ppn = 0, perm = 0; };
// TLB de 64 entradas: o acesso para escrita (operator[] não const, begin()) avança as épocas, então qualquer alteração das entradas
// invalida as linhas do caminho rápido sem que cada uso as revalide. O Bus lê pela interface const.
class TlbArray {
 public:
  explicit TlbArray(Epochs* e) : ep(e) {}
  TlbArray(const TlbArray&) = delete;
  TlbArray& operator=(const TlbArray&) = delete;
  TlbEntry& operator[](int i) { ep->all(); return a[i]; }
  const TlbEntry& operator[](int i) const { return a[i]; }
  TlbEntry* begin() { ep->all(); return a; }
  TlbEntry* end() { return a + 64; }
  const TlbEntry* begin() const { return a; }
  const TlbEntry* end() const { return a + 64; }
 private:
  TlbEntry a[64]; Epochs* ep;
};

class Bus {
 public:
  Epochs ep;                        // épocas do caminho rápido (ver Epochs)
  Sched* sched = nullptr;
  std::vector<u8> ddr, vram, aram, sram;
  ModeFlag cache_enabled{true, &ep};
  DCache dc;
  bool fault = false; u32 fault_addr = 0;     // erro de barramento sticky (CPU consulta)
  u32 irq_status = 0, irq_mask = 0;
  u32 sem[16] = {0};
  // Backend de hardware (B.A.S.E. Host Fabric, fabric/): no build com o backend SystemVerilog guarda o modelo do SoC ligado a
  // este barramento; vazio no modelo de referência em C++.
  std::shared_ptr<void> fabric;

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
  TlbArray tlb{&ep}; int tlb_rr = 0; ModeFlag mmu_on{false, &ep}, mmu_super_bypass{false, &ep}; u32 mmu_far = 0, mmu_fsr = 0;
  u8 tlb_hint[64] = {0};            // dica de busca por vpn & 63 (só velocidade: a entrada é sempre revalidada)
  ModeFlag cpu_super{true, &ep};      // modo supervisor corrente da CPU (atualizado pelo núcleo)
  ModeFlag mmu_bypass{false, &ep};    // acessos de exceção da CPU (pilha/vetores) são físicos
  bool xlate(u32 va, u32 acc, u32& pa);
  u64 tlb_hits = 0, tlb_misses = 0;
  // Cache de instruções (somente temporização): 32 KB, 4 vias, linhas de 16 B.
  // A I-cache guarda os bytes das linhas: sem snoop, uma instrução alterada em memória só é vista depois de ICACHE_INVAL_*
  // (o carregador do host, Bus::raw_write/Soc::load, invalida sozinho). Busca de instrução não passa pela D-cache.
  u32 itag[128][4] = {{0}}; bool ivalid[128][4] = {{false}}; u8 irr[128] = {0}; u8 idata[128][4][16]; u64 ihits = 0, imisses = 0;
  ModeFlag icache_enabled{true, &ep}; u64 ifetches = 0;   // palavras de instrução buscadas (para ferramentas/testes)
  void icache_inval_all() { std::memset(ivalid, 0, sizeof ivalid); ep.i++; }
  void icache_inval_range(u32 a, u32 len);

  Bus();
  Bus(const Bus&) = delete;
  Bus& operator=(const Bus&) = delete;
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
  // Caminho rápido (só velocidade): repete a última linha de cache acessada, revalidando a entrada da TLB, a via/tag da cache e o modo
  // a cada uso; os contadores (acertos de TLB/caches, ifetches) avançam exatamente como no caminho completo.
  u32 cpu_read(u32 a, int size) {
    const LineHint& h = rh[(a >> 4) & (DHINTS - 1)];
    if (!on_access && (a & 15) + (u32)size <= 16 && (a & ~15u) == h.vline && h.ep == ep.d) {
      dc.hits++; if (h.ti >= 0) tlb_hits++;
      const u8* p = h.data + (a & 15);
      return size == 1 ? p[0] : size == 2 ? rbe16(p) : rbe32(p);
    }
    return cpu_read_slow(a, size);
  }
  void cpu_write(u32 a, int size, u32 v) {
    const LineHint& h = wh[(a >> 4) & (DHINTS - 1)];
    if (!on_access && (a & 15) + (u32)size <= 16 && (a & ~15u) == h.vline && h.ep == ep.d) {
      dc.hits++; if (h.ti >= 0) tlb_hits++;
      u8* p = h.data + (a & 15);
      if (size == 1) p[0] = (u8)v; else if (size == 2) wbe16(p, (u16)v); else wbe32(p, v);
      h.ln->d = true; return;
    }
    cpu_write_slow(a, size, v);
  }
  // Leitura sem efeitos colaterais (não conta acertos/faltas nem altera a D-cache): enxerga linhas sujas da D-cache. Para depuradores.
  bool peek(u32 a, void* dst, u32 n) const;
  bool peek32(u32 a, u32& v) const { u8 b[4]; if (!peek(a, b, 4)) return false; v = rbe32(b); return true; }
  bool peek16(u32 a, u16& v) const { u8 b[2]; if (!peek(a, b, 2)) return false; v = rbe16(b); return true; }
  // Gancho de acessos de dados da CPU (depurador: watchpoints). Chamado com o endereço virtual, antes do acesso.
  std::function<void(u32 addr, int size, bool write)> on_access;
  __attribute__((always_inline)) u16 fetch16(u32 a) {  // busca de instrução (I-cache em DDR; demais regiões direto)
    const LineHint& h = fh[(a >> 4) & (FHINTS - 1)];
    if (!(a & 1) && (a & ~15u) == h.vline && h.ep == ep.i) {
      ifetches++; ihits++; if (h.ti >= 0) tlb_hits++;
      return rbe16(h.data + (a & 15));
    }
    return fetch16_slow(a);
  }

  // Linha de busca recente (para a CPU guardar a última): valida enquanto ep.i == ep_out; false se a linha de 'a' não está no caminho rápido.
  bool ifetch_hint(u32 a, const u8*& data, int& ti, u64& ep_out) const {
    const LineHint& h = fh[(a >> 4) & (FHINTS - 1)];
    if ((a & ~15u) != h.vline || h.ep != ep.i) return false;
    data = h.data; ti = h.ti; ep_out = h.ep; return true;
  }

  // Cache
  void cache_flush_range(u32 a, u32 len, bool inval);
  void cache_inval_range(u32 a, u32 len);
  void cache_flush_all(bool inval) { cache_flush_range(0, abi::DDR_SIZE, inval); }
  void cache_inval_all() { cache_inval_range(0, abi::DDR_SIZE); }

  void raise_irq(u32 bits) { irq_status |= bits; }
  bool irq_pending() const { return (irq_status & irq_mask) != 0; }

 private:
  // Linha recente do caminho rápido. Vale enquanto ep (época de busca ou de dados) não avançar: modos, TLB e a validade/identidade
  // das linhas de cache só mudam em pontos que avançam a época (ModeFlag, TlbArray, preenchimento/invalidação das caches).
  struct LineHint { u64 ep = ~0ull; u32 vline = 1; int ti = -1; u8* data = nullptr; DCache::Line* ln = nullptr; };   // data: início da linha de cache; ln: a linha da D-cache
  static constexpr int FHINTS = 64, DHINTS = 32;   // tabelas diretas por (va >> 4): busca de instrução / leitura / escrita de dados
  LineHint fh[FHINTS], rh[DHINTS], wh[DHINTS];     // vline ímpar = vazia
  int xl_ti = -1;                    // entrada da TLB usada pelo último xlate bem-sucedido (-1 = sem tradução)
  int hit_set = 0, hit_way = 0;      // via da última linha de cache (I ou D) devolvida
  void set_hint(LineHint* tab, int n, u64 epoch, u32 va, u8* data, DCache::Line* ln);
  u32 cpu_read_slow(u32 a, int size);
  void cpu_write_slow(u32 a, int size, u32 v);
  u16 fetch16_slow(u32 a);
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
  friend class Fabric;                              // backend SystemVerilog (fabric/)
  Bus& bus; u32 time_hi_latch = 0; u32 cache_addr = 0, cache_len = 0;
  void start(int c, bool chain);
  void exec(int c, u32 src, u32 dst, u32 len, u32 flags, u32 next);
  void fail(int c);
};

}  // namespace sf
