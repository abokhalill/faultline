// SPDX-License-Identifier: Apache-2.0
//
// What FL001 actually claims: a struct occupying more than one cache line,
// with atomics on several of those lines, costs "RFO traffic on each distinct
// line". One thread touching every field then pays a line fill per line
// instead of one.
//
// Note this is in tension with FL002, which flags atomics sharing a line. Both
// cannot be Critical for the same pair of fields: separating them is FL002's
// prescribed fix. The single-thread arm here measures footprint honestly, with
// no other thread to contend, so whatever it shows is the cost of the fix
// rather than of the hazard.

#define _GNU_SOURCE
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define LINE 64

struct packed_s {                       // both atomics inside one line
    _Atomic uint64_t a, b;
    char pad[LINE - 2 * sizeof(uint64_t)];
} __attribute__((aligned(LINE)));

struct spread_s {                       // one atomic per line: spans two
    _Atomic uint64_t a;
    char pad0[LINE - sizeof(uint64_t)];
    _Atomic uint64_t b;
    char pad1[LINE - sizeof(uint64_t)];
} __attribute__((aligned(LINE)));

static struct packed_s P;
static struct spread_s S;
static volatile uint64_t sink;

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (sched_setaffinity(0, sizeof(s), &s)) { perror("pin"); exit(2); }
}
static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

int main(int argc, char **argv) {
    int cpu = argc > 1 ? atoi(argv[1]) : 1;
    long n  = argc > 2 ? atol(argv[2]) : 20000000;
    pin(cpu);

    printf("%-22s %10s %12s\n", "layout", "lines", "ns/iter");

    uint64_t acc = 0;
    double t0 = now();
    for (long i = 0; i < n; i++) {
        atomic_fetch_add_explicit(&P.a, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&P.b, 1, memory_order_relaxed);
        acc += atomic_load_explicit(&P.a, memory_order_relaxed);
    }
    double t1 = now(); sink = acc;
    printf("%-22s %10d %12.3f\n", "both in one line", 1, (t1 - t0) / n);

    acc = 0;
    t0 = now();
    for (long i = 0; i < n; i++) {
        atomic_fetch_add_explicit(&S.a, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&S.b, 1, memory_order_relaxed);
        acc += atomic_load_explicit(&S.a, memory_order_relaxed);
    }
    t1 = now(); sink = acc;
    printf("%-22s %10d %12.3f\n", "one per line (spans)", 2, (t1 - t0) / n);

    // The other half of FL001's claim is eviction, which is a working-set
    // argument a single hot instance cannot test. Same field count, double the
    // footprint, swept so the array outruns cache.
    //
    // Runs to 2M because the bound has to clear the LLC of the part in hand,
    // not the part the sweep was written on. 262144 tops out at 32MB spread,
    // which a 5950X holds entirely in its 32MB per-CCD L3.
    for (long count = 4096; count <= 2097152; count *= 8) {
        struct packed_s *pa = aligned_alloc(LINE, count * sizeof *pa);
        struct spread_s *sa = aligned_alloc(LINE, count * sizeof *sa);
        if (!pa || !sa) { fprintf(stderr, "alloc\n"); return 2; }
        for (long i = 0; i < count; i++) { pa[i].a = 0; sa[i].a = 0; }

        long reps = 40000000 / count + 1;
        acc = 0; t0 = now();
        for (long r = 0; r < reps; r++)
            for (long i = 0; i < count; i++)
                acc += atomic_fetch_add_explicit(&pa[i].a, 1, memory_order_relaxed);
        t1 = now(); sink = acc;
        double pk = (t1 - t0) / (reps * count);

        acc = 0; t0 = now();
        for (long r = 0; r < reps; r++)
            for (long i = 0; i < count; i++)
                acc += atomic_fetch_add_explicit(&sa[i].a, 1, memory_order_relaxed);
        t1 = now(); sink = acc;
        double sp = (t1 - t0) / (reps * count);

        printf("array n=%-7ld  %4ldKB vs %4ldKB   packed %7.3f  spread %7.3f  %+.1f%%\n",
               count, count * sizeof *pa >> 10, count * sizeof *sa >> 10,
               pk, sp, 100.0 * (sp - pk) / pk);
        free(pa); free(sa);
    }
    return 0;
}
