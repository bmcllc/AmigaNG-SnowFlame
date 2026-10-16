#include "helpers.h"
#include "cpu68k.h"
#include "m68k2cf.h"
#include <random>

// ======================= Item 7: tradução 68000 -> ColdFire =======================
struct XMem : M68kMem {
  std::vector<u8> m = std::vector<u8>(1u << 20, 0);
  u8 read8(u32 a) override { return m[a & 0xFFFFF]; } u16 read16(u32 a) override { return (u16)((m[a & 0xFFFFF] << 8) | m[(a + 1) & 0xFFFFF]); }
  void write8(u32 a, u8 v) override { m[a & 0xFFFFF] = v; } void write16(u32 a, u16 v) override { m[a & 0xFFFFF] = (u8)(v >> 8); m[(a + 1) & 0xFFFFF] = (u8)v; }
};
struct Prog68 {
  std::vector<u16> w; u32 base = 0x1000;
  u32 here() const { return base + (u32)w.size() * 2; }
  void op(std::initializer_list<u16> ws) { for (u16 x : ws) w.push_back(x); }
  void bytes(const std::vector<u8>& b) { for (size_t i = 0; i + 1 < b.size(); i += 2) w.push_back(rbe16(&b[i])); }
  std::vector<u8> out() const { std::vector<u8> o; for (u16 x : w) { o.push_back((u8)(x >> 8)); o.push_back((u8)x); } return o; }
};

struct Compare { bool ok = true; std::string why; };

// roda o programa 68k no Cpu68k e a tradução no núcleo ColdFire, comparando nas fronteiras de cada instrução de origem
static Compare run_both(const std::vector<u8>& code68, const XlatOptions& xo, const XlatResult& xr, u32 end68, const std::vector<u32>& init_regs, const std::vector<u8>& data_init, u32 reg_cmp_mask_all = 0x1F, u32 skip_a_mask = 0) {
  Compare cmp;
  XMem m; for (size_t i = 0; i < code68.size(); i++) m.m[xo.base68 + i] = code68[i];
  for (size_t i = 0; i < data_init.size(); i++) m.m[0x20000 + i] = data_init[i];
  Cpu68k c(m); c.reset_regs(); c.pc = xo.base68; c.a[7] = 0x8000; c.ssp = 0x8000;
  Soc s; s.bus.cache_enabled = false; s.load(xr.base_cf, xr.code);
  for (size_t i = 0; i < data_init.size(); i++) s.bus.ddr[0x20000 + i] = data_init[i];
  // dados dentro do código (data_ranges) ficam na imagem traduzida: o tradutor já os copiou
  s.cpu.reset_regs(); s.cpu.a[7] = 0x8000; s.cpu.set_pc(xr.base_cf);
  for (int i = 0; i < 8; i++) { c.d[i] = s.cpu.d[i] = init_regs[i]; } for (int i = 0; i < 7; i++) { c.a[i] = s.cpu.a[i] = init_regs[8 + i]; }
  int guard = 0;
  while (c.pc != end68 && guard++ < 4000) {
    u32 pc0 = c.pc; u16 op = m.read16(pc0); c.step();
    if (c.halted) { cmp.ok = false; cmp.why = "68k parou"; return cmp; }
    auto it = xr.map.find(c.pc); std::pair<const u32, u32> endpair(end68, xr.base_cf + (u32)xr.code.size()); if (it == xr.map.end() && c.pc == end68) it = xr.map.end();
    u32 want_cf = it != xr.map.end() ? it->second : (c.pc == end68 ? endpair.second : 0xFFFFFFFFu);
    if (want_cf == 0xFFFFFFFFu) { cmp.ok = false; char b[64]; std::snprintf(b, sizeof b, "pc 68k %x sem mapa (op %04x em %x)", c.pc, op, pc0); cmp.why = b; return cmp; }
    int g2 = 0; while (s.cpu.pc != want_cf && g2++ < 400) { s.cpu.step(); if (s.cpu.halted) { cmp.ok = false; cmp.why = "CF parou"; return cmp; } }
    if (s.cpu.pc != want_cf) { cmp.ok = false; char b[96]; std::snprintf(b, sizeof b, "CF não chegou em %x (68k pc %x op %04x em %x; CF em %x)", want_cf, c.pc, op, pc0, s.cpu.pc); cmp.why = b; return cmp; }
    u32 mask = reg_cmp_mask_all;
    for (int r = 0; r < 8; r++) if (c.d[r] != s.cpu.d[r]) { cmp.ok = false; char b[96]; std::snprintf(b, sizeof b, "d%d: 68k %08x x CF %08x após op %04x em %x", r, c.d[r], s.cpu.d[r], op, pc0); cmp.why = b; return cmp; }
    for (int r = 0; r < 8; r++) if (!((skip_a_mask >> r) & 1) && c.a[r] != s.cpu.a[r]) { cmp.ok = false; char b[96]; std::snprintf(b, sizeof b, "a%d: 68k %08x x CF %08x após op %04x em %x", r, c.a[r], s.cpu.a[r], op, pc0); cmp.why = b; return cmp; }
    if ((c.sr & mask) != (s.cpu.sr & mask)) { cmp.ok = false; char b[96]; std::snprintf(b, sizeof b, "CCR: 68k %02x x CF %02x após op %04x em %x", c.sr & 0x1F, s.cpu.sr & 0x1F, op, pc0); cmp.why = b; return cmp; }
  }
  for (u32 x = 0x20000; x < 0x20800 && cmp.ok; x++) if (m.m[x] != s.bus.ddr[x]) { cmp.ok = false; char b[64]; std::snprintf(b, sizeof b, "memória difere em %x", x); cmp.why = b; }
  return cmp;
}

