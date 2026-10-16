#include "helpers.h"
#include "cpu68k.h"
#include <random>
#include "cfdis.h"

// ======================= Item 7: CPU 68000 (interpretador) =======================
struct FlatMem : M68kMem {
  std::vector<u8> m; int irq = 0; int acked = 0; bool reset_seen = false;
  FlatMem() : m(1u << 20, 0) {}
  u8 read8(u32 a) override { return m[a & 0xFFFFF]; }
  u16 read16(u32 a) override { return (u16)((m[a & 0xFFFFF] << 8) | m[(a + 1) & 0xFFFFF]); }
  void write8(u32 a, u8 v) override { m[a & 0xFFFFF] = v; }
  void write16(u32 a, u16 v) override { m[a & 0xFFFFF] = (u8)(v >> 8); m[(a + 1) & 0xFFFFF] = (u8)v; }
  int irq_level() override { return irq; }
  void irq_ack(int) override { acked++; irq = 0; }
  void reset_signal() override { reset_seen = true; }
  void put32(u32 a, u32 v) { write16(a, (u16)(v >> 16)); write16(a + 2, (u16)v); }
  u32 get32(u32 a) { return ((u32)read16(a) << 16) | read16(a + 2); }
  void load(u32 a, const std::vector<u8>& b) { for (size_t i = 0; i < b.size(); i++) m[a + i] = b[i]; }
};
struct Asm68 {                                    // montador mínimo de palavras (reaproveita o CfAsm para o que é comum e escreve o resto cru)
  A a{0x1000}; Asm68& w(std::initializer_list<u16> ws) { for (u16 x : ws) a.w(x); return *this; }
  std::vector<u8> bytes() { return a.finish(); }
};
static void run68(Cpu68k& c, int n) { for (int i = 0; i < n; i++) c.step(); }
static Cpu68k setup(FlatMem& m, Asm68& as, bool m020 = false) {
  m.load(0x1000, as.bytes()); Cpu68k c(m); c.m68020 = m020; c.reset_regs(); c.pc = 0x1000; c.a[7] = 0x8000; c.ssp = 0x8000; return c;
}

TEST(m68k_basic_integer_and_flags) {
  FlatMem m; Asm68 as;
  as.a.moveq(5, 0); as.a.moveq(-3, 1); as.a.add(4, A::D(1), 0);                // d0 = 2
  as.a.movel(A::Imm(0x7FFFFFFF), A::D(2)); as.a.addq(4, 1, A::D(2));            // overflow: V, N
  as.w({0x40C3});                                                              // move sr,d3 (68000: não privilegiada)
  as.a.moveq(0, 4); as.a.subq(1, 1, A::D(4));                                  // d4.b = 0xFF, borrow
  as.w({0x40C5});                                                              // move sr,d5
  as.a.movel(A::Imm(0x12345678), A::D(6)); as.a.swap(6); as.a.ext_w(6);        // swap, depois ext.w
  Cpu68k c = setup(m, as); run68(c, 12);
  CHECK_EQ(c.d[0], 2); CHECK_EQ(c.d[2], 0x80000000u); CHECK((c.d[3] & 0x0A) == 0x0A);   // N e V
  CHECK_EQ(c.d[4] & 0xFF, 0xFF); CHECK((c.d[5] & 0x11) == 0x11);                        // C e X
  CHECK_EQ(c.d[6], 0x56780034u);                                                       // swap: 0x56781234; ext.w sobre o byte 0x34 -> 0x0034 na palavra baixa
}

