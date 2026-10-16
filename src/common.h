// SnowFlame SF-100 - tipos e utilitários comuns (memória SEMPRE big-endian)
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <deque>
#include <queue>
#include <functional>
#include <algorithm>
#include <stdexcept>

namespace sf {

using u8 = uint8_t;  using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;
using i8 = int8_t;   using i16 = int16_t;  using i32 = int32_t;  using i64 = int64_t;

inline u16 rbe16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }
inline u32 rbe32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
inline u64 rbe64(const u8* p) { return ((u64)rbe32(p) << 32) | rbe32(p + 4); }
inline void wbe16(u8* p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
inline void wbe32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
inline void wbe64(u8* p, u64 v) { wbe32(p, (u32)(v >> 32)); wbe32(p + 4, (u32)v); }

inline u32 f2u(float f) { u32 u; std::memcpy(&u, &f, 4); return u; }
inline float u2f(u32 u) { float f; std::memcpy(&f, &u, 4); return f; }
inline u64 d2u(double f) { u64 u; std::memcpy(&u, &f, 8); return u; }
inline double u2d(u64 u) { double f; std::memcpy(&f, &u, 8); return f; }

inline float rbef(const u8* p) { return u2f(rbe32(p)); }
inline void wbef(u8* p, float f) { wbe32(p, f2u(f)); }

// Tempo simulado em picossegundos.
using Ps = u64;
constexpr u64 CPU_HZ = 266000000ull;   // SnowFire-CF266
constexpr u64 VPU_HZ = 150000000ull;   // SnowVector-128
constexpr u64 GPU_HZ = 200000000ull;   // FlameCore 3D
constexpr u64 DSP_HZ = 100000000ull;   // SnowPaula
constexpr u64 CHIP_HZ = 100000000ull;  // Blitter / Copper
constexpr u64 FRAME_PS = 16666666667ull; // 60 Hz
inline Ps cyc2ps(u64 cycles, u64 hz) { return (Ps)((__uint128_t)cycles * 1000000000000ull / hz); }

// Agendador de eventos determinístico (tempo em ps).
class Sched {
 public:
  Ps now = 0;
  void at(Ps delay, std::function<void()> fn) { q.push(Ev{now + delay, seq++, std::move(fn)}); }
  bool empty() const { return q.empty(); }
  Ps next_time() const { return q.empty() ? ~0ull : q.top().t; }
  // Avança o relógio para t executando todos os eventos com tempo <= t.
  void advance_to(Ps t) {
    while (!q.empty() && q.top().t <= t) {
      Ev e = q.top(); q.pop();
      if (e.t > now) now = e.t;
      e.fn();
    }
    if (t > now) now = t;
  }
 private:
  struct Ev { Ps t; u64 seq; std::function<void()> fn; };
  struct Cmp { bool operator()(const Ev& a, const Ev& b) const { return a.t != b.t ? a.t > b.t : a.seq > b.seq; } };
  std::priority_queue<Ev, std::vector<Ev>, Cmp> q;
  u64 seq = 0;
};

}  // namespace sf
