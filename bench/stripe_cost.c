// SPDX-License-Identifier: Apache-2.0
//
// FL003's shape: a striped array where thread i owns slot i. Single writer per
// slot, so no data race and no atomics needed -- the reason FL003 must not gate
// on atomicity. The only variable is stride: at 8B every slot shares one line,
// at 64B each gets its own. That is the whole rule, isolated.
//
// Measured at the dense end (tight loop, no spacing). rmw_cost already showed
// the cost collapses past ~1us of separation; this answers the orthogonal
// question of how it scales with geometry when density is not the limiter.

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXT 16
#define ARENA (1 << 20)

static char arena[ARENA] __attribute__((aligned(4096)));
static int n_threads, stride, cpus[MAXT], nc;
static long n_iters;
static _Atomic int arrived = 0, go = 0;
static double ns[MAXT];

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (pthread_setaffinity_np(pthread_self(), sizeof(s), &s)) {
        fprintf(stderr, "FATAL: pin %d\n", cpu); _exit(2);
    }
}

static void *worker(void *arg) {
    long id = (long)arg;
    pin(cpus[id]);
    // Plain volatile store, not an atomic: single-writer-per-slot is the
    // idiom, and using lock-prefixed ops here would measure a different rule.
    volatile unsigned long *slot = (volatile unsigned long *)(arena + id * stride);
    atomic_fetch_add(&arrived, 1);
    while (!atomic_load_explicit(&go, memory_order_acquire)) __builtin_ia32_pause();

    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (long i = 0; i < n_iters; i++) *slot = *slot + 1;
    clock_gettime(CLOCK_MONOTONIC, &b);
    ns[id] = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / (double)n_iters;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s <threads> <stride_bytes> <iters> <cpulist>\n", argv[0]);
        return 2;
    }
    n_threads = atoi(argv[1]); stride = atoi(argv[2]); n_iters = atol(argv[3]);
    for (char *t = strtok(argv[4], ","); t && nc < MAXT; t = strtok(NULL, ",")) cpus[nc++] = atoi(t);
    if (nc != n_threads) { fprintf(stderr, "FATAL: %d cpus for %d threads\n", nc, n_threads); return 2; }
    if ((long)n_threads * stride > ARENA) { fprintf(stderr, "FATAL: arena\n"); return 2; }

    pthread_t th[MAXT];
    for (long i = 0; i < n_threads; i++)
        if (pthread_create(&th[i], NULL, worker, (void *)i)) { fprintf(stderr, "FATAL create\n"); return 2; }
    while (atomic_load(&arrived) < n_threads) sched_yield();
    atomic_store_explicit(&go, 1, memory_order_release);
    for (int i = 0; i < n_threads; i++) pthread_join(th[i], NULL);

    double mx = 0, sum = 0;
    for (int i = 0; i < n_threads; i++) { sum += ns[i]; if (ns[i] > mx) mx = ns[i]; }
    printf("threads=%-2d stride=%-4d mean_ns=%7.3f  max_ns=%7.3f\n",
           n_threads, stride, sum / n_threads, mx);
    return 0;
}
