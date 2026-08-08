/* regression: 1990d1a — FAM structs invisible to layout rules */
#include <stdatomic.h>

struct ring {                       /* 4: FL001+FL002+FL041 anchor */
    _Atomic unsigned long head;
    _Atomic unsigned long tail;
    unsigned long len;
    char data[];
};

struct ring *g_ring;

void publish(unsigned long v) {
    atomic_store(&g_ring->head, v);
}
