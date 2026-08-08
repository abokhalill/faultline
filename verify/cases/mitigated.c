/* regression: d52b726 — deliberate layout demotes FL002 to Medium;
   904777a — proven tier requires alignment; unmitigated stays Critical */
#include <stdatomic.h>

/* 6: explicit line alignment -> FL002 Medium */
struct __attribute__((aligned(64))) io_thread_like {
    _Atomic int paused;
    _Atomic int running;
    void *el;
};

/* 13: trailing pad-to-line -> FL002 Medium */
struct mem_entry_like {
    _Atomic long long used;
    _Atomic long long peak;
    char padding[64 - 2 * sizeof(long long)];
};

/* 20: no mitigation signal -> FL002 Critical */
struct unmitigated {
    _Atomic long long x;
    _Atomic long long y;
};

/* 26: FL090-eligible (>=3 lines, atomics, escape), aligned -> Medium */
struct __attribute__((aligned(64))) amplified_mitigated {
    _Atomic long long a;
    char pad0[56];
    _Atomic long long b;
    char pad1[56];
    long long hot[8];
};

/* 36: same signals, no layout intent -> FL090 Critical */
struct amplified_raw {
    _Atomic long long a;
    long long f[10];
    _Atomic long long b;
    long long g[10];
};

/* 44: unmitigated pair but no observed writers -> High, not Critical */
struct unwritten {
    _Atomic long long x;
    _Atomic long long y;
};

struct io_thread_like g_io;
struct mem_entry_like g_mem;
struct unmitigated g_un;
struct amplified_mitigated g_am;
struct amplified_raw g_ar;
struct unwritten g_uw;

void touch_a(long long v) { g_am.a += v; g_ar.a += v; }
void touch_b(long long v) { g_am.b += v; g_ar.b += v; }
void wx(long long v) { g_un.x += v; }
void wy(long long v) { g_un.y += v; }

/* FL090 needs a sharing route independent of "contains an atomic": the
   escape verdict sets escapes for any record with an atomic member, so
   counting that as a third signal scored one fact twice and reported a
   compound hazard for a large struct nobody shares. pthread_create is
   declared rather than #included so the line numbers above stay fixed. */
extern int pthread_create(unsigned long *, const void *,
                          void *(*)(void *), void *);

static void *thr_a(void *p) { (void)p; g_am.a += 1; g_ar.a += 1; return 0; }
static void *thr_b(void *p) { (void)p; g_am.b += 1; g_ar.b += 1; return 0; }

void spawn_writers(void) {
    unsigned long t;
    pthread_create(&t, 0, thr_a, 0);
    pthread_create(&t, 0, thr_b, 0);
}

/* same geometry as amplified_raw, written only from ordinary functions:
   no sharing route, so no compound. FL001/FL002 still report the layout. */
struct amplified_unshared {
    _Atomic long long a;
    long long f[10];
    _Atomic long long b;
    long long g[10];
};
struct amplified_unshared g_us;
void us_a(long long v) { g_us.a += v; }
void us_b(long long v) { g_us.b += v; }
