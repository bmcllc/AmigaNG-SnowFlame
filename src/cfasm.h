// Mini-assembler C++ para o subconjunto ColdFire do SnowFire-CF266 (usado por testes e demos).
// Gera bytes big-endian com rótulos e correções (fixups).
#pragma once
#include "common.h"
#include <map>

namespace sf {

class CfAsm {
 public:
  struct Op { int mode = 0, reg = 0; std::vector<u16> ext; bool imm = false; u32 immv = 0; std::string label; };
  static Op D(int n) { Op o; o.mode = 0; o.reg = n; return o; }
  static Op A(int n) { Op o; o.mode = 1; o.reg = n; return o; }
  static Op Ind(int n) { Op o; o.mode = 2; o.reg = n; return o; }
  static Op Post(int n) { Op o; o.mode = 3; o.reg = n; return o; }
  static Op Pre(int n) { Op o; o.mode = 4; o.reg = n; return o; }
  static Op Disp(int n, int d) { Op o; o.mode = 5; o.reg = n; o.ext.push_back((u16)d); return o; }
  // (d8,An,Xi.L*scale)  xi: 0-7 = Dn, 8-15 = An
  static Op Idx(int n, int d8, int xi, int scale_log2 = 0) {
    Op o; o.mode = 6; o.reg = n; o.ext.push_back((u16)((xi << 12) | 0x800 | (scale_log2 << 9) | (d8 & 0xFF))); return o;
  }
  // Formato completo (68020): (bd,An|PC,Xn*SF) com supressão de base/índice e indireção de memória (iis: 0 nenhuma, 1-3 pré-indexado/indireto com od nulo/word/long, 5-7 pós-indexado).
  // bdsz/odsz: 1 nulo, 2 word, 3 long. xi < 0: índice suprimido. n < 0: base suprimida (usa PC se pc = true).
  static Op Full(int n, i32 bd, int xi, int scale_log2 = 0, int bdsz = 3, int iis = 0, i32 od = 0, bool pc = false, bool long_idx = true) {
    Op o; o.mode = pc ? 7 : 6; o.reg = pc ? 3 : (n < 0 ? 0 : n);
    bool bs = !pc && n < 0, is = xi < 0; int odsz = (iis & 3);
    u16 e = (u16)((is ? 0 : (xi << 12)) | (long_idx ? 0x800 : 0) | (scale_log2 << 9) | 0x100 | (bs ? 0x80 : 0) | (is ? 0x40 : 0) | (bdsz << 4) | iis);
    o.ext.push_back(e);
    if (bdsz == 2) o.ext.push_back((u16)bd); else if (bdsz == 3) { o.ext.push_back((u16)((u32)bd >> 16)); o.ext.push_back((u16)bd); }
    if (odsz == 2) o.ext.push_back((u16)od); else if (odsz == 3) { o.ext.push_back((u16)((u32)od >> 16)); o.ext.push_back((u16)od); }
    return o;
  }
  static Op Abs(u32 addr) { Op o; o.mode = 7; o.reg = 1; o.ext = {(u16)(addr >> 16), (u16)addr}; return o; }
  static Op AbsL(const std::string& lbl) { Op o; o.mode = 7; o.reg = 1; o.ext = {0, 0}; o.label = lbl; return o; }
  static Op PcDisp(int d) { Op o; o.mode = 7; o.reg = 2; o.ext.push_back((u16)d); return o; }
  static Op Imm(u32 v) { Op o; o.mode = 7; o.reg = 4; o.imm = true; o.immv = v; return o; }
  static Op ImmL(const std::string& lbl) { Op o; o.mode = 7; o.reg = 4; o.imm = true; o.label = lbl; return o; }