TEST(m68k_bcd_dbcc_movem_cmpm_exg_roxl_movep_chk_tas) {
  FlatMem m; Asm68 as;
  as.a.moveq(0x19, 0); as.a.moveq(0x28, 1); as.w({0xC101});                                // abcd d1,d0 -> 0x47
  as.a.moveq(0x19, 2); as.w({0x8102});                                                       // sbcd d2,d0 -> 0x28
  as.a.moveq(1, 7); as.w({0x4807});                                                          // nbcd d7: 0 - 1 = 0x99, empresta
  as.a.moveq(0, 3); as.a.moveq(4, 4); as.a.label("l"); as.a.addq(4, 1, A::D(3)); as.w({0x51CC, 0xFFFC});   // dbra d4,l: 5 iterações
  as.a.movel(A::Imm(0x11111111), A::D(0)); as.a.movel(A::Imm(0x22222222), A::D(1)); as.a.movel(A::Imm(0x33333333), A::D(2));
  as.w({0x48E7, 0xE000});                                                                    // movem.l d0-d2,-(a7)
  as.w({0x4CDF, 0x0070});                                                                    // movem.l (a7)+,d4-d6
  as.a.lea(A::Abs(0x3000), 0); as.a.lea(A::Abs(0x3100), 1);
  as.w({0xB308});                                                                            // cmpm.b (a0)+,(a1)+
  as.a.moveq(10, 2); as.a.moveq(20, 3); as.w({0xC543});                                      // exg d2,d3
  as.a.movel(A::Imm(0x80000000), A::D(5)); as.w({0x44FC, 0x0010});                           // move #$10,ccr  (X=1)
  as.w({0xE395});                                                                            // roxl.l #1,d5 -> 1, X=C=1
  as.a.movel(A::Imm(0xAABBCCDD), A::D(6)); as.a.lea(A::Abs(0x3200), 1); as.w({0x0DC9, 0x0000});   // movep.l d6,(0,a1)
  Cpu68k c = setup(m, as); m.m[0x3000] = 0x42; m.m[0x3100] = 0x42;
  run68(c, 3); CHECK_EQ(c.d[0] & 0xFF, 0x47);
  run68(c, 2); CHECK_EQ(c.d[0] & 0xFF, 0x28);
  run68(c, 2); CHECK_EQ(c.d[7] & 0xFF, 0x99); CHECK_EQ(c.sr & 0x11, 0x11);
  run68(c, 2 + 5 * 2); CHECK_EQ(c.d[3], 5); CHECK_EQ(c.d[4] & 0xFFFF, 0xFFFF);
  run68(c, 3 + 2); CHECK_EQ(c.d[4], 0x11111111u); CHECK_EQ(c.d[5], 0x22222222u); CHECK_EQ(c.d[6], 0x33333333u); CHECK_EQ(c.a[7], 0x8000u);
  run68(c, 3); CHECK(c.sr & Cpu68k::Z); CHECK_EQ(c.a[0], 0x3001u); CHECK_EQ(c.a[1], 0x3101u);
  run68(c, 3); CHECK_EQ(c.d[2], 20u); CHECK_EQ(c.d[3], 10u);
  run68(c, 3); CHECK_EQ(c.d[5], 1u); CHECK_EQ(c.sr & 0x11, 0x11);
  run68(c, 3); CHECK_EQ(m.m[0x3200], 0xAA); CHECK_EQ(m.m[0x3202], 0xBB); CHECK_EQ(m.m[0x3204], 0xCC); CHECK_EQ(m.m[0x3206], 0xDD);
}

TEST(m68k_chk_tas_div_mul_and_address_error) {
  FlatMem m; Asm68 as;
  as.a.moveq(-1, 0); as.a.moveq(10, 1); as.w({0x4181});                                      // chk d1,d0: d0 < 0 -> vetor 6
  as.a.label("after_chk"); as.a.nop();
  as.a.moveq(0, 2); as.w({0x4AC2});                                                          // tas d2: Z, depois bit 7
  as.a.movel(A::Imm(0x00020000), A::D(3)); as.a.moveq(1, 4); as.w({0x86C4});                 // divu d4,d3: 0x20000 / 1 = 0x20000 > 0xFFFF -> V
  as.a.movel(A::Imm(100000), A::D(5)); as.a.moveq(7, 6); as.w({0x8AC6});                     // divu d6,d5: 100000/7 = 14285 r 5
  as.a.moveq(-20, 7); as.a.moveq(-3, 3); as.w({0xCFC3});                                     // muls d3,d7 -> 60
  as.a.lea(A::Abs(0x3001), 0); as.w({0x3010});                                               // move.w (a0),d0 em endereço ímpar -> vetor 3
  as.a.label("hang"); as.a.bra("hang");
  as.a.label("chk"); as.a.moveq(66, 5); as.w({0x4E73});
  as.a.label("aerr"); as.a.moveq(77, 1); as.w({0x4E72, 0x2700});
  auto code = as.bytes(); m.load(0x1000, code);
  m.put32(6 * 4, as.a.label_addr("chk")); m.put32(3 * 4, as.a.label_addr("aerr"));
  Cpu68k c(m); c.reset_regs(); c.pc = 0x1000; c.a[7] = 0x8000; c.ssp = 0x8000;
  run68(c, 3); CHECK_EQ(c.pc, as.a.label_addr("chk")); CHECK_EQ(c.d[5], 0u); run68(c, 2); CHECK_EQ(c.d[5], 66u);
  CHECK_EQ(c.pc, as.a.label_addr("after_chk"));                                              // RTE volta para depois do CHK
  run68(c, 1 + 2); CHECK(c.d[2] & 0x80); run68(c, 3); CHECK(c.sr & Cpu68k::V); CHECK_EQ(c.d[3], 0x00020000u);
  run68(c, 3); CHECK_EQ(c.d[5], (5u << 16) | 14285u);
  run68(c, 3); CHECK_EQ(c.d[7], 60u);
  run68(c, 2); CHECK_EQ(c.pc, as.a.label_addr("aerr"));                                      // erro de endereço
  u32 sp = c.a[7];
  CHECK_EQ(0x8000u - sp, 14u); CHECK_EQ(m.get32(sp + 2) , 0x3001u);                          // endereço de acesso no quadro
}

