# Faultline Validation Harness

Ground-truth validation for Faultline's microarchitectural claims.

## Tiers

### Tier 1: Corpus-Scale Regression
Runs faultline against real open-source C++ codebases. Asserts:
- Zero crashes (exit code != 2)
- Deterministic output (two runs produce identical diagnostics)
- Diagnostic location validity (referenced lines exist in source)
- Evidence field consistency (sizeof matches clang -print-record-layout)
- Distribution sanity (no single rule > 50% of all diagnostics)

### Tier 2: Ground Truth Microbenchmarks
Per-rule paired benchmarks (hazardous vs. fixed) with perf counter validation:
- Faultline must flag hazardous variant, must NOT flag fixed variant
- perf counters must show statistically significant hardware effect
- Effect size validated via paired t-test (α=0.01, power≥0.9)

## Usage

```bash
# Full validation (both tiers)
./validation/run.sh

# Tier 1 only (no perf counters needed)
./validation/run.sh --tier1

# Tier 2 only (requires perf_event_open access)
./validation/run.sh --tier2

# Tier 2 with specific rule
./validation/run.sh --tier2 --rule FL002
```

## Requirements
- Built faultline binary at `build/faultline`
- clang++ (for corpus compilation and benchmark builds)
- perf (for Tier 2 counter validation)
- Python 3.8+ with scipy (for statistical analysis)
- `perf_event_paranoid <= 1` or CAP_PERFMON for hardware counters
