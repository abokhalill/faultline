// SPDX-License-Identifier: Apache-2.0
//
// FL020/FL021, the case alloc_cost did not reach. Same-thread malloc/free is
// flat in thread count because tcache and per-thread arenas keep it off shared
// state. Freeing on a different thread than allocated returns the block to the
// OWNING arena, which is shared state by definition -- the shape a
// producer/consumer pipeline actually has.
//
// same : producer allocates and frees its own block (control)
// cross: producer allocates, consumer frees  (the hazard)
// Identical work and queue mechanics in both arms; the delta is arena
// ownership. Reports ns per block from the producer's side.

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RING 1024
static void *slot[RING];
static _Atomic unsigned long head, tail;
static _Atomic int arrived = 0, go = 0;
static long n_items;
static int sz, cross, cpu_p, cpu_c;
static double prod_ns;

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (pthread_setaffinity_np(pthread_self(), sizeof(s), &s)) {
        fprintf(stderr, "FATAL: pin %d\n", cpu); _exit(2);
    }
}

static void *consumer(void *u) {
    (void)u; pin(cpu_c);
    atomic_fetch_add(&arrived, 1);
    while (!atomic_load_explicit(&go, memory_order_acquire)) __builtin_ia32_pause();
    for (unsigned long t = 0; t < (unsigned long)n_items; t++) {
        while (atomic_load_explicit(&head, memory_order_acquire) == t)
            __builtin_ia32_pause();
        void *p = slot[t & (RING - 1)];
        if (cross) free(p);          // block returns to the producer's arena
        atomic_store_explicit(&tail, t + 1, memory_order_release);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s <items> <size> <same|cross> <cpu_p> <cpu_c>\n", argv[0]);
        return 2;
    }
    n_items = atol(argv[1]); sz = atoi(argv[2]);
    cross = strcmp(argv[3], "cross") == 0;
    if (!cross && strcmp(argv[3], "same")) { fprintf(stderr, "bad mode\n"); return 2; }
    cpu_p = atoi(argv[4]); cpu_c = atoi(argv[5]);

    pthread_t c;
    if (pthread_create(&c, NULL, consumer, NULL)) { fprintf(stderr, "FATAL create\n"); return 2; }
    pin(cpu_p);
    atomic_fetch_add(&arrived, 1);
    while (atomic_load(&arrived) < 2) sched_yield();
    atomic_store_explicit(&go, 1, memory_order_release);

    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (unsigned long h = 0; h < (unsigned long)n_items; ) {
        while (h - atomic_load_explicit(&tail, memory_order_acquire) >= RING)
            __builtin_ia32_pause();
        void *p = malloc(sz);
        *(volatile char *)p = (char)h;
        slot[h & (RING - 1)] = p;
        if (!cross) free(p);         // control: same thread frees
        atomic_store_explicit(&head, ++h, memory_order_release);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    pthread_join(c, NULL);

    prod_ns = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / (double)n_items;
    printf("%-6s size=%-6d ns_per_block=%7.2f\n", cross ? "cross" : "same", sz, prod_ns);
    return 0;
}
