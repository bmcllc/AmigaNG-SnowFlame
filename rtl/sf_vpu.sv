// SnowVector-128 em SystemVerilog: unidade SIMD FP32 de 4 vias (32 registradores vetoriais, 8 escalares), fila de 8 jobs,
// DMA local (DMEM <-> barramento, pelo árbitro de memória) e SIGNAL (V() num semáforo do SnowBridge no ciclo da instrução).
// Mesmo comportamento de src/vpu.cpp: cada job executa no kick e conclui após os ciclos dele (150 MHz).
// IMEM (8 KiB) e DMEM (64 KiB) são memórias locais realizadas pelo Memory Host e vistas nas janelas do mapa (VPUx_IMEM/DMEM).
module sf_vpu;
  import fabric::*; import sf100_abi::*;
  localparam u32_t NWORDS = VPU_IMEM_SIZE / 4;
  localparam longint unsigned WATCHDOG = 4000000, HZ = 150000000;
  localparam f32_t F_1 = 32'h3f800000;
  // opcodes (vop:: de src/vpu.h)
  localparam u32_t HALT = 32'h00, VADD = 32'h01, VSUB = 32'h02, VMUL = 32'h03, VMAC = 32'h04, VDOT3 = 32'h05, VMIN = 32'h06, VMAX = 32'h07,
                   VLD = 32'h08, VST = 32'h09, SETA = 32'h0A, SADD = 32'h0B, SCMP = 32'h0C, BRLT = 32'h0D, BRNE = 32'h0E, BRA = 32'h0F,
                   VMOV = 32'h10, VRCP = 32'h11, VRSQ = 32'h12, VABS = 32'h13, VDOT4 = 32'h14, NOP = 32'h15, DMAIN = 32'h16, DMAOUT = 32'h17,
                   DMAWAIT = 32'h18, SIGNAL = 32'h19, VFRC = 32'h1A;
  localparam int EV_SIGNAL = 0, EV_DONE = 1;
  typedef struct { u32_t pc; u32_t s[8]; bit irq; } job_t;

  chandle h; int dev; u32_t imem, dmem, irq_bit;
  f32_t vr[32][4]; u32_t sr[8];
  job_t q[$]; bit running, done, faulted; longint unsigned gen;
  u32_t next_pc, next_s[8], fault_pc;
  longint unsigned last_cycles, last_flops, total_cycles, total_flops, jobs_done, dma_bytes, dma_xfers, signals;
  // resultado do último run_now
  longint unsigned r_cycles, r_flops, r_dma_bytes, r_dma_xfers; bit r_fault; u32_t r_pc;
  longint unsigned sig_cyc[$]; u32_t sig_n[$];

  export "DPI-C" function vpu_bind;
  export "DPI-C" function vpu_read;
  export "DPI-C" function vpu_write;
  export "DPI-C" function vpu_event;
  export "DPI-C" function vpu_submit;
  export "DPI-C" function vpu_status;
  export "DPI-C" function vpu_reset;
  export "DPI-C" function vpu_run_now;
  export "DPI-C" function vpu_result;
  export "DPI-C" function vpu_vreg;
  export "DPI-C" function vpu_sreg;

  function automatic void vpu_bind(chandle hh, int d, u32_t im, u32_t dm, u32_t irq);
    h = hh; dev = d; imem = im; dmem = dm; irq_bit = irq;
    next_pc = 0; fault_pc = 0; for (int i = 0; i < 8; i++) next_s[i] = 0;
    last_cycles = 0; last_flops = 0; total_cycles = 0; total_flops = 0; jobs_done = 0; dma_bytes = 0; dma_xfers = 0; signals = 0;
    gen = 0; vpu_reset();
  endfunction
  // RESET de estado (registradores, fila); IMEM/DMEM são zeradas pelo frontend (memória do hospedeiro)
  function automatic void vpu_reset();
    for (int i = 0; i < 32; i++) for (int l = 0; l < 4; l++) vr[i][l] = 0;
    for (int i = 0; i < 8; i++) sr[i] = 0;
    q.delete(); running = 0; done = 0; faulted = 0; gen++; obs();
  endfunction

  // Espelhos: 0 last_cycles 1 last_flops 2 total_cycles 3 total_flops 4 jobs_done 5 dma_bytes 6 dma_xfers 7 signals 8 fault_pc 9 running
  function automatic void obs();
    fab_obs(h, dev, 0, last_cycles); fab_obs(h, dev, 1, last_flops); fab_obs(h, dev, 2, total_cycles); fab_obs(h, dev, 3, total_flops);
    fab_obs(h, dev, 4, jobs_done); fab_obs(h, dev, 5, dma_bytes); fab_obs(h, dev, 6, dma_xfers); fab_obs(h, dev, 7, signals);
    fab_obs(h, dev, 8, 64'(fault_pc)); fab_obs(h, dev, 9, 64'(running));
  endfunction
  function automatic u32_t vpu_status();
    return (running ? VPUS_BUSY : 0) | (done ? VPUS_DONE : 0) | (faulted ? VPUS_FAULT : 0) | (u32_t'(q.size()) << 8);
  endfunction
  function automatic u32_t vpu_vreg(int r, int l); return vr[r & 31][l & 3]; endfunction
  function automatic u32_t vpu_sreg(int r); return sr[r & 7]; endfunction

  function automatic ps_t cyc2ps(longint unsigned c); return ps_t'((128'(c) * 128'(64'd1000000000000)) / 128'(HZ)); endfunction

  // ---------------- Núcleo de execução ----------------
  function automatic void swz4(int c, u32_t swz, output f32_t o[4]);
    for (int i = 0; i < 4; i++) o[i] = vr[c][(swz >> (2 * i)) & 3];
  endfunction

  function automatic void vpu_run_now(u32_t pc0, u32_t s0, u32_t s1, u32_t s2, u32_t s3, u32_t s4, u32_t s5, u32_t s6, u32_t s7);
    u32_t pc, w, op, a, b, c, swz, cur, addr; int imm; bit lt, eq; f32_t vb[4], t[4], dd;
    ps_t t0, dma_end; longint unsigned dma_done;
    r_cycles = 0; r_flops = 0; r_dma_bytes = 0; r_dma_xfers = 0; r_fault = 0; r_pc = 0; sig_cyc.delete(); sig_n.delete();
    sr[0] = s0; sr[1] = s1; sr[2] = s2; sr[3] = s3; sr[4] = s4; sr[5] = s5; sr[6] = s6; sr[7] = s7;
    pc = pc0; lt = 0; eq = 0;
    t0 = fab_now(h); dma_end = t0; dma_done = 0;
    forever begin
      if (pc >= NWORDS) begin r_fault = 1; r_pc = pc; break; end
      if (r_cycles >= WATCHDOG) begin r_fault = 1; r_pc = pc; break; end
      w = fab_r32(h, imem + pc * 4);
      op = w >> 26; a = (w >> 21) & 31; b = (w >> 16) & 31; c = (w >> 11) & 31; swz = w & 32'hFF; imm = int'(shortint'(w[15:0]));
      cur = pc; pc++;
      r_cycles++;
      case (op)
        HALT: begin if (dma_done > r_cycles) r_cycles = dma_done; return; end     // o job só termina com o DMA concluído
        NOP: ;
        VADD, VSUB, VMUL, VMAC, VMIN, VMAX: begin
          swz4(int'(c), swz, vb);
          for (int i = 0; i < 4; i++)
            case (op)
              VADD: t[i] = f32_add(vr[b][i], vb[i]);
              VSUB: t[i] = f32_sub(vr[b][i], vb[i]);
              VMUL: t[i] = f32_mul(vr[b][i], vb[i]);
              VMAC: t[i] = f32_add(vr[a][i], f32_mul(vr[b][i], vb[i]));
              VMIN: t[i] = f32_lt(vr[b][i], vb[i]) ? vr[b][i] : vb[i];
              default: t[i] = f32_gt(vr[b][i], vb[i]) ? vr[b][i] : vb[i];
            endcase
          for (int i = 0; i < 4; i++) vr[a][i] = t[i];
          r_flops += op == VMAC ? 8 : 4;
        end
        VDOT3, VDOT4: begin
          swz4(int'(c), swz, vb);
          dd = f32_add(f32_add(f32_mul(vr[b][0], vb[0]), f32_mul(vr[b][1], vb[1])), f32_mul(vr[b][2], vb[2]));
          if (op == VDOT4) begin dd = f32_add(dd, f32_mul(vr[b][3], vb[3])); r_flops += 7; end else r_flops += 5;
          for (int i = 0; i < 4; i++) vr[a][i] = dd;
        end
        VMOV, VRCP, VRSQ, VABS, VFRC: begin
          swz4(int'(c), swz, vb);
          for (int i = 0; i < 4; i++)
            case (op)
              VMOV: t[i] = vb[i];
              VRCP: t[i] = f32_div(F_1, vb[i]);
              VRSQ: t[i] = f32_div(F_1, f32_sqrt(vb[i]));
              VFRC: t[i] = f32_sub(vb[i], f32_floor(vb[i]));
              default: t[i] = f32_abs(vb[i]);
            endcase
          for (int i = 0; i < 4; i++) vr[a][i] = t[i];
          if (op != VMOV) r_flops += 4;
        end
        VLD, VST: begin
          addr = sr[b & 7] + u32_t'(imm * 16);
          if ((addr & 15) != 0 || addr + 16 > VPU_DMEM_SIZE) begin r_fault = 1; r_pc = cur; return; end
          if (op == VLD) for (int i = 0; i < 4; i++) vr[a][i] = fab_r32(h, dmem + addr + 4 * i);
          else for (int i = 0; i < 4; i++) fab_w32(h, dmem + addr + 4 * i, vr[a][i]);
        end
        DMAIN: if (!dma(sr[a & 7], sr[b & 7], sr[c & 7], 1, t0, dma_end, dma_done)) begin r_fault = 1; r_pc = cur; return; end
        DMAOUT: if (!dma(sr[b & 7], sr[a & 7], sr[c & 7], 0, t0, dma_end, dma_done)) begin r_fault = 1; r_pc = cur; return; end
        DMAWAIT: if (dma_done > r_cycles) r_cycles = dma_done;
        SIGNAL: begin sig_cyc.push_back(r_cycles); sig_n.push_back(a & 15); end
        SETA: sr[a & 7] = u32_t'(imm);
        SADD: sr[a & 7] = sr[b & 7] + u32_t'(imm);
        SCMP: begin lt = int'(sr[a & 7]) < int'(sr[b & 7]); eq = sr[a & 7] == sr[b & 7]; end
        BRLT: if (lt) pc = u32_t'(int'(pc) + imm);
        BRNE: if (!eq) pc = u32_t'(int'(pc) + imm);
        BRA: pc = u32_t'(int'(pc) + imm);
        default: begin r_fault = 1; r_pc = cur; return; end
      endcase
    end
  endfunction

  // DMA local: cópia funcional no instante da instrução; o término (em ciclos do job) vem do árbitro de memória.
  function automatic bit dma(u32_t dm, u32_t ext, u32_t len, bit in_, ps_t t0, inout ps_t dma_end, inout longint unsigned dma_done);
    ps_t st, c;
    if (((dm | ext | len) & 3) != 0 || 64'(dm) + 64'(len) > 64'(VPU_DMEM_SIZE)) return 0;
    if (len != 0 && !fab_ok(h, ext, len)) return 0;
    if (in_) fab_copy(h, dmem + dm, ext, len, 0); else fab_copy(h, ext, dmem + dm, len, 0);
    st = t0 + cyc2ps(r_cycles + VPU_DMA_SETUP); if (dma_end > st) st = dma_end;
    dma_end = fab_reserve(h, ext, 64'(len), st);
    c = (dma_end - t0);
    dma_done = 64'((128'(c) * 128'(HZ) + 128'(64'd999999999999)) / 128'(64'd1000000000000));
    r_dma_bytes += 64'(len); r_dma_xfers++;
    return 1;
  endfunction

  // Resultado do último run_now: 0 ciclos 1 flops 2 bytes de DMA 3 transferências 4 falha 5 pc da falha 6 nº de SIGNALs;
  // 16+2i / 17+2i: ciclo / semáforo do SIGNAL i
  function automatic longint unsigned vpu_result(int k);
    case (k)
      0: return r_cycles; 1: return r_flops; 2: return r_dma_bytes; 3: return r_dma_xfers; 4: return 64'(r_fault); 5: return 64'(r_pc);
      6: return 64'(sig_cyc.size());
      default: return (k & 1) == 0 ? sig_cyc[(k - 16) / 2] : 64'(sig_n[(k - 17) / 2]);
    endcase
  endfunction

  // ---------------- Fila de jobs / MMIO ----------------
  function automatic bit vpu_submit(u32_t pc, u32_t s0, u32_t s1, u32_t s2, u32_t s3, u32_t s4, u32_t s5, u32_t s6, u32_t s7, bit irq);
    job_t j;
    if (q.size() + (running ? 1 : 0) >= VPU_QUEUE_DEPTH) return 0;
    j.pc = pc; j.s[0] = s0; j.s[1] = s1; j.s[2] = s2; j.s[3] = s3; j.s[4] = s4; j.s[5] = s5; j.s[6] = s6; j.s[7] = s7; j.irq = irq;
    q.push_back(j); done = 0;
    if (!running) kick();
    obs();
    return 1;
  endfunction

  function automatic void kick();
    job_t j;
    if (q.size() == 0) begin running = 0; return; end
    j = q.pop_front();
    running = 1;
    vpu_run_now(j.pc, j.s[0], j.s[1], j.s[2], j.s[3], j.s[4], j.s[5], j.s[6], j.s[7]);
    last_cycles = r_cycles; last_flops = r_flops; total_cycles += r_cycles; total_flops += r_flops;
    dma_bytes += r_dma_bytes; dma_xfers += r_dma_xfers;
    if (r_fault) begin faulted = 1; fault_pc = r_pc; end
    for (int i = 0; i < sig_cyc.size(); i++) fab_at(h, dev, cyc2ps(sig_cyc[i]), EV_SIGNAL, gen, 64'(sig_n[i]));
    fab_at(h, dev, cyc2ps(r_cycles > 1 ? r_cycles : 1), EV_DONE, gen, 64'(j.irq));
  endfunction

  function automatic void vpu_event(int tag, longint unsigned g, longint unsigned arg);
    if (g != gen) return;                           // job cancelado por RESET
    if (tag == EV_SIGNAL) begin
      fab_set_busreg(h, BUS_SEM0 + int'(arg), fab_busreg(h, BUS_SEM0 + int'(arg)) + 1); signals++;
    end else begin
      jobs_done++; running = 0;
      if (arg != 0) fab_irq(h, irq_bit);
      if (q.size() != 0) kick(); else done = 1;
    end
    obs();
  endfunction

  function automatic u32_t vpu_read(u32_t off);
    case (off)
      VPU_STATUS: return vpu_status();
      VPU_PC: return next_pc;
      VPU_CYCLES: return u32_t'(last_cycles);
      VPU_FLOPS: return u32_t'(last_flops);
      VPU_FAULTPC: return fault_pc;
      VPU_TOTAL_CYCLES: return u32_t'(total_cycles);
      VPU_DMA_BYTES: return u32_t'(dma_bytes);
      VPU_JOBS_DONE: return u32_t'(jobs_done);
      default: ;
    endcase
    if (off >= VPU_S0 && off < VPU_S0 + 32) return next_s[(off - VPU_S0) / 4];
    return 0;
  endfunction
  function automatic void vpu_write(u32_t off, u32_t val);
    if (off == VPU_PC) begin next_pc = val; return; end
    if (off >= VPU_S0 && off < VPU_S0 + 32) begin next_s[(off - VPU_S0) / 4] = val; return; end
    if (off == VPU_STATUS) begin if ((val & VPUS_FAULT) != 0) faulted = 0; return; end
    if (off == VPU_CTRL) begin
      if ((val & VPUC_RESET) != 0) begin q.delete(); running = 0; done = 0; faulted = 0; gen++; end
      if ((val & VPUC_START) != 0) begin
        if (!vpu_submit(next_pc, next_s[0], next_s[1], next_s[2], next_s[3], next_s[4], next_s[5], next_s[6], next_s[7], (val & VPUC_IRQ) != 0)) faulted = 1;
      end
      obs();
    end
  endfunction
endmodule
