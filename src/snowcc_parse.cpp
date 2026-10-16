// snowcc: analisador sintático + semântico (tipos, escopos, constantes) que produz a AST usada pelo gerador de código.
#include "snowcc_int.h"

namespace sf { namespace cc {

int tsize(const Type* t) {
  switch (t->k) {
    case TY_VOID: return 1; case TY_CHAR: return 1; case TY_SHORT: return 2; case TY_INT: case TY_PTR: return 4;
    case TY_ARRAY: return t->len * tsize(t->base); case TY_STRUCT: return t->sd->size; case TY_FUNC: return 1;
  }
  return 4;
}
int talign(const Type* t) {
  switch (t->k) { case TY_CHAR: case TY_VOID: case TY_FUNC: return 1; case TY_SHORT: return 2; case TY_ARRAY: return talign(t->base); case TY_STRUCT: return t->sd->align; default: return 4; }
}
Sym* Program::find_global(const std::string& n) const { auto it = gtab.find(n); return it == gtab.end() ? nullptr : it->second; }

namespace {

static const char* const kKeywords[] = {"void", "char", "short", "int", "long", "unsigned", "signed", "struct", "union", "enum", "const", "volatile", "static", "extern", "typedef", "register", "auto", "inline",
                                        "if", "else", "while", "do", "for", "return", "break", "continue", "switch", "case", "default", "sizeof", "__interrupt", "goto"};
static bool is_kw(const std::string& s) { for (auto k : kKeywords) if (s == k) return true; return false; }

class Parser {
 public:
  Parser(const std::vector<Token>& t, Program& p) : T(t), P(p) {
    t_void = mkt(TY_VOID); t_char = mkt(TY_CHAR); t_uchar = mkt(TY_CHAR); t_uchar->uns = true; t_short = mkt(TY_SHORT); t_ushort = mkt(TY_SHORT); t_ushort->uns = true;
    t_int = mkt(TY_INT); t_uint = mkt(TY_INT); t_uint->uns = true;
    scopes.emplace_back();
    static const struct { const char* n; int ret; } bi[] = {{"__halt", 0}, {"__nop", 0}, {"__stop", 0}, {"__trap", 0}, {"__trapcall", 1}, {"__set_vbr", 0}, {"__get_sp", 1}, {"__set_sp", 0},
                                                           {"__get_sr", 1}, {"__set_sr", 0}, {"__get_usp", 1}, {"__set_usp", 0}, {"__set_cacr", 0}};
    for (auto& b : bi) { Sym* s = mks(b.n, S_BUILTIN); s->ty = mkt(TY_FUNC); s->ty->base = b.ret ? t_uint : t_void; scopes[0][b.n] = s; }
  }
  void run();

 private:
  const std::vector<Token>& T; Program& P; size_t p = 0;
  Type *t_void, *t_char, *t_uchar, *t_short, *t_ushort, *t_int, *t_uint;
  std::vector<std::unordered_map<std::string, Sym*>> scopes; std::unordered_map<std::string, StructDef*> tags;
  FuncDef* fn = nullptr; std::map<std::string, std::pair<int, int>> lbls;   // rótulos da função: nome -> {id, linha da definição (0 = só usado)}
  std::map<std::string, int> lbl_use_line; std::vector<Stmt*> sw_stack; int static_counter = 0;
  std::vector<std::string> last_pnames; bool last_variadic = false;

  // ---- infraestrutura ----
  [[noreturn]] void err(const std::string& m) { const Token& t = T[std::min(p, T.size() - 1)]; fail(t.file, t.line, m); }
  const Token& cur() const { return T[p]; }
  bool is_op(const char* s) const { return T[p].t == T_OP && T[p].s == s; }
  bool is_id(const char* s) const { return T[p].t == T_ID && T[p].s == s; }
  bool accept_op(const char* s) { if (is_op(s)) { p++; return true; } return false; }
  bool accept_id(const char* s) { if (is_id(s)) { p++; return true; } return false; }
  void expect_op(const char* s) { if (!accept_op(s)) err(std::string("esperava '") + s + "' mas achei '" + (cur().t == T_EOF ? "fim do arquivo" : cur().s) + "'"); }
  int line() const { return T[p].line; }

  Type* mkt(TK k) { P.types.emplace_back(new Type()); P.types.back()->k = k; return P.types.back().get(); }
  Type* ptr_to(Type* b) { Type* t = mkt(TY_PTR); t->base = b; return t; }
  Type* array_of(Type* b, int n) { Type* t = mkt(TY_ARRAY); t->base = b; t->len = n; return t; }
  Sym* mks(const std::string& n, SK k) { P.syms.emplace_back(new Sym()); P.syms.back()->name = n; P.syms.back()->kind = k; return P.syms.back().get(); }
  Expr* mke(EK k, int ln) { P.exprs.emplace_back(new Expr()); P.exprs.back()->k = k; P.exprs.back()->line = ln; return P.exprs.back().get(); }
  Stmt* mkst(StK k, int ln) { P.stmts.emplace_back(new Stmt()); P.stmts.back()->k = k; P.stmts.back()->line = ln; return P.stmts.back().get(); }
  Sym* lookup(const std::string& n) { for (size_t i = scopes.size(); i-- > 0;) { auto it = scopes[i].find(n); if (it != scopes[i].end()) return it->second; } return nullptr; }
  int new_label() { return P.next_label++; }

  // ---- tipos ----
  bool is_type_start() {
    if (cur().t != T_ID) return false;
    const std::string& s = cur().s;
    if (s == "void" || s == "char" || s == "short" || s == "int" || s == "long" || s == "unsigned" || s == "signed" || s == "struct" || s == "union" || s == "enum" || s == "const" || s == "volatile" ||
        s == "static" || s == "extern" || s == "typedef" || s == "register" || s == "auto" || s == "inline" || s == "__interrupt") return true;
    Sym* sy = lookup(s); return sy && sy->kind == S_TYPEDEF;
  }
  struct Spec { bool is_static = false, is_extern = false, is_typedef = false, interrupt = false; };
  Type* parse_base_type(Spec& sp);
  Type* parse_declarator(Type* base, std::string& name, bool abstract_ok = false);
  void parse_params(std::vector<Type*>& pt, std::vector<std::string>& pn, bool& proto);
  Type* parse_type_name();
  Type* struct_spec();
  void enum_spec();

