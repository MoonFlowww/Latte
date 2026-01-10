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
#include <array>
#include <limits>
#include <cstring>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#define LATTE_PULSE(id_str) \
    do { \
        static thread_local Latte::RingBuffer* _l_rb = nullptr; \
        static thread_local uint64_t _l_last = 0; \
        if (__builtin_expect(!_l_rb, 0)) { \
            _l_rb = Latte::Internal::GetBuffer(id_str); \
            _l_last = Latte::Intrinsic::RDTSC(); \
        } else { \
            uint64_t _l_now = Latte::Intrinsic::RDTSC(); \
            _l_rb->push(_l_now - _l_last, Latte::Internal::CALIB_KEY_PULSE); \
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

    enum class Mode : uint8_t { Fast = 0, Mid = 1, Hard = 2 };

    namespace Internal {
        constexpr uint8_t CALIB_KEY_UNSET = 0xFF;
        constexpr uint8_t CALIB_KEY_MIXED = 0xFE;
        constexpr uint8_t CALIB_KEY_PULSE = 9;
        constexpr size_t  CALIB_KEY_COUNT = 10;

        __attribute__((always_inline)) static inline uint8_t CalibKey(uint8_t start_mode, uint8_t stop_mode) {
            return (start_mode < 3 && stop_mode < 3) ? static_cast<uint8_t>(start_mode * 3 + stop_mode) : CALIB_KEY_UNSET;
        }

        __attribute__((always_inline)) static inline void LFENCE() {
#if defined(_MSC_VER)
            _mm_lfence();
#else
            asm volatile ("lfence" ::: "memory");
#endif
        }

        // Calibration labels (single address across TUs)
        inline constexpr char CALIB_FxF[] = "FxF";
        inline constexpr char CALIB_FxM[] = "FxM";
        inline constexpr char CALIB_FxH[] = "FxH";
        inline constexpr char CALIB_MxF[] = "MxF";
        inline constexpr char CALIB_MxM[] = "MxM";
        inline constexpr char CALIB_MxH[] = "MxH";
        inline constexpr char CALIB_HxF[] = "HxF";
        inline constexpr char CALIB_HxM[] = "HxM";
        inline constexpr char CALIB_HxH[] = "HxH";
        inline constexpr char CALIB_PULSE[] = "PxP";

        struct CleanResult {
            std::vector<double> values; // sorted
            size_t bypass = 0; // OS bypass
            double cutoff = std::numeric_limits<double>::max();
        };

        inline CleanResult CleanData(const std::vector<double>& values) {
            CleanResult out;
            if (values.empty()) return out;

            std::vector<double> bucket_maxes;
            const size_t BUCKET_SIZE = 1000;

            for (size_t i = 0; i < values.size(); i += BUCKET_SIZE) {
                double b_max = 0;
                size_t end = std::min(i + BUCKET_SIZE, values.size());

                // Is bucket.size > 50%
                if ((end - i) < BUCKET_SIZE / 2) continue;

                for (size_t j = i; j < end; ++j) {
                    if (values[j] > b_max) b_max = values[j];
                }
                bucket_maxes.push_back(b_max);
            }

            double cutoff = std::numeric_limits<double>::max();

            if (bucket_maxes.size() >= 4) {
                std::sort(bucket_maxes.begin(), bucket_maxes.end());

                const size_t n = bucket_maxes.size();
                const double q1 = bucket_maxes[n / 4];
                const double q3 = bucket_maxes[(n * 3) / 4];
                const double iqr = q3 - q1;

                cutoff = q3 + (3.0 * iqr);
                if (iqr == 0) cutoff = q3 * 1.5;
            } else if (!bucket_maxes.empty()) {
                cutoff = (*std::max_element(bucket_maxes.begin(), bucket_maxes.end())) * 1.5;
            }

            //Filter OS BYPASS
            out.values.reserve(values.size());
            for (double v : values) {
                if (v > cutoff) out.bypass++;
                else out.values.push_back(v);
            }

            if (out.values.empty()) {
                out.values = values;
                out.bypass = 0;
            }

            std::sort(out.values.begin(), out.values.end());
            out.cutoff = cutoff;
            return out;
        }

        inline double MedianFromSorted(const std::vector<double>& sorted) {
            if (sorted.empty()) return 0.0;
            const size_t n = sorted.size();
            return (n % 2 == 0) ? (sorted[n/2 - 1] + sorted[n/2]) / 2.0 : sorted[n/2];
        }
    }

    struct alignas(64) RingBuffer {
        Cycles data[MAX_SAMPLES];
        size_t head = 0;

        // 0xFF: unset/unknown, 0xFE: mixed
        uint8_t calib_key = 0xFF;

        __attribute__((always_inline)) inline void push(Cycles val, uint8_t key) {
            if (calib_key == 0xFF) calib_key = key;
            else if (calib_key != key) calib_key = 0xFE;

            data[head] = val;
            head = (head + 1) & BUFFER_MASK; // wrapping
        }
    };

    struct ThreadStorage {
        ID stack_ids[MAX_ACTIVE_SLOTS];
        Cycles stack_starts[MAX_ACTIVE_SLOTS];
        uint8_t stack_modes[MAX_ACTIVE_SLOTS]; // Latte::Mode encoded
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

        
        double cycles_per_ns = 1.0; //Default: Unknown

        static Manager& Get() { static Manager instance; return instance; }

        __attribute__((always_inline)) inline void EnsureCalibrated() {
            std::call_once(calibrate_once, [&]() { Calibrate(); });
        }

        __attribute__((always_inline)) inline Cycles CalibrationOffset(uint8_t key) const {
            if (key >= Internal::CALIB_KEY_COUNT) return 0;
            if (!calib_valid[key]) return 0;
            return calib_offsets[key];
        }

        void Register(ThreadStorage* ts) {
            std::lock_guard<std::mutex> lock(mutex);
            thread_buffers.push_back(ts);
        }

        // Non-blocking Data Extraction
        // Returns all valid samples collected so far for a specific ID
        std::vector<Cycles> ExtractRaw(ID id) {
            std::vector<Cycles> output;
            output.reserve(1024);

            std::lock_guard<std::mutex> lock(mutex);
            for (auto* ts : thread_buffers) {
                auto it = ts->history.find(id);
                if (it == ts->history.end()) continue;
                RingBuffer& rb = it->second;

                for (size_t i = 0; i < MAX_SAMPLES; ++i) {
                    Cycles v = rb.data[i];
                    if (v > 0) output.push_back(v);
                }
            }
            return output;
        }

        void Calibrate(); //scroll down

    private:
        std::once_flag calibrate_once;
        std::array<Cycles, Internal::CALIB_KEY_COUNT> calib_offsets{};
        std::array<bool, Internal::CALIB_KEY_COUNT> calib_valid{};
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

    template <Mode M, Cycles (*TimeFunc)()>
    struct Recorder {
        __attribute__((always_inline)) static inline void Start(ID id) {
            ThreadStorage* ts = GetThreadStorage();
            if (__builtin_expect(ts->stack_ptr < MAX_ACTIVE_SLOTS, 1)) {
                ts->stack_ids[ts->stack_ptr] = id;
                ts->stack_starts[ts->stack_ptr] = TimeFunc();
                ts->stack_modes[ts->stack_ptr] = static_cast<uint8_t>(M);
                ts->stack_ptr++;
            }
        }

        __attribute__((always_inline)) static inline Cycles Stop(ID /*id*/) {
            Cycles end = TimeFunc();
            ThreadStorage* ts = GetThreadStorage();

            if (__builtin_expect(ts->stack_ptr > 0, 1)) {
                ts->stack_ptr--;
                Cycles delta = end - ts->stack_starts[ts->stack_ptr]; // raw latency
                const uint8_t start_mode = ts->stack_modes[ts->stack_ptr];
                const uint8_t stop_mode  = static_cast<uint8_t>(M);
                const uint8_t key = Internal::CalibKey(start_mode, stop_mode);
                ts->history[ts->stack_ids[ts->stack_ptr]].push(delta, key);
                return delta;
            }
            return 0;
        }
    };
    namespace Fast { inline void Start(ID id) { Recorder<Mode::Fast, Intrinsic::RDTSC>::Start(id); } inline void Stop(ID id) { Recorder<Mode::Fast, Intrinsic::RDTSC>::Stop(id); } }
    namespace Mid  { inline void Start(ID id) { Recorder<Mode::Mid, Intrinsic::RDTSCP>::Start(id); } inline void Stop(ID id) { Recorder<Mode::Mid, Intrinsic::RDTSCP>::Stop(id); } }
    namespace Hard { inline void Start(ID id) { Recorder<Mode::Hard, Intrinsic::RDTSCP_LFENCE>::Start(id); } inline void Stop(ID id) { Recorder<Mode::Hard, Intrinsic::RDTSCP_LFENCE>::Stop(id); } }

    inline void Manager::Calibrate() {
        { // TIME CALIBRATION (cycles_per_ns)
            auto t1 = std::chrono::high_resolution_clock::now();
            Cycles c1 = Intrinsic::RDTSC();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            Cycles c2 = Intrinsic::RDTSC();
            auto t2 = std::chrono::high_resolution_clock::now();
            double ns = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
            cycles_per_ns = (ns > 0.0) ? (double)(c2 - c1) / ns : 1.0;
        }

        // PERMUTATION OVERHEAD
        constexpr int WARMUP_ITERS = 10000; // naturally overwrite by circular buffer
        const int iters = (int)MAX_SAMPLES + WARMUP_ITERS;


        (void)GetThreadStorage(); // Force TLS init before sampling

        for (volatile int i = 0; i < iters; ++i) {
            Internal::LFENCE();
            Latte::Fast::Start(Internal::CALIB_FxF);
            Latte::Fast::Stop(Internal::CALIB_FxF);
            Internal::LFENCE();

            Internal::LFENCE();
            Latte::Fast::Start(Internal::CALIB_FxM);
            Latte::Mid::Stop(Internal::CALIB_FxM);
            Internal::LFENCE();

            Internal::LFENCE();
            Latte::Fast::Start(Internal::CALIB_FxH);
            Latte::Hard::Stop(Internal::CALIB_FxH);
            Internal::LFENCE();

            Internal::LFENCE();
            Latte::Mid::Start(Internal::CALIB_MxF);
            Latte::Fast::Stop(Internal::CALIB_MxF);
            Internal::LFENCE();

            Internal::LFENCE();
            Latte::Mid::Start(Internal::CALIB_MxM);
            Latte::Mid::Stop(Internal::CALIB_MxM);
            Internal::LFENCE();

            Internal::LFENCE();
            Latte::Mid::Start(Internal::CALIB_MxH);
            Latte::Hard::Stop(Internal::CALIB_MxH);
            Internal::LFENCE();

            Internal::LFENCE();
            Latte::Hard::Start(Internal::CALIB_HxF);
            Latte::Fast::Stop(Internal::CALIB_HxF);
            Internal::LFENCE();

            Internal::LFENCE();
            Latte::Hard::Start(Internal::CALIB_HxM);
            Latte::Mid::Stop(Internal::CALIB_HxM);
            Internal::LFENCE();

            Internal::LFENCE();
            Latte::Hard::Start(Internal::CALIB_HxH);
            Latte::Hard::Stop(Internal::CALIB_HxH);
            Internal::LFENCE();
        }

        // PULSE OVERHEAD
        for (volatile int i = 0; i < iters; ++i) {
            Internal::LFENCE();
            Latte::Fast::Start(Internal::CALIB_PULSE);
            LATTE_PULSE("xxxx");
            Latte::Mid::Stop(Internal::CALIB_PULSE);
            Internal::LFENCE();
        }

        auto BUMED = [&](ID id) -> Cycles { // Median(Min(Bucket[1'000] ))
            std::vector<Cycles> raw = ExtractRaw(id);
            if (raw.empty()) return 0;

            constexpr size_t BUCKET = 1000;
            const size_t full = (raw.size() / BUCKET) * BUCKET;
            if (full == 0) {
                return *std::min_element(raw.begin(), raw.end());
            }

            std::vector<Cycles> mins;
            mins.reserve(full / BUCKET);

            for (size_t i = 0; i < full; i += BUCKET) {
                Cycles m = std::numeric_limits<Cycles>::max();
                for (size_t j = i; j < i + BUCKET; ++j) {
                    const Cycles v = raw[j];
                    if (v > 0 && v < m) m = v;
                }
                if (m != std::numeric_limits<Cycles>::max())
                    mins.push_back(m);
            }

            if (mins.empty()) {
                return *std::min_element(raw.begin(), raw.end());
            }

            std::sort(mins.begin(), mins.end());
            const size_t n = mins.size();
            if (n & 1) return mins[n / 2];

            const unsigned __int128 a = mins[n / 2 - 1];
            const unsigned __int128 b = mins[n / 2];
            return (Cycles)((a + b + 1) / 2);
        };


        calib_offsets[Internal::CalibKey((uint8_t)Mode::Fast, (uint8_t)Mode::Fast)] = BUMED(Internal::CALIB_FxF);
        calib_offsets[Internal::CalibKey((uint8_t)Mode::Fast, (uint8_t)Mode::Mid)]  = BUMED(Internal::CALIB_FxM);
        calib_offsets[Internal::CalibKey((uint8_t)Mode::Fast, (uint8_t)Mode::Hard)] = BUMED(Internal::CALIB_FxH);

        calib_offsets[Internal::CalibKey((uint8_t)Mode::Mid,  (uint8_t)Mode::Fast)] = BUMED(Internal::CALIB_MxF);
        calib_offsets[Internal::CalibKey((uint8_t)Mode::Mid,  (uint8_t)Mode::Mid)]  = BUMED(Internal::CALIB_MxM);
        calib_offsets[Internal::CalibKey((uint8_t)Mode::Mid,  (uint8_t)Mode::Hard)] = BUMED(Internal::CALIB_MxH);

        calib_offsets[Internal::CalibKey((uint8_t)Mode::Hard, (uint8_t)Mode::Fast)] = BUMED(Internal::CALIB_HxF);
        calib_offsets[Internal::CalibKey((uint8_t)Mode::Hard, (uint8_t)Mode::Mid)]  = BUMED(Internal::CALIB_HxM);
        calib_offsets[Internal::CalibKey((uint8_t)Mode::Hard, (uint8_t)Mode::Hard)] = BUMED(Internal::CALIB_HxH);

        calib_offsets[Internal::CALIB_KEY_PULSE] = BUMED(Internal::CALIB_PULSE);

        for (size_t i = 0; i < Internal::CALIB_KEY_COUNT; ++i) {
            calib_valid[i] = true;
        }

        // Remove calibration telemetry
        if (ThreadStorage* ts = GetThreadStorage()) {
            ts->history.erase(Internal::CALIB_FxF);
            ts->history.erase(Internal::CALIB_FxM);
            ts->history.erase(Internal::CALIB_FxH);
            ts->history.erase(Internal::CALIB_MxF);
            ts->history.erase(Internal::CALIB_MxM);
            ts->history.erase(Internal::CALIB_MxH);
            ts->history.erase(Internal::CALIB_HxF);
            ts->history.erase(Internal::CALIB_HxM);
            ts->history.erase(Internal::CALIB_HxH);
            ts->history.erase(Internal::CALIB_PULSE);
            ts->history.erase("xxxx");
        }
    }

    inline std::vector<Cycles> Snapshot(ID id) {
        return Manager::Get().ExtractRaw(id);
    }

    inline std::string FormatTime(double ns) {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(2);
        if (ns < 1000.0)  ss << ns << " ns";
        else if (ns < 1e6) ss << (ns / 1e3) << " us";
        else if (ns < 1e9) ss << (ns / 1e6) << " ms";
        else if (ns < 60e9) ss << (ns / 1e9) << " s";
        else ss << (ns / 60e9) << " min";

        return ss.str();
    }

    namespace Parameter {
        enum Unit { Cycle, Time };
        enum Data { Raw, Calibrated };
    }

    inline Internal::CleanResult DataClean(const std::vector<double>& values) {
        return Internal::CleanData(values);
    }

    inline void DumpToStream(std::ostream& oss, Parameter::Unit unit = Parameter::Cycle, Parameter::Data data_mode = Parameter::Raw) {
        Manager& mgr = Manager::Get();
        if (unit == Parameter::Time || data_mode == Parameter::Calibrated) {
            mgr.EnsureCalibrated();
        }

        struct Series {
            std::vector<double> values;
            uint8_t calib_key = Internal::CALIB_KEY_UNSET;
        };

        std::map<ID, Series> global_data;

        { // Thread-safe data collection
            std::lock_guard<std::mutex> lock(mgr.mutex);
            for (auto* ts : mgr.thread_buffers) {
                for (auto& [id, buffer] : ts->history) {
                    Series& s = global_data[id];

                    if (s.calib_key == Internal::CALIB_KEY_UNSET) s.calib_key = buffer.calib_key;
                    else if (s.calib_key != buffer.calib_key)      s.calib_key = Internal::CALIB_KEY_MIXED;

                    for (size_t i = 0; i < MAX_SAMPLES; ++i) {
                        if (buffer.data[i] > 0) s.values.push_back((double)buffer.data[i]);
                    }
                }
            }
        }

        auto FormatLarge = [](double val) {
            const char* units[] = {"", "K", "M", "B", "T"};
            int unit_idx = 0;
            while (val >= 1000.0 && unit_idx < 4) { val /= 1000.0; unit_idx++; }
            std::ostringstream ss;
            if (unit_idx == 0) ss << std::fixed << std::setprecision(0) << val;
            else ss << std::fixed << std::setprecision(2) << val << " " << units[unit_idx];
            return ss.str();
        };

        auto ToDisp = [&](double cycles) {
            return (unit == Parameter::Time) ? FormatTime(cycles / mgr.cycles_per_ns) : FormatLarge(cycles);
        };

        // Column Widths
        const int C1 = 20;
        const int C2 = 9;
        const int C3 = 10;
        const int C4 = 10;
        const int C5 = 10;
        const int C6 = 8;
        const int C7 = 10;
        const int C8 = 10;
        const int C9 = 10;
        const int C_BY = 10;

        const int TABLE_WIDTH = C1 + C2 + C3 + C4 + C5 + C6 + C7 + C8 + C9 + C_BY + 3;
        const std::string line(TABLE_WIDTH, '-');
        const std::string d_line(TABLE_WIDTH, '=');

        auto col = [](const std::string& s, int width, bool left = false) {
            std::ostringstream ss;
            if (left) ss << std::left  << std::setw(width) << s;
            else      ss << std::right << std::setw(width) << s;
            std::string res = ss.str();
            return (res.size() > (size_t)width) ? res.substr(0, width) : res;
        };

        oss << "\n#" << d_line << "#\n";
        std::string title = "LATTE TELEMETRY [" + std::string((unit == Parameter::Time) ? "TIME" : "CYCLES") + "][" + std::string((data_mode == Parameter::Calibrated) ? "CAL" : "RAW") + "]";
        oss << "| " << col(title, TABLE_WIDTH - 2, true) << " |\n";
        oss << "#" << d_line << "#\n";

        // Removing overhead measured by your Latte-calls themself (noise)
        if (data_mode == Parameter::Calibrated) {
            auto off_str = [&](uint8_t sm, uint8_t em) -> std::string {
                const uint8_t k = Internal::CalibKey(sm, em);
                return ToDisp((double)mgr.CalibrationOffset(k));
            };

            auto off_pulse_str = [&]() -> std::string {
                return ToDisp((double)mgr.CalibrationOffset(Internal::CALIB_KEY_PULSE));
            };

            constexpr int MW = 14;
            auto mcol = [&](const std::string& s, int w, bool left = false) {
                std::ostringstream ss;
                if (left) ss << std::left  << std::setw(w) << s;
                else      ss << std::right << std::setw(w) << s;
                std::string res = ss.str();
                return (res.size() > (size_t)w) ? res.substr(0, w) : res;
            };

            const auto F = (uint8_t)Mode::Fast;
            const auto M = (uint8_t)Mode::Mid;
            const auto H = (uint8_t)Mode::Hard;

            const std::string END = (std::string(57, ' ')+"|\n").c_str();

            oss << "| " << col("OVERHEAD H[Start] x W[Stop]", TABLE_WIDTH - 2, true) << " |\n";
            oss << "| " << mcol("", 10, true)
                << mcol("F", MW) << mcol("M", MW) << mcol("H", MW) << END;
            oss << "| " << mcol("F", 10, true)
                << mcol(off_str(F, F), MW) << mcol(off_str(F, M), MW) << mcol(off_str(F, H), MW) << END;
            oss << "| " << mcol("M", 10, true)
                << mcol(off_str(M, F), MW) << mcol(off_str(M, M), MW) << mcol(off_str(M, H), MW) << END;
            oss << "| " << mcol("H", 10, true)
                << mcol(off_str(H, F), MW) << mcol(off_str(H, M), MW) << mcol(off_str(H, H), MW) << END;
            oss << "| " << mcol("PULSE", 10, true)
                << mcol(off_pulse_str(), MW) << mcol("", MW) << mcol("", MW) << END;

            oss << "|" << line << "|\n";
        }

        oss << "| " << col("COMPONENT", C1, true)
            << col("SAMPLES", C2)
            << col("AVG", C3)
            << col("MEDIAN", C4)
            << col("STD DEV", C5)
            << col("SKEW", C6)
            << col("MIN", C7)
            << col("MAX", C8)
            << col("RANGE", C9)
            << col("BYPASS", C_BY) << "  |\n";

        oss << "|" << line << "|\n";

        for (auto& [id, series] : global_data) {
            if (series.values.empty()) continue;

            
            std::vector<double> adjusted; // noise removal
            adjusted.reserve(series.values.size());

            const double off = (data_mode == Parameter::Calibrated) ? (double)mgr.CalibrationOffset(series.calib_key) : 0.0;

            for (double v : series.values) {
                double x = v - off;
                if (x < 0.0) x = 0.0;
                adjusted.push_back(x);
            }

            // user-extracted cleaning function
            Internal::CleanResult clean = Internal::CleanData(adjusted);
            std::vector<double>& clean_values = clean.values;
            size_t cpu_bypass_count = clean.bypass;

            const size_t n = clean_values.size();
            if (n == 0) continue;

            double sum = 0;
            for (double v : clean_values) sum += v;
            const double avg = sum / (double)n;
            const double median = Internal::MedianFromSorted(clean_values);

            double var_sum = 0, skew_sum = 0;
            for (double v : clean_values) {
                double d = v - avg;
                var_sum += d * d;
                skew_sum += (d * d * d);
            }

            const double std_dev = std::sqrt(var_sum / (double)n);
            const double skew = (n > 1 && std_dev > 1e-9) ? (skew_sum / (double)n) / (std_dev * std_dev * std_dev) : 0.0;

            std::ostringstream sk;
            sk << std::fixed << std::setprecision(2) << skew;

            oss << "| " << col(id, C1, true)
                << col(std::to_string(n), C2)
                << col(ToDisp(avg), C3)
                << col(ToDisp(median), C4)
                << col(ToDisp(std_dev), C5)
                << col(sk.str(), C6)
                << col(ToDisp(clean_values.front()), C7)
                << col(ToDisp(clean_values.back()), C8)
                << col(ToDisp(clean_values.back() - clean_values.front()), C9)
                << col(std::to_string(cpu_bypass_count), C_BY) << "  |\n";
        }

        oss << "#" << d_line << "#" << std::endl;
    }


}

//once
#define LATTE_CALIBRATE() do { Latte::Manager::Get().EnsureCalibrated(); } while(0)
