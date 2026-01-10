# ☕️ Latte: Ultra-Low Latency C++ Telemetry Framework

Latte is a header-only C++ telemetry library designed for high-frequency trading, game engines, and real-time systems where measurement overhead must be measured in nanoseconds rather than microseconds.

Latte measures **CPU cycles** using x86_64 timestamp counters (TSC) and stores samples in **per-thread fixed-size ring buffers** for later reporting.

---

## Key Optimization Principles

Latte is engineered to solve the "Observer Effect," ensuring that the act of measurement does not significantly distort the performance of the system being observed.

### 1. Zero Contention (Thread Local Storage)
Standard profiling tools often utilize global mutexes or atomic counters that cause cache line contention between CPU cores. Latte utilizes `thread_local` storage. Each thread records data into its own private buffers, ensuring that measurement never forces one thread to stall for another.

### 2. ID-as-a-Pointer (Zero String Hashing)
Most profilers process string IDs by hashing them at runtime to locate a storage bucket. Latte utilizes `const char*` as the ID. Because string literals occupy fixed memory addresses in the data segment, Latte compares the 64-bit memory address rather than the string content. This reduces ID identification to a single pointer comparison.

**Important:** IDs are compared by pointer value, not by string contents. Only use:
- string literals: `"MyComponent"`
- stable static storage: `static const char name[] = "MyComponent";`

Do **not** pass temporary `std::string::c_str()` pointers or stack buffers.

### 3. Stack-Based Capturing (Nesting support)
To support deep nesting without linear search overhead, Latte utilizes a per-thread SoA stack.
* `Start()` pushes the ID and timestamp to the stack index.
* `Stop()` pops the top of the stack and calculates the cycle delta.

This keeps Start/Stop overhead stable with nesting (up to the fixed maximum depth).

### 4. About “Deferred Aggregation / Zero Map Lookups” (current behavior)
Earlier descriptions of Latte emphasized a two-phase “raw log then aggregate” model with “zero map lookups” on the hot path.

**The current `Latte.hpp` implementation does *not* provide zero-lookups on `Stop()`.** Samples are written into a per-thread container keyed by ID:
- Each thread owns a `ThreadStorage`.
- Each `ThreadStorage` keeps a `std::map<ID, RingBuffer>` (`history`).
- On `Stop()`, Latte performs a `std::map` lookup (`O(log N)`, where `N` is the number of IDs seen by that thread) and then pushes the cycle delta into that ring buffer.

What you still get:
- no cross-thread contention on sampling (per-thread storage)
- fixed-capacity buffers (bounded memory)

How to minimize the impact in extremely hot paths:
- **Prefer `LATTE_PULSE("ID")`** when you want “delta since last event” (it caches the ring buffer pointer per thread after the first call).
- Warm up the ID set at thread startup to move “first use” work (map insert/alloc) out of the hot path.
- If you need true fixed-index, zero-lookup recording for many IDs, the typical next step is adding a numeric-ID + flat-array mode.

### 5. Cache-Line Alignment & SoA
To prevent "False Sharing" and maximize CPU pre-fetcher efficiency, internal buffers are aligned to 64-byte boundaries `(alignas(64))`. The use of **Structure of Arrays** instead of Arrays of Structs ensures that only relevant timing data is pulled into the **L1 cache**, preventing unnecessary memory bandwidth usage.

### 6. Hardware-Level Timing
Latte provides three levels of precision by wrapping x86 intrinsics directly:
* **Fast (RDTSC):** Lowest overhead. Non-serializing; suitable for general logic.
* **Mid (RDTSCP):** More ordered than RDTSC.
* **Hard (LFENCE + RDTSCP):** More serialized; forces stronger ordering.

---

## Statistical Analysis
Latte provides insights into the distribution of latency, focusing on long-tail behavior:
* **Average**
* **Median**
* **Standard Deviation**
* **Skewness**
* **Min / Max**
* **Range (Δ)**

