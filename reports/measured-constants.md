# Measured constants

Every hardware number lshaz relies on, with the instrument that produced it.
A constant with no row here is an assertion, not a measurement, and should be
treated as unvalidated regardless of how reasonable it sounds.

Reproduce with `bench/` — see `bench/README.md`.

## Reference machine

| | |
|---|---|
| CPU | Intel Core i7-8700, Coffee Lake, 6C/12T, 1 socket |
| Cache | L1d 32 KiB/core, L2 256 KiB/core, L3 12 MiB shared, 1 instance |
| Topology | 1 NUMA node, monolithic ring |
| Kernel | 6.8.0-134-generic, Ubuntu 24.04 |
| State | turbo off (3.2 GHz base), performance governor, `isolcpus=1,2,3,7,8,9`, THP off, ASLR off, IRQs pinned off isolated cores |
| Date | 2026-08-08 |

Single-socket, single-L3. Cross-socket and multi-CCD coherence are more
expensive; these figures are a **lower bound** for those topologies.

## Contended atomic RMW vs inter-write spacing

`__atomic_add_fetch(..., __ATOMIC_RELAXED)` — `lock add` on x86-64. Two
threads, adjacent 8-byte counters on one line, versus one private line per
thread. The delta is coherence; identical spin work in both arms cancels.

| spacing | private | shared | Δ |
|---:|---:|---:|---:|
| 8 ns | 7.81 ns | 29.60 ns | **+21.79** |
| 125 ns | 125.30 | 125.59 | +0.29 |
| 625 ns | 625.58 | 625.77 | +0.19 |
| 1.25 µs | 1250.82 | 1250.95 | +0.13 |
| 2.5 µs | 2501.38 | 2501.61 | +0.16 (n=7, 0.03–0.26) |
| 20 µs | 20009.58 | 20008.26 | ~0 |

**Cost collapses ~75× between 8 ns and 125 ns and is unmeasurable past ~1 µs.**

Uncontended `lock add`: 4.2 ns (~13 cycles). At 3 threads the peak doubles
(+43.45 ns at 8 ns spacing) but the collapse point does not move.

Consumed by: FL002's density conjunct.

### Why the threshold is this tight

False sharing requires co-residency *in time*, not only in address space. Two
threads writing one line 20 µs apart never find it in each other's L1 — the
working set turns over many times in between — so the write is a clean L3 hit
rather than a snoop of a modified peer line.

## Coherence traffic in redis under io-threads

Baseline `b48e4abe`, `io-threads 3` (main + 2 io), redis on isolated cores
1–3, `redis-benchmark` on 4,5,10,11, ~530K ops/s SET/GET 1 KiB p10.

| | |
|---|---|
| Writes to `stat_net_input_bytes` | 244,672 / 5 s |
| Writes to `stat_net_output_bytes` | 244,647 / 5 s |
| Combined on that one line | **97,864 /s** (~10.2 µs global spacing) |
| Writers | both `io_thd_1` and `io_thd_2`, both addresses |
| **HITM on that line** | **0** |
| Shared lines with HITM | 260 kernel, 1 userspace (TLS) — none in the redis binary |

Textbook false sharing by construction, producing no coherence traffic. The
spacing is ~200× beyond where cost exists.

Total coherence budget for the whole process: 312,088 HITM/s against
3.174 G cycles/s ≈ **1.2% of cycles at HITM latency**, ceiling, essentially
all of it in the kernel network path (`ep_poll_callback`,
`skb_page_frag_refill`/`tcp_sendmsg`, `net_rx_action`).

Consumed by: the withdrawal analysis for redis/redis#15445.

## Coherence traffic in memcached — and a correction to our ground truth

`memcached -t 2` on isolated cores 1–3. The two symbols below were previously
adjudicated by the wattr write-attribution runtime: `stats_state` a true
positive, `settings` a false positive.

| symbol | writes / 8 s | rate |
|---|---:|---:|
| `stats_state` | 1,884,538 | **235,567 /s** |
| `settings` | 0 | **0** |

The adjudication holds on the sharing question: the "false positive" is never
written at all under load. But:

| | |
|---|---|
| HITM total (whole process) | 445 |
| HITM on the `stats_state` line | **0** |
| Shared lines belonging to the memcached binary | **0** |

235K writes/s across two threads is ~4.2 µs spacing — beyond the collapse
point — so even the *confirmed* finding costs nothing measurable.