TEST(xlat_expansions_dbcc_exg_movem_memshift_cmpm_ccr_roxl_rtr) {
  Prog68 p;
  auto alu = [&](int k) { switch (k % 5) { case 0: p.op({0xD081}); break; /* add.l d1,d0 */ case 1: p.op({0x9242}); break; /* sub.w d2,d1 */ case 2: p.op({(u16)(0x7403 + ((k * 7) & 0x3F))}); break; /* moveq */ case 3: p.op({0x4A83}); break; /* tst.l d3 */ default: p.op({0xC443}); } };
  // 1) DBRA d3: soma 5 vezes d0 += d1
  p.op({0x7001, 0x7203, 0x7604}); u32 L = p.here(); p.op({0xD081}); p.op({0x51CB}); p.op({(u16)(L - p.here())});     // moveq; ...; L: add.l d1,d0; dbra d3,L
  // 2) DBNE (cond verdadeira na primeira volta: não repete)
  p.op({0x7800, 0x7A02}); L = p.here(); p.op({0x5285}); p.op({0x56CD}); p.op({(u16)(L - p.here())});               // moveq #0,d4; moveq #2,d5? (usa d5 como contador de dbne a5? -> dbne d5)
  // 3) EXG: d0<->d1, a0<->a1, d2<->a2
  p.op({0xC141, 0xC149, 0xC58A});
  // 4) MOVEM com pré/pós-incremento em a6
  p.op({0x2C7C, 0x0002, 0x0800});                                                         // movea.l #$20800,a6
  p.op({0x48E6, 0xF0F0});                                                                   // movem.l ...,-(a6)  (máscara invertida: d0-d3 -> bits 15-12... a0-a3 -> 7-4)
  p.op({0x4CDE, 0x0F0F});                                                                   // movem.l (a6)+,...  d0-d3/a0-a3? (a6 = a6 -> 0x4CDE = (a6)+)
  // 5) deslocamentos em memória: asl.w (a0) ; lsr.w (a1)+ ; ror.w -(a2) ; asr.w 4(a3)
  p.op({0x41F9, 0x0002, 0x0100, 0x43F9, 0x0002, 0x0200, 0x45F9, 0x0002, 0x0310, 0x47F9, 0x0002, 0x0400});   // lea abs,a0-a3
  p.op({0xE1D0, 0xE2D9, 0xE6E2, 0xE0EB, 0x0004});                                                          // asl.w (a0); lsr.w (a1)+; ror.w -(a2); asr.w 4(a3)
  // 6) CMPM.w e CMPM.l
  p.op({0xB348, 0xB988});                                                                  // cmpm.w (a0)+,(a1)+ ; cmpm.l (a0)+,(a4)+ ... (validação por execução)
  // 7) CCR imediato: move #$0A,ccr ; ori #$11,ccr ; andi #$FB,ccr ; eori #$0F,ccr
  p.op({0x44FC, 0x000A, 0x003C, 0x0011, 0x023C, 0x00FB, 0x0A3C, 0x000F});
  // 8) ROXL.L #3,d0 com X ligado
  p.op({0x44FC, 0x0010, 0xE790});
  // 9) RTR: empilha PC de retorno e CCR e executa
  u32 here_rtr = p.here(); (void)here_rtr;
  p.op({0x4879, 0, 0});  size_t patch = p.w.size() - 2;                                      // pea abs.l (preenchido com o endereço da instrução depois do RTR)
  p.op({0x3F3C, 0x0005, 0x4E77});                                                           // move.w #5,-(a7) ; rtr
  u32 after = p.here(); p.w[patch] = (u16)(after >> 16); p.w[patch + 1] = (u16)after; p.op({0x7C09, 0x4E71});
  u32 end68 = p.here(); (void)alu;
  XlatOptions xo; auto code = p.out(); XlatResult xr = translate_68k(code, xo);
  CHECK(xr.ok); if (!xr.ok) std::printf("    %s\n", xr.error.c_str());
  if (!xr.ok) return;
  CHECK(xr.expanded >= 14); CHECK(xr.cf_bytes > (int)code.size());
  std::vector<u32> regs = {0x11111111, 0x00000003, 0xFFFFFFF0, 0x7FFFFFFF, 0, 0x80000000, 7, 0x12345678, 0x20100, 0x20200, 0x20300, 0x20400, 0x20500, 0x20600, 0x20700};
  std::vector<u8> data(0x800); std::mt19937 rng(7); for (auto& b : data) b = (u8)rng();
  Compare cmp = run_both(code, xo, xr, end68, regs, data); CHECK(cmp.ok); if (!cmp.ok) std::printf("    %s\n", cmp.why.c_str());
}

