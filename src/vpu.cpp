#include "vpu.h"
#include <map>
#include <sstream>
#include <cctype>

namespace sf {
using namespace abi;

Vpu::Vpu(Bus& b, u32 bit) : bus(b), irq_bit(bit) { reset_state(); }

void Vpu::reset_state() {
  std::memset(imem, 0, sizeof imem); std::memset(dmem, 0, sizeof dmem);
  std::memset(v, 0, sizeof v); std::memset(s, 0, sizeof s);
  queue.clear(); running = done = faulted = false; gen++;
}

void Vpu::load_program(const std::vector<u32>& code, u32 at) {
  if ((at + code.size()) * 4 > VPU_IMEM_SIZE) throw std::runtime_error("programa VPU excede 8 KiB");
  for (size_t i = 0; i < code.size(); i++) wbe32(imem + (at + i) * 4, code[i]);
}

u32 Vpu::status() const {
  return (running ? (u32)VPUS_BUSY : 0u) | (done ? (u32)VPUS_DONE : 0u) | (faulted ? (u32)VPUS_FAULT : 0u) | ((u32)queue.size() << 8);
}

// ---------------- Núcleo de execução ----------------
static inline void swz4(const float* in, u32 swz, float* out) {
  for (int i = 0; i < 4; i++) out[i] = in[(swz >> (2 * i)) & 3];
}

Vpu::Result Vpu::run_now(u32 pc, const u32 sregs[8]) {
  Result r; for (int i = 0; i < 8; i++) s[i] = sregs[i];
  bool lt = false, eq = false;
  const u32 nwords = VPU_IMEM_SIZE / 4;
  const u64 WATCHDOG = 4000000;
  auto fault = [&](u32 at) { r.fault = true; r.pc = at; };
  // DMA local: copia funcional no instante da instrução; o término (em ciclos do job) vem do árbitro de memória.
  const Ps t0 = bus.sched ? bus.sched->now : 0;
  Ps dma_end = t0; u64 dma_done = 0;
  auto dma = [&](u32 dm, u32 ext, u32 len, bool in) -> bool {
    if ((dm | ext | len) & 3 || (u64)dm + len > VPU_DMEM_SIZE) return false;
    u8* p = bus.raw(ext, len); if (len && !p) return false;
    if (in) std::memcpy(dmem + dm, p, len); else if (len) std::memcpy(p, dmem + dm, len);
    Ps start = std::max(t0 + cyc2ps(r.cycles + VPU_DMA_SETUP, VPU_HZ), dma_end);
    dma_end = bus.reserve(ext, len, start);
    dma_done = (u64)(((__uint128_t)(dma_end - t0) * VPU_HZ + 999999999999ull) / 1000000000000ull);
    r.dma_bytes += len; r.dma_xfers++;
    return true;
  };
  while (true) {
    if (pc >= nwords) { fault(pc); break; }
    if (r.cycles >= WATCHDOG) { fault(pc); break; }
    u32 w = rbe32(imem + pc * 4);
    u32 op = w >> 26, a = (w >> 21) & 31, b = (w >> 16) & 31, c = (w >> 11) & 31, swz = w & 0xFF;
    i32 imm = (i16)(w & 0xFFFF);
    u32 cur = pc++;
    r.cycles++;
    float vb[4];
    switch (op) {
      case vop::HALT: r.cycles = std::max(r.cycles, dma_done); return r;     // o job só termina com o DMA concluído
      case vop::NOP: break;
      case vop::VADD: case vop::VSUB: case vop::VMUL: case vop::VMAC: case vop::VMIN: case vop::VMAX: {
        swz4(v[c], swz, vb); float* d = v[a]; const float* x = v[b]; float t[4];
        for (int i = 0; i < 4; i++) {
          switch (op) {
            case vop::VADD: t[i] = x[i] + vb[i]; break;
            case vop::VSUB: t[i] = x[i] - vb[i]; break;
            case vop::VMUL: t[i] = x[i] * vb[i]; break;
            case vop::VMAC: t[i] = d[i] + x[i] * vb[i]; break;
            case vop::VMIN: t[i] = x[i] < vb[i] ? x[i] : vb[i]; break;
            default:        t[i] = x[i] > vb[i] ? x[i] : vb[i]; break;
          }
        }
        std::memcpy(d, t, 16); r.flops += (op == vop::VMAC) ? 8 : 4; break;
      }
      case vop::VDOT3: case vop::VDOT4: {
        swz4(v[c], swz, vb); const float* x = v[b];
        float d = x[0] * vb[0] + x[1] * vb[1] + x[2] * vb[2];
        if (op == vop::VDOT4) { d += x[3] * vb[3]; r.flops += 7; } else r.flops += 5;
        for (int i = 0; i < 4; i++) v[a][i] = d; break;
      }
      case vop::VMOV: case vop::VRCP: case vop::VRSQ: case vop::VABS: case vop::VFRC: {
        swz4(v[c], swz, vb); float t[4];
        for (int i = 0; i < 4; i++) {
          switch (op) {
            case vop::VMOV: t[i] = vb[i]; break;
            case vop::VRCP: t[i] = 1.0f / vb[i]; break;
            case vop::VRSQ: t[i] = 1.0f / std::sqrt(vb[i]); break;
            case vop::VFRC: t[i] = vb[i] - std::floor(vb[i]); break;
            default:        t[i] = std::fabs(vb[i]); break;
          }
        }
        std::memcpy(v[a], t, 16); if (op != vop::VMOV) r.flops += 4; break;
      }
      case vop::VLD: case vop::VST: {
        u32 addr = s[b & 7] + (u32)(imm * 16);
        if ((addr & 15) || addr + 16 > VPU_DMEM_SIZE) { fault(cur); return r; }
        if (op == vop::VLD) for (int i = 0; i < 4; i++) v[a][i] = rbef(dmem + addr + 4 * i);
        else for (int i = 0; i < 4; i++) wbef(dmem + addr + 4 * i, v[a][i]);
        break;
      }
      case vop::DMAIN:  if (!dma(s[a & 7], s[b & 7], s[c & 7], true)) { fault(cur); return r; } break;
      case vop::DMAOUT: if (!dma(s[b & 7], s[a & 7], s[c & 7], false)) { fault(cur); return r; } break;
      case vop::DMAWAIT: r.cycles = std::max(r.cycles, dma_done); break;
      case vop::SIGNAL: r.sigs.push_back({r.cycles, a & 15}); break;
      case vop::SETA: s[a & 7] = (u32)imm; break;
      case vop::SADD: s[a & 7] = s[b & 7] + (u32)imm; break;
      case vop::SCMP: lt = (i32)s[a & 7] < (i32)s[b & 7]; eq = s[a & 7] == s[b & 7]; break;
      case vop::BRLT: if (lt) pc = (u32)((i32)pc + imm); break;
      case vop::BRNE: if (!eq) pc = (u32)((i32)pc + imm); break;
      case vop::BRA: pc = (u32)((i32)pc + imm); break;
      default: fault(cur); return r;
    }
  }
  return r;
}

// ---------------- Fila de jobs / MMIO ----------------
bool Vpu::submit(u32 pc, const u32 sregs[8], bool irq) {
  if ((int)queue.size() + (running ? 1 : 0) >= VPU_QUEUE_DEPTH) return false;
  Job j; j.pc = pc; for (int i = 0; i < 8; i++) j.s[i] = sregs[i]; j.irq = irq;
  queue.push_back(j); done = false;
  if (!running) kick();
  return true;
}

void Vpu::kick() {
  if (queue.empty()) { running = false; return; }
  Job j = queue.front(); queue.pop_front();
  running = true;
  Result r = run_now(j.pc, j.s);
  last_cycles = r.cycles; last_flops = r.flops; total_cycles += r.cycles; total_flops += r.flops;
  dma_bytes += r.dma_bytes; dma_xfers += r.dma_xfers;
  if (r.fault) { faulted = true; fault_pc = r.pc; }
  u64 g = gen; bool irq = j.irq;
  for (auto& sg : r.sigs) {
    u32 n = sg.second;
    bus.sched->at(cyc2ps(sg.first, VPU_HZ), [this, g, n]() { if (g != gen) return; bus.sem[n] += 1; signals++; });
  }
  bus.sched->at(cyc2ps(std::max<u64>(r.cycles, 1), VPU_HZ), [this, g, irq]() {
    if (g != gen) return;       // job cancelado por RESET
    jobs_done++; running = false;
    if (irq) bus.raise_irq(irq_bit);
    if (!queue.empty()) kick(); else done = true;
  });
}

u32 Vpu::read(u32 off) {
  switch (off) {
    case VPU_STATUS: return status();
    case VPU_PC: return next_pc;
    case VPU_CYCLES: return (u32)last_cycles;
    case VPU_FLOPS: return (u32)last_flops;
    case VPU_FAULTPC: return fault_pc;
    case VPU_TOTAL_CYCLES: return (u32)total_cycles;
    case VPU_DMA_BYTES: return (u32)dma_bytes;
    case VPU_JOBS_DONE: return (u32)jobs_done;
  }
  if (off >= VPU_S0 && off < VPU_S0 + 32) return next_s[(off - VPU_S0) / 4];
  return 0;
}
void Vpu::write(u32 off, u32 val) {
  if (off == VPU_PC) { next_pc = val; return; }
  if (off >= VPU_S0 && off < VPU_S0 + 32) { next_s[(off - VPU_S0) / 4] = val; return; }
  if (off == VPU_STATUS) { if (val & VPUS_FAULT) faulted = false; return; }
  if (off == VPU_CTRL) {
    if (val & VPUC_RESET) { queue.clear(); running = false; done = false; faulted = false; gen++; }
    if (val & VPUC_START) { if (!submit(next_pc, next_s, val & VPUC_IRQ)) faulted = true; }
  }
}

// ---------------- Assembler / Disassembler ----------------
namespace {
struct Mn { const char* name; u32 op; int kind; };
// kind: 0 sem args, 1 vd,va,vb[.swz], 2 vd,vb[.swz], 3 vd,[s+imm], 4 [s+imm],vs, 5 sd,imm, 6 sd,ss,imm, 7 sa,sb, 8 label,
//       9 sa,sb,sc (DMA), 10 n (semáforo 0..15)
const Mn mnemonics[] = {
  {"HALT", vop::HALT, 0}, {"NOP", vop::NOP, 0},
  {"VADD", vop::VADD, 1}, {"VSUB", vop::VSUB, 1}, {"VMUL", vop::VMUL, 1}, {"VMAC", vop::VMAC, 1},
  {"VDOT3", vop::VDOT3, 1}, {"VDOT4", vop::VDOT4, 1}, {"VMIN", vop::VMIN, 1}, {"VMAX", vop::VMAX, 1},
  {"VMOV", vop::VMOV, 2}, {"VRCP", vop::VRCP, 2}, {"VRSQ", vop::VRSQ, 2}, {"VABS", vop::VABS, 2},
  {"VLD", vop::VLD, 3}, {"VST", vop::VST, 4}, {"SETA", vop::SETA, 5}, {"SADD", vop::SADD, 6},
  {"SCMP", vop::SCMP, 7}, {"BRLT", vop::BRLT, 8}, {"BRNE", vop::BRNE, 8}, {"BRA", vop::BRA, 8},
  {"VFRC", vop::VFRC, 2}, {"DMAIN", vop::DMAIN, 9}, {"DMAOUT", vop::DMAOUT, 9}, {"DMAWAIT", vop::DMAWAIT, 0}, {"SIGNAL", vop::SIGNAL, 10},
};
std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
bool parse_reg(const std::string& t, char kind, int maxn, u32& out, u32* swz) {
  std::string x = trim(t);
  if (x.size() < 2 || std::tolower(x[0]) != kind) return false;
  size_t i = 1; u32 n = 0;
  if (!std::isdigit((unsigned char)x[i])) return false;
  while (i < x.size() && std::isdigit((unsigned char)x[i])) n = n * 10 + (x[i++] - '0');
  if ((int)n >= maxn) return false;
  out = n;
  if (i < x.size()) {
    if (!swz || x[i] != '.' || x.size() - i - 1 != 4) return false;
    u32 sw = 0;
    for (int k = 0; k < 4; k++) {
      char ch = (char)std::tolower(x[i + 1 + k]); int l;
      if (ch == 'x' || ch == 'r') l = 0; else if (ch == 'y' || ch == 'g') l = 1;
      else if (ch == 'z' || ch == 'b') l = 2; else if (ch == 'w' || ch == 'a') l = 3; else return false;
      sw |= (u32)l << (2 * k);
    }
    *swz = sw;
  } else if (swz) *swz = vop::SWZ_IDENT;
  return true;
}
bool parse_int(const std::string& t, i32& out) {
  std::string x = trim(t); if (x.empty()) return false;
  try { size_t p; long v = std::stol(x, &p, 0); if (p != x.size()) return false; out = (i32)v; return true; } catch (...) { return false; }
}
bool parse_mem(const std::string& t, u32& sreg, i32& imm) {
  std::string x = trim(t);
  if (x.size() < 4 || x.front() != '[' || x.back() != ']') return false;
  x = x.substr(1, x.size() - 2);
  size_t p = x.find_first_of("+-");
  std::string r = trim(p == std::string::npos ? x : x.substr(0, p));
  if (!parse_reg(r, 's', 8, sreg, nullptr)) return false;
  imm = 0;
  if (p != std::string::npos) { if (!parse_int(x.substr(p), imm)) { std::string q = x.substr(p); if (q[0] == '+') q = q.substr(1); if (!parse_int(q, imm)) return false; } }
  return true;
}
std::vector<std::string> split_args(const std::string& s) {
  std::vector<std::string> v; std::string cur; int depth = 0;
  for (char ch : s) { if (ch == '[') depth++; if (ch == ']') depth--; if (ch == ',' && depth == 0) { v.push_back(trim(cur)); cur.clear(); } else cur += ch; }
  if (!trim(cur).empty()) v.push_back(trim(cur));
  return v;
}
}  // namespace

bool vpu_assemble(const std::string& src, std::vector<u32>& out, std::string& err) {
  out.clear(); err.clear();
  struct Line { int no; std::string mn; std::vector<std::string> args; };
  std::vector<Line> lines; std::map<std::string, u32> labels;
  std::istringstream is(src); std::string ln; int no = 0;
  while (std::getline(is, ln)) {
    no++;
    size_t c = ln.find(';'); if (c != std::string::npos) ln = ln.substr(0, c);
    c = ln.find("//"); if (c != std::string::npos) ln = ln.substr(0, c);
    ln = trim(ln);
    while (!ln.empty()) {
      size_t col = ln.find(':');
      if (col != std::string::npos && ln.find_first_of(" \t[") > col) {   // rótulo
        std::string l = trim(ln.substr(0, col));
        if (labels.count(l)) { err = "linha " + std::to_string(no) + ": rótulo duplicado " + l; return false; }
        labels[l] = (u32)lines.size(); ln = trim(ln.substr(col + 1)); continue;
      }
      break;
    }
    if (ln.empty()) continue;
    size_t sp = ln.find_first_of(" \t");
    Line L; L.no = no; L.mn = sp == std::string::npos ? ln : ln.substr(0, sp);
    for (auto& ch : L.mn) ch = (char)std::toupper((unsigned char)ch);
    if (sp != std::string::npos) L.args = split_args(ln.substr(sp + 1));
    lines.push_back(L);
  }
  auto E = [&](const Line& L, const std::string& m) { err = "linha " + std::to_string(L.no) + ": " + m; return false; };
  for (size_t idx = 0; idx < lines.size(); idx++) {
    const Line& L = lines[idx]; const Mn* m = nullptr;
    for (auto& x : mnemonics) if (L.mn == x.name) m = &x;
    if (!m) return E(L, "instrução desconhecida " + L.mn);
    u32 a = 0, b = 0, c = 0, sw = vop::SWZ_IDENT; i32 imm = 0; u32 w = 0;
    auto need = [&](size_t n) { return L.args.size() == n; };
    switch (m->kind) {
      case 0: if (!need(0)) return E(L, "sem operandos"); w = m->op << 26; break;
      case 1: if (!need(3) || !parse_reg(L.args[0], 'v', 32, a, nullptr) || !parse_reg(L.args[1], 'v', 32, b, nullptr) ||
                   !parse_reg(L.args[2], 'v', 32, c, &sw)) return E(L, "esperado vd, va, vb[.swz]");
              w = vpu_enc_r(m->op, a, b, c, sw); break;
      case 2: if (!need(2) || !parse_reg(L.args[0], 'v', 32, a, nullptr) || !parse_reg(L.args[1], 'v', 32, c, &sw))
                return E(L, "esperado vd, vb[.swz]");
              w = vpu_enc_r(m->op, a, 0, c, sw); break;
      case 3: if (!need(2) || !parse_reg(L.args[0], 'v', 32, a, nullptr) || !parse_mem(L.args[1], b, imm)) return E(L, "esperado vd, [sN+imm]");
              w = vpu_enc_i(m->op, a, b, imm); break;
      case 4: if (!need(2) || !parse_mem(L.args[0], b, imm) || !parse_reg(L.args[1], 'v', 32, a, nullptr)) return E(L, "esperado [sN+imm], vs");
              w = vpu_enc_i(m->op, a, b, imm); break;
      case 5: if (!need(2) || !parse_reg(L.args[0], 's', 8, a, nullptr) || !parse_int(L.args[1], imm)) return E(L, "esperado sd, imm");
              if (imm < -32768 || imm > 32767) return E(L, "imediato fora de 16 bits");
              w = vpu_enc_i(m->op, a, 0, imm); break;
      case 6: if (!need(3) || !parse_reg(L.args[0], 's', 8, a, nullptr) || !parse_reg(L.args[1], 's', 8, b, nullptr) || !parse_int(L.args[2], imm))
                return E(L, "esperado sd, ss, imm");
              if (imm < -32768 || imm > 32767) return E(L, "imediato fora de 16 bits");
              w = vpu_enc_i(m->op, a, b, imm); break;
      case 7: if (!need(2) || !parse_reg(L.args[0], 's', 8, a, nullptr) || !parse_reg(L.args[1], 's', 8, b, nullptr)) return E(L, "esperado sa, sb");
              w = vpu_enc_i(m->op, a, b, 0); break;
      case 9: if (!need(3) || !parse_reg(L.args[0], 's', 8, a, nullptr) || !parse_reg(L.args[1], 's', 8, b, nullptr) || !parse_reg(L.args[2], 's', 8, c, nullptr))
                return E(L, "esperado sDst, sSrc, sLen");
              w = vpu_enc_r(m->op, a, b, c, 0); break;
      case 10: if (!need(1) || !parse_int(L.args[0], imm) || imm < 0 || imm > 15) return E(L, "esperado semáforo 0..15");
              w = vpu_enc_r(m->op, (u32)imm, 0, 0, 0); break;
      case 8: { if (!need(1)) return E(L, "esperado rótulo");
              auto it = labels.find(L.args[0]); if (it == labels.end()) return E(L, "rótulo indefinido " + L.args[0]);
              imm = (i32)it->second - (i32)(idx + 1);
              w = vpu_enc_i(m->op, 0, 0, imm); break; }
    }
    out.push_back(w);
  }
  return true;
}

std::string vpu_disasm(u32 w) {
  u32 op = w >> 26, a = (w >> 21) & 31, b = (w >> 16) & 31, c = (w >> 11) & 31, sw = w & 0xFF; i32 imm = (i16)(w & 0xFFFF);
  const Mn* m = nullptr; for (auto& x : mnemonics) if (x.op == op) m = &x;
  if (!m) return "???";
  auto swz = [&](u32 s) { if (s == vop::SWZ_IDENT) return std::string(); std::string r = "."; for (int i = 0; i < 4; i++) r += "xyzw"[(s >> (2 * i)) & 3]; return r; };
  std::ostringstream o; o << m->name;
  switch (m->kind) {
    case 1: o << " v" << a << ", v" << b << ", v" << c << swz(sw); break;
    case 2: o << " v" << a << ", v" << c << swz(sw); break;
    case 3: o << " v" << a << ", [s" << (b & 7) << (imm >= 0 ? "+" : "") << imm << "]"; break;
    case 4: o << " [s" << (b & 7) << (imm >= 0 ? "+" : "") << imm << "], v" << a; break;
    case 5: o << " s" << (a & 7) << ", " << imm; break;
    case 6: o << " s" << (a & 7) << ", s" << (b & 7) << ", " << imm; break;
    case 7: o << " s" << (a & 7) << ", s" << (b & 7); break;
    case 8: o << " " << imm << "(rel)"; break;
    case 9: o << " s" << (a & 7) << ", s" << (b & 7) << ", s" << (c & 7); break;
    case 10: o << " " << (a & 15); break;
  }
  return o.str();
}

}  // namespace sf
