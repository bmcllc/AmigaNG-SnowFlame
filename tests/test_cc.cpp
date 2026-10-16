#include "helpers.h"
#include "snowcc.h"

// ======================= Item 5: compilador C (snowcc) =======================
struct CRun { bool ok = false; std::string err; Soc* soc = nullptr; CcResult r; u32 ret = 0; };
static CRun run_c(const std::string& src, Soc& s, u64 limit_ps = 4000000000000ull, const CcOptions* o = nullptr) {
  CRun x; CcOptions opt; if (o) opt = *o; x.r = cc_compile(src, "teste.c", opt);
  if (!x.r.ok) { x.err = x.r.error; return x; }
  s.load(x.r.base, x.r.image); s.cpu.reset_regs(); s.cpu.a[7] = 0x00F00000; s.cpu.set_pc(x.r.entry);
  x.ok = s.run_until_halt(limit_ps); s.bus.cache_flush_all(false); x.ret = s.cpu.d[0]; x.soc = &s; return x;
}
static i64 c_int(const std::string& body, const std::string& globals = "") {
  Soc s; CRun r = run_c(globals + "\nint main(void) {\n" + body + "\n}\n", s);
  if (!r.r.ok) { std::printf("    erro de compilação: %s\n", r.err.c_str()); return -999999; }
  if (!r.ok) { std::printf("    programa não terminou (pc=%08x)\n", s.cpu.pc); return -999998; }
  return (i32)r.ret;
}
#define CC_CHECK(expected, body) do { i64 _v = c_int(body); if (_v != (expected)) report_fail(__FILE__, __LINE__, std::string("C: ") + body + "  =>  " + std::to_string(_v) + " (esperado " + std::to_string(expected) + ")"); } while (0)

#define CC_CHECK_G(expected, globals, body) do { i64 _v = c_int(body, globals); if (_v != (expected)) report_fail(__FILE__, __LINE__, std::string("C: ") + body + "  =>  " + std::to_string(_v) + " (esperado " + std::to_string(expected) + ")"); } while (0)

TEST(cc_arithmetic_and_precedence) {
  CC_CHECK(14, "return 2 + 3 * 4;");
  CC_CHECK(20, "return (2 + 3) * 4;");
  CC_CHECK(-7, "return -(3 + 4);");
  CC_CHECK(3, "return 17 / 5;");  CC_CHECK(2, "return 17 % 5;");  CC_CHECK(-3, "return -17 / 5;");  CC_CHECK(-2, "return -17 % 5;");
  CC_CHECK(1, "return 1 << 0;"); CC_CHECK(40, "int a = 5; return a << 3;"); CC_CHECK(-4, "int a = -16; return a >> 2;");
  CC_CHECK(0x0F, "return 0xFF & 0x0F;"); CC_CHECK(0xFF, "return 0xF0 | 0x0F;"); CC_CHECK(0xF0, "return 0xFF ^ 0x0F;"); CC_CHECK(-1, "return ~0;");
  CC_CHECK(1, "return 5 > 3;"); CC_CHECK(0, "return 5 < 3;"); CC_CHECK(1, "return 3 <= 3 && 4 >= 4;"); CC_CHECK(1, "return 1 != 2 || 0;");
  CC_CHECK(0, "int a = 0; return !a == 0;");  CC_CHECK(1, "int a = 7; int b = 7; return a == b;");
  CC_CHECK(7, "int a = 3, b = 4; return a + b;"); CC_CHECK(12, "int a = 3, b = 4; return a * b;"); CC_CHECK(-1, "int a = 3, b = 4; return a - b;");
  CC_CHECK(5, "int a = 100, b = 20; return a / b;"); CC_CHECK(0, "int a = 100, b = 20; return a % b;");
  CC_CHECK(1000000, "return 1000 * 1000;"); CC_CHECK(-2147483647 - 1, "return 0x7FFFFFFF + 1;");
}

