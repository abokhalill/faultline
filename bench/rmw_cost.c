// SPDX-License-Identifier: Apache-2.0
//
// Marginal cost of a contended atomic RMW, as a function of writer count and
// inter-write spacing.
//
// Two arms at identical thread count and identical spacing:
//   shared  -- two adjacent counters on ONE line   (the redis stat_net_*_bytes shape)
//   private -- one counter per thread, own line    (the post-patch shape)
//
// The coefficient is the DELTA. Spacing work is identical in both arms, so its
// cost cancels; what remains is coherence.
//
// Spacing is the non-obvious parameter. A tight RMW loop saturates the
// interconnect and overstates per-event cost: redis issues one increment per
// syscall, not back-to-back. Sweeping spacing lets the coefficient be read off
// at redis's actual write density instead of at peak contention.

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LINE 64
#define MAXT 64

// redis: #define atomicIncr(var,count) __atomic_add_fetch(&var,(count),__ATOMIC_RELAXED)
#define RMW(p) __atomic_add_fetch((p), 1, __ATOMIC_RELAXED)

// adjacent, one line, guaranteed co-resident -- offsets 0 and 8 of an aligned line
static struct {
    long long a;
    long long b;
} __attribute__((aligned(LINE))) shared_pair;

// one line per thread, no two writers ever share
static struct {
    long long v;
    char pad[LINE - sizeof(long long)];
} __attribute__((aligned(LINE))) priv[MAXT];

static _Atomic int  arrived = 0;
static _Atomic int  go      = 0;
static int          n_iters, n_spin, n_threads, mode_shared;
static int          cpus[MAXT];        // explicit placement; NEVER thread-index
static double       ns_per_op[MAXT];
static uint64_t     sink[MAXT];

static inline uint64_t spin(uint64_t x, int n) {
    // The empty asm forces x through a register every iteration. Without it
    // gcc -O2 elides the whole loop (observed: private arm flat at 5.65ns from
    // spin=0 to spin=5000, which is not physically possible), and the spacing
    // parameter silently becomes a no-op.
    for (int i = 0; i < n; i++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        __asm__ volatile("" : "+r"(x));
    }
    return x;
}

static void pin(int cpu) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    if (pthread_setaffinity_np(pthread_self(), sizeof(s), &s) != 0) {
        fprintf(stderr, "FATAL: cannot pin to cpu %d\n", cpu);
        _exit(2);
    }
}

static void *worker(void *arg) {
    const long id = (long)arg;
    pin(cpus[id]);

    // Alternate which of the two adjacent counters each thread hits, so the
    // arm models genuine false sharing (distinct addresses, same line) rather
    // than true sharing of one address.
    long long *tgt = mode_shared ? (id & 1 ? &shared_pair.b : &shared_pair.a)
                                 : &priv[id].v;

    uint64_t x = 0x9E3779B97F4A7C15ULL ^ (uint64_t)id;

    atomic_fetch_add(&arrived, 1);
    while (!atomic_load_explicit(&go, memory_order_acquire)) __builtin_ia32_pause();

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < n_iters; i++) {
        x = spin(x, n_spin);
        RMW(tgt);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    sink[id] = x;
    ns_per_op[id] = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec))
                    / (double)n_iters;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr,
                "usage: %s <threads> <spin> <iters> <shared|private> <cpulist>\n"
                "  cpulist: comma-separated, one cpu per thread, e.g. 1,2,3\n",
                argv[0]);
        return 2;
    }
    n_threads   = atoi(argv[1]);
    n_spin      = atoi(argv[2]);
    n_iters     = atoi(argv[3]);
    mode_shared = strcmp(argv[4], "shared") == 0;

    if (n_threads < 1 || n_threads > MAXT) { fprintf(stderr, "bad threads\n"); return 2; }
    if (!mode_shared && strcmp(argv[4], "private") != 0) {
        fprintf(stderr, "mode must be shared|private\n"); return 2;
    }

    // Placement is explicit and fatal-on-mismatch. Pinning thread i to cpu i
    // silently lands thread 0 on the housekeeping core and contaminates the run.
    int nc = 0;
    for (char *t = strtok(argv[5], ","); t && nc < MAXT; t = strtok(NULL, ","))
        cpus[nc++] = atoi(t);
    if (nc != n_threads) {
        fprintf(stderr, "FATAL: cpulist has %d entries, need exactly %d\n",
                nc, n_threads);
        return 2;
    }

    pthread_t th[MAXT];
    for (long i = 0; i < n_threads; i++)
        if (pthread_create(&th[i], NULL, worker, (void *)i) != 0) {
            fprintf(stderr, "FATAL: pthread_create\n"); return 2;
        }

    while (atomic_load(&arrived) < n_threads) sched_yield();
    atomic_store_explicit(&go, 1, memory_order_release);

    for (int i = 0; i < n_threads; i++) pthread_join(th[i], NULL);

    // Report the max, not the mean: the slowest writer is the one that gates a
    // request-per-event path, and averaging hides asymmetric victimisation.
    double mx = 0, sum = 0;
    for (int i = 0; i < n_threads; i++) {
        if (ns_per_op[i] > mx) mx = ns_per_op[i];
        sum += ns_per_op[i];
    }
    printf("%s\tthreads=%d\tspin=%d\tmean_ns=%.3f\tmax_ns=%.3f\n",
           mode_shared ? "shared" : "private", n_threads, n_spin,
           sum / n_threads, mx);
    return 0;
}
