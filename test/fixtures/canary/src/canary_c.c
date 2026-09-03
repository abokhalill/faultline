// Known-positive canaries for detection paths that exist only in C.
//
// The registry gate asks whether a rule fires on some canary, and both
// fixtures here were C++. A rule matching only C++ spellings therefore
// satisfied the gate while reporting nothing on any C codebase, which is
// how FL013 came to miss every spin loop in redis.
#include <stdatomic.h>

#define CANARY_UNPAUSED 0
#define CANARY_PAUSING  1
#define CANARY_PAUSED   2

typedef struct {
    _Atomic int paused;
} canary_io_thread;

static canary_io_thread canary_io_threads[4];

// FL013. Bare spin on a C11 _Atomic through atomic_load_explicit, which
// Clang models as an AtomicExpr and not a CallExpr. Shape taken from
// redis pauseIOThreadsRange.
__attribute__((hot))
void canary_wait_paused(int id) {
    int paused = CANARY_PAUSING;
    while (paused != CANARY_PAUSED)
        paused = atomic_load_explicit(&canary_io_threads[id].paused,
                                      memory_order_seq_cst);
}

void canary_release(int id) {
    atomic_store_explicit(&canary_io_threads[id].paused, CANARY_UNPAUSED,
                          memory_order_seq_cst);
}