TEST(m68k_user_mode_usp_privilege_and_trace) {
  FlatMem m; Asm68 as;
  as.a.movel(A::Imm(0x6000), A::A(0)); as.w({0x4E60 | 0});                                   // move a0,usp
  as.w({0x027C, 0xD8FF});                                                                    // andi #$d8ff,sr  -> modo usuário (S=0), máscara 0
  as.a.movel(A::A(7), A::D(0));                                                              // d0 = USP
  as.a.moveq(1, 1); as.w({0x4E72, 0x2000});                                                  // stop: privilegiada -> vetor 8
  as.a.label("priv"); as.a.movel(A::A(7), A::D(2)); as.w({0x4E61 | 0, });                    // handler (supervisor): d2 = SSP; move usp,a1
  as.a.label("trace"); as.a.nop();
  auto code = as.bytes(); m.load(0x1000, code); m.put32(8 * 4, as.a.label_addr("priv"));
  Cpu68k c(m); c.reset_regs(); c.pc = 0x1000; c.a[7] = 0x8000; c.ssp = 0x8000;
  run68(c, 4); CHECK(!(c.sr & 0x2000)); CHECK_EQ(c.d[0], 0x6000u);                          // em usuário A7 é a USP
  run68(c, 2); CHECK(c.sr & 0x2000); CHECK_EQ(c.pc, as.a.label_addr("priv"));                // moveq em usuário; STOP privilegiada -> exceção, volta ao supervisor
  CHECK_EQ(c.a[7], 0x8000u - 6u);                                                            // quadro de 6 bytes na pilha de supervisor
  CHECK_EQ(c.usp, 0x6000u);
  // trace: T=1 gera exceção 9 após cada instrução
  FlatMem m2; Asm68 b; b.a.nop(); b.a.nop(); b.a.nop(); auto code2 = b.bytes(); m2.load(0x1000, code2); m2.put32(9 * 4, 0x2000); m2.write16(0x2000, 0x4E71);
  Cpu68k t(m2); t.reset_regs(); t.pc = 0x1000; t.a[7] = 0x8000; t.ssp = 0x8000; t.sr = 0xA700;
  run68(t, 1); CHECK_EQ(t.pc, 0x2000u); CHECK(!(t.sr & 0x8000));
}

