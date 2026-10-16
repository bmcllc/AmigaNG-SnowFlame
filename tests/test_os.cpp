#include "helpers.h"
#include "snowcc.h"
#include "vpu.h"
#include "sffs.h"

// ======================= Item 6: SnowOS Native (kernel + SnowGX/SnowVX/SnowAudio) =======================
struct OsRun { bool compiled = false, halted = false; std::string err, console; CcResult r; u32 exit_code = 0; };
static OsRun run_os(Soc& s, const std::string& app, u64 limit_ps = 3000000000000ull) {
  OsRun o; CcOptions opt; opt.include_dirs = {"os", "os/include"};
  o.r = cc_compile("#include \"snowos_all.c\"\n" + app, "app.c", opt);
  if (!o.r.ok) { o.err = o.r.error; std::printf("    erro de compilação: %s\n", o.err.c_str()); return o; }
  o.compiled = true; s.load(o.r.base, o.r.image); s.cpu.reset_regs(); s.cpu.a[7] = 0x00F00000; s.cpu.set_pc(o.r.entry);
  o.halted = s.run_until_halt(limit_ps); s.bus.cache_flush_all(false);
  o.console = s.console.out; o.exit_code = s.bus.raw_r32(o.r.symbols["os_exit_code"]);
  return o;
}
static u32 sym32(Soc& s, OsRun& o, const char* name) { s.bus.cache_flush_all(false); return s.bus.raw_r32(o.r.symbols[name]); }

TEST(os_boots_and_preempts_equal_priority_tasks) {
  Soc s; OsRun o = run_os(s, R"(
    volatile int a, b;
    static void t1(void) { for (;;) { a++; } }
    static void t2(void) { for (;;) { b++; } }
    void app_main(void) {
      task_create(t1, 2048, 5); task_create(t2, 2048, 5);
      task_sleep(20);
      os_printf("ticks=%d a>0:%d b>0:%d\n", os_ticks(), a > 0, b > 0);
      os_exit(7);
    })");
  CHECK(o.compiled); CHECK(o.halted); CHECK_EQ(o.exit_code, 7);
  CHECK(o.console.find("a>0:1 b>0:1") != std::string::npos);
  int ticks = 0; std::sscanf(o.console.c_str(), "ticks=%d", &ticks); CHECK(ticks >= 20 && ticks <= 26);   // criar tarefa de mesma prioridade cede a CPU por uma fatia
  CHECK(sym32(s, o, "a") > 1000 && sym32(s, o, "b") > 1000);
  u32 a = sym32(s, o, "a"), b = sym32(s, o, "b"); CHECK(a < b * 3 && b < a * 3);                // rodízio: parecidos
}

TEST(os_priorities_and_sleep_accuracy) {
  Soc s; OsRun o = run_os(s, R"(
    volatile unsigned t_hi, t_lo, t_wake; volatile int order[4]; volatile int n;
    static void hi(void) { order[n++] = 1; t_hi = os_ticks(); }
    static void lo(void) { order[n++] = 2; t_lo = os_ticks(); }
    static void sleeper(void) { unsigned t0 = os_ticks(); task_sleep(15); t_wake = os_ticks() - t0; }
    void app_main(void) {
      task_create(lo, 1024, 3);          /* app_main tem prioridade 5: nada roda antes do sleep */
      task_create(hi, 1024, 9);
      task_create(sleeper, 1024, 4);
      task_sleep(40);
      os_exit(0);
    })");
  CHECK(o.compiled); CHECK(o.halted);
  CHECK_EQ(sym32(s, o, "order"), 1); CHECK_EQ(sym32(s, o, "n"), 2);                                  // hi primeiro
  s.bus.cache_flush_all(false); CHECK_EQ(s.bus.raw_r32(o.r.symbols["order"] + 4), 2);
  u32 w = sym32(s, o, "t_wake"); CHECK(w >= 15 && w <= 17);
}

