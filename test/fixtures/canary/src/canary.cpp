// Known-positive canary for rules the hft_core fixture does not reach.
//
// Every registered rule must fire on some canary, enforced by
// scan_test. A rule that stops firing is a silent recall loss, and both
// defects found in the FL002/FL041 audit were exactly that: caught by a
// fixture, never by a gate.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

extern "C" {
// Reached from a thread body so the cross-TU hot verdict covers the C
// fixtures. The remark channel keys on that verdict, not a local attribute.
long canary_scale_into(long *out, const long *bound, long n);
int canary_drain_pending(int id);
}

namespace canary {

// FL060. NUMA-unfriendly shared structure: >=256B, escapes, mutable.
struct alignas(64) SharedRegistry {
    std::atomic<uint64_t> sequence{0};
    std::atomic<uint64_t> epoch{0};
    uint64_t slots[48];
    uint64_t checkpoints[16];
};
static SharedRegistry g_registry;

// FL090. Hazard amplification: atomics on distinct lines of one shared
// object, written from thread bodies. hft_core cannot canary this rule:
// it spawns no threads at all, so amplification has no mechanism there and
// firing on it was evidence-free.
struct AmplifiedCounters {
    std::atomic<uint64_t> head{0};
    uint64_t pad_a[7];
    std::atomic<uint64_t> tail{0};
    uint64_t pad_b[7];
    std::atomic<uint64_t> drops{0};
    uint64_t pad_c[7];
};
static AmplifiedCounters g_amplified;

// FL002. The thread-pool shape: one shared object, two adjacent plain
// counters, ONE writer function run by every pool thread. Requiring two
// distinct writer *functions* rejected this, which is backwards: two
// *cores* is the requirement, and a pool already has them.
struct PoolCounters {
    uint64_t hits;
    uint64_t misses;
};
static PoolCounters g_pool_counters;
static void record_pool_event(int hit) {
    if (hit) g_pool_counters.hits++;
    else     g_pool_counters.misses++;
}

// FL003. Striped per-thread array: one slot per thread, packed several to
// a line, written under a thread-index.
static uint64_t g_thread_bytes[64];

void account(int thread_id, uint64_t n) { g_thread_bytes[thread_id] += n; }

uint64_t total_accounted() {
    uint64_t t = 0;
    for (int i = 0; i < 64; ++i) t += g_thread_bytes[i];
    return t;
}

// FL061. Centralized dispatcher: hot function with high fan-out.
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
    long buf[64] = {0}, bound = 3;
    for (int i = 0; i < 1000; ++i) {
        canary_scale_into(buf, &bound, 64);
        canary_drain_pending(id);
        record_pool_event(i & 1);
        g_amplified.head.fetch_add(1);
        g_amplified.tail.fetch_add(1);
        g_amplified.drops.fetch_add(id & 1);
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

// A loop-swept subscript reached outside any enclosing function. The striped
// array visitor keyed writer attribution on the current function without
// filtering that state, which segfaulted the analyzer on rocksdb rather than
// producing a finding. A crash is a silent recall loss for the whole TU.
static constexpr int kSeedLen = 8;
static int seed_table[kSeedLen] = {0, 1, 2, 3, 4, 5, 6, 7};
struct SeedSum {
    int value = [] {
        int acc = 0;
        for (int i = 0; i < kSeedLen; ++i) acc += seed_table[i];
        return acc;
    }();
};
static SeedSum g_seed_sum;

} // namespace canary

extern "C" {
// Reachable from main so the cross-TU hot verdict covers the C fixtures;
// the remark channel keys on that verdict rather than on a local attribute.
long canary_scale_into(long *out, const long *bound, long n);
int canary_drain_pending(int id);
}

int main() {
    canary::run();
    return static_cast<int>(canary::total_accounted() & 1);
}
