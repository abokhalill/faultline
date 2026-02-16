# Faultline — Architecture and Latency Model

## 1. System Architecture Overview

Faultline is implemented as a Clang-based static analysis tool with optional LLVM IR augmentation.

It operates in three layers:

1. **AST Layer** (Structural Semantics)
2. **IR Layer** (Lowered Semantics)
3. **Latency Risk Engine** (Hardware-Aware Heuristics)

Each layer serves a distinct purpose.

---

## 2. Analysis Layers

### 2.1 AST Layer (Primary Structural Analysis)

The AST layer performs:

- Struct layout inspection
- Field-level mutability analysis
- Escape analysis (does an object cross thread boundaries?)

**Detection of:**

- Virtual dispatch
- `std::function` usage
- `std::shared_ptr` / `std::unique_ptr` in hot paths
- Dynamic allocation sites
- Atomic types and memory order usage
- Lock usage (`std::mutex`, spinlocks, etc.)

This layer captures high-level structural intent before compiler optimizations.

**Primary responsibility:**

Detect structural patterns that correlate with cache instability or synchronization amplification.

### 2.2 IR Layer (Lowered Behavior Inspection)

The LLVM IR layer is used selectively for:

- Detecting actual heap allocation calls after inlining
- Identifying atomic instructions and fence emissions
- Inspecting generated memory barriers
- Estimating stack frame size
- Identifying indirect calls that survived devirtualization
- Detecting `memset`/`memcpy` expansions in hot paths

The IR layer exists to validate or refine AST-level assumptions.

Faultline does not attempt full cycle-accurate modeling.

It inspects structural machine-level implications.

### 2.3 Latency Risk Engine

The Latency Risk Engine consumes structured findings from AST and IR layers.

It:

- Maps findings to microarchitectural risk categories
- Scores severity
- Correlates interacting risks
- Generates diagnostics with hardware reasoning

**Example:**

- Struct spans 192 bytes
- Contains atomics
- Escapes thread boundary

→ Elevated false sharing probability  
→ Severity escalated from High to Critical

The engine is rule-driven but context-sensitive.

---

## 3. Hot Path Detection Strategy

Static analysis cannot reliably know "hotness".

Faultline supports three mechanisms:

| Mechanism | Description |
|-----------|-------------|
| **Attribute-based** | `[[faultline::hot]]` |
| **Configuration-based** | File or function patterns |
| **Heuristic-based** | Functions called within tight loops; Functions reachable from annotated entry points |

Hot path classification affects rule strictness.

Non-hot code is analyzed with reduced severity.

---

## 4. Latency Model

Faultline assumes the following baseline hardware model.

### 4.1 Cache Model

- Cache line size: 64 bytes
- L1: Private per core
- L2: Private per core
- L3/LLC: Shared
- Write-back caches
- MESI coherence protocol

**Risk factors modeled:**

- Cache line spanning
- False sharing
- Line ping-pong under atomic writes
- Excessive line footprint
- Poor spatial locality

Faultline does not simulate cache sets or associativity exhaustively.

It models line-level structural exposure.

### 4.2 Memory Model

**Assumed architecture:** x86-64  
**Memory model:** TSO (Total Store Order)

**Key properties:**

- Loads may not reorder with other loads
- Stores are globally ordered
- Store buffer exists
- Fences incur pipeline cost

**Faultline detects:**

- Overly strong `memory_order_seq_cst` usage
- Redundant fences
- Potential acquire/release misuse
- Unnecessary atomic operations in single-writer contexts

It does not verify correctness of concurrent algorithms.

It evaluates structural latency cost.

### 4.3 Store Buffer Model

Store buffers:

- Temporarily hold pending stores
- Can stall when saturated
- Cause inter-core ownership transfer on atomic writes

**Faultline flags:**

- Tight loops with frequent atomic stores
- Multiple atomics within same cache line
- Shared-write heavy structures

These are high-probability tail-latency amplifiers.

### 4.4 TLB Model

Faultline tracks:

- Large stack allocations
- Large contiguous heap allocations
- Potential page fragmentation patterns

It assumes 4KB pages by default.

Huge page usage is configurable.

TLB modeling is heuristic, not exact.

### 4.5 Branch Prediction Model

Faultline flags:

- Deeply nested conditionals in hot paths
- High polymorphism call sites
- Indirect calls
- Large switch statements

It cannot compute branch entropy statically.

It identifies unpredictability risk patterns.

### 4.6 NUMA Model

**Baseline assumption:**

- Multi-core system
- Potential multi-socket deployment

**Static detection includes:**

- Global shared mutable state
- Thread-escaping heap objects
- Centralized queues used by multiple threads

Faultline cannot determine runtime NUMA placement.

It flags structural patterns likely to cause remote memory access.

---

## 5. Severity Escalation Logic

Risk severity increases when multiple hazards interact.

**Example escalation rules:**

| Condition | Severity |
|-----------|----------|
| Struct &gt; 128 bytes + Shared across threads + Contains atomic writes | **Critical** |
| Atomic with `seq_cst` + Inside tight loop | **High** |
| Large stack frame (&gt; 2KB) + Deep call chain | TLB pressure warning |

This interaction modeling differentiates Faultline from trivial linters.

---

## 6. Output Architecture

Faultline outputs:

- CLI-readable diagnostics
- JSON structured output
- SARIF integration for CI

**Each diagnostic includes:**

- Rule ID
- Severity
- Hardware reasoning
- Structural evidence
- Suggested mitigation
- Confidence score

Confidence score is essential to avoid blind trust.

---

## 7. Precision Strategy

Faultline prioritizes:

&gt; **Precision &gt; Coverage**

False positives reduce credibility.

Rules must include:

- Structural verification
- Thread escape analysis
- Context awareness

Speculative warnings are classified as Informational.

---

## 8. Extensibility

Rules are modular.

Latency model assumptions are configurable via:

- `faultline.config.yaml`

Future hardware models may be added.

But v1 is x86-64 focused.

---

## 9. Architectural Principle

Faultline does not attempt to guess runtime behavior.

It identifies structural stress concentration points under plausible multi-core load.

&gt; If it cannot justify a hardware-level explanation for a warning, it does not emit one.