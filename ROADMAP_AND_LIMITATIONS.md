# Faultline — Roadmap and Limitations

## 1. Core Principle

Faultline is a structural latency risk detector.

It does **not**:

- Predict exact latency
- Replace runtime profiling
- Guarantee performance
- Prove concurrency correctness

Its purpose is to identify architectural stress points before they manifest as tail latency collapses.

Scope discipline is mandatory.

---

## 2. Roadmap

We build in controlled phases.

No feature creep.

No premature sophistication.

### Phase 0 — Foundation (Weeks 1–3)

**Goal:** Working Clang-based analyzer with structured diagnostics.

**Deliverables:**

- Clang LibTooling integration
- AST traversal infrastructure
- Hot-path annotation support
- Basic CLI + JSON output
- Config file support
- Rule registration system
- Severity classification engine

No IR analysis yet.

Focus: correctness and infrastructure.

### Phase 1 — Structural Cache & Allocation Rules (Weeks 4–8)

**Implement:**

- FL001 (Cache Line Spanning Struct)
- FL002 (False Sharing Candidate)
- FL020 (Heap Allocation in Hot Path)
- FL030 (Virtual Dispatch in Hot Path)
- FL031 (std::function in Hot Path)
- FL021 (Large Stack Frame)

**Add:**

- Basic escape analysis
- Struct layout analysis
- Field-level mutability detection

**Validation:**

- Microbenchmarks for each rule
- perf correlation experiments

At end of Phase 1, Faultline must produce meaningful output on real systems.

### Phase 2 — Synchronization & Atomic Analysis (Weeks 9–14)

**Implement:**

- FL010 (Overly Strong Atomic Ordering)
- FL011 (Atomic Contention Hotspot)
- FL012 (Lock in Hot Path)
- FL041 (Contended Queue Pattern)
- FL090 (Hazard Amplification)

**Add:**

- Memory order inspection
- Thread escape refinement
- Basic contention heuristics
- Rule interaction scoring

**Validation:**

- Store buffer stress tests
- Atomic contention microbenchmarks
- Tail latency histogram shifts

This phase differentiates Faultline from trivial linters.

### Phase 3 — IR Layer & Deeper Modeling (Weeks 15–22)

**Add LLVM IR inspection:**

- Detect real indirect calls post-devirtualization
- Detect lowered fences
- Detect inlined allocation paths
- Stack frame size from IR
- Confirm atomic instruction emission

**Enhance:**

- Confidence scoring
- Reduced false positives
- Composite risk reasoning

**Validation:**

- Re-run full corpus
- False positive audit
- Confidence calibration

### Phase 4 — NUMA & Advanced Structural Heuristics (Weeks 23–30)

**Add:**

- Global mutable state detection
- Cross-thread object sharing analysis
- Heuristic NUMA risk classification
- Centralized dispatcher detection

**Validation:**

- Dual-socket experiments
- Remote memory measurement

At this stage, Faultline becomes architecturally interesting.

### Phase 5 — Case Studies & Publication (Weeks 31+)

**Deliverables:**

- 3–5 documented real-world analyses
- Before/after performance measurements
- Whitepaper-level writeup
- Public repo with reproducible experiments

This is where signaling value peaks.

---

## 3. Limitations

Faultline has hard limits.

We state them explicitly.

### 3.1 Static Analysis Cannot Infer Runtime Scheduling

Faultline cannot:

- Know actual thread placement
- Detect real contention probability
- Infer runtime core affinity

It flags structural exposure, not runtime certainty.

### 3.2 No Cycle-Accurate Modeling

Faultline does not simulate:

- Cache associativity exhaustion
- Precise pipeline stalls
- Exact branch misprediction rates
- Store buffer depth

It reasons at the structural level only.

### 3.3 No Concurrency Correctness Guarantees

Faultline does not verify:

- Data race absence
- Lock correctness
- Linearizability
- Wait-freedom

It evaluates latency risk, not correctness proof.

### 3.4 No Architecture Generalization (v1)

v1 assumes:

- x86-64
- 64B cache lines
- TSO memory model

Other architectures are out of scope.

### 3.5 Escape Analysis Is Heuristic

Thread escape detection may produce:

- Conservative false positives
- Missed edge cases

Confidence scoring must reflect this.

### 3.6 Branch Predictability Cannot Be Proven

Faultline flags structural unpredictability patterns.

It cannot measure entropy statically.

Branch-related warnings carry lower confidence.

### 3.7 Performance Is Non-Linear

A flagged issue may:

- Show minimal impact in isolation
- Become catastrophic under real load

Faultline models structural stress accumulation, not isolated microbench throughput.

---

## 4. Non-Expansion Rules

Faultline must not expand into:

- Generic C++ style linting
- Algorithmic complexity analysis
- Security vulnerability scanning
- Undefined behavior detection
- Code formatting tools

If a rule does not tie directly to:

- Cache
- Coherence
- Store buffer
- TLB
- Branch predictor
- NUMA
- Allocator behavior

It does not belong.

---

## 5. Success Criteria for v1

Faultline v1 is complete when:

- At least 20 validated high-confidence rules exist.
- False positive rate for High/Critical &lt; 15%.
- At least 3 real-world case studies show measurable improvements.
- Senior low-latency engineers acknowledge diagnostic validity.
- The tool can run in CI without excessive noise.

If those are met, Faultline is real.

If not, it remains academic.

---

## 6. Strategic Outcome

If executed properly, Faultline becomes:

- A credibility engine
- A differentiator in interviews
- A technical signal amplifier
- Potential internal tooling candidate at elite firms
- Potential foundation for a latency-focused developer platform

If executed poorly, it becomes:

- A clever static analyzer no one uses

The difference will not be in code volume.

It will be in empirical rigor.