// Mini-framework de testes (sem dependências externas).
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>

struct TestCase { const char* name; void (*fn)(); };
inline std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
inline int& failures_in_test() { static int n = 0; return n; }
struct Registrar { Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); } };
inline void report_fail(const char* file, int line, const std::string& msg) {
  failures_in_test()++; std::printf("    FALHA %s:%d: %s\n", file, line, msg.c_str());
}
#define TEST(name) static void name(); static Registrar reg_##name(#name, name); static void name()
#define CHECK(c) do { if (!(c)) report_fail(__FILE__, __LINE__, #c); } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); if (_a != _b) { \
  char _m[256]; std::snprintf(_m, sizeof _m, "%s == %s  (%lld != %lld; 0x%llx != 0x%llx)", #a, #b, _a, _b, (unsigned long long)_a, (unsigned long long)_b); report_fail(__FILE__, __LINE__, _m); } } while (0)
#define CHECK_NEAR(a, b, eps) do { double _a = (double)(a), _b = (double)(b); if (!(std::fabs(_a - _b) <= (eps))) { \
  char _m[256]; std::snprintf(_m, sizeof _m, "%s ~= %s  (%g vs %g, eps %g)", #a, #b, _a, _b, (double)(eps)); report_fail(__FILE__, __LINE__, _m); } } while (0)
