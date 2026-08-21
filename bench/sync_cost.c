// SPDX-License-Identifier: Apache-2.0
//
// FL012, FL013 and FL021 in one place; each is a small claim and none had a
// number.
//
//   lock  - FL012: uncontended mutex, contended mutex, and the futex/context
//           switch the rule prices at 1-10us.
//   spin  - FL013: poll loop with and without PAUSE, on an SMT sibling, which
//           is the only placement where the sibling-starvation half is real.
//   stack - FL021: touching a large frame vs a small one.

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}
static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (sched_setaffinity(0, sizeof(s), &s)) { perror("pin"); exit(2); }
}
static volatile uint64_t sink;

/* ---------------- FL012: locks ---------------- */
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static _Atomic long shared_ctr;
static _Atomic int go, done_flag;
static long lock_iters;

static void *lock_thread(void *a) {
    pin((int)(long)a);
    while (!atomic_load(&go)) sched_yield();
    for (long i = 0; i < lock_iters; i++) {
        pthread_mutex_lock(&mtx);
        shared_ctr++;
        pthread_mutex_unlock(&mtx);
    }
    return NULL;
}

static void bench_locks(int c0, int c1, long n) {
    lock_iters = n;
    pin(c0);
    double t0 = now();
    for (long i = 0; i < n; i++) {
        pthread_mutex_lock(&mtx); shared_ctr++; pthread_mutex_unlock(&mtx);
    }
    double t1 = now();
    printf("  %-34s %10.2f ns/op\n", "mutex uncontended", (t1 - t0) / n);

    // Same critical section, two cores. Any excess over the uncontended
    // figure is contention plus whatever blocking costs.
    atomic_store(&go, 0);
    pthread_t th;
    pthread_create(&th, NULL, lock_thread, (void *)(long)c1);
    t0 = now(); atomic_store(&go, 1);
    for (long i = 0; i < n; i++) {
        pthread_mutex_lock(&mtx); shared_ctr++; pthread_mutex_unlock(&mtx);
    }
    pthread_join(th, NULL);
    t1 = now();
    printf("  %-34s %10.2f ns/op\n", "mutex contended, 2 cores", (t1 - t0) / (2 * n));

    // Atomic increment on the same counter: the lock-free floor for the same
    // work, so the mutex's own overhead is the difference.
    t0 = now();
    for (long i = 0; i < n; i++)
        atomic_fetch_add_explicit(&shared_ctr, 1, memory_order_relaxed);
    t1 = now();
    printf("  %-34s %10.2f ns/op\n", "atomic increment (floor)", (t1 - t0) / n);
}

/* ---------------- FL013: spin with and without PAUSE ---------------- */
static _Atomic long spin_flag;
static long spin_work;

static void *spinner(void *a) {
    long usePause = (long)a & 1;
    pin((int)((long)a >> 1));
    while (!atomic_load(&go)) sched_yield();
    while (!atomic_load_explicit(&done_flag, memory_order_relaxed)) {
        if (usePause) __builtin_ia32_pause();
    }
    return NULL;
}

static double sibling_throughput(int worker_cpu, int spin_cpu, int usePause) {
    atomic_store(&go, 0); atomic_store(&done_flag, 0);
    pthread_t th;
    pthread_create(&th, NULL, spinner,
                   (void *)(long)((spin_cpu << 1) | usePause));
    pin(worker_cpu);
    atomic_store(&go, 1);
    // Dependent chain: throughput here is what the spinning sibling leaves us.
    uint64_t acc = 1;
    double t0 = now();
    for (long i = 0; i < spin_work; i++) { acc ^= acc >> 7; acc *= 0x2545F4914F6CDD1DULL; }
    double t1 = now();
    sink = acc;
    atomic_store(&done_flag, 1);
    pthread_join(th, NULL);
    return (t1 - t0) / spin_work;
}

/* ---------------- FL021: stack frame ---------------- */
static uint64_t __attribute__((noinline)) frame(unsigned kb, uint64_t seed) {
    // alloca, not a fixed array: the frame must actually change size, or this
    // measures per-call overhead amortised over a varying touch count.
    unsigned n = kb << 10;
    volatile char *buf = __builtin_alloca(n);
    for (unsigned i = 0; i < n; i += 64) buf[i] = (char)(seed + i);
    uint64_t acc = 0;
    for (unsigned i = 0; i < n; i += 64) acc += buf[i];
    return acc;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <cpu> <sibling-cpu> <other-core>\n", argv[0]);
        return 2;
    }
    int cpu = atoi(argv[1]), sib = atoi(argv[2]), other = atoi(argv[3]);

    printf("=== FL012 locks ===\n");
    bench_locks(cpu, other, 3000000);

    printf("=== FL013 spin-wait, sibling on the same physical core ===\n");
    spin_work = 60000000;
    double nop = sibling_throughput(cpu, sib, 0);
    double wp  = sibling_throughput(cpu, sib, 1);
    printf("  %-34s %10.3f ns/op\n", "sibling spins WITHOUT pause", nop);
    printf("  %-34s %10.3f ns/op\n", "sibling spins WITH pause", wp);
    printf("  %-34s %9.1f%%\n", "pause recovers", 100.0 * (nop - wp) / nop);

    printf("=== FL021 stack frame ===\n");
    pin(cpu);
    for (unsigned kb = 1; kb <= 512; kb *= 8) {
        long reps = 200000000 / (kb << 10);
        double t0 = now();
        uint64_t a = 0;
        for (long r = 0; r < reps; r++) a += frame(kb, r);
        double t1 = now();
        sink = a;
        printf("  frame %4uKB  %8.3f ns/touched-line\n",
               kb, (t1 - t0) / (reps * (double)((kb << 10) / 64)));
    }
    return 0;
}
