// g++ -O2 -std=c++17 sanity.cpp -lpthread            (checks the instrumented build)
// g++ -O2 -std=c++17 -DLATTE_DISABLE sanity.cpp -lpthread  (checks the no-op build)
//
// Pass/fail checks for edge cases that are easy to get wrong: unmatched
// Stop(), unknown IDs, ring-buffer wraparound, concurrent registration, and
// LATTE_DISABLE actually compiling down to a true no-op.
#include <cassert>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../Latte.hpp"

static int g_checks = 0;
#define CHECK(cond) do { \
  ++g_checks; \
  if (!(cond)) { \
    std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #cond << "\n"; \
    std::exit(1); \
  } \
} while (0)

#ifndef LATTE_DISABLE

static void busy_spin(int iters) {
  volatile uint64_t sink = 0;
  for (int i = 0; i < iters; ++i) sink += i;
  (void)sink;
}

int main() {
  // Unknown/never-recorded ID must yield an empty snapshot, not a crash.
  CHECK(Latte::Snapshot("never_used_component").empty());

  // A Stop() with no matching Start() must not crash or corrupt state.
  Latte::Fast::Stop("orphan_stop");
  Latte::Mid::Stop("orphan_stop");
  Latte::Hard::Stop("orphan_stop");
  CHECK(Latte::Snapshot("orphan_stop").empty());

  // Basic Start/Stop must actually record positive-duration samples.
  for (int i = 0; i < 50; ++i) {
    Latte::Hard::Start("sanity_component");
    busy_spin(1000);
    Latte::Hard::Stop("sanity_component");
  }
  auto samples = Latte::Snapshot("sanity_component");
  CHECK(!samples.empty());
  for (auto v : samples) CHECK(v > 0);

  // Ring buffer must survive wraparound (push well past MAX_SAMPLES) without crashing,
  // and exposed samples must stay bounded by the buffer capacity.
  for (int i = 0; i < 70000; ++i) {
    Latte::Fast::Start("wraparound_component");
    Latte::Fast::Stop("wraparound_component");
  }
  auto wrapped = Latte::Snapshot("wraparound_component");
  CHECK(wrapped.size() <= 65536);

  LATTE_CALIBRATE();

  // DumpToStream must not throw/crash on both raw and calibrated views.
  std::ostringstream oss;
  Latte::DumpToStream(oss, Latte::Parameter::Time, Latte::Parameter::Raw);
  Latte::DumpToStream(oss, Latte::Parameter::Cycle, Latte::Parameter::Calibrated);
  CHECK(!oss.str().empty());

  // Multiple threads registering and recording concurrently must not corrupt
  // Manager's thread list or drop samples (validates Register()'s mutex-
  // protected path). Respect the documented contract: only read (Snapshot)
  // after all worker threads have joined.
  constexpr int THREADS = 8;
  constexpr int ITERS_PER_THREAD = 2000;
  std::vector<std::thread> workers;
  std::vector<std::string> ids(THREADS);
  for (int t = 0; t < THREADS; ++t) {
    ids[t] = "thread_component_" + std::to_string(t);
  }
  for (int t = 0; t < THREADS; ++t) {
    workers.emplace_back([id = ids[t].c_str()]() {
      for (int i = 0; i < ITERS_PER_THREAD; ++i) {
        Latte::Mid::Start(id);
        busy_spin(50);
        Latte::Mid::Stop(id);
      }
    });
  }
  for (auto& w : workers) w.join();

  for (int t = 0; t < THREADS; ++t) {
    auto s = Latte::Snapshot(ids[t].c_str());
    CHECK(s.size() == static_cast<size_t>(ITERS_PER_THREAD));
  }

  std::cout << "sanity (enabled build): " << g_checks << " checks passed\n";
  return 0;
}

#else // LATTE_DISABLE

int main() {
  // In a disabled build every entry point must be a true no-op: it must
  // compile with the same call sites as the enabled build (this is itself
  // part of the check) and must never touch state or crash.
  Latte::Fast::Start("x"); Latte::Fast::Stop("x");
  Latte::Mid::Start("x");  Latte::Mid::Stop("x");
  Latte::Hard::Start("x"); Latte::Hard::Stop("x");
  LATTE_PULSE("x");
  LATTE_CALIBRATE();

  CHECK(Latte::Snapshot("x").empty());
  CHECK(Latte::FormatTime(123.0).empty());
  CHECK(Latte::DataClean({1.0, 2.0, 3.0}).values.empty());

  std::ostringstream oss;
  Latte::DumpToStream(oss);
  CHECK(oss.str().empty());

  std::cout << "sanity (disabled build): " << g_checks << " checks passed\n";
  return 0;
}

#endif
