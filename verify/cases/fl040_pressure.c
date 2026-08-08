/* regression: FL040 Critical requires sustained write pressure —
   a loop write or >=4 sites. Flat 2-site atomics are lifecycle signals. */
#include <stdatomic.h>

/* 6: 2 flat writes -> High (was Critical) */
_Atomic int g_in_progress;

/* 9: 1 write, but in a loop -> Critical */
_Atomic long g_ops;

/* 12: 4 flat writes -> Critical */
_Atomic int g_mode;

void start(void) { g_in_progress = 1; }
void stop(void)  { g_in_progress = 0; }

void worker(int n) {
    for (int i = 0; i < n; i++)
        g_ops += 1;
}

void m1(void) { g_mode = 1; }
void m2(void) { g_mode = 2; }
void m3(void) { g_mode = 3; }
void m4(void) { g_mode = 4; }

/* 28: plain global, single in-loop site -> Informational (one write
   path; concurrent writers would be a race, not a latency hazard) */
long g_parse_pos;

void parse(int n) {
    for (int i = 0; i < n; i++)
        g_parse_pos = i;
}
