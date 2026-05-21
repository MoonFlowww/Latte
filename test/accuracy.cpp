#include <iostream>
#include <vector>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <iomanip>

#include <x86intrin.h>
#include <immintrin.h>

#include "../Latte.hpp"  

static double cycles_per_ns = 0.0;

static inline void busy_wait_ns(uint64_t target_ns) {
  if (cycles_per_ns <= 0.0) return;
  uint64_t target_cycles = static_cast<uint64_t>(target_ns * cycles_per_ns);
  uint64_t start = __rdtsc();
  uint64_t now;
  do {
    _mm_pause();
    now = __rdtsc();
  } while (now - start < target_cycles);
}

struct ChronoTimer {
  using TimePoint = std::chrono::high_resolution_clock::time_point;
  static TimePoint start() { return std::chrono::high_resolution_clock::now(); }
  static double elapsed_ns(TimePoint a, TimePoint b) {
    return std::chrono::duration<double, std::nano>(b - a).count();
  }
};

struct RdtscTimer {
  static uint64_t start() { return __rdtsc(); }
  static uint64_t elapsed_cycles(uint64_t a, uint64_t b) { return b - a; }
};

struct RdtscpTimer {
  static uint64_t start() {
    unsigned int aux;
    return __rdtscp(&aux);
  }
  static uint64_t elapsed_cycles(uint64_t a, uint64_t b) { return b - a; }
};

struct RdtscLfenceTimer {
  static uint64_t start() {
    _mm_lfence();
    return __rdtsc();
  }
  static uint64_t elapsed_cycles(uint64_t a, uint64_t b) { return b - a; }
};

struct Stats {
  double mean;
  double med;
  double stddev;
  double min;
  double max;
  double error_mean;
  double rel_error_pct;
};


static Stats compute_stats(const std::vector<double>& values, double target_ns) {
  if (values.empty()) return {};
  
  double med = [&]() -> double {
    if (values.empty()) return 0.0;
    std::vector<double> vals = values;
    std::sort(vals.begin(), vals.end());
    size_t n = vals.size();
    if (n % 2 == 0) {
      return (vals[n/2 - 1] + vals[n/2]) / 2.0;
    } else {
      return vals[n/2];
    }
  }();

  double sum = std::accumulate(values.begin(), values.end(), 0.0);
  double mean = sum / values.size();

  double sq_sum = std::inner_product(
    values.begin(), values.end(), values.begin(), 0.0,
    std::plus<>(),
    [mean](double a, double b) { return (a - mean) * (b - mean); }
  );

  double stddev = std::sqrt(sq_sum / values.size());
  
  double min = *std::min_element(values.begin(), values.end());
  double max = *std::max_element(values.begin(), values.end());

  double error_mean = mean - target_ns;
  double rel_error_pct = (error_mean / target_ns) * 100.0;

  return {mean, med, stddev, min, max, error_mean, rel_error_pct};
}

static void print_stats(const char* name, const Stats& s) {
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "[" << name << "]\n";
  std::cout << "  Avg (ns):       " << s.mean << "\n";
  std::cout << "  StdDev (ns):    " << s.stddev << "\n";
  std::cout << "  Median (ns):    " << s.med << "\n";
  std::cout << "  Min (ns):       " << s.min << "\n";
  std::cout << "  Max (ns):       " << s.max << "\n";
  std::cout << "  Error (ns):     " << s.error_mean << "\n";
  std::cout << "  Rel error (%):  " << s.rel_error_pct << "\n\n";
}

static void warm_up(int iterations, uint64_t sleep_ns) {
  for (int i = 0; i < iterations; ++i) {
    busy_wait_ns(sleep_ns);
  }
}

template<typename Timer>
void benchmark_timer(const char* name, uint64_t target_ns, int iters) {
  std::vector<double> elapsed_ns;
  elapsed_ns.reserve(iters);

  for (int iter = 0; iter < iters; ++iter) {
    auto t1 = Timer::start();
    busy_wait_ns(target_ns);
    auto t2 = Timer::start();

    double measured_ns;
    if constexpr (std::is_same_v<Timer, ChronoTimer>) {
      measured_ns = Timer::elapsed_ns(t1, t2);
    } else {
      uint64_t cycles = Timer::elapsed_cycles(t1, t2);
      measured_ns = static_cast<double>(cycles) / cycles_per_ns;
    }
    elapsed_ns.push_back(measured_ns);
  }

  Stats s = compute_stats(elapsed_ns, static_cast<double>(target_ns));
  print_stats(name, s);
}

struct FastPolicy {
  static void Start(const char* id) { Latte::Fast::Start(id); }
  static void Stop(const char* id)  { Latte::Fast::Stop(id); }
  static constexpr uint8_t mode_val = 0;
};

struct MidPolicy {
  static void Start(const char* id) { Latte::Mid::Start(id); }
  static void Stop(const char* id)  { Latte::Mid::Stop(id); }
  static constexpr uint8_t mode_val = 1;
};

struct HardPolicy {
  static void Start(const char* id) { Latte::Hard::Start(id); }
  static void Stop(const char* id)  { Latte::Hard::Stop(id); }
  static constexpr uint8_t mode_val = 2;
};

template<typename Policy>
void benchmark_latte_mode(const char* mode_name, uint64_t target_ns, int iters) {
  for (int iter = 0; iter < iters; ++iter) {
    Policy::Start(mode_name);
    busy_wait_ns(target_ns);
    Policy::Stop(mode_name);
  }

  auto snap = Latte::Snapshot(mode_name);
  if (snap.empty()) {
    std::cout << "[" << mode_name << "] No data collected.\n\n";
    return;
  }

  std::vector<double> elapsed_ns;
  elapsed_ns.reserve(snap.size());
  for (uint64_t cycles : snap) {
    elapsed_ns.push_back(static_cast<double>(cycles) / cycles_per_ns);
  }

  Stats s = compute_stats(elapsed_ns, static_cast<double>(target_ns));
  print_stats(mode_name, s);
}

int main() {
  const uint64_t sleep_ns = 10000ULL;
  const int warmup_iters = 5000;
  const int measure_iters = 100000;

  LATTE_FREQ(cycles_per_ns);
  std::cout << std::fixed << std::setprecision(3);
  std::cout 
    << "Cycles per ns: " << cycles_per_ns
    << " (approx. CPU frequency: " << cycles_per_ns << " GHz)\n\n";

  std::cout << "Warming up (" << warmup_iters << " iterations)...\n";
  warm_up(warmup_iters, sleep_ns);
  std::cout << "Measuring (" << measure_iters << " iterations per test):\n\n";

  benchmark_timer<ChronoTimer> ("std::chrono", sleep_ns, measure_iters);
  benchmark_timer<RdtscTimer> ("rdtsc", sleep_ns, measure_iters);
  benchmark_timer<RdtscpTimer> ("rdtscp", sleep_ns, measure_iters);
  benchmark_timer<RdtscLfenceTimer> ("rdtsc+lfence", sleep_ns, measure_iters);

  benchmark_latte_mode<FastPolicy> ("FastAcc", sleep_ns, measure_iters);
  benchmark_latte_mode<MidPolicy> ("MidAcc", sleep_ns, measure_iters);
  benchmark_latte_mode<HardPolicy> ("HardAcc", sleep_ns, measure_iters);

  //Latte::DumpToStream(std::cout, Latte::Parameter::Time, Latte::Parameter::Calibrated);
  return 0;
}
