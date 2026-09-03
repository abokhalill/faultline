// SPDX-License-Identifier: Apache-2.0
//
// FL010 asserts: on x86-64 only the seq_cst STORE costs anything (XCHG vs
// plain MOV). A load is already a MOV and an RMW is LOCK-prefixed at every
// ordering, so neither weakens into different machine code. That claim sets
// FL010's severity and drives the arm64 variant in regress/run.sh, and has
// never been measured.
//
// Single-threaded and uncontended by design: this measures instruction cost,
// not coherence. Contention would swamp exactly the difference under test.

#define _GNU_SOURCE
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define N 200000000L

static _Atomic long v;
static volatile long sink;

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (sched_setaffinity(0, sizeof(s), &s)) { fprintf(stderr, "FATAL pin\n"); exit(2); }
}

static double ns_per(void (*f)(void), long n) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    f();
    clock_gettime(CLOCK_MONOTONIC, &b);
    return ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / (double)n;
}

#define STORE(name, ord) \
    static void name(void){ for(long i=0;i<N;i++) atomic_store_explicit(&v,i,ord); }
#define LOAD(name, ord) \
    static void name(void){ long s=0; for(long i=0;i<N;i++) s+=atomic_load_explicit(&v,ord); sink=s; }
#define RMW(name, ord) \
    static void name(void){ for(long i=0;i<N;i++) atomic_fetch_add_explicit(&v,1,ord); }

STORE(st_rlx, memory_order_relaxed)
STORE(st_rel, memory_order_release)
STORE(st_sc,  memory_order_seq_cst)
LOAD(ld_rlx, memory_order_relaxed)
LOAD(ld_acq, memory_order_acquire)
LOAD(ld_sc,  memory_order_seq_cst)
RMW(rmw_rlx, memory_order_relaxed)
RMW(rmw_ar,  memory_order_acq_rel)
RMW(rmw_sc,  memory_order_seq_cst)

int main(int argc, char **argv) {
    pin(argc > 1 ? atoi(argv[1]) : 1);
    struct { const char *n; void (*f)(void); } t[] = {
        {"store relaxed", st_rlx}, {"store release", st_rel}, {"store seq_cst", st_sc},
        {"load  relaxed", ld_rlx}, {"load  acquire", ld_acq}, {"load  seq_cst", ld_sc},
        {"rmw   relaxed", rmw_rlx},{"rmw   acq_rel", rmw_ar}, {"rmw   seq_cst", rmw_sc},
    };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) {
        t[i].f();                                   // warm
        double best = 1e18;
        for (int r = 0; r < 3; r++) {               // min of 3: cost is a floor
            double x = ns_per(t[i].f, N);
            if (x < best) best = x;
        }
        printf("%-14s %7.3f ns\n", t[i].n, best);
    }
    return 0;
}
