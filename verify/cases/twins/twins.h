/* regression: cf8ab03 — same file:line, different symbol per TU
   (jemalloc je_-prefix pattern). ordering must not follow shard schedule. */
#ifndef TWIN_PREFIX
#error "define TWIN_PREFIX"
#endif
#define TWIN_CAT2(a, b) a##b
#define TWIN_CAT(a, b) TWIN_CAT2(a, b)
#define TWIN(name) TWIN_CAT(TWIN_PREFIX, name)

unsigned long TWIN(shared_counter) = 0;

void TWIN(writer_a)(void) { TWIN(shared_counter) = 1; }
void TWIN(writer_b)(void) { TWIN(shared_counter) = 2; }
void TWIN(writer_c)(void) { TWIN(shared_counter) += 3; }

/* distinct struct type per TU at one header line: each twin must keep
   its own FL002 finding, not be absorbed as a cross-TU duplicate. */
#include <stdatomic.h>
struct TWIN(counters) {
    _Atomic unsigned long a;
    _Atomic unsigned long b;
};
struct TWIN(counters) TWIN(g_counters);
void TWIN(bump_a)(void) { TWIN(g_counters).a += 1; }
void TWIN(bump_b)(void) { TWIN(g_counters).b += 2; }
