// snowcc: gerador de código para o ColdFire (SnowFire-CF266) + driver (cc_compile).
// Convenção: argumentos empilhados da direita para a esquerda (slots de 32 bits), retorno em D0, quem chama limpa a pilha.
// Quadro: LINK A6. D0-D2/A0-A1 são temporários; nada é preservado entre chamadas (os locais vivem na pilha).
#include "snowcc_int.h"
#include "cfdis.h"
#include <fstream>
#include <sstream>

namespace sf { namespace cc {
namespace {

using Op = CfAsm::Op;
using A_ = CfAsm;

class Gen {
 public:
  Gen(Program& p, CfAsm& a, const CcOptions& o) : P(p), A(a), opt(o) {}
  void run();

 private:
  Program& P; CfAsm& A; const CcOptions& opt; FuncDef* fn = nullptr; int ret_label = 0;
  std::vector<int> brk, cont;

  static std::string L(int id) { return ".L" + std::to_string(id); }
  int newl() { return P.next_label++; }
  void label(int id) { A.label(L(id)); }
  [[noreturn]] void err(int line, const std::string& m) { fail(fn ? "" : "", line, m); }

  // ---------- constantes, extensão, carga/armazenamento ----------
  void const_to(i64 v, int reg) { if (v >= -128 && v <= 127) A.moveq((int)v, reg); else A.movel(A_::Imm((u32)v), A_::D(reg)); }
  void extend(const Type* t) {
    if (t->k == TY_CHAR) { if (t->uns) A.andi(4, 0xFF, A_::D(0)); else A.extb_l(0); }
    else if (t->k == TY_SHORT) { if (t->uns) A.andi(4, 0xFFFF, A_::D(0)); else A.ext_l(0); }
  }
  void load_ea(const Type* t, const Op& ea, int reg) {
    int sz = tsize(t);
    if (sz == 4) A.movel(ea, A_::D(reg)); else if (t->uns) A.mvz(sz, ea, reg); else A.mvs(sz, ea, reg);
  }
  void store_ea(const Type* t, int reg, const Op& ea) { A.move(tsize(t), A_::D(reg), ea); }
  void push_d0() { A.movel(A_::D(0), A_::Pre(7)); }
  void pop_to(int reg) { A.movel(A_::Post(7), A_::D(reg)); }

  bool simple_ea(Expr* e, Op& ea) {
    if (e->k != E_VAR || !is_scalar(e->ty)) return false;
    Sym* s = e->sym;
    if (s->kind == S_LOCAL || s->kind == S_PARAM) { ea = A_::Disp(6, s->off); return true; }
    if (s->kind == S_GLOBAL) { ea = A_::AbsL(s->label); return true; }
    return false;
  }
  bool simple_value(Expr* e) { Op o; return e->k == E_NUM || simple_ea(e, o); }
  void load_simple(Expr* e, int reg) { if (e->k == E_NUM) { const_to(e->num, reg); return; } Op ea; simple_ea(e, ea); load_ea(e->ty, ea, reg); }

  // ---------- endereços ----------
  void genaddr(Expr* e) {                                      // -> A0
    switch (e->k) {
      case E_VAR: { Sym* s = e->sym;
        if (s->kind == S_LOCAL || s->kind == S_PARAM) A.lea(A_::Disp(6, s->off), 0); else A.lea(A_::AbsL(s->kind == S_FUNC ? s->label : s->label), 0); return; }
      case E_STR: A.lea(A_::AbsL(".LS" + std::to_string(e->num)), 0); return;
      case E_DEREF: gen(e->a); A.movel(A_::D(0), A_::A(0)); return;
      case E_MEMBER: genaddr(e->a); if (e->num) A.lea(A_::Disp(0, (int)e->num), 0); return;
      default: err(e->line, "expressão sem endereço");
    }
  }

