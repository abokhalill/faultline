// Known-positive canaries for detection paths that exist only in C.
//
// The registry gate asks whether a rule fires on some canary, and both
// fixtures here were C++. A rule matching only C++ spellings therefore
// satisfied the gate while reporting nothing on any C codebase, which is
// how FL013 came to miss every spin loop in redis.
#include <pthread.h>
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

// FL003. Owner-indexed striping. The subscript is a field of an object the
// caller handed in, so it names the client's owning thread and not the one
// executing the write; one thread can drive every slot. redis
// io_threads_clients_num has exactly this shape and is written only from the
// main thread. Grades below canary.cpp's g_thread_bytes, which subscripts on
// its own parameter and so names its writer.
typedef struct {
    int tid;
} canary_client;

static int canary_clients_per_thread[64];

__attribute__((hot))
void canary_bind_client(canary_client *c) {
    canary_clients_per_thread[c->tid]++;
}

__attribute__((hot))
void canary_unbind_client(canary_client *c) {
    canary_clients_per_thread[c->tid]--;
}

// FL012. POSIX lock through a free function, which arrives as a CallExpr
// and not a member call. Two sequential lock/unlock pairs, so a depth
// tracker that ignores unlock reports the second as nested.
static pthread_mutex_t canary_handoff_mutex[4];
static int canary_pending[4];

__attribute__((hot))
int canary_drain_pending(int id) {
    pthread_mutex_lock(&canary_handoff_mutex[id]);
    int n = canary_pending[id];
    canary_pending[id] = 0;
    pthread_mutex_unlock(&canary_handoff_mutex[id]);

    pthread_mutex_lock(&canary_handoff_mutex[id]);
    canary_pending[id] += n;
    pthread_mutex_unlock(&canary_handoff_mutex[id]);
    return n;
}