TEST(cc_unsigned_and_sign_extension) {
  CC_CHECK(1, "unsigned a = 4000000000u; return a > 5;");
  CC_CHECK(1, "int a = -1; return a > 5u;");                       // int vs unsigned: comparação sem sinal (-1 vira 0xFFFFFFFF)
  CC_CHECK(1, "int a = -1; return a < 5;");
  CC_CHECK(1073741823, "unsigned a = 0xFFFFFFFFu; return a >> 2;");
  CC_CHECK(-1, "int a = -4; return a >> 2;");
  CC_CHECK(715827882, "unsigned a = 0xFFFFFFFEu; return a / 6;");
  CC_CHECK(2, "unsigned a = 0xFFFFFFFEu; return a % 4;");
  CC_CHECK(-1, "char c = (char)0xFF; return c;");  CC_CHECK(255, "unsigned char c = 0xFF; return c;");
  CC_CHECK(-32768, "short s = (short)0x8000; return s;");  CC_CHECK(65535, "unsigned short s = 0xFFFF; return s;");
  CC_CHECK(44, "int x = 300; char c = (char)x; return c;");  CC_CHECK(1, "char c = 127; c++; return c < 0;");
  CC_CHECK(5, "return (unsigned char)261;");
}

TEST(cc_control_flow_and_loops) {
  CC_CHECK(55, "int s = 0; for (int i = 1; i <= 10; i++) s += i; return s;");
  CC_CHECK(55, "int s = 0, i = 1; while (i <= 10) { s += i; i++; } return s;");
  CC_CHECK(55, "int s = 0, i = 1; do { s += i; i++; } while (i <= 10); return s;");
  CC_CHECK(25, "int s = 0; for (int i = 0; i < 10; i++) { if (i % 2) continue; if (i > 8) break; s += i + 1; } return s;");
  CC_CHECK(1, "int a = 3; if (a > 2) return 1; else return 2;");  CC_CHECK(2, "int a = 1; if (a > 2) return 1; else return 2;");
  CC_CHECK(3, "int a = 5; int r; if (a < 0) r = 1; else if (a < 4) r = 2; else r = 3; return r;");
  CC_CHECK(9, "int a = 0; return a ? 1 : 9;");  CC_CHECK(1, "int a = 5; return a ? 1 : 9;");
  CC_CHECK(5, "int i = 0; for (;;) { if (++i == 5) break; } return i;");
  CC_CHECK(45, "int s = 0; for (int i = 0; i < 10; i++) for (int j = 0; j < 10; j++) if (j == i) s += i; return s;");
  CC_CHECK(20, "int s = 0; int i = 0; while (1) { i++; if (i > 20) break; if (i < 20) continue; s = i; } return s;");
  CC_CHECK(6, "int x = 3; int y = (x++, x * 2) - 2; return y;");
}

TEST(cc_switch_statement) {
  const std::string sw = "int f(int v) { int r = 0; switch (v) { case 1: r = 10; break; case 2: r = 20; case 3: r = r + 30; break; case 7: return 70; default: r = -1; } return r; }\n";
  Soc s; CRun r = run_c(sw + "int main(void) { return f(1) + f(2) * 1000 + f(3) * 100000 + f(7) * 10 + (f(9) == -1) * 100000000; }", s);
  CHECK(r.r.ok); CHECK(r.ok); CHECK_EQ((i32)r.ret, 10 + 50 * 1000 + 30 * 100000 + 700 + 100000000);
}

TEST(cc_functions_recursion_and_pointers) {
  Soc s; CRun r = run_c(R"(
    int fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); }
    int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
    int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }
    void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }
    int sum3(int a, int b, int c) { return a + b + c; }
    int main(void) {
      int x = 3, y = 9; swap(&x, &y);
      return fact(6) + fib(15) * 1000 + gcd(84, 36) * 1000000 + (x * 10 + y) * 1000000;
    })", s);
  CHECK(r.r.ok); if (!r.r.ok) std::printf("    %s\n", r.err.c_str()); CHECK(r.ok);
  CHECK_EQ((i32)r.ret, 720 + 610 * 1000 + 12 * 1000000 + 93 * 1000000);
}

