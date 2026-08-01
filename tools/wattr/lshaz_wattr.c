// SPDX-License-Identifier: Apache-2.0
//
// Write-attribution runtime: ground truth for cache-line sharing without a PMU.
//
// Clang's ThreadSanitizer instrumentation emits ordinary calls to __tsan_*.
// Linking this instead of libtsan keeps the instrumentation and discards the
// race detector, which matters because the dominant false-sharing idiom is
// deliberately race-free: striped and role-partitioned fields are
// single-writer-per-slot, so TSan reports nothing while the coherence traffic
// is exactly what we are trying to observe.
//
// What it records: for each 8-byte granule, which threads wrote it. That
// separates the two cases a static analyser cannot:
//
//   same granule, several threads   -> true sharing (contention on one field)
//   same line, DIFFERENT granules,
//   different threads               -> FALSE SHARING (the claim FL002 makes)
//
//   build:  clang -fsanitize=thread -c target.c        (instrumentation)
//           clang target.o lshaz_wattr.o -lpthread     (this, not libtsan)
//   run:    LSHAZ_WATTR_OUT=w.tsv ./target
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GRANULE 8u
#define LINE_BYTES 64u
#define TABLE_BITS 20
#define TABLE_SIZE (1u << TABLE_BITS)
#define MAX_THREADS 64

// Open addressing, insert-only, fixed capacity. Never resizes: a resize would
// need a lock on the hot path, and losing a granule under saturation is a
// reported shortfall rather than a stall.
struct slot {
    _Atomic uintptr_t key;   // granule address, 0 = empty
    _Atomic uint64_t  tids;  // bitmask of writing threads
    _Atomic uint64_t  count;
};
static struct slot g_table[TABLE_SIZE];
static _Atomic uint64_t g_next_tid = 0;
static _Atomic uint64_t g_dropped  = 0;
static __thread int t_tid = -1;

static inline int my_tid(void) {
    if (t_tid < 0) {
        uint64_t v = atomic_fetch_add_explicit(&g_next_tid, 1,
                                               memory_order_relaxed);
        t_tid = (v < MAX_THREADS) ? (int)v : MAX_THREADS - 1;
    }
    return t_tid;
}

static inline uint32_t hash_addr(uintptr_t a) {
    a *= 0x9E3779B97F4A7C15ull;      // splitmix-style finalizer
    a ^= a >> 29;
    return (uint32_t)(a >> 32) & (TABLE_SIZE - 1);
}

static void note_write(uintptr_t addr, unsigned size) {
    const uint64_t bit = 1ull << my_tid();
    uintptr_t g   = addr & ~(uintptr_t)(GRANULE - 1);
    uintptr_t end = addr + size;
    for (; g < end; g += GRANULE) {
        uint32_t h = hash_addr(g);
        for (unsigned probe = 0; probe < 64; ++probe) {
            struct slot *s = &g_table[(h + probe) & (TABLE_SIZE - 1)];
            uintptr_t k = atomic_load_explicit(&s->key, memory_order_acquire);
            if (k == 0) {
                uintptr_t expect = 0;
                if (!atomic_compare_exchange_strong_explicit(
                        &s->key, &expect, g, memory_order_acq_rel,
                        memory_order_acquire) && expect != g)
                    continue;               // lost the race to another key
            } else if (k != g) {
                continue;
            }
            atomic_fetch_or_explicit(&s->tids, bit, memory_order_relaxed);
            atomic_fetch_add_explicit(&s->count, 1, memory_order_relaxed);
            goto next;
        }
        atomic_fetch_add_explicit(&g_dropped, 1, memory_order_relaxed);
    next:;
    }
}

static int popcount64(uint64_t v) { return __builtin_popcountll(v); }

static void dump(void) {
    const char *path = getenv("LSHAZ_WATTR_OUT");
    FILE *f = path ? fopen(path, "w") : stderr;
    if (!f) f = stderr;

    // Runtime addresses are ASLR'd; the join needs the bias to reach the
    // link-time symbol table. First executable mapping of the main binary.
    unsigned long long base = 0;
    FILE *m = fopen("/proc/self/maps", "r");
    if (m) {
        char line[512];
        while (fgets(line, sizeof line, m)) {
            unsigned long long lo, hi;
            char perms[8];
            if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) == 3 &&
                perms[2] == 'x') { base = lo; break; }
        }
        fclose(m);
    }
    fprintf(f, "# base\t%llx\n", base);

    // Group granules by line so the same-granule / different-granule
    // distinction can be made, which is the whole point.
    fprintf(f, "# line\tgranule_off\ttid_mask\tn_tids\twrites\n");
    unsigned long lines_multi = 0, lines_false = 0, granules = 0;
    for (uint32_t i = 0; i < TABLE_SIZE; ++i) {
        uintptr_t k = atomic_load_explicit(&g_table[i].key,
                                           memory_order_relaxed);
        if (!k) continue;
        uint64_t m = atomic_load_explicit(&g_table[i].tids,
                                          memory_order_relaxed);
        uint64_t c = atomic_load_explicit(&g_table[i].count,
                                          memory_order_relaxed);
        ++granules;
        fprintf(f, "%p\t%u\t%llu\t%d\t%llu\n",
                (void *)(k & ~(uintptr_t)(LINE_BYTES - 1)),
                (unsigned)(k & (LINE_BYTES - 1)),
                (unsigned long long)m, popcount64(m),
                (unsigned long long)c);
        if (popcount64(m) > 1) ++lines_multi;
    }
    (void)lines_false;
    fprintf(f, "# granules=%lu multi_writer_granules=%lu dropped=%llu\n",
            granules, lines_multi,
            (unsigned long long)atomic_load(&g_dropped));
    if (f != stderr) fclose(f);
}

// ---- the __tsan ABI Clang's instrumentation calls ----
void __tsan_init(void) {
    static _Atomic int once = 0;
    int expect = 0;
    if (atomic_compare_exchange_strong(&once, &expect, 1))
        atexit(dump);
}

#define WRITE_N(N)                                                      \
    void __tsan_write##N(void *a) { note_write((uintptr_t)a, N); }      \
    void __tsan_unaligned_write##N(void *a) { note_write((uintptr_t)a, N); }
WRITE_N(1) WRITE_N(2) WRITE_N(4) WRITE_N(8) WRITE_N(16)

// Reads do not cause the ownership transfer the rules claim, so they are
// dropped rather than recorded: keeping them would multiply the table for
// no signal. A reader on another core matters only once a writer exists,
// and the writer is already recorded.
#define READ_N(N)                                                       \
    void __tsan_read##N(void *a) { (void)a; }                           \
    void __tsan_unaligned_read##N(void *a) { (void)a; }
READ_N(1) READ_N(2) READ_N(4) READ_N(8) READ_N(16)

void __tsan_func_entry(void *pc) { (void)pc; }
void __tsan_func_exit(void) {}
void __tsan_vptr_update(void **a, void *b) { (void)a; (void)b; }
void __tsan_vptr_read(void **a) { (void)a; }
void __tsan_range_write(void *a, unsigned long n) {
    note_write((uintptr_t)a, (unsigned)n);
}
void __tsan_range_read(void *a, unsigned long n) { (void)a; (void)n; }