  // ---- expressões ----
  Expr* decay(Expr* e);
  Type* arith(Type* a, Type* b) { return ((a->k == TY_INT && a->uns) || (b->k == TY_INT && b->uns)) ? t_uint : t_int; }
  Type* promoted(Type* t) { return (t->k == TY_CHAR || t->k == TY_SHORT) ? t_int : t; }
  Expr* num(i64 v, Type* t, int ln) { Expr* e = mke(E_NUM, ln); e->num = v; e->ty = t; return e; }
  i64 wrap(i64 v, Type* t) {
    if (t->k == TY_CHAR) return t->uns ? (i64)(u8)v : (i64)(i8)v; if (t->k == TY_SHORT) return t->uns ? (i64)(u16)v : (i64)(i16)v;
    return t->uns || t->k == TY_PTR ? (i64)(u32)v : (i64)(i32)v;
  }
  Expr* make_binary(const std::string& op, Expr* a, Expr* b, int ln);
  Expr* make_unary_deref(Expr* a, int ln);
  Expr* make_assign(const std::string& op, Expr* a, Expr* b, int ln);
  Expr* make_index(Expr* base, Expr* idx, int ln) { return make_unary_deref(make_binary("+", base, idx, ln), ln); }
  Expr* expression();         // com vírgula
  Expr* assignment();
  Expr* conditional();
  Expr* binary_rhs(int minprec, Expr* lhs);
  Expr* cast_expr();
  Expr* unary();
  Expr* postfix();
  Expr* primary();
  Expr* parse_call(Expr* callee, int ln);
  bool const_eval(Expr* e, i64& v, std::string& ref);
  i64 const_int(Expr* e);

