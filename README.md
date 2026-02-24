# Faultline

Structural latency hazard analyzer for C++ on **Linux x86-64**.

Faultline is built on Clang/LLVM and focused on **TSO, 64B cache lines, coherence traffic, and branch/allocator side effects** in low-latency systems.

It is not a style linter. It is a static structural analyzer that tries to map code shape to hardware risk before runtime profiling.

---

## Core Thesis

Low-latency failures in x86-64 systems are frequently locked in by source structure long before production load tests:

- cache-line sharing patterns (false sharing, multi-line hot structs),
- atomic ordering choices on TSO,
- lock/allocator usage in hot code,
- dispatch structures that stress branch prediction.

Runtime tools (`perf`, production tracing) tell you what happened after integration. Faultline aims to catch structural hazards **at compile-analysis time**, where fixes are cheapest.

### Platform model

| Platform | Status | Notes |
|---|---|---|
| Linux x86-64 | Supported | Primary model and assumptions |
| WSL2 | Supported | Linux userspace/kernel path |
| macOS Intel | Not supported | No validated PMU/runtime assumptions |
| macOS Apple Silicon | Not supported | Different cache-line and memory model |
| Windows native | Not supported | Build and IR-emission path assumes Unix tooling |

---

## Differentiation

Faultline is complementary to existing tools.

| Tool | What it does well | What it cannot do alone | Where Faultline fits |
|---|---|---|---|
| `pahole` | Post-build struct layout introspection (DWARF/BTF) | No hot-path semantics, no atomic ordering analysis, no function-level hazard attribution | Faultline reasons on AST/IR semantics and emits code-location diagnostics |
| `perf` / PMU tracing | Ground-truth runtime counters, latency distribution, bottleneck attribution | Requires runnable workload; discovers issues late; weak compile-time prevention | Faultline is pre-runtime structural triage; hypothesis subsystem can generate perf experiments |
| `llvm-mca` | Throughput/latency modeling for instruction blocks | No whole-program sharing/topology/layout semantics; no thread-coherence modeling | Faultline operates at source/IR structure level across declarations and functions |

**Pipeline position:** source/compile-analysis phase.  
Faultline detects structural risk candidates; runtime tools validate impact magnitude.

---

## How It Works 

## 1) Clang AST pass (structural detection)

- Entry point: `ClangTool` + `FaultlineActionFactory`.
- `FaultlineASTConsumer::HandleTranslationUnit` walks top-level decls, classifies hot-path functions, then runs all registered rules.
- Rule contract: `Rule::analyze(const Decl*, ASTContext&, HotPathOracle&, vector<Diagnostic>&)`.

### Key APIs used

- `ASTContext::getASTRecordLayout` for record size and field offsets.
- `SourceManager` for stable file/line/column diagnostics.
- `RecursiveASTVisitor` per rule for function-body pattern scanning.

## 2) Optional LLVM IR pass (post-lowering refinement)

- Compiler resolved from `compile_commands.json` argv[0] with PATH fallback.
- IR emitted via `llvm::sys::ExecuteAndWait` (no shell interpolation).
- Bounded parallelism: `std::counting_semaphore` capped at `hardware_concurrency()`.
- Deterministic temp file naming via MD5(source content + compile args + tool version).
- Incremental cache: identical inputs reuse prior IR without recompilation.
- Stderr captured per subprocess and logged on failure.
- Parses IR using `llvm::parseIRFile` (sequential — `LLVMContext` is not thread-safe).
- `IRAnalyzer` inspects:
  - `AllocaInst` (stack footprint),
  - atomic `LoadInst`/`StoreInst`/`AtomicRMWInst`/`AtomicCmpXchgInst`,
  - `FenceInst`,
  - direct/indirect call sites.
- `DiagnosticRefiner` adjusts confidence/escalations using named evidence constants (`ConfidenceModel.h`), with per-factor floor/ceiling bounds.

## 3) Data layout and ordering boundaries

- Layout in AST rules uses target ABI through Clang record layout APIs.
- Ordering inspection at IR level uses LLVM atomic ordering enums.
- Current model is **AST-first + IR refinement**, not a full site-precise lowering proof engine.

## 4) Hot-path selection

- `[[clang::annotate("faultline_hot")]]`
- glob patterns (`hot_function_patterns`, `hot_file_patterns`) in config

---

## Build and Run

## Requirements

| Dependency | Version |
|---|---|
| Linux x86-64 | required |
| LLVM/Clang | 16+ |
| CMake | 3.20+ |
| C++ | C++20 |

```bash
# Ubuntu/Debian
apt install llvm-18-dev libclang-18-dev clang-18 cmake

# Arch
pacman -S llvm clang cmake
```

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