TEST(cc_arrays_pointers_strings_and_structs) {
  Soc s; CRun r = run_c(R"(
    struct P { int x; int y; char tag; short w; };
    typedef struct { int a[4]; struct P p; } Box;
    int g[5] = {1, 2, 3, 4, 5};
    char msg[] = "snow";
    Box box = { {10, 20, 30, 40}, { 7, 8, 'z', 9 } };
    int total;
    int len(const char *s) { int n = 0; while (*s++) n++; return n; }
    int sum(int *v, int n) { int t = 0; for (int i = 0; i < n; i++) t += v[i]; return t; }
    int main(void) {
      int loc[6]; for (int i = 0; i < 6; i++) loc[i] = i * i;
      struct P q; q.x = 5; q.y = 6; q.tag = 'a'; q.w = -2;
      struct P *pp = &q; pp->x += 10;
      Box b2 = box; b2.a[2] = 99;
      char *s = msg; s[0] = 'S';
      total = sum(g, 5) + sum(loc, 6) * 100 + len("hello!") * 10000 + len(msg) * 100000;
      total += pp->x * 1000000 + (b2.a[2] + box.a[2] + b2.p.y) * 10000000;
      return total + q.w + q.tag + sizeof(struct P) + sizeof(Box) + (&g[3] - &g[1]) + (msg[0] == 'S') + box.p.tag - 'z';
    })", s);
  CHECK(r.r.ok); if (!r.r.ok) std::printf("    %s\n", r.err.c_str()); CHECK(r.ok);
  i32 expect = 15 + 55 * 100 + 6 * 10000 + 4 * 100000 + 15 * 1000000 + (99 + 30 + 8) * 10000000;
  expect += -2 + 'a' + 12 + 28 + 2 + 1 + 0;
  CHECK_EQ((i32)r.ret, expect);
}

TEST(cc_globals_initializers_pointers_and_function_pointers) {
  Soc s; CRun r = run_c(R"(
    int table[4] = {3, 1, 4, 1};
    int *tp = &table[2];
    const char *names[3] = {"aa", "bbb", "c"};
    int twice(int x) { return 2 * x; }
    int plus1(int x) { return x + 1; }
    int (*fns[2])(int) = { twice, plus1 };
    int apply(int (*f)(int), int v) { return f(v); }
    static int counter = 100;
    int bump(void) { static int calls; calls++; counter++; return calls; }
    int main(void) {
      bump(); bump(); int c = bump();
      int (*fp)(int) = twice; int (*gp)(int) = &plus1;
      return *tp * 1000 + apply(fp, 10) + apply(gp, 1) * 10 + fns[1](5) * 100000 + (names[1][2] == 'b') * 10000000 + c * 100000000 + counter * 0 + (int)(sizeof names);
    })", s);
  CHECK(r.r.ok); if (!r.r.ok) std::printf("    %s\n", r.err.c_str()); CHECK(r.ok);
  CHECK_EQ((i32)r.ret, 4 * 1000 + 20 + 2 * 10 + 6 * 100000 + 10000000 + 3 * 100000000 + 12);
}

TEST(cc_goto_and_labels) {
  CC_CHECK(10, "int i = 0; again: i++; if (i < 10) goto again; return i;");
  CC_CHECK(3, "int i; for (i = 0; i < 100; i++) { if (i == 3) goto out; } return -1; out: return i;");
  CC_CHECK(6, "int s = 0; goto mid; s = 100; mid: s += 6; end: ; return s;");
  { Soc s; CRun r = run_c("int main(void) { goto nada; return 0; }", s); CHECK(!r.r.ok); }
  { Soc s; CRun r = run_c("int main(void) { a: a: return 0; }", s); CHECK(!r.r.ok); }
}

TEST(cc_preprocessor_stringize_and_paste) {
  std::string g = "#define STR(x) #x\n#define CAT(a, b) a ## b\n#define FIELD(n) int f_ ## n = n;\n#define GLUE foo ## bar\n";
  CC_CHECK_G(101, g, "return STR(hello)[1];");
  CC_CHECK_G(9, g, "int CAT(va, r) = 4; int vab = 5; return var + CAT(v, ab);");
  CC_CHECK_G(7, g, "int GLUE = 7; return foobar;");
  { Soc s; CRun r = run_c(g + "FIELD(3)\nint main(void) { return f_3 + STR( a \"b\" )[3]; }", s); CHECK(r.r.ok); CHECK_EQ((i32)r.ret, 3 + 'b'); }
}

