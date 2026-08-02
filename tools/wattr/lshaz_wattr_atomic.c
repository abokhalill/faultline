// SPDX-License-Identifier: Apache-2.0
//
// The __tsan_atomic* ABI, for targets that use C11/C++11 atomics.
//
// Clang rewrites every atomic operation in an instrumented TU into a call
// here. Without these, anything using atomics -- which is every interesting
// concurrent program -- fails to link.
//
// The whole risk in this file is memory ordering. These functions must
// perform the real operation with the ordering the program asked for: get it
// wrong and the target's behaviour silently changes, which for a measurement
// tool means confidently wrong numbers rather than a crash. So the ordering
// is translated explicitly rather than assumed to match, and anything
// unrecognised is STRENGTHENED to seq_cst. Over-ordering costs speed in a
// build that is already an order of magnitude slow; under-ordering is a
// correctness bug in someone else's program.
#include <stdint.h>

void lshaz_wattr_note_write(void *addr, unsigned size);

// compiler-rt's __tsan_memory_order. Deliberately not assumed to equal the
// C11 values even though they currently coincide.
enum {
    TSAN_MO_RELAXED = 0,
    TSAN_MO_CONSUME = 1,
    TSAN_MO_ACQUIRE = 2,
    TSAN_MO_RELEASE = 3,
    TSAN_MO_ACQ_REL = 4,
    TSAN_MO_SEQ_CST = 5,
};

static inline int mo_of(int m) {
    switch (m) {
        case TSAN_MO_RELAXED: return __ATOMIC_RELAXED;
        case TSAN_MO_CONSUME: return __ATOMIC_CONSUME;
        case TSAN_MO_ACQUIRE: return __ATOMIC_ACQUIRE;
        case TSAN_MO_RELEASE: return __ATOMIC_RELEASE;
        case TSAN_MO_ACQ_REL: return __ATOMIC_ACQ_REL;
        case TSAN_MO_SEQ_CST: return __ATOMIC_SEQ_CST;
        default:              return __ATOMIC_SEQ_CST;
    }
}

// A compare-exchange failure order may not be release or acq_rel, and may
// not exceed the success order. Violating either is undefined, and these
// arrive as runtime values, so they are clamped rather than trusted.
static inline int fail_mo_of(int fail, int succ) {
    int f = mo_of(fail);
    if (f == __ATOMIC_RELEASE || f == __ATOMIC_ACQ_REL)
        f = __ATOMIC_ACQUIRE;
    int s = mo_of(succ);
    if (s == __ATOMIC_RELAXED)
        f = __ATOMIC_RELAXED;
    else if (s == __ATOMIC_RELEASE && f == __ATOMIC_ACQUIRE)
        f = __ATOMIC_RELAXED;
    return f;
}

// Loads are not recorded: they cause no ownership transfer, and the writer
// that makes a line contended is recorded on its own account.
#define ATOMIC_OPS(BITS, TYPE)                                                \
    TYPE __tsan_atomic##BITS##_load(const volatile TYPE *a, int m) {          \
        return __atomic_load_n((const TYPE *)a, mo_of(m));                    \
    }                                                                         \
    void __tsan_atomic##BITS##_store(volatile TYPE *a, TYPE v, int m) {       \
        lshaz_wattr_note_write((void *)a, sizeof(TYPE));                      \
        __atomic_store_n((TYPE *)a, v, mo_of(m));                             \
    }                                                                         \
    TYPE __tsan_atomic##BITS##_exchange(volatile TYPE *a, TYPE v, int m) {    \
        lshaz_wattr_note_write((void *)a, sizeof(TYPE));                      \
        return __atomic_exchange_n((TYPE *)a, v, mo_of(m));                   \
    }                                                                         \
    TYPE __tsan_atomic##BITS##_fetch_add(volatile TYPE *a, TYPE v, int m) {   \
        lshaz_wattr_note_write((void *)a, sizeof(TYPE));                      \
        return __atomic_fetch_add((TYPE *)a, v, mo_of(m));                    \
    }                                                                         \
    TYPE __tsan_atomic##BITS##_fetch_sub(volatile TYPE *a, TYPE v, int m) {   \
        lshaz_wattr_note_write((void *)a, sizeof(TYPE));                      \
        return __atomic_fetch_sub((TYPE *)a, v, mo_of(m));                    \
    }                                                                         \
    TYPE __tsan_atomic##BITS##_fetch_and(volatile TYPE *a, TYPE v, int m) {   \
        lshaz_wattr_note_write((void *)a, sizeof(TYPE));                      \
        return __atomic_fetch_and((TYPE *)a, v, mo_of(m));                    \
    }                                                                         \
    TYPE __tsan_atomic##BITS##_fetch_or(volatile TYPE *a, TYPE v, int m) {    \
        lshaz_wattr_note_write((void *)a, sizeof(TYPE));                      \
        return __atomic_fetch_or((TYPE *)a, v, mo_of(m));                     \
    }                                                                         \
    TYPE __tsan_atomic##BITS##_fetch_xor(volatile TYPE *a, TYPE v, int m) {   \
        lshaz_wattr_note_write((void *)a, sizeof(TYPE));                      \
        return __atomic_fetch_xor((TYPE *)a, v, mo_of(m));                    \
    }                                                                         \
    TYPE __tsan_atomic##BITS##_fetch_nand(volatile TYPE *a, TYPE v, int m) {  \
        lshaz_wattr_note_write((void *)a, sizeof(TYPE));                      \
        return __atomic_fetch_nand((TYPE *)a, v, mo_of(m));                   \
    }                                                                         \
    /* A failed CAS still takes the line exclusively on x86 (LOCK CMPXCHG),  \
       so it is recorded too: the coherence traffic is what we measure. */    \
    int __tsan_atomic##BITS##_compare_exchange_strong(                        \
            volatile TYPE *a, TYPE *c, TYPE v, int m, int fm) {               \
        lshaz_wattr_note_write((void *)a, sizeof(TYPE));                      \
        return __atomic_compare_exchange_n((TYPE *)a, c, v, 0,                \
                                           mo_of(m), fail_mo_of(fm, m));      \
    }                                                                         \
    int __tsan_atomic##BITS##_compare_exchange_weak(                          \
            volatile TYPE *a, TYPE *c, TYPE v, int m, int fm) {               \
        lshaz_wattr_note_write((void *)a, sizeof(TYPE));                      \
        return __atomic_compare_exchange_n((TYPE *)a, c, v, 1,                \
                                           mo_of(m), fail_mo_of(fm, m));      \
    }                                                                         \
    TYPE __tsan_atomic##BITS##_compare_exchange_val(                          \
            volatile TYPE *a, TYPE c, TYPE v, int m, int fm) {                \
        lshaz_wattr_note_write((void *)a, sizeof(TYPE));                      \
        TYPE expected = c;                                                    \
        __atomic_compare_exchange_n((TYPE *)a, &expected, v, 0,               \
                                    mo_of(m), fail_mo_of(fm, m));             \
        return expected;  /* the value found, per the ABI */                  \
    }

ATOMIC_OPS(8,  int8_t)
ATOMIC_OPS(16, int16_t)
ATOMIC_OPS(32, int32_t)
ATOMIC_OPS(64, int64_t)

void __tsan_atomic_thread_fence(int m) { __atomic_thread_fence(mo_of(m)); }
void __tsan_atomic_signal_fence(int m) { __atomic_signal_fence(mo_of(m)); }
