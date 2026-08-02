// SPDX-License-Identifier: Apache-2.0
//
// Optional heap attribution for the write-attribution runtime.
//
// Separate from the core because --wrap creates an undefined __real_* for
// every wrapped symbol: linking this without the flags is a link error, and
// forcing the flags on callers who only want global attribution is worse.
//
//   clang target.o lshaz_wattr.o lshaz_wattr_heap.o -lpthread \\
//     -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
#include <stddef.h>

void lshaz_wattr_note_alloc(void *p, size_t n, void *ra);

extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
extern void *__real_realloc(void *, size_t);
extern void  __real_free(void *);

void *__wrap_malloc(size_t n) {
    void *p = __real_malloc(n);
    lshaz_wattr_note_alloc(p, n, __builtin_return_address(0));
    return p;
}
void *__wrap_calloc(size_t a, size_t b) {
    void *p = __real_calloc(a, b);
    lshaz_wattr_note_alloc(p, a * b, __builtin_return_address(0));
    return p;
}
void *__wrap_realloc(void *q, size_t n) {
    void *p = __real_realloc(q, n);
    lshaz_wattr_note_alloc(p, n, __builtin_return_address(0));
    return p;
}
void __wrap_free(void *p) { __real_free(p); }
