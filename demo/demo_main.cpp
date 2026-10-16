#include "scene.h"
#include <cstdio>

int main(int argc, char** argv) {
  using namespace sf; using namespace sf::demo;
  std::string out = argc > 1 ? argv[1] : "out"; int frames = argc > 2 ? std::atoi(argv[2]) : 120;
  SnowDemo d; d.soc.video.start(); d.boot();
  std::printf("SnowFlame SF-100 - demo integrada (%dx%d, %d quadros)\n", W, H, frames);
  Ps worst = 0; double sum_cpu = 0; u64 tri = 0, shaded = 0, covered = 0;
  for (int f = 0; f < frames; f++) {
    FrameStats fs = d.frame(f); worst = std::max(worst, fs.cpu_ps); sum_cpu += fs.cpu_ps / 1e9; tri += fs.tris; shaded += fs.shaded; covered += fs.covered;
    if (f % 30 == 0 || f == frames - 1)
      std::printf("quadro %3d: quadro de CPU %.3f ms | VPU-A %llu cic (%.1f us) VPU-B %llu cic | GPU %llu cic (%.2f ms) tris=%llu cobertos=%llu sombreados=%llu blits=%llu\n", f, fs.cpu_ps / 1e9,
        (unsigned long long)fs.vpuA_cycles, fs.vpuA_cycles / 150.0, (unsigned long long)fs.vpuB_cycles, (unsigned long long)fs.gpu_cycles, fs.gpu_cycles / 200e3,
        (unsigned long long)fs.tris, (unsigned long long)fs.covered, (unsigned long long)fs.shaded, (unsigned long long)fs.blits);
    if (f == 0 || f == 45 || f == frames - 1) write_ppm(out + "/frame_" + std::to_string(f) + ".ppm", d.soc.video.width(), d.soc.video.height(), d.soc.video.frame);
  }
  d.soc.sync_audio(); write_wav(out + "/audio.wav", d.soc.paula.pcm);
  std::printf("tempo simulado: %.1f ms; pior quadro %.3f ms (orçamento 16.667 ms); média %.3f ms\n", d.soc.sched.now / 1e9, worst / 1e9, sum_cpu / frames);
  std::printf("GPU: HSR sombreou %.1f%% dos pixels cobertos; áudio: %zu amostras estéreo\n", 100.0 * shaded / std::max<u64>(covered, 1), d.soc.paula.pcm.size() / 2);
  std::printf("DSP SnowPaula: pico %.1f%% do orçamento de %u ciclos/quadro, %llu vozes descartadas\n", d.soc.paula.dsp_peak / 10.0, (unsigned)Paula::DSP_BUDGET, (unsigned long long)d.soc.paula.dsp_drops);
  std::printf("I-cache: %llu acertos / %llu falhas | D-cache: %llu acertos / %llu falhas | espera de barramento acumulada: %.3f ms\n",
    (unsigned long long)d.soc.bus.ihits, (unsigned long long)d.soc.bus.imisses, (unsigned long long)d.soc.bus.dc.hits, (unsigned long long)d.soc.bus.dc.misses, d.soc.bus.arb_wait_ps / 1e9);
  return 0;
}
