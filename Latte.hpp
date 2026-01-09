#pragma GCC optimize ("O3")
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
#include <chrono>
#include <fstream>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

// It automatically handles the first-run initialization and caching
#define LATTE_PULSE(id_str) \
    do { \
        static thread_local Latte::RingBuffer* _l_rb = nullptr; \
        static thread_local uint64_t _l_last = 0; \
        if (__builtin_expect(!_l_rb, 0)) { \
            _l_rb = Latte::Internal::GetBuffer(id_str); \
            _l_last = Latte::Intrinsic::RDTSC(); \
        } else { \
            uint64_t _l_now = Latte::Intrinsic::RDTSC(); \
            _l_rb->push(_l_now - _l_last); \
            _l_last = _l_now; \
        } \
    } while(0)

namespace Latte {
    using ID = const char*;
    using Cycles = uint64_t;
    constexpr size_t MAX_ACTIVE_SLOTS = 64;

    constexpr size_t BUFFER_PWR = 16;
    constexpr size_t MAX_SAMPLES = 1 << BUFFER_PWR; // 65536
    constexpr size_t BUFFER_MASK = MAX_SAMPLES - 1;

    struct Intrinsic {
        __attribute__((always_inline)) static inline Cycles RDTSC() { return __rdtsc(); }
        __attribute__((always_inline)) static inline Cycles RDTSCP() { unsigned int aux; return __rdtscp(&aux); }
        __attribute__((always_inline)) static inline Cycles RDTSCP_LFENCE() { _mm_lfence(); unsigned int aux; return __rdtscp(&aux); }
    };

    struct alignas(64) RingBuffer {
        Cycles data[MAX_SAMPLES];
        size_t head = 0;

        __attribute__((always_inline)) inline void push(Cycles val) {
            data[head] = val;
            head = (head + 1) & BUFFER_MASK; // Fast Bitwise wrapping
        }
    };

    struct ThreadStorage {
        ID stack_ids[MAX_ACTIVE_SLOTS];
        Cycles stack_starts[MAX_ACTIVE_SLOTS];
        size_t stack_ptr = 0;

        // pointer comparison
        std::map<ID, RingBuffer> history;
        __attribute__((always_inline)) inline RingBuffer* GetOrAdd(ID id) {
            return &history[id];
        }
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
            double ns = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
            cycles_per_ns = (double)(c2 - c1) / ns;
        }
        void Register(ThreadStorage* ts) {
            std::lock_guard<std::mutex> lock(mutex);
            thread_buffers.push_back(ts);
        }

