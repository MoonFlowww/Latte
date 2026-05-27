# ☕️ Latte: Ultra-Low Latency C++ Telemetry Framework
![GitHub last commit](https://img.shields.io/github/last-commit/MoonFlowww/Latte?logo=github)
![Unique Cloners](https://img.shields.io/badge/Unique_Cloners-87-blue?logo=github)

Latte is a header-only C++ telemetry library designed for high‑frequency trading, game engines, and real‑time systems where measurement overhead must be measured in nanoseconds rather than microseconds.
Latte measures **CPU cycles** using x86_64 timestamp counters (RDTSC / RDTSCP) and stores samples in **per‑thread fixed‑size ring buffers** for later reporting.

> *Zero allocations in steady state. The first `Start(id)` per thread allocates a ring buffer entry in the per-thread map; all subsequent calls to the same ID are allocation-free and lock-free.*

---

## Implementation Guide

### 1. Integration
Latte is header-only. Include the file in your project. It requires a C++17 compliant compiler and an x86_64 architecture.

```cpp
#include "Latte.hpp"
```

### 2. Scoped instrumentation (Start/Stop)
Use string literals directly as identifiers. No pre-registration is required.

```cpp
void ProcessOrder() {
    Latte::Fast::Start(__func__);
    // Core logic execution here
    Latte::Fast::Stop(__func__);
}
```
#### Measurement modes: Fast, Mid, Hard

Latte provides three levels of timing precision. They differ in **ordering guarantees** and **overhead**, choose the right one for your measurement context.

| Mode   | Intrinsic(s)            | Serialization        | Best for |
|--------|-------------------------|----------------------|-----------|
| `Fast` | `__rdtsc()`             | None                 | Coarse, high‑frequency polling where every cycle matters (e.g., Hot Path). |
| `Mid`  | `__rdtscp()`            | Partial barrier (waits on prior instructions only)   | Default for function‑level profiling, balanced accuracy and overhead. |
| `Hard` | `lfence` + `__rdtscp()` and `__rdtscp()`  + `lfence` | Full (LFENCE + serialize) | Measuring tiny snippets (few dozen cycles) or when out‑of‑order execution could distort deltas. |


### 3. Nested monitoring
The framework supports up to **64** active overlapping slots per thread.

```cpp
Latte::Fast::Start("Frame_Total");
Latte::Mid::Start("Physics_Engine");
// Core logic execution
Latte::Mid::Stop("Physics_Engine");
Latte::Fast::Stop("Frame_Total");
```




### 4. `LATTE_PULSE("ID")` (delta between successive events)
`LATTE_PULSE("ID")` records the cycle delta **between successive calls** on the same thread.

**Implementation details:**

- The macro uses static `thread_local` pointers to a `RingBuffer` and a `last` timestamp, avoiding repeated map lookups after the first call.
- First call per thread: initialises the buffer pointer and stores `RDTSC()` as `last` – **no sample is pushed**.
- Subsequent calls: compute `now - last`, push the delta into the ring buffer (with a fixed calibration key `Internal::CALIB_KEY_PULSE`), and update `last`.

> *The recorded delta represents the time span between two consecutive `LATTE_PULSE` invocations, which can be used to measure loop iteration time or polling frequency.*

```cpp
for (;;) {
    // ... poll / process ...
    LATTE_PULSE("Toroidal_Record");
}
```

### 5. `Snapshot(ID)` (raw sample extraction)
`Snapshot("ID")` returns raw cycle samples collected so far for a given ID, aggregated across threads.

```cpp
std::vector<Latte::Cycles> samples = Latte::Snapshot("Physics_Engine"); // vec<uint64_t>
```

### 6. Report generation: `DumpToStream`
Send output to any `std::ostream` at the conclusion of the execution period.

```cpp
Latte::DumpToStream(std::ostream& os,
                    Latte::Parameter::Unit unit,
                    Latte::Parameter::Data data_mode);
```

Defaults:
- `unit = Latte::Parameter::Cycle`
- `data_mode = Latte::Parameter::Raw`

Common usage:
```cpp
// Raw cycles (built-in defaults)
Latte::DumpToStream(std::cout);

// Calibrated time (ns/us/ms formatting) with overhead correction
Latte::DumpToStream(std::cout,
                    Latte::Parameter::Time,
                    Latte::Parameter::Calibrated);
```

Calibration and overhead:
- Time formatting uses an internal `cycles_per_ns`.
- When calibration is active, `DumpToStream()` subtracts measured instrumentation overhead from each sample before computing statistics (conceptually: `v' = v - overhead`).
- When calibration is active, `DumpToStream()` prints a secondary table labeled `OVERHEAD H[Start] x W[Stop]` with measured overhead for each Start/Stop mode permutation.

Mixed-mode calibration:
- The per-thread stack stores the capture Mode (Fast/Mid/Hard) alongside the timestamp.
- On `Stop()`, calibration/overhead selection is keyed by the `(start_mode, stop_mode)` pair (e.g., Fast×Fast, Fast×Mid, Hard×Mid).

---

## Statistical Analysis
Latte provides insights into the distribution of latency, focusing on long-tail behavior:
* **Average**
* **Median**
* **Standard Deviation**
* **Skewness**
* **Min**
* **Max**
* **Range (Δ)** 
* **OUTLIER** number of filtered samples classified as outlier from IQR


### Data cleaning

Before computing report statistics, `DumpToStream()` runs `Internal::CleanData` over the collected samples for each `(thread, id)`. The cleaning pass:

- **Buckets samples** into groups of 1 000, records the maximum of each bucket.
- Calculates an **IQR (interquartile range) on the bucket maxima** to determine a cutoff.
- Samples **above the cutoff** are counted as `OUTLIER` and excluded from statistics.
- Remaining samples are sorted and used for median, mean, stddev, skew, min, max, and range.

> *This bucket‑max IQR method is more robust against long‑tail outliers than a raw IQR.*


```ascii
#==============================================================================================================#
| LATTE TELEMETRY [TIME][CAL]                                                                                  |
#==============================================================================================================#
| SELF-OFFSET H[Start] x W[Stop]                                                                               |
|                        F             M             H                                                         |
| F                0.21 ns      10.02 ns      10.02 ns                                                         |
| M                0.21 ns      10.02 ns      10.02 ns                                                         |
| H                0.21 ns      10.02 ns      10.02 ns                                                         |
| PULSE           10.02 ns                                                                                     |
|--------------------------------------------------------------------------------------------------------------|
| COMPONENT             SAMPLES       AVG    MEDIAN   STD DEV    SKEW       MIN       MAX     RANGE    OUTLIER |
|--------------------------------------------------------------------------------------------------------------|
| DP_Build_Total              1   38.07 s   38.07 s   0.00 ns    0.00   38.07 s   38.07 s   0.00 ns         0  |
| DP_StateLoop            65536  31.15 us  30.96 us   1.42 us   12.13  29.32 us  95.10 us  65.78 us         0  |
| Sim_Tick_Total           4997 226.79 ns 220.42 ns  76.04 ns    1.40 110.21 ns 821.55 ns 711.34 ns         3  |
| Sim_OrderFlow            4997  43.62 ns  29.63 ns  36.61 ns    2.02   9.59 ns 380.51 ns 370.91 ns         3  |
| Sim_AskLoop              1216   1.03 us 711.34 ns   1.06 us    2.14   0.00 ns   8.30 us   8.30 us         0  |
| Sim_BidLoop              1247   1.12 us 751.42 ns   4.21 us   32.56   0.00 ns 145.96 us 145.96 us         0  |
| Sim_RiskPnL              5000   6.24 ns   9.59 ns   4.67 ns   -0.57   0.00 ns  19.82 ns  19.82 ns         0  |
#==============================================================================================================#
```
> *The `SELF-OFFSET` table shows what Latte measures when `Start` and `Stop` are called back-to-back with no work between them (function call overhead only). Row = Start mode, Column = Stop mode. These values are automatically subtracted when using `Parameter::Calibrated`.*

---

## Storage model

### Ring buffer behavior (overwrite semantics)
Each `(thread, id)` owns a fixed‑size ring buffer (`alignas(64)` for cache‑line isolation).
- **Write:** `push()` stores a new `Cycles` value at the current `head` and advances `head = (head+1) & BUFFER_MASK`. No zeroing is performed – old values are silently overwritten.
- **Read:** `ExtractRaw()` and `DumpToStream()` consider **any positive value** as a valid sample. (Zero is the initial state and is ignored.)
- Because overwrites happen unconditionally, the buffer always contains the **most recent** `MAX_SAMPLES` samples (some of which may be zero only if fewer than `MAX_SAMPLES` have ever been written).
- Default capacity: `MAX_SAMPLES = 65536` (`BUFFER_PWR = 16`). Must stay a power of two for the bitmask wrap.



### `MAX_SAMPLES` default (2^16)
The default capacity is **65,536 samples** per ID per thread:

- `BUFFER_PWR = 16`
- `MAX_SAMPLES = 1 << BUFFER_PWR`  → `65536`
- wrapping uses a bitmask (`MAX_SAMPLES` must remain a power of two)

---

## Correctness rules (Start/Stop stack semantics)

Latte uses a per-thread stack to support nesting:

- Maximum depth: **64** (`MAX_ACTIVE_SLOTS`)
  - If the stack is full, extra `Start()` calls are ignored (no sample recorded for that scope).
- `Stop()` pops the most recent `Start()` (LIFO).
  - The `id` argument to `Stop(id)` is **not validated** against the top-of-stack ID.
- `Stop()` on an empty stack returns without recording.

**Best practice:** always pair Start/Stop in strict LIFO order and pass the same ID for readability.

---

## Thread-safety

Sampling (`Start/Stop` and `LATTE_PULSE`) is designed for low contention by using per-thread storage.

**However `DumpToStream` and `Snapshot` are NOT safe to call concurrently with active sampling threads.**
- The manager’s mutex only protects the global list of `ThreadStorage` pointers – **it does not synchronise access to the ring buffers** or the per‑thread `history` map.
- Writes (from `Start`/`Stop`/`LATTE_PULSE`) are performed without locks or atomics for maximum speed.
- Concurrent reads while writes happen on the same thread or another thread cause **data races** (undefined behaviour).

**Recommended usage:** Call `DumpToStream` / `Snapshot` only after all worker threads have stopped instrumenting (e.g., after joining threads, or after a barrier that guarantees no `Start`/`Stop` is in flight).

---

## Key Optimization Principles

Latte is engineered to solve the "Observer Effect," ensuring that the act of measurement does not significantly distort the performance of the system being observed.

### 1. Zero Contention (Thread Local Storage)
Standard profiling tools often utilize global mutexes or atomic counters that cause cache line contention between CPU cores. Latte utilizes `thread_local` storage. Each thread records data into its own private buffers, ensuring that measurement never forces one thread to stall for another.

### 2. ID-as-a-Pointer (Zero String Hashing)
Latte uses `const char*` IDs so identification is based on the pointer value (address), avoiding string hashing and string comparisons. IDs are used as keys (e.g., `std::map<const char*, RingBuffer>`); comparisons are pointer comparisons, and lookup cost scales as `O(log N)` with the number of distinct IDs on the thread.

**Important:** IDs are compared by pointer value, not by string contents. Only use:
- string literals: `"MyComponent"`
- stable static storage: `static const char name[] = "MyComponent";`

Do **not** pass temporary `std::string::c_str()` pointers or stack buffers.


### 3. Stack-Based Capturing (Nesting support)
To support deep nesting without linear search overhead, Latte utilizes a per-thread SoA stack.
* `Start()` pushes the ID, timestamp, and capture Mode (Fast/Mid/Hard) to the stack index.
* `Stop()` pops the top of the stack, calculates the cycle delta, and carries the start/stop Modes into calibration selection.

This keeps Start/Stop overhead stable with nesting (up to the fixed maximum depth).

### 4. Map lookups on `Stop()`

Each thread owns a `ThreadStorage` that keeps per-ID history in an ordered map:

- `std::map<const char*, RingBuffer> history`

On `Stop()`, Latte looks up the ring buffer for the ID in that map (`O(log N)` pointer comparisons) and pushes the measured cycle delta into the buffer.

Notes:
- Keys are compared by **address** (`const char*`), so there is **no string hashing** and no `strcmp`.
- The container is still a tree (`std::map`), so lookup scales as `O(log N)` with the number of distinct IDs on the thread.

### 5. Cache-Line Alignment & SoA
To prevent "False Sharing" and maximize CPU pre-fetcher efficiency, internal buffers are aligned to 64-byte boundaries `(alignas(64))`. The use of **Structure of Arrays** instead of Arrays of Structs ensures that only relevant timing data is pulled into the **L1 cache**, preventing unnecessary memory bandwidth usage.

### 6. Hardware-Level Timing
Latte provides three levels of precision by wrapping x86 intrinsics directly:
* **Fast (RDTSC):** Lowest overhead. Non-serializing; suitable for general logic.
* **Mid (RDTSCP):** More ordered than RDTSC.
* **Hard (LFENCE + RDTSCP):** More serialized; forces stronger ordering.
---

## Requirements and Constraints

* **Architecture:** x86_64 required for `__rdtsc` / `__rdtscp`.
* **C++ standard:** C++17.
* **ID Persistence:** Use string literals or stable static pointers (`const char*`).
* **Memory Footprint:** Reserves space for **65,536** samples per identifier per thread by default (fixed-size ring buffer, overwriting on wrap). Configurable by changing `BUFFER_PWR` / `MAX_SAMPLES` (must stay power-of-two for mask wrap).

---

## Compiler constructs and portability notes

`Latte.hpp` contains GCC/Clang‑specific constructs. To compile under MSVC, you must wrap them with preprocessor conditionals. Example:
```cpp
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC optimize ("O3")
#define ALWAYS_INLINE __attribute__((always_inline))
#define LIKELY(expr) __builtin_expect(!!(expr), 1)
#elif defined(_MSC_VER)
#define ALWAYS_INLINE __forceinline
#define LIKELY(expr) (expr)
#endif
```
The intrinsics headers are already handled: `<intrin.h>` for MSVC, `<x86intrin.h>` for GCC/Clang.

> *The library is not portable to non‑x86_64 architectures because it relies on `__rdtsc` / `__rdtscp`.*


---


## ☕️ Latency Report

### **ASM**
| Function         | Avg (cycles) | Median (cycles) | StdDev (cycles) | Min (cycles) | Max (cycles) | Δ Min-Max (cycles) |
|:-----------------|-------------:|----------------:|----------------:|-------------:|-------------:|-------------------:|
| __rdtsc          | 30.1         | 29.9            | 0.4             | 29.7         | 31.2         | 1.5                |
| __rdtscp         | 57.7         | 57.5            | 0.9             | 57.3         | 62.6         | 5.2                |
| _LFENCE          | 14.7         | 14.7            | 0.1             | 14.7         | 15.3         | 0.6                |

### **Latte**
| Function         | Avg (cycles) | Median (cycles) | StdDev (cycles) | Min (cycles) | Max (cycles) | Δ Min-Max (cycles) |
|:-----------------|-------------:|----------------:|----------------:|-------------:|-------------:|-------------------:|
| Fast::Start+Stop | 60.1         | 60.0            | 0.1             | 59.9         | 60.4         | 0.5                |
| Mid::Start+Stop  | 119.8        | 119.7           | 0.4             | 119.9        | 122.7        | 2.8                |
| Hard::Start+Stop | 175.8        | 175.4           | 0.9             | 175.4        | 179.3        | 3.9                |
| LATTE_PULSE      | 29.9         | 29.8            | 0.1             | 29.7         | 30.3         | 0.6                |

### **Chrono**
| Function         | Avg (cycles) | Median (cycles) | StdDev (cycles) | Min (cycles) | Max (cycles) | Δ Min-Max (cycles) |
|:-----------------|-------------:|----------------:|----------------:|-------------:|-------------:|-------------------:|
| std::chrono::now | 153.9        | 153.4           | 0.4             | 153.1        | 156.9        | 3.3                |

Measurements were computed using:
- **Batch size:** 100 000 iterations per trial  
- **Trials:** 100 independent batch
- **Warm-up:** 1 independent batch
- **Core:** Pinned
- **Optimization:** g++ -O3 -march=native
- **CPU:** AMD Ryzen 5 7600X 6-Core 4.7 GHz (Boost Clock: 5.3 GHz)

Each function’s latency was measured in **CPU cycles** using a high-accuracy timer and batched calls, with initial warm-ups to stabilize branch predictors and caches.
> For example:
> - On a **3.5 GHz** core (≈ 3.5 billion cycles/sec), **1 cycle ≈ 0.286 ns**.  
> - On a **4.0 GHz** core, **1 cycle ≈ 0.250 ns**.  
> - On a **4.7 GHz** core, **1 cycle ≈ 0.213 ns**.  
>  
> These conversions come from the fact that clock rate (in hertz) is the number of cycles per second:  
> `time per cycle = 1 / frequency` (in seconds).