  explicit CfAsm(u32 base = 0x1000) : base_(base) {}
  u32 here() const { return base_ + (u32)code_.size(); }
  void org_align(u32 a) { while (here() % a) w(0x4E71); }
  void label(const std::string& n) { labels_[n] = here(); }
  u32 label_addr(const std::string& n) const { auto it = labels_.find(n); if (it == labels_.end()) throw std::runtime_error("rótulo indefinido: " + n); return it->second; }
  void w(u16 v) { code_.push_back((u8)(v >> 8)); code_.push_back((u8)v); }
  void l(u32 v) { w((u16)(v >> 16)); w((u16)v); }
  void data_l(u32 v) { l(v); }
  void data_b(u8 v) { code_.push_back(v); }
  void data_w(u16 v) { w(v); }
  void data_ref(const std::string& lbl, i32 add = 0) { fix_.push_back({(u32)code_.size(), lbl, 1, add}); l(0); }   // palavra de 32 bits com o endereço de um rótulo
  void align(u32 n) { while (here() % n) code_.push_back(0); }
  void space(u32 n) { code_.insert(code_.end(), n, 0); }
  bool has_label(const std::string& n) const { return labels_.count(n) != 0; }
  void data_d(double d) { u64 b = d2u(d); l((u32)(b >> 32)); l((u32)b); }

