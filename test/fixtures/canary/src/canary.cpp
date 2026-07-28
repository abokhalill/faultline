// Known-positive canary for rules the hft_core fixture does not reach.
//
// Every registered rule must fire on some canary, enforced by
// scan_e2e_test. A rule that stops firing is a silent recall loss, and both
// defects found in the FL002/FL041 audit were exactly that: caught by a
// fixture, never by a gate.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace canary {

// FL060 — NUMA-unfriendly shared structure: >=256B, escapes, mutable.
struct alignas(64) SharedRegistry {
    std::atomic<uint64_t> sequence{0};
    std::atomic<uint64_t> epoch{0};
    uint64_t slots[48];
    uint64_t checkpoints[16];
};
static SharedRegistry g_registry;

// FL003 — striped per-thread array: one slot per thread, packed several to
// a line, written under a thread-index.
static uint64_t g_thread_bytes[64];

void account(int thread_id, uint64_t n) { g_thread_bytes[thread_id] += n; }

uint64_t total_accounted() {
    uint64_t t = 0;
    for (int i = 0; i < 64; ++i) t += g_thread_bytes[i];
    return t;
}

// FL061 — centralized dispatcher: hot function with high fan-out.
static void op_add(uint64_t v)  { g_registry.sequence.fetch_add(v); }
static void op_sub(uint64_t v)  { g_registry.sequence.fetch_sub(v); }
static void op_mark(uint64_t v) { g_registry.epoch.store(v); }
static void op_slot(uint64_t v) { g_registry.slots[v & 47] = v; }
static void op_ckpt(uint64_t v) { g_registry.checkpoints[v & 15] = v; }
static void op_seq(uint64_t v)  { g_registry.sequence.store(v); }
static void op_bump(uint64_t v) { g_registry.epoch.fetch_add(v); }
static void op_zero(uint64_t v) { g_registry.slots[v & 47] = 0; }
static void op_tag(uint64_t v)  { g_registry.checkpoints[v & 15] += v; }

__attribute__((hot))
void dispatch(int opcode, uint64_t v) {
    switch (opcode) {
        case 0:  op_add(v);  break;
        case 1:  op_sub(v);  break;
        case 2:  op_mark(v); break;
        case 3:  op_slot(v); break;
        case 4:  op_ckpt(v); break;
        case 5:  op_seq(v);  break;
        case 6:  op_bump(v); break;
        case 7:  op_zero(v); break;
        default: op_tag(v);  break;
    }
}

static void worker(int id) {
    for (int i = 0; i < 1000; ++i) {
        account(id, static_cast<uint64_t>(i));
        dispatch(i & 7, static_cast<uint64_t>(i));
    }
}

void run() {
    std::thread a(worker, 0);
    std::thread b(worker, 1);
    a.join();
    b.join();
}

} // namespace canary

int main() {
    canary::run();
    return static_cast<int>(canary::total_accounted() & 1);
}