  // ---------- operadores ----------
  static int cc_for(const std::string& op, bool uns) {
    if (op == "==") return 7; if (op == "!=") return 6;
    if (op == "<") return uns ? 5 : 13; if (op == ">=") return uns ? 4 : 12; if (op == ">") return uns ? 2 : 14; return uns ? 3 : 15;   // "<="
  }
  static bool is_rel(const std::string& op) { return op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">="; }
  static int log2_exact(i64 v) { if (v <= 0 || (v & (v - 1))) return -1; int n = 0; while ((1ll << n) < v) n++; return n; }

  void shift_const(bool left, bool arith, int cnt) {
    while (cnt > 0) { int n = std::min(8, cnt); A.shift_i(arith ? 0 : 1, left, n, 0); cnt -= n; }
  }
  // d0 = d0 <op> d1
  void arith_reg(const std::string& op, bool uns) {
    if (op == "+") A.add(4, A_::D(1), 0); else if (op == "-") A.sub(4, A_::D(1), 0);
    else if (op == "*") { if (uns) A.mulu_l(A_::D(1), 0); else A.muls_l(A_::D(1), 0); }
    else if (op == "/") { if (uns) A.divu_l(A_::D(1), 0, 0); else A.divs_l(A_::D(1), 0, 0); }
    else if (op == "%") { if (uns) A.divu_l(A_::D(1), 0, 1); else A.divs_l(A_::D(1), 0, 1); A.movel(A_::D(1), A_::D(0)); }
    else if (op == "&") A.and_(4, A_::D(1), 0); else if (op == "|") A.or_(4, A_::D(1), 0); else if (op == "^") A.eor(4, 1, A_::D(0));
    else if (op == "<<") A.shift_r(1, true, 1, 0); else if (op == ">>") A.shift_r(uns ? 1 : 0, false, 1, 0);
    else err(0, "operador desconhecido " + op);
  }
  // d0 = d0 <op> imm (com atalhos)
  void arith_imm(const std::string& op, bool uns, i64 v) {
    if ((op == "+" || op == "-") && v >= 1 && v <= 8) { if (op == "+") A.addq(4, (int)v, A_::D(0)); else A.subq(4, (int)v, A_::D(0)); return; }
    if (op == "+") { A.addi(4, (u32)v, A_::D(0)); return; } if (op == "-") { A.subi(4, (u32)v, A_::D(0)); return; }
    if (op == "&") { A.andi(4, (u32)v, A_::D(0)); return; } if (op == "|") { A.ori(4, (u32)v, A_::D(0)); return; } if (op == "^") { A.eori(4, (u32)v, A_::D(0)); return; }
    if (op == "<<") { shift_const(true, false, (int)(v & 31)); return; } if (op == ">>") { shift_const(false, !uns, (int)(v & 31)); return; }
    int lg = log2_exact(v);
    if (op == "*" && lg >= 0) { shift_const(true, false, lg); return; }
    if (op == "/" && uns && lg >= 0) { shift_const(false, false, lg); return; }
    if (op == "%" && uns && lg >= 0) { A.andi(4, (u32)(v - 1), A_::D(0)); return; }
    const_to(v, 1); arith_reg(op, uns);
  }
  void binary_operands(Expr* a, Expr* b) {                     // a -> D0, b -> D1
    if (simple_value(b)) { gen(a); load_simple(b, 1); return; }
    gen(a); push_d0(); gen(b); A.movel(A_::D(0), A_::D(1)); pop_to(0);
  }
  void gen_compare(Expr* e) {                                  // deixa os flags de (a - b)
    if (e->b->k == E_NUM) { gen(e->a); A.cmpi(4, (u32)e->b->num, A_::D(0)); return; }
    binary_operands(e->a, e->b); A.cmp(4, A_::D(1), 0);
  }