  // ---- movimentação ----
  void move(int size, const Op& s, const Op& d) {
    int sb = size == 1 ? 1 : size == 2 ? 3 : 2;
    w((u16)((sb << 12) | (d.reg << 9) | (d.mode << 6) | (s.mode << 3) | s.reg));
    ea_ext(s, size); ea_ext(d, size);
  }
  void movel(const Op& s, const Op& d) { move(4, s, d); }
  void moveq(int v, int dn) { w((u16)(0x7000 | (dn << 9) | (v & 0xFF))); }
  void lea(const Op& s, int an) { w((u16)(0x41C0 | (an << 9) | (s.mode << 3) | s.reg)); ea_ext(s, 4); }
  void pea(const Op& s) { w((u16)(0x4840 | (s.mode << 3) | s.reg)); ea_ext(s, 4); }
  void swap(int dn) { w((u16)(0x4840 | dn)); }
  void ext_w(int dn) { w((u16)(0x4880 | dn)); }
  void ext_l(int dn) { w((u16)(0x48C0 | dn)); }
  void extb_l(int dn) { w((u16)(0x49C0 | dn)); }
  void movem_to(u16 mask, const Op& e) { w((u16)(0x48C0 | (e.mode << 3) | e.reg)); w(mask); ea_ext(e, 4); }
  void movem_from(const Op& e, u16 mask) { w((u16)(0x4CC0 | (e.mode << 3) | e.reg)); w(mask); ea_ext(e, 4); }
  // ---- aritmética ----
  void addq(int size, int q, const Op& e) { w((u16)(0x5000 | ((q & 7) << 9) | (szb(size) << 6) | (e.mode << 3) | e.reg)); ea_ext(e, size); }
  void subq(int size, int q, const Op& e) { w((u16)(0x5100 | ((q & 7) << 9) | (szb(size) << 6) | (e.mode << 3) | e.reg)); ea_ext(e, size); }
  void add(int size, const Op& s, int dn) { w((u16)(0xD000 | (dn << 9) | (szb(size) << 6) | (s.mode << 3) | s.reg)); ea_ext(s, size); }
  void sub(int size, const Op& s, int dn) { w((u16)(0x9000 | (dn << 9) | (szb(size) << 6) | (s.mode << 3) | s.reg)); ea_ext(s, size); }
  void cmp(int size, const Op& s, int dn) { w((u16)(0xB000 | (dn << 9) | (szb(size) << 6) | (s.mode << 3) | s.reg)); ea_ext(s, size); }
  void and_(int size, const Op& s, int dn) { w((u16)(0xC000 | (dn << 9) | (szb(size) << 6) | (s.mode << 3) | s.reg)); ea_ext(s, size); }
  void or_(int size, const Op& s, int dn) { w((u16)(0x8000 | (dn << 9) | (szb(size) << 6) | (s.mode << 3) | s.reg)); ea_ext(s, size); }
  void add_to(int size, int dn, const Op& d) { w((u16)(0xD100 | (dn << 9) | (szb(size) << 6) | (d.mode << 3) | d.reg)); ea_ext(d, size); }
  void sub_to(int size, int dn, const Op& d) { w((u16)(0x9100 | (dn << 9) | (szb(size) << 6) | (d.mode << 3) | d.reg)); ea_ext(d, size); }
  void and_to(int size, int dn, const Op& d) { w((u16)(0xC100 | (dn << 9) | (szb(size) << 6) | (d.mode << 3) | d.reg)); ea_ext(d, size); }
  void or_to(int size, int dn, const Op& d) { w((u16)(0x8100 | (dn << 9) | (szb(size) << 6) | (d.mode << 3) | d.reg)); ea_ext(d, size); }
  void eor(int size, int dn, const Op& d) { w((u16)(0xB100 | (dn << 9) | (szb(size) << 6) | (d.mode << 3) | d.reg)); ea_ext(d, size); }
  void adda(int size, const Op& s, int an) { w((u16)(0xD000 | (an << 9) | (size == 2 ? 0xC0 : 0x1C0) | (s.mode << 3) | s.reg)); ea_ext(s, size); }
  void suba(int size, const Op& s, int an) { w((u16)(0x9000 | (an << 9) | (size == 2 ? 0xC0 : 0x1C0) | (s.mode << 3) | s.reg)); ea_ext(s, size); }
  void cmpa(int size, const Op& s, int an) { w((u16)(0xB000 | (an << 9) | (size == 2 ? 0xC0 : 0x1C0) | (s.mode << 3) | s.reg)); ea_ext(s, size); }
  void imm_op(u16 base, int size, u32 v, const Op& e) {
    w((u16)(base | (szb(size) << 6) | (e.mode << 3) | e.reg));
    if (size == 4) l(v); else w((u16)(size == 1 ? (v & 0xFF) : v));
    ea_ext(e, size);
  }
  void ori(int s, u32 v, const Op& e) { imm_op(0x0000, s, v, e); }
  void andi(int s, u32 v, const Op& e) { imm_op(0x0200, s, v, e); }
  void subi(int s, u32 v, const Op& e) { imm_op(0x0400, s, v, e); }
  void addi(int s, u32 v, const Op& e) { imm_op(0x0600, s, v, e); }
  void eori(int s, u32 v, const Op& e) { imm_op(0x0A00, s, v, e); }
  void cmpi(int s, u32 v, const Op& e) { imm_op(0x0C00, s, v, e); }
  void clr(int size, const Op& e) { un(0x4200, size, e); }
  void neg(int size, const Op& e) { un(0x4400, size, e); }
  void not_(int size, const Op& e) { un(0x4600, size, e); }
  void tst(int size, const Op& e) { un(0x4A00, size, e); }
  void muls_l(const Op& s, int dl) { w((u16)(0x4C00 | (s.mode << 3) | s.reg)); w((u16)((dl << 12) | 0x0800)); ea_ext(s, 4); }
  void mulu_l(const Op& s, int dl) { w((u16)(0x4C00 | (s.mode << 3) | s.reg)); w((u16)(dl << 12)); ea_ext(s, 4); }
  void divs_l(const Op& s, int dq, int dr) { w((u16)(0x4C40 | (s.mode << 3) | s.reg)); w((u16)((dq << 12) | 0x0800 | dr)); ea_ext(s, 4); }
  void divu_l(const Op& s, int dq, int dr) { w((u16)(0x4C40 | (s.mode << 3) | s.reg)); w((u16)((dq << 12) | dr)); ea_ext(s, 4); }
  void muls_w(const Op& s, int dn) { w((u16)(0xC1C0 | (dn << 9) | (s.mode << 3) | s.reg)); ea_ext(s, 2); }
  void mulu_w(const Op& s, int dn) { w((u16)(0xC0C0 | (dn << 9) | (s.mode << 3) | s.reg)); ea_ext(s, 2); }
  void divs_w(const Op& s, int dn) { w((u16)(0x81C0 | (dn << 9) | (s.mode << 3) | s.reg)); ea_ext(s, 2); }
  void divu_w(const Op& s, int dn) { w((u16)(0x80C0 | (dn << 9) | (s.mode << 3) | s.reg)); ea_ext(s, 2); }
  void addx_l(int dy, int dx) { w((u16)(0xD180 | (dx << 9) | dy)); }
  void subx_l(int dy, int dx) { w((u16)(0x9180 | (dx << 9) | dy)); }
  void negx_l(int dn) { w((u16)(0x4080 | dn)); }
  void mov3q(int q, const Op& d) { w((u16)(0xA140 | (((q == -1 ? 0 : q) & 7) << 9) | (d.mode << 3) | d.reg)); ea_ext(d, 4); }   // q = -1 codifica 0
  void mvs(int size, const Op& s, int dn) { w((u16)(0x7100 | (dn << 9) | (size == 1 ? 0 : 0x40) | (s.mode << 3) | s.reg)); ea_ext(s, size); }
  void mvz(int size, const Op& s, int dn) { w((u16)(0x7180 | (dn << 9) | (size == 1 ? 0 : 0x40) | (s.mode << 3) | s.reg)); ea_ext(s, size); }
  void sats(int dn) { w((u16)(0x4C80 | dn)); }
  void ff1(int dn) { w((u16)(0x04C0 | dn)); }
  void byterev(int dn) { w((u16)(0x02C0 | dn)); }
  void bitrev(int dn) { w((u16)(0x00C0 | dn)); }
  void tas(const Op& e) { w((u16)(0x4AC0 | (e.mode << 3) | e.reg)); ea_ext(e, 1); }
  void move_from_ccr(int dn) { w((u16)(0x42C0 | dn)); }
  void tpf() { w(0x51F8); }
  void bchg_d(int dn, const Op& e) { w((u16)(0x0140 | (dn << 9) | (e.mode << 3) | e.reg)); ea_ext(e, 1); }
  void bclr_d(int dn, const Op& e) { w((u16)(0x0180 | (dn << 9) | (e.mode << 3) | e.reg)); ea_ext(e, 1); }
  void bset_d(int dn, const Op& e) { w((u16)(0x01C0 | (dn << 9) | (e.mode << 3) | e.reg)); ea_ext(e, 1); }
  void bchg_i(int bit, const Op& e) { w((u16)(0x0840 | (e.mode << 3) | e.reg)); w((u16)bit); ea_ext(e, 1); }
  // ---- deslocamentos (tipo: 0 AS, 1 LS, 3 RO) ----
  void shift_i(int type, bool left, int cnt, int dn, int size = 4) { w((u16)(0xE000 | ((cnt & 7) << 9) | (left ? 0x100 : 0) | (szb(size) << 6) | (type << 3) | dn)); }
  void shift_r(int type, bool left, int cntreg, int dn, int size = 4) { w((u16)(0xE020 | (cntreg << 9) | (left ? 0x100 : 0) | (szb(size) << 6) | (type << 3) | dn)); }
  void lsl(int cnt, int dn) { shift_i(1, true, cnt, dn); }
  void lsr(int cnt, int dn) { shift_i(1, false, cnt, dn); }
  void asr(int cnt, int dn) { shift_i(0, false, cnt, dn); }
  void asl(int cnt, int dn) { shift_i(0, true, cnt, dn); }
  // ---- bits ----
  void btst_i(int bit, const Op& e) { w((u16)(0x0800 | (e.mode << 3) | e.reg)); w((u16)bit); ea_ext(e, 1); }
  void bset_i(int bit, const Op& e) { w((u16)(0x08C0 | (e.mode << 3) | e.reg)); w((u16)bit); ea_ext(e, 1); }
  void bclr_i(int bit, const Op& e) { w((u16)(0x0880 | (e.mode << 3) | e.reg)); w((u16)bit); ea_ext(e, 1); }
  void btst_d(int dn, const Op& e) { w((u16)(0x0100 | (dn << 9) | (e.mode << 3) | e.reg)); ea_ext(e, 1); }
  // ---- fluxo ----
  void bcc(int cc, const std::string& lbl) { w((u16)(0x6000 | (cc << 8))); fix16(lbl); }
  void bra(const std::string& l_) { bcc(0, l_); }
  void bsr(const std::string& l_) { bcc(1, l_); }
  void bne(const std::string& l_) { bcc(6, l_); }
  void beq(const std::string& l_) { bcc(7, l_); }
  void blt(const std::string& l_) { bcc(13, l_); }
  void bge(const std::string& l_) { bcc(12, l_); }
  void bgt(const std::string& l_) { bcc(14, l_); }
  void ble(const std::string& l_) { bcc(15, l_); }
  void bhi(const std::string& l_) { bcc(2, l_); }
  void bcs(const std::string& l_) { bcc(5, l_); }
  void bpl(const std::string& l_) { bcc(10, l_); }
  void bmi(const std::string& l_) { bcc(11, l_); }
  void scc(int cc, const Op& e) { w((u16)(0x50C0 | (cc << 8) | (e.mode << 3) | e.reg)); ea_ext(e, 1); }
  void jsr(const Op& e) { w((u16)(0x4E80 | (e.mode << 3) | e.reg)); ea_ext(e, 4); }
  void jmp(const Op& e) { w((u16)(0x4EC0 | (e.mode << 3) | e.reg)); ea_ext(e, 4); }
  void rts() { w(0x4E75); }
  void rte() { w(0x4E73); }
  void nop() { w(0x4E71); }
  void halt() { w(0x4AC8); }
  void illegal() { w(0x4AFC); }
  void stop(u16 sr) { w(0x4E72); w(sr); }
  void trap(int n) { w((u16)(0x4E40 | n)); }
  void link(int an, int d) { w((u16)(0x4E50 | an)); w((u16)d); }
  void unlk(int an) { w((u16)(0x4E58 | an)); }
  void movec_vbr(int dn) { w(0x4E7B); w((u16)((dn << 12) | 0x801)); }
  void move_to_ccr(const Op& e) { w((u16)(0x44C0 | (e.mode << 3) | e.reg)); ea_ext(e, 2); }
  void move_to_sr(const Op& e) { w((u16)(0x46C0 | (e.mode << 3) | e.reg)); ea_ext(e, 2); }
  void move_from_sr(int dn) { w((u16)(0x40C0 | dn)); }
  // ---- privilegiadas / modo usuário ----
  void move_usp_from(int an) { w((u16)(0x4E60 | an)); }          // MOVE An,USP
  void move_usp_to(int an) { w((u16)(0x4E68 | an)); }            // MOVE USP,An
  void movec_to(int rc, bool is_an, int rn) { w(0x4E7B); w((u16)((is_an ? 0x8000 : 0) | (rn << 12) | rc)); }     // MOVEC Rn,Rc (0x800 OTHER_A7, 0x801 VBR, 0x002 CACR)
  void movec_from(int rc, bool is_an, int rn) { w(0x4E7A); w((u16)((is_an ? 0x8000 : 0) | (rn << 12) | rc)); }   // MOVEC Rc,Rn
  void fscc(int cond, const Op& d) { w((u16)(0xF240 | (d.mode << 3) | d.reg)); w((u16)cond); ea_ext(d, 1); }
  void fsave(const Op& d) { w((u16)(0xF300 | (d.mode << 3) | d.reg)); ea_ext(d, 4); }
  void frestore(const Op& s) { w((u16)(0xF340 | (s.mode << 3) | s.reg)); ea_ext(s, 4); }
  // ---- EMAC (CFPRM rev. 3). acc 0..3; ry/rx 0..15 (8-15 = An); uy/ux = metade alta (.U) quando word; sh: 0, +1 (<<1), -1 (>>1)
  // dual: 0 = MAC/MSAC; 1 = MAAAC (sub=false) / MSAAC (sub=true); 3 = MASAC (sub=false) / MSSAC (sub=true); accw = ACCw das duplas
  void mac_x(bool sub, int ry, int rx, int acc, bool word = false, bool uy = false, bool ux = false, int sh = 0, int dual = 0, int accw = 0) {
    w((u16)(0xA000 | ((rx & 7) << 9) | ((rx >> 3) << 6) | ((acc & 1) << 7) | ry));
    w((u16)((word ? 0 : 0x800) | ((sh > 0 ? 1 : sh < 0 ? 3 : 0) << 9) | (sub ? 0x100 : 0) | (ux ? 0x80 : 0) | (uy ? 0x40 : 0) | (((acc >> 1) & 1) << 4) | ((accw & 3) << 2) | dual));
  }
  // MAC/MSAC com carga: rw recebe <ea>; mask = ANDa o endereço com MASK; o operando de memória é lido em paralelo
  void mac_load(bool sub, int rw, const Op& src, int ry, int rx, int acc, bool word = false, bool uy = false, bool ux = false, int sh = 0, bool mask = false) {
    w((u16)(0xA000 | ((rw & 7) << 9) | ((rw >> 3) << 6) | (((acc & 1) ^ 1) << 7) | (src.mode << 3) | src.reg));
    w((u16)((rx << 12) | (word ? 0 : 0x800) | ((sh > 0 ? 1 : sh < 0 ? 3 : 0) << 9) | (sub ? 0x100 : 0) | (ux ? 0x80 : 0) | (uy ? 0x40 : 0) | (mask ? 0x20 : 0) | (((acc >> 1) & 1) << 4) | ry));
    ea_ext(src, 4);
  }
  void mac_set_acc(int acc, const Op& s) { w((u16)(0xA100 | (acc << 9) | (s.mode << 3) | s.reg)); ea_ext(s, 4); }   // MOVE.L <ea>,ACCn
  void mac_copy(int x, int y) { w((u16)(0xA110 | (x << 9) | y)); }                                                // MOVE.L ACCy,ACCx
  void mac_get(int acc, int rx) { w((u16)(0xA180 | (acc << 9) | rx)); }                                           // MOVE.L ACCn,Rx
  void mac_movclr(int acc, int rx) { w((u16)(0xA1C0 | (acc << 9) | rx)); }                                        // MOVCLR.L ACCn,Rx
  void mac_set_macsr(const Op& s) { w((u16)(0xA900 | (s.mode << 3) | s.reg)); ea_ext(s, 4); }                     // MOVE.L <ea>,MACSR
  void mac_get_macsr(int rx) { w((u16)(0xA980 | rx)); }                                                           // MOVE.L MACSR,Rx
  void mac_macsr_ccr() { w(0xA9C0); }                                                                              // MOVE.L MACSR,CCR
  void mac_set_mask(const Op& s) { w((u16)(0xAD00 | (s.mode << 3) | s.reg)); ea_ext(s, 4); }                      // MOVE.L <ea>,MASK
  void mac_get_mask(int rx) { w((u16)(0xAD80 | rx)); }                                                            // MOVE.L MASK,Rx
  void mac_set_accext(int pair, const Op& s) { w((u16)((pair ? 0xAF00 : 0xAB00) | (s.mode << 3) | s.reg)); ea_ext(s, 4); }   // pair 0: ACCext01; 1: ACCext23
  void mac_get_accext(int pair, int rx) { w((u16)((pair ? 0xAF80 : 0xAB80) | rx)); }
  // ---- nomes de compatibilidade usados pelos testes ----
  void mac_l(int ry, int dx) { mac_x(false, ry, dx, 0); }
  void msac_l(int ry, int dx) { mac_x(true, ry, dx, 0); }
  void mac_getacc(int acc, int dx) { mac_get(acc, dx); }
  void mac_setacc(int ry, int acc) { mac_set_acc(acc, D(ry)); }
  void mac_set(int ry) { mac_set_acc(0, D(ry)); }
  void mac_clr() { mac_set_acc(0, Imm(0)); }
  // ---- FPU (formatos: 0 L, 1 S, 4 W, 5 D, 6 B) ----
  void fop_rr(int opm, int src, int dst) { w(0xF200); w((u16)((src << 10) | (dst << 7) | opm)); }
  void fop_ea(int opm, int fmt, const Op& s, int dst) { w((u16)(0xF200 | (s.mode << 3) | s.reg)); w((u16)(0x4000 | (fmt << 10) | (dst << 7) | opm)); ea_ext(s, fmt == 5 ? 8 : fmt == 4 ? 2 : fmt == 6 ? 1 : 4); }
  void fstore(int fmt, int fpn, const Op& d) { w((u16)(0xF200 | (d.mode << 3) | d.reg)); w((u16)(0x6000 | (fmt << 10) | (fpn << 7))); ea_ext(d, fmt == 5 ? 8 : fmt == 4 ? 2 : fmt == 6 ? 1 : 4); }
  void fmove_to_ctrl(int sel, const Op& s) { w((u16)(0xF200 | (s.mode << 3) | s.reg)); w((u16)(0x8000 | (sel << 10))); ea_ext(s, 4); }     // sel: 4 FPCR, 2 FPSR, 1 FPIAR
  void fmove_from_ctrl(int sel, const Op& d) { w((u16)(0xF200 | (d.mode << 3) | d.reg)); w((u16)(0xA000 | (sel << 10))); ea_ext(d, 4); }
  void fmovem_load(const Op& s, u8 list) { w((u16)(0xF200 | (s.mode << 3) | s.reg)); w((u16)(0xD000 | list)); ea_ext(s, 8); }          // lista: bit 7 = FP0
  void fmovem_store(u8 list, const Op& d) { w((u16)(0xF200 | (d.mode << 3) | d.reg)); w((u16)(0xF000 | list)); ea_ext(d, 8); }
  void fbcc(int cond, const std::string& lbl) { w((u16)(0xF280 | cond)); fix16(lbl); }

