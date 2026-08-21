// SPDX-License-Identifier: Apache-2.0
//
// Minimal memcached load generator for the recall harness. Not a benchmark —
// it never reports throughput. Its only job is to drive enough concurrent
// traffic that whatever coherence cost the server has becomes visible to c2c.
//
// Pipelines a batch per write so the server stays busy rather than
// round-trip-bound, and mixes set/get because they touch different server
// state.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int port, nthreads, seconds, depth, keyspace;
static int cpus[64], ncpu;
static volatile int stop_now;

static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (pthread_setaffinity_np(pthread_self(), sizeof(s), &s)) {
        fprintf(stderr, "FATAL: pin %d\n", cpu); _exit(2);
    }
}

static void *worker(void *arg) {
    long id = (long)arg;
    pin(cpus[id % ncpu]);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { perror("connect"); _exit(2); }
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    char val[1024]; memset(val, 'x', sizeof val);
    char buf[64 * 1024], rsp[64 * 1024];
    unsigned long n = 0;

    while (!stop_now) {
        int off = 0;
        for (int i = 0; i < depth && off < (int)sizeof(buf) - 1400; i++, n++) {
            // Keys spread across threads so slab and hash traffic is shared,
            // which is where server-side contention would live.
            if (n % 4)
                off += snprintf(buf + off, sizeof buf - off,
                                "get k%lu\r\n", (n * 7919) % keyspace);
            else
                off += snprintf(buf + off, sizeof buf - off,
                                "set k%lu 0 0 512\r\n%.512s\r\n",
                                (n * 7919) % keyspace, val);
        }
        if (write(fd, buf, off) != off) break;
        if (read(fd, rsp, sizeof rsp) <= 0) break;
    }
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s <port> <threads> <seconds> <cpulist> <keyspace>\n"
                        "  keyspace=1 forces every client onto one item, which is\n"
                        "  the adversarial case for refcount and LRU contention\n", argv[0]);
        return 2;
    }
    port = atoi(argv[1]); nthreads = atoi(argv[2]); seconds = atoi(argv[3]);
    keyspace = atoi(argv[5]);
    if (keyspace < 1) keyspace = 1;
    for (char *t = strtok(argv[4], ","); t && ncpu < 64; t = strtok(NULL, ","))
        cpus[ncpu++] = atoi(t);
    if (!ncpu) { fprintf(stderr, "no cpus\n"); return 2; }
    depth = 32;

    pthread_t th[64];
    for (long i = 0; i < nthreads; i++)
        if (pthread_create(&th[i], NULL, worker, (void *)i)) return 2;
    sleep(seconds);
    stop_now = 1;
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
    return 0;
}
