# Faultline — Rules Specification (v1)

## Rule Format

Each rule contains:

- Rule ID
- Title
- Severity
- Hardware Mechanism
- Detection Logic
- Escalation Conditions
- False Positives
- Suggested Mitigation
- Confidence Model

---

## STRUCTURAL CACHE RISKS

### FL001 — Cache Line Spanning Struct

**Severity:** High (Critical if shared-write)

**Hardware Mechanism:**  
L1/L2 cache line footprint expansion. Increased eviction probability. Higher coherence traffic under multi-core writes.

**Detection Logic:**

- Compute `sizeof(T)`
- If `sizeof(T) &gt; 64` bytes
- AND type appears in hot path
- → Flag

**Escalation:**

- If `sizeof(T) &gt; 128` bytes → escalate
- If struct contains atomic or mutable fields AND escapes thread boundary → Critical

**False Positives:**

- Read-only large structs
- Thread-local instances

**Mitigation:**

- Convert AoS → SoA
- Split hot/cold fields
- `alignas(64)` where justified

**Confidence:**  
High for hot-path types.

---

### FL002 — False Sharing Candidate

**Severity:** Critical

**Hardware Mechanism:**  
MESI invalidation ping-pong across cores due to shared cache line writes.

**Detection Logic:**

- Struct contains 2+ mutable fields
- Struct size &lt; 64 bytes
- Fields likely accessed by different threads
- Type escapes thread boundary

**Escalation:**

- Presence of `std::atomic` fields → escalate
- Used in tight loop → escalate

**False Positives:**

- Struct always accessed under same-thread confinement

**Mitigation:**

- Separate fields
- Pad to 64B
- Use per-thread storage

**Confidence:**  
Medium-high (depends on escape analysis quality)

---

## SYNCHRONIZATION RISKS

### FL010 — Overly Strong Atomic Ordering

**Severity:** High

**Hardware Mechanism:**  
Full memory fences emitted for `memory_order_seq_cst` cause pipeline serialization and store buffer drains.

**Detection Logic:**

- `std::atomic` operations
- `memory_order_seq_cst` used
- In hot path

**Escalation:**

- Inside tight loop
- Multiple atomics in same function

**False Positives:**

- Correctness requires seq_cst

**Mitigation:**

- Consider `memory_order_release`/`acquire`
- Use relaxed where safe

**Confidence:**  
High

---

### FL011 — Atomic Contention Hotspot

**Severity:** Critical

**Hardware Mechanism:**  
Cache line ownership thrashing. Store buffer pressure. Cross-core invalidation storms.

**Detection Logic:**

- Atomic variable
- Written in hot path
- Escapes thread boundary
- Used by multiple threads (heuristic)

**Escalation:**

- Multiple atomic writes per iteration
- Adjacent atomics in same struct

**False Positives:**

- Single-writer multi-reader pattern with mostly reads

**Mitigation:**

- Shard per-core
- Use batching
- Redesign ownership model

**Confidence:**  
Medium (static contention inference is imperfect)

---

### FL012 — Lock in Hot Path

**Severity:** Critical

**Hardware Mechanism:**  
Lock convoy. Kernel transition (if blocking). Cache line contention.

**Detection Logic:**

- `std::mutex` / spinlock detected
- Lock acquired in hot path

**Escalation:**

- Nested locks
- Lock inside loop

**False Positives:**

- Extremely low contention scenarios

**Mitigation:**

- Lock-free design
- Single-writer model
- Partition state

**Confidence:**  
High

---

## MEMORY ALLOCATION RISKS

### FL020 — Heap Allocation in Hot Path

**Severity:** Critical

**Hardware Mechanism:**  
Allocator lock contention. TLB pressure. Page faults. Fragmentation.

**Detection Logic:**

- `new`/`delete`
- `std::vector` growth
- `std::function`
- `std::shared_ptr`
- `malloc` in IR
- In hot path

**Escalation:**

- Allocation inside loop
- Allocation size &gt; 256 bytes

**False Positives:**

- Custom allocator known to be lock-free (configurable whitelist)