        // Non-blocking Data Extraction
        // Returns all valid samples collected so far for a specific ID
        // Thread-safe relative to the writers (atomics)
        std::vector<Cycles> ExtractRaw(ID id) {
            std::vector<Cycles> output;
            output.reserve(1024);

            std::lock_guard<std::mutex> lock(mutex);

            for (auto* ts : thread_buffers) {
                // Safety: map lookup isn't technically thread-safe if keys are added dynamically during extraction, but usually ID set is stable
                auto it = ts->history.find(id);
                if (it != ts->history.end()) {
                    RingBuffer& rb = it->second;
                    for(size_t i=0; i<MAX_SAMPLES; ++i) {
                        Cycles v = rb.data[i];
                        if(v > 0) output.push_back(v);
                    }
                }
            }
            return output;
        }
    };

    inline ThreadStorage* GetThreadStorage() {
        static thread_local ThreadStorage* ts = nullptr;
        if (__builtin_expect(!ts, 0)) {
            ts = new ThreadStorage();
            Manager::Get().Register(ts);
        }
        return ts;
    }

    namespace Internal {
        inline RingBuffer* GetBuffer(ID id) { return GetThreadStorage()->GetOrAdd(id); }
    }

    template <Cycles (*TimeFunc)()>
    struct Recorder {
        __attribute__((always_inline)) static inline void Start(ID id) {
            ThreadStorage* ts = GetThreadStorage();
            if (__builtin_expect(ts->stack_ptr < MAX_ACTIVE_SLOTS, 1)) {
                ts->stack_ids[ts->stack_ptr] = id;
                ts->stack_starts[ts->stack_ptr] = TimeFunc();
                ts->stack_ptr++;
            }
        }

        __attribute__((always_inline)) static inline Cycles Stop(ID id) {
            Cycles end = TimeFunc();
            ThreadStorage* ts = GetThreadStorage();

            if (__builtin_expect(ts->stack_ptr > 0, 1)) {
                ts->stack_ptr--;
                Cycles delta = end - ts->stack_starts[ts->stack_ptr]; // Fast bitwise push
                ts->history[ts->stack_ids[ts->stack_ptr]].push(delta);
                return delta; //Return the raw latency
            }
            return 0;
        }
    };
    namespace Fast { inline void Start(ID id) { Recorder<Intrinsic::RDTSC>::Start(id); } inline void Stop(ID id) { Recorder<Intrinsic::RDTSC>::Stop(id); } }
    namespace Mid  { inline void Start(ID id) { Recorder<Intrinsic::RDTSCP>::Start(id); } inline void Stop(ID id) { Recorder<Intrinsic::RDTSCP>::Stop(id); } }
    namespace Hard { inline void Start(ID id) { Recorder<Intrinsic::RDTSCP_LFENCE>::Start(id); } inline void Stop(ID id) { Recorder<Intrinsic::RDTSCP_LFENCE>::Stop(id); } }
    inline std::vector<Cycles> Snapshot(ID id) {
        return Manager::Get().ExtractRaw(id);
    }
    
    inline std::string FormatTime(double ns) {
        std::stringstream ss; ss << std::fixed << std::setprecision(2);
        if (ns < 1000.0) ss << ns << " ns";
        else if (ns < 1e6) ss << (ns / 1000.0) << " us";
        else ss << (ns / 1e6) << " ms";
        return ss.str();
    }

    inline void DumpToStream(std::ostream& oss, bool InTime = true) {
        Manager& mgr = Manager::Get();
        std::map<ID, std::vector<double>> global_data;

        { // Thread-safe data collection
            std::lock_guard<std::mutex> lock(mgr.mutex);
            for (auto* ts : mgr.thread_buffers) {
                for (auto& [id, buffer] : ts->history) {
                    // Scan buffer for non-zero entries
                    for (size_t i = 0; i < MAX_SAMPLES; ++i) {
                        if (buffer.data[i] > 0) {
                            global_data[id].push_back(static_cast<double>(buffer.data[i]));
                        }
                    }
                }
            }
        }

        auto FormatLarge = [](double val) {
            const char* units[] = {"", "K", "M", "B", "T"};
            int unit_idx = 0;
            while (val >= 1000.0 && unit_idx < 4) {
                val /= 1000.0;
                unit_idx++;
            }
            std::ostringstream ss;
            if (unit_idx == 0) ss << std::fixed << std::setprecision(0) << val;
            else ss << std::fixed << std::setprecision(2) << val << " " << units[unit_idx];
            return ss.str();
        };

        const int C1 = 25; // Name
        const int C2 = 10; // Samples
        const int C3 = 13; // Avg
        const int C4 = 13; // Median
        const int C5 = 13; // Std Dev
        const int C6 = 10; // Skew
        const int C7 = 13; // Min
        const int C8 = 13; // Max
        const int C9 = 13; // delta Range

        const int TABLE_WIDTH = C1 + C2 + C3 + C4 + C5 + C6 + C7 + C8 + C9 + 2;
        const std::string line(TABLE_WIDTH, '-');
        const std::string d_line(TABLE_WIDTH, '=');

        auto col = [](const std::string& s, int width, bool left = false) {
            std::ostringstream ss;
            if (left) ss << std::left << std::setw(width) << s;
            else      ss << std::right << std::setw(width) << s;
            std::string res = ss.str();
            return (res.length() > (size_t)width) ? res.substr(0, width) : res;
        };

        auto ToNS = [&](double value) {
            return InTime ? FormatTime(value / mgr.cycles_per_ns) : FormatLarge(value);
        };

        oss << "\n#" << d_line << "#\n";
        std::string title = "LATTE TELEMETRY [" + std::string(InTime ? "TIME" : "CYCLES") + "]";
        oss << "| " << col(title, TABLE_WIDTH - 1, true) << "|\n";
        oss << "#" << d_line << "#\n";

        oss << "| " << col("COMPONENT", C1, true)
            << col("SAMPLES", C2)
            << col("AVG", C3)
            << col("MEDIAN", C4)
            << col("STD DEV", C5)
            << col("SKEW", C6)
            << col("MIN", C7)
            << col("MAX", C8)
            << col("RANGE (Δ)", C9) << " |\n";

        oss << "|" << line << "|\n";

        for (auto& [id, values] : global_data) {
            if (values.empty()) continue;
            std::sort(values.begin(), values.end());

            const size_t n = values.size();
            double sum = 0;
            for(double v : values) sum += v;
            const double avg = sum / n;
            const double median = (n % 2 == 0) ? (values[n/2 - 1] + values[n/2]) / 2.0 : values[n/2];

            double var_sum = 0, skew_sum = 0;
            for(double v : values) {
                double d = v - avg;
                var_sum += d * d;
                skew_sum += (d * d * d);
            }

            const double std_dev = std::sqrt(var_sum / n);
            const double skew = (n > 1 && std_dev > 1e-9) ? (skew_sum / n) / (std_dev * std_dev * std_dev) : 0.0;

            std::ostringstream sk;
            sk << std::fixed << std::setprecision(2) << skew;

            oss << "| " << col(id, C1, true)
                << col(std::to_string(n), C2)
                << col(ToNS(avg), C3)
                << col(ToNS(median), C4)
                << col(ToNS(std_dev), C5)
                << col(sk.str(), C6)
                << col(ToNS(values.front()), C7)
                << col(ToNS(values.back()), C8)
                << col(ToNS(values.back() - values.front()), C9) << " |\n";
        }

        oss << "#" << d_line << "#" << std::endl;
    }
}