  // Resolve rótulos e devolve os bytes.
  std::vector<u8> finish() {
    for (auto& f : fix_) {
      u32 t = label_addr(f.label);
      if (f.kind == 0) { i32 disp = (i32)t - (i32)(base_ + f.pos); if (disp < -32768 || disp > 32767) throw std::runtime_error("desvio fora de alcance: " + f.label); code_[f.pos] = (u8)(disp >> 8); code_[f.pos + 1] = (u8)disp; }
      else { wbe32(&code_[f.pos], t + (u32)f.add); }
    }
    fix_.clear(); return code_;
  }
  u32 base() const { return base_; }
  const std::map<std::string, u32>& symbols() const { return labels_; }

 private:
  struct Fix { u32 pos; std::string label; int kind; i32 add = 0; };   // kind 0: disp16 (base = próprio endereço da palavra), 1: abs32
  u32 base_; std::vector<u8> code_; std::map<std::string, u32> labels_; std::vector<Fix> fix_;
  static int szb(int size) { return size == 1 ? 0 : size == 2 ? 1 : 2; }
  void un(u16 base, int size, const Op& e) { w((u16)(base | (szb(size) << 6) | (e.mode << 3) | e.reg)); ea_ext(e, size); }
  void fix16(const std::string& lbl) { fix_.push_back({(u32)code_.size(), lbl, 0}); w(0); }
  void ea_ext(const Op& o, int size) {
    if (o.imm) {
      if (!o.label.empty()) { fix_.push_back({(u32)code_.size(), o.label, 1}); l(0); return; }
      if (size == 4) l(o.immv); else w((u16)(size == 1 ? (o.immv & 0xFF) : o.immv));
      return;
    }
    if (!o.label.empty()) { fix_.push_back({(u32)code_.size(), o.label, 1}); l(0); return; }
    for (u16 x : o.ext) w(x);
  }
};

}  // namespace sf