// Programas aleatórios com ALU (cópia direta), desvios, BSR/RTS, tabelas relativas ao PC (data_ranges) e as expansões: tudo tem que bater com o 68000.
TEST(xlat_random_programs_match_the_68000_core) {
  std::mt19937 rng(2024); auto R = [&](int n) { return (int)(rng() % (u32)n); };
  int programs = 0, boundaries_checked = 0;
  for (int trial = 0; trial < 150; trial++) {
    Prog68 p; auto alu = [&]() {
      int x = R(8), y = R(8), k = 1 + R(8);
      switch (R(16)) {
        case 0: p.op({(u16)(0xD080 | (y << 9) | x)}); break; case 1: p.op({(u16)(0x9080 | (y << 9) | x)}); break; case 2: p.op({(u16)(0xC080 | (y << 9) | x)}); break;
        case 3: p.op({(u16)(0x8080 | (y << 9) | x)}); break; case 4: p.op({(u16)(0xB180 | (x << 9) | y)}); break; case 5: p.op({(u16)(0x7000 | (y << 9) | (R(256)))}); break;
        case 6: p.op({(u16)(0xE188 | ((k & 7) << 9) | x)}); break; case 7: p.op({(u16)(0xE088 | ((k & 7) << 9) | x)}); break; case 8: p.op({(u16)(0x5080 | ((k & 7) << 9) | x)}); break;
        case 9: p.op({(u16)(0x4480 | x)}); break; case 10: p.op({(u16)(0x4680 | x)}); break; case 11: p.op({(u16)(0x4840 | x)}); break; case 12: p.op({(u16)(0x48C0 | x)});
          break;
        case 13: p.op({(u16)(0x4A80 | x)}); break; case 14: p.op({(u16)(0xB080 | (y << 9) | x)}); break; default: p.op({(u16)(0x2080 | x)}); break;   // move.l dX,(a0)
      }
    };
    // desvio inicial por cima da sub-rotina: bra.w main ; sub: add.l d1,d0 ; eor.l d0,d2 ; rts
    p.op({0x6000, 0x0000}); size_t bra_patch = p.w.size() - 1; u32 sub = p.here(); p.op({0xD081, 0xB142, 0x4E75}); p.w[bra_patch] = (u16)(p.here() - (p.base + 2));
    int chunks = 10 + R(15);
    std::vector<size_t> table_fix;
    for (int i = 0; i < chunks; i++) {
      switch (R(14)) {
        case 0: case 1: case 2: case 3: alu(); alu(); break;
        case 4: { int dn = R(4) + 3; p.op({(u16)(0x7000 | (dn << 9) | (1 + R(4)))}); u32 L = p.here(); alu(); alu(); p.op({(u16)(0x51C8 | dn), 0}); p.w.back() = (u16)(L - (p.here() - 2)); break; }   // moveq; L: alu alu; dbra dn,L (dn=3..6)
        case 5: { int dn = 6 + R(2); int cc = 2 + R(14); p.op({(u16)(0x7000 | (dn << 9) | (1 + R(3)))}); u32 L = p.here(); alu(); p.op({(u16)(0x50C8 | (cc << 8) | dn), 0}); p.w.back() = (u16)(L - (p.here() - 2)); break; }
        case 6: { int a = R(8), b = R(8); if (R(2)) p.op({(u16)(0xC140 | (a << 9) | b)}); else p.op({(u16)(0xC148 | ((1 + R(3)) << 9) | (1 + R(3)))}); break; }   // exg Dx,Dy / Ax,Ay (só a1-a3: a0 pode apontar para a tabela relocada)
        case 7: { int cc = 6 + R(2) * 1; p.op({(u16)(0x4A80 | R(8))}); p.op({(u16)(0x6000 | (cc << 8) | 2)}); alu(); break; }                       // tst; bne/beq.s sobre um alu
        case 8: p.op({0x6002}); p.op({0x7863}); alu(); break;                                                                                    // bra.s sobre uma instrução (nunca executada)
        case 9: p.op({0x6100, 0}); p.w.back() = (u16)(sub - (p.here() - 2)); alu(); break;                                                       // bsr.w sub
        case 10: { u16 sh = 0xE1D0 + (u16)(R(4) == 0 ? 0 : 0); (void)sh; static const u16 ms[4] = {0xE1D0, 0xE3D1, 0xE2D2, 0xE7D3}; p.op({ms[R(4)]}); break; }   // asl.w (a0) / rol.w (a1) / lsr.w (a2) / rol.w... (memória)
        case 11: { int m1 = R(0x100); p.op({0x44FC, (u16)(R(32))}); p.op({(u16)(0xE390 | ((1 + R(4)) << 9) | R(8))}); (void)m1; break; }   // move #x,ccr ; roxl.l #n,dX (Z/V aproximados)
        case 12: { p.op({0x41FA, 0}); table_fix.push_back(p.w.size() - 1); p.op({0x3A3A, 0}); table_fix.push_back(p.w.size() - 1); p.op({(u16)(0xDA40 | 0)}); break; }   // lea tab(pc),a0 ; move.w tab(pc),d5 ; add.w d0,d5
        default: p.op({0x44FC, (u16)R(32), 0x023C, (u16)(0xFF - R(32)), 0x0A3C, (u16)R(32)}); break;                                               // CCR via imediatos
      }
    }
    p.op({0x4E71}); u32 end68 = p.here(); u32 tab = p.here();
    for (int i = 0; i < 8; i++) p.op({(u16)rng()});
    for (size_t f : table_fix) { u32 ext = p.base + (u32)f * 2; p.w[f] = (u16)(tab - ext); }
    // a0 foi redefinido por lea pc-relativo: os endereços dos registradores diferem de propósito (relocação) -> não comparar a0 e usar a0 só como ponteiro depois
    auto code = p.out(); XlatOptions xo; xo.base_cf = 0x3000 + (u32)R(8) * 2; xo.data_ranges.push_back({tab, tab + 16});
    XlatResult xr = translate_68k(code, xo);
    if (!xr.ok) { std::printf("    trial %d: %s\n", trial, xr.error.c_str()); CHECK(xr.ok); break; }
    std::vector<u32> regs(15); for (auto& r : regs) r = (u32)rng(); for (int i = 0; i < 4; i++) regs[8 + i] = 0x20100 + i * 0x100; regs[12] = 0x20800; regs[13] = 0x20900; regs[14] = 0x20A00;
    std::vector<u8> data(0x800); for (auto& b : data) b = (u8)rng();
    Compare cmp = run_both(code, xo, xr, end68, regs, data, 0x1F, 0x01);
    programs++; if (!cmp.ok) { std::printf("    trial %d: %s\n", trial, cmp.why.c_str()); CHECK(cmp.ok); break; }
    boundaries_checked += (int)code.size() / 2;
  }
  CHECK(programs >= 100); CHECK(boundaries_checked > 3000);
}

