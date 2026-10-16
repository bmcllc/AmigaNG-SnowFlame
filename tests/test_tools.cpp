#include "helpers.h"
#include "cfdis.h"

// ======================= Item 5: desassembador / depurador / profiler =======================
static std::string dis1(Soc& s, u32 at, int* len = nullptr) {
  auto rd = [&](u32 a) { return s.bus.raw_r16(a); }; CfInsn i = cf_disasm(rd, at); if (len) *len = i.len; return i.text;
}
static std::string squash(std::string t) { std::string o; bool sp = false; for (char c : t) { if (c == ' ') { sp = true; continue; } if (sp && !o.empty()) o += ' '; sp = false; o += c; } return o; }

TEST(disasm_matches_assembler_output) {
  Soc s; A a(0x1000);
  struct Case { const char* want; std::function<void()> emit; };
  std::vector<Case> cases = {
    {"move.l d1,d0", [&] { a.movel(A::D(1), A::D(0)); }},
    {"move.l #$12345678,d3", [&] { a.movel(A::Imm(0x12345678), A::D(3)); }},
    {"move.l (a2)+,-(a7)", [&] { a.movel(A::Post(2), A::Pre(7)); }},
    {"move.l $8(a1),d4", [&] { a.movel(A::Disp(1, 8), A::D(4)); }},
    {"move.w -$4(a6),d0", [&] { a.move(2, A::Disp(6, -4), A::D(0)); }},
    {"move.l ($3000).l,d1", [&] { a.movel(A::Abs(0x3000), A::D(1)); }},
    {"moveq #-1,d5", [&] { a.moveq(-1, 5); }},
    {"lea ($3000).l,a0", [&] { a.lea(A::Abs(0x3000), 0); }},
    {"addq.l #3,d2", [&] { a.addq(4, 3, A::D(2)); }},
    {"subq.l #8,a7", [&] { a.subq(4, 8, A::A(7)); }},
    {"add.l d1,d0", [&] { a.add(4, A::D(1), 0); }},
    {"sub.l d0,(a3)", [&] { a.sub_to(4, 0, A::Ind(3)); }},
    {"cmp.l d2,d1", [&] { a.cmp(4, A::D(2), 1); }},
    {"cmpi.l #$a,d0", [&] { a.cmpi(4, 10, A::D(0)); }},
    {"and.l d1,d0", [&] { a.and_(4, A::D(1), 0); }},
    {"eor.l d2,d3", [&] { a.eor(4, 2, A::D(3)); }},
    {"clr.l d6", [&] { a.clr(4, A::D(6)); }},
    {"neg.l d0", [&] { a.neg(4, A::D(0)); }},
    {"tst.l (a0)", [&] { a.tst(4, A::Ind(0)); }},
    {"muls.l d1,d0", [&] { a.muls_l(A::D(1), 0); }},
    {"divu.l d1,d0", [&] { a.divu_l(A::D(1), 0, 0); }},
    {"rems.l d1,d2:d0", [&] { a.divs_l(A::D(1), 0, 2); }},
    {"lsl.l #4,d2", [&] { a.lsl(4, 2); }},
    {"asr.l d1,d0", [&] { a.shift_r(0, false, 1, 0); }},
    {"swap d3", [&] { a.swap(3); }},
    {"ext.l d5", [&] { a.ext_l(5); }},
    {"movem.l d0-d3/a0-a2,(a7)", [&] { a.movem_to(0x070F, A::Ind(7)); }},
    {"link a6,#-$10", [&] { a.link(6, -16); }},
    {"unlk a6", [&] { a.unlk(6); }},
    {"jsr (a0)", [&] { a.jsr(A::Ind(0)); }},
    {"rts", [&] { a.rts(); }},
    {"rte", [&] { a.rte(); }},
    {"trap #3", [&] { a.trap(3); }},
    {"halt", [&] { a.halt(); }},
    {"stop #$2000", [&] { a.stop(0x2000); }},
    {"btst #5,d0", [&] { a.btst_i(5, A::D(0)); }},
    {"bset d1,d0", [&] { a.bset_d(1, A::D(0)); }},
    {"seq d0", [&] { a.scc(7, A::D(0)); }},
    {"addx.l d1,d0", [&] { a.addx_l(1, 0); }},
    {"negx.l d4", [&] { a.negx_l(4); }},
    {"mov3q.l #5,d0", [&] { a.mov3q(5, A::D(0)); }},
    {"mov3q.l #-1,d1", [&] { a.mov3q(-1, A::D(1)); }},
    {"mvs.b (a0),d1", [&] { a.mvs(1, A::Ind(0), 1); }},
    {"mvz.w (a0),d1", [&] { a.mvz(2, A::Ind(0), 1); }},
    {"ff1 d2", [&] { a.ff1(2); }},
    {"byterev d2", [&] { a.byterev(2); }},
    {"move.l a1,usp", [&] { a.move_usp_from(1); }},
    {"move.l usp,a2", [&] { a.move_usp_to(2); }},
    {"movec d1,vbr", [&] { a.movec_to(0x801, false, 1); }},
    {"fmove.d ($3000).l,fp0", [&] { a.fop_ea(0x00, 5, A::Abs(0x3000), 0); }},
    {"fadd.d ($3008).l,fp0", [&] { a.fop_ea(0x22, 5, A::Abs(0x3008), 0); }},
    {"fsmul.d ($3008).l,fp1", [&] { a.fop_ea(0x63, 5, A::Abs(0x3008), 1); }},
    {"fsqrt fp0,fp1", [&] { a.fop_rr(0x04, 0, 1); }},
    {"ftst fp2", [&] { a.fop_rr(0x3A, 2, 2); }},
    {"fmove.l fp0,d1", [&] { a.fstore(0, 0, A::D(1)); }},
    {"fmove.l d0,fpcr", [&] { a.fmove_to_ctrl(4, A::D(0)); }},
    {"fmove.l fpsr,d1", [&] { a.fmove_from_ctrl(2, A::D(1)); }},
    {"fmovem.d (a0),fp0/fp1/fp2", [&] { a.fmovem_load(A::Ind(0), 0xE0); }},
    {"fseq d0", [&] { a.fscc(1, A::D(0)); }},
    {"mac.l d1,d2,acc0", [&] { a.mac_l(1, 2); }},
    {"mac.w d1.u,d2.l<<1,acc2", [&] { a.mac_x(false, 1, 2, 2, true, true, false, +1); }},
    {"move.l acc1,d3", [&] { a.mac_getacc(1, 3); }},
    {"move.l (a0,d1.l*4),d2", [&] { a.movel(A::Idx(0, 0, 1, 2), A::D(2)); }},
  };
  std::vector<u32> at;
  for (auto& c : cases) { at.push_back(a.here()); c.emit(); }
  auto code = a.finish(); s.load(a.base(), code);
  int bad = 0;
  for (size_t i = 0; i < cases.size(); i++) {
    int len = 0; std::string got = squash(dis1(s, at[i], &len)); u32 expect_len = (i + 1 < at.size() ? at[i + 1] : a.here()) - at[i];
    if (got != cases[i].want || (u32)len != expect_len) { std::printf("    [%zu] esperado '%s' (%u B), obtido '%s' (%d B)\n", i, cases[i].want, expect_len, got.c_str(), len); bad++; }
  }
  CHECK_EQ(bad, 0);
}

