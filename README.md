# Faultline

Compile-time structural latency landmine detector for C++ systems.

Built on Clang/LLVM. Targets **x86-64 / 64B cache lines / TSO**.

Faultline identifies source-level patterns that cause microarchitectural degradation — cache line contention, store buffer serialization, coherence storms, TLB pressure, branch misprediction — before the code reaches production.

It does not guess. Every finding maps to a specific hardware mechanism on x86-64.

---

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| **Linux x86-64** | Supported | Primary target. Fully verified. |
| **WSL2** | Supported | Linux under the hood. |
| **macOS (Intel)** | Unsupported | AST/IR analysis may build, but hardware model assumes x86-64 TSO. No `perf` support. |
| **macOS (Apple Silicon)** | Unsupported | ARM uses 128B cache lines and a weak memory model. All hardware reasoning is wrong. |
| **Windows (native)** | Unsupported | Build system and IR emission assume Unix toolchain. |

Faultline's hardware model — 64B cache lines, TSO store buffer semantics, MESI coherence costs, x86-64 PMU counters — is x86-64 Linux specific. Running it on other platforms would produce diagnostics with incorrect hardware reasoning.

## Requirements

| Dependency | Version |
|------------|---------|
| OS | Linux x86-64 (or WSL2) |
| LLVM/Clang | 16+ (dev libraries) |
| CMake | 3.20+ |
| C++ Standard | C++20 |

```bash
# Ubuntu/Debian
apt install llvm-18-dev libclang-18-dev clang-18 cmake

# Arch
pacman -S llvm clang cmake
```

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

If cmake cannot find LLVM, specify the prefix path for your install:

```bash
# Ubuntu/Debian (llvm-18-dev)
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/lib/llvm-18

# Arch / Fedora (system LLVM)
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/lib/cmake/llvm

# Custom install
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/llvm
```

## Usage

```bash
./faultline /path/to/source.cpp -- -std=c++20
./faultline --config=../faultline.config.yaml /path/to/source.cpp --
./faultline --json /path/to/source.cpp --
./faultline --min-severity=High /path/to/source.cpp --
./faultline --no-ir /path/to/source.cpp --          # AST-only, skip IR pass
./faultline --ir-opt=O1 /path/to/source.cpp --      # IR at -O1 (optimizer effects)
```

With a compilation database:

```bash
cd /project/build && /path/to/faultline /project/src/hot_path.cpp
```

---

## What It Detects

### Landmine: False Sharing

Two atomics on the same cache line. Two threads writing to different fields. MESI invalidation ping-pong on every write.

```cpp
struct SequenceCounters {
    std::atomic<uint64_t> inboundSeq;   // Thread A writes
    std::atomic<uint64_t> outboundSeq;  // Thread B writes
};
// sizeof = 16B. Both fields share one 64B cache line.
// Every write by Thread A invalidates Thread B's L1 copy. And vice versa.
// Cost: 40-100ns per write (intra-socket), 120-300ns (cross-socket).
```

Faultline output:

```
[CRITICAL] FL002 — False Sharing Candidate
  Location: order_engine.cpp:32 (SequenceCounters)
  Hardware: MESI invalidation ping-pong across cores due to shared cache line
            writes. Each write by one core forces invalidation of the line in
            all other cores' L1/L2, triggering RFO traffic.
  Evidence: sizeof=16B; mutable_fields=2; atomics=yes; thread_escape=true
  Mitigation: Separate fields with alignas(64) padding.
  Confidence: 0.87
```

Fix:

```cpp
struct SequenceCounters {
    alignas(64) std::atomic<uint64_t> inboundSeq;
    alignas(64) std::atomic<uint64_t> outboundSeq;
};
// Each field on its own cache line. Zero cross-core invalidation.
```

### Landmine: Unnecessary Store Buffer Drain

`memory_order_seq_cst` is the default for `std::atomic`. On x86-64 TSO, `seq_cst` stores emit `XCHG` or `MOV` + `MFENCE`, draining the store buffer and serializing the pipeline. `acquire`/`release` compiles to plain `MOV` — zero overhead on TSO.

```cpp
[[clang::annotate("faultline_hot")]]
void updateSequence(std::atomic<uint64_t>& seq) {
    uint64_t s = seq.load();           // seq_cst load: free on x86 (plain MOV)
    seq.store(s + 1);                  // seq_cst store: XCHG → store buffer drain
}
// In a 10M iteration loop: 200M-400M cycles of pure serialization overhead.
```

Faultline output:

```
[HIGH] FL010 — Overly Strong Atomic Ordering
  Location: engine.cpp:4 (updateSequence)
  Hardware: Full memory fence emitted for seq_cst store causes pipeline
            serialization and store buffer drain. On x86-64 TSO, release
            ordering provides equivalent visibility guarantees for stores
            with zero fence overhead.
  Evidence: ordering=seq_cst; operation=store; in_hot_path=true
  Mitigation: Use memory_order_release for stores, memory_order_acquire for loads.
  Confidence: 0.91
```

### Landmine: Heap Allocation in Hot Path

`new`/`malloc` in a tight loop: allocator lock contention, TLB misses on new pages, potential kernel transition via `mmap`.

```cpp
[[clang::annotate("faultline_hot")]]
void processBatch(const uint64_t* ids, int count) {
    for (int i = 0; i < count; ++i) {
        auto* tmp = new uint64_t(ids[i]);  // malloc per iteration
        process(*tmp);
        delete tmp;
    }
}
```