TEST(xlat_relocation_pc_relative_data_and_error_reporting) {
  // lea tab(pc),a0 ; move.w (a0)+,d0 ; jsr rotina ; bra fim ; rotina: move.w (a0),d1 ; rts ; fim: nop ; tab: dados
  Prog68 p; p.op({0x41FA, 0}); size_t f1 = p.w.size() - 1; p.op({0x3018, 0x4EBA, 0}); size_t f2 = p.w.size() - 1; p.op({0x6000, 0}); size_t f3 = p.w.size() - 1;
  u32 rot = p.here(); p.op({0x3210, 0x4E75}); u32 fim = p.here(); p.op({0x4E71}); u32 end68 = p.here(); u32 tab = p.here(); p.op({0xCAFE, 0xBEEF});
  p.w[f1] = (u16)(tab - (p.base + (u32)f1 * 2)); p.w[f2] = (u16)(rot - (p.base + (u32)f2 * 2)); p.w[f3] = (u16)(fim - (p.base + (u32)f3 * 2));
  XlatOptions xo; xo.base_cf = 0x5000; xo.data_ranges.push_back({tab, tab + 4}); auto code = p.out(); XlatResult xr = translate_68k(code, xo);
  CHECK(xr.ok); if (!xr.ok) return;
  Compare cmp = run_both(code, xo, xr, end68, std::vector<u32>(15, 0), {}, 0x1F, 0x01); CHECK(cmp.ok); if (!cmp.ok) std::printf("    %s\n", cmp.why.c_str());
  Soc s; s.load(xr.base_cf, xr.code); s.cpu.reset_regs(); s.cpu.a[7] = 0x8000; s.cpu.set_pc(xr.base_cf);
  for (int i = 0; i < 30 && s.cpu.pc != xr.base_cf + (u32)xr.code.size() - 4 - 0; i++) s.cpu.step();
  CHECK_EQ(s.cpu.d[0] & 0xFFFF, 0xCAFEu); CHECK_EQ(s.cpu.d[1] & 0xFFFF, 0xBEEFu);
  CHECK(xr.map.at(tab) > xr.map.at(rot)); CHECK_EQ(xr.map.at(p.base), 0x5000u);
  // instruções que o tradutor recusa, com mensagem útil
  auto fails = [&](std::initializer_list<u16> ws, const char* what) { Prog68 q; q.op(ws); XlatOptions o; XlatResult r = translate_68k(q.out(), o); bool ok = !r.ok && r.error.find(what) != std::string::npos; if (!ok) std::printf("    esperava erro com '%s', obtive '%s'\n", what, r.ok ? "(ok)" : r.error.c_str()); return ok; };
  CHECK(fails({0xC101}, "ABCD")); CHECK(fails({0x8101}, "ABCD")); CHECK(fails({0x4801}, "NBCD")); CHECK(fails({0x0188, 0x0000}, "MOVEP")); CHECK(fails({0x4181}, "CHK"));
  CHECK(fails({0x4E76}, "TRAPV")); CHECK(fails({0x48A7, 0x8000}, "MOVEM.W")); CHECK(fails({0xD308}, "ADDX"));
  // linha A vira ILLEGAL com aviso
  Prog68 q; q.op({0xA123, 0x4E71}); XlatOptions o; XlatResult r = translate_68k(q.out(), o); CHECK(r.ok); CHECK(!r.notes.empty()); CHECK_EQ(rbe16(&r.code[0]), 0x4AFC);
}