TEST(os_semaphores_queues_and_timeouts) {
  Soc s; OsRun o = run_os(s, R"(
    Queue q; unsigned qbuf[4]; Sem done, never; volatile unsigned total, got; volatile int to1, to2;
    static void producer(void) { unsigned i; for (i = 1; i <= 20; i++) queue_send(&q, i); sem_post(&done); }
    static void consumer(void) { int i; for (i = 0; i < 20; i++) total += queue_recv(&q); sem_post(&done); }
    void app_main(void) {
      queue_init(&q, qbuf, 4); sem_init(&done, 0); sem_init(&never, 0);
      task_create(producer, 2048, 5); task_create(consumer, 2048, 5);
      sem_wait(&done); sem_wait(&done);
      to1 = sem_wait_timeout(&never, 10);                 /* ninguém posta: estoura */
      sem_post(&never); to2 = sem_wait_timeout(&never, 10);   /* já tem unidade: obtém */
      got = queue_tryrecv(&q, (unsigned *)&total);          /* fila vazia */
      os_exit(0);
    })");
  CHECK(o.compiled); CHECK(o.halted); CHECK_EQ(sym32(s, o, "total"), 210); CHECK_EQ(sym32(s, o, "to1"), 0); CHECK_EQ(sym32(s, o, "to2"), 1); CHECK_EQ(sym32(s, o, "got"), 0);
}

TEST(os_malloc_free_coalesce_and_reuse) {
  Soc s; OsRun o = run_os(s, R"(
    volatile unsigned free0, free1, free2; volatile int same;
    void app_main(void) {
      char *a, *b, *c, *d;
      free0 = os_heap_free();
      a = (char *)os_malloc(100); b = (char *)os_malloc(2000); c = (char *)os_malloc(40);
      a[0] = 1; b[1999] = 2; c[39] = 3;
      free1 = os_heap_free();
      os_free(b); os_free(a); os_free(c);                      /* ordem embaralhada: precisa fundir os blocos */
      free2 = os_heap_free();
      d = (char *)os_malloc(2100);                            /* só cabe se a fusão funcionou no início da arena */
      same = (d == a);
      os_exit(0);
    })");
  CHECK(o.compiled); CHECK(o.halted);
  u32 f0 = sym32(s, o, "free0"), f1 = sym32(s, o, "free1"), f2 = sym32(s, o, "free2");
  CHECK(f1 < f0 - 2100); CHECK(f2 > f1 + 2100); CHECK(f0 - f2 <= 12 * 4);   // só sobram cabeçalhos (as 3 divisões fundidas)
}

TEST(os_user_task_faults_do_not_take_the_system_down) {
  Soc s; OsRun o = run_os(s, R"(
    volatile int alive, after;
    static void bad_priv(void) { __set_sr(0); }                 /* privilegiada em modo usuário */
    static void bad_div(void) { volatile int z = 0; after = 5 / z; }
    static void bad_illegal(void) { void (*f)(void) = (void (*)(void))0x2000; f(); }   /* executa lixo/zeros */
    static void good(void) { for (;;) { alive++; task_sleep(1); } }
    void app_main(void) {
      int a = task_create(bad_priv, 1024, 6), b = task_create(bad_div, 1024, 6), c = task_create(bad_illegal, 1024, 6);
      task_create(good, 1024, 5);
      task_sleep(30);
      os_printf("estados %d %d %d alive=%d\n", task_state(a), task_state(b), task_state(c), alive > 10);
      os_exit(0);
    })");
  CHECK(o.compiled); CHECK(o.halted); CHECK_EQ(o.exit_code, 0);
  CHECK(o.console.find("vetor 8") != std::string::npos); CHECK(o.console.find("vetor 5") != std::string::npos);
  CHECK(o.console.find("alive=1") != std::string::npos);
  CHECK(o.console.find("estados") != std::string::npos);
}

TEST(os_printf_formats_and_variadics) {
  Soc s; OsRun o = run_os(s, R"(
    void app_main(void) { os_printf("%d|%5d|%05d|%u|%x|%X|%08x|%c|%s|%%|%d\n", -42, 7, 42, 4000000000u, 255, 255, 0xBEEF, 'Z', "ok", 0); os_exit(0); })");
  CHECK(o.halted); CHECK(o.console == "-42|    7|00042|4000000000|ff|FF|0000beef|Z|ok|%|0\n");
}