**Generalization.** Two unrelated servers, two independently confirmed
false-sharing sites, zero HITM in either. Application-level statistics
counters do not reach the write density where false sharing costs anything;
all measurable coherence traffic in both processes is in the kernel network
path. Absent a profile showing sub-microsecond spacing, a counter-shaped
FL002 finding is not a merge-worthy PR however real its geometry.

This distinguishes two things wattr conflated: **sharing is real and
measurable; cost is a separate question with a much higher bar.**

## The dense end: SPSC ring indices (FL041's shape)

Producer owns `head`, consumer owns `tail`, both updated once per element.
Producer on cpu1, consumer on cpu2, distinct physical cores. `bench/ring_cost.c`.

| run | one line | line each | Δ |
|---|---:|---:|---:|
| 1 | 7.199 ns | 4.289 ns | +68% |
| 2 | 6.675 | 3.762 | +77% |
| 3 | 9.129 | 3.889 | +135% |

Mean ~7.7 ns vs ~4.0 ns: **padding roughly halves cost per element.**

This is the same geometry as an FL002 counter pair — two adjacent atomics on
one line, two threads — and it costs 3–5 ns per element here versus nothing at
all in redis and memcached. The only variable is spacing: ~4 ns between writes
instead of 4–20 µs.

**FL002 and FL041 therefore discriminate for a mechanical reason, not by
convention.** It also justifies FL041's exemption from FL002's
deliberate-layout demotion: in a ring buffer, line-aligned indices are the bug
rather than evidence the author reasoned about layout.


## FL010 — memory ordering cost on x86-64

Single-threaded, uncontended, pinned to an isolated core, min of 3.
`bench/order_cost.c`. Contention would swamp the difference under test.

| operation | relaxed | mid | seq_cst |
|---|---:|---:|---:|
| store | 0.312 ns | 0.312 (release) | **5.630** |
| load | 0.313 | 0.312 (acquire) | 0.313 |
| rmw (`fetch_add`) | 5.630 | 5.630 (acq_rel) | 5.630 |

Emitted instructions confirm the mechanism: relaxed store is `mov`, seq_cst
store is `mov`+`XCHG`, every RMW carries `lock` at every ordering.

**Only the seq_cst store is weakenable: +5.32 ns, ~18x.** Loads cost nothing
to strengthen and RMWs are LOCK-prefixed regardless, so neither can be
compiled into different machine code by relaxing it.

This confirms the expectation hardcoded in `verify/run.sh` (`fl010_c_atomics`)
and the reasoning behind the arm64 variant, where LDAR and LDAXR/STLXR *are*
distinguishable and the same fixture legitimately reports more findings.

Consumed by: FL010 severity model.


## FL020/FL021 — allocator serialization does not fire (same-thread free)

k threads, each malloc/touch/free at a fixed size, pinned to isolated cores.
`bench/alloc_cost.c`. glibc 2.39.

| size | 1 thread | 2 threads | 3 threads |
|---|---:|---:|---:|
| 64 B | 14.72 ns | 14.49 | 14.66 |
| 4096 B | 55.39 | 55.35 | 55.34 |

**Flat.** Per-thread arenas plus tcache mean same-thread alloc/free never
reaches shared allocator state, so the serialization FL020/FL021 assert does
not occur at all in this configuration.

### Cross-thread free — the conjunct, and it is large

Producer allocates, consumer frees, SPSC ring between them so queue mechanics
are identical in both arms. Producer cpu1, consumer cpu2. `bench/xfree_cost.c`.

| size | same-thread free | cross-thread free | ratio |
|---|---:|---:|---:|
| 64 B | 31.97 ns | 158.45 ns | **5.0x** |
| 512 B | 34.36 | 848.96 | **24.7x** |
| 4096 B | 96.10 | 1081.22 | **11.3x** |

Freeing a block returns it to the arena that owns it. When that arena belongs
to another thread the free touches shared state, and glibc's tcache cannot
absorb it. **FL020/FL021's mechanism is real; the missing precondition is
cross-thread ownership transfer, not allocation volume.** A rule that fires on
allocation sites alone flags mostly-harmless code and misses this entirely.

Largest effect measured in this campaign by an order of magnitude.

### Under jemalloc — blunted, not removed

redis links jemalloc, which has remote-free queues for exactly this pattern.
Same harness under `LD_PRELOAD`:

| size | glibc same/cross | jemalloc same/cross | glibc ratio | jemalloc ratio |
|---|---:|---:|---:|---:|
| 64 B | 34.16 / 151.45 | 24.72 / 100.59 | 4.4x | **4.1x** |
| 512 B | 33.74 / 819.04 | 33.00 / 161.77 | 24.3x | **4.9x** |

