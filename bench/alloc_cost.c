// SPDX-License-Identifier: Apache-2.0
//
// FL020/FL021 assert that concurrent malloc/free serializes on allocator
// state. Modern allocators use per-thread arenas or caches, so the claim may
// not fire. The same failure mode FL002 had.
//
// Scaling test: k threads each doing malloc/free at a fixed size. Perfect
// per-thread caching => ns/op flat in k. Serialization => ns/op rises with k.
// Report ns per alloc+free pair, per thread.

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define MAXT 16
static int n_threads, n_iters, sz, cpus[MAXT], nc;
static _Atomic int arrived = 0, go = 0;
static double ns[MAXT];
static volatile void *sink;

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (pthread_setaffinity_np(pthread_self(), sizeof(s), &s)) {
        fprintf(stderr, "FATAL: pin %d\n", cpu); _exit(2);
    }
}

static void *worker(void *arg) {
    long id = (long)arg;
    pin(cpus[id]);
    atomic_fetch_add(&arrived, 1);
    while (!atomic_load_explicit(&go, memory_order_acquire)) __builtin_ia32_pause();

    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < n_iters; i++) {
        void *p = malloc(sz);
        // touch: an untouched allocation may never fault a page in, which
        // measures the fast path only and hides arena work.
        *(volatile char *)p = (char)i;
        sink = p;
        free(p);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    ns[id] = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / (double)n_iters;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s <threads> <size> <iters> <cpulist>\n", argv[0]);
        return 2;
    }
    n_threads = atoi(argv[1]); sz = atoi(argv[2]); n_iters = atoi(argv[3]);
    for (char *t = strtok(argv[4], ","); t && nc < MAXT; t = strtok(NULL, ",")) cpus[nc++] = atoi(t);
    if (nc != n_threads) { fprintf(stderr, "FATAL: %d cpus for %d threads\n", nc, n_threads); return 2; }

    pthread_t th[MAXT];
    for (long i = 0; i < n_threads; i++)
        if (pthread_create(&th[i], NULL, worker, (void *)i)) { fprintf(stderr, "FATAL create\n"); return 2; }
    while (atomic_load(&arrived) < n_threads) sched_yield();
    atomic_store_explicit(&go, 1, memory_order_release);
    for (int i = 0; i < n_threads; i++) pthread_join(th[i], NULL);

    double mx = 0, sum = 0;
    for (int i = 0; i < n_threads; i++) { sum += ns[i]; if (ns[i] > mx) mx = ns[i]; }
    printf("threads=%-2d size=%-6d mean_ns=%7.2f  max_ns=%7.2f\n",
           n_threads, sz, sum / n_threads, mx);
    return 0;
}