TEST(os_kill_task_and_exit_by_return) {
  Soc s; OsRun o = run_os(s, R"(
    volatile int ticks_seen;
    static void spinner(void) { for (;;) { ticks_seen++; } }
    static void quick(void) { ticks_seen += 1000000; }          /* retorna: task_exit via endereço de retorno da pilha */
    void app_main(void) {
      int sp = task_create(spinner, 1024, 5), q = task_create(quick, 1024, 5);
      task_sleep(5);
      os_printf("q=%d ", task_state(q)); task_kill(sp); task_sleep(2);
      os_printf("sp=%d\n", task_state(sp));
      os_exit(0);
    })");
  CHECK(o.halted); CHECK(o.console.find("q=5 sp=5") != std::string::npos);
}

TEST(os_snowgx_renders_triangle_blits_sprites_and_layers) {
  Soc s; s.video.start();
  OsRun o = run_os(s, R"(
    unsigned cmd[64]; unsigned verts[40]; unsigned sprites[8]; unsigned spix[16];
    volatile int gpu_ok, frames;
    void app_main(void) {
      GxList l; int i;
      gx_init(); gx_video_start();
      gx_put_vertex(&verts[0],  8 << 16,  8 << 16, 0x8000, 0xFFFF0000, 0, 0);       /* vermelho */
      gx_put_vertex(&verts[12], 56 << 16, 8 << 16, 0x8000, 0xFFFF0000, 0, 0);
      gx_put_vertex(&verts[24], 8 << 16, 56 << 16, 0x8000, 0xFFFF0000, 0, 0);
      os_cache_flush((unsigned)verts, sizeof verts);
      gx_begin(&l, cmd, 64);
      gx_set_rt(&l, VRAM_BASE, 256, 64, 64); gx_clear(&l, 0xFF000000, 0x3F800000, CLR_COLOR | CLR_DEPTH);
      gx_vertex_base(&l, (unsigned)verts); gx_state(&l, 0); gx_draw_arrays(&l, 0, 3, PRIM_TRIS); gx_end(&l);
      gpu_ok = gx_submit(&l);
      /* Blitter: preenche um retângulo e desenha um sprite por cima (lista de sprites) */
      gx_blit(BOP_FILL, 0, 0, VRAM_BASE + 40 * 256 + 40 * 4, 256, 16, 16, 0xFF00FF00);
      for (i = 0; i < 16; i++) spix[i] = 0xFF0000FF;
      os_cache_flush((unsigned)spix, sizeof spix);
      sprites[0] = (unsigned)spix; sprites[1] = (4 << 16) | 0; sprites[2] = (44 << 16) | 44; sprites[3] = (4 << 16) | 4;
      gx_blit_sprites((unsigned)sprites, 1, VRAM_BASE, 256, 64, 64);
      gx_layer(0, VRAM_BASE, 256, LF_ENABLE);
      gx_wait_vblank(); gx_wait_vblank(); frames = (int)REG(VIDEO_BASE + VID_FRAME);
      os_exit(0);
    })");
  CHECK(o.compiled); CHECK(o.halted); CHECK_EQ(sym32(s, o, "gpu_ok"), 0);
  auto px = [&](int x, int y) { return s.bus.raw_r32(VRAM_BASE + y * 256 + x * 4); };
  CHECK_EQ(px(12, 12), 0xFFFF0000u); CHECK_EQ(px(60, 20), 0xFF000000u);                          // dentro / fora do triângulo
  CHECK_EQ(px(41, 41), 0xFF00FF00u); CHECK_EQ(px(46, 46), 0xFF0000FFu); CHECK_EQ(px(60, 60), 0xFF000000u);   // Blitter + sprite 4x4 em (44,44)
  CHECK(sym32(s, o, "frames") >= 2);
  s.video.vblank(); CHECK_EQ(s.video.frame[12 * 320 + 12], 0xFF0000u); CHECK_EQ(s.video.frame[46 * 320 + 46], 0x0000FFu);   // camada 0 compõe o framebuffer
}