jemalloc removes most of the size-dependent blowup (24.3x -> 4.9x at 512 B)
but the cross-thread penalty stays at **~4-5x** at both sizes. The mechanism
survives the allocator that was most likely to eliminate it.

**Superseded — refuted, see below.** ~~Lead worth pursuing:~~ redis's io-threads model allocates client buffers on
one thread and may free them on another, and the cycle profile puts ~5.9% in
`zmalloc`/`sdsfree`/`decrRefCount`. If that is a cross-thread free pattern
under jemalloc it is the most promising redis finding surfaced so far. It is a
hypothesis, not a result — the last three locality hypotheses in this campaign
were all refuted by measurement.

**Superseded note:** cross-thread free. Allocating on thread A
and freeing on thread B returns the block to A's arena and *does* touch shared
state. That is the shape a producer/consumer pipeline actually has, and it is
likely the missing conjunct — the FL002 lesson in a different mechanism. Do
not weaken FL020/FL021 on the table above alone; it only rules out the
easy case.

Consumed by: nothing yet. Recorded so the rule is not tightened on a
half-measurement.

## What this machine cannot answer

- **FL030/FL031 (NUMA)** — single NUMA node. Cross-node latency and page
  placement are not measurable here at all; a two-socket host is required.
- **FL070/FL090/FL091** — composites over other rules, no single mechanism to
  isolate.

Still testable, still unmeasured: FL003 (striped geometry), FL011-FL013,
FL020/FL021 (allocator serialization — the redis profile puts ~5.9% of cycles
in `zmalloc`/`sdsfree`/`decrRefCount`, making this the highest-prior target),
FL050 (BTB), FL060/FL061 (virtual dispatch).

## Instrument validation

Before any null result was believed, `perf c2c` was pointed at a
known-positive (`rmw_cost` in `shared` mode):

- 27,620 of 28,321 loads Local HITM (**97.5%**)
- 99.99% of HITM attributed to one line, split 49.33%/50.67% across byte
  offsets **0x0** and **0x8** — the two counters

A null on redis therefore means redis is clean, not that the tool was
misconfigured.

**`perf c2c`'s `cycles` column is not a cost.** It read 4209/4750 cycles where
wall-clock showed ~198 cycles/op; PEBS `ldlat=30` samples only loads above a
latency threshold, so the figure is tail-biased. c2c answers *where* and *how
much*; `rmw_cost` answers *how expensive*. Never cross-cite them.

## Not yet measured

Assertions currently shipping without a row above:

- FL010 — "on x86 only the seq_cst *store* costs anything; loads are MOV and
  RMWs are LOCK-prefixed at every ordering." Encoded as a hardcoded
  expectation in `verify/run.sh`.
- FL003 — striped-array slot geometry vs contention.
- FL020/FL021 — allocator serialization.
- FL030/FL031 — TLB and NUMA page-walk cost.
- FL041 — ring-buffer head/tail, the dense end of the density curve.
- FL050 — BTB pressure.
- FL060/FL061 — virtual dispatch indirection.

## FL020/FL021 in redis — the conjunct, confirmed by a third party

The lead recorded above ("redis may free client buffers cross-thread") is
**refuted by code read**, and refuted in the most useful possible way: redis
already implements the mitigation, and names the mechanism.

```c
/* Attempt to defer freeing the object to the IO thread. We usually call this
 * since we know the object is allocated in the IO thread, to avoid memory
 * arena contention, ... */
void tryDeferFreeClientObject(client *c, int type, void *ptr)
```

Three layers, all in-tree at `3c0984a0`:

| mechanism | direction | site |
|---|---|---|
| `thread_reusable_qb` (`__thread sds`) | no allocation at all | networking.c:4042 |
| `tryDeferFreeClientObject` | main -> io thread | networking.c:1813 |
| `ioDeferFreeRobj` / `freeClientIODeferredObjects` | io -> main thread | networking.c:1848 |

The common-case `querybuf` is a per-io-thread buffer allocated once and
`sdsclear`'d per command, so the hot path allocates nothing. Residual
cross-thread frees are all low-rate: `clientsCronResizeQueryBuffer` (10 Hz,
sampled clients), `freeClient` (connection-close rate), and the
`CLIENT_MAX_DEFERRED_OBJECTS 32` overflow fallback.

**No PR here.** The ~5.9% of cycles in `zmalloc`/`sdsfree`/`decrRefCount` is
same-thread traffic that tcache and jemalloc's per-thread caches absorb —
exactly the flat case measured above.

