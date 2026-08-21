// SPDX-License-Identifier: Apache-2.0
//
// FL061 is the third-highest firing rule and has never been measured. It
// claims a wide dispatcher costs through I-cache pressure and misprediction.
//
// btb_cost already showed misprediction is about the target sequence, not the
// target count, so the untested half is I-cache. That is a footprint question:
// arms are inlined into one function, so a dispatcher with many fat arms
// occupies more instruction bytes than the L1I can hold and every dispatch
// fetches cold.
//
// Two axes, separated: arm count at fixed arm size, and arm size at fixed
// count. If only the product matters it is footprint; if count matters alone
// it is something else.

#define _GNU_SOURCE
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static volatile uint64_t sink;
#define SEQ (1 << 16)
static unsigned seq[SEQ];

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (sched_setaffinity(0, sizeof(s), &s)) { perror("pin"); exit(2); }
}
static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

// Arm bodies sized by repetition. noinline would defeat the point: FL061's
// mechanism is that arms sit inline in the dispatcher's own footprint.
#define W1(n)  acc += (uint64_t)(n); acc ^= acc >> 3; acc *= 0x9E3779B97F4A7C15ULL;
#define W4(n)  W1(n) W1(n+1) W1(n+2) W1(n+3)
#define W16(n) W4(n) W4(n+4) W4(n+8) W4(n+12)
#define W64(n) W16(n) W16(n+16) W16(n+32) W16(n+48)

#define ARM(i, BODY) case i: { BODY(i) } break;
#define ARMS8(BODY)   ARM(0,BODY)  ARM(1,BODY)  ARM(2,BODY)  ARM(3,BODY) \
                      ARM(4,BODY)  ARM(5,BODY)  ARM(6,BODY)  ARM(7,BODY)
#define ARMS32(BODY)  ARMS8(BODY) \
                      ARM(8,BODY)  ARM(9,BODY)  ARM(10,BODY) ARM(11,BODY) \
                      ARM(12,BODY) ARM(13,BODY) ARM(14,BODY) ARM(15,BODY) \
                      ARM(16,BODY) ARM(17,BODY) ARM(18,BODY) ARM(19,BODY) \
                      ARM(20,BODY) ARM(21,BODY) ARM(22,BODY) ARM(23,BODY) \
                      ARM(24,BODY) ARM(25,BODY) ARM(26,BODY) ARM(27,BODY) \
                      ARM(28,BODY) ARM(29,BODY) ARM(30,BODY) ARM(31,BODY)

#define DISPATCHER(name, ARMS, BODY)                       \
    static uint64_t name(unsigned k, uint64_t acc) {       \
        switch (k) { ARMS(BODY) default: break; }          \
        return acc;                                        \
    }

DISPATCHER(d8_thin,   ARMS8,  W1)
DISPATCHER(d8_fat,    ARMS8,  W64)
DISPATCHER(d32_thin,  ARMS32, W1)
DISPATCHER(d32_fat,   ARMS32, W64)

static double run(uint64_t (*fn)(unsigned, uint64_t), unsigned arms, long n) {
    for (int i = 0; i < SEQ; i++) seq[i] = (unsigned)(random() % arms);
    uint64_t acc = 0;
    for (long i = 0; i < n / 4; i++) acc = fn(seq[i & (SEQ - 1)], acc);
    double t0 = now();
    for (long i = 0; i < n; i++) acc = fn(seq[i & (SEQ - 1)], acc);
    double t1 = now();
    sink = acc;
    return (t1 - t0) / n;
}

int main(int argc, char **argv) {
    int cpu = argc > 1 ? atoi(argv[1]) : 1;
    long n  = argc > 2 ? atol(argv[2]) : 20000000;
    pin(cpu);
    srandom(12345);

    printf("%-10s %-8s %10s   %s\n", "arms", "armsize", "ns/dispatch", "note");
    printf("%-10d %-8s %10.3f\n",  8, "thin",  run(d8_thin,   8,  n));
    printf("%-10d %-8s %10.3f\n", 32, "thin",  run(d32_thin, 32,  n));
    printf("%-10d %-8s %10.3f\n",  8, "fat",   run(d8_fat,    8,  n));
    printf("%-10d %-8s %10.3f\n", 32, "fat",   run(d32_fat,  32,  n));
    printf("\nthin arms isolate dispatch width; fat arms add footprint.\n");
    printf("if only fat*32 rises, the mechanism is I-cache, not arm count.\n");
    return 0;
}
