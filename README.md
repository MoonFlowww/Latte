# ☕️ Latte: Ultra-Low Latency C++ Telemetry Framework
Latte is a header-only, zero-allocation C++ telemetry library designed for high-frequency trading, game engines, and real-time systems where measurement overhead must be measured in nanoseconds rather than microseconds.

---

## Key Optimization Principles

Latte is engineered to solve the "Observer Effect," ensuring that the act of measurement does not significantly distort the performance of the system being observed.

### 1. Zero Contention (Thread Local Storage)
Standard profiling tools often utilize global mutexes or atomic counters that cause cache line contention between CPU cores. Latte utilizes `thread_local` storage. Each thread records data into its own private buffer, ensuring that measurement never forces one thread to stall for another.

### 2. ID-as-a-Pointer (Zero String Hashing)
Most profilers process string IDs by hashing them at runtime to locate a storage bucket. Latte utilizes `const char*` as the ID. Because string literals occupy fixed memory addresses in the data segment, Latte compares the 64-bit memory address rather than the string content. This reduces ID identification to a single pointer comparison.

### 3. Allocation-Free Hot Path
Memory allocation is a non-deterministic operation that can trigger operating system kernel transitions. Latte utilizes pre-allocated circular ring buffers. Once the sample limit is reached, the framework overwrites the oldest data, ensuring constant memory pressure regardless of execution duration.

### 4. Hardware-Level Timing
Latte provides three levels of precision by wrapping x86 assembly intrinsics directly:
* **Fast (RDTSC):** Lowest overhead. Non-serializing; suitable for general logic.
* **Mid (RDTSCP):** Partially serializing; prevents the CPU from reordering instructions after the timestamp is taken.
* **Hard (LFENCE + RDTSCP):** Fully serializing; forces the CPU to retire all pending instructions before the timer starts.

---

## Statistical Analysis

Latte provides comprehensive insights into the distribution of latency, specifically focusing on the "long tail" of execution:
* **Median:** Provides the 50th percentile, filtering out the noise of infrequent spikes to show typical performance.
* **P99:** Represents the 99th percentile, critical for maintaining service-level agreements (SLAs).
* **Standard Deviation:** Measures the stability and jitter of the component.
* **Skewness:** Indicates the asymmetry of the performance distribution; a high positive skew identifies components prone to infrequent but massive delays.
* **Intrinsic Tax:** Latte auto-calibrates at runtime to report the framework's own overhead for each timing mode (Fast, Mid, and Hard), allowing for more accurate net latency calculations.


---

## Implementation Guide

### 1. Integration
Latte is header-only. Include the file in your project. It requires a C++17 compliant compiler and an x86_64 architecture.

```cpp
#include "Latte.hpp"
```

### 2. Instrumentation
Use string literals directly as identifiers. No pre-registration is required.

```cpp
void ProcessOrder() {
    Latte::Fast::Start(__func__);
    
    // Core logic execution here

    Latte::Fast::Stop(__func__);
}
```

### 3. Nested Monitoring
The framework supports up to 32 active overlapping slots per thread, allowing for granular analysis of nested function calls.
```cpp
Latte::Fast::Start("Frame_Total");
Latte::Mid::Start("Physics_Engine");
// Core logic execution
Latte::Mid::Stop("Physics_Engine");
Latte::Fast::Stop("Frame_Total");
```

### 4. Report Generation
Direct the telemetry output to any `std::ostream` (such as `std::cout` or a file) at the conclusion of the execution period.
```cpp
int main() {
    // Application execution
    Latte::DumpToStream(std::cout);
    return 0;
}
```

---
## Requirements and Constraints
* Architecture: Explicitly requires x86_64 for `__rdtsc` and `__rdtscp` support.
* ID Persistence: Users must use string literals or static `const char*` pointers. Temporary buffers or dynamic strings will result in inconsistent IDs due to shifting memory addresses.
* Memory Footprint: The framework reserves space for 100,000 samples per identifier per thread by default. This is configurable via the `MAX_SAMPLES` constant.
---
## ☕️ Latency Report

| Function             | Avg (cycles)  | Median (cycles) | StdDev (cycles) | Min (cycles) | Max (cycles) | Δ Min-Max (cycles) |
|----------------------|--------------:|----------------:|-----------------:|-------------:|-------------:|-------------------:|
| `Latte::Fast::Start` |         9.74  |            9.68 |             0.20 |        9.63 |       10.93 |              1.30 |
| `Latte::Fast::Stop`  |        30.54  |           30.53 |             0.05 |       30.49 |       30.78 |              0.29 |
| `Latte::Mid::Start`  |         9.70  |            9.69 |             0.03 |        9.67 |        9.84 |              0.17 |
| `Latte::Mid::Stop`   |        62.49  |           62.49 |             0.02 |       62.47 |       62.57 |              0.10 |
| `Latte::Hard::Start` |         9.80  |            9.74 |             0.20 |        9.68 |       11.01 |              1.33 |
| `Latte::Hard::Stop`  |        77.88  |           77.46 |             1.68 |       77.11 |       86.72 |              9.61 |


Measurements were computed using:
- **Batch size:** 5 000 000 iterations per trial  
- **Trials:** 50 independent batch
- **Warm-up:** 3 independent batch
- **Thread:** Pinned
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