**What this is worth instead.** A production system independently hit
cross-thread free, diagnosed it as arena contention, and built directional
deferral to fix it. That is external attestation of the conjunct measured
here, from a source that is not us, and it supplies FL020/FL021's canonical
mitigation: *defer the free to the allocating thread.*

Consumed by: FL020/FL021 cross-thread-free conjunct (pending) and its
`explain` text.

## FL003 — striped-array geometry

3 threads, thread i owns slot i, plain non-atomic `*slot = *slot + 1` in a
tight loop, isolated cores 1-3. `bench/stripe_cost.c`. Stride is the only
variable.

| stride | ns/op | slots per line |
|---:|---:|---|
| 8 B | 2.470 | 3 threads share one line |
| 16 B | 2.451 | 3 share |
| 32 B | 2.432 | 2 share + 1 |
| 64 B | **1.732** | one line each |
| 128 B | 1.732 | one line each |

**+43% for the shared arm.** Real, and far milder than the atomic case
(+280% at 8 ns spacing in the RMW table). Two reasons: a plain store retires
into the store buffer without waiting for line ownership, and nothing
synchronizes the threads, so each core wins the line and keeps it for many
iterations. **Line transfers happen far below the write rate.**

This refines the density law: the quantity that costs is not write frequency
but *ownership-transfer* frequency, and unsynchronized writers batch. It also
justifies FL003 carrying lower severity than FL002-with-atomics at identical
geometry.

Padding beyond 64 B buys nothing, confirming the single-line model.

## FL050 — BTB: capacity is not the mechanism, predictability is

One indirect call site, N distinct targets, driven two ways: a predictable
cycle, and a precomputed shuffled sequence. `bench/btb_cost.c`, 20M calls,
min of 3.

| targets | predictable | random | Δ |
|---:|---:|---:|---:|
| 1 | 1.562 | 1.574 | 0.012 |
| 2 | 1.719 | 5.968 | 4.249 |
| 8 | 1.699 | 9.077 | 7.377 |
| 64 | 1.735 | 10.004 | 8.269 |
| 512 | 1.748 | 10.022 | 8.274 |
| 4096 | 1.725 | 10.212 | 8.487 |

**Predictable dispatch is flat from 1 to 4096 targets.** No capacity cliff
exists to find. The cost is misprediction: ~8.3 ns (~26 cycles at 3.2 GHz),
saturating by 64 targets. Fanout=1 gives random == predictable, so the
shuffled arm's index load adds nothing.

**FL050 currently grades on indirect-branch count. That is the wrong
variable** — the same error as grading false sharing on geometry alone. The
rule needs evidence the target *sequence* is data-dependent; a dispatch table
walked in a fixed order is free no matter how large.

## FL060/FL061 — virtual dispatch decomposes into two unequal costs

Same site, receiver type cycled vs shuffled over a 64K ring (past what the
predictor memorizes). `bench/vdispatch_cost.cpp`, 30M calls.

| | ns/call | vs direct |
|---|---:|---:|
| direct, inlinable | 0.625 | — |
| virtual, monomorphic | 1.615 | **+0.99** |
| virtual, 2 types shuffled | 6.442 | +5.82 |
| virtual, 8 types shuffled | 9.763 | **+9.14** |

| types | cyclic | shuffled | Δ |
|---:|---:|---:|---:|
| 1 | 1.615 | 1.910 | 0.295 |
| 2 | 1.614 | 6.442 | 4.827 |
| 4 | 1.758 | 8.633 | 6.876 |
| 8 | 1.757 | 9.763 | 8.006 |

Two separable costs with different fixes:

- **Lost inline, ~1 ns.** Paid by every virtual call. Fix is devirtualization
  (`final`, CRTP, visitor).
- **Misprediction, up to ~8 ns.** Paid only when receiver type is
  data-dependent. Fix is type-partitioned batching, not devirtualization.

**A single severity for "virtual call in hot path" is wrong by up to 9x.**
FL060/FL061 should report the ~1 ns floor unless there is evidence of
polymorphic, data-dependent receivers, and escalate only then.

### Methodology note: two instrument bugs, same shape

First pass of both harnesses read 9.2 and 8.5 ns *flat across every arm*. The
index was `i % fanout` with a runtime divisor — a ~25-cycle IDIV on the hot
path, 3x the quantity under test. It would have been reported as "BTB pressure
does not exist." Fixed with a power-of-two mask.

Second instance this campaign of a null produced by the instrument rather than
the system, after `perf script -F cpu` silently emitted zero rows. **A flat
result is a claim about the harness until the harness is shown to be able to
move.** Every table above now includes an arm that does move.