TEST(m68k_line_a_line_f_illegal_and_68020_extensions) {
  FlatMem m; Asm68 as;
  as.w({0xA123}); as.w({0xF456}); as.w({0x4AFC});
  auto code = as.bytes(); m.load(0x1000, code); m.put32(10 * 4, 0x3000); m.put32(11 * 4, 0x3100); m.put32(4 * 4, 0x3200);
  Cpu68k c(m); c.reset_regs(); c.pc = 0x1000; c.a[7] = 0x8000; c.ssp = 0x8000; c.step(); CHECK_EQ(c.pc, 0x3000u); c.pc = 0x1002; c.step(); CHECK_EQ(c.pc, 0x3100u); c.pc = 0x1004; c.step(); CHECK_EQ(c.pc, 0x3200u);
  // 68000 puro: instruções 68020 são ilegais; em modo 68020 funcionam
  FlatMem m2; Asm68 b;
  b.a.movel(A::Imm(100000), A::D(1)); b.a.movel(A::Imm(50000), A::D(2)); b.w({0x4C02, 0x1C04});                // muls.l d2,d4:d1  -> 5e9
  b.w({0x4C42, 0x1404});                                                                                       // divu.l d2,d4:d1  -> q=100000 r=0
  b.a.movel(A::Imm(0xFFFFFF9C), A::D(3)); b.a.moveq(7, 5); b.w({0x4C45, 0x3806});                               // divs.l d5,d6:d3 (32 bits): -100/7 = -14 r -2
  b.a.moveq(0, 7); b.a.movel(A::Imm(0xFB), A::D(7)); b.w({0x49C7});                                            // extb.l d7
  b.a.moveq(0, 0); b.w({0x60FF, 0x0000, 0x0008}); b.a.moveq(1, 0); b.a.moveq(2, 0); b.a.moveq(3, 0);          // bra.l pula dois moveq
  b.a.lea(A::Abs(0x3000), 0); b.a.moveq(4, 1); b.a.lea(A::Full(0, 0x10, 1, 2, 2), 1);                          // lea (16,a0,d1.l*4),a1 = 0x3020
  auto code2 = b.bytes(); m2.load(0x1000, code2);
  Cpu68k d(m2); d.m68020 = true; d.reset_regs(); d.pc = 0x1000; d.a[7] = 0x8000; d.ssp = 0x8000;
  run68(d, 3); CHECK_EQ(d.d[1], 705032704u); CHECK_EQ(d.d[4], 1u); CHECK(!(d.sr & Cpu68k::V));
  run68(d, 1); CHECK_EQ(d.d[1], 100000u); CHECK_EQ(d.d[4], 0u);
  run68(d, 3); CHECK_EQ(d.d[3], (u32)-14); CHECK_EQ(d.d[6], (u32)-2);
  run68(d, 3); CHECK_EQ(d.d[7], 0xFFFFFFFBu);
  run68(d, 3); CHECK_EQ(d.d[0], 3u);
  run68(d, 3); CHECK_EQ(d.a[1], 0x3020u);
  FlatMem m3; m3.load(0x1000, code2); Cpu68k e(m3); e.reset_regs(); e.pc = 0x1000; e.a[7] = 0x8000; e.ssp = 0x8000; m3.put32(4 * 4, 0x3200);
  run68(e, 3); CHECK_EQ(e.pc, 0x3200u);                                                                        // 68000: MULS.L é ilegal
}

