// Estruturas internas do snowcc (pré-processador, léxico, AST e tabelas de símbolos).
#pragma once
#include "snowcc.h"
#include "cfasm.h"
#include <memory>
#include <unordered_map>
#include <set>

namespace sf { namespace cc {

struct CcError { std::string msg; };
[[noreturn]] inline void fail(const std::string& file, int line, const std::string& m) { throw CcError{file + ":" + std::to_string(line) + ": " + m}; }

enum Tok { T_EOF, T_ID, T_NUM, T_CHAR, T_STR, T_OP };
struct Token { Tok t = T_EOF; std::string s; i64 v = 0; bool uns = false; int line = 0; std::string file; };
std::vector<Token> preprocess_and_lex(const std::string& src, const std::string& file, const CcOptions& opt);

// ---- tipos ----
enum TK { TY_VOID, TY_CHAR, TY_SHORT, TY_INT, TY_PTR, TY_ARRAY, TY_STRUCT, TY_FUNC };
struct StructDef;
struct Type {
  TK k = TY_INT; bool uns = false; Type* base = nullptr; int len = 0; StructDef* sd = nullptr;
  std::vector<Type*> ptypes; bool has_proto = false, variadic = false;         // TY_FUNC
};
struct Member { std::string name; Type* ty; int off; };
struct StructDef { std::string tag; std::vector<Member> mem; int size = 0, align = 1; bool complete = false, is_union = false; };
int tsize(const Type* t);
int talign(const Type* t);
inline bool is_int(const Type* t) { return t->k == TY_CHAR || t->k == TY_SHORT || t->k == TY_INT; }
inline bool is_ptr(const Type* t) { return t->k == TY_PTR; }
inline bool is_scalar(const Type* t) { return is_int(t) || is_ptr(t); }

// ---- símbolos ----
enum SK { S_LOCAL, S_PARAM, S_GLOBAL, S_FUNC, S_ENUM, S_TYPEDEF, S_BUILTIN };
struct GInit { int off, size; i64 val; std::string ref; };       // item de inicializador de global: valor (ou rótulo+val)
struct Sym {
  std::string name, label; SK kind = S_LOCAL; Type* ty = nullptr; int off = 0; i64 enumv = 0; bool defined = false, is_static = false, is_extern = false;
  std::vector<GInit> init; bool has_init = false;
};

// ---- expressões ----
enum EK { E_NUM, E_STR, E_VAR, E_UN, E_BIN, E_ASSIGN, E_COND, E_CALL, E_MEMBER, E_CAST, E_COMMA, E_INCDEC, E_ADDR, E_DEREF };
struct Expr {
  EK k = E_NUM; Type* ty = nullptr; int line = 0; i64 num = 0; std::string str, op; Sym* sym = nullptr;
  Expr *a = nullptr, *b = nullptr, *c = nullptr; std::vector<Expr*> args; bool post = false, lval = false, uns = false;
};

// ---- comandos ----
enum StK { ST_EXPR, ST_IF, ST_WHILE, ST_DO, ST_FOR, ST_RETURN, ST_BREAK, ST_CONTINUE, ST_BLOCK, ST_SWITCH, ST_CASE, ST_DEFAULT, ST_NOP, ST_GOTO, ST_LABEL };
struct Stmt {
  StK k = ST_NOP; int line = 0; Expr *e = nullptr, *e2 = nullptr, *e3 = nullptr; Stmt *body = nullptr, *els = nullptr; std::vector<Stmt*> list;
  i64 val = 0; int label = 0;                                    // ST_CASE: valor e rótulo; ST_SWITCH: cases
  std::vector<std::pair<i64, int>> cases; int default_label = -1;
};
struct FuncDef { std::string name; Type* ret = nullptr; std::vector<Sym*> params; Stmt* body = nullptr; int frame = 0; bool interrupt = false; int line = 0; Sym* sym = nullptr; };

struct Program {
  std::vector<std::unique_ptr<Type>> types; std::vector<std::unique_ptr<StructDef>> structs; std::vector<std::unique_ptr<Sym>> syms;
  std::vector<std::unique_ptr<Expr>> exprs; std::vector<std::unique_ptr<Stmt>> stmts; std::vector<std::unique_ptr<FuncDef>> funcs;
  std::vector<Sym*> globals; std::vector<std::string> strings; int next_label = 0;
  Sym* find_global(const std::string& n) const;
  std::unordered_map<std::string, Sym*> gtab;
};

void parse_program(const std::vector<Token>& toks, Program& prog);
void generate(Program& prog, CfAsm& a, const CcOptions& opt);

}}  // namespace sf::cc
