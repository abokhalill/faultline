// SPDX-License-Identifier: Apache-2.0
//
// FL013 quotes ~140 cycles for PAUSE and labels it unmeasured. That figure is
// Skylake's: Intel raised PAUSE roughly 10x there and AMD never followed, so a
// spin-wait rule carrying one number is wrong on whichever vendor it skipped.
//
// Loop overhead is subtracted using an identical loop with the PAUSE block
// removed, and cycles come from an in-situ calibration rather than a nominal
// clock, since a fixed governor still is not a promise about frequency.

#define _GNU_SOURCE
#include <immintrin.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (sched_setaffinity(0, sizeof(s), &s)) { perror("pin"); exit(2); }
}
static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

#define TEN(x) x x x x x x x x x x

// A serial dependent add chain retires one per cycle, which turns wall time
// into a frequency reading taken on the core under test.
static double calibrateGHz(long n) {
    double t0 = now();
    uint64_t a = 0;
    for (long i = 0; i < n; i++) { TEN(__asm__ volatile("add $1,%0" : "+r"(a));) }
    double ns = now() - t0;
    __asm__ volatile("" :: "r"(a));
    return (double)(n * 10) / ns;
}

int main(int argc, char **argv) {
    int cpu   = argc > 1 ? atoi(argv[1]) : 4;
    long n    = argc > 2 ? atol(argv[2]) : 20000000;
    int reps  = argc > 3 ? atoi(argv[3]) : 5;
    pin(cpu);

    double ghz = 0, bestPause = 1e18, bestEmpty = 1e18;
    for (int r = 0; r < reps; r++) {
        double g = calibrateGHz(n / 10);
        if (g > ghz) ghz = g;

        double t0 = now();
        for (long i = 0; i < n; i++) { TEN(_mm_pause();) }
        double p = (now() - t0) / (double)(n * 10);
        if (p < bestPause) bestPause = p;

        volatile long k = 0;
        t0 = now();
        for (long i = 0; i < n; i++) { TEN(k++;) }
        double e = (now() - t0) / (double)(n * 10);
        if (e < bestEmpty) bestEmpty = e;
    }

    double net = bestPause - bestEmpty;
    printf("cpu%d, %ld x10 iters, min of %d\n", cpu, n, reps);
    printf("  calibrated          %8.3f GHz\n", ghz);
    printf("  loop baseline       %8.3f ns/op\n", bestEmpty);
    printf("  pause (gross)       %8.3f ns/op\n", bestPause);
    printf("  pause (net)         %8.3f ns  = %.1f cycles\n", net, net * ghz);
    return 0;
}
