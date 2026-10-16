// snowcc: pré-processador (#define objeto/função, #undef, #include, #if/#ifdef/#ifndef/#elif/#else/#endif) e analisador léxico.
#include "snowcc_int.h"
#include <fstream>
#include <sstream>

namespace sf { namespace cc {
namespace {

struct Macro { bool func = false; std::vector<std::string> params; std::string body; };

static bool is_idstart(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool is_idchar(char c) { return isalnum((unsigned char)c) || c == '_'; }

// Remove comentários preservando quebras de linha e literais.
static std::string strip_comments(const std::string& s, const std::string& file) {
  std::string o; size_t i = 0; int line = 1;
  while (i < s.size()) {
    char c = s[i];
    if (c == '"' || c == '\'') { char q = c; o += c; i++; while (i < s.size() && s[i] != q && s[i] != '\n') { if (s[i] == '\\' && i + 1 < s.size()) { o += s[i++]; } o += s[i++]; } if (i < s.size() && s[i] == q) o += s[i++]; continue; }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') { while (i < s.size() && s[i] != '\n') i++; continue; }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
      i += 2; int start = line; bool closed = false;
      while (i < s.size()) { if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '/') { i += 2; closed = true; break; } if (s[i] == '\n') { o += '\n'; line++; } i++; }
      if (!closed) fail(file, start, "comentário não terminado");
      o += ' '; continue;
    }
    if (c == '\n') line++;
    o += c; i++;
  }
  return o;
}

class PP {
 public:
  PP(const CcOptions& o) : opt(o) { for (auto& kv : o.defines) { Macro m; m.body = kv.second; macros[kv.first] = m; } }
  std::vector<Token> toks;
  void process(const std::string& text, const std::string& file, int depth);

 private:
  const CcOptions& opt; std::map<std::string, Macro> macros;
  struct Cond { bool active, taken, parent; };

