// SPDX-License-Identifier: Apache-2.0
//
// FL014 says a split-line atomic "stalls every core on the socket". span_cost
// measured what it costs the core that issues it. The collateral half of the
// claim has never been measured on any part, and it is the half the severity
// rests on.
//
// One aggressor hammers a split lock on its own line; a victim on another core
// does aligned atomics on its own line, 4KB away, sharing nothing. An
// aligned-atomic aggressor is the control: if the victim slows the same amount
// either way, what we are seeing is a busy neighbour and not a bus lock.

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char arena[1 << 20] __attribute__((aligned(4096)));
static atomic_int go;
static atomic_ullong aggrOps;
static int mode, aggrCpu;
static double aggrNsPerOp[3];

static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}
static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (sched_setaffinity(0, sizeof(s), &s)) { perror("pin"); exit(2); }
}

// 8252 straddles the line at 8256 while staying inside one page, so the
// treatment is a line split and not a page split.
#define AGGR_ALIGNED 8192
#define AGGR_SPLIT   8252

static void *aggressor(void *unused) {
    (void)unused;
    pin(aggrCpu);
    _Atomic uint64_t *a = (_Atomic uint64_t *)
        (arena + (mode == 2 ? AGGR_SPLIT : AGGR_ALIGNED));
    while (atomic_load_explicit(&go, memory_order_relaxed) == 0)
        ;
    unsigned long long n = 0;
    while (atomic_load_explicit(&go, memory_order_relaxed) == 1) {
        atomic_fetch_add_explicit(a, 1, memory_order_relaxed);
        n++;
    }
    atomic_store(&aggrOps, n);
    return NULL;
}

static double victimRun(int victimCpu, long iters) {
    pin(victimCpu);
    _Atomic uint64_t *v = (_Atomic uint64_t *)(arena + 0);
    for (long i = 0; i < iters / 100; i++)
        atomic_fetch_add_explicit(v, 1, memory_order_relaxed);
    double t0 = now();
    for (long i = 0; i < iters; i++)
        atomic_fetch_add_explicit(v, 1, memory_order_relaxed);
    return (now() - t0) / (double)iters;
}

static double measure(int victimCpu, int m, long iters) {
    mode = m;
    pthread_t th;
    atomic_store(&go, 0);
    if (m != 0 && pthread_create(&th, NULL, aggressor, NULL)) {
        perror("pthread_create");
        exit(2);
    }
    atomic_store(&aggrOps, 0);
    atomic_store(&go, 1);
    double t0 = now();
    double ns = victimRun(victimCpu, iters);
    double elapsed = now() - t0;
    atomic_store(&go, 2);
    if (m != 0) {
        pthread_join(th, NULL);
        // Proves the treatment landed. An aggressor silently running aligned
        // ops looks exactly like a bus lock with no collateral effect.
        aggrNsPerOp[m] = elapsed / (double)atomic_load(&aggrOps);
    }
    return ns;
}

int main(int argc, char **argv) {
    int victimCpu = argc > 1 ? atoi(argv[1]) : 5;
    aggrCpu       = argc > 2 ? atoi(argv[2]) : 4;
    long iters    = argc > 3 ? atol(argv[3]) : 20000000;
    int reps      = argc > 4 ? atoi(argv[4]) : 5;
    memset(arena, 0, sizeof arena);

    double best[3] = {1e18, 1e18, 1e18};
    for (int r = 0; r < reps; r++)
        for (int m = 0; m < 3; m++) {
            double ns = measure(victimCpu, m, iters);
            if (ns < best[m]) best[m] = ns;
        }

    printf("victim cpu%d, aggressor cpu%d, %ld iters, min of %d\n",
           victimCpu, aggrCpu, iters, reps);
    printf("  %-28s %8.3f ns\n", "alone", best[0]);
    printf("  %-28s %8.3f ns  (%+.3f, %.2fx)\n", "vs aligned-atomic neighbour",
           best[1], best[1] - best[0], best[1] / best[0]);
    printf("  %-28s %8.3f ns  (%+.3f, %.2fx)\n", "vs split-lock neighbour",
           best[2], best[2] - best[0], best[2] / best[0]);
    printf("  aggressor: aligned %.3f ns/op, split %.3f ns/op\n",
           aggrNsPerOp[1], aggrNsPerOp[2]);
    return 0;
}
