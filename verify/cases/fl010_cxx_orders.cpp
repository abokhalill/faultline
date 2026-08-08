// regression: 1e2ac0a — constant stores misread as memory_order args
#include <atomic>

struct Flags {
    std::atomic<bool> stop{false};
    std::atomic<unsigned long> seq{0};
};
Flags g;

__attribute__((hot)) void store_constant() {
    g.stop.store(true);   // 11: implicit seq_cst, constant -> FL010
    g.seq.store(0);       // 12: implicit seq_cst, constant -> FL010
}

__attribute__((hot)) void store_variable(unsigned long v) {
    g.seq.store(v);       // 16: implicit seq_cst, variable -> FL010
}

__attribute__((hot)) void store_weak(unsigned long v) {
    g.seq.store(v, std::memory_order_release);      // 20: skip
    g.seq.store(0, std::memory_order_relaxed);      // 21: skip
    g.stop.store(true, std::memory_order_seq_cst);  // 22: explicit -> FL010
}