**Mitigation:**

- Preallocate
- Use arena/slab
- Object pools

**Confidence:**  
High (IR-backed detection)

---

### FL021 — Large Stack Frame

**Severity:** Medium (High in deep call chains)

**Hardware Mechanism:**  
TLB pressure. L1 data cache pressure.

**Detection Logic:**

- Stack frame &gt; configurable threshold (default 2KB)
- In hot path

**Escalation:**

- Deep call stack (estimated)
- Recursive patterns

**False Positives:**

- Cold-path function

**Mitigation:**

- Move large arrays to heap or static region
- Reduce local buffers

**Confidence:**  
High

---

## DISPATCH RISKS

### FL030 — Virtual Dispatch in Hot Path

**Severity:** High

**Hardware Mechanism:**  
Indirect branch misprediction. Pipeline flush. BTB pressure.

**Detection Logic:**

- Virtual function call
- In hot path
- Not devirtualized in IR

**Escalation:**

- Inside tight loop

**False Positives:**

- Monomorphic usage reliably predicted

**Mitigation:**

- CRTP
- Variant + visitation
- Function pointers with known targets

**Confidence:**  
High (IR-confirmed indirect call)

---

### FL031 — std::function in Hot Path

**Severity:** High

**Hardware Mechanism:**  
Possible heap allocation. Indirect dispatch. Poor inlining.

**Detection Logic:**

- `std::function` type detected
- Invocation in hot path

**Escalation:**

- Constructed dynamically
- Captures large objects

**False Positives:**

- Small object optimization case

**Mitigation:**

- Template callable
- Auto lambda
- Function pointer

**Confidence:**  
High

---

## STRUCTURAL DESIGN RISKS

### FL040 — Centralized Mutable Global State

**Severity:** High (Critical if multi-writer)

**Hardware Mechanism:**  
NUMA remote memory access. Cache line contention. Scalability collapse.

**Detection Logic:**

- Global/static mutable object
- Referenced in hot path
- Escapes thread-local confinement

**Escalation:**

- Contains atomics
- Modified by multiple functions

**False Positives:**

- Initialized once, read-only thereafter

**Mitigation:**

- Partition per thread/core
- Inject via context

**Confidence:**  
Medium

---

### FL041 — Contended Queue Pattern

**Severity:** High

**Hardware Mechanism:**  
Head/tail cache line bouncing. Atomic contention.

**Detection Logic:**

- Shared queue type
- Multiple producers/consumers
- Atomic head/tail indices

**Escalation:**

- No padding between head/tail
- Tight loop enqueue/dequeue

**False Positives:**

- SPSC queue properly partitioned

**Mitigation:**

- Pad head/tail to 64B
- Use per-core queues

**Confidence:**  
Medium

---

## BRANCHING RISKS

### FL050 — Deep Conditional Tree in Hot Path

**Severity:** Medium (High if data-dependent)

**Hardware Mechanism:**  
Branch misprediction. I-cache pressure.

**Detection Logic:**

- N nested conditionals (configurable)
- In hot path

**Escalation:**

- Large switch on non-constexpr values
- Polymorphic dispatch tree

**False Positives:**

- Predictable branches

**Mitigation:**

- Table-driven dispatch
- Flatten logic
- Precompute decision trees

**Confidence:**  
Low-Medium (entropy cannot be statically proven)

---

## INTERACTION RULES

### FL090 — Hazard Amplification

**Severity:** Critical

**Hardware Mechanism:**  
Multiple interacting latency multipliers.

**Detection Logic:**

- Struct &gt; 128B
- Contains atomic
- Shared across threads
- Used in loop

→ Escalate to Critical with composite explanation.

**Confidence:**  
High when all signals present.

---

## Rule Count (v1)

**Current:** 14 primary rules  
**Target for v1:** 25–35 high-confidence rules

No filler rules allowed.

Each must map to:

- Cache
- Coherence
- Store buffer
- TLB
- Branch predictor
- NUMA
- Allocator

If a rule cannot be tied to one of these mechanisms, it does not belong.