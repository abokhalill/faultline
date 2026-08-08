/* recall: the four shapes false sharing takes. Atomicity is not the gate —
   striped and role-partitioned fields guarantee single-writer-per-slot and
   are therefore routinely plain — and an array is one FieldDecl, so pairing
   distinct decls alone can never see two of its elements share a line.
   Both holes were silent; this fixture is the canary. */
#include <pthread.h>
#include <stdatomic.h>

/* 10: atomic scalars */
struct fs_atom_scalar { _Atomic unsigned long head; _Atomic unsigned long tail; char pad[48]; };
/* 12: two ELEMENTS of one atomic array */
struct fs_atom_array { _Atomic unsigned long idx[2]; char pad[48]; };
/* 14: plain scalars, distinct owners, no data race */
struct fs_plain_scalar { unsigned long head; unsigned long tail; char pad[48]; };
/* 16: two ELEMENTS of one plain array */
struct fs_plain_array { unsigned long slot[8]; char pad[32]; };

static struct fs_atom_scalar  a;
static struct fs_atom_array   b;
static struct fs_plain_scalar c;
static struct fs_plain_array  d;

static void *pa(void *x) { (void)x; for (;;) atomic_fetch_add(&a.head, 1); }
static void *ca(void *x) { (void)x; for (;;) atomic_fetch_add(&a.tail, 1); }
static void *pb(void *x) { (void)x; for (;;) atomic_fetch_add(&b.idx[0], 1); }
static void *cb(void *x) { (void)x; for (;;) atomic_fetch_add(&b.idx[1], 1); }
static void *pc(void *x) { (void)x; for (;;) c.head++; }
static void *cc(void *x) { (void)x; for (;;) c.tail++; }
static void *pd(void *x) { (void)x; for (;;) d.slot[0]++; }
static void *cd(void *x) { (void)x; for (;;) d.slot[1]++; }

void fs_start(void) {
    pthread_t t;
    pthread_create(&t, 0, pa, 0); pthread_create(&t, 0, ca, 0);
    pthread_create(&t, 0, pb, 0); pthread_create(&t, 0, cb, 0);
    pthread_create(&t, 0, pc, 0); pthread_create(&t, 0, cc, 0);
    pthread_create(&t, 0, pd, 0); pthread_create(&t, 0, cd, 0);
}