  // ---- declarações / comandos ----
  void toplevel();
  Sym* declare_global(const std::string& name, Type* ty, const Spec& sp, int ln, bool allow_init);
  Sym* declare_func(const std::string& name, Type* fty, int ln);
  void parse_ginit(Type* ty, std::vector<GInit>& out, int off);
  void func_definition(Type* fty, const std::string& name, const std::vector<std::string>& pn, const Spec& sp, int ln);
  Stmt* statement();
  Stmt* block(bool new_scope = true);
  void local_decl(std::vector<Stmt*>& out);
  void local_init(Sym* s, std::vector<Stmt*>& out, int ln);
  void init_agg(Expr* base, int off, Type* ty, std::vector<Stmt*>& out, int ln);
  Expr* elem_lv(Expr* base, int off, Type* t, int ln);
  Type* infer_len(Type* ty);
  int loops = 0, switches = 0;
  Sym* alloc_local(const std::string& name, Type* ty);
};

// -------------------------------------------------------------------------------------------------------------------
Type* Parser::parse_base_type(Spec& sp) {
  bool uns = false, sgn = false, any = false; int longs = 0, shorts = 0; Type* base = nullptr; bool ch = false, in = false, vd = false;
  while (cur().t == T_ID) {
    const std::string s = cur().s;
    if (s == "static") sp.is_static = true; else if (s == "extern") sp.is_extern = true; else if (s == "typedef") sp.is_typedef = true;
    else if (s == "const" || s == "volatile" || s == "register" || s == "auto" || s == "inline") {}
    else if (s == "__interrupt") sp.interrupt = true;
    else if (s == "unsigned") { uns = true; any = true; } else if (s == "signed") { sgn = true; any = true; }
    else if (s == "long") { longs++; any = true; } else if (s == "short") { shorts++; any = true; }
    else if (s == "char") { ch = true; any = true; } else if (s == "int") { in = true; any = true; } else if (s == "void") { vd = true; any = true; }
    else if (s == "struct" || s == "union") { p++; base = struct_spec(); any = true; continue; }
    else if (s == "enum") { p++; enum_spec(); base = t_int; any = true; continue; }
    else { Sym* sy = lookup(s); if (sy && sy->kind == S_TYPEDEF && !any && !base) { base = sy->ty; any = true; } else break; }
    p++;
  }
  if (!any) err("esperava um tipo");
  (void)sgn;
  if (base) return base;
  if (longs > 1) err("'long long' não é suportado");
  if (vd) return t_void;
  if (ch) return uns ? t_uchar : t_char;
  if (shorts) return uns ? t_ushort : t_short;
  (void)in; return uns ? t_uint : t_int;
}

Type* Parser::struct_spec() {
  bool is_union = T[p - 1].s == "union"; std::string tag;
  if (cur().t == T_ID && !is_kw(cur().s)) { tag = cur().s; p++; }
  StructDef* sd = nullptr;
  if (!tag.empty()) { auto it = tags.find(tag); if (it != tags.end()) sd = it->second; }
  if (!sd) { P.structs.emplace_back(new StructDef()); sd = P.structs.back().get(); sd->tag = tag; sd->is_union = is_union; if (!tag.empty()) tags[tag] = sd; }
  Type* t = mkt(TY_STRUCT); t->sd = sd;
  if (accept_op("{")) {
    if (sd->complete) err("estrutura '" + tag + "' redefinida");
    int off = 0, maxal = 1, maxsz = 0;
    while (!is_op("}")) {
      Spec sp2; Type* mb = parse_base_type(sp2);
      do {
        std::string name; Type* mt = parse_declarator(mb, name);
        if (name.empty()) err("membro sem nome"); if (mt->k == TY_VOID || mt->k == TY_FUNC) err("membro com tipo inválido");
        if (mt->k == TY_STRUCT && !mt->sd->complete) err("membro com estrutura incompleta");
        int al = talign(mt), sz = tsize(mt); maxal = std::max(maxal, al);
        int mo = is_union ? 0 : (off + al - 1) / al * al; sd->mem.push_back({name, mt, mo});
        if (!is_union) off = mo + sz; maxsz = std::max(maxsz, sz);
      } while (accept_op(","));
      expect_op(";");
    }
    expect_op("}");
    sd->align = maxal; sd->size = ((is_union ? maxsz : off) + maxal - 1) / maxal * maxal; if (sd->size == 0) sd->size = maxal; sd->complete = true;
  }
  return t;
}

void Parser::enum_spec() {
  if (cur().t == T_ID && !is_kw(cur().s)) p++;                      // etiqueta ignorada: enum é int
  if (!accept_op("{")) return;
  i64 v = 0;
  while (!is_op("}")) {
    if (cur().t != T_ID) err("nome de enumerador esperado");
    std::string n = cur().s; p++;
    if (accept_op("=")) v = const_int(conditional());
    Sym* s = mks(n, S_ENUM); s->enumv = v; s->ty = t_int; scopes.back()[n] = s; v++;
    if (!accept_op(",")) break;
  }
  expect_op("}");
}

void Parser::parse_params(std::vector<Type*>& pt, std::vector<std::string>& pn, bool& proto) {
  proto = true; last_variadic = false;
  if (accept_op(")")) { proto = false; return; }
  if (is_id("void") && T[p + 1].t == T_OP && T[p + 1].s == ")") { p += 2; return; }
  while (true) {
    if (is_op("...")) { if (pt.empty()) err("'...' precisa de pelo menos um parâmetro nomeado"); p++; expect_op(")"); last_variadic = true; break; }
    Spec sp; Type* b = parse_base_type(sp); std::string name; Type* t = parse_declarator(b, name, true);
    if (t->k == TY_ARRAY) t = ptr_to(t->base); else if (t->k == TY_FUNC) t = ptr_to(t);
    if (t->k == TY_STRUCT) err("estruturas por valor não são suportadas como parâmetro");
    pt.push_back(t); pn.push_back(name);
    if (accept_op(",")) continue;
    expect_op(")"); break;
  }
}

// declarador: '*'* ( nome | '(' '*' nome ')' '(' params ')' ) ( '[' n ']' )* | ( '(' params ')' )
Type* Parser::parse_declarator(Type* base, std::string& name, bool abstract_ok) {
  while (accept_op("*")) { base = ptr_to(base); while (is_id("const") || is_id("volatile")) p++; }
  if (is_op("(") && T[p + 1].t == T_OP && T[p + 1].s == "*") {      // ponteiro para função (ou array de): (*nome)(params)
    p += 2; int stars = 1; while (accept_op("*")) stars++;
    std::string n2; if (cur().t == T_ID && !is_kw(cur().s)) { n2 = cur().s; p++; }
    int arr = -1; if (accept_op("[")) { arr = (int)const_int(conditional()); expect_op("]"); }
    expect_op(")"); expect_op("(");
    Type* f = mkt(TY_FUNC); f->base = base; bool proto; std::vector<std::string> pn; parse_params(f->ptypes, pn, proto); f->has_proto = proto; f->variadic = last_variadic;
    Type* r = f; for (int i = 0; i < stars; i++) r = ptr_to(r);
    if (arr >= 0) r = array_of(r, arr);
    name = n2; return r;
  }
  if (cur().t == T_ID && !is_kw(cur().s) && !(lookup(cur().s) && lookup(cur().s)->kind == S_TYPEDEF && abstract_ok && false)) { name = cur().s; p++; } else if (!abstract_ok && !(is_op("[") || is_op("("))) err("nome esperado na declaração");
  std::vector<int> dims;
  while (is_op("[")) { p++; if (is_op("]")) { dims.push_back(0); p++; continue; } dims.push_back((int)const_int(conditional())); expect_op("]"); }
  Type* r = base;
  for (size_t i = dims.size(); i-- > 0;) r = array_of(r, dims[i]);
  if (is_op("(") && dims.empty()) {
    p++; Type* f = mkt(TY_FUNC); f->base = base; bool proto; last_pnames.clear(); parse_params(f->ptypes, last_pnames, proto); f->has_proto = proto; f->variadic = last_variadic; r = f;
  }
  return r;
}

Type* Parser::parse_type_name() {
  Spec sp; Type* b = parse_base_type(sp); std::string n; return parse_declarator(b, n, true);
}

// -------------------------------------------------------------------------------------------------------------------
Expr* Parser::decay(Expr* e) {
  if (e->ty->k == TY_ARRAY) { Expr* r = mke(E_ADDR, e->line); r->a = e; r->ty = ptr_to(e->ty->base); return r; }
  if (e->ty->k == TY_FUNC) { Expr* r = mke(E_ADDR, e->line); r->a = e; r->ty = ptr_to(e->ty); return r; }
  return e;
}

Expr* Parser::make_unary_deref(Expr* a, int ln) {
  a = decay(a); if (a->ty->k != TY_PTR) fail(T[p].file, ln, "'*' aplicado a um valor que não é ponteiro");
  if (a->ty->base->k == TY_VOID) fail(T[p].file, ln, "desreferência de 'void *'");
  Expr* r = mke(E_DEREF, ln); r->a = a; r->ty = a->ty->base; r->lval = r->ty->k != TY_FUNC; return r;
}

Expr* Parser::make_binary(const std::string& op, Expr* a, Expr* b, int ln) {
  a = decay(a); b = decay(b);
  auto bad = [&](const char* m) { fail(T[p].file, ln, std::string("operandos inválidos para '") + op + "': " + m); };
  if (!is_scalar(a->ty) || !is_scalar(b->ty)) bad("esperava inteiros ou ponteiros");
  bool pa = is_ptr(a->ty), pb = is_ptr(b->ty);
  Expr* r = mke(E_BIN, ln); r->op = op;
  if (op == "+" || op == "-") {
    if (pa && pb) {
      if (op == "+") bad("soma de dois ponteiros");
      int es = tsize(a->ty->base); if (es != tsize(b->ty->base)) bad("ponteiros de tipos diferentes");
      r->op = "ptrdiff"; r->a = a; r->b = b; r->num = es; r->ty = t_int; return r;
    }
    if (pa || pb) {
      if (pb && op == "-") bad("inteiro menos ponteiro");
      Expr* ptr = pa ? a : b; Expr* idx = pa ? b : a; int es = tsize(ptr->ty->base);
      if (ptr->ty->base->k == TY_VOID) es = 1;
      if (es != 1) {
        if (idx->k == E_NUM) idx = num(wrap(idx->num * es, t_int), t_int, ln);
        else { Expr* m = mke(E_BIN, ln); m->op = "*"; m->a = idx; m->b = num(es, t_int, ln); m->ty = t_int; idx = m; }
      }
      if (ptr->k == E_NUM && idx->k == E_NUM) return num(wrap(op == "+" ? ptr->num + idx->num : ptr->num - idx->num, ptr->ty), ptr->ty, ln);
      r->a = ptr; r->b = idx; r->ty = ptr->ty; return r;
    }
  }
  bool rel = op == "<" || op == ">" || op == "<=" || op == ">=" || op == "==" || op == "!=";
  if (op == "&&" || op == "||") { r->a = a; r->b = b; r->ty = t_int; if (a->k == E_NUM && b->k == E_NUM) return num(op == "&&" ? (a->num && b->num) : (a->num || b->num), t_int, ln); return r; }
  if (rel) {
    bool u = pa || pb || arith(a->ty, b->ty)->uns; r->uns = u; r->a = a; r->b = b; r->ty = t_int;
    if (a->k == E_NUM && b->k == E_NUM && !pa && !pb) {
      i64 x = a->num, y = b->num; if (u) { x = (u32)x; y = (u32)y; }
      bool v = op == "<" ? x < y : op == ">" ? x > y : op == "<=" ? x <= y : op == ">=" ? x >= y : op == "==" ? x == y : x != y; return num(v, t_int, ln);
    }
    return r;
  }
  if (pa || pb) bad("aritmética com ponteiro não permitida");
  bool shift = op == "<<" || op == ">>"; Type* rt = shift ? promoted(a->ty) : arith(a->ty, b->ty);
  r->uns = rt->uns; r->a = a; r->b = b; r->ty = rt;
  if (a->k == E_NUM && b->k == E_NUM) {
    i64 x = a->num, y = b->num, v = 0; bool u = rt->uns; if (u) { x = (u32)x; y = (u32)y; }
    if (op == "+") v = x + y; else if (op == "-") v = x - y; else if (op == "*") v = x * y;
    else if (op == "/" || op == "%") { if (y == 0) fail(T[p].file, ln, "divisão por zero em constante"); v = op == "/" ? (u ? (i64)((u32)x / (u32)y) : x / y) : (u ? (i64)((u32)x % (u32)y) : x % y); }
    else if (op == "&") v = x & y; else if (op == "|") v = x | y; else if (op == "^") v = x ^ y;
    else if (op == "<<") v = (u32)x << (y & 31); else if (op == ">>") v = u ? (i64)((u32)x >> (y & 31)) : (i64)((i32)x >> (y & 31));
    return num(wrap(v, rt), rt, ln);
  }
  return r;
}

Expr* Parser::make_assign(const std::string& op, Expr* a, Expr* b, int ln) {
  if (!a->lval) fail(T[p].file, ln, "o lado esquerdo da atribuição não é um lvalue");
  if (a->ty->k == TY_ARRAY || a->ty->k == TY_FUNC) fail(T[p].file, ln, "não é possível atribuir a array/função");
  b = decay(b);
  Expr* r = mke(E_ASSIGN, ln); r->op = op; r->a = a; r->ty = a->ty;
  if (a->ty->k == TY_STRUCT) {
    if (op != "=" || b->ty->k != TY_STRUCT || b->ty->sd != a->ty->sd) fail(T[p].file, ln, "atribuição de estruturas incompatíveis");
    r->b = b; return r;
  }
  if (!is_scalar(b->ty)) fail(T[p].file, ln, "valor atribuído inválido");
  if (op == "+=" || op == "-=") {
    if (is_ptr(a->ty)) { int es = tsize(a->ty->base); if (a->ty->base->k == TY_VOID) es = 1; if (es != 1) { if (b->k == E_NUM) b = num(b->num * es, t_int, ln); else { Expr* m = mke(E_BIN, ln); m->op = "*"; m->a = b; m->b = num(es, t_int, ln); m->ty = t_int; b = m; } } }
  }
  r->uns = a->ty->uns || b->ty->uns; r->b = b; return r;
}

Expr* Parser::expression() {
  Expr* e = assignment();
  while (is_op(",")) { int ln = line(); p++; Expr* r = assignment(); Expr* c = mke(E_COMMA, ln); c->a = e; c->b = decay(r); c->ty = c->b->ty; e = c; }
  return e;
}

Expr* Parser::assignment() {
  Expr* lhs = conditional();
  static const char* aops[] = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="};
  for (auto op : aops) if (is_op(op)) { int ln = line(); p++; Expr* rhs = assignment(); return make_assign(op, lhs, rhs, ln); }
  return lhs;
}

Expr* Parser::conditional() {
  Expr* c = binary_rhs(0, cast_expr());
  if (!is_op("?")) return c;
  int ln = line(); p++; Expr* a = decay(expression()); expect_op(":"); Expr* b = decay(conditional()); c = decay(c);
  if (!is_scalar(c->ty)) fail(T[p].file, ln, "condição inválida em '?:'");
  Type* rt;
  if (is_ptr(a->ty)) rt = a->ty; else if (is_ptr(b->ty)) rt = b->ty; else if (is_int(a->ty) && is_int(b->ty)) rt = arith(a->ty, b->ty);
  else fail(T[p].file, ln, "tipos incompatíveis em '?:'");
  if (c->k == E_NUM) return c->num ? a : b;
  Expr* r = mke(E_COND, ln); r->a = c; r->b = a; r->c = b; r->ty = rt; return r;
}

static int prec_of(const std::string& op) {
  if (op == "||") return 1; if (op == "&&") return 2; if (op == "|") return 3; if (op == "^") return 4; if (op == "&") return 5;
  if (op == "==" || op == "!=") return 6; if (op == "<" || op == ">" || op == "<=" || op == ">=") return 7; if (op == "<<" || op == ">>") return 8;
  if (op == "+" || op == "-") return 9; if (op == "*" || op == "/" || op == "%") return 10; return -1;
}
Expr* Parser::binary_rhs(int minprec, Expr* lhs) {
  while (cur().t == T_OP) {
    int pr = prec_of(cur().s); if (pr < minprec || pr < 0) break;
    std::string op = cur().s; int ln = line(); p++;
    Expr* rhs = cast_expr();
    while (cur().t == T_OP) { int pr2 = prec_of(cur().s); if (pr2 <= pr) break; rhs = binary_rhs(pr + 1, rhs); }
    lhs = make_binary(op, lhs, rhs, ln);
  }
  return lhs;
}

Expr* Parser::cast_expr() {
  if (is_op("(") && T[p + 1].t == T_ID) {
    size_t save = p; p++;
    if (is_type_start()) {
      int ln = line(); Type* t = parse_type_name(); expect_op(")");
      if (is_op("{")) err("literais compostos não são suportados");
      Expr* a = decay(cast_expr());
      if (t->k == TY_STRUCT || t->k == TY_ARRAY || t->k == TY_FUNC) fail(T[p].file, ln, "conversão para tipo inválido");
      if (t->k != TY_VOID && !is_scalar(a->ty)) fail(T[p].file, ln, "conversão de valor inválido");
      if (a->k == E_NUM && t->k != TY_VOID) return num(wrap(a->num, t), t, ln);
      Expr* r = mke(E_CAST, ln); r->a = a; r->ty = t; return r;
    }
    p = save;
  }
  return unary();
}

Expr* Parser::unary() {
  int ln = line();
  if (cur().t == T_OP) {
    const std::string op = cur().s;
    if (op == "-" || op == "+" || op == "~" || op == "!") {
      p++; Expr* a = decay(cast_expr()); if (!is_scalar(a->ty)) fail(T[p].file, ln, "operando inválido para '" + op + "'");
      if (op == "!") { if (a->k == E_NUM) return num(a->num == 0, t_int, ln); Expr* r = mke(E_UN, ln); r->op = op; r->a = a; r->ty = t_int; return r; }
      if (is_ptr(a->ty)) fail(T[p].file, ln, "operando inválido para '" + op + "'");
      Type* t = promoted(a->ty);
      if (op == "+") return a;
      if (a->k == E_NUM) return num(wrap(op == "-" ? -a->num : ~a->num, t), t, ln);
      Expr* r = mke(E_UN, ln); r->op = op; r->a = a; r->ty = t; r->uns = t->uns; return r;
    }
    if (op == "*") { p++; Expr* a = cast_expr(); return make_unary_deref(a, ln); }
    if (op == "&") {
      p++; Expr* a = cast_expr();
      if (a->k == E_DEREF) return a->a;                              // &*p == p
      if (a->ty->k == TY_FUNC) return decay(a);
      if (!a->lval) fail(T[p].file, ln, "'&' exige um lvalue");
      Expr* r = mke(E_ADDR, ln); r->a = a; r->ty = ptr_to(a->ty); return r;
    }
    if (op == "++" || op == "--") {
      p++; Expr* a = unary(); if (!a->lval || !is_scalar(a->ty)) fail(T[p].file, ln, "operando inválido para '" + op + "'");
      Expr* r = mke(E_INCDEC, ln); r->op = op; r->a = a; r->ty = a->ty; r->post = false; r->num = is_ptr(a->ty) ? tsize(a->ty->base) : 1; return r;
    }
  }
  if (is_id("sizeof")) {
    p++;
    if (is_op("(") && T[p + 1].t == T_ID) { size_t save = p; p++; if (is_type_start()) { Type* t = parse_type_name(); expect_op(")"); return num(tsize(t), t_uint, ln); } p = save; }
    Expr* a = unary(); return num(tsize(a->ty), t_uint, ln);
  }
  return postfix();
}

Expr* Parser::parse_call(Expr* callee, int ln) {
  Expr* r = mke(E_CALL, ln); r->a = callee;
  Type* ft = nullptr; Sym* bs = nullptr;
  if (callee->k == E_VAR && callee->sym && callee->sym->kind == S_BUILTIN) { bs = callee->sym; ft = callee->sym->ty; }
  else if (callee->ty->k == TY_FUNC) ft = callee->ty;
  else { Expr* d = decay(callee); if (d->ty->k == TY_PTR && d->ty->base->k == TY_FUNC) { ft = d->ty->base; r->a = d; } else fail(T[p].file, ln, "chamada de algo que não é função"); }
  if (!is_op(")")) { do { Expr* a = decay(assignment()); if (!is_scalar(a->ty)) fail(T[p].file, ln, "argumento inválido (estruturas por valor não são suportadas)"); r->args.push_back(a); } while (accept_op(",")); }
  expect_op(")");
  if (!bs && ft->has_proto && (ft->variadic ? r->args.size() < ft->ptypes.size() : ft->ptypes.size() != r->args.size())) fail(T[p].file, ln, "número de argumentos incorreto na chamada (esperava " + std::string(ft->variadic ? "pelo menos " : "") + std::to_string(ft->ptypes.size()) + ", recebeu " + std::to_string(r->args.size()) + ")");
  r->ty = ft->base; return r;
}

Expr* Parser::postfix() {
  Expr* e = primary();
  while (true) {
    int ln = line();
    if (is_op("(")) { p++; e = parse_call(e, ln); continue; }
    if (is_op("[")) { p++; Expr* i = expression(); expect_op("]"); e = make_index(e, i, ln); continue; }
    if (is_op(".") || is_op("->")) {
      bool arrow = is_op("->"); p++;
      if (cur().t != T_ID) err("nome de membro esperado"); std::string mn = cur().s; p++;
      if (arrow) { e = make_unary_deref(e, ln); }
      if (e->ty->k != TY_STRUCT) fail(T[p].file, ln, "acesso a membro de algo que não é estrutura");
      StructDef* sd = e->ty->sd; const Member* m = nullptr; for (auto& x : sd->mem) if (x.name == mn) m = &x;
      if (!m) fail(T[p].file, ln, "a estrutura não tem o membro '" + mn + "'");
      Expr* r = mke(E_MEMBER, ln); r->a = e; r->num = m->off; r->ty = m->ty; r->lval = true; r->str = mn; e = r; continue;
    }
    if (is_op("++") || is_op("--")) {
      if (!e->lval || !is_scalar(e->ty)) fail(T[p].file, ln, "operando inválido para '" + cur().s + "'");
      Expr* r = mke(E_INCDEC, ln); r->op = cur().s; p++; r->a = e; r->ty = e->ty; r->post = true; r->num = is_ptr(e->ty) ? tsize(e->ty->base) : 1; e = r; continue;
    }
    break;
  }
  return e;
}

Expr* Parser::primary() {
  const Token& t = cur(); int ln = t.line;
  if (t.t == T_NUM) { p++; return num(t.v, t.uns ? t_uint : t_int, ln); }
  if (t.t == T_CHAR) { p++; return num(t.v, t_int, ln); }
  if (t.t == T_STR) {
    std::string s; while (cur().t == T_STR) { s += cur().s; p++; }
    Expr* e = mke(E_STR, ln); e->str = s; e->ty = array_of(t_char, (int)s.size() + 1); e->lval = true; e->num = (i64)P.strings.size(); P.strings.push_back(s); return e;
  }
  if (t.t == T_OP && t.s == "(") { p++; Expr* e = expression(); expect_op(")"); return e; }
  if (t.t == T_ID) {
    if (is_kw(t.s)) err("palavra-chave '" + t.s + "' inesperada em expressão");
    Sym* s = lookup(t.s); if (!s) err("identificador '" + t.s + "' não declarado");
    p++;
    if (s->kind == S_ENUM) return num(s->enumv, t_int, ln);
    if (s->kind == S_TYPEDEF) err("'" + t.s + "' é um tipo, não uma variável");
    Expr* e = mke(E_VAR, ln); e->sym = s; e->ty = s->ty; e->lval = s->kind == S_LOCAL || s->kind == S_PARAM || s->kind == S_GLOBAL; return e;
  }
  err("expressão esperada");
}

// avalia expressão constante (inteira ou endereço+deslocamento)
bool Parser::const_eval(Expr* e, i64& v, std::string& ref) {
  switch (e->k) {
    case E_NUM: v = e->num; ref.clear(); return true;
    case E_STR: v = 0; ref = ".LS" + std::to_string(e->num); return true;
    case E_CAST: return const_eval(e->a, v, ref);
    case E_ADDR: {
      Expr* a = e->a;
      if (a->k == E_VAR && (a->sym->kind == S_GLOBAL || a->sym->kind == S_FUNC)) { v = 0; ref = a->sym->label.empty() ? a->sym->name : a->sym->label; return true; }
      if (a->k == E_STR) return const_eval(a, v, ref);
      if (a->k == E_DEREF) return const_eval(a->a, v, ref);
      if (a->k == E_MEMBER) { i64 b; std::string r2; Expr tmp = *a; Expr ad; ad.k = E_ADDR; ad.a = a->a; ad.ty = ptr_to(a->a->ty); if (!const_eval(&ad, b, r2)) return false; v = b + a->num; ref = r2; return true; }
      return false;
    }
    case E_BIN: {
      if (e->op != "+" && e->op != "-") return false;
      i64 x, y; std::string rx, ry; if (!const_eval(e->a, x, rx) || !const_eval(e->b, y, ry)) return false;
      if (!ry.empty()) return false; if (e->op == "-" && !rx.empty()) { v = x - y; ref = rx; return true; }
      v = e->op == "+" ? x + y : x - y; ref = rx; return true;
    }
    default: return false;
  }
}
i64 Parser::const_int(Expr* e) { i64 v; std::string r; if (!const_eval(e, v, r) || !r.empty()) fail(T[p].file, e->line, "esperava uma expressão constante inteira"); return v; }

// -------------------------------------------------------------------------------------------------------------------
Sym* Parser::alloc_local(const std::string& name, Type* ty) {
  if (ty->k == TY_VOID) err("variável 'void'");
  if (ty->k == TY_STRUCT && !ty->sd->complete) err("estrutura incompleta");
  Sym* s = mks(name, S_LOCAL); s->ty = ty; int sz = (tsize(ty) + 3) / 4 * 4; fn->frame += sz; s->off = -fn->frame;
  if (fn->frame > 30000) err("quadro de pilha grande demais (use globais para arrays grandes)");
  return s;
}

Expr* Parser::elem_lv(Expr* base, int off, Type* t, int ln) {
  Expr* add = make_binary("+", base, num(off, t_int, ln), ln);       // base: char*
  Expr* c = mke(E_CAST, ln); c->a = add; c->ty = ptr_to(t); return make_unary_deref(c, ln);
}

// '{...}' ou string para arrays/estruturas locais: atribuições elemento a elemento
void Parser::init_agg(Expr* base, int off, Type* ty, std::vector<Stmt*>& out, int ln) {
  auto assign = [&](Expr* lv, Expr* val) { Stmt* s = mkst(ST_EXPR, ln); s->e = make_assign("=", lv, val, ln); out.push_back(s); };
  if (ty->k == TY_ARRAY) {
    int esz = tsize(ty->base);
    if (ty->base->k == TY_CHAR && cur().t == T_STR) {
      std::string s; while (cur().t == T_STR) { s += cur().s; p++; }
      if ((int)s.size() > ty->len) err("string maior que o array");
      for (int i = 0; i < ty->len; i++) assign(elem_lv(base, off + i, t_char, ln), num(i < (int)s.size() ? (u8)s[i] : 0, t_int, ln));
      return;
    }
    expect_op("{"); int i = 0;
    while (!is_op("}")) {
      if (i >= ty->len) err("inicializadores demais");
      if (ty->base->k == TY_ARRAY || ty->base->k == TY_STRUCT) init_agg(base, off + i * esz, ty->base, out, ln);
      else assign(elem_lv(base, off + i * esz, ty->base, ln), assignment());
      i++; if (!accept_op(",")) break;
    }
    expect_op("}");
    if (ty->len - i > 64 && ty->base->k != TY_ARRAY && ty->base->k != TY_STRUCT) err("inicializador local incompleto grande demais: use uma variável global");
    for (; i < ty->len; i++) if (ty->base->k != TY_ARRAY && ty->base->k != TY_STRUCT) assign(elem_lv(base, off + i * esz, ty->base, ln), num(0, t_int, ln));
    return;
  }
  if (ty->k == TY_STRUCT) {
    expect_op("{"); size_t mi = 0;
    while (!is_op("}")) {
      if (mi >= ty->sd->mem.size()) err("inicializadores demais");
      const Member& m = ty->sd->mem[mi++];
      if (m.ty->k == TY_ARRAY || m.ty->k == TY_STRUCT) init_agg(base, off + m.off, m.ty, out, ln); else assign(elem_lv(base, off + m.off, m.ty, ln), assignment());
      if (ty->sd->is_union) { accept_op(","); break; }
      if (!accept_op(",")) break;
    }
    expect_op("}"); return;
  }
  err("inicializador com chaves em tipo escalar");
}

Type* Parser::infer_len(Type* ty) {
  if (ty->k != TY_ARRAY || ty->len != 0 || !is_op("=")) return ty;
  size_t save = p; p++; int n = 0;
  if (cur().t == T_STR && ty->base->k == TY_CHAR) { size_t len = 0; while (cur().t == T_STR) { len += cur().s.size(); p++; } n = (int)len + 1; }
  else if (is_op("{")) {
    int depth = 0, commas = 0; bool any = false, last_comma = false;
    while (true) {
      const Token& t = cur(); if (t.t == T_EOF) err("inicializador não terminado");
      if (t.t == T_OP && (t.s == "{" || t.s == "(" || t.s == "[")) { depth++; if (depth > 1 || t.s != "{") any = true; last_comma = false; }
      else if (t.t == T_OP && (t.s == "}" || t.s == ")" || t.s == "]")) { depth--; if (depth == 0) break; last_comma = false; }
      else if (t.t == T_OP && t.s == "," && depth == 1) { commas++; last_comma = true; }
      else { any = true; last_comma = false; }
      p++;
    }
    n = any ? commas + (last_comma ? 0 : 1) : 0;
  } else err("array sem tamanho precisa de inicializador");
  p = save; return array_of(ty->base, n);
}

void Parser::local_init(Sym* s, std::vector<Stmt*>& out, int ln) {
  Expr* var = mke(E_VAR, ln); var->sym = s; var->ty = s->ty; var->lval = true;
  if (s->ty->k == TY_ARRAY || s->ty->k == TY_STRUCT) {
    if (s->ty->k == TY_STRUCT && !is_op("{")) { Stmt* st = mkst(ST_EXPR, ln); st->e = make_assign("=", var, assignment(), ln); out.push_back(st); return; }
    Expr* ad = mke(E_ADDR, ln); ad->a = var; ad->ty = ptr_to(s->ty); Expr* base = mke(E_CAST, ln); base->a = ad; base->ty = ptr_to(t_char);
    init_agg(base, 0, s->ty, out, ln); return;
  }
  Stmt* st = mkst(ST_EXPR, ln); st->e = make_assign("=", var, assignment(), ln); out.push_back(st);
}

Sym* Parser::declare_func(const std::string& name, Type* fty, int ln) {
  { auto it = scopes[0].find(name); if (it != scopes[0].end() && it->second->kind == S_BUILTIN) return it->second; }   // protótipo de intrínseco: ignorado
  Sym* s = P.find_global(name);
  if (s && s->kind != S_FUNC) fail(T[p].file, ln, "'" + name + "' redeclarado com outro tipo de símbolo");
  if (!s) { s = mks(name, S_FUNC); s->label = name; P.gtab[name] = s; scopes[0][name] = s; }
  if (s->ty && s->ty->has_proto && fty->has_proto && s->ty->ptypes.size() != fty->ptypes.size()) fail(T[p].file, ln, "declaração conflitante de '" + name + "'");
  if (!s->ty || fty->has_proto) s->ty = fty;
  return s;
}

Sym* Parser::declare_global(const std::string& name, Type* ty, const Spec& sp, int ln, bool allow_init) {
  if (ty->k == TY_VOID) fail(T[p].file, ln, "variável 'void'");
  if (is_op("=") ) ty = infer_len(ty);
  if (ty->k == TY_STRUCT && !ty->sd->complete && !sp.is_extern) fail(T[p].file, ln, "estrutura incompleta");
  Sym* s = P.find_global(name);
  if (s && s->kind != S_GLOBAL) fail(T[p].file, ln, "'" + name + "' redeclarado com outro tipo de símbolo");
  if (!s) { s = mks(name, S_GLOBAL); s->label = name; P.gtab[name] = s; scopes[0][name] = s; P.globals.push_back(s); s->ty = ty; s->is_static = sp.is_static; s->is_extern = sp.is_extern; }
  else if (tsize(s->ty) != tsize(ty) && ty->k != TY_ARRAY) fail(T[p].file, ln, "'" + name + "' redeclarado com outro tipo");
  else if (ty->k == TY_ARRAY && ty->len && !s->ty->len) s->ty = ty;
  if (!sp.is_extern) s->is_extern = false;
  if (accept_op("=")) {
    if (!allow_init) err("inicializador não permitido aqui");
    if (s->has_init) fail(T[p].file, ln, "'" + name + "' inicializada duas vezes");
    s->has_init = true; parse_ginit(s->ty, s->init, 0);
  }
  return s;
}

void Parser::parse_ginit(Type* ty, std::vector<GInit>& out, int off) {
  if (ty->k == TY_ARRAY) {
    int esz = tsize(ty->base);
    if (ty->base->k == TY_CHAR && cur().t == T_STR) {
      std::string s; while (cur().t == T_STR) { s += cur().s; p++; }
      if ((int)s.size() > ty->len) err("string maior que o array");
      for (size_t i = 0; i < s.size(); i++) out.push_back({off + (int)i, 1, (i64)(u8)s[i], ""});
      return;
    }
    expect_op("{"); int i = 0;
    while (!is_op("}")) {
      if (i >= ty->len) err("inicializadores demais");
      parse_ginit(ty->base, out, off + i * esz); i++; if (!accept_op(",")) break;
    }
    expect_op("}"); return;
  }
  if (ty->k == TY_STRUCT) {
    expect_op("{"); size_t mi = 0;
    while (!is_op("}")) {
      if (mi >= ty->sd->mem.size()) err("inicializadores demais");
      const Member& m = ty->sd->mem[mi++]; parse_ginit(m.ty, out, off + m.off);
      if (ty->sd->is_union) { accept_op(","); break; }
      if (!accept_op(",")) break;
    }
    expect_op("}"); return;
  }
  if (is_op("{")) { p++; parse_ginit(ty, out, off); accept_op(","); expect_op("}"); return; }   // escalar entre chaves
  int ln = line(); Expr* e = decay(assignment()); i64 v; std::string ref;
  if (!const_eval(e, v, ref)) fail(T[p].file, ln, "o inicializador de uma variável global precisa ser constante");
  int sz = tsize(ty); if (!ref.empty() && sz != 4) fail(T[p].file, ln, "endereço em objeto que não tem 32 bits");
  out.push_back({off, sz, v, ref});
}

void Parser::local_decl(std::vector<Stmt*>& out) {
  int ln = line(); Spec sp; Type* base = parse_base_type(sp);
  if (accept_op(";")) return;
  do {
    std::string name; Type* ty = parse_declarator(base, name);
    if (sp.is_typedef) { Sym* s = mks(name, S_TYPEDEF); s->ty = ty; scopes.back()[name] = s; continue; }
    if (ty->k == TY_FUNC) { declare_func(name, ty, ln); continue; }
    if (sp.is_extern) { Spec s2 = sp; Sym* g = declare_global(name, ty, s2, ln, false); scopes.back()[name] = g; continue; }
    if (sp.is_static) {
      std::string label = fn->name + "." + name + "." + std::to_string(static_counter++);
      if (is_op("=")) ty = infer_len(ty);
      Sym* g = mks(name, S_GLOBAL); g->label = label; g->ty = ty; g->is_static = true; P.globals.push_back(g); scopes.back()[name] = g;
      if (accept_op("=")) { g->has_init = true; parse_ginit(ty, g->init, 0); }
      continue;
    }
    ty = infer_len(ty);
    Sym* s = alloc_local(name, ty); scopes.back()[name] = s;
    if (accept_op("=")) local_init(s, out, ln);
  } while (accept_op(","));
  expect_op(";");
}

Stmt* Parser::block(bool new_scope) {
  Stmt* b = mkst(ST_BLOCK, line()); expect_op("{"); if (new_scope) scopes.emplace_back();
  while (!is_op("}")) {
    if (cur().t == T_EOF) err("bloco não terminado");
    if (is_type_start()) { std::vector<Stmt*> v; local_decl(v); for (auto s : v) b->list.push_back(s); } else b->list.push_back(statement());
  }
  p++; if (new_scope) scopes.pop_back(); return b;
}

Stmt* Parser::statement() {
  int ln = line();
  if (is_op("{")) return block();
  if (accept_op(";")) return mkst(ST_NOP, ln);
  auto cond = [&]() { expect_op("("); Expr* e = decay(expression()); expect_op(")"); if (!is_scalar(e->ty)) fail(T[p].file, ln, "condição inválida"); return e; };
  if (accept_id("if")) { Stmt* s = mkst(ST_IF, ln); s->e = cond(); s->body = statement(); if (accept_id("else")) s->els = statement(); return s; }
  if (accept_id("while")) { Stmt* s = mkst(ST_WHILE, ln); s->e = cond(); loops++; s->body = statement(); loops--; return s; }
  if (accept_id("do")) {
    Stmt* s = mkst(ST_DO, ln); loops++; s->body = statement(); loops--;
    if (!accept_id("while")) err("esperava 'while' após o corpo do 'do'"); s->e = cond(); expect_op(";"); return s;
  }
  if (accept_id("for")) {
    Stmt* s = mkst(ST_FOR, ln); expect_op("("); scopes.emplace_back();
    if (is_type_start()) local_decl(s->list); else if (!accept_op(";")) { s->e = expression(); expect_op(";"); }
    if (!is_op(";")) { s->e2 = decay(expression()); } expect_op(";");
    if (!is_op(")")) s->e3 = expression(); expect_op(")");
    loops++; s->body = statement(); loops--; scopes.pop_back(); return s;
  }
  if (accept_id("return")) {
    Stmt* s = mkst(ST_RETURN, ln);
    if (!is_op(";")) { s->e = decay(expression()); if (fn->ret->k == TY_VOID) fail(T[p].file, ln, "'return' com valor em função 'void'"); if (!is_scalar(s->e->ty)) fail(T[p].file, ln, "valor de retorno inválido"); }
    expect_op(";"); return s;
  }
  if (accept_id("break")) { if (!loops && !switches) err("'break' fora de laço/switch"); expect_op(";"); return mkst(ST_BREAK, ln); }
  if (accept_id("continue")) { if (!loops) err("'continue' fora de laço"); expect_op(";"); return mkst(ST_CONTINUE, ln); }
  if (accept_id("switch")) {
    Stmt* s = mkst(ST_SWITCH, ln); s->e = cond(); sw_stack.push_back(s); switches++; s->body = statement(); switches--; sw_stack.pop_back(); return s;
  }
  if (accept_id("case")) {
    if (sw_stack.empty()) err("'case' fora de switch");
    Stmt* s = mkst(ST_CASE, ln); s->val = const_int(conditional()); expect_op(":");
    s->label = new_label(); for (auto& c : sw_stack.back()->cases) if (c.first == s->val) err("valor de 'case' duplicado");
    sw_stack.back()->cases.push_back({s->val, s->label}); s->body = statement(); return s;
  }
  if (accept_id("default")) {
    if (sw_stack.empty()) err("'default' fora de switch"); if (sw_stack.back()->default_label >= 0) err("'default' duplicado");
    Stmt* s = mkst(ST_DEFAULT, ln); expect_op(":"); s->label = new_label(); sw_stack.back()->default_label = s->label; s->body = statement(); return s;
  }
  if (accept_id("goto")) {
    if (cur().t != T_ID) err("esperava o nome do rótulo após 'goto'");
    std::string n = cur().s; p++; expect_op(";");
    auto it = lbls.find(n); if (it == lbls.end()) it = lbls.emplace(n, std::make_pair(new_label(), 0)).first;
    if (!it->second.second) lbl_use_line[n] = ln;
    Stmt* s = mkst(ST_GOTO, ln); s->label = it->second.first; return s;
  }
  if (cur().t == T_ID && !is_kw(cur().s) && T[p + 1].t == T_OP && T[p + 1].s == ":") {
    std::string n = cur().s; p += 2;
    auto it = lbls.find(n); if (it == lbls.end()) it = lbls.emplace(n, std::make_pair(new_label(), 0)).first;
    if (it->second.second) fail(T[p].file, ln, "rótulo '" + n + "' definido duas vezes");
    it->second.second = ln; lbl_use_line.erase(n);
    Stmt* s = mkst(ST_LABEL, ln); s->label = it->second.first;
    s->body = is_op("}") ? mkst(ST_NOP, ln) : statement(); return s;
  }
  Stmt* s = mkst(ST_EXPR, ln); s->e = expression(); expect_op(";"); return s;
}

void Parser::func_definition(Type* fty, const std::string& name, const std::vector<std::string>& pn, const Spec& sp, int ln) {
  Sym* s = declare_func(name, fty, ln);
  if (s->defined) fail(T[p].file, ln, "função '" + name + "' definida duas vezes");
  s->defined = true; s->ty = fty;
  P.funcs.emplace_back(new FuncDef()); FuncDef* f = P.funcs.back().get(); f->name = name; f->ret = fty->base; f->interrupt = sp.interrupt; f->line = ln; f->sym = s;
  if (f->ret->k == TY_STRUCT) fail(T[p].file, ln, "retorno de estruturas por valor não é suportado");
  if (f->interrupt && (fty->ptypes.size() != 1 || !is_scalar(fty->ptypes[0]) || !is_scalar(f->ret))) fail(T[p].file, ln, "função __interrupt deve ter a forma: unsigned nome(unsigned sp)");
  scopes.emplace_back(); fn = f;
  for (size_t i = 0; i < fty->ptypes.size(); i++) {
    Sym* ps = mks(i < pn.size() ? pn[i] : "", S_PARAM); ps->ty = fty->ptypes[i]; int sz = tsize(ps->ty); ps->off = 8 + 4 * (int)i + (sz < 4 ? 4 - sz : 0);
    f->params.push_back(ps); if (!ps->name.empty()) scopes.back()[ps->name] = ps;
  }
  lbls.clear(); lbl_use_line.clear();
  f->body = block(false); scopes.pop_back(); fn = nullptr;
  if (!lbl_use_line.empty()) fail(T[p].file, lbl_use_line.begin()->second, "rótulo '" + lbl_use_line.begin()->first + "' usado em 'goto' mas nunca definido");
}

void Parser::toplevel() {
  int ln = line(); Spec sp; Type* base = parse_base_type(sp);
  if (accept_op(";")) return;
  bool first = true;
  while (true) {
    std::string name; Type* ty = parse_declarator(base, name); std::vector<std::string> pn = last_pnames;
    if (sp.is_typedef) { Sym* s = mks(name, S_TYPEDEF); s->ty = ty; scopes[0][name] = s; }
    else if (ty->k == TY_FUNC) {
      if (first && is_op("{")) { func_definition(ty, name, pn, sp, ln); return; }
      declare_func(name, ty, ln);
    } else declare_global(name, ty, sp, ln, true);
    first = false;
    if (accept_op(",")) continue;
    expect_op(";"); break;
  }
}

void Parser::run() { while (cur().t != T_EOF) toplevel(); }

}  // namespace

void parse_program(const std::vector<Token>& toks, Program& prog) { Parser ps(toks, prog); ps.run(); }

}}  // namespace sf::cc
