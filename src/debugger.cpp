#include "debugger.h"
#include "cfdis.h"
#include <sstream>

namespace sf {

Debugger::Debugger(Soc& s) : soc(s) {}
Debugger::~Debugger() { if (hooked) soc.bus.on_access = prev_hook; }

void Debugger::install_hook() {
  if (hooked) return;
  prev_hook = soc.bus.on_access; hooked = true;
  soc.bus.on_access = [this](u32 a, int size, bool wr) {
    if (prev_hook) prev_hook(a, size, wr);
    for (auto& w : wps) {
      if ((wr ? w.wr : w.rd) && a < w.addr + w.len && a + (u32)size > w.addr) { wp_hit = true; wp_info = Stop{W_WATCHPOINT, w.id, soc.cpu.pc, a, wr}; }
    }
  };
}

std::string Debugger::symbol_at(u32 addr) const {
  const std::string* best = nullptr; u32 ba = 0;
  for (auto& kv : symbols) if (kv.second <= addr && (!best || kv.second >= ba)) { if (!best || kv.second > ba) { best = &kv.first; ba = kv.second; } }
  if (!best) return "";
  if (addr == ba) return *best;
  char b[24]; std::snprintf(b, sizeof b, "+0x%x", addr - ba); return *best + b;
}

bool Debugger::eval(const std::string& expr, u32& out) const {
  std::string e; for (char c : expr) if (c != ' ') e += c;
  if (e.empty()) return false;
  // termos separados por + e - (sem precedência; o primeiro pode ter sinal)
  u64 acc = 0; size_t i = 0; int sign = 1; bool any = false;
  while (i < e.size()) {
    if (e[i] == '+') { sign = 1; i++; continue; } if (e[i] == '-') { sign = -1; i++; continue; }
    size_t j = i; while (j < e.size() && e[j] != '+' && e[j] != '-') j++;
    std::string t = e.substr(i, j - i); i = j; u32 v = 0;
    if (t[0] == '*') { u32 a; if (!eval(t.substr(1), a)) return false; if (!soc.bus.peek32(a, v)) return false; }
    else if (t == "pc") v = soc.cpu.pc; else if (t == "sp") v = soc.cpu.a[7]; else if (t == "sr") v = soc.cpu.sr; else if (t == "vbr") v = soc.cpu.vbr;
    else if (t.size() == 2 && (t[0] == 'd' || t[0] == 'a') && t[1] >= '0' && t[1] <= '7') v = (t[0] == 'd' ? soc.cpu.d : soc.cpu.a)[t[1] - '0'];
    else if (t[0] == '$') { char* end; v = (u32)std::strtoul(t.c_str() + 1, &end, 16); if (*end) return false; }
    else if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) { char* end; v = (u32)std::strtoul(t.c_str(), &end, 16); if (*end) return false; }
    else if (isdigit((unsigned char)t[0])) { char* end; v = (u32)std::strtoul(t.c_str(), &end, 10); if (*end) return false; }
    else { auto it = symbols.find(t); if (it == symbols.end()) return false; v = it->second; }
    acc += sign > 0 ? v : (u64)(u32)(0 - v); any = true; sign = 1;
  }
  out = (u32)acc; return any;
}

int Debugger::add_breakpoint(u32 addr, bool temporary) { bps.push_back({next_id, addr, temporary}); return next_id++; }
int Debugger::add_watch(u32 addr, u32 len, bool rd, bool wr) { install_hook(); wps.push_back({next_id, addr, len ? len : 1, rd, wr}); return next_id++; }
bool Debugger::remove(int id) {
  for (size_t i = 0; i < bps.size(); i++) if (bps[i].id == id) { bps.erase(bps.begin() + i); return true; }
  for (size_t i = 0; i < wps.size(); i++) if (wps[i].id == id) { wps.erase(wps.begin() + i); return true; }
  return false;
}

