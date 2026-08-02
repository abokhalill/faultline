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
#include <link.h>

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

// ---- heap attribution ----
//
// Most contended objects are not globals, so a symbol-keyed trace cannot
// see them. Recording each allocation's extent and call site lets a granule
// be attributed to the site that produced the object.
//
// Records are kept after free rather than removed. An address reused by a
// pool genuinely belongs to several objects over the run, and with no
// timestamps that ambiguity cannot be resolved -- so it is surfaced as
// POOL_REUSE instead of silently attributed to whichever record won.
#define MAX_ALLOCS (1u << 20)
struct alloc_rec {
    uintptr_t base;
    uint64_t  size;
    void     *ra;
};
static struct alloc_rec g_allocs[MAX_ALLOCS];
static _Atomic uint64_t g_alloc_n = 0;
static _Atomic uint64_t g_alloc_overflow = 0;

static void note_alloc(void *p, size_t n, void *ra) {
    if (!p) return;
    uint64_t i = atomic_fetch_add_explicit(&g_alloc_n, 1,
                                           memory_order_relaxed);
    if (i >= MAX_ALLOCS) {
        atomic_fetch_add_explicit(&g_alloc_overflow, 1, memory_order_relaxed);
        return;
    }
    g_allocs[i].base = (uintptr_t)p;
    g_allocs[i].size = n;
    g_allocs[i].ra   = ra;
}

extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
extern void *__real_realloc(void *, size_t);
extern void  __real_free(void *);

void *__wrap_malloc(size_t n) {
    void *p = __real_malloc(n);
    note_alloc(p, n, __builtin_return_address(0));
    return p;
}
void *__wrap_calloc(size_t a, size_t b) {
    void *p = __real_calloc(a, b);
    note_alloc(p, a * b, __builtin_return_address(0));
    return p;
}
void *__wrap_realloc(void *q, size_t n) {
    void *p = __real_realloc(q, n);
    note_alloc(p, n, __builtin_return_address(0));
    return p;
}
void __wrap_free(void *p) { __real_free(p); }

static int popcount64(uint64_t v) { return __builtin_popcountll(v); }

// First callback is always the main executable.
static int first_object_bias(struct dl_phdr_info *info, size_t sz, void *out) {
    (void)sz;
    *(unsigned long long *)out = (unsigned long long)info->dlpi_addr;
    return 1; // stop
}

static void dump(void) {
    const char *path = getenv("LSHAZ_WATTR_OUT");
    FILE *f = path ? fopen(path, "w") : stderr;
    if (!f) f = stderr;

    // Runtime addresses are ASLR'd; the join needs the relocation bias to
    // reach nm's link-time addresses. dl_iterate_phdr's first entry is the
    // main executable and dlpi_addr IS the bias -- exactly, by definition.
    // Reading /proc/self/maps for the first executable mapping is off by
    // whatever read-only segment precedes it, which silently misresolved
    // every symbol by a page.
    unsigned long long base = 0;
    dl_iterate_phdr(first_object_bias, &base);
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

    // Allocation extents, so the reader can attribute a heap granule to the
    // site that produced it. Emitted raw: resolving a return address to a
    // source line needs the binary, which the reader has and this does not.
    uint64_t an = atomic_load(&g_alloc_n);
    if (an > MAX_ALLOCS) an = MAX_ALLOCS;
    fprintf(f, "# alloc\tbase\tsize\tra\n");
    for (uint64_t i = 0; i < an; ++i)
        fprintf(f, "@\t%p\t%llu\t%p\n", (void *)g_allocs[i].base,
                (unsigned long long)g_allocs[i].size, g_allocs[i].ra);

    fprintf(f, "# granules=%lu multi_writer_granules=%lu dropped=%llu"
               " allocs=%llu alloc_overflow=%llu\n",
            granules, lines_multi,
            (unsigned long long)atomic_load(&g_dropped),
            (unsigned long long)an,
            (unsigned long long)atomic_load(&g_alloc_overflow));
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

// Clang rewrites memset/memcpy/memmove into these, so they must both do the
// work and record the write. Missing them is a link error, not a silent gap.
void *__tsan_memset(void *d, int c, unsigned long n) {
    note_write((uintptr_t)d, (unsigned)n);
    return memset(d, c, n);
}
void *__tsan_memcpy(void *d, const void *s, unsigned long n) {
    note_write((uintptr_t)d, (unsigned)n);
    return memcpy(d, s, n);
}
void *__tsan_memmove(void *d, const void *s, unsigned long n) {
    note_write((uintptr_t)d, (unsigned)n);
    return memmove(d, s, n);
}
