#include <x86intrin.h>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <pthread.h>
#include <unistd.h>
#include <cstdlib>
#include <sched.h>

#include "Latte.hpp"

static inline uint64_t tsc_begin() {
    _mm_lfence();
    uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

static inline uint64_t tsc_end() {
    unsigned aux;
    uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

struct Stats {
    double avg;
    double median;
    double stddev;
    double min;
    double max;
};

template <typename F>
Stats measure_func(F fn, int trials, int batch, int warmups) {
    uint64_t overhead = UINT64_MAX;
    for (int i = 0; i < 100000; i++) {
        uint64_t a = tsc_begin();
        uint64_t b = tsc_end();
        uint64_t d = b - a;
        if (d < overhead) overhead = d;
    }

    for (int w = 0; w < warmups; ++w) {
        for (int i = 0; i < batch; ++i)
            fn();
    }

    std::vector<double> results;
    results.reserve(trials);

    for (int t = 0; t < trials; t++) {
        uint64_t start = tsc_begin();
        for (int i = 0; i < batch; i++) {
            fn();
        }
        uint64_t end = tsc_end();

        double cyc = double((end - start) - overhead) / batch;
        results.push_back(cyc);
    }

    std::sort(results.begin(), results.end());
    double sum = std::accumulate(results.begin(), results.end(), 0.0);
    double avg = sum / results.size();

    double med;
    if (results.size() % 2 == 0) {
        med = 0.5 * (results[results.size()/2 - 1] + results[results.size()/2]);
    } else {
        med = results[results.size()/2];
    }

    double var = 0;
    for (double x : results) {
        double diff = x - avg;
        var += diff * diff;
    }
    var /= results.size();
    double stddev = std::sqrt(var);
    auto min = std::min_element(results.begin(),results.end());
    auto max = std::max_element(results.begin(), results.end());
    return {avg, med, stddev, *min, *max};
}

int main() {
    int core_id = 0;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_t current_thread = pthread_self();
    int result = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
    if (result != 0) {
        std::cerr << "Error pinning thread" << std::endl;
        return 1;
    }

  
    constexpr int TRIALS = 50;
    constexpr int BATCH = 5000000;
    constexpr int WARMUPS = 3;

    auto fFastStart = []() { Latte::Fast::Start("1"); };
    auto fFastEnd = []() { Latte::Fast::Stop("1"); };
    auto fMidStart = []() { Latte::Mid::Start("2"); };
    auto fMidEnd = []() { Latte::Mid::Stop("2"); };
    auto fHardStart = []() { Latte::Hard::Start("3"); };
    auto fHardEnd = []() { Latte::Hard::Stop("3"); };

    Stats s;
    std::cout << "Latte Report: " << std::endl;
    s = measure_func(fFastStart, TRIALS, BATCH, WARMUPS);
    printf("\nLatte::Fast::Start: avg=%.2f med=%.2f stddev=%.2f min=%.2f max=%.2f cycles\n", s.avg, s.median, s.stddev, s.min, s.max);

    s = measure_func(fFastEnd, TRIALS, BATCH, WARMUPS);
    printf("Latte::Fast::Stop : avg=%.2f med=%.2f stddev=%.2f min=%.2f max=%.2f cycles\n", s.avg, s.median, s.stddev, s.min, s.max);

    s = measure_func(fMidStart, TRIALS, BATCH, WARMUPS);
    printf("\nLatte::Mid::Start : avg=%.2f med=%.2f stddev=%.2f min=%.2f max=%.2f cycles\n", s.avg, s.median, s.stddev, s.min, s.max);

    s = measure_func(fMidEnd, TRIALS, BATCH, WARMUPS);
    printf("Latte::Mid::Stop  : avg=%.2f med=%.2f stddev=%.2f min=%.2f max=%.2f cycles\n", s.avg, s.median, s.stddev, s.min, s.max);

    s = measure_func(fHardStart, TRIALS, BATCH, WARMUPS);
    printf("\nLatte::Hard::Start: avg=%.2f med=%.2f stddev=%.2f min=%.2f max=%.2f cycles\n", s.avg, s.median, s.stddev, s.min, s.max);

    s = measure_func(fHardEnd, TRIALS, BATCH, WARMUPS);
    printf("Latte::Hard::Stop : avg=%.2f med=%.2f stddev=%.2f min=%.2f max=%.2f cycles\n", s.avg, s.median, s.stddev, s.min, s.max);
    return 0;
}
