// Pool de threads do host para paralelizar trabalho do emulador que não depende de ordem (blocos do FlameCore, linhas do vídeo).
// Só velocidade: quem usa garante que cada tarefa lê dados imutáveis durante o lote e escreve uma área própria, e soma
// estatísticas por thread no fim, então o resultado é idêntico ao serial em qualquer número de threads.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace sf {

class HostPool {
 public:
  // Número de threads automático: SF_HOST_THREADS (ou SF_GPU_THREADS, nome antigo) ou os núcleos do host, até 16.
  static int auto_threads() {
    static const int n = []() {
      const char* e = std::getenv("SF_HOST_THREADS"); if (!e) e = std::getenv("SF_GPU_THREADS");
      int k = e ? std::atoi(e) : (int)std::thread::hardware_concurrency();
      return k < 1 ? 1 : k > 16 ? 16 : k;
    }();
    return n;
  }
  explicit HostPool(int n) : nthreads(n < 1 ? 1 : n) {
    for (int k = 1; k < nthreads; k++) th.emplace_back([this, k]() { worker(k); });
  }
  ~HostPool() { { std::lock_guard<std::mutex> l(m); quit = true; } cv.notify_all(); for (auto& t : th) t.join(); }
  int size() const { return nthreads; }
  // Executa fn(tarefa, thread) para tarefa em [0, ntasks), distribuídas dinamicamente; a thread chamadora é a de índice 0.
  void run(int ntasks, const std::function<void(int task, int thread)>& fn) {
    if (ntasks <= 0) return;
    next.store(0, std::memory_order_relaxed); total = ntasks;
    if (nthreads == 1 || ntasks == 1) { for (int t = 0; t < ntasks; t++) fn(t, 0); return; }
    { std::lock_guard<std::mutex> l(m); job = &fn; active = (int)th.size(); gen++; }
    cv.notify_all();
    drain(0);
    std::unique_lock<std::mutex> l(m); done_cv.wait(l, [&]() { return active == 0; });
    job = nullptr;
  }

 private:
  int nthreads; std::vector<std::thread> th;
  std::mutex m; std::condition_variable cv, done_cv; unsigned long long gen = 0; int active = 0; bool quit = false;
  const std::function<void(int, int)>* job = nullptr; std::atomic<int> next{0}; int total = 0;
  void drain(int k) { for (int t; (t = next.fetch_add(1, std::memory_order_relaxed)) < total;) (*job)(t, k); }
  void worker(int k) {
    unsigned long long seen = 0;
    for (;;) {
      { std::unique_lock<std::mutex> l(m); cv.wait(l, [&]() { return quit || gen != seen; }); if (quit) return; seen = gen; }
      drain(k);
      { std::lock_guard<std::mutex> l(m); if (--active == 0) done_cv.notify_one(); }
    }
  }
};

}  // namespace sf