TEST(disasm_branches_and_symbols_and_full_format) {
  Soc s; A a(0x1000);
  a.label("top"); a.nop(); a.bne("top"); a.bsr("sub"); a.fbcc(0x12, "top"); a.lea(A::Full(0, 0x10, 1, 2, 2, 0), 1); a.lea(A::Full(0, 0, 1, 0, 1, 2, 0x10), 2); a.halt(); a.label("sub"); a.rts();
  auto code = a.finish(); s.load(a.base(), code);
  std::map<u32, std::string> syms = {{a.label_addr("top"), "top"}, {a.label_addr("sub"), "sub"}};
  std::function<std::string(u32)> sf = [&](u32 ad) { auto it = syms.find(ad); return it == syms.end() ? std::string() : it->second; };
  auto rd = [&](u32 ad) { return s.bus.raw_r16(ad); }; u32 p = a.base(); std::vector<std::string> out;
  for (int i = 0; i < 7; i++) { CfInsn in = cf_disasm(rd, p, &sf); out.push_back(squash(in.text)); p += in.len; }
  CHECK(out[1] == "bne.w top"); CHECK(out[2] == "bsr.w sub"); CHECK(out[3] == "fbgt.w top");
  CHECK(out[4] == "lea ($10,a0,d1.l*4),a1"); CHECK(out[5] == "lea ([$0,a0,d1.l],$10),a2");
}

