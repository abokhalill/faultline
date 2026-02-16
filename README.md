# Faultline

Structural latency landmine detector for ultra-low-latency C++ systems.

Built on Clang/LLVM. Targets x86-64 / 64B cache lines / TSO memory model.

## Prerequisites

- LLVM/Clang 16+ (development libraries)
- CMake 3.20+
- C++20 compiler

### Ubuntu/Debian

```bash
apt install llvm-18-dev libclang-18-dev clang-18 cmake
```

### Arch

```bash
pacman -S llvm clang cmake
```

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/lib/llvm-18
make -j$(nproc)
```

## Usage

```bash
# Analyze source files
./faultline /path/to/source.cpp -- -std=c++20

# With config
./faultline --config=../faultline.config.yaml /path/to/source.cpp --

# JSON output
./faultline --json /path/to/source.cpp --

# Filter by severity
./faultline --min-severity=High /path/to/source.cpp --

# AST-only mode (skip IR analysis)
./faultline --no-ir /path/to/source.cpp --

# IR analysis at O1 (shows optimizer effects)
./faultline --ir-opt=O1 /path/to/source.cpp --

# With compilation database
cd /project/build
/path/to/faultline /project/src/hot_path.cpp
```

## Rules

### Structural Cache Risks

| ID | Title | Severity | Mechanism |
|----|-------|----------|-----------|
| FL001 | Cache Line Spanning Struct | High/Critical | L1D pressure, coherence invalidation surface |
| FL002 | False Sharing Candidate | Critical | MESI invalidation ping-pong on shared cache line |

### Synchronization Risks

| ID | Title | Severity | Mechanism |
|----|-------|----------|-----------|
| FL010 | Overly Strong Atomic Ordering | High/Critical | Store buffer drain from unnecessary seq_cst fences |
| FL011 | Atomic Contention Hotspot | Critical | Cache line ownership thrashing via RFO |
| FL012 | Lock in Hot Path | Critical | Lock convoy, futex syscall, context switch |

### Memory Allocation Risks

| ID | Title | Severity | Mechanism |
|----|-------|----------|-----------|
| FL020 | Heap Allocation in Hot Path | Critical | Allocator lock contention, TLB pressure, page faults |
| FL021 | Large Stack Frame | Medium/Critical | TLB pressure, L1D capacity, stack page faults |

### Dispatch Risks

| ID | Title | Severity | Mechanism |
|----|-------|----------|-----------|
| FL030 | Virtual Dispatch in Hot Path | High/Critical | BTB misprediction, pipeline flush |
| FL031 | std::function in Hot Path | High/Critical | Type-erased indirect call, potential heap alloc |

### Structural Design Risks

| ID | Title | Severity | Mechanism |
|----|-------|----------|-----------|
| FL040 | Centralized Mutable Global State | High/Critical | NUMA remote access, cache line contention |
| FL041 | Contended Queue Pattern | High/Critical | Head/tail cache line bouncing |
| FL060 | NUMA-Unfriendly Shared Structure | High/Critical | Remote memory access penalty on multi-socket |
| FL061 | Centralized Dispatcher Bottleneck | High/Critical | I-cache pressure, BTB contention from fan-out |

### Branching Risks

| ID | Title | Severity | Mechanism |
|----|-------|----------|-----------|
| FL050 | Deep Conditional Tree | Medium/High | Branch misprediction chains |

### Interaction Rules

| ID | Title | Severity | Mechanism |
|----|-------|----------|-----------|
| FL090 | Hazard Amplification | Critical | Nonlinear compound risk from interacting hazards |

## IR Analysis Layer

Faultline includes an LLVM IR analysis pass that refines AST-layer findings:

- **Stack frame sizing**: Precise alloca-based measurement vs AST heuristic
- **Atomic confirmation**: Verifies seq_cst instructions are actually emitted
- **Heap allocation**: Confirms alloc/free calls survive inlining
- **Indirect calls**: Counts remaining indirect calls post-devirtualization
- **Fence detection**: Identifies explicit fence instructions in lowered IR

The IR pass emits LLVM IR via `clang -emit-llvm` and parses it with LLVM's IRReader. Default optimization level is `-O0` (structural confirmation). Use `--ir-opt=O1` to see optimizer effects.

## Hot Path Annotation

```cpp
[[clang::annotate("faultline_hot")]]
void onMarketData(const MDUpdate& update) { ... }
```

Or use config patterns in `faultline.config.yaml`:

```yaml
hot_function_patterns:
  - "*::onMarketData*"
  - "*::process*"
hot_file_patterns:
  - "*/hot_path/*"
```

## Exit Codes

| Code | Meaning |
|------|---------|
| 0    | No hazards detected |
| 1    | Hazards detected |
| 2    | Tool error |

## Project Structure

```
include/faultline/
  core/           Severity, Diagnostic, Rule, RuleRegistry, Config, HotPathOracle
  analysis/       FaultlineAction, ASTConsumer, StructLayoutVisitor, EscapeAnalysis
  ir/             IRAnalyzer, IRFunctionProfile, DiagnosticRefiner
  output/         CLI and JSON formatters
src/
  core/           Core implementations
  analysis/       AST traversal, escape analysis
  ir/             IR analysis and diagnostic refinement
  output/         Output formatting
  rules/          15 rule implementations (FL001–FL090)
  main.cpp        CLI driver
test/samples/     Test inputs including HFT order engine case study
```

## Documentation

- `VISION.md` — Scope and mission
- `ARCHITECTURE.md` — Analysis layers and latency model
- `RULEBOOK.md` — Rule specifications
- `SCORING_MODEL.md` — Validation and benchmarking strategy
- `ROADMAP_AND_LIMITATIONS.md` — Phased delivery and hard limits
