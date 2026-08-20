// SPDX-License-Identifier: Apache-2.0
//
// FL060 asserts remote memory costs ~100-300ns against ~60-80ns local. That
// number has never been measured on any machine this project has had, because
// none of them had two sockets.
//
// Latency is a dependent pointer chase over a working set well past LLC, so
// every step is a real memory access the prefetcher cannot hide. Bandwidth is
// a separate question and answered separately: a rule that conflates them
// prescribes the wrong fix.

#define _GNU_SOURCE
#include <numa.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

volatile char     *sink_ptr;
volatile uint64_t  sink_u64;

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (sched_setaffinity(0, sizeof(s), &s)) { perror("pin"); exit(2); }
}

static double now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s <cpu> <node> <lat|bw> <size_mb>\n", argv[0]);
        return 2;
    }
    int cpu = atoi(argv[1]), node = atoi(argv[2]);
    int do_lat = strcmp(argv[3], "lat") == 0;
    size_t bytes = (size_t)atoll(argv[4]) << 20;

    if (numa_available() < 0) { fprintf(stderr, "no libnuma\n"); return 2; }
    pin(cpu);

    void *buf = numa_alloc_onnode(bytes, node);
    if (!buf) { fprintf(stderr, "numa_alloc_onnode failed\n"); return 2; }
    memset(buf, 0, bytes);   // fault in before timing; first touch is not the thing under test

    if (do_lat) {
        // One pointer per cache line, shuffled. Sequential order would be
        // prefetched and would measure the prefetcher, not memory.
        size_t n = bytes / 64;
        size_t *idx = malloc(n * sizeof(size_t));
        for (size_t i = 0; i < n; i++) idx[i] = i;
        for (size_t i = n - 1; i > 0; i--) {
            size_t j = (size_t)rand() % (i + 1);
            size_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
        }
        char **p = (char **)buf;
        for (size_t i = 0; i < n; i++)
            *(char **)((char *)buf + idx[i] * 64) =
                (char *)buf + idx[(i + 1) % n] * 64;
        free(idx);

        // Plain pointer, sunk after the loop. The load-to-load dependency is
        // what serialises the chase; a volatile here adds a store per step and
        // measures that instead.
        char *cur = (char *)buf;
        for (size_t i = 0; i < n; i++) cur = *(char **)cur;   // warm

        size_t steps = n * 4;
        double t0 = now_ns();
        for (size_t i = 0; i < steps; i++) cur = *(char **)cur;
        double t1 = now_ns();
        sink_ptr = cur;
        (void)p;
        printf("cpu=%-3d node=%-2d lat  %8.2f ns/access  (ws=%zu MB)\n",
               cpu, node, (t1 - t0) / steps, bytes >> 20);
    } else {
        uint64_t sum = 0;
        uint64_t *q = (uint64_t *)buf;
        size_t n = bytes / 8;
        for (size_t i = 0; i < n; i++) sum += q[i];        // warm

        double t0 = now_ns();
        for (int r = 0; r < 4; r++)
            for (size_t i = 0; i < n; i++) sum += q[i];
        double t1 = now_ns();
        sink_u64 = sum;
        double gbs = (double)bytes * 4 / (t1 - t0);        // bytes/ns == GB/s
        printf("cpu=%-3d node=%-2d read %8.2f GB/s        (ws=%zu MB)\n",
               cpu, node, gbs, bytes >> 20);
    }
    numa_free(buf, bytes);
    return 0;
}
