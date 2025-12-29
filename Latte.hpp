#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <chrono>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace Latte {
    // --- Tuning Constants ---
    constexpr size_t MAX_ACTIVE_SLOTS = 16;   
    constexpr size_t MAX_SAMPLES = 100000;    

    using ID = const char*; 
    using Cycles = uint64_t;

    struct Intrinsic {
        static inline Cycles RDTSC() { return __rdtsc(); }
        static inline Cycles RDTSCP() { unsigned int aux; return __rdtscp(&aux); }
        static inline Cycles RDTSCP_LFENCE() { _mm_lfence(); unsigned int aux; return __rdtscp(&aux); }
    };

    struct ThreadStorage {
        struct ActiveSlot { ID id = nullptr; Cycles start = 0; };
        struct RingBuffer {
            Cycles data[MAX_SAMPLES];
            size_t head = 0; size_t count = 0;
            void push(Cycles val) {
                data[head] = val;
                head = (head + 1) % MAX_SAMPLES;
                if (count < MAX_SAMPLES) count++;
            }
        };

        ActiveSlot active_slots[MAX_ACTIVE_SLOTS];
        std::map<ID, RingBuffer> history;
        ThreadStorage() { for (auto& slot : active_slots) slot.id = nullptr; }
    };

    inline ThreadStorage* GetThreadStorage();

    class Manager {
    public:
        std::mutex mutex;
        std::vector<ThreadStorage*> thread_buffers;
        double cycles_per_ns = 1.0;
        
        // Self-profiling results (in nanoseconds)
        double fast_overhead_ns = 0;
        double mid_overhead_ns = 0;
        double hard_overhead_ns = 0;

        static Manager& Get() { static Manager instance; return instance; }

        Manager() { 
            Calibrate(); 
        }

        void Calibrate() {
            auto t1 = std::chrono::high_resolution_clock::now();
            Cycles c1 = Intrinsic::RDTSC();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            Cycles c2 = Intrinsic::RDTSC();
            auto t2 = std::chrono::high_resolution_clock::now();
            double ns_elapsed = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
            cycles_per_ns = (double)(c2 - c1) / ns_elapsed;
        }

        void RegisterThreadBuffer(ThreadStorage* ts) {
            std::lock_guard<std::mutex> lock(mutex);
            thread_buffers.push_back(ts);
        }
    };

    inline ThreadStorage* GetThreadStorage() {
        static thread_local ThreadStorage* ts = nullptr;
        if (!ts) {
            ts = new ThreadStorage();
            Manager::Get().RegisterThreadBuffer(ts);
        }
        return ts;
    }

    template <Cycles (*TimeFunc)()>
    struct Recorder {
        static inline void Start(ID id) {
            ThreadStorage* ts = GetThreadStorage();
            for (auto& slot : ts->active_slots) {
                if (slot.id == nullptr || slot.id == id) {
                    slot.id = id;
                    slot.start = TimeFunc();
                    return;
                }
            }
        }
        static inline void Stop(ID id) {
            Cycles end = TimeFunc();
            ThreadStorage* ts = GetThreadStorage();
            for (auto& slot : ts->active_slots) {
                if (slot.id == id) {
                    ts->history[id].push(end - slot.start);
                    slot.id = nullptr;
                    return;
                }
            }
        }
    };

    namespace Fast {
        inline void Start(ID id) { Recorder<Intrinsic::RDTSC>::Start(id); }
        inline void Stop(ID id) { Recorder<Intrinsic::RDTSC>::Stop(id); }
    }
    namespace Mid { 
        inline void Start(ID id) { Recorder<Intrinsic::RDTSCP>::Start(id); }
        inline void Stop(ID id) { Recorder<Intrinsic::RDTSCP>::Stop(id); }
    }
    namespace Hard { 
        inline void Start(ID id) { Recorder<Intrinsic::RDTSCP_LFENCE>::Start(id); }
        inline void Stop(ID id) { Recorder<Intrinsic::RDTSCP_LFENCE>::Stop(id); }
    }

    inline std::string FormatTime(double ns) {
        std::stringstream ss; ss << std::fixed << std::setprecision(2);
        if (ns < 1000.0) ss << ns << " ns";
        else if (ns < 1e6) ss << (ns / 1000.0) << " us";
        else ss << (ns / 1e6) << " ms";
        return ss.str();
    }

    inline void DumpToStream(std::ostream& oss) {
        Manager& mgr = Manager::Get();
        std::map<ID, std::vector<double>> global_data;


        { // Collect data from all threads
            std::lock_guard<std::mutex> lock(mgr.mutex);
            for (auto* ts : mgr.thread_buffers) {
                for (auto& [id, buffer] : ts->history) {
                    if (std::string(id) == "__internal_null_ping") continue;
                    for (size_t i = 0; i < buffer.count; ++i) {
                        global_data[id].push_back(buffer.data[i] / mgr.cycles_per_ns);
                    }
                }
            }
        }

        // Refresh Overhead Calculation
        auto measure_overhead = [&](auto start_func, auto stop_func) {
            const int iterations = 1000;
            Cycles total = 0;
            ID test_id = "__internal_null_ping";
            for(int i = 0; i < iterations; ++i) {
                Cycles s = Intrinsic::RDTSC();
                start_func(test_id);
                stop_func(test_id);
                Cycles e = Intrinsic::RDTSC();
                total += (e - s);
            }
            return (double)(total / iterations) / mgr.cycles_per_ns;
        };
        mgr.fast_overhead_ns = measure_overhead(Fast::Start, Fast::Stop);
	    mgr.mid_overhead_ns = measure_overhead(Mid::Start, Mid::Stop);
	    mgr.hard_overhead_ns = measure_overhead(Hard::Start, Hard::Stop);


        oss << std::string(140, '=') << "\n";
        oss << "LATTE TELEMETRY REPORT | Tax:\n";
	    oss << " - Fast(RDTSC): " << mgr.fast_overhead_ns << "ns\n";
	    oss << " - Mid (RDTSCP): " << mgr.mid_overhead_ns << "ns\n";
	    oss << " - Hard(RDTSCP+LFENCE): " << mgr.hard_overhead_ns << "ns\n";
        oss << std::string(140, '=') << "\n";
        
        oss << std::left << std::setw(25) << "COMPONENT NAME" << std::right 
            << std::setw(8) << "SAMPLES" 
            << std::setw(12) << "AVG" 
            << std::setw(12) << "MEDIAN" 
            << std::setw(12) << "STD"
            << std::setw(10) << "SKEW"
            << std::setw(12) << "P99" << "\n";
        oss << std::string(140, '-') << "\n";

        for (auto& [id, times] : global_data) {
            if (times.empty()) continue;
            size_t n = times.size();
            std::sort(times.begin(), times.end());

            // Average
            double sum = 0;
            for(double t : times) sum += t;
            double avg = sum / n;

            // Median
            double median = (n % 2 == 0) ? (times[n/2 - 1] + times[n/2]) / 2.0 : times[n/2];

            // Std Dev & Skewness
            double variance_sum = 0;
            double skew_sum = 0;
            for(double t : times) {
                double diff = t - avg;
                variance_sum += diff * diff;
                skew_sum += diff * diff * diff;
            }
            
            double std_dev = std::sqrt(variance_sum / n);
            double skew = 0;
            if (n > 1 && std_dev > 0) { // Fisher-Pearson coefficient of skewness
                skew = (skew_sum / n) / (std_dev * std_dev * std_dev);
            }

            oss << std::left << std::setw(25) << id << std::right 
                << std::setw(8) << n 
                << std::setw(12) << FormatTime(avg) 
                << std::setw(12) << FormatTime(median) 
                << std::setw(12) << FormatTime(std_dev) 
                << std::setw(10) << std::fixed << std::setprecision(2) << skew
                << std::setw(12) << FormatTime(times[(size_t)(n * 0.99)]) << "\n";
        }
        oss << std::string(140, '=') << std::endl;
    }
}
