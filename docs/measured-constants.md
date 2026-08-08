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