If LLVM is not auto-detected:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/lib/llvm-18
```

## Usage

```bash
./build/faultline /path/to/source.cpp -- -std=c++20
./build/faultline --config=./faultline.config.yaml /path/to/source.cpp --
./build/faultline --format=json /path/to/source.cpp --
./build/faultline --format=sarif /path/to/source.cpp --
./build/faultline --output=report.sarif --format=sarif /path/to/source.cpp --
./build/faultline --min-severity=High /path/to/source.cpp --
./build/faultline --min-evidence=proven /path/to/source.cpp --
./build/faultline --no-ir /path/to/source.cpp --
./build/faultline --ir-opt=O1 /path/to/source.cpp --
./build/faultline --calibration-store=/path/to/store /path/to/source.cpp --
```

---

## Rule Surface (Current)

| ID | Hazard |
|---|---|
| FL001 | Cache line spanning struct |
| FL002 | False sharing candidate |
| FL010 | Overly strong atomic ordering |
| FL011 | Atomic contention hotspot |
| FL012 | Lock in hot path |
| FL020 | Heap allocation in hot path |
| FL021 | Large stack frame |
| FL030 | Virtual dispatch in hot path |
| FL031 | `std::function` in hot path |
| FL040 | Centralized mutable global state |
| FL041 | Contended queue pattern |
| FL050 | Deep conditional tree |
| FL060 | NUMA-unfriendly shared structure |
| FL061 | Centralized dispatcher bottleneck |
| FL090 | Hazard amplification |

---

## Before vs After Examples

## 1) Cache-line footprint / split risk (FL001)

### Before

```cpp
struct OrderBookLevel {
    uint64_t px[8];
    uint64_t qty[8];
    uint64_t flags[4];
};
// sizeof = 160B (spans 3 cache lines)
```

### Faultline diagnostic (example)

```text
[HIGH] FL001 — Cache Line Spanning Struct
  Evidence: sizeof(OrderBookLevel)=160B; lines_spanned=3
```

### After

```cpp
struct alignas(64) OrderBookHot {
    uint64_t px[4];
    uint64_t qty[4];
};

struct OrderBookCold {
    uint64_t px[4];
    uint64_t qty[4];
    uint64_t flags[4];
};
```

### Why latency can improve

Hot-path accesses touch fewer lines, reducing L1D pressure and coherence surface for write-heavy members.

## 2) False sharing (FL002 / FL041)

### Before

```cpp
struct Counters {
    std::atomic<uint64_t> head;
    std::atomic<uint64_t> tail;
};
```

### Faultline diagnostic (example)

```text
[CRITICAL] FL002 — False Sharing Candidate
  Evidence: sizeof=16B; mutable_fields=[head,tail]; atomics=yes; thread_escape=true
```

### After

```cpp
struct Counters {
    alignas(64) std::atomic<uint64_t> head;
    alignas(64) std::atomic<uint64_t> tail;
};
```

### Why latency can improve

Separating independent writers onto different lines reduces cross-core ownership transfer (RFO/HITM traffic).

## 3) TSO ordering misuse risk (FL010)

### Before

```cpp
[[clang::annotate("faultline_hot")]]
void publish(std::atomic<uint64_t>& seq, uint64_t v) {
    seq.store(v); // implicit seq_cst
}
```

### Faultline diagnostic (example)

```text
[HIGH] FL010 — Overly Strong Atomic Ordering
  Evidence: ordering=seq_cst; function=publish
```

### After

```cpp
[[clang::annotate("faultline_hot")]]
void publish(std::atomic<uint64_t>& seq, uint64_t v) {
    seq.store(v, std::memory_order_release);
}
```

### Why latency can improve

On x86-64 TSO, weaker ordering for simple publish patterns can avoid unnecessary global-order constraints, reducing serialization pressure in hot loops.

---

## Hot Path Annotation

```cpp
[[clang::annotate("faultline_hot")]]
void onMarketData(const Update& u);
```

Or config:

```yaml
hot_function_patterns:
  - "*::onMarketData*"
  - "*::process*"
hot_file_patterns:
  - "*/hot_path/*"
```

---

## Output Formats

| Format | Flag | Description |
|---|---|---|
| CLI | `--format=cli` (default) | Human-readable terminal output with severity, rule ID, location, evidence |
| JSON | `--format=json` | Machine-readable; includes `metadata` block with tool version, timestamp, compiler paths, source files |
| SARIF 2.1.0 | `--format=sarif` | GitHub/CI-compatible; `invocations` with execution provenance, `artifacts`, rule descriptors |

All structured formats embed execution metadata for reproducibility.

---

## Evidence Model

Every diagnostic carries three orthogonal quality signals:

| Signal | Values | Meaning |
|---|---|---|
| **Severity** | Critical, High, Medium, Informational | Worst-case latency impact |
| **Confidence** | 0.0–1.0 | Tool's belief the hazard is real at this site |
| **Evidence Tier** | Proven, Likely, Speculative | Strength of structural/IR backing |

IR refinement adjusts confidence using named evidence constants with per-factor floor/ceiling bounds. Adjustments are traceable via `escalations` in output.

Calibration feedback (`--calibration-store`) suppresses known false positives via feature-neighborhood matching. **Safety rail:** Critical/High + Proven findings are never auto-suppressed.

---

## Validation

```bash
./validation/run.sh              # Both tiers
./validation/run.sh --tier1      # Corpus regression (internal + external)
./validation/run.sh --tier2      # Ground truth microbenchmarks
```

**Tier 1** runs faultline against internal test samples and external corpora (folly, abseil, seastar). Asserts: no crash, deterministic output, valid locations, distribution sanity, evidence parseability.

**Tier 2** runs paired hazardous/fixed microbenchmarks per rule with hardware timing and optional `perf stat` counters. Statistical analysis via paired t-test when scipy is available.

See `validation/README.md` for full details.

---

## Boundaries

- Static analysis only. Does not measure runtime PMU events.
- Some hazards (NUMA remoteness, LFB pressure, prefetch pollution) require runtime validation.
- IR refinement improves confidence but is not a full source-to-lowered site-bijective proof.
- EscapeAnalysis uses AST-structural type matching (template specialization, qualified names), not interprocedural dataflow.
- Incremental cache keys on source content + compile args + tool version; external header changes require cache invalidation.

---

## Exit Codes

| Code | Meaning |
|---|---|
| 0 | No findings at or above min severity |
| 1 | Findings emitted |
| 2 | Input parse error (missing headers, invalid source) |