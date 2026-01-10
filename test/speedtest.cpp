#include <iostream>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <iomanip>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <cstring>
#include <memory>
#include <sstream>
#include <chrono>

#include "Latte.hpp"

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

constexpr int ITERATIONS = 100000;
constexpr int SAMPLES = 100;

struct BenchResult {
    double avg, med, min, max, std_dev;
    std::string name;
};

#define BENCHMARK(NAME, CODE_BLOCK) \
    ([]() -> std::unique_ptr<BenchResult> { \
        auto samples_ptr = std::make_unique<double[]>(SAMPLES); \
        double* samples = samples_ptr.get(); \
        \
        /* Warmup */ \
        for(volatile int w=0; w < (ITERATIONS); w+=1) { CODE_BLOCK } \
        \
        uint64_t start, end; \
        \
        for(int s=0; s < SAMPLES; ++s) { \
            start = rdtsc_begin(); \
            for(int i=0; i < ITERATIONS; ++i) { \
                CODE_BLOCK \
            } \
            end = rdtsc_end(); \
            samples[s] = (double)(end - start) / ITERATIONS; \
        } \
        \
        /* Statistics */ \
        std::sort(samples, samples + SAMPLES); \
        double sum = 0.0; \
        for(int i=0; i<SAMPLES; ++i) sum += samples[i]; \
        \
        double avg = sum / SAMPLES; \
        double med = samples[SAMPLES / 2]; \
        double min = samples[0]; \
        double max = samples[SAMPLES - 1]; \
        \
        double var_sum = 0; \
        for(int i=0; i<SAMPLES; ++i) \
            var_sum += (samples[i] - avg) * (samples[i] - avg); \
        double std_dev = std::sqrt(var_sum / SAMPLES); \
        \
        return std::make_unique<BenchResult>(BenchResult{avg, med, min, max, std_dev, NAME}); \
    })()

void PrintResult(const BenchResult& r, double baseline) {
    // Pure Latency calculations (subtracting latency of ACTION(CODE_BLOCK -> TARGET_CODE))
    double adj_med = (r.med - baseline) > 0 ? (r.med - baseline) : 0.0;
    double adj_avg = (r.avg - baseline) > 0 ? (r.avg - baseline) : 0.0;
    double adj_min = (r.min - baseline) > 0 ? (r.min - baseline) : 0.0;
    double adj_max = (r.max - baseline) > 0 ? (r.max - baseline) : 0.0;

    double delta  = r.max - r.min;

    std::cout << "| " << std::left  << std::setw(23) << r.name
              << " | " << std::right << std::fixed << std::setprecision(1) << std::setw(8) << r.med // Raw Total
              << " | " << "\033[1;34m" << std::setw(8) << adj_med << "\033[0m" // Cost (Blue)
              << " | " << std::setw(8) << adj_avg
              << " | " << std::setw(8) << adj_min
              << " | " << std::setw(8) << adj_max
              << " | " << std::setw(8) << r.std_dev
              << " | " << std::setw(8) << delta
              << " |" << std::endl;
}

int main() {
    PinThread(3);

    std::cout << "+======================================================================================================+" << std::endl;
    std::cout << "| LATTE LATENCY BENCHMARK (Cycles per Operation)                                                       |" << std::endl;
    std::cout << "+======================================================================================================+" << std::endl;

    auto r_baseline = BENCHMARK("Baseline (Empty Loop)", {
        asm volatile("");
    });

    std::cout << "| Loop Overhead Baseline: \033[1;34m" << std::fixed << std::setprecision(2) << r_baseline->med << "\033[0m cycles/iter                                                             |" << std::endl;
    std::cout << "+=========================+==========+==========+==========+==========+==========+==========+==========+" << std::endl;

    // Table Header
    std::cout << "| " << std::left  << std::setw(23) << "Benchmark Name"
              << " | " << std::right << std::setw(8) << "Total"
              << " | " << std::right << std::setw(8) << "Cost"
              << " | " << std::right << std::setw(8) << "Avg"
              << " | " << std::right << std::setw(8) << "Min"
              << " | " << std::right << std::setw(8) << "Max"
              << " | " << std::right << std::setw(8) << "StdDev"
              << " | " << std::right << std::setw(8) << "Delta"
              << " |" << std::endl;
    std::cout << "+-------------------------+----------+----------+----------+----------+----------+----------+----------+" << std::endl;


    auto r_rdtsc = BENCHMARK("__rdtsc", { do_not_optimize(__rdtsc()); });
    auto r_rdtscp = BENCHMARK("__rdtscp", { unsigned int aux; do_not_optimize(__rdtscp(&aux)); });
    auto r_LFENCE = BENCHMARK("_LFENCE", { _mm_lfence(); });

    PrintResult(*r_rdtsc, r_baseline->med);
    PrintResult(*r_rdtscp, r_baseline->med);
    PrintResult(*r_LFENCE, r_baseline->med);
    std::cout << "+-------------------------+----------+----------+----------+----------+----------+----------+----------+" << std::endl;


    auto r_fast = BENCHMARK("Fast::Start + Stop", {
        Latte::Fast::Start("BenchFast");
        Latte::Fast::Stop(nullptr);
    });

    auto r_mid = BENCHMARK("Mid::Start + Stop", {
        Latte::Mid::Start("BenchMid");
        Latte::Mid::Stop(nullptr);
    });

    auto r_hard = BENCHMARK("Hard::Start + Stop", {
        Latte::Hard::Start("BenchHard");
        Latte::Hard::Stop(nullptr);
    });

    PrintResult(*r_fast, r_baseline->med);
    PrintResult(*r_mid, r_baseline->med);
    PrintResult(*r_hard, r_baseline->med);
    std::cout << "+-------------------------+----------+----------+----------+----------+----------+----------+----------+" << std::endl;


    auto r_pulse = BENCHMARK("LATTE_PULSE (Loop)", {
        LATTE_PULSE("BenchPulse");
    });
    PrintResult(*r_pulse, r_baseline->med);
    std::cout << "+-------------------------+----------+----------+----------+----------+----------+----------+----------+" << std::endl;


    auto r_chrono = BENCHMARK("std::chrono::now()", {
         auto t1 = std::chrono::high_resolution_clock::now();
         auto cd = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - t1).count();
    });
    PrintResult(*r_chrono, r_baseline->med);
    std::cout << "+-------------------------+----------+----------+----------+----------+----------+----------+----------+" << std::endl;

    Latte::DumpToStream(std::cout, Latte::Parameter::Cycle, Latte::Parameter::Calibrated);

    return 0;
}
