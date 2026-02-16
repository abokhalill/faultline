# Faultline — Validation and Benchmarking Strategy

## 1. Core Validation Principle

Faultline does not claim to predict exact latency.

It claims:

&gt; Structural patterns flagged by Faultline correlate with measurable degradation in microarchitectural behavior and/or tail latency under load.

Validation must prove correlation between:

**Static Finding → Hardware Counter Degradation → Tail Latency Amplification**

If we cannot demonstrate this chain, the rule is weakened or removed.

---

## 2. Validation Layers

Faultline validation operates at three layers:

1. **Microbenchmark Validation**
2. **Synthetic System Validation**
3. **Real-World Codebase Validation**

All three are required.

---

## 3. Measurement Environment Standardization

To reduce noise:

- CPU frequency scaling disabled
- Turbo disabled
- C-states minimized
- CPU pinning enforced
- Isolated cores used
- Transparent huge pages disabled (unless testing TLB)
- NUMA topology documented
- Compiler flags fixed (`-O3`, LTO optional)

**Each experiment records:**

- CPU model
- Core count
- Cache sizes
- Kernel version
- Compiler version

Reproducibility is mandatory.

---

## 4. Instrumentation Strategy

**Primary tools:**

- `perf` (hardware performance counters)
- `rdtsc`-based cycle timing
- `perf_event_open` for programmatic capture
- Flamegraphs
- Histogram generation for latency distribution

**Measured counters include:**

- `cycles`
- `instructions`
- `cache-misses`
- `L1-dcache-load-misses`
- `LLC-load-misses`
- `branch-misses`
- `stalled-cycles-frontend`
- `stalled-cycles-backend`
- `dtlb-load-misses`

**Latency reporting includes:**

- median
- p99
- p99.9
- p99.99
- max

Average alone is insufficient.

---

## 5. Microbenchmark Validation

Each rule must have at least one minimal reproducible benchmark.

**Example: FL002 (False Sharing Candidate)**

**Construct:**

- **Case A:** Shared struct with adjacent atomics
- **Case B:** Padded 64B separated fields

**Measure:**

- Throughput under multi-core load
- Cache misses
- LLC invalidations
- Tail latency of update operation

**Expected:**

Case A exhibits higher invalidation traffic and elevated p99+ latency.

If not, rule requires refinement.

---

## 6. Synthetic System Validation

Build controlled subsystems:

- Lock-free queue
- Order book
- Market data parser
- Shared counter stress test
- Centralized dispatcher

**Inject faults intentionally:**

- Add heap allocation in hot loop
- Add virtual dispatch
- Add `seq_cst` atomics
- Add centralized global mutable state

**Measure:**

- Latency histogram shift
- Hardware counter deltas
- Variance increase

Confirm that Faultline flags the injected hazard.

---

## 7. Real-World Codebase Validation

**Select:**

- Open-source trading engines
- Exchange simulators
- High-performance networking libraries
- Lock-free queue implementations

**Procedure:**

1. Run Faultline.
2. Identify flagged hazards.
3. Construct targeted microbenchmarks isolating flagged region.
4. Measure performance before/after mitigation.
5. Document findings in `CASE_STUDIES.md`.

**If Faultline produces no meaningful findings on serious codebases, credibility drops.**

**If it finds real issues, signal skyrockets.**

---

## 8. False Positive Auditing

Every rule must track:

- False positive rate
- Confidence score calibration

**Procedure:**

Engineers manually review flagged cases.

**Categorize as:**

- Confirmed risk
- Context-dependent
- False positive

**Target:**

False positives under 15% for High/Critical severity.

Noise kills adoption.

---

## 9. Correlation Requirement

For each rule, we attempt to demonstrate:

**Static Pattern → Hardware Counter Degradation (X%) → Tail Latency Increase (Y%)**

Even small increases in hardware counters may create nonlinear tail effects under contention.

Documentation must reflect this nuance.

---

## 10. NUMA Validation

On dual-socket hardware:

- Construct centralized shared state
- Construct per-socket partitioned state

**Measure:**

- Remote memory accesses
- LLC miss rate
- Tail latency under cross-socket traffic

Confirm Faultline flags centralized shared mutable structures.

---

## 11. Store Buffer Stress Validation

**Construct:**

- Tight atomic store loop
- Batched store version
- Relaxed ordering version

**Measure:**

- `stalled-cycles-backend`
- Pipeline serialization events
- Latency jitter

Validate FL010 and FL011.

---

## 12. Regression Testing

Faultline includes:

- Corpus of known hazardous patterns
- Corpus of safe optimized patterns

**Every rule change must:**

- Preserve detection of known hazards
- Avoid introducing excessive false positives

---

## 13. Quantitative Credibility Standard

A rule remains in v1 only if:

- It has at least one validated experiment
- Hardware mechanism explanation holds
- Observed degradation is measurable (not anecdotal)

If evidence is weak, rule is downgraded or removed.

---

## 14. Publication Strategy

Validation results are documented as:

- Reproducible experiments
- Performance graphs
- Counter deltas
- Before/after diffs
- Clear hardware explanation

This documentation becomes external-facing material.

Credibility is built on data, not claims.

---

## 15. Success Metric

Faultline validation succeeds when:

1. A senior low-latency engineer reviews experiments and agrees the rule maps to real hardware behavior.
2. A flagged issue, once fixed, produces measurable tail-latency improvement.

If both occur, Faultline graduates from idea to instrument.