  std::string expand(const std::string& s, std::set<std::string>& active, const std::string& file, int line);
  bool load_include(const std::string& name, bool quoted, const std::string& from, std::string& text, std::string& path);
  i64 eval_if(const std::string& expr, const std::string& file, int line);
  void lex_line(const std::string& s, const std::string& file, int line);
};

std::string PP::expand(const std::string& s, std::set<std::string>& active, const std::string& file, int line) {
  std::string o; size_t i = 0;
  while (i < s.size()) {
    char c = s[i];
    if (c == '"' || c == '\'') { char q = c; o += c; i++; while (i < s.size() && s[i] != q) { if (s[i] == '\\' && i + 1 < s.size()) o += s[i++]; o += s[i++]; } if (i < s.size()) o += s[i++]; continue; }
    if (isdigit((unsigned char)c)) { while (i < s.size() && (is_idchar(s[i]) || s[i] == '.')) o += s[i++]; continue; }
    if (!is_idstart(c)) { o += c; i++; continue; }
    size_t j = i; while (j < s.size() && is_idchar(s[j])) j++;
    std::string id = s.substr(i, j - i);
    if (id == "__LINE__") { o += std::to_string(line); i = j; continue; }
    auto it = macros.find(id);
    if (it == macros.end() || active.count(id)) { o += id; i = j; continue; }
    const Macro& m = it->second;
    if (!m.func) {
      std::set<std::string> a2 = active; a2.insert(id);
      std::string bd = m.body;
      for (size_t h; (h = bd.find("##")) != std::string::npos;) {
        size_t x = h, y = h + 2; while (x > 0 && isspace((unsigned char)bd[x - 1])) x--; while (y < bd.size() && isspace((unsigned char)bd[y])) y++;
        bd.erase(x, y - x);
      }
      o += expand(bd, a2, file, line); i = j; continue;
    }
    size_t k = j; while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) k++;
    if (k >= s.size() || s[k] != '(') { o += id; i = j; continue; }
    // argumentos
    k++; std::vector<std::string> args; std::string cur; int depth = 0;
    while (k < s.size()) {
      char d = s[k];
      if (d == '"' || d == '\'') { char q = d; cur += d; k++; while (k < s.size() && s[k] != q) { if (s[k] == '\\' && k + 1 < s.size()) cur += s[k++]; cur += s[k++]; } if (k < s.size()) cur += s[k++]; continue; }
      if (d == '(') depth++;
      if (d == ')') { if (depth == 0) break; depth--; }
      if (d == ',' && depth == 0) { args.push_back(cur); cur.clear(); k++; continue; }
      cur += d; k++;
    }
    if (k >= s.size()) fail(file, line, "macro '" + id + "' com parênteses não fechados");
    args.push_back(cur); k++;
    if (m.params.empty() && args.size() == 1) { bool blank = true; for (char ch : args[0]) if (!isspace((unsigned char)ch)) blank = false; if (blank) args.clear(); }
    if (args.size() != m.params.size()) fail(file, line, "macro '" + id + "': número de argumentos incorreto");
    std::set<std::string> a2 = active; a2.insert(id);
    std::vector<std::string> raw = args;                  // argumentos crus: usados por # e ##
    for (auto& a : args) a = expand(a, active, file, line);
    // substitui parâmetros no corpo
    std::string sub; size_t b = 0; bool pasting = false; const std::string& body = m.body;
    auto trim = [](std::string t) { size_t x = 0, y = t.size(); while (x < y && isspace((unsigned char)t[x])) x++; while (y > x && isspace((unsigned char)t[y - 1])) y--; return t.substr(x, y - x); };
    auto param_idx = [&](const std::string& w) { for (size_t pi = 0; pi < m.params.size(); pi++) if (m.params[pi] == w) return (int)pi; return -1; };
    while (b < body.size()) {
      char ch = body[b];
      if (ch == '"' || ch == '\'') { char q = ch; sub += ch; b++; while (b < body.size() && body[b] != q) { if (body[b] == '\\' && b + 1 < body.size()) sub += body[b++]; sub += body[b++]; } if (b < body.size()) sub += body[b++]; continue; }
      if (ch == '#' && b + 1 < body.size() && body[b + 1] == '#') {   // ##: cola os tokens vizinhos
        while (!sub.empty() && isspace((unsigned char)sub.back())) sub.pop_back();
        b += 2; while (b < body.size() && isspace((unsigned char)body[b])) b++;
        pasting = true; continue;
      }
      if (ch == '#') {                                    // #param: transforma o argumento em string
        size_t q = b + 1; while (q < body.size() && isspace((unsigned char)body[q])) q++;
        size_t e = q; while (e < body.size() && is_idchar(body[e])) e++;
        int pi = e > q && is_idstart(body[q]) ? param_idx(body.substr(q, e - q)) : -1;
        if (pi < 0) fail(file, line, "macro '" + id + "': '#' deve preceder um parâmetro");
        std::string str = "\""; for (char cc : trim(raw[pi])) { if (cc == '"' || cc == '\\') str += '\\'; str += cc; }
        sub += str + "\""; b = e; continue;
      }
      if (is_idstart(ch)) {
        size_t e = b; while (e < body.size() && is_idchar(body[e])) e++;
        std::string w = body.substr(b, e - b); int pi = param_idx(w);
        size_t nx = e; while (nx < body.size() && isspace((unsigned char)body[nx])) nx++;
        bool next_paste = nx + 1 < body.size() && body[nx] == '#' && body[nx + 1] == '#';
        if (pi >= 0) sub += (pasting || next_paste) ? trim(raw[pi]) : args[pi]; else sub += w;
        pasting = false; b = e; continue;
      }
      if (!isspace((unsigned char)ch)) pasting = false;
      sub += ch; b++;
    }
    o += expand(sub, a2, file, line); i = k;
  }
  return o;
}

bool PP::load_include(const std::string& name, bool quoted, const std::string& from, std::string& text, std::string& path) {
  if (opt.include_loader && opt.include_loader(name, text)) { path = name; return true; }
  std::vector<std::string> dirs;
  if (quoted) { size_t sl = from.find_last_of('/'); dirs.push_back(sl == std::string::npos ? "." : from.substr(0, sl)); }
  for (auto& d : opt.include_dirs) dirs.push_back(d);
  for (auto& d : dirs) {
    std::string p = d + "/" + name; std::ifstream f(p, std::ios::binary);
    if (f) { std::stringstream ss; ss << f.rdbuf(); text = ss.str(); path = p; return true; }
  }
  return false;
}

// Avaliador de expressões de #if (inteiros, defined, ! ~ - + * / % << >> < > <= >= == != & ^ | && || ?: e parênteses)
struct IfEval {
  std::vector<std::string> t; size_t p = 0; const std::string& file; int line;
  IfEval(const std::string& s, const std::string& f, int l) : file(f), line(l) {
    size_t i = 0;
    while (i < s.size()) {
      if (isspace((unsigned char)s[i])) { i++; continue; }
      if (isalnum((unsigned char)s[i]) || s[i] == '_') { size_t j = i; while (j < s.size() && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++; t.push_back(s.substr(i, j - i)); i = j; continue; }
      static const char* two[] = {"<<", ">>", "<=", ">=", "==", "!=", "&&", "||"};
      bool m = false; for (auto op : two) if (s.compare(i, 2, op) == 0) { t.push_back(op); i += 2; m = true; break; }
      if (m) continue; t.push_back(std::string(1, s[i])); i++;
    }
  }
  bool at(const char* s) { return p < t.size() && t[p] == s; }
  bool eat(const char* s) { if (at(s)) { p++; return true; } return false; }
  i64 primary() {
    if (p >= t.size()) fail(file, line, "expressão de #if incompleta");
    std::string x = t[p++];
    if (x == "(") { i64 v = cond(); if (!eat(")")) fail(file, line, "falta ')' em #if"); return v; }
    if (x == "!") return !primary(); if (x == "~") return ~primary(); if (x == "-") return -primary(); if (x == "+") return primary();
    if (isdigit((unsigned char)x[0])) { char* e; i64 v = (i64)std::strtoull(x.c_str(), &e, 0); return v; }
    return 0;                                                  // identificador não definido
  }
  i64 mul() { i64 v = primary(); while (true) { if (eat("*")) v *= primary(); else if (eat("/")) { i64 d = primary(); v = d ? v / d : 0; } else if (eat("%")) { i64 d = primary(); v = d ? v % d : 0; } else return v; } }
  i64 add() { i64 v = mul(); while (true) { if (eat("+")) v += mul(); else if (eat("-")) v -= mul(); else return v; } }
  i64 shf() { i64 v = add(); while (true) { if (eat("<<")) v <<= add(); else if (eat(">>")) v >>= add(); else return v; } }
  i64 rel() { i64 v = shf(); while (true) { if (eat("<=")) v = v <= shf(); else if (eat(">=")) v = v >= shf(); else if (eat("<")) v = v < shf(); else if (eat(">")) v = v > shf(); else return v; } }
  i64 eq() { i64 v = rel(); while (true) { if (eat("==")) v = v == rel(); else if (eat("!=")) v = v != rel(); else return v; } }
  i64 band() { i64 v = eq(); while (at("&")) { p++; v &= eq(); } return v; }
  i64 bxor() { i64 v = band(); while (at("^")) { p++; v ^= band(); } return v; }
  i64 bor() { i64 v = bxor(); while (at("|")) { p++; v |= bxor(); } return v; }
  i64 land() { i64 v = bor(); while (eat("&&")) { i64 r = bor(); v = v && r; } return v; }
  i64 lor() { i64 v = land(); while (eat("||")) { i64 r = land(); v = v || r; } return v; }
  i64 cond() { i64 v = lor(); if (eat("?")) { i64 a = cond(); if (!eat(":")) fail(file, line, "falta ':' em #if"); i64 b = cond(); return v ? a : b; } return v; }
};

i64 PP::eval_if(const std::string& expr, const std::string& file, int line) {
  // defined X / defined(X) antes da expansão
  std::string e; size_t i = 0;
  while (i < expr.size()) {
    if (is_idstart(expr[i])) {
      size_t j = i; while (j < expr.size() && is_idchar(expr[j])) j++;
      std::string id = expr.substr(i, j - i);
      if (id == "defined") {
        size_t k = j; while (k < expr.size() && isspace((unsigned char)expr[k])) k++; bool par = k < expr.size() && expr[k] == '('; if (par) k++;
        while (k < expr.size() && isspace((unsigned char)expr[k])) k++;
        size_t s = k; while (k < expr.size() && is_idchar(expr[k])) k++; std::string name = expr.substr(s, k - s);
        while (k < expr.size() && isspace((unsigned char)expr[k])) k++; if (par && k < expr.size() && expr[k] == ')') k++;
        e += macros.count(name) ? " 1 " : " 0 "; i = k; continue;
      }
      e += id; i = j; continue;
    }
    e += expr[i++];
  }
  std::set<std::string> act; std::string x = expand(e, act, file, line);
  IfEval ev(x, file, line); return ev.cond();
}

void PP::lex_line(const std::string& s, const std::string& file, int line) {
  size_t i = 0;
  while (i < s.size()) {
    char c = s[i];
    if (isspace((unsigned char)c)) { i++; continue; }
    Token t; t.line = line; t.file = file;
    if (is_idstart(c)) { size_t j = i; while (j < s.size() && is_idchar(s[j])) j++; t.t = T_ID; t.s = s.substr(i, j - i); i = j; toks.push_back(t); continue; }
    if (isdigit((unsigned char)c)) {
      size_t j = i; int base = 10;
      if (c == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X')) { base = 16; j = i + 2; }
      else if (c == '0' && i + 1 < s.size() && (s[i + 1] == 'b' || s[i + 1] == 'B')) { base = 2; j = i + 2; }
      else if (c == '0' && i + 1 < s.size() && isdigit((unsigned char)s[i + 1])) { base = 8; j = i + 1; }
      size_t ds = j; while (j < s.size() && isxdigit((unsigned char)s[j]) && (base == 16 || (s[j] >= '0' && s[j] <= '9'))) j++;
      std::string digits = s.substr(ds, j - ds); if (digits.empty()) fail(file, line, "constante numérica inválida");
      unsigned long long v = std::strtoull(digits.c_str(), nullptr, base);
      while (j < s.size() && (s[j] == 'u' || s[j] == 'U' || s[j] == 'l' || s[j] == 'L')) { if (s[j] == 'u' || s[j] == 'U') t.uns = true; j++; }
      if (j < s.size() && is_idchar(s[j])) fail(file, line, "sufixo numérico inválido");
      if (v > 0xFFFFFFFFull) fail(file, line, "constante maior que 32 bits");
      if (v > 0x7FFFFFFFull) t.uns = true;
      t.t = T_NUM; t.v = (i64)v; i = j; toks.push_back(t); continue;
    }
    if (c == '\'') {
      size_t j = i + 1; i64 v = 0;
      if (j < s.size() && s[j] == '\\') {
        j++; char e = j < s.size() ? s[j++] : 0;
        switch (e) { case 'n': v = 10; break; case 't': v = 9; break; case 'r': v = 13; break; case '0': v = 0; break; case 'a': v = 7; break; case 'b': v = 8; break; case 'f': v = 12; break; case 'v': v = 11; break; case '\\': v = '\\'; break; case '\'': v = '\''; break; case '"': v = '"'; break;
          case 'x': { while (j < s.size() && isxdigit((unsigned char)s[j])) { v = v * 16 + (isdigit((unsigned char)s[j]) ? s[j] - '0' : (tolower(s[j]) - 'a' + 10)); j++; } break; }
          default: v = e; }
        if (e >= '1' && e <= '7') { v = e - '0'; while (j < s.size() && s[j] >= '0' && s[j] <= '7') v = v * 8 + (s[j++] - '0'); }
      } else if (j < s.size()) v = (i8)s[j++];
      if (j >= s.size() || s[j] != '\'') fail(file, line, "constante de caractere inválida");
      t.t = T_CHAR; t.v = v; i = j + 1; toks.push_back(t); continue;
    }
    if (c == '"') {
      size_t j = i + 1; std::string v;
      while (j < s.size() && s[j] != '"') {
        if (s[j] == '\\' && j + 1 < s.size()) {
          j++; char e = s[j++];
          switch (e) { case 'n': v += '\n'; break; case 't': v += '\t'; break; case 'r': v += '\r'; break; case 'a': v += '\a'; break; case 'b': v += '\b'; break; case 'f': v += '\f'; break; case 'v': v += '\v'; break;
            case '\\': v += '\\'; break; case '\'': v += '\''; break; case '"': v += '"'; break;
            case 'x': { int x = 0; while (j < s.size() && isxdigit((unsigned char)s[j])) { x = x * 16 + (isdigit((unsigned char)s[j]) ? s[j] - '0' : (tolower(s[j]) - 'a' + 10)); j++; } v += (char)x; break; }
            default: if (e >= '0' && e <= '7') { int x = e - '0'; int n = 1; while (n < 3 && j < s.size() && s[j] >= '0' && s[j] <= '7') { x = x * 8 + (s[j++] - '0'); n++; } v += (char)x; } else v += e; }
        } else v += s[j++];
      }
      if (j >= s.size()) fail(file, line, "string não terminada");
      t.t = T_STR; t.s = v; i = j + 1; toks.push_back(t); continue;
    }
    static const char* ops3[] = {"<<=", ">>=", "..."};
    static const char* ops2[] = {"->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^="};
    bool m = false;
    for (auto op : ops3) if (s.compare(i, 3, op) == 0) { t.t = T_OP; t.s = op; i += 3; m = true; break; }
    if (!m) for (auto op : ops2) if (s.compare(i, 2, op) == 0) { t.t = T_OP; t.s = op; i += 2; m = true; break; }
    if (!m) { if (!strchr("+-*/%&|^~!<>=?:;,.(){}[]", c)) fail(file, line, std::string("caractere inesperado '") + c + "'"); t.t = T_OP; t.s = std::string(1, c); i++; }
    toks.push_back(t);
  }
}

void PP::process(const std::string& text0, const std::string& file, int depth) {
  if (depth > 16) fail(file, 1, "#include aninhado demais");
  std::string text = strip_comments(text0, file);
  std::vector<std::pair<int, std::string>> lines; { std::istringstream in(text); std::string l; int n = 0; while (std::getline(in, l)) { n++; lines.push_back({n, l}); } }
  std::vector<Cond> cs;
  auto active = [&]() { return cs.empty() || cs.back().active; };
  for (size_t li = 0; li < lines.size(); li++) {
    int line = lines[li].first; std::string l = lines[li].second;
    while (!l.empty() && l.back() == '\\' && li + 1 < lines.size()) { l.pop_back(); l += lines[++li].second; }
    size_t k = 0; while (k < l.size() && isspace((unsigned char)l[k])) k++;
    if (k < l.size() && l[k] == '#') {
      k++; while (k < l.size() && isspace((unsigned char)l[k])) k++;
      size_t e = k; while (e < l.size() && isalpha((unsigned char)l[e])) e++;
      std::string dir = l.substr(k, e - k), rest = l.substr(e); size_t rs = 0; while (rs < rest.size() && isspace((unsigned char)rest[rs])) rs++; rest = rest.substr(rs);
      while (!rest.empty() && isspace((unsigned char)rest.back())) rest.pop_back();
      if (dir == "if") { bool par = active(); bool v = par && eval_if(rest, file, line) != 0; cs.push_back({v, v, par}); }
      else if (dir == "ifdef" || dir == "ifndef") { bool par = active(); bool d = macros.count(rest) != 0; bool v = par && (dir == "ifdef" ? d : !d); cs.push_back({v, v, par}); }
      else if (dir == "elif") { if (cs.empty()) fail(file, line, "#elif sem #if"); Cond& c = cs.back(); if (c.taken || !c.parent) c.active = false; else { bool v = eval_if(rest, file, line) != 0; c.active = v; c.taken = v; } }
      else if (dir == "else") { if (cs.empty()) fail(file, line, "#else sem #if"); Cond& c = cs.back(); c.active = c.parent && !c.taken; c.taken = true; }
      else if (dir == "endif") { if (cs.empty()) fail(file, line, "#endif sem #if"); cs.pop_back(); }
      else if (!active()) { /* ignora outras diretivas em bloco inativo */ }
      else if (dir == "define") {
        size_t i = 0; while (i < rest.size() && is_idchar(rest[i])) i++;
        std::string name = rest.substr(0, i); if (name.empty()) fail(file, line, "#define sem nome");
        Macro m;
        if (i < rest.size() && rest[i] == '(') {
          m.func = true; size_t j = i + 1; std::string cur;
          while (j < rest.size() && rest[j] != ')') { if (rest[j] == ',') { m.params.push_back(cur); cur.clear(); } else if (!isspace((unsigned char)rest[j])) cur += rest[j]; j++; }
          if (!cur.empty()) m.params.push_back(cur);
          if (j >= rest.size()) fail(file, line, "#define: falta ')'");
          i = j + 1;
        }
        size_t b = i; while (b < rest.size() && isspace((unsigned char)rest[b])) b++;
        m.body = rest.substr(b); macros[name] = m;
      }
      else if (dir == "undef") macros.erase(rest);
      else if (dir == "include") {
        if (rest.size() < 3) fail(file, line, "#include inválido");
        bool quoted = rest[0] == '"'; char close = quoted ? '"' : '>'; size_t q = rest.find(close, 1);
        if ((rest[0] != '"' && rest[0] != '<') || q == std::string::npos) fail(file, line, "#include inválido");
        std::string name = rest.substr(1, q - 1), txt, path;
        if (!load_include(name, quoted, file, txt, path)) fail(file, line, "não achei o include '" + name + "'");
        process(txt, path, depth + 1);
      }
      else if (dir == "error") fail(file, line, "#error " + rest);
      else if (dir == "pragma" || dir == "line" || dir.empty()) { /* ignorado */ }
      else fail(file, line, "diretiva desconhecida #" + dir);
      continue;
    }
    if (!active()) continue;
    std::set<std::string> act; std::string x = expand(l, act, file, line); lex_line(x, file, line);
  }
  if (!cs.empty()) fail(file, lines.empty() ? 1 : lines.back().first, "#if sem #endif");
}

}  // namespace

std::vector<Token> preprocess_and_lex(const std::string& src, const std::string& file, const CcOptions& opt) {
  PP pp(opt); pp.process(src, file, 0);
  Token e; e.t = T_EOF; e.file = file; e.line = pp.toks.empty() ? 1 : pp.toks.back().line; pp.toks.push_back(e);
  return pp.toks;
}

}}  // namespace sf::cc
