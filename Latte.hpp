#pragma GCC optimize ("O3")
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
    constexpr size_t MAX_ACTIVE_SLOTS = 64;
    constexpr size_t MAX_SAMPLES = 262144; // Power of 2 for better alignment
    using ID = const char*;
    using Cycles = uint64_t;

    struct Intrinsic {
        static inline Cycles RDTSC() { return __rdtsc(); }
        static inline Cycles RDTSCP() { unsigned int aux; return __rdtscp(&aux); }
        static inline Cycles RDTSCP_LFENCE() { _mm_lfence(); unsigned int aux; return __rdtscp(&aux); }
    };

    struct ThreadStorage {
        // Flat, pre-allocated event log to ensure O(1) symmetry in Start/Stop
        struct RawEvent { ID id; Cycles ts; bool is_start; };
        RawEvent events[MAX_SAMPLES];
        size_t event_ptr = 0;

        ThreadStorage() : event_ptr(0) {}
    };

    class Manager {
    public:
        std::mutex mutex;
        std::vector<ThreadStorage*> thread_buffers;
        double cycles_per_ns = 1.0;
        static Manager& Get() { static Manager instance; return instance; }

        Manager() { Calibrate(); }
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
        // ~9 cycles: Capture and flat write
        static inline void Start(ID id) {
            ThreadStorage* ts = GetThreadStorage();
            if (ts->event_ptr < MAX_SAMPLES) {
                ts->events[ts->event_ptr++] = { id, TimeFunc(), true };
            }
        }

        // ~9 cycles: Capture and flat write (Symmetry achieved)
        static inline void Stop(ID id) {
            Cycles now = TimeFunc();
            ThreadStorage* ts = GetThreadStorage();
            if (ts->event_ptr < MAX_SAMPLES) {
                ts->events[ts->event_ptr++] = { id, now, false };
            }
        }
    };

    namespace Fast { inline void Start(ID id) { Recorder<Intrinsic::RDTSC>::Start(id); } inline void Stop(ID id) { Recorder<Intrinsic::RDTSC>::Stop(id); } }
    namespace Mid  { inline void Start(ID id) { Recorder<Intrinsic::RDTSCP>::Start(id); } inline void Stop(ID id) { Recorder<Intrinsic::RDTSCP>::Stop(id); } }
    namespace Hard { inline void Start(ID id) { Recorder<Intrinsic::RDTSCP_LFENCE>::Start(id); } inline void Stop(ID id) { Recorder<Intrinsic::RDTSCP_LFENCE>::Stop(id); } }

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

        {
            std::lock_guard<std::mutex> lock(mgr.mutex);
            for (auto* ts : mgr.thread_buffers) {
                // Post-mortem matching: Reconstruct durations using a stack
                ID     shadow_stack_ids[MAX_ACTIVE_SLOTS];
                Cycles shadow_stack_ts[MAX_ACTIVE_SLOTS];
                size_t s_ptr = 0;

                for (size_t i = 0; i < ts->event_ptr; ++i) {
                    auto& ev = ts->events[i];
                    if (ev.is_start) {
                        if (s_ptr < MAX_ACTIVE_SLOTS) {
                            shadow_stack_ids[s_ptr] = ev.id;
                            shadow_stack_ts[s_ptr] = ev.ts;
                            s_ptr++;
                        }
                    } else {
                        if (s_ptr > 0) {
                            s_ptr--;
                            // Calculate delta in cold path
                            Cycles delta = ev.ts - shadow_stack_ts[s_ptr];
                            global_data[ev.id].push_back(delta / mgr.cycles_per_ns);
                        }
                    }
                }
                ts->event_ptr = 0; // Clear the log after processing
            }
        }

        // Report headers and statistics processing...
        oss << std::string(140, '=') << "\nLATTE TELEMETRY REPORT (DEFERRED AGGREGATION)\n" << std::string(140, '=') << "\n";
        oss << std::left << std::setw(25) << "COMPONENT NAME" << std::right << std::setw(8) << "SAMPLES" << std::setw(12) << "AVG" << std::setw(12) << "MEDIAN" << std::setw(12) << "STD" << std::setw(10) << "SKEW" << std::setw(12) << "P99" << "\n";
        oss << std::string(140, '-') << "\n";

        for (auto& [id, times] : global_data) {
            if (times.empty()) continue;
            size_t n = times.size();
            std::sort(times.begin(), times.end());
            double sum = 0; for(double t : times) sum += t;
            double avg = sum / n;
            double median = (n % 2 == 0) ? (times[n/2 - 1] + times[n/2]) / 2.0 : times[n/2];
            double variance_sum = 0, skew_sum = 0;
            for(double t : times) { double diff = t - avg; variance_sum += diff * diff; skew_sum += diff * diff * diff; }
            double std_dev = std::sqrt(variance_sum / n);
            double skew = (n > 1 && std_dev > 0) ? (skew_sum / n) / (std_dev * std_dev * std_dev) : 0;

            oss << std::left << std::setw(25) << id << std::right << std::setw(8) << n << std::setw(12) << FormatTime(avg) << std::setw(12) << FormatTime(median) << std::setw(12) << FormatTime(std_dev) << std::setw(10) << std::fixed << std::setprecision(2) << skew << std::setw(12) << FormatTime(times[(size_t)(n * 0.99)]) << "\n";
        } oss << std::string(140, '=') << std::endl;
    }
}
