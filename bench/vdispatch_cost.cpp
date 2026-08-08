// SPDX-License-Identifier: Apache-2.0
//
// FL060/FL061 flag virtual dispatch in hot paths. The cost is not the indirect
// call -- btb_cost shows a predicted one is nearly free -- it is (a) the missed
// inline and (b) the BTB miss when the receiver type varies. Arms:
//
//   direct  : non-virtual, inlinable                  (floor)
//   mono    : virtual, one concrete type              (predictable, no inline)
//   poly-k  : virtual, k types interleaved            (adds misprediction)
//
// direct->mono isolates the inlining loss; mono->poly isolates prediction. A
// rule that reports one number for "virtual call" cannot tell a caller which
// of the two it is paying, and they have different fixes.

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <sched.h>

struct Base { virtual ~Base() = default; virtual long op(long x) const = 0; };
template <int N> struct Impl : Base { long op(long x) const override { return x + N; } };
struct Plain { long op(long x) const { return x + 1; } };   // non-virtual twin of Impl<1>

static volatile long sink;

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (sched_setaffinity(0, sizeof(s), &s)) { std::fprintf(stderr, "FATAL pin\n"); std::exit(2); }
}

static double timed(long iters, long (*body)(long)) {
    body(iters / 4);                                        // warm
    double best = 1e18;
    for (int r = 0; r < 3; r++) {
        timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        long acc = body(iters);
        clock_gettime(CLOCK_MONOTONIC, &b);
        sink = acc;
        double x = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / double(iters);
        if (x < best) best = x;
    }
    return best;
}

// Fixed power-of-two ring so the index is an AND. A runtime `% k` is a
// ~25-cycle IDIV that buries the dispatch cost entirely. Sized past what the
// predictor can memorize as a repeating history, so the shuffled arm stays
// unpredictable rather than being learned.
static constexpr size_t RING = 1 << 16;
static Base *objs[RING];

static long run_direct(long n) { Plain p; long a = 0; for (long i = 0; i < n; i++) a += p.op(i); return a; }
static long run_virt(long n)   { long a = 0;
                                 { for (long i = 0; i < n; i++) a += objs[i & (RING - 1)]->op(i); } return a; }

int main(int argc, char **argv) {
    long iters = argc > 1 ? std::atol(argv[1]) : 50000000L;
    pin(argc > 2 ? std::atoi(argv[2]) : 1);

    std::printf("direct       ns_per_call=%6.3f\n", timed(iters, run_direct));

    // Receiver cycle length k. k=1 is monomorphic: the indirect target is
    // invariant, so only the lost inline is being paid.
    Base *pool[] = { new Impl<1>, new Impl<2>, new Impl<3>, new Impl<4>,
                     new Impl<5>, new Impl<6>, new Impl<7>, new Impl<8> };
    std::printf("types   cyclic  shuffled    delta\n");
    for (int k : {1, 2, 4, 8}) {
        for (size_t i = 0; i < RING; i++) objs[i] = pool[i % k];
        double cyc = timed(iters, run_virt);
        for (size_t i = 0; i < RING; i++) objs[i] = pool[std::rand() % k];
        double shuf = timed(iters, run_virt);
        std::printf("%-7d %7.3f %9.3f %8.3f\n", k, cyc, shuf, shuf - cyc);
    }
    return 0;
}
