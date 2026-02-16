// Phase 2 comprehensive test input.
// Exercises: FL010, FL011, FL012, FL040, FL041, FL050, FL090

#include <atomic>
#include <cstdint>
#include <mutex>

// --- FL040: Centralized Mutable Global State ---
// Non-const, non-thread_local global. Should flag High.
std::atomic<uint64_t> g_sequenceNumber{0};

// Negative: const global. Should NOT flag.
const uint64_t g_maxSeqNum = 1000000;

// Negative: thread_local. Should NOT flag.
thread_local uint64_t tl_localSeqNum = 0;

// --- FL041: Contended Queue Pattern ---
// Head/tail atomics on same cache line. Should flag Critical.
struct MPMCQueue {
    std::atomic<uint64_t> head;
    std::atomic<uint64_t> tail;
    char buffer[4096];
};

// Negative: properly padded. Should NOT flag FL041.
struct PaddedQueue {
    alignas(64) std::atomic<uint64_t> head;
    alignas(64) std::atomic<uint64_t> tail;
    char buffer[4096];
};

// --- FL090: Hazard Amplification ---
// >128B + atomics + thread escape. Should flag Critical.
struct HazardousState {
    std::atomic<uint64_t> seqNum;
    std::atomic<uint64_t> lastPrice;
    std::atomic<uint64_t> volume;
    char metadata[256];
};

// --- FL010: Overly Strong Atomic Ordering ---
// seq_cst in hot path. Should flag High, escalate in loop.
[[clang::annotate("faultline_hot")]]
void updateSequence(std::atomic<uint64_t>& seq) {
    // Default ordering = seq_cst. Should flag.
    seq.store(seq.load() + 1);
}

// Negative: explicit relaxed ordering. Should NOT flag FL010.
[[clang::annotate("faultline_hot")]]
void updateSequenceRelaxed(std::atomic<uint64_t>& seq) {
    seq.store(
        seq.load(std::memory_order_relaxed) + 1,
        std::memory_order_relaxed
    );
}

// --- FL011: Atomic Contention Hotspot ---
// Multiple atomic writes in hot loop. Should flag Critical.
[[clang::annotate("faultline_hot")]]
void heavyAtomicLoop(std::atomic<uint64_t>& a, std::atomic<uint64_t>& b) {
    for (int i = 0; i < 1000; ++i) {
        a.fetch_add(1);
        b.fetch_add(1);
    }
}

// --- FL012: Lock in Hot Path ---
// Mutex lock in hot function. Should flag Critical.
[[clang::annotate("faultline_hot")]]
void lockedUpdate(std::mutex& mtx, uint64_t& value) {
    std::lock_guard<std::mutex> guard(mtx);
    value += 1;
}

// Lock inside loop — escalation.
[[clang::annotate("faultline_hot")]]
void lockedLoop(std::mutex& mtx, uint64_t& value) {
    for (int i = 0; i < 100; ++i) {
        std::lock_guard<std::mutex> guard(mtx);
        value += i;
    }
}

// --- FL050: Deep Conditional Tree ---
[[clang::annotate("faultline_hot")]]
void deepBranching(int a, int b, int c, int d, int e) {
    if (a > 0) {
        if (b > 0) {
            if (c > 0) {
                if (d > 0) {
                    if (e > 0) {
                        // depth 5 — exceeds threshold of 4
                        volatile int sink = a + b + c + d + e;
                        (void)sink;
                    }
                }
            }
        }
    }
}

// Large switch — should flag.
[[clang::annotate("faultline_hot")]]
int dispatchMessage(int msgType) {
    switch (msgType) {
        case 0: return 10;
        case 1: return 20;
        case 2: return 30;
        case 3: return 40;
        case 4: return 50;
        case 5: return 60;
        case 6: return 70;
        case 7: return 80;
        case 8: return 90;
        case 9: return 100;
        default: return -1;
    }
}

// --- Negative: cold path function, should NOT trigger hot-path rules ---
void coldInit(std::atomic<uint64_t>& seq) {
    seq.store(0);
}