TEST(cc_preprocessor_macros_enum_and_conditionals) {
  Soc s; CRun r = run_c(R"(
    #define N 8
    #define SQR(x) ((x) * (x))
    #define MAX(a, b) ((a) > (b) ? (a) : (b))
    #define FLAG 1
    enum Color { RED, GREEN = 5, BLUE };
    #ifdef FLAG
    int flag = 11;
    #else
    int flag = 22;
    #endif
    #if N > 4 && !defined(NOPE)
    int big = 1;
    #elif N > 2
    int big = 2;
    #else
    int big = 3;
    #endif
    #ifndef FLAG
    #error nunca
    #endif
    int main(void) { return SQR(N + 1) + MAX(3, 7) * 100 + BLUE * 10000 + flag * 1000000 + big * 100000000 + RED; })", s);
  CHECK(r.r.ok); if (!r.r.ok) std::printf("    %s\n", r.err.c_str()); CHECK(r.ok);
  CHECK_EQ((i32)r.ret, 81 + 700 + 6 * 10000 + 11 * 1000000 + 100000000);
  CcOptions o; o.defines["VAL"] = "41"; Soc s2; CRun r2 = run_c("int main(void) { return VAL + 1; }", s2, 4000000000000ull, &o); CHECK(r2.ok); CHECK_EQ(r2.ret, 42);
  CcOptions o2; o2.include_loader = [](const std::string& n, std::string& t) { if (n == "inc.h") { t = "#define FROM_INC 5\nint inc_fn(void) { return FROM_INC; }\n"; return true; } return false; };
  Soc s3; CRun r3 = run_c("#include \"inc.h\"\nint main(void) { return inc_fn() * 2; }", s3, 4000000000000ull, &o2); CHECK(r3.ok); CHECK_EQ(r3.ret, 10);
}

TEST(cc_compound_assignment_incdec_and_pointer_arithmetic) {
  CC_CHECK(30, "int a = 10; a += 5; a *= 2; return a;");
  CC_CHECK(3, "int a = 17; a /= 5; return a;");  CC_CHECK(2, "int a = 17; a %= 5; return a;");
  CC_CHECK(12, "int a = 3; a <<= 2; return a;");  CC_CHECK(3, "int a = 12; a >>= 2; return a;");
  CC_CHECK(8, "int a = 12; a &= 10; return a;");  CC_CHECK(14, "int a = 12; a |= 2; return a;");  CC_CHECK(6, "int a = 12; a ^= 10; return a;");
  CC_CHECK(5, "int a = 4; return ++a;");  CC_CHECK(4, "int a = 4; return a++;");  CC_CHECK(9, "int a = 4; a++; ++a; a++; a--; a--; a++; ++a; return a + 2;");
  CC_CHECK(3, "int v[4] = {1, 2, 3, 4}; int *p = v; p++; p += 1; return *p;");
  CC_CHECK(4, "int v[4] = {1, 2, 3, 4}; int *p = v + 3; return *p--; ");
  CC_CHECK(3, "int v[4] = {1, 2, 3, 4}; int *p = v + 3; return p - v;");
  CC_CHECK(2, "short v[4] = {1, 2, 3, 4}; short *p = v; return *(p + 1);");
  CC_CHECK(7, "char v[4] = {5, 6, 7, 8}; char *p = &v[2]; return *p;");
  CC_CHECK(30, "int a[3]; int *p = a; p[0] = 10; p[1] = 20; *(p + 2) = 0; return p[0] + p[1];");
  CC_CHECK(15, "int a = 5; int b = (a += 5) + 5; return b;");
  CC_CHECK(-5, "char c = -5; short s = c; int i = s; return i;");
  CC_CHECK(200, "unsigned char u = 100; u += 100; return u;");
  CC_CHECK(44, "unsigned char u = 200; u += 100; return u;");
}