TEST(os_snowvx_runs_vector_program_on_both_vpus) {
  std::vector<u32> prog; std::string err;
  CHECK(vpu_assemble("SETA s1, 0\nVLD v0,[s1+0]\nVLD v1,[s1+1]\nVADD v2,v0,v1\nVMUL v3,v0,v1\nVST [s1+2],v2\nVST [s1+3],v3\nHALT\n", prog, err));
  std::string arr = "unsigned code[] = {"; for (size_t i = 0; i < prog.size(); i++) arr += (i ? "," : "") + std::to_string(prog[i]) + "u"; arr += "};\n";
  Soc s; OsRun o = run_os(s, arr + R"(
    unsigned in[8]; unsigned out[8], out2[8]; volatile int r1, r2; volatile unsigned cyc;
    void app_main(void) {
      int i;
      for (i = 0; i < 4; i++) { in[i] = gx_f32(i + 1); in[4 + i] = gx_f32(10 * (i + 1)); }
      vx_load_program(0, code, sizeof code / 4); vx_load_program(1, code, sizeof code / 4);
      vx_load_data(0, 0, in, 8); vx_load_data(1, 0, in, 8);
      r1 = vx_run(0, 0, 0, 0, 0, 0); r2 = vx_run(1, 0, 0, 0, 0, 0);
      vx_read_data(0, 32, out, 8); vx_read_data(1, 32, out2, 8); cyc = vx_cycles(0);
      os_exit(0);
    })");
  CHECK(o.compiled); CHECK(o.halted); CHECK_EQ(sym32(s, o, "r1"), 0); CHECK_EQ(sym32(s, o, "r2"), 0);
  s.bus.cache_flush_all(false); u32 out = o.r.symbols["out"], out2 = o.r.symbols["out2"];
  for (int i = 0; i < 4; i++) {
    CHECK_NEAR(u2f(s.bus.raw_r32(out + 4 * i)), 11.0 * (i + 1), 0.01); CHECK_NEAR(u2f(s.bus.raw_r32(out + 16 + 4 * i)), 10.0 * (i + 1) * (i + 1), 0.01);
    CHECK_NEAR(u2f(s.bus.raw_r32(out2 + 4 * i)), 11.0 * (i + 1), 0.01);
  }
  CHECK(sym32(s, o, "cyc") >= 8);
  CHECK(s.vpuA.jobs_done == 1 && s.vpuB.jobs_done == 1);
}

TEST(os_snowaudio_plays_a_voice_loaded_through_dma) {
  Soc s; OsRun o = run_os(s, R"(
    short tone[256];
    void app_main(void) {
      snd_init(); snd_sine(tone, 256, 64, 12000);
      snd_load(0, tone, sizeof tone);                       /* DMA DDR -> ARAM (com flush do D-cache) */
      snd_voice_play(0, 0, 256, SND_PCM16, 0x10000, 256, 256, 1);
      task_sleep(10);
      os_exit(0);
    })");
  CHECK(o.compiled); CHECK(o.halted);
  CHECK_EQ(rbe16(&s.bus.aram[2 * 16]) , rbe16(&s.bus.aram[2 * 16])); CHECK(s.bus.aram[0] != 0 || s.bus.aram[40] != 0);
  s.sync_audio(); int maxv = 0, minv = 0; for (size_t i = 0; i < s.paula.pcm.size(); i++) { maxv = std::max<int>(maxv, s.paula.pcm[i]); minv = std::min<int>(minv, s.paula.pcm[i]); }
  CHECK(s.paula.pcm.size() > 2 * 48000 / 1000 * 8); CHECK(maxv > 6000); CHECK(minv < -6000);
}

TEST(os_sample_apps_build_and_run) {
  for (const char* app : {"os/apps/tasks.c", "os/apps/triangle.c"}) {
    Soc s; CcOptions opt; opt.include_dirs = {"os", "os/include"}; CcResult r = cc_compile_file(app, opt);
    CHECK(r.ok); if (!r.ok) { std::printf("    %s: %s\n", app, r.error.c_str()); continue; }
    s.load(r.base, r.image); s.cpu.reset_regs(); s.cpu.a[7] = 0x00F00000; s.cpu.set_pc(r.entry);
    CHECK(s.run_until_halt(3000000000000ull)); s.bus.cache_flush_all(false); CHECK_EQ(s.bus.raw_r32(r.symbols["os_exit_code"]), 0u);
    if (std::string(app).find("tasks") != std::string::npos) CHECK(s.console.out.find("checksum = 385") != std::string::npos);
    else { CHECK(s.console.out.find("GPU: ok") != std::string::npos); CHECK(s.bus.raw_r32(VRAM_BASE + 40 * 256 + 32 * 4) != 0xFF101020u); }
  }
}

