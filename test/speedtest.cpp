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
        /* Warmup: Run without timing to populate I-Cache/D-Cache/TLB */ \
        /* Volatile loop prevents compiler from removing "useless" code */ \
        for(volatile int w=0; w < (ITERATIONS); ++w) { CODE_BLOCK } \
        \
        uint64_t start, end; /* alloc cold path */\
        \
        for(int s=0; s < SAMPLES; ++s) { \
            start = rdtsc_begin(); \
            for(int i=0; i < ITERATIONS; ++i) { \
                CODE_BLOCK \
            } \
            end = rdtsc_end(); \
            \
            /* delta after end beacon */ \
            samples[s] = (double)(end - start) / ITERATIONS; \
        } \
        \
        /* Statistics*/ \
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
    // Pure Latency = Measured - EmptyLoopCost
    double pure_cost = r.med - baseline;
    if (pure_cost < 0) pure_cost = 0;

    std::stringstream ss;
    ss << std::fixed << std::setprecision(1);
    ss << "Range: [" << r.min << " - " << r.max << "]";
    std::string rangeStr = ss.str();
    std::cout << "| " << std::left << std::setw(25) << r.name
              << "| Total: " << std::fixed << std::setprecision(1) << std::setw(6) << r.med
              << " | Overhead: " << "\033[1;34m" << std::setw(6) << pure_cost << "\033[0m"
              << " | " << std::left << std::setw(25) << rangeStr << " |" << std::endl;
}


int main() {
    PinThread(2); // isolated from OS interrupts

    std::cout << "+=========================================================================================+\n";
    std::cout << "| LATTE LATENCY BENCHMARK (Cycles per Operation)                                          |\n";
    std::cout << "+=========================================================================================+\n";

    // Measure Baseline (Empty Loop Cost)
    auto r_baseline = BENCHMARK("Baseline (Empty Loop)", {
        asm volatile("");
    });

    // Curiosity
    auto r_rdtsc = BENCHMARK("__rdtsc", {
        do_not_optimize(__rdtsc());
    });
    auto r_rdtscp = BENCHMARK("__rdtscp", {
        unsigned int aux;
        do_not_optimize(__rdtscp(&aux));
    });
    auto r_LFENCE = BENCHMARK("_LFENCE", {
        _mm_lfence();
    });

    std::cout << "| Loop Overhead detected: \033[1;34m" << r_baseline->med << "\033[0m cycles/iter                                             |\n";
    std::cout << "+-----------------------------------------------------------------------------------------+\n";

    // Measure Start+Stop PAIRS (::stop() expect values fetched from ::start())
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

    // Measure PULSE (The Optimized Loop Method)
    auto r_pulse = BENCHMARK("LATTE_PULSE (Loop)", {
        LATTE_PULSE("BenchPulse");
    });

    // Against std::chrono::now()
    auto r_chrono = BENCHMARK("std::chrono::now()", {
         auto t1 = std::chrono::high_resolution_clock::now();
         auto cd = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - t1).count();
    });


    PrintResult(*r_rdtsc, r_baseline->med);
    PrintResult(*r_rdtscp, r_baseline->med);
    PrintResult(*r_LFENCE, r_baseline->med);
    std::cout << "+-----------------------------------------------------------------------------------------+\n";
    PrintResult(*r_fast, r_baseline->med);
    PrintResult(*r_mid, r_baseline->med);
    PrintResult(*r_hard, r_baseline->med);
    std::cout << "+-----------------------------------------------------------------------------------------+\n";
    PrintResult(*r_pulse, r_baseline->med);
    std::cout << "+-----------------------------------------------------------------------------------------+\n";
    PrintResult(*r_chrono, r_baseline->med);
    std::cout << "+=========================================================================================+" << std::endl;
    return 0;
}
