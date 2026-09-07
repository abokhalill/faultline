// Cross-TU read/write line sharing. The store and the read compile apart, so
// no single TU holds both halves and a per-TU field-evidence gate cannot
// express the pair. Shape taken from redis, where call() stores
// redisCommand::calls in server.c while getKeysFromCommandWithSpecs reads the
// key specs on the same line from db.c.
#ifndef CANARY_XTU_H
#define CANARY_XTU_H

typedef struct {
    unsigned long key_spec_a;
    unsigned long key_spec_b;
    unsigned long key_spec_c;
    unsigned long calls;
} canary_xtu_cmd;

extern canary_xtu_cmd canary_xtu_table[4];

#ifdef __cplusplus
extern "C" {
#endif
unsigned long canary_xtu_lookup(int slot);
#ifdef __cplusplus
}
#endif

#endif
