// bench/annot_bench.cpp
//
// Cross-tool instrumentation benchmark: Latte vs Caliper vs Likwid vs Tracy
//   vs std::chrono. Google Benchmark harness, cycle-accurate RDTSC timing.
//
// Part A (default):  overhead of one instrumented region per tool.
//                    Run the same binary under several configs:
//       ./bin/annot_bench                                  Caliper/Likwid inactive
//       CALI_CONFIG=runtime-report ./bin/annot_bench       Caliper aggregates regions
//       CALI_CONFIG=event-trace  ./bin/annot_bench         Caliper records every event
//       likwid-perfctr -m -g CLOCK -c 3 ./bin/annot_bench  Likwid markers active
//
// Part B (--error):   measurement error vs truth for a fixed busy-workload
//                    (~20k cycles). Latte/chrono/truth are printed by the
//                    program; Caliper/Likwid reports are captured from
//                    their output files by the calling script.
//       ./bin/annot_bench --error
//       CALI_CONFIG=runtime-report ./bin/annot_bench --error
//       likwid-perfctr -m -g CLOCK -c 3 ./bin/annot_bench --error
//
// Build (repo root):
//   g++ -O3 -march=native -std=c++17 -pthread -I. -I$HOME/.local/include
//       -I$HOME/.local/src/tracy/public -DLIKWID_PERFMON -DTRACY_ENABLE
//       bench/annot_bench.cpp $HOME/.local/src/tracy/public/TracyClient.cpp
//       -o bin/annot_bench -L$HOME/.local/lib -lcaliper -llikwid -lbenchmark
//       -Wl,-rpath,$HOME/.local/lib
//
// Tracy variants: add -DTRACY_ON_DEMAND for the on-demand build (no server =>
// near-zero zone cost). Without any server, the always-on client grows its
// queue (no stall, but memory grows); connect tracy-capture for the realistic
// number: the CLIENT listens on 127.0.0.1:8086, tracy-capture CONNECTS to it.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <vector>

#include "../Latte.hpp"
#include <caliper/cali.h>
#include <caliper/Annotation.h>
#include <likwid-marker.h>
#include <tracy/Tracy.hpp>

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

template <typename T>
static inline void do_not_optimize(T const& value) {
  asm volatile("" : : "r,m"(value));
}

void PinThread(int core_id) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);
  pthread_t current_thread = pthread_self();
  if (pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset) != 0) {
    std::cerr << "Warning: Failed to pin thread. Benchmarks may be unstable.\n";
  }
}

static __inline__ uint64_t __attribute__((always_inline)) rdtsc_begin() {
  _mm_lfence();
  return __rdtsc();
}
static __inline__ uint64_t __attribute__((always_inline)) rdtsc_end() {
  unsigned int aux;
  uint64_t res = __rdtscp(&aux);
  _mm_lfence();
  return res;
}

static double g_cps = 0.0;  // cycles per nanosecond (measured once)

static void calibrate_cps() {
  struct timespec ts;
  ts.tv_sec = 1;
  ts.tv_nsec = 0;
  uint64_t t0 = rdtsc_begin();
  nanosleep(&ts, nullptr);
  uint64_t t1 = rdtsc_end();
  g_cps = double(t1 - t0) / 1e9;
}

// deterministic busy workload: dependent-multiply chain, ~4 cycles/iter
static inline void busy_work(uint64_t k) {
  volatile uint64_t acc = 1;
  for (uint64_t i = 1; i <= k; ++i) acc = acc * 131u + i;
  do_not_optimize(acc);
}

static uint64_t calibrate_busy_k(uint64_t target_cycles) {
  const uint64_t k0 = 100000;
  uint64_t t0 = rdtsc_begin();
  busy_work(k0);
  uint64_t t1 = rdtsc_end();
  double cpi = double(t1 - t0) / double(k0);
  return uint64_t(double(target_cycles) / cpi);
}

struct Stats {
  double mean, median, min, max, stddev;
};