  // salta para 'lbl' se a veracidade de e == when; senão segue adiante
  void branch(Expr* e, int lbl, bool when) {
    if (e->k == E_NUM) { if ((e->num != 0) == when) A.bra(L(lbl)); return; }
    if (e->k == E_UN && e->op == "!") { branch(e->a, lbl, !when); return; }
    if (e->k == E_BIN && is_rel(e->op)) { gen_compare(e); int cc = cc_for(e->op, e->uns); A.bcc(when ? cc : (cc ^ 1), L(lbl)); return; }
    if (e->k == E_BIN && e->op == "&&") {
      if (when) { int skip = newl(); branch(e->a, skip, false); branch(e->b, lbl, true); label(skip); }
      else { branch(e->a, lbl, false); branch(e->b, lbl, false); }
      return;
    }
    if (e->k == E_BIN && e->op == "||") {
      if (when) { branch(e->a, lbl, true); branch(e->b, lbl, true); }
      else { int skip = newl(); branch(e->a, skip, true); branch(e->b, lbl, false); label(skip); }
      return;
    }
    gen(e); A.tst(4, A_::D(0)); A.bcc(when ? 6 : 7, L(lbl));
  }

  // ---------- expressões ----------
  void gen_call(Expr* e);
  void gen_builtin(Expr* e);
  void gen_assign(Expr* e);
  void gen_incdec(Expr* e, bool effect_only);
  void struct_copy(Expr* dst, Expr* src);

  void gen(Expr* e) {                                          // resultado em D0
    switch (e->k) {
      case E_NUM: const_to(e->num, 0); return;
      case E_STR: A.movel(A_::ImmL(".LS" + std::to_string(e->num)), A_::D(0)); return;
      case E_VAR: {
        if (e->ty->k == TY_FUNC) { A.movel(A_::ImmL(e->sym->label), A_::D(0)); return; }
        Op ea; if (!simple_ea(e, ea)) err(e->line, "uso inválido de '" + e->sym->name + "' como valor");
        load_ea(e->ty, ea, 0); return;
      }
      case E_ADDR: {
        Expr* a = e->a;
        if (a->k == E_VAR && (a->sym->kind == S_GLOBAL || a->sym->kind == S_FUNC)) { A.movel(A_::ImmL(a->sym->label), A_::D(0)); return; }
        if (a->k == E_STR) { A.movel(A_::ImmL(".LS" + std::to_string(a->num)), A_::D(0)); return; }
        genaddr(a); A.movel(A_::A(0), A_::D(0)); return;
      }
      case E_DEREF: case E_MEMBER:
        if (!is_scalar(e->ty)) err(e->line, "valor de estrutura/array em contexto escalar");
        if (e->k == E_DEREF) { gen(e->a); A.movel(A_::D(0), A_::A(0)); } else genaddr(e);
        load_ea(e->ty, A_::Ind(0), 0); return;
      case E_UN:
        if (e->op == "!") { value_of_bool(e); return; }
        gen(e->a); if (e->op == "-") A.neg(4, A_::D(0)); else A.not_(4, A_::D(0)); return;
      case E_BIN: {
        const std::string& op = e->op;
        if (op == "&&" || op == "||") { value_of_bool(e); return; }
        if (is_rel(op)) { gen_compare(e); A.scc(cc_for(op, e->uns), A_::D(0)); A.extb_l(0); A.neg(4, A_::D(0)); return; }
        if (op == "ptrdiff") {
          binary_operands(e->a, e->b); A.sub(4, A_::D(1), 0);
          int lg = log2_exact(e->num); if (e->num == 1) return; if (lg >= 0) { shift_const(false, true, lg); return; }
          const_to(e->num, 1); A.divs_l(A_::D(1), 0, 0); return;
        }
        if (e->b->k == E_NUM) { gen(e->a); arith_imm(op, e->uns, e->b->num); return; }
        binary_operands(e->a, e->b); arith_reg(op, e->uns); return;
      }
      case E_ASSIGN: gen_assign(e); return;
      case E_INCDEC: gen_incdec(e, false); return;
      case E_COND: { int lf = newl(), le = newl(); branch(e->a, lf, false); gen(e->b); A.bra(L(le)); label(lf); gen(e->c); label(le); return; }
      case E_CALL: gen_call(e); return;
      case E_CAST: gen(e->a); if (e->ty->k == TY_CHAR || e->ty->k == TY_SHORT) extend(e->ty); return;
      case E_COMMA: gen(e->a); gen(e->b); return;
    }
  }
  void value_of_bool(Expr* e) { int lf = newl(), le = newl(); branch(e, lf, false); A.moveq(1, 0); A.bra(L(le)); label(lf); A.moveq(0, 0); label(le); }

