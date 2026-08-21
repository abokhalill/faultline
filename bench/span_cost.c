// SPDX-License-Identifier: Apache-2.0
//
// FL001 fires more than any other rule and has never been measured.
//
// A field straddling a cache line boundary costs two line touches instead of
// one. That is the plain-access claim. The atomic case is a different
// mechanism entirely: a LOCK-prefixed op crossing a line cannot be satisfied
// by cache-line locking, so the CPU falls back to a bus lock. Same geometry,
// costs that differ by orders of magnitude, and a rule reporting one number
// for both is reporting the wrong one at least half the time.
//
// Offsets are chosen against the real line size rather than assumed: a value
// at 60 spans 64-byte lines but sits inside a 128-byte one.

#define _GNU_SOURCE
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char arena[1 << 16] __attribute__((aligned(4096)));
static volatile uint64_t sink;

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (sched_setaffinity(0, sizeof(s), &s)) { perror("pin"); exit(2); }
}

static double ns_per(long n, double t0, double t1) { return (t1 - t0) / (double)n; }
static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

int main(int argc, char **argv) {
    int cpu   = argc > 1 ? atoi(argv[1]) : 1;
    long iters = argc > 2 ? atol(argv[2]) : 50000000;
    pin(cpu);
    memset(arena, 0, sizeof arena);

    printf("%-10s %-9s %12s %12s %12s\n",
           "offset", "spans", "read ns", "write ns", "atomic ns");

    // 64 is aligned; 60 straddles a 64-byte line; 124 straddles and is also
    // the worst case for a 128-byte sector.
    const int offs[] = {64, 60, 124};
    for (unsigned k = 0; k < sizeof offs / sizeof *offs; k++) {
        int off = offs[k];
        int spans = (off % 64) + 8 > 64;
        volatile uint64_t *p = (volatile uint64_t *)(arena + off);

        double t0 = now();
        uint64_t acc = 0;
        for (long i = 0; i < iters; i++) acc += *p;
        double t1 = now();
        sink = acc;
        double rd = ns_per(iters, t0, t1);

        t0 = now();
        for (long i = 0; i < iters; i++) *p = (uint64_t)i;
        t1 = now();
        double wr = ns_per(iters, t0, t1);

        // Same address, LOCK-prefixed. On a split line x86 must escalate to a
        // bus lock; recent kernels may trap it instead, which shows up as a
        // far larger number still.
        _Atomic uint64_t *a = (_Atomic uint64_t *)(arena + off);
        long an = iters / 10;
        t0 = now();
        for (long i = 0; i < an; i++)
            atomic_fetch_add_explicit(a, 1, memory_order_relaxed);
        t1 = now();
        double at = ns_per(an, t0, t1);

        printf("%-10d %-9s %12.3f %12.3f %12.3f\n",
               off, spans ? "YES" : "no", rd, wr, at);
    }
    return 0;
}
