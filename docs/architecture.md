# Architecture

lshaz maps C/C++ source-level patterns to microarchitectural latency hazards.
This document describes the analysis pipeline, the supporting analyses rules
draw on, the evidence model, and the determinism contract. Per-rule detection
logic lives in [rules.md](rules.md).

## System layers

1. **AST layer**. Structural analysis via Clang AST: record layouts, field
   mutability, escape analysis, atomic usage, write sites, dispatch patterns,
   allocation sites.
2. **IR layer** (optional; `--no-ir` disables), re-emits LLVM IR per TU and
   confirms or refutes AST findings after optimization: surviving heap calls,
   atomic instructions, indirect calls, real frame sizes.
3. **Post-processing**. Cross-TU aggregation, deduplication, interaction
   synthesis, precision budget, calibration suppression, build-health
   detection, final sort.

## Stage 1: AST analysis

Entry point: `LshazASTConsumer::HandleTranslationUnit`. For each TU, walks all
top-level declarations. Recursing into namespaces, linkage specs, and nested
record types, and runs every registered rule. System headers are skipped;
dependent and invalid declarations are filtered before rule execution.

Rules are **stateless singletons** registered via `LSHAZ_REGISTER_RULE`. All
per-TU state lives in the analyses injected into `Rule::analyze`, never on
the rule object (see [Determinism](#determinism)).

**TU-level safety:**

- TUs with fatal parse errors are skipped entirely; no diagnostics are emitted
  from partial ASTs. Each failure is recorded as a `FailedTU` carrying the
  file path and the verbatim first error message, captured by an
  `ErrorCapture` diagnostic consumer. These feed header-fingerprint detection
  (B001, below).
- Per-TU crash isolation via `llvm::CrashRecoveryContext`: a TU that raises
  SIGSEGV/SIGABRT is recorded as failed and scanning continues.

### Supporting analyses

**CacheLineMap** (`src/analysis/cache_line.cpp`), exact field-to-line
mapping from `ASTRecordLayout`, including base subobjects and nested records.
Key semantics:

- *Bucketing.* When the record's alignment is below the line size, the base
  address can sit at any realizable shift; each field is bucketed into the
  union of lines it could occupy under any shift. Bucket contents are
  therefore an over-approximation for sub-line-aligned records and exact for
  line-aligned ones.
- *Pair co-residency.* Shared-line pairs are **not** derived from bucket
  co-membership alone (fields whose shift ranges overlap in line index may
  never coexist at the same shift). A pair requires a realizable common
  shift placing both fields on one line, checked over all shifts in
  record-alignment steps; degenerates to the exact same-line test at
  alignment ≥ line size.
- *Straddlers.* The per-field `straddles` flag is geometric (spans a boundary
  under some shift). `straddlingFields()`. The API behind split load/store
  penalty escalations. Additionally requires an access granule wider than
  one byte: byte arrays span lines but cannot split a single access.
- *Layout-intent signals.* `isCacheLineAligned()` (record alignment ≥ line)
  and `hasTrailingLinePad()` (trailing byte-array pad reaching an exact line
  multiple) feed the deliberate-layout demotion contract in FL001/FL002/FL090.
- *Atomic detection* covers `_Atomic`, `std::atomic`, volatile typedefs with
  "atomic" in the name, and user-configured wrapper types
  (`atomic_type_names`).
- *Refcount heuristic.* A record whose only atomic matches a refcount naming
  pattern is downgraded (FL001) or suppressed (FL002), COW/`shared_ptr`
  control blocks do not false-share.

**EscapeAnalysis** (`src/analysis/escape.cpp`), decides whether a
type may be accessed from multiple threads and quantifies expected contention.

- *Escape signals* (eight): atomic members, sync-primitive members
  (`std::mutex` family + POSIX types), `shared_ptr`/`weak_ptr` members,
  volatile members, publication to `std::thread`/`std::jthread`/`std::async`,
  storage in a non-`thread_local` mutable global, global-scope `shared_ptr`
  pointees, and **direct thread writers**. A record written from ≥2 functions
  one of which is spawned as a thread. Publication requires an address to
  cross a thread boundary; a file-scope object written directly from two
  thread bodies never does, and that is the striped-counter shape.
  Conservative: uncertainty means escape.

  All member-type predicates peel array extents first. A field declared
  `_Atomic uint64_t c[N]` or `std::atomic<T> slots[N]` has field type
  `ArrayType(element)`, so without peeling the atomic, sync and volatile
  checks all see an array and nothing else, arrays of atomics, the dominant
  striped-counter shape, were invisible to every rule gated on them.

- *Sharing route*. Escape means "threads can reach this type." False sharing
  needs the stronger "two cores can reach the **same object**," which
  `EscapeVerdict::hasSharingRoute` states once so that rules stop
  re-deriving it and disagreeing:

  ```
  hasSharingRoute = hasPublication                       // address crossed a thread boundary
                  || hasThreadWriters                    // >=2 writers, one thread-borne
                  || (hasGlobalInstance && anyWriterOnThread)
  ```

  `hasGlobalInstance` (a file-scope instance of the type exists) is tracked
  separately from `hasPublication`. Conflating them made every global look
  published and, because `getAsCXXRecordDecl()` returns null for a C struct,
  made publication evidence silently never fire on C at all.

- *Standing versus handed-over writes*. The discriminator writer counts
  cannot express. `g_stats.hits++` reaches a fixed object every thread can
  name; `io->len = n` operates on whatever the caller passed in, and a queue
  hands each request to one owner at a instant. Both look identical to a
  writer count, which is why per-request objects graded as contended.
  `recordWrite` classifies each field write by walking its base expression to
  the root declaration: global storage means standing access, a parameter
  means the object arrived from elsewhere.

- *Pool roles*. Contention needs two **cores**, not two functions. A thread
  entry spawned inside a loop, or from more than one site, runs on several
  threads at once, so a single writer function already puts two cores on the
  line. Requiring two distinct writer functions rejected the commonest
  thread-pool shape outright. The loop usually sits around a spawner wrapper
  rather than the `pthread_create` itself, so multiplicity is read one level
  out through spawner resolution.
- *Write-site collection* (one traversal over all TU function bodies):
  - **Global write counts** per `VarDecl`, across all write forms, plain
    assignment, `++`/`--`, member writes through the global, C11/GNU atomic
    builtins, `__sync_*`, and non-const `std::atomic` mutating methods. Feeds
    FL040 and write-once analysis.
  - **Field write evidence** per `FieldDecl`: write-site count and the set of
    writer functions. Constructor member-init lists are excluded,
    initialization is not contention. Feeds FL002's pair grading
    (`pairHasDistinctWriters`: the union of two fields' writers has ≥2
    members; for an intra-array self-pair this reduces to "this array is
    written from ≥2 functions", which is the correct question).
    Array subscripts are peeled: a write to `arr[i]` is a write to the field
    `arr`, and without that every element write of every striped counter
    resolves to nothing and the array reads as never written.
- *Lifecycle.* Instantiated fresh per TU inside `HandleTranslationUnit` and
  passed by reference into every rule. After rule execution,
  `buildEscapeSummary()` snapshots per-type signals keyed by canonical
  qualified name for cross-TU aggregation.

**AllocatorTopology**. Classifies allocator contention from `--allocator`:
glibc (arena lock), tcmalloc/jemalloc (thread-local cache), mimalloc
(pool/slab). Shapes FL020 severity.

**NUMATopology**. Infers page placement under first-touch: local-init,
main-thread, any-thread, interleaved, explicit-bind, unknown. Feeds FL060.

**CallGraph**. Per-TU caller→callee map from `CallExpr` visits. Used by
HotPathOracle for transitive hotness. The same walk detects thread-entry
arguments (`pthread_create`, `thrd_create`, `std::thread`/`std::jthread`,
`std::async`) and snapshots name-keyed edges for the thread-role reduce.

**Thread-role attribution** (`ThreadRoleSummary`), per-TU facts (entries,
name-keyed call edges, field-writer names) piggyback the CallGraph and
EscapeAnalysis traversals, merge across TUs beside the escape summary, and
reduce on the parent to per-function MAIN/WORKER masks by BFS from `main()`
and the observed entries (config globs seed roots that function-pointer
dispatch hides). Verdicts exist only post-merge. Consumers: FL002/FL090
confidence escalation when a flagged pair's writers attribute to provably
disjoint roles (any unknown or mixed-role writer defeats it), and the FL092
precedent join.

**DataFlowAnalyzer**. Intra-procedural, two passes: bind variables to heap
allocations and atomic loads, then track uses, alloc-escapes,
alloc-flows-to-loop, atomic-feeds-branch (CAS retry / spin-wait signature).
Escalation input to FL010 and FL020.

**HotPathOracle**. Classifies functions hot and records *how*, because the
strength of the signal bounds the finding's severity. Sources, strongest
first (`HotnessSource`):

| Source | Established by | Ceiling |
|---|---|---|
| `Profiled` | perf samples above `hotness_threshold_pct` | none |
| `Declared` | `__attribute__((hot))`, `[[clang::annotate("lshaz_hot")]]`, config globs, or transitive propagation from such a root | none |
| `InferredDeep` | nested loops or recursion on a path from an entry | one grade below the assigned severity |
| `InferredShallow` | one loop level from an entry | two grades below |
| `None` |, | rule does not fire |

`record()` keeps the strongest source, so an inference can never downgrade an
explicit signal.

**Structural inference** (`inferFromCodeShape`, enabled by `infer_hot_paths`)
exists because an unconfigured scan otherwise leaves every hot-path rule
inert. Memcached reported 0 hot of 2219 functions, rocksdb 0 of 1.4M.
Repetition is what makes a cache miss steady-state, and a loop is where
repetition is written down:

```
seeds:     thread entry points (from CallGraph) and main
relax:     depth(callee) = max(depth(caller) + loopDepth(call site))
recursion: self-edge counts as one level
bound:     kMaxDepth = 4, so cycles settle rather than diverge
grade:     own loop nesting >= 2 sharpens by one level
```

No project symbol is named, so the inference cannot overfit to one codebase.

> **A function's own loop nesting is not a seed.** It establishes cost per
> call, not call frequency, and the two are independent. Seeding on it marked
> every initializer hot: memcached's `extstore_init` contains six loops, and
> half the codebase graded hot, which makes the label meaningless.

Inference is per-TU. A function looped over from another translation unit is
invisible to it, so a library scanned without its application has thin
coverage. Reported explicitly rather than left to look like a clean result.

## Stage 2: IR refinement

The IR pass re-compiles each TU to LLVM IR and adjusts AST-finding confidence
against post-optimization reality. TUs that failed AST parsing are skipped
individually; one broken TU does not disable refinement for the rest.

**Emission:** compiler resolved from `compile_commands.json` (the entry's
argv[0] is dropped, not re-executed), with PATH fallback to
`clang++`/`clang++-18`/`-17`/`-16`. When the recorded compiler is GCC,
emission substitutes clang and strips GCC-only flags. Subprocesses run via
`llvm::sys::ExecuteAndWait` (no shell), bounded by `--ir-jobs`, 120s timeout,
sharded per `--ir-batch-size` with one `LLVMContext` per shard. IR artifacts
are content-addressed (MD5 of source + mtime + args + tool version); identical
inputs reuse the cache unless `--no-ir-cache`.

**Analysis (`IRAnalyzer`):** per function, stack allocations (name, size),
heap call sites (direct/indirect, in-loop), atomic operations (kind, ordering,
in-loop, source location), block/loop counts, indirect vs direct calls.

**Refinement (`DiagnosticRefiner`):** matches IR functions to diagnostics by
demangled name (component-boundary match, deterministic rank-based pick among
overloads) and applies bounded confidence deltas:

| Factor | Delta |
|---|---|
| Site-confirmed (source line match) | +0.10 |
| Function-confirmed (no line match) | +0.05 |
| Pattern absent in optimized IR | −0.20 |
| Heap allocation survived inlining | +0.05 |
| Heap allocation eliminated | −0.15 |
| Indirect calls confirmed (devirtualization failed) | +0.10 |
| Fully devirtualized | −0.25 |
| Lock call confirmed in lowered code | +0.05 |
| Stack frame size confirmed | +0.10 |

Every adjustment appends an "IR confirmed"/"IR refinement" line to the
diagnostic's escalation trace, refinement is visible, never silent.

## Stage 3: Post-processing

In execution order:

1. **Canonical sort** of merged diagnostics (see Determinism).
2. **FL040 reduce**. Sums per-TU write and loop-write counts per
   `(var, type)` and grades severity on the global aggregate (write
   pressure, not site count; see [rules.md](rules.md#fl040--centralized-mutable-global-state)).
3. **Cross-TU escape suppression**. Per-TU `EscapeSummary` maps are merged;
   diagnostics whose `type_name` shows no escape evidence in any TU are
   suppressed. Runs before dedup so all duplicate instances are reclassified
   consistently. Proven-tier findings and diagnostics without `type_name` are
   never suppressed.
4. **Deduplication**. Headers included by many TUs produce one finding per
   TU. Keys: `(ruleID, file, line)` for struct-level rules;
   `(ruleID, var, type)` for FL040 (the same global appears at different
   header paths); `(ruleID, functionName, line)` for function rules. The
   survivor is the **highest-confidence** instance (ties: better evidence
   tier, then shortest path → lexicographic → lowest line/column → content
   order). Escalation traces from all duplicates are merged, sorted, and
   annotated with the TU count. Because FL002 encodes write evidence into
   confidence, the TU that observes the writers decides the canonical
   verdict.
5. **Interaction synthesis (FL091)**, joins diagnostics sharing an entity
   key: `file:line`, `type:` + type name, or `fn:` + function. Eligible
   pairs/triples per the `InteractionEligibilityMatrix` produce compound
   findings; severity derives from the (post-demotion) parents. One compound
   per (template, participant set). Followed by the **FL092 precedent
   join** (see [rules.md](rules.md#fl092--unapplied-in-tree-mitigation)).
   The thread-role reduce and the FL002/FL090 disjoint-writer escalation
   run earlier, between cross-TU escape suppression and dedup, so every
   duplicate instance is escalated consistently before the canonical
   survivor is chosen.
6. **Precision budget**. Per-rule governance: max emissions per TU,
   confidence floors, severity caps.
7. **Calibration suppression**, with `--calibration-store`, findings whose
   10-dimension structural feature vector falls within Euclidean radius 0.25
   of a pattern with ≥3 experimentally refuted instances are suppressed.
   Safety rail: Critical/High findings at Proven tier are never suppressed. A
   store path that exists but cannot be parsed is a hard error (exit 3),
   scanning with silently disabled calibration would misreport.
8. **Header fingerprint (B001)**. Aggregates `FailedTU` error text; a header
   missing in ≥3 TUs becomes a single B001 diagnostic naming the header,
   converting systematic build breakage into one actionable finding.
9. **Filter and final sort**. Suppressed findings drop; output orders by
   severity (Critical first), then file, then line, with a total-order
   content tiebreaker.

## Evidence model

Every diagnostic carries four signals (see
[output-formats.md](output-formats.md)): severity (worst-case impact),
confidence ∈ [0,1] (belief the hazard is real here), evidence tier
(`proven` (layout-guaranteed; `likely`) strong structural signals;
`speculative`), and **mechanism claims**.

### Mechanism claims

A rule does not assert a hazard as an opaque verdict. It decomposes its
hardware argument into claims, each naming an effect, the precondition that
effect requires, whether that precondition was established, and the severity
it can support:

```cpp
struct MechanismClaim {
    std::string effect;        // what the hardware does
    std::string precondition;  // what must hold for it to happen
    bool        established;
    Severity    supports;
    bool        gating;
};
```

`Diagnostic::severitySupportedByClaims()` combines them, and the pipeline
clamps each finding's severity to the result. A finding cannot be graded
Critical on a mechanism whose precondition was never shown.

**Ordinary claims are alternatives; gating claims are conjuncts.** Any one
established mechanism can carry a finding, so ordinary claims combine with
`max`. A gating claim caps the result instead:

```
result = min( max(established ordinary claims), min(all gating claims) )
```

Hotness is the canonical gating claim: no mechanism costs anything in code
that never runs. This distinction is load-bearing rather than decorative, a
cap combined with `max` is a no-op, which is exactly what the first
implementation did.

`scan_e2e_test` gates the contract shut: every emitted finding must declare
its claims, and severity may never outrank an established one.

### Grading principles

- **Claims are downgraded to what the evidence supports.** Sub-line-aligned
  records cannot prove co-residency → `likely`, not `proven`. No observed
  writers in the TU → "structural evidence only," capped severity.
- **Mitigation intent is respected.** Explicit line alignment or pad-to-line
  layout caps FL001/FL002/FL090 at Medium with the reason stated. Compounds
  never outrank their mitigation-adjusted components.
- **A claim being constant is not a defect.** A rule's own entry condition is
  legitimately always true and supports only the floor grade. What matters is
  that the claim is *computed*, which a gate cannot verify, see
  `verify/claim_discrimination.py`.

## Determinism

Output is **byte-identical regardless of `--jobs` count or scheduling**. This
is a hard invariant with specific machinery behind it:

- Parallel AST analysis shards sources round-robin across **forked child
  processes** (not threads), hardware-level isolation from Clang's
  thread-unsafe globals. Children serialize diagnostics, `FailedTU`s, and
  `EscapeSummary` over a JSON IPC protocol; the parent merges after
  `waitpid()`.
- Merged diagnostics are sorted by the canonical key
  `(ruleID, file, line, column, functionName)` **before any order-dependent
  pass**. Key collisions (e.g., two TUs defining distinct same-line symbols
  via macro pasting. The jemalloc `je_`-prefix pattern) fall through to
  `diagnosticContentLess`, a total order over severity, confidence, tier,
  function, title, evidence, escalations, and mitigation. No comparison ends
  in "equal" for distinct content.
- Rules never cache per-TU state; `EscapeAnalysis` is constructed per TU.
  Heap-address reuse across TUs in forked children has caused real
  non-determinism; dependency injection is the fix, not discipline.
- Cross-TU aggregation is map/reduce (FL040 write counts, escape summaries):
  children emit facts, the parent computes verdicts. No per-TU partial
  verdicts.
- Locations are resolved via `getFileLoc()` so Clang `<scratch space>`
  token-paste artifacts map back to physical files.
- The per-shard memory cap derives from **total** system memory, not
  available. Available memory fluctuates with ambient load, so deriving from
  it would make output depend on what else the machine was doing, a safety
  valve must not breach the invariant it protects.

The only run-varying output field is `metadata.timestamp`.

### Shard accounting

A shard is accounted for in exactly one way: its IPC parsed, or every
translation unit it owned marked failed with a reason. Anything else converts
a lost shard into silently missing coverage that reads identically to a clean
scan.

Four paths could previously drop a shard while exiting 0, `fork()` failure,
a child whose IPC write failed, a signalled child whose partial IPC still
parsed, and unparseable IPC. All now report. Records are written one per TU
and flushed as each completes, so a shard that dies mid-way surrenders only
the translation unit it died on rather than everything it had finished.

`LSHAZ_FAULT_KILL_SHARD=<shard>[:<n>]` kills a shard deterministically,
before any TU, or after `n` of them, because the realistic trigger (the OOM
killer) cannot be summoned on demand, and a silent-failure guard that cannot
be made to fail is not a guard.

## Latency model

| Component | Assumption |
|---|---|
| Cache line | 64 bytes (`cache_line_bytes`) |
| L1/L2 | private per core |
| L3/LLC | shared |
| Coherence | MESI |
| Memory model | x86-64 TSO (`--target-arch arm64` switches ordering costs) |
| Page size | 4KB (`page_size`) |
| NUMA | first-touch placement |

The tool models line-level structural exposure; it does not simulate sets,
associativity, or cycle timing. Runtime impact claims are delegated to the
experiment pipeline.

## Experiment pipeline

`scan → hyp → exp → build/run → feedback → scan` closes the loop between
static findings and hardware measurements. See
[hypothesis-engine.md](hypothesis-engine.md) for the full contract. In brief:

- **`lshaz hyp`** maps each diagnostic to a falsifiable hypothesis: H0/H1,
  primary tail-latency metric, PMU counter groups partitioned to hardware
  limits, minimum detectable effect, and confound controls. All hazard
  classes have hypothesis templates.
- **`lshaz exp`** synthesizes self-contained experiment bundles: treatment
  and control kernels parameterized by the diagnostic's structural evidence,
  compiled as separate TUs into one binary dispatched by `--variant` (the
  compiler cannot optimize across the comparison boundary); a measurement
  harness with `lfence`-bracketed `rdtsc`; an `analyze` tool that bootstraps
  the percentile CI of the relative p99.9 effect; PMU collection scripts with
  guarded per-variant passes and teardown-on-failure; and `hypothesis.json`
  embedding the structural features calibration requires. 13 hazard classes
  have dedicated kernel generators; CentralizedDispatch, HazardAmplification,
  and SynthesizedInteraction emit editable stubs.
- **`lshaz feedback`** ingests binary sample files and the recorded
  environment (`results/env.json`), runs Welch's t-test, computes achieved
  power (two-sample z at the achieved sample sizes), and writes a verdict
  into the versioned calibration store (atomic temp+rename). Quality gates:
  labels below 0.60 quality demote to unlabeled; refutations require power
  ≥ 0.80; environment penalties for missing confound controls (turbo −0.15,
  governor −0.10, pinning −0.20). Bundles without structural features are
  refused.

Refuted patterns suppress structurally similar findings on subsequent scans
(post-processing step 7). PMU trace feedback (`--pmu-trace`, `--pmu-priors`)
provides a parallel ingestion path from production `perf stat` data with
Bayesian per-class priors.