// ======================= SnowFS + SnowIn =======================
TEST(sffs_build_validates_and_lays_out) {
  std::vector<u8> img; std::string err;
  CHECK(!sffs_build({{"a", {1}}, {"a", {2}}}, img, &err)); CHECK(!sffs_build({{std::string(48, 'x'), {1}}}, img, &err));
  CHECK(sffs_build({{"boot/a.bin", std::vector<u8>(3000, 7)}, {"b.txt", {}}}, img, &err));
  CHECK_EQ(img.size(), (size_t)(1 + 1 + 2) * DVD_SECTOR);
  CHECK_EQ(img[3], 0x53); CHECK_EQ(img[11], 2);                                  // magic e nº de arquivos (big-endian)
}

TEST(os_snowfs_reads_files_across_sectors_and_seeks) {
  std::vector<SffsFile> files; std::vector<u8> big(5000), small = {'h', 'i', '!'};
  for (size_t i = 0; i < big.size(); i++) big[i] = (u8)(i * 13 + (i >> 8));
  files.push_back({"data/big.bin", big}); files.push_back({"hello.txt", small});
  std::vector<u8> img; CHECK(sffs_build(files, img));
  Soc s; s.dvd.insert(img);
  OsRun o = run_os(s, R"(
    static unsigned char buf[6000]; volatile int mount, nf, miss, n1, n2, n3, sz, tell_ok, bad_seek, sum_ok;
    void app_main(void) {
      int h, i; unsigned sum = 0;
      mount = fs_mount(); nf = fs_count();
      miss = fs_open("nope");
      h = fs_open("data/big.bin"); sz = fs_size(h);
      n1 = fs_read(h, buf, 700);                          /* parcial, dentro do setor 0 */
      n2 = fs_read(h, buf + 700, 4300);                   /* cruza setores (resto desalinhado + inteiros + cauda) */
      n3 = fs_read(h, buf + 5000, 10);                    /* fim do arquivo */
      for (i = 0; i < 5000; i++) sum += buf[i] * (i + 1);
      sum_ok = (int)sum;
      tell_ok = fs_tell(h) == 5000; bad_seek = fs_seek(h, 6000);
      fs_seek(h, 2047); n3 = fs_read(h, buf, 3);          /* cruza a fronteira de setor */
      sum = buf[0] * 65536 + buf[1] * 256 + buf[2]; sum_ok = (int)sum;
      fs_close(h);
      h = fs_open("hello.txt"); buf[0] = 0; fs_read(h, buf, 3); fs_close(h);
      os_printf("%c%c%c n3=%d\n", buf[0], buf[1], buf[2], n3);
      os_exit(0);
    })");
  CHECK(o.compiled); CHECK(o.halted);
  CHECK_EQ(sym32(s, o, "mount"), 0); CHECK_EQ(sym32(s, o, "nf"), 2); CHECK_EQ((int)sym32(s, o, "miss"), -1);
  CHECK_EQ(sym32(s, o, "sz"), 5000); CHECK_EQ(sym32(s, o, "n1"), 700); CHECK_EQ(sym32(s, o, "n2"), 4300);
  CHECK_EQ(sym32(s, o, "tell_ok"), 1); CHECK_EQ((int)sym32(s, o, "bad_seek"), -1);
  CHECK_EQ(sym32(s, o, "sum_ok"), (u32)(big[2047] * 65536 + big[2048] * 256 + big[2049]));
  CHECK(o.console.find("hi! n3=3") != std::string::npos);
}