```ascii
#=============================================================================================================================#
| LATTE TELEMETRY [TIME]                                                                                                      |
#=============================================================================================================================#
| COMPONENT                   SAMPLES          AVG       MEDIAN      STD DEV      SKEW          MIN          MAX    RANGE (Δ) |
|-----------------------------------------------------------------------------------------------------------------------------|
| NestedLevel                      11    222.14 us    244.34 us     70.24 us     -2.85     10.02 ns    244.56 us    244.55 us |
| PointerChasing                    1    901.71 ns    901.71 ns      0.00 ns      0.00    901.71 ns    901.71 ns      0.00 ns |
| LoopIteration                  1000     11.03 ns     10.02 ns      3.02 ns      2.65     10.02 ns     20.04 ns     10.02 ns |
| Worker_3                       1000     65.60 us     65.14 us     15.25 us     24.52     17.93 us    496.67 us    478.74 us |
| Worker_2                       1000     65.64 us     65.14 us     15.78 us     24.92     14.47 us    516.61 us    502.14 us |
| Worker_1                       1000     65.72 us     65.14 us     22.71 us     30.63     17.96 us    776.41 us    758.45 us |
| Worker_0                       1000     65.74 us     65.13 us     21.69 us     30.33     16.05 us    742.28 us    726.23 us |
#=============================================================================================================================#
```

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

Behavior:
- The first call per thread initializes internal state (captures a baseline timestamp) and **does not record** a delta.
- Subsequent calls record `(now - last)` in a cached ring buffer pointer and update `last`.

```cpp
for (;;) {
    // ... poll / process ...
    LATTE_PULSE("Poll_Loop_Delta");
}
```

### 5. `Snapshot(ID)` (raw sample extraction)
`Snapshot("ID")` returns raw cycle samples collected so far for a given ID, aggregated across threads.

```cpp
std::vector<Latte::Cycles> samples = Latte::Snapshot("Physics_Engine");
```

### 6. Report generation: `DumpToStream(std::ostream&, bool InTime = true)`
Send output to any `std::ostream` at the conclusion of the execution period.

```cpp
Latte::DumpToStream(std::cout);        // default: TIME (ns/us/ms)
Latte::DumpToStream(std::cout, false); // TSCs (raw)
```

Time formatting uses an internal calibration (`cycles_per_ns`) performed during initialization.

---

## Storage model

### Ring buffer behavior (overwrite semantics)
Each `(thread, id)` owns a fixed-size ring buffer. New samples overwrite old ones when the buffer wraps.

- Slots are initialized to `0`.
- Non-zero entries are treated as valid samples during extraction and dumping.
- Only the most recent `MAX_SAMPLES` samples per `(thread, id)` are retained.

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

However, **`DumpToStream` and `Snapshot` are not safe to run concurrently with active sampling threads**:
- While the manager mutex protects the list of registered thread buffers, ring-buffer data and per-thread maps are written without atomics.
- Calling `DumpToStream` / `Snapshot` while other threads are recording can cause data races.

**Recommended usage:** call `DumpToStream` / `Snapshot` only after worker threads have stopped instrumenting (e.g., after joining threads or after a barrier that stops sampling).

---

## Requirements and Constraints

* **Architecture:** x86_64 required for `__rdtsc` / `__rdtscp`.
* **C++ standard:** C++17.
* **ID Persistence:** Use string literals or stable static pointers (`const char*`).
* **Memory Footprint:** Reserves space for **65,536** samples per identifier per thread by default (fixed-size ring buffer, overwriting on wrap). Configurable by changing `BUFFER_PWR` / `MAX_SAMPLES` (must stay power-of-two for mask wrap).

---

## Compiler constructs and portability notes

`Latte.hpp` contains GCC/Clang-oriented constructs:
- `#pragma GCC optimize ("O3")`
- `__attribute__((always_inline))`
- `__builtin_expect`

It also includes intrinsics headers as:
- MSVC: `<intrin.h>`
- GCC/Clang: `<x86intrin.h>`

For MSVC portability, you may need to guard/replace GCC pragmas and attributes and replace `__builtin_expect` with an MSVC-friendly branch hint.

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
| Hard::Start+Stop | 148.5        | 148.4           | 0.5             | 147.9        | 150.4        | 2.5                |
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
- **Optimization:** g++ -O3
- **CPU:** AMD Ryzen 5 7600X 6-Core 4.7 GHz (Boost Clock: 5.3 GHz)

Each function’s latency was measured in **CPU cycles** using a high-accuracy timer and batched calls, with initial warm-ups to stabilize branch predictors and caches.
> For example:
> - On a **3.5 GHz** core (≈ 3.5 billion cycles/sec), **1 cycle ≈ 0.286 ns**.  
> - On a **4.0 GHz** core, **1 cycle ≈ 0.250 ns**.  
> - On a **4.7 GHz** core, **1 cycle ≈ 0.213 ns**.  
>  
> These conversions come from the fact that clock rate (in hertz) is the number of cycles per second:  
> `time per cycle = 1 / frequency` (in seconds).