  void gen_effect(Expr* e) {
    if (e->k == E_INCDEC) { gen_incdec(e, true); return; }
    if (e->k == E_COMMA) { gen_effect(e->a); gen_effect(e->b); return; }
    gen(e);
  }

  // ---------- comandos ----------
  void stmt(Stmt* s);
  void function(FuncDef* f);
  void emit_data();
};

void Gen::struct_copy(Expr* dst, Expr* src) {
  int n = tsize(dst->ty);
  genaddr(src); A.movel(A_::A(0), A_::Pre(7)); genaddr(dst); A.movel(A_::Post(7), A_::A(1));      // a1 = origem, a0 = destino
  if (n % 4 == 0 && n <= 64) { for (int i = 0; i < n / 4; i++) A.movel(A_::Post(1), A_::Post(0)); return; }
  bool w = n % 4 == 0; int cnt = w ? n / 4 : n; int lp = newl(); const_to(cnt, 2);
  label(lp); if (w) A.movel(A_::Post(1), A_::Post(0)); else A.move(1, A_::Post(1), A_::Post(0));
  A.subq(4, 1, A_::D(2)); A.bne(L(lp));
}

void Gen::gen_assign(Expr* e) {
  Expr* a = e->a; Type* t = e->ty; const std::string& op = e->op;
  if (t->k == TY_STRUCT) { struct_copy(a, e->b); return; }
  Op ea; bool simple = simple_ea(a, ea);
  if (op == "=") {
    if (simple) { gen(e->b); store_ea(t, 0, ea); }
    else { gen(e->b); push_d0(); genaddr(a); pop_to(0); store_ea(t, 0, A_::Ind(0)); }
    extend(t); return;
  }
  std::string bop = op.substr(0, op.size() - 1);
  if (simple) {
    if (simple_value(e->b)) load_simple(e->b, 1); else { gen(e->b); A.movel(A_::D(0), A_::D(1)); }
    load_ea(t, ea, 0); arith_reg(bop, e->uns); store_ea(t, 0, ea);
  } else {
    genaddr(a); A.movel(A_::A(0), A_::Pre(7)); gen(e->b); A.movel(A_::D(0), A_::D(1)); A.movel(A_::Post(7), A_::A(0));
    load_ea(t, A_::Ind(0), 0); arith_reg(bop, e->uns); store_ea(t, 0, A_::Ind(0));
  }
  extend(t);
}

void Gen::gen_incdec(Expr* e, bool effect_only) {
  Expr* a = e->a; Type* t = e->ty; int step = (int)e->num; bool inc = e->op == "++"; Op ea; bool simple = simple_ea(a, ea);
  auto add_step = [&](int reg) {
    if (step >= 1 && step <= 8) { if (inc) A.addq(4, step, A_::D(reg)); else A.subq(4, step, A_::D(reg)); }
    else { if (inc) A.addi(4, (u32)step, A_::D(reg)); else A.subi(4, (u32)step, A_::D(reg)); }
  };
  if (simple && effect_only && tsize(t) == 4 && step >= 1 && step <= 8) { if (inc) A.addq(4, step, ea); else A.subq(4, step, ea); return; }
  if (!simple) { genaddr(a); ea = A_::Ind(0); }
  load_ea(t, ea, 0);
  if (e->post) { A.movel(A_::D(0), A_::D(1)); add_step(1); store_ea(t, 1, ea); }
  else { add_step(0); store_ea(t, 0, ea); extend(t); }
}

void Gen::gen_builtin(Expr* e) {
  const std::string& n = e->a->sym->name; auto& ar = e->args;
  auto need = [&](size_t k) { if (ar.size() != k) err(e->line, "'" + n + "' espera " + std::to_string(k) + " argumento(s)"); };
  auto cnum = [&](Expr* x) { if (x->k != E_NUM) err(e->line, "'" + n + "' exige argumento constante"); return x->num; };
  if (n == "__halt") { need(0); A.halt(); }
  else if (n == "__nop") { need(0); A.nop(); }
  else if (n == "__stop") { need(1); A.stop((u16)cnum(ar[0])); }
  else if (n == "__trap") { need(1); i64 v = cnum(ar[0]); if (v < 0 || v > 15) err(e->line, "TRAP #0..15"); A.trap((int)v); }
  else if (n == "__trapcall") {
    if (ar.size() < 1 || ar.size() > 4) err(e->line, "__trapcall(n, a, b, c)");
    i64 v = cnum(ar[0]); if (v < 0 || v > 15) err(e->line, "TRAP #0..15");
    int k = (int)ar.size() - 1;
    for (int i = k; i >= 1; i--) { gen(ar[i]); push_d0(); }
    for (int i = 0; i < k; i++) pop_to(i);
    A.trap((int)v);
  }
  else if (n == "__set_vbr") { need(1); gen(ar[0]); A.movec_to(0x801, false, 0); }
  else if (n == "__set_cacr") { need(1); gen(ar[0]); A.movec_to(0x002, false, 0); }
  else if (n == "__get_sp") { need(0); A.movel(A_::A(7), A_::D(0)); }
  else if (n == "__set_sp") { need(1); gen(ar[0]); A.movel(A_::D(0), A_::A(7)); }
  else if (n == "__get_sr") { need(0); A.move_from_sr(0); A.andi(4, 0xFFFF, A_::D(0)); }
  else if (n == "__set_sr") { need(1); gen(ar[0]); A.move_to_sr(A_::D(0)); }
  else if (n == "__get_usp") { need(0); A.move_usp_to(0); A.movel(A_::A(0), A_::D(0)); }
  else if (n == "__set_usp") { need(1); gen(ar[0]); A.movel(A_::D(0), A_::A(0)); A.move_usp_from(0); }
  else err(e->line, "intrínseco desconhecido " + n);
}

void Gen::gen_call(Expr* e) {
  if (e->a->k == E_VAR && e->a->sym && e->a->sym->kind == S_BUILTIN) { gen_builtin(e); return; }
  int n = (int)e->args.size();
  for (int i = n - 1; i >= 0; i--) { gen(e->args[i]); push_d0(); }
  Expr* c = e->a;
  if (c->k == E_VAR && c->sym->kind == S_FUNC) A.jsr(A_::AbsL(c->sym->label));
  else { gen(c); A.movel(A_::D(0), A_::A(0)); A.jsr(A_::Ind(0)); }
  if (n) { if (n <= 2) A.addq(4, 4 * n, A_::A(7)); else A.lea(A_::Disp(7, 4 * n), 7); }
}

void Gen::stmt(Stmt* s) {
  switch (s->k) {
    case ST_NOP: return;
    case ST_EXPR: gen_effect(s->e); return;
    case ST_BLOCK: for (auto x : s->list) stmt(x); return;
    case ST_IF: {
      int le = newl(); branch(s->e, le, false); stmt(s->body);
      if (s->els) { int lend = newl(); A.bra(L(lend)); label(le); stmt(s->els); label(lend); } else label(le);
      return;
    }
    case ST_WHILE: {
      int top = newl(), end = newl(); label(top); branch(s->e, end, false);
      brk.push_back(end); cont.push_back(top); stmt(s->body); brk.pop_back(); cont.pop_back(); A.bra(L(top)); label(end); return;
    }
    case ST_DO: {
      int top = newl(), c = newl(), end = newl(); label(top); brk.push_back(end); cont.push_back(c); stmt(s->body); brk.pop_back(); cont.pop_back();
      label(c); branch(s->e, top, true); label(end); return;
    }
    case ST_FOR: {
      for (auto x : s->list) stmt(x); if (s->e) gen_effect(s->e);
      int top = newl(), c = newl(), end = newl(); label(top); if (s->e2) branch(s->e2, end, false);
      brk.push_back(end); cont.push_back(c); stmt(s->body); brk.pop_back(); cont.pop_back();
      label(c); if (s->e3) gen_effect(s->e3); A.bra(L(top)); label(end); return;
    }
    case ST_RETURN: if (s->e) { gen(s->e); extend(fn->ret); } A.bra(L(ret_label)); return;
    case ST_BREAK: A.bra(L(brk.back())); return;
    case ST_CONTINUE: A.bra(L(cont.back())); return;
    case ST_SWITCH: {
      gen(s->e); int end = newl();
      for (auto& c : s->cases) { A.cmpi(4, (u32)c.first, A_::D(0)); A.beq(L(c.second)); }
      A.bra(L(s->default_label >= 0 ? s->default_label : end));
      brk.push_back(end); stmt(s->body); brk.pop_back(); label(end); return;
    }
    case ST_CASE: case ST_DEFAULT: case ST_LABEL: label(s->label); stmt(s->body); return;
    case ST_GOTO: A.bra(L(s->label)); return;
  }
}

void Gen::function(FuncDef* f) {
  fn = f; ret_label = newl(); A.align(2);
  std::string body = f->interrupt ? f->name + "__body" : f->name;
  if (f->interrupt) {
    A.align(2); A.label(f->name);                                   // stub: salva D0-D7/A0-A6, chama o corpo com SP, adota o SP devolvido
    A.lea(A_::Disp(7, -64), 7); A.movem_to(0x7FFF, A_::Ind(7));
    A.movel(A_::A(7), A_::Pre(7)); A.jsr(A_::AbsL(body)); A.addq(4, 4, A_::A(7));
    A.movel(A_::D(0), A_::A(7)); A.movem_from(A_::Ind(7), 0x7FFF); A.lea(A_::Disp(7, 64), 7); A.rte();
  }
  A.label(body); A.link(6, -f->frame);
  stmt(f->body);
  label(ret_label); A.unlk(6); A.rts();
  fn = nullptr;
}

void Gen::emit_data() {
  A.align(4);
  for (size_t i = 0; i < P.strings.size(); i++) {
    A.label(".LS" + std::to_string(i)); for (char c : P.strings[i]) A.data_b((u8)c); A.data_b(0); A.align(4);
  }
  for (Sym* g : P.globals) {
    if (g->is_extern && !g->has_init) { bool used = true; (void)used; continue; }
    A.align(4); A.label(g->label);
    int sz = tsize(g->ty); std::vector<u8> buf(sz, 0); std::map<int, std::pair<std::string, i32>> relocs;
    for (auto& it : g->init) {
      if (!it.ref.empty()) { relocs[it.off] = {it.ref, (i32)it.val}; continue; }
      for (int b = 0; b < it.size; b++) buf[it.off + b] = (u8)((u64)it.val >> (8 * (it.size - 1 - b)));
    }
    for (int i = 0; i < sz;) { auto r = relocs.find(i); if (r != relocs.end()) { A.data_ref(r->second.first, r->second.second); i += 4; } else { A.data_b(buf[i]); i++; } }
    A.align(4);
  }
}

void Gen::run() {
  Sym* mainsym = P.find_global("main");
  if (opt.crt0) {
    A.label("_start"); A.movel(A_::Imm(opt.stack_top), A_::A(7));
    if (mainsym && mainsym->kind == S_FUNC) A.jsr(A_::AbsL("main")); A.halt();
  }
  for (auto& f : P.funcs) function(f.get());
  A.align(4); A.label("_code_end");
  emit_data(); A.align(4); A.label("_end");
}

std::string make_listing(const std::vector<u8>& img, u32 base, u32 code_end, const std::map<std::string, u32>& syms) {
  std::multimap<u32, std::string> by; for (auto& kv : syms) by.insert({kv.second, kv.first});
  std::function<std::string(u32)> sf = [&](u32 a) { auto r = by.equal_range(a); for (auto it = r.first; it != r.second; ++it) if (it->second[0] != '.' || true) return it->second; return std::string(); };
  auto rd = [&](u32 a) -> u16 { if (a < base || a + 2 > base + img.size()) return 0; return rbe16(&img[a - base]); };
  std::string o; char b[160]; u32 pc = base;
  while (pc < code_end) {
    auto r = by.equal_range(pc); for (auto it = r.first; it != r.second; ++it) if (it->second != "_code_end") { o += it->second + ":\n"; }
    CfInsn in = cf_disasm(rd, pc, &sf); std::string hex;
    for (int i = 0; i < in.len && i < 10; i += 2) { std::snprintf(b, sizeof b, "%04x ", rd(pc + i)); hex += b; }
    std::snprintf(b, sizeof b, "  %08x  %-26s %s\n", pc, hex.c_str(), in.text.c_str()); o += b; pc += in.len;
  }
  o += "; ---- dados ----\n";
  for (u32 a = code_end; a < base + img.size(); a += 4) {
    auto r = by.equal_range(a); for (auto it = r.first; it != r.second; ++it) if (it->second != "_code_end" && it->second != "_end") o += it->second + ":\n";
    if (a + 4 <= base + img.size()) { std::snprintf(b, sizeof b, "  %08x  .long $%08x\n", a, rbe32(&img[a - base])); o += b; }
  }
  return o;
}

}  // namespace
}}  // namespace sf::cc