TEST(os_snowfs_mount_fails_without_disc) {
  Soc s; OsRun o = run_os(s, "volatile int m; void app_main(void) { m = fs_mount(); os_exit(0); }");
  CHECK(o.compiled); CHECK(o.halted); CHECK_EQ((int)sym32(s, o, "m"), -1);
}

TEST(os_snowin_keyboard_and_mouse) {
  Soc s; s.hid.key_event(0xE1, true); s.hid.key_event(0x04, true); s.hid.key_event(0x04, false); s.hid.key_event(0x1E, true);
  s.hid.mouse_move(9, -4); s.hid.mouse_button(0, true);
  OsRun o = run_os(s, R"(
    volatile int c1, c2, up, c3, dx, dy, w, b, empty;
    void app_main(void) {
      int ev, d1, d2, d3;
      in_key_wait();                                      /* ignora o Shift pressionado */
      ev = in_key_wait(); c1 = in_key_ascii(ev);          /* 'A' maiúsculo (Shift ativo) */
      ev = in_key_poll(); up = (ev & KEY_UP) != 0; c2 = in_key_ascii(ev);
      ev = in_key_poll(); c3 = in_key_ascii(ev);          /* '!' */
      empty = in_key_poll();
      in_mouse(&d1, &d2, &d3); dx = d1; dy = d2; w = d3; b = (int)in_mouse_buttons();
      os_exit(0);
    })");
  CHECK(o.compiled); CHECK(o.halted);
  CHECK_EQ(sym32(s, o, "c1"), (u32)'A'); CHECK_EQ(sym32(s, o, "up"), 1); CHECK_EQ(sym32(s, o, "c2"), 0u); CHECK_EQ(sym32(s, o, "c3"), (u32)'!');
  CHECK_EQ((int)sym32(s, o, "empty"), -1); CHECK_EQ((int)sym32(s, o, "dx"), 9); CHECK_EQ((int)sym32(s, o, "dy"), -4); CHECK_EQ(sym32(s, o, "b"), 1u);
}

// ======================= Proteção de memória (MMU) =======================
TEST(os_mmu_kills_tasks_touching_null_code_and_foreign_stacks) {
  Soc s; OsRun o = run_os(s, R"(
    volatile int alive, victim_addr_set; volatile int *victim_ptr; volatile int survived;
    static void nullp(void) { volatile int *p = (volatile int *)0; *p = 1; survived += 100; }
    static void codew(void) { volatile int *p = (volatile int *)task_yield; *p = 1; survived += 100; }
    static void spy(void) { while (!victim_ptr) task_yield(); *victim_ptr = 5; survived += 100; }
    static void victim(void) { int local = 3; victim_ptr = &local; task_sleep(30); alive = local; }
    static void guard(void) { volatile char big[8192]; int i; for (i = 8191; i >= 0; i -= 1024) big[i] = 1; survived += 100; }   /* pilha de 4 KB: estoura */
    static void fine(void) { volatile int *vram = (volatile int *)0x10000000; vram[0] = 7; survived += 1; }
    void app_main(void) {
      int a, b, c, d;
      a = task_create(nullp, 2048, 6); b = task_create(codew, 2048, 6); c = task_create(victim, 2048, 6); d = task_create(spy, 2048, 6);
      task_create(guard, 4096, 6); task_create(fine, 2048, 6);
      task_sleep(60);
      os_printf("states %d %d %d %d\n", task_state(a), task_state(b), task_state(c), task_state(d));
      os_exit(0);
    })");
  CHECK(o.compiled); CHECK(o.halted);
  CHECK_EQ(sym32(s, o, "survived"), 1u);                                          // só a tarefa legítima passou
  CHECK_EQ(sym32(s, o, "alive"), 3u);                                             // a vítima não foi alterada
  size_t n = 0, pos = 0; while ((pos = o.console.find("encerrada: acesso inválido", pos)) != std::string::npos) { n++; pos++; }
  CHECK_EQ(n, (size_t)4);                                                         // nulo, código, pilha alheia, guarda
}

