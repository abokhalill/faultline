/* regression: pair co-residability — atomics >64B apart can never share
   a line under any base shift; shift-union bucketing claimed they did. */
#include <stdatomic.h>

/* 6: pads make co-residence impossible -> no pair, not Critical */
struct separated {
    _Atomic int c1;
    char pad[64];
    _Atomic int c2;
    long long data[10];
};

/* 14: adjacent atomics -> pair under every shift, Critical */
struct together {
    _Atomic int c1;
    _Atomic int c2;
    long long data[10];
};

struct separated g_s;
struct together g_t;

void w1(int v) { g_s.c1 += v; g_t.c1 += v; }
void w2(int v) { g_s.c2 += v; g_t.c2 += v; }