Debugger::Stop Debugger::run_loop(const std::function<bool()>& stop_after, Ps max_ps, u64 max_steps) {
  Ps t_end = soc.sched.now + max_ps; u64 steps = 0; bool first = true; wp_hit = false;
  while (true) {
    if (soc.cpu.halted) return Stop{W_HALTED, 0, soc.cpu.pc, 0, false};
    if (!first) {
      for (size_t i = 0; i < bps.size(); i++) if (bps[i].addr == soc.cpu.pc && !soc.cpu.idle()) {
        Stop s{W_BREAKPOINT, bps[i].id, soc.cpu.pc, 0, false}; if (bps[i].temp) bps.erase(bps.begin() + i); return s;
      }
    }
    first = false;
    last_op_ = rd16(soc.cpu.pc);
    if (!soc.step_one()) return Stop{soc.cpu.halted ? W_HALTED : W_IDLE, 0, soc.cpu.pc, 0, false};
    steps++;
    if (wp_hit) { wp_info.pc = soc.cpu.pc; return wp_info; }
    if (stop_after && stop_after()) return Stop{W_STEPPED, 0, soc.cpu.pc, 0, false};
    if (steps >= max_steps) return Stop{W_STEPPED, 0, soc.cpu.pc, 0, false};
    if (soc.sched.now >= t_end) return Stop{W_TIME, 0, soc.cpu.pc, 0, false};
  }
}

Debugger::Stop Debugger::step(int n) { return run_loop(nullptr, ~0ull >> 1, (u64)std::max(1, n)); }
Debugger::Stop Debugger::cont(Ps max_ps) { return run_loop(nullptr, max_ps, ~0ull); }
Debugger::Stop Debugger::next() {
  u16 op = rd16(soc.cpu.pc);
  bool call = (op & 0xFF00) == 0x6100 || (op & 0xFFC0) == 0x4E80;
  if (!call) return step(1);
  int len = 2; disline(soc.cpu.pc, &len);
  u32 ret = soc.cpu.pc + len; int id = add_breakpoint(ret, true);
  Stop s = run_loop(nullptr, ~0ull >> 1, ~0ull);
  if (s.why != W_BREAKPOINT || s.id != id) remove(id); else s.why = W_STEPPED;
  return s;
}
Debugger::Stop Debugger::finish() {
  u32 sp0 = soc.cpu.a[7];                                // roda até um RTS que desempilhe acima do SP inicial
  Stop s = run_loop([&]() { return last_op_ == 0x4E75 && soc.cpu.a[7] > sp0; }, ~0ull >> 1, ~0ull);
  if (s.why == W_STEPPED) s.why = W_FINISHED;
  return s;
}

std::vector<u32> Debugger::backtrace(int max_frames) const {
  std::vector<u32> v{soc.cpu.pc}; u32 fp = soc.cpu.a[6];
  // dentro do prólogo (antes do LINK) o endereço de retorno está no topo da pilha
  u16 op = rd16(soc.cpu.pc);
  if ((op & 0xFFF8) == 0x4E50) { u32 r; if (soc.bus.peek32(soc.cpu.a[7], r)) v.push_back(r); return v; }
  for (int i = 0; i < max_frames && fp; i++) {
    u32 prev, ret; if (!soc.bus.peek32(fp, prev) || !soc.bus.peek32(fp + 4, ret)) break; if (!ret) break; v.push_back(ret);
    if (prev <= fp) break; fp = prev;
  }
  return v;
}

std::string Debugger::disline(u32 addr, int* len) const {
  std::function<std::string(u32)> sf = [&](u32 a) { std::string s = symbol_at(a); return s; };
  auto rd = [&](u32 a) { return rd16(a); };
  CfInsn in = cf_disasm(rd, addr, &sf); if (len) *len = in.len;
  char b[160]; std::string sy = symbol_at(addr);
  std::snprintf(b, sizeof b, "  0x%08x%s%s:  ", addr, sy.empty() ? "" : " <", sy.empty() ? "" : (sy + ">").c_str());
  return b + in.text;
}

std::string Debugger::regs_text() const {
  char b[256]; std::string o; const Cpu& c = soc.cpu;
  for (int i = 0; i < 8; i++) { std::snprintf(b, sizeof b, "d%d=%08x%s", i, c.d[i], i % 4 == 3 ? "\n" : "  "); o += b; }
  for (int i = 0; i < 8; i++) { std::snprintf(b, sizeof b, "a%d=%08x%s", i, c.a[i], i % 4 == 3 ? "\n" : "  "); o += b; }
  std::snprintf(b, sizeof b, "pc=%08x  sr=%04x [%s%s%s%s%s%s] ipl=%d  %s\n", c.pc, c.sr, (c.sr & 0x2000) ? "S" : "U", (c.sr & 16) ? "X" : "-", (c.sr & 8) ? "N" : "-", (c.sr & 4) ? "Z" : "-", (c.sr & 2) ? "V" : "-", (c.sr & 1) ? "C" : "-", (c.sr >> 8) & 7, c.halted ? "HALTED" : c.stopped ? "STOPPED" : "running");
  o += b;
  return o;
}

