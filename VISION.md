# Faultline — Vision and Scope

## 1. Mission

Faultline is a static analysis tool for detecting structural latency hazards in ultra-low-latency C++ systems.

Its purpose is not general performance optimization.

Its purpose is to identify code patterns that create tail-latency amplification, nondeterminism, cache instability, and microarchitectural contention in systems where microseconds matter.

**Target domain:**

- High-frequency trading systems
- Exchange infrastructure
- Market data handlers
- Matching engines
- Kernel-bypass networking stacks
- Deterministic event-driven runtimes

Faultline exists to answer one question:

&gt; Where are the structural faultlines that will break under load?

---

## 2. Definition: Latency Landmine

A **latency landmine** is a code-level construct that:

- Is structurally capable of causing high-percentile latency spikes (p99.9+), and
- May not be visible in average throughput benchmarks, and
- Is often introduced unintentionally.

**Examples include:**

- False sharing across cache lines
- Hidden heap allocations in hot paths
- Contended atomic operations
- Virtual dispatch in tight loops
- Poor struct layout spanning multiple cache lines
- Memory ordering misuse
- Lock convoy risks
- Excessive branch entropy in critical sections
- Cross-NUMA memory access patterns

Faultline focuses on structural causes, not incidental slow code.

---

## 3. Design Philosophy

### 3.1 Tail Latency Over Averages

Average latency is not a meaningful metric in ultra-low-latency systems.

Faultline prioritizes detection of patterns that can amplify tail latency.

### 3.2 Determinism Over Abstraction

Abstraction layers that obscure memory behavior are treated as risk multipliers.

Faultline favors transparency of:

- Memory layout
- Allocation behavior
- Synchronization semantics
- Cache-line ownership

### 3.3 Hardware-Aware Static Analysis

Faultline assumes:

- x86-64 architecture
- TSO (Total Store Order) memory model
- 64-byte cache lines
- MESI coherence protocol
- Multi-core, potentially multi-socket systems

The tool does not attempt to be architecture-agnostic.

Precision &gt; portability.

### 3.4 No Throughput Theater

Faultline does not attempt to:

- Estimate cycles
- Predict exact latency
- Replace runtime profiling
- Provide synthetic performance scores

It identifies structural risk patterns.

Runtime validation is separate.

---

## 4. Non-Goals

Faultline does not aim to:

- Optimize general application performance
- Suggest algorithmic complexity improvements
- Replace profilers (perf, VTune, etc.)
- Analyze GPU code
- Model speculative execution vulnerabilities
- Guarantee absence of latency spikes

Faultline is a structural risk detector, not a performance oracle.

---

## 5. Target Language and Toolchain

**Initial target:**

- C++20
- Clang/LLVM toolchain
- Linux-based environments

**Assumptions:**

- Codebases compiled with `-O2` or `-O3`
- Modern low-latency coding conventions
- Minimal reliance on heavy RTTI or exceptions in hot paths

---

## 6. Severity Philosophy

Each detected issue is classified as:

| Severity | Description |
|----------|-------------|
| **Critical** | High probability of tail-latency amplification under multi-core contention |
| **High** | Strong structural risk, dependent on workload |
| **Medium** | Context-dependent risk |
| **Informational** | Structural awareness warning |

Faultline errs on the side of precision over volume.

False positives erode trust. Trust is non-negotiable.

---

## 7. Success Criteria

**Faultline is successful if:**

- It identifies real structural latency hazards in production-grade systems.
- Its findings correlate with measurable microarchitectural degradation.
- Senior low-latency engineers agree with its diagnostics.
- It becomes a tool engineers run before merging hot-path changes.

**Faultline is not successful if:**

- It produces noise.
- It becomes a stylistic linter.
- It flags trivial patterns without hardware relevance.

---

## 8. Core Principle

Latency collapses do not come from obvious code.

They emerge from invisible structural stress.

Faultline exists to expose those stress points before the market does.