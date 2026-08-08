// SPDX-License-Identifier: Apache-2.0
//
// FL041's shape: SPSC ring buffer, producer owns head, consumer owns tail.
// Co-located on one line vs one line each. Unlike FL002's counter shape the
// indices are updated per element, so spacing is set by the loop body and
// not by a syscall -- this is the dense end of the density curve, and the
// reason FL041 is exempt from FL002's deliberate-layout demotion.
//
// Reports ns per element pushed, which is the quantity a queue user feels.

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LINE 64
#define RING 4096

static struct {          // both indices on ONE line: the hazard FL041 flags
    _Atomic unsigned long head;
    _Atomic unsigned long tail;
} __attribute__((aligned(LINE))) shared_idx;

static struct {          // one line each: the fix
    _Atomic unsigned long head;
    char p0[LINE - sizeof(unsigned long)];
    _Atomic unsigned long tail;
    char p1[LINE - sizeof(unsigned long)];
} __attribute__((aligned(LINE))) padded_idx;

static unsigned long slot[RING];
static _Atomic int arrived = 0, go = 0;
static long n_items;
static int split;                       // 1 = padded arm
static double prod_ns;

static _Atomic unsigned long *HEAD(void) { return split ? &padded_idx.head : &shared_idx.head; }
static _Atomic unsigned long *TAIL(void) { return split ? &padded_idx.tail : &shared_idx.tail; }

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (pthread_setaffinity_np(pthread_self(), sizeof(s), &s)) {
        fprintf(stderr, "FATAL: pin %d\n", cpu); _exit(2);
    }
}

static int cpu_p, cpu_c;

static void *consumer(void *unused) {
    (void)unused; pin(cpu_c);
    atomic_fetch_add(&arrived, 1);
    while (!atomic_load_explicit(&go, memory_order_acquire)) __builtin_ia32_pause();
    unsigned long t = 0;
    while (t < (unsigned long)n_items) {
        while (atomic_load_explicit(HEAD(), memory_order_acquire) == t)
            __builtin_ia32_pause();
        (void)slot[t & (RING - 1)];
        atomic_store_explicit(TAIL(), ++t, memory_order_release);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s <items> <shared|padded> <cpu_prod> <cpu_cons>\n", argv[0]);
        return 2;
    }
    n_items = atol(argv[1]);
    split   = strcmp(argv[2], "padded") == 0;
    if (!split && strcmp(argv[2], "shared")) { fprintf(stderr, "bad mode\n"); return 2; }
    cpu_p = atoi(argv[3]); cpu_c = atoi(argv[4]);

    pthread_t c;
    if (pthread_create(&c, NULL, consumer, NULL)) { fprintf(stderr, "FATAL: create\n"); return 2; }
    pin(cpu_p);
    atomic_fetch_add(&arrived, 1);
    while (atomic_load(&arrived) < 2) sched_yield();
    atomic_store_explicit(&go, 1, memory_order_release);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (unsigned long h = 0; h < (unsigned long)n_items; ) {
        while (h - atomic_load_explicit(TAIL(), memory_order_acquire) >= RING)
            __builtin_ia32_pause();
        slot[h & (RING - 1)] = h;
        atomic_store_explicit(HEAD(), ++h, memory_order_release);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    pthread_join(c, NULL);

    prod_ns = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)n_items;
    printf("%-7s items=%ld\tns_per_item=%.3f\n", split ? "padded" : "shared", n_items, prod_ns);
    return 0;
}