namespace sf {

CcResult cc_compile(const std::string& source, const std::string& filename, const CcOptions& opt) {
  CcResult r; r.base = opt.base;
  try {
    auto toks = cc::preprocess_and_lex(source, filename, opt);
    cc::Program prog; cc::parse_program(toks, prog);
    CfAsm a(opt.base); cc::Gen g(prog, a, opt); g.run();
    std::map<std::string, u32> labels = a.symbols();
    r.image = a.finish();
    r.code_end = a.label_addr("_code_end");
    for (auto& f : prog.funcs) r.symbols[f->name] = a.label_addr(f->name);
    for (auto* gv : prog.globals) if (a.has_label(gv->label)) r.symbols[gv->label] = a.label_addr(gv->label);
    r.symbols["_end"] = a.label_addr("_end");
    r.entry = opt.crt0 ? a.label_addr("_start") : opt.base;
    r.listing = cc::make_listing(r.image, opt.base, r.code_end, labels);
    r.ok = true;
  } catch (cc::CcError& e) { r.error = e.msg; }
  catch (std::runtime_error& e) {
    std::string m = e.what();
    if (m.rfind("rótulo indefinido: ", 0) == 0) r.error = "símbolo indefinido: " + m.substr(std::strlen("rótulo indefinido: "));
    else if (m.rfind("desvio fora de alcance", 0) == 0) r.error = "função grande demais (desvio fora de alcance de 16 bits)";
    else r.error = m;
  }
  return r;
}

CcResult cc_compile_file(const std::string& path, CcOptions opt) {
  std::ifstream f(path, std::ios::binary); CcResult r;
  if (!f) { r.error = "não consegui abrir " + path; return r; }
  std::stringstream ss; ss << f.rdbuf();
  size_t sl = path.find_last_of('/'); opt.include_dirs.push_back(sl == std::string::npos ? "." : path.substr(0, sl));
  return cc_compile(ss.str(), path, opt);
}

}  // namespace sf