TEST(disasm_length_agrees_with_the_interpreter_for_every_opcode) {
  Soc s; s.bus.cache_enabled = false;
  const u16 ext[4][3] = {{0x0000, 0x0000, 0x0000}, {0x0F00, 0x2468, 0x1357}, {0x8000, 0x0001, 0xA5A5}, {0x0120, 0x00B0, 0x4E71}};
  int checked = 0, mism = 0, unknown_but_ok = 0; std::vector<std::string> samples;
  for (int e = 0; e < 4; e++) for (u32 op = 0; op < 0x10000; op++) {
    s.cpu.reset_regs(); for (int i = 0; i < 8; i++) { s.cpu.d[i] = 0x12345 * (i + 1); s.cpu.a[i] = 0x20000 + i * 0x100; }
    s.cpu.a[7] = 0x00F00000;
    s.bus.raw_w32(0x1000, ((u32)op << 16) | ext[e][0]); s.bus.raw_w32(0x1004, ((u32)ext[e][1] << 16) | ext[e][2]); s.bus.raw_w32(0x1008, 0x4E714E71);
    s.bus.icache_inval_all(); s.cpu.set_pc(0x1000); s.bus.fault = false;
    auto rd = [&](u32 ad) { return s.bus.raw_r16(ad); }; CfInsn in = cf_disasm(rd, 0x1000);   // antes de executar (a instrução pode escrever no próprio código)
    u64 f0 = s.bus.ifetches; s.cpu.step();
    if (s.cpu.halted && s.cpu.reason != Cpu::HALT_INSN) continue;                 // exceção (sem vetor / falta dupla): não há como comparar
    if (s.cpu.reason == Cpu::HALT_INSN) { /* HALT: 1 palavra */ }
    int consumed = (int)(s.bus.ifetches - f0) * 2; checked++;
    if (!in.known) { unknown_but_ok++; if (samples.size() < 400) { char b[64]; std::snprintf(b, sizeof b, "op=%04x ext=%d: CPU executa, disasm desconhece", op, e); samples.push_back(b); } continue; }
    if (in.len != consumed) { mism++; if (samples.size() < 400) { char b[96]; std::snprintf(b, sizeof b, "op=%04x ext=%d: disasm %d B x CPU %d B (%s)", op, e, in.len, consumed, in.text.c_str()); samples.push_back(b); } }
  }
  for (auto& m : samples) std::printf("    %s\n", m.c_str());
  CHECK(checked > 20000); CHECK_EQ(mism, 0); CHECK_EQ(unknown_but_ok, 0);
}

#include "debugger.h"
#include "profiler.h"

static void prog_calls(Soc& s, A& a) {
  a.label("main"); a.moveq(5, 0); a.bsr("outer"); a.movel(A::Imm(0x1234), A::Abs(0x4000)); a.halt();
  a.label("outer"); a.link(6, 0); a.moveq(3, 1); a.bsr("inner"); a.bsr("inner"); a.addq(4, 1, A::D(1)); a.unlk(6); a.rts();
  a.label("inner"); a.link(6, 0); a.moveq(9, 2);
  a.label("loop"); a.subq(4, 1, A::D(2)); a.bpl("loop");
  a.label("inner_end"); a.movel(A::Imm(7), A::Abs(0x4004)); a.unlk(6); a.rts();
  auto code = a.finish(); s.load(a.base(), code); s.cpu.reset_regs(); s.cpu.a[7] = 0x00F00000; s.cpu.set_pc(a.base());
}