std::string Debugger::describe(const Stop& s) const {
  char b[200];
  switch (s.why) {
    case W_BREAKPOINT: std::snprintf(b, sizeof b, "Breakpoint %d em 0x%08x\n", s.id, s.pc); break;
    case W_WATCHPOINT: std::snprintf(b, sizeof b, "Watchpoint %d: %s de 0x%08x (pc=0x%08x)\n", s.id, s.write ? "escrita" : "leitura", s.addr, s.pc); break;
    case W_HALTED: std::snprintf(b, sizeof b, "CPU parada (HALT) em 0x%08x\n", s.pc); break;
    case W_IDLE: std::snprintf(b, sizeof b, "CPU parada em STOP sem eventos pendentes (pc=0x%08x)\n", s.pc); break;
    case W_TIME: std::snprintf(b, sizeof b, "Limite de tempo simulado atingido (pc=0x%08x)\n", s.pc); break;
    case W_FINISHED: std::snprintf(b, sizeof b, "Retornou para 0x%08x\n", s.pc); break;
    default: b[0] = 0;
  }
  return std::string(b) + disline(s.pc) + "\n";
}

std::string Debugger::exec(const std::string& line) {
  std::istringstream in(line); std::string cmd; in >> cmd; std::vector<std::string> a; std::string t; while (in >> t) a.push_back(t);
  std::string rest; for (size_t i = 0; i < a.size(); i++) rest += (i ? " " : "") + a[i];
  auto num = [&](const std::string& e, u32& v) { return eval(e, v); };
  char b[256];
  if (cmd.empty()) return "";
  if (cmd == "help" || cmd == "h") return "regs | step [n] | next | finish | cont [ps] | break X | tbreak X | watch X [len] [r|w|rw] | delete N | info break|sym | x[/N[b|w|l|s]] X | dis [X] [n] | bt | print E | set REG|*ADDR V | time\n";
  if (cmd == "regs" || cmd == "r") return regs_text();
  if (cmd == "step" || cmd == "s") { u32 n = 1; if (!a.empty()) num(a[0], n); return describe(step((int)n)); }
  if (cmd == "next" || cmd == "n") return describe(next());
  if (cmd == "finish") return describe(finish());
  if (cmd == "cont" || cmd == "c") { u32 ps = 0; if (!a.empty() && num(a[0], ps)) return describe(cont((Ps)ps)); return describe(cont()); }
  if (cmd == "break" || cmd == "b" || cmd == "tbreak") {
    u32 ad; if (a.empty() || !num(rest, ad)) return "uso: break <endereço|símbolo>\n";
    int id = add_breakpoint(ad, cmd == "tbreak"); std::snprintf(b, sizeof b, "Breakpoint %d em 0x%08x\n", id, ad); return b;
  }
  if (cmd == "watch") {
    u32 ad, len = 4; if (a.empty() || !num(a[0], ad)) return "uso: watch <endereço> [len] [r|w|rw]\n";
    bool rd = false, wr = true; for (size_t i = 1; i < a.size(); i++) { if (a[i] == "r") { rd = true; wr = false; } else if (a[i] == "w") { rd = false; wr = true; } else if (a[i] == "rw") { rd = wr = true; } else num(a[i], len); }
    int id = add_watch(ad, len, rd, wr); std::snprintf(b, sizeof b, "Watchpoint %d em 0x%08x (%u B, %s)\n", id, ad, len, rd && wr ? "rw" : rd ? "r" : "w"); return b;
  }
  if (cmd == "delete" || cmd == "d") { u32 id; if (a.empty() || !num(a[0], id)) return "uso: delete N\n"; return remove((int)id) ? "removido\n" : "não existe\n"; }
  if (cmd == "info") {
    std::string o;
    if (!a.empty() && a[0] == "break") { for (auto& x : bps) { std::snprintf(b, sizeof b, "%d  breakpoint%s 0x%08x %s\n", x.id, x.temp ? " (temp)" : "", x.addr, symbol_at(x.addr).c_str()); o += b; }
      for (auto& w : wps) { std::snprintf(b, sizeof b, "%d  watchpoint 0x%08x len %u %s\n", w.id, w.addr, w.len, w.rd && w.wr ? "rw" : w.rd ? "r" : "w"); o += b; } if (o.empty()) o = "nenhum\n"; return o; }
    if (!a.empty() && a[0] == "sym") { std::vector<std::pair<u32, std::string>> v; for (auto& kv : symbols) v.push_back({kv.second, kv.first}); std::sort(v.begin(), v.end());
      for (auto& p : v) { std::snprintf(b, sizeof b, "0x%08x %s\n", p.first, p.second.c_str()); o += b; } return o; }
    return "info break | info sym\n";
  }
  if (cmd.rfind("x", 0) == 0 && (cmd.size() == 1 || cmd[1] == '/')) {
    u32 n = 1; char u = 'l'; if (cmd.size() > 2) { std::string f = cmd.substr(2); size_t i = 0; u32 v = 0; bool dig = false; while (i < f.size() && isdigit((unsigned char)f[i])) { v = v * 10 + (f[i] - '0'); i++; dig = true; } if (dig) n = v; if (i < f.size()) u = f[i]; }
    u32 ad; if (a.empty() || !num(rest, ad)) return "uso: x/N[bwl|s] <endereço>\n";
    std::string o;
    if (u == 's') { std::string s; for (u32 i = 0; i < 256; i++) { u8 c; if (!soc.bus.peek(ad + i, &c, 1) || !c) break; s += (char)c; } std::snprintf(b, sizeof b, "0x%08x: \"%s\"\n", ad, s.c_str()); return b; }
    int sz = u == 'b' ? 1 : u == 'w' ? 2 : 4; int per = 16 / sz;
    for (u32 i = 0; i < n; i++) {
      if (i % per == 0) { std::snprintf(b, sizeof b, "%s0x%08x:", i ? "\n" : "", ad + i * sz); o += b; }
      u8 q[4]; if (!soc.bus.peek(ad + i * sz, q, sz)) { o += " ??"; continue; }
      std::snprintf(b, sizeof b, sz == 1 ? " %02x" : sz == 2 ? " %04x" : " %08x", sz == 1 ? q[0] : sz == 2 ? rbe16(q) : rbe32(q)); o += b;
    }
    return o + "\n";
  }
  if (cmd == "dis") {
    u32 ad = soc.cpu.pc, n = 8; if (!a.empty()) { if (!num(a[0], ad)) return "endereço inválido\n"; } if (a.size() > 1) num(a[1], n);
    std::string o; for (u32 i = 0; i < n; i++) { int len = 2; std::string l = disline(ad, &len); o += (ad == soc.cpu.pc ? "=>" : "  ") + l.substr(2) + "\n"; ad += len; } return o;
  }
  if (cmd == "bt") {
    std::string o; int i = 0; for (u32 pc : backtrace()) { std::string s = symbol_at(pc); std::snprintf(b, sizeof b, "#%d  0x%08x %s\n", i++, pc, s.c_str()); o += b; } return o;
  }
  if (cmd == "print" || cmd == "p") { u32 v; if (!num(rest, v)) return "expressão inválida\n"; std::snprintf(b, sizeof b, "%u (0x%08x)\n", v, v); return b; }
  if (cmd == "set") {
    if (a.size() < 2) return "uso: set <reg|*endereço> <valor>\n"; u32 v; std::string val; for (size_t i = 1; i < a.size(); i++) val += a[i]; if (!num(val, v)) return "valor inválido\n";
    const std::string& r = a[0];
    if (r == "pc") soc.cpu.pc = v; else if (r == "sp") soc.cpu.a[7] = v; else if (r == "sr") soc.cpu.sr = v & 0xFFFF;
    else if (r.size() == 2 && (r[0] == 'd' || r[0] == 'a') && r[1] >= '0' && r[1] <= '7') (r[0] == 'd' ? soc.cpu.d : soc.cpu.a)[r[1] - '0'] = v;
    else if (r[0] == '*') { u32 ad; if (!num(r.substr(1), ad)) return "endereço inválido\n"; soc.bus.cache_flush_all(false); soc.bus.raw_w32(ad, v); soc.bus.cache_inval_range(ad, 4); soc.bus.icache_inval_range(ad, 4); }
    else return "registrador desconhecido\n";
    return "ok\n";
  }
  if (cmd == "time") { std::snprintf(b, sizeof b, "tempo simulado %.3f us; %llu instruções; %llu ciclos\n", soc.sched.now / 1e6, (unsigned long long)soc.cpu.instrs, (unsigned long long)soc.cpu.cycles); return b; }
  return "comando desconhecido (help)\n";
}

}  // namespace sf
