#include "Latte.hpp"
#include <vector>
#include <random>

// --- CASE 1: Nested Functions ---
void DeepFunction(int depth) {
    Latte::Fast::Start("NestedLevel");
    if (depth > 0) DeepFunction(depth - 1);
    Latte::Fast::Stop("NestedLevel");
}

// --- CASE 2: Pointer Chasing (Cache Miss Latency) ---
struct Node { Node* next; int padding[14]; };

void CacheLatency(size_t count) {
    std::vector<Node> memory(count);
    for (size_t i = 0; i < count - 1; ++i) memory[i].next = &memory[i + 1];
    memory[count - 1].next = nullptr;

    Latte::Hard::Start("PointerChasing");
    Node* curr = &memory[0];
    while (curr) {
        curr = curr->next;
    }
    Latte::Hard::Stop("PointerChasing");
}

// --- CASE 3: Multi-threaded Contention ---
void WorkerThread(int thread_idx) {
    static thread_local char thread_name[32]; // lifetime of thread
    std::snprintf(thread_name, sizeof(thread_name), "Worker_%d", thread_idx); // to avoid binary invented names

    for (int i = 0; i < 1000; ++i) {
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        LATTE_PULSE(thread_name) // function dedicated for loops
    }
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


    std::cout << "Starting Latte Performance Benchmark..." << std::endl;

    // --- Testing Loops ---
    for (int i = 0; i < 1; ++i) {
        Latte::Fast::Start("LoopIteration");
        asm volatile(""); // Prevent compiler from optimizing loop away
        Latte::Fast::Stop("LoopIteration");
    }

    // --- Testing Nested Depth ---
    DeepFunction(10);

    // --- Testing Memory Latency ---
    CacheLatency(1000);

    // --- Testing Multi-threading ---
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) threads.emplace_back(WorkerThread, i);
    for (auto& t : threads) t.join();

    Latte::DumpToStream(std::cout, false);

    return 0;
}