TEST(debugger_breakpoints_step_next_finish_backtrace) {
  Soc s; A a(0x1000); prog_calls(s, a); Debugger d(s); d.add_symbols(a.symbols());
  u32 v; CHECK(d.eval("inner+4", v)); CHECK_EQ(v, a.label_addr("inner") + 4); CHECK(d.eval("$10+0x20-3", v)); CHECK_EQ(v, 0x2D);
  d.add_breakpoint(a.label_addr("inner"));
  auto st = d.cont(); CHECK(st.why == Debugger::W_BREAKPOINT); CHECK_EQ(st.pc, a.label_addr("inner")); CHECK(d.symbol_at(st.pc) == "inner");
  d.step(2); auto bt = d.backtrace();                                      // dentro de inner após link: [pc, retorno em outer, retorno em main]
  CHECK_EQ(bt.size(), 3); CHECK(d.symbol_at(bt[1]).rfind("outer", 0) == 0); CHECK(d.symbol_at(bt[2]).rfind("main", 0) == 0);
  st = d.finish(); CHECK(st.why == Debugger::W_FINISHED); CHECK(d.symbol_at(st.pc).rfind("outer", 0) == 0);
  // next passa por cima da segunda chamada de inner: o breakpoint dentro dela interrompe, então removemos
  d.remove(1); st = d.next(); CHECK(st.why == Debugger::W_STEPPED); CHECK(d.symbol_at(s.cpu.pc).rfind("outer", 0) == 0);
  st = d.cont(); CHECK(st.why == Debugger::W_HALTED); CHECK_EQ(s.cpu.d[1], 4); s.bus.cache_flush_all(false); CHECK_EQ(s.bus.raw_r32(0x4000), 0x1234);
}

TEST(debugger_watchpoints_text_commands_and_registers) {
  Soc s; A a(0x1000); prog_calls(s, a); Debugger d(s); d.add_symbols(a.symbols());
  std::string o = d.exec("watch 0x4004 4 w"); CHECK(o.find("Watchpoint 1") != std::string::npos);
  o = d.exec("cont"); CHECK(o.find("Watchpoint 1: escrita de 0x00004004") != std::string::npos); CHECK(o.find("unlk") != std::string::npos);   // pára depois da escrita
  CHECK(d.exec("info break").find("watchpoint 0x00004004") != std::string::npos);
  d.exec("delete 1"); o = d.exec("break outer"); CHECK(o.find("Breakpoint 2") != std::string::npos);
  d.exec("set pc main"); d.exec("set d3 7"); CHECK_EQ(s.cpu.d[3], 7); CHECK_EQ(s.cpu.pc, a.label_addr("main"));
  o = d.exec("cont"); CHECK(o.find("Breakpoint 2") != std::string::npos);
  o = d.exec("regs"); CHECK(o.find("d3=00000007") != std::string::npos); CHECK(o.find("pc=") != std::string::npos);
  o = d.exec("dis outer 3"); CHECK(o.find("link") != std::string::npos); CHECK(o.find("a6,#$0") != std::string::npos || o.find("a6,#") != std::string::npos);
  o = d.exec("x/2l 0x1000"); CHECK(o.find("0x00001000:") != std::string::npos);
  o = d.exec("print outer+2"); CHECK(o.find(std::to_string(a.label_addr("outer") + 2)) != std::string::npos);
  o = d.exec("bt"); CHECK(o.find("outer") != std::string::npos);
  CHECK(d.exec("frobnicate").find("desconhecido") != std::string::npos);
  d.exec("set *0x5000 0xCAFEBABE"); CHECK_EQ(s.bus.raw_r32(0x5000), 0xCAFEBABE);
}

TEST(profiler_attributes_cycles_to_functions_and_instructions) {
  Soc s; A a(0x1000); prog_calls(s, a); Profiler p(s); p.symbols = {{"main", a.label_addr("main")}, {"outer", a.label_addr("outer")}, {"inner", a.label_addr("inner")}}; p.start();
  CHECK(s.run_until_halt()); p.stop();
  auto fs = p.functions(); CHECK(!fs.empty()); CHECK(fs[0].name == "inner");                               // o laço domina
  u64 total = 0; for (auto& f : fs) total += f.excl; CHECK_EQ(total, p.total_cycles());
  CHECK_EQ(total, s.cpu.cycles);
  Profiler::Func inner, outer, mainf; for (auto& f : fs) { if (f.name == "inner" || f.name.rfind("inner_", 0) == 0) { if (f.name == "inner") inner = f; } if (f.name == "outer") outer = f; if (f.name == "main") mainf = f; }
  CHECK_EQ(inner.calls, 2); CHECK_EQ(outer.calls, 1);
  CHECK(outer.incl >= inner.incl && outer.incl > outer.excl);                                              // inclusivo de outer cobre as duas chamadas de inner
  CHECK(mainf.incl >= outer.incl);
  CHECK(p.cycles_at(a.label_addr("loop")) > 0);
  std::string r = p.report(5); CHECK(r.find("inner") != std::string::npos); CHECK(r.find("VPU-A") != std::string::npos); CHECK(r.find("D-cache") != std::string::npos); CHECK(r.find("subq.l") != std::string::npos);
}
