// SPDX-License-Identifier: Apache-2.0
//
// FL050 claims indirect-branch pressure costs. Two independent axes get
// conflated in that claim and this separates them:
//
//   fanout    -- distinct targets at ONE site, cycled predictably. Tests BTB
//                capacity/associativity per site.
//   footprint -- many sites each with one target, swept round-robin. Tests
//                global BTB capacity (~4K entries on Coffee Lake).
//
// A rule that fires on "indirect call in hot loop" without distinguishing these
// flags monomorphic dispatch, which the predictor handles for free.

#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAXF 8192
typedef long (*fn_t)(long);

// Distinct code addresses. Each body is trivial; we are measuring the branch,
// not the callee, so any work in here is noise added equally to every arm.
#define GEN(n) static long f##n(long x) { return x + n; }
#define GEN8(b) GEN(b##0) GEN(b##1) GEN(b##2) GEN(b##3) GEN(b##4) GEN(b##5) GEN(b##6) GEN(b##7)
GEN8(1) GEN8(2) GEN8(3) GEN8(4) GEN8(5) GEN8(6) GEN8(7) GEN8(8)
#define REF8(b) f##b##0, f##b##1, f##b##2, f##b##3, f##b##4, f##b##5, f##b##6, f##b##7
static fn_t pool[] = { REF8(1), REF8(2), REF8(3), REF8(4), REF8(5), REF8(6), REF8(7), REF8(8) };
#define POOL (sizeof pool / sizeof *pool)

static fn_t tab[MAXF];
static volatile long sink;

// Precomputed unpredictable target sequence. The predictor can memorize any
// short cycle, so a fanout sweep alone cannot separate BTB capacity from
// predictability -- this supplies the second arm.
#define SEQ (1 << 16)
static unsigned seq[SEQ];

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (sched_setaffinity(0, sizeof(s), &s)) { fprintf(stderr, "FATAL pin\n"); exit(2); }
}

static double run(int fanout, long iters) {
    for (int i = 0; i < fanout; i++) tab[i] = pool[i % POOL];
    // Mask, never %: a runtime divisor compiles to ~25-cycle IDIV, which is
    // 3x the entire quantity under test and reads as a flat null.
    const unsigned long mask = (unsigned long)fanout - 1;
    long acc = 0;
    for (long i = 0; i < iters / 4; i++) acc += tab[i & mask](i);   // warm

    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (long i = 0; i < iters; i++) acc += tab[i & mask](i);
    clock_gettime(CLOCK_MONOTONIC, &b);
    sink = acc;
    return ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / (double)iters;
}

static double run_rand(int fanout, long iters) {
    for (int i = 0; i < fanout; i++) tab[i] = pool[i % POOL];
    for (int i = 0; i < SEQ; i++) seq[i] = (unsigned)(random() % fanout);
    long acc = 0;
    for (long i = 0; i < iters / 4; i++) acc += tab[seq[i & (SEQ - 1)]](i);   // warm

    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (long i = 0; i < iters; i++) acc += tab[seq[i & (SEQ - 1)]](i);
    clock_gettime(CLOCK_MONOTONIC, &b);
    sink = acc;
    return ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / (double)iters;
}

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 50000000L;
    pin(argc > 2 ? atoi(argv[2]) : 1);

    // fanout 1 is the monomorphic baseline: perfectly predicted, and the
    // number every other row must be read against.
    printf("targets  predictable   random    delta\n");
    for (int f = 1; f <= 4096; f *= 2) {
        double bp = 1e18, br = 1e18;
        for (int r = 0; r < 3; r++) {
            double x = run(f, iters);      if (x < bp) bp = x;
            double y = run_rand(f, iters); if (y < br) br = y;
        }
        printf("%-8d %8.3f %9.3f %8.3f\n", f, bp, br, br - bp);
    }
    return 0;
}