static Stats compute_stats(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  Stats s{};
  s.min = v.front();
  s.max = v.back();
  s.median = v[v.size() / 2];
  double sum = 0;
  for (double x : v) sum += x;
  s.mean = sum / double(v.size());
  double var = 0;
  for (double x : v) var += (x - s.mean) * (x - s.mean);
  s.stddev = std::sqrt(var / double(v.size()));
  return s;
}

static cali::Annotation* g_annot = nullptr;  // cached-attribute Caliper handle

// ---------------------------------------------------------------------------
// Part A: overhead benchmarks (one timed batch of K invocations per sample)
// ---------------------------------------------------------------------------

#define DEFINE_BM(NAME, ...)                                                       \
  static std::vector<double> BM_##NAME##_samples;                                   \
  static void BM_##NAME(benchmark::State& state) {                                 \
    constexpr int K = 2000;                                                        \
    for (auto _ : state) {                                                         \
      uint64_t t0 = rdtsc_begin();                                                 \
      for (int i = 0; i < K; ++i) { __VA_ARGS__ }                                  \
      uint64_t t1 = rdtsc_end();                                                   \
      BM_##NAME##_samples.push_back(double(t1 - t0) / double(K));                  \
    }                                                                              \
  }                                                                                \
  BENCHMARK(BM_##NAME)->MinTime(0.5);

DEFINE_BM(baseline_empty, asm volatile("");)

DEFINE_BM(latte_fast,
  Latte::Fast::Start("LATTE_F");
  Latte::Fast::Stop(nullptr);)

DEFINE_BM(latte_mid,
  Latte::Mid::Start("LATTE_M");
  Latte::Mid::Stop(nullptr);)

DEFINE_BM(latte_hard,
  Latte::Hard::Start("LATTE_H");
  Latte::Hard::Stop(nullptr);)

DEFINE_BM(latte_pulse,
  LATTE_PULSE("LATTE_P");)

DEFINE_BM(caliper_mark,
  CALI_MARK_BEGIN("CAL_MARK");
  CALI_MARK_END("CAL_MARK");)

DEFINE_BM(caliper_annot,
  g_annot->begin();
  g_annot->end();)

DEFINE_BM(likwid_mark,
  LIKWID_MARKER_START("LIK_MARK");
  LIKWID_MARKER_STOP("LIK_MARK");)

DEFINE_BM(tracy_zone,
  { ZoneScopedN("TRACY_Z"); })

DEFINE_BM(chrono_now2,
  auto t1 = std::chrono::high_resolution_clock::now();
  auto t2 = std::chrono::high_resolution_clock::now();
  auto cd = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
  do_not_optimize(cd);)

// ---------------------------------------------------------------------------
// Part B: measurement error vs truth
// ---------------------------------------------------------------------------

static void print_ns_row(const char* name, const Stats& s) {
  std::printf("%-10s mean=%10.2f med=%10.2f std=%10.2f min=%10.2f max=%10.2f (ns)\n",
              name, s.mean, s.median, s.stddev, s.min, s.max);
}

static void RunErrorAnalysis() {
  constexpr int    KREG = 2000;        // regions per tool
  constexpr uint64_t TARGET = 20000;   // target busy-workload cycles

  std::printf("=== ERROR ANALYSIS (target ~%llu cycles) ===\n",
              (unsigned long long)TARGET);

  uint64_t k = calibrate_busy_k(TARGET);

  // --- truth: median of 200 rdtsc-bracketed measurements ---
  std::vector<double> truth;
  for (int i = 0; i < 200; ++i) {
    uint64_t t0 = rdtsc_begin();
    busy_work(k);
    uint64_t t1 = rdtsc_end();
    truth.push_back(double(t1 - t0));
  }
  Stats ts = compute_stats(truth);
  std::printf("TRUTH cycles: "); print_ns_row("truth", ts);

  // --- Latte Fast Start/Stop ---
  for (int i = 0; i < KREG; ++i) {
    Latte::Fast::Start("ERR");
    busy_work(k);
    Latte::Fast::Stop(nullptr);
  }
  auto latte_raw = Latte::Snapshot("ERR");
  std::vector<double> latte_ns;
  for (uint64_t c : latte_raw) latte_ns.push_back(double(c) / g_cps);
  Stats ls = compute_stats(latte_ns);
  std::printf("LATTE (Fast) %d samples\n", (int)latte_raw.size());
  print_ns_row("latte", ls);

  // --- std::chrono ---
  std::vector<double> chrono_ns;
  for (int i = 0; i < KREG; ++i) {
    auto t1 = std::chrono::high_resolution_clock::now();
    busy_work(k);
    auto t2 = std::chrono::high_resolution_clock::now();
    chrono_ns.push_back(
        double(std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count()));
  }
  Stats cs = compute_stats(chrono_ns);
  print_ns_row("chrono", cs);

  // --- Caliper markers (report printed by runtime-report config at exit) ---
  for (int i = 0; i < KREG; ++i) {
    CALI_MARK_BEGIN("ERR");
    busy_work(k);
    CALI_MARK_END("ERR");
  }

  // --- Likwid markers (report printed by likwid-perfctr at exit) ---
  for (int i = 0; i < KREG; ++i) {
    LIKWID_MARKER_START("ERR");
    busy_work(k);
    LIKWID_MARKER_STOP("ERR");
  }

  std::printf("=== END ERROR ANALYSIS (k=%llu) ===\n", (unsigned long long)k);
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  bool error_mode = false;
  for (int i = 1; i < argc; ++i) {
    if (argv[i] && std::strcmp(argv[i], "--error") == 0) {
      error_mode = true;
      argv[i] = nullptr;
    }
  }
  // compact argv (drop nulled "--error")
  int n = 1;
  for (int i = 1; i < argc; ++i)
    if (argv[i]) argv[n++] = argv[i];
  argv[n] = nullptr;
  argc = n;

  benchmark::Initialize(&argc, argv);

  PinThread(3);
  calibrate_cps();

  g_annot = new cali::Annotation("BenchAnnot");
  likwid_markerInit();

  const char* cfg = std::getenv("CALI_CONFIG");
  std::printf("=== ANNOT BENCH | caliper=%s | likwid=compiled-in | tracy=ENABLED | cpu=%.2f GHz | cps=%.4f ===\n",
              (cfg && *cfg) ? cfg : "inactive",
              g_cps, g_cps);

  if (error_mode) {
    RunErrorAnalysis();
    delete g_annot;
    likwid_markerClose();
    return 0;
  }

  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();

  // Print cycle-accurate per-primitive stats from our own RDTSC samples.
  std::printf("\n=== PER-PRIMITIVE CYCLES (RDTSC, median of batch means) ===\n");
  std::printf("%-22s %10s %10s %10s %10s %10s\n",
              "benchmark", "mean", "median", "stddev", "min", "max");
  auto print_stats = [](const char* name, std::vector<double>& v) {
    if (v.empty()) {
      std::printf("%-22s %10s (no samples - filtered out)\n", name, "-");
      return;
    }
    Stats s = compute_stats(v);
    std::printf("%-22s %10.2f %10.2f %10.2f %10.2f %10.2f  (n=%zu)\n",
                name, s.mean, s.median, s.stddev, s.min, s.max, v.size());
  };
  print_stats("baseline_empty", BM_baseline_empty_samples);
  print_stats("latte_fast", BM_latte_fast_samples);
  print_stats("latte_mid", BM_latte_mid_samples);
  print_stats("latte_hard", BM_latte_hard_samples);
  print_stats("latte_pulse", BM_latte_pulse_samples);
  print_stats("caliper_mark", BM_caliper_mark_samples);
  print_stats("caliper_annot", BM_caliper_annot_samples);
  print_stats("likwid_mark", BM_likwid_mark_samples);
  print_stats("tracy_zone", BM_tracy_zone_samples);
  print_stats("chrono_now2", BM_chrono_now2_samples);

  delete g_annot;
  likwid_markerClose();
  return 0;
}
