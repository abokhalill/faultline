#!/usr/bin/env python3
"""
Tier 2 Statistical Analysis: validates that benchmark timing deltas
between hazardous and fixed variants are statistically significant.

Parses benchmark output files and runs paired t-tests.
"""

import re
import sys
import os
from dataclasses import dataclass
from pathlib import Path

@dataclass
class TrialResult:
    label: str
    ns: int
    ns_per_op: float

def parse_benchmark_output(filepath: str) -> dict[str, list[TrialResult]]:
    """Parse benchmark output into per-label trial results."""
    results: dict[str, list[TrialResult]] = {}
    with open(filepath) as f:
        for line in f:
            # Match: "label            123456 ns  ( 1.23 ns/op)"
            # Labels can contain spaces, parens, etc. — match up to the number.
            m = re.match(r'^(.+?)\s{2,}(-?\d+)\s+ns\s+\(\s*(-?[\d.]+)\s+ns/(?:op|elem)', line.strip())
            if m:
                label = m.group(1).strip()
                if label == 'warmup':
                    continue
                ns = int(m.group(2))
                ns_per_op = float(m.group(3))
                results.setdefault(label, []).append(
                    TrialResult(label=label, ns=ns, ns_per_op=ns_per_op)
                )
    return results

def paired_ttest(a: list[float], b: list[float], alpha: float = 0.01):
    """Paired t-test. Returns (t_stat, p_value, significant, effect_size_d)."""
    try:
        from scipy import stats
        import numpy as np
    except ImportError:
        return None, None, None, None

    a_arr = np.array(a)
    b_arr = np.array(b)

    if len(a_arr) < 2 or len(b_arr) < 2:
        return None, None, None, None

    t_stat, p_value = stats.ttest_rel(a_arr, b_arr)

    # Cohen's d for paired samples
    diff = a_arr - b_arr
    d = diff.mean() / diff.std(ddof=1) if diff.std(ddof=1) > 0 else 0.0

    return t_stat, p_value, p_value < alpha, d

# Rule definitions: (rule_id, hazardous_label, fixed_label, expected_direction)
# expected_direction: "higher" means hazardous should be slower (higher ns)
RULES = [
    ("FL001", "large (192B)", "split (32B)", "higher"),
    ("FL002", "hazardous", "fixed", "higher"),
    ("FL010", "seq_cst store", "release store", "higher"),
    ("FL012", "mutex", "atomic", "higher"),
    ("FL020", "heap alloc", "preallocated", "higher"),
    ("FL030", "virtual", "crtp", "higher"),
    ("FL041", "unpadded", "padded", "higher"),
]

def analyze_rule(rule_id: str, results_dir: str) -> tuple[bool, str]:
    """Analyze a single rule's benchmark results. Returns (passed, message)."""
    bench_file = os.path.join(results_dir, f"{rule_id}_bench.txt")
    if not os.path.exists(bench_file):
        return None, f"No benchmark output for {rule_id}"

    data = parse_benchmark_output(bench_file)
    if not data:
        return None, f"Could not parse benchmark output for {rule_id}"

    rule_def = next((r for r in RULES if r[0] == rule_id), None)
    if not rule_def:
        return None, f"No rule definition for {rule_id}"

    _, haz_label, fix_label, direction = rule_def

    haz_results = data.get(haz_label, [])
    fix_results = data.get(fix_label, [])

    if len(haz_results) < 3 or len(fix_results) < 3:
        return None, f"Insufficient trials (need ≥3, got haz={len(haz_results)}, fix={len(fix_results)})"

    haz_ns = [r.ns for r in haz_results]
    fix_ns = [r.ns for r in fix_results]

    n = min(len(haz_ns), len(fix_ns))
    haz_ns = haz_ns[:n]
    fix_ns = fix_ns[:n]

    t_stat, p_value, significant, effect_d = paired_ttest(haz_ns, fix_ns)

    if t_stat is None:
        haz_mean = sum(haz_ns) / len(haz_ns)
        fix_mean = sum(fix_ns) / len(fix_ns)
        ratio = haz_mean / fix_mean if fix_mean > 0 else 0
        return None, (f"scipy unavailable — manual check: "
                      f"haz={haz_mean:.0f}ns, fix={fix_mean:.0f}ns, ratio={ratio:.2f}x")

    haz_mean = sum(haz_ns) / len(haz_ns)
    fix_mean = sum(fix_ns) / len(fix_ns)
    ratio = haz_mean / fix_mean if fix_mean > 0 else 0

    msg = (f"haz={haz_mean:.0f}ns fix={fix_mean:.0f}ns ratio={ratio:.2f}x "
           f"t={t_stat:.3f} p={p_value:.6f} d={effect_d:.2f} "
           f"{'SIGNIFICANT' if significant else 'NOT significant'} (α=0.01)")

    if direction == "higher":
        passed = significant and haz_mean > fix_mean
    else:
        passed = significant and haz_mean < fix_mean

    return passed, msg

def main():
    results_dir = sys.argv[1] if len(sys.argv) > 1 else "validation/tier2/results"

    print("Faultline Tier 2: Statistical Analysis")
    print("=" * 50)
    print()

    total_pass = 0
    total_fail = 0
    total_skip = 0

    for rule_id, _, _, _ in RULES:
        passed, msg = analyze_rule(rule_id, results_dir)
        if passed is None:
            print(f"  [SKIP] {rule_id}: {msg}")
            total_skip += 1
        elif passed:
            print(f"  [PASS] {rule_id}: {msg}")
            total_pass += 1
        else:
            print(f"  [FAIL] {rule_id}: {msg}")
            total_fail += 1

    print()
    print("=" * 50)
    print(f"  PASS: {total_pass}  FAIL: {total_fail}  SKIP: {total_skip}")

    if total_fail > 0:
        print(f"\nRESULT: FAIL ({total_fail} rule(s) failed ground truth)")
        sys.exit(1)
    elif total_pass > 0:
        print(f"\nRESULT: PASS (all {total_pass} validated rule(s) confirmed)")
    else:
        print(f"\nRESULT: SKIP (no rules validated)")

if __name__ == "__main__":
    main()