TEST(cc_reports_errors_with_line_numbers) {
  auto err = [](const std::string& src) { CcResult r = cc_compile(src, "x.c"); return r.ok ? std::string("OK") : r.error; };
  CHECK(err("int main(void) { return y; }").find("x.c:1:") == 0);
  CHECK(err("int main(void) {\n  int a = 1;\n  return a +;\n}").find("x.c:3:") == 0);
  CHECK(err("int main(void) { 3 = 4; }").find("lvalue") != std::string::npos);
  CHECK(err("int f(int a); int main(void) { return f(1, 2); }").find("número de argumentos") != std::string::npos);
  CHECK(err("int main(void) { break; }").find("fora de laço") != std::string::npos);
  CHECK(err("int main(void) { return g(); }").find("não declarado") != std::string::npos);
  CHECK(err("int main(void) { return 1; ").find("não terminado") != std::string::npos || err("int main(void) { return 1; ").find("esperava") != std::string::npos);
  CHECK(err("int f(void); int main(void) { return f(); }").find("símbolo indefinido: f") != std::string::npos);
  CHECK(err("#include \"nao_existe.h\"\nint main(void){return 0;}").find("include") != std::string::npos);
  CHECK(err("int main(void) { struct S s; return 0; }").find("incompleta") != std::string::npos);
  CHECK(err("int x = 1; int x = 2; int main(void){return 0;}").find("inicializada duas vezes") != std::string::npos);
  CHECK(err("int main(void) { return 1; }") == "OK");
}

TEST(cc_intrinsics_listing_and_symbols) {
  Soc s; CRun r = run_c(R"(
    int g = 3;
    int main(void) { unsigned sr = __get_sr(); __set_sr(sr); return (sr & 0x2000) ? 1 : 0; })", s);
  CHECK(r.ok); CHECK_EQ(r.ret, 1);
  CHECK(r.r.symbols.count("main") && r.r.symbols.count("g") && r.r.symbols.count("_end")); CHECK(r.r.symbols["g"] >= r.r.code_end);
  CHECK(r.r.listing.find("main:") != std::string::npos); CHECK(r.r.listing.find("link") != std::string::npos); CHECK(r.r.listing.find("rts") != std::string::npos);
  CHECK(r.r.listing.find("g:") != std::string::npos);
}

TEST(cc_interrupt_handlers_and_trap_calls) {
  // timer do SoC dispara IRQ; o handler (stub __interrupt) incrementa um contador; main usa TRAP #1 com argumentos em d0/d1
  Soc s; CRun r = run_c(R"(
    volatile int ticks;
    volatile int trap_sum;
    __interrupt unsigned timer_isr(unsigned sp) {
      ticks++;
      *(volatile unsigned *)0xF0000010 = 0x400;           /* limpa IRQ_TIMER no SnowBridge */
      return sp;
    }
    __interrupt unsigned trap_handler(unsigned sp) {
      unsigned *regs = (unsigned *)sp;                    /* d0..d7, a0..a6 salvos */
      trap_sum += regs[0] * 100 + regs[1];
      regs[0] = regs[0] + regs[1];                        /* devolve em d0 */
      return sp;
    }
    int main(void) {
      unsigned *vec = (unsigned *)0x400;
      vec[28] = (unsigned)&timer_isr; vec[33] = (unsigned)&trap_handler;
      __set_vbr(0x400);
      *(volatile unsigned *)0xF0000014 = 0x400;           /* IRQ_MASK: timer */
      *(volatile unsigned *)0xF0080000 = 100;             /* TMR_LOAD: 100 us */
      *(volatile unsigned *)0xF0080004 = 1 | 2 | 4;       /* enable | periódico | IRQ */
      __set_sr(0x2000);                                   /* habilita interrupções */
      while (ticks < 3) { }
      int r = __trapcall(1, 40, 2, 0);
      return r * 1000 + ticks;
    })", s);
  CHECK(r.r.ok); if (!r.r.ok) std::printf("    %s\n", r.err.c_str());
  CHECK(r.ok); CHECK(r.ret % 1000 >= 3); CHECK_EQ(r.ret / 1000, 42);
  CHECK_EQ(s.bus.raw_r32(r.r.symbols["trap_sum"]), 4002);
}

TEST(cc_variadic_functions_with_va_macros) {
  Soc s; CRun r = run_c(R"(
    typedef char *va_list;
    #define va_start(ap, last) ((ap) = (char *)&(last) + 4)
    #define va_arg(ap, type) (*(type *)(((ap) += 4) - 4))
    int sum(int n, ...) { va_list ap; va_start(ap, n); int t = 0; for (int i = 0; i < n; i++) t += va_arg(ap, int); return t; }
    int main(void) { return sum(4, 10, 20, 30, 40) * 100 + sum(1, 7) + sum(0); })", s);
  CHECK(r.r.ok); if (!r.r.ok) std::printf("    %s\n", r.err.c_str()); CHECK(r.ok); CHECK_EQ(r.ret, 10007);
}
