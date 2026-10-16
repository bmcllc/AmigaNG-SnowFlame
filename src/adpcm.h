// IMA ADPCM 4 bits (nibble alto primeiro). Decodificador usado pelo SnowPaula; codificador para ferramentas.
#pragma once
#include "common.h"

namespace sf {

namespace adpcm {
static const int kStep[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157,
  173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
  2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
  27086, 29794, 32767};
static const int kIdx[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
struct State { int pred = 0; int idx = 0; };
inline int decode(State& s, int code) {
  int step = kStep[s.idx], d = step >> 3;
  if (code & 4) d += step; if (code & 2) d += step >> 1; if (code & 1) d += step >> 2;
  s.pred += (code & 8) ? -d : d;
  s.pred = std::max(-32768, std::min(32767, s.pred));
  s.idx = std::max(0, std::min(88, s.idx + kIdx[code & 7]));
  return s.pred;
}
inline int encode_one(State& s, int sample) {
  int step = kStep[s.idx], diff = sample - s.pred, code = 0;
  if (diff < 0) { code = 8; diff = -diff; }
  if (diff >= step) { code |= 4; diff -= step; } step >>= 1;
  if (diff >= step) { code |= 2; diff -= step; } step >>= 1;
  if (diff >= step) code |= 1;
  decode(s, code); return code;
}
inline std::vector<u8> encode(const std::vector<int16_t>& pcm, State init = State()) {
  std::vector<u8> out((pcm.size() + 1) / 2, 0); State s = init;
  for (size_t i = 0; i < pcm.size(); i++) { int c = encode_one(s, pcm[i]); if (i & 1) out[i / 2] |= (u8)c; else out[i / 2] |= (u8)(c << 4); }
  return out;
}
}  // namespace adpcm

}  // namespace sf