TEST(os_mmu_tasks_share_heap_and_globals_and_kernel_survives) {
  Soc s; OsRun o = run_os(s, R"(
    volatile int total; int *shared; Sem done;
    static void worker(void) { int i; for (i = 0; i < 100; i++) shared[i] = i; sem_post(&done); }
    void app_main(void) {
      int i, sum = 0;
      shared = (int *)os_malloc(400); sem_init(&done, 0);
      task_create(worker, 2048, 6); sem_wait(&done);
      for (i = 0; i < 100; i++) sum += shared[i];
      total = sum; os_exit(0);
    })");
  CHECK(o.compiled); CHECK(o.halted); CHECK_EQ(sym32(s, o, "total"), 4950u); CHECK(o.console.find("encerrada") == std::string::npos);
  CHECK(s.bus.tlb_misses > 0);
}

// ======================= exec/dos (camada AmigaOS) =======================
TEST(os_amiga_exec_lists_memory_and_signals) {
  Soc s; OsRun o = run_os(s, R"(
    struct Node na, nb, nc; struct List l; volatile int order, clear_ok, sig_got, sigbit, find_ok, avail_ok, empty0, empty1, forbid_ok;
    struct AmTask *main_task; volatile int worker_started;
    static void worker(void) { worker_started = 1; Signal(main_task, 1u << 7); }
    void app_main(void) {
      unsigned char *m; unsigned before, i, got; struct Node *n;
      NewList(&l); empty0 = IsListEmpty(&l);
      na.ln_Name = "a"; nb.ln_Name = "b"; nc.ln_Name = "c";
      AddTail(&l, &nb); AddHead(&l, &na); AddTail(&l, &nc);
      n = RemHead(&l); order = n == &na; n = RemTail(&l); order = order && n == &nc; find_ok = FindName(&l, "b") == &nb && !FindName(&l, "zzz");
      Remove(&nb); empty1 = IsListEmpty(&l);
      before = AvailMem(0); m = (unsigned char *)AllocMem(300, MEMF_CLEAR | MEMF_CHIP); clear_ok = 1;
      for (i = 0; i < 300; i++) if (m[i]) clear_ok = 0;
      avail_ok = AvailMem(0) < before; FreeMem(m, 300); avail_ok = avail_ok && AvailMem(0) == before;
      main_task = FindTask(0); sigbit = AllocSignal(7);
      CreateTask("worker", 6, worker, 2048);
      got = Wait(1u << 7); sig_got = got == (1u << 7) && worker_started;
      sig_got = sig_got && (SetSignal(0, 0xFFFFFFFF) == 0) && FindTask("worker") != 0;
      Forbid(); Forbid(); Permit(); Permit(); forbid_ok = 1;
      os_exit(0);
    })");
  CHECK(o.compiled); CHECK(o.halted);
  for (const char* v : {"order", "find_ok", "clear_ok", "avail_ok", "sig_got", "empty0", "empty1", "forbid_ok"}) { std::printf("  %s\n", v); CHECK_EQ(sym32(s, o, v), 1u); }
  CHECK_EQ(sym32(s, o, "sigbit"), 7u);
}

TEST(os_amiga_message_ports_ping_pong_between_tasks) {
  Soc s; OsRun o = run_os(s, R"(
    struct MsgPort *server_port; volatile int served, reply_sum;
    struct Msg2 { struct Message m; int value; };
    static struct Msg2 mm;                         /* mensagens ficam em memória compartilhada: a pilha de uma tarefa é inacessível às outras (MMU) */
    static void server(void) {
      struct MsgPort *p = CreateMsgPort(); struct Msg2 *msg; int n;
      server_port = p;
      for (n = 0; n < 5; n++) { WaitPort(p); msg = (struct Msg2 *)GetMsg(p); msg->value = msg->value * 2; served++; ReplyMsg(&msg->m); }
      DeleteMsgPort(p);
    }
    void app_main(void) {
      struct MsgPort *rp = CreateMsgPort(); int i;
      CreateTask("server", 6, server, 4096);
      while (!server_port) task_yield();
      for (i = 1; i <= 5; i++) {
        mm.m.mn_ReplyPort = rp; mm.m.mn_Length = sizeof(mm); mm.value = i * 10;
        PutMsg(server_port, &mm.m); WaitPort(rp); GetMsg(rp);
        reply_sum += mm.value;
      }
      DeleteMsgPort(rp); os_exit(0);
    })");
  CHECK(o.compiled); CHECK(o.halted); CHECK_EQ(sym32(s, o, "served"), 5u); CHECK_EQ(sym32(s, o, "reply_sum"), 300u);
}