// Teste diferencial: o subconjunto comum de instruções tem a mesma codificação no ColdFire e no 68000, então os dois interpretadores
// têm que chegar ao mesmo estado (registradores, CCR a cada passo e memória) para sequências aleatórias.
TEST(m68k_agrees_with_coldfire_core_on_random_common_instructions) {
  std::mt19937 rng(12345); auto R = [&](int n) { return (int)(rng() % (u32)n); };
  int total = 0, bad = 0; const int shift_types[3] = {0, 1, 3};     // AS, LS, RO (o ColdFire não tem ROX)
  for (int trial = 0; trial < 300 && bad < 3; trial++) {
    A a(0x1000); int ninstr = 30 + R(30);
    auto sz = [&]() { static const int s[3] = {1, 2, 4}; return s[R(3)]; };
    int cur_sz = 4;   // acessos de palavra/longo em endereço ímpar dão erro de endereço no 68000: bytes não usam pré/pós-incremento (mantém os ponteiros pares)
    auto mem_op = [&]() { int an = R(4); switch (R(cur_sz == 1 ? 3 : 5)) { case 0: return A::Ind(an); case 1: return A::Disp(an, (R(16) - 8) * 2); case 2: return A::Abs(0x20100 + R(64) * 2); case 3: return A::Post(an); default: return A::Pre(an); } };
    for (int i = 0; i < ninstr; i++) {
      int k = R(22), d1 = R(8), d2 = R(8), s = sz(); cur_sz = s;
      switch (k) {
        case 0: a.moveq(R(256) - 128, d1); break;
        case 1: a.move(s, A::D(d1), A::D(d2)); break;
        case 2: a.move(s, A::D(d1), mem_op()); break;
        case 3: a.move(s, mem_op(), A::D(d1)); break;
        case 4: a.move(s, A::Imm(rng()), A::D(d1)); break;
        case 5: a.add(s, A::D(d1), d2); break;
        case 6: a.sub(s, mem_op(), d2); break;
        case 7: a.and_(s, A::D(d1), d2); break;
        case 8: a.or_(s, mem_op(), d2); break;
        case 9: a.eor(s, d1, A::D(d2)); break;
        case 10: a.cmp(s, A::D(d1), d2); break;
        case 11: a.addq(s, 1 + R(8), A::D(d1)); break;
        case 12: a.subq(s, 1 + R(8), mem_op()); break;
        case 13: a.neg(s, A::D(d1)); break;
        case 14: a.not_(s, A::D(d1)); break;
        case 15: a.clr(s, A::D(d1)); break;
        case 16: a.tst(s, A::D(d1)); break;
        case 17: a.swap(d1); break;
        case 18: if (R(2)) a.ext_w(d1); else a.ext_l(d1); break;
        case 19: a.shift_i(shift_types[R(3)], R(2), 1 + R(8), d1, s); break;
        case 20: a.shift_r(shift_types[R(3)], R(2), d2, d1, s); break;
        default: a.mulu_w(A::D(d1), d2); break;
      }
    }
    a.halt();
    auto code = a.finish();
    Soc s; s.bus.cache_enabled = false; s.load(0x1000, code); FlatMem m; m.load(0x1000, code);
    std::vector<u32> init(16); for (int i = 0; i < 8; i++) init[i] = (u32)rng();
    for (int i = 0; i < 4; i++) init[8 + i] = 0x20200 + i * 0x40; for (int i = 4; i < 7; i++) init[8 + i] = 0x20200;
    for (u32 x = 0x20000; x < 0x20400; x++) { u8 v = (u8)rng(); s.bus.ddr[x] = v; m.m[x] = v; }
    s.cpu.reset_regs(); s.cpu.a[7] = 0x8000; s.cpu.set_pc(0x1000); Cpu68k c(m); c.reset_regs(); c.a[7] = 0x8000; c.ssp = 0x8000; c.pc = 0x1000;
    for (int i = 0; i < 8; i++) { s.cpu.d[i] = c.d[i] = init[i]; } for (int i = 0; i < 7; i++) { s.cpu.a[i] = c.a[i] = init[8 + i]; }
    for (int i = 0; i < ninstr && bad < 3; i++) {
      u32 pc0 = c.pc; s.cpu.step(); c.step(); total++;
      bool same = s.cpu.pc == c.pc; for (int r = 0; r < 8; r++) same = same && s.cpu.d[r] == c.d[r]; for (int r = 0; r < 7; r++) same = same && s.cpu.a[r] == c.a[r];
      same = same && (s.cpu.sr & 0x1F) == (c.sr & 0x1F);
      if (!same) {
        bad++; char buf[200]; std::snprintf(buf, sizeof buf, "trial %d instr %d pc=%x op=%04x%04x: CF sr=%02x d0=%08x vs 68k sr=%02x d0=%08x", trial, i, pc0, m.read16(pc0), m.read16(pc0 + 2), s.cpu.sr & 0x1F, s.cpu.d[0], c.sr & 0x1F, c.d[0]);
        for (int r = 0; r < 8; r++) if (s.cpu.d[r] != c.d[r]) std::snprintf(buf + strlen(buf), 40, " [d%d %08x/%08x]", r, s.cpu.d[r], c.d[r]);
        std::printf("    %s\n", buf);
        { auto rd = [&](u32 ad) { return m.read16(ad); }; u32 p = 0x1000; for (int k = 0; k <= i; k++) { CfInsn in = cf_disasm(rd, p, nullptr); if (k + 8 > i) std::printf("      %04x: %s\n", p, in.text.c_str()); p += in.len; } }
        break;
      }
    }
    if (!bad) for (u32 x = 0x20000; x < 0x20400; x++) if (s.bus.ddr[x] != m.m[x]) { bad++; std::printf("    trial %d: memória difere em %x\n", trial, x); break; }
  }
  CHECK(total > 5000); CHECK_EQ(bad, 0);
}