```
[CRITICAL] FL020 — Heap Allocation in Hot Path
  Location: engine.cpp:3 (processBatch)
  Hardware: Allocator lock contention. TLB pressure from new page mappings.
            Potential minor page fault (1-10µs kernel transition).
  Evidence: allocation_site=new; in_loop=true; in_hot_path=true
  Escalation: [allocation_in_loop]
  Mitigation: Preallocate. Use arena/slab allocator. Object pool.
  Confidence: 0.93
```

### Landmine: Compound Hazard

Multiple hazards on the same structure produce super-additive tail latency. A 192-byte struct with atomics, thread escape, and loop access combines cache spanning + coherence traffic + contention into a single amplified cost.

```cpp
struct MarketDataLevel {
    std::atomic<uint64_t> bidPrice;
    std::atomic<uint64_t> askPrice;
    std::atomic<uint64_t> bidQty;
    std::atomic<uint64_t> askQty;
    uint64_t bidOrders[10];
    uint64_t askOrders[10];
    uint64_t timestamps[4];
};
// sizeof = 192B = 3 cache lines.
// Atomics span multiple lines → 3 separate RFO transactions per write.
// Cross-socket: 3 × 200ns = 600ns per atomic update.
```

```
[CRITICAL] FL090 — Hazard Amplification
  Location: market_data.cpp:1 (MarketDataLevel)
  Hardware: Multiple interacting latency multipliers: cache line spanning (3 lines)
            + atomic contention + thread escape. Compound coherence cost is
            super-additive.
  Evidence: sizeof=192B; cache_lines=3; atomics=4; thread_escape=true
  Confidence: 0.89
```

---

## Rules

| ID | Hazard | Severity | x86-64 Mechanism |
|----|--------|----------|-----------------|
| FL001 | Cache Line Spanning Struct | High/Critical | L1D pressure, multi-line coherence surface |
| FL002 | False Sharing Candidate | Critical | MESI RFO ping-pong on shared cache line |
| FL010 | Overly Strong Atomic Ordering | High/Critical | `MFENCE`/`XCHG` store buffer drain (TSO-specific) |
| FL011 | Atomic Contention Hotspot | Critical | Cache line ownership thrashing, RFO storms |
| FL012 | Lock in Hot Path | Critical | Futex syscall, context switch, cache cold restart |
| FL020 | Heap Allocation in Hot Path | Critical | Allocator contention, dTLB pressure, page faults |
| FL021 | Large Stack Frame | Medium/Critical | dTLB pressure, L1D capacity exhaustion |
| FL030 | Virtual Dispatch in Hot Path | High/Critical | BTB misprediction, pipeline flush (14-20 cycles) |
| FL031 | std::function in Hot Path | High/Critical | Type-erased indirect call, potential heap alloc |
| FL040 | Centralized Mutable Global State | High/Critical | NUMA remote access, cache line contention |
| FL041 | Contended Queue Pattern | High/Critical | Head/tail cache line bouncing |
| FL050 | Deep Conditional Tree | Medium/High | Branch misprediction chains |
| FL060 | NUMA-Unfriendly Shared Structure | High/Critical | Remote DRAM penalty (2-5x local latency) |
| FL061 | Centralized Dispatcher Bottleneck | High/Critical | I-cache pressure, BTB contention from fan-out |
| FL090 | Hazard Amplification | Critical | Super-additive compound risk |

Every rule maps to a specific hardware subsystem: cache, coherence protocol, store buffer, TLB, branch predictor, NUMA interconnect, or allocator. If a pattern cannot be tied to a hardware mechanism, it is not a rule.

---

## Analysis Layers

### Layer 1: AST (Structural Semantics)

Clang AST traversal via `RecursiveASTVisitor`. Detects structural patterns: struct layout, atomic operations, lock acquisitions, allocation sites, dispatch patterns. Uses `ASTRecordLayout` for precise `sizeof` computation. Escape analysis infers thread visibility from `std::atomic`, `std::mutex`, `std::shared_ptr` members and global mutability.

### Layer 2: IR (Lowered Behavior)

LLVM IR emission via `clang -emit-llvm -S`. Parses with `llvm::parseIRFile()`. Validates AST findings against lowered code:

- Confirms `seq_cst` fence emission (not all `seq_cst` ops emit fences on x86)
- Confirms heap allocations survive inlining
- Confirms indirect calls survive devirtualization
- Precise stack frame sizing via `AllocaInst`

Default: `-O0` (structural confirmation). Use `--ir-opt=O1` to see optimizer effects.

### Layer 3: Hypothesis Engine

Converts findings into falsifiable statistical hypotheses. Generates self-contained experiment bundles (treatment + control microbenchmarks) with PMU counter collection scripts. Feeds results back into calibration.

---

## Hot Path Annotation

```cpp
[[clang::annotate("faultline_hot")]]
void onMarketData(const MDUpdate& update) { ... }
```

Or via config:

```yaml
hot_function_patterns:
  - "*::onMarketData*"
  - "*::process*"
hot_file_patterns:
  - "*/hot_path/*"
```

Non-hot code is analyzed with reduced severity.

---

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | No hazards at or above minimum severity |
| 1 | Hazards detected |
| 2 | Tool error |