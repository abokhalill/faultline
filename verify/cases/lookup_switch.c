/* regression: a switch whose every arm returns a constant is a lookup, not
   a branch tree. The compiler emits a jump table into trivial stubs or an
   indexed array with no branch at all, so FL050's BTB-capacity mechanism
   does not apply and claiming it is a fabricated mechanism. The control
   below is the same shape with real work per arm and must still fire. */

__attribute__((hot))
const char *state_name(int s) {
    switch (s) {
    case 0: return "none";
    case 1: return "wait_start";
    case 2: return "select";
    case 3: return "send";
    case 4: return "promote";
    case 5: return "reconf";
    case 6: return "update";
    default: return "unknown";
    }
}

extern int work_a(int), work_b(int), work_c(int), work_d(int);

/* 24: control — same case count, real work per arm, must still fire */
__attribute__((hot))
int dispatch_work(int s, int v) {
    switch (s) {
    case 0: return work_a(v);
    case 1: return work_b(v) + 1;
    case 2: { int t = work_c(v); return t * 2; }
    case 3: return work_d(v) - work_a(v);
    case 4: { int t = work_b(v); return t ? work_c(t) : 0; }
    case 5: return work_d(v) + work_b(v);
    case 6: { int t = work_a(v); return t + work_c(t); }
    default: return work_d(v);
    }
}
