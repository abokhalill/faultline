/* regression: 43ddaba — C11/GNU atomics invisible to FL010/FL011 */
#include <stdatomic.h>

struct counters {
    _Atomic unsigned long hits;
    _Atomic unsigned long misses;
    unsigned long cold[14];
};
struct counters g;
unsigned long plain_counter;

__attribute__((hot)) void record(unsigned long v) {
    g.hits++;                                                     /* 13: rmw  -> FL010 Medium */
    g.misses = v;                                                 /* 14: store -> FL010 High */
    atomic_store(&g.misses, v);                                   /* 15: store -> FL010 High */
    atomic_fetch_add_explicit(&g.hits, 1, memory_order_relaxed);  /* 16: skip */
    atomic_store_explicit(&g.misses, 0, memory_order_release);    /* 17: skip */
    __atomic_store_n(&plain_counter, v, __ATOMIC_SEQ_CST);        /* 18: store -> FL010 High */
    __sync_fetch_and_add(&plain_counter, 1);                      /* 19: rmw -> FL010 Medium */
}