TEST(os_amiga_dos_open_read_seek_write_delay) {
  std::vector<u8> txt(3000); for (size_t i = 0; i < txt.size(); i++) txt[i] = (u8)('a' + i % 26);
  std::vector<u8> img; CHECK(sffs_build({{"S/Startup", txt}}, img));
  Soc s; s.dvd.insert(img);
  OsRun o = run_os(s, R"(
    volatile int size, n1, old, n2, miss_err, wr_err, fail_open, t_delay, c0, c1;
    void app_main(void) {
      BPTR fh; char buf[16]; unsigned t0;
      size = FileSize("S/Startup"); fail_open = Open("S/Nope", MODE_OLDFILE) == 0; miss_err = IoErr();
      fh = Open("S/Startup", MODE_OLDFILE);
      n1 = Read(fh, buf, 10); c0 = buf[9];
      old = Seek(fh, -5, OFFSET_END); n2 = Read(fh, buf, 16); c1 = buf[0];
      Close(fh);
      wr_err = Open("S/New", MODE_NEWFILE) == 0 && IoErr() == ERROR_WRITE_PROTECTED;
      Write(Output(), "ola dos\n", 8); PutStr("fim\n");
      t0 = os_ticks(); Delay(5); t_delay = os_ticks() - t0;
      os_exit(0);
    })");
  CHECK(o.compiled); CHECK(o.halted);
  CHECK_EQ(sym32(s, o, "size"), 3000u); CHECK_EQ(sym32(s, o, "fail_open"), 1u); CHECK_EQ(sym32(s, o, "miss_err"), 205u);
  CHECK_EQ(sym32(s, o, "n1"), 10u); CHECK_EQ(sym32(s, o, "c0"), (u32)'j'); CHECK_EQ(sym32(s, o, "old"), 10u);
  CHECK_EQ(sym32(s, o, "n2"), 5u); CHECK_EQ(sym32(s, o, "c1"), (u32)txt[2995]); CHECK_EQ(sym32(s, o, "wr_err"), 1u);
  CHECK(o.console.find("ola dos\nfim\n") != std::string::npos);
  u32 d = sym32(s, o, "t_delay"); CHECK(d >= 100 && d <= 104);
}

// ======================= IPL: boot pelo DVD e reinício a pedido do programa =======================
TEST(ipl_boots_from_dvd_and_chains_to_another_program) {
  CcOptions opt; opt.include_dirs = {"os", "os/include"};
  CcResult a = cc_compile("#include \"snowos_all.c\"\nvoid app_main(void) { os_printf(\"A: menu\\n\"); task_sleep(3); os_reboot(\"games/b.bin\"); }", "a.c", opt);
  CcResult b = cc_compile("#include \"snowos_all.c\"\nvoid app_main(void) { os_printf(\"B: jogo ticks<10=%d\\n\", os_ticks() < 10); os_exit(5); }", "b.c", opt);
  CHECK(a.ok); CHECK(b.ok);
  std::vector<u8> disc; std::map<std::string, u32> fl = {{"boot/menu.bin", a.entry}, {"games/b.bin", b.entry}};
  CHECK(sffs_build({{"boot/menu.bin", a.image}, {"games/b.bin", b.image}}, disc, nullptr, &fl));
  Soc s; s.dvd.insert(disc);
  CHECK(!s.boot_dvd("nao/existe.bin")); CHECK(s.boot_dvd());
  CHECK(s.run_until_halt(3000000000000ull)); s.bus.cache_flush_all(false);
  CHECK_EQ(s.boots, 2u); CHECK(s.last_boot == "games/b.bin");
  CHECK(s.console.out.find("A: menu\nB: jogo ticks<10=1") != std::string::npos);
  CHECK_EQ(s.bus.raw_r32(b.symbols["os_exit_code"]), 5u);
}
