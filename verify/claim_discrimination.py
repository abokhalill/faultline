#!/usr/bin/env python3
"""Report how much work each mechanism claim is doing.

The e2e gate checks that severity never outranks an established claim. It
cannot check that a claim is *computed* — an `established` hardcoded true
would pass silently, and the contract would be decorative.

This reports, per claim, whether `established` or `supports` varies across a
real corpus. Constant is not automatically wrong: a rule's entry condition
is legitimately always true and supports only the floor grade, and a claim
can be genuinely computed with no instances in a given corpus (virtual
dispatch in C, TAS spins where none are written). So this prints rather than
fails — it is a reading aid for the next person auditing a rule, not a gate.

    ./verify/claim_discrimination.py <scan.json>
"""
import collections
import json
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip())
        return 2
    diags = json.load(open(sys.argv[1]))["diagnostics"]

    agg = collections.defaultdict(
        lambda: {"est": set(), "sup": set(), "n": 0})
    undeclared = collections.Counter()
    for d in diags:
        claims = d.get("mechanismClaims")
        if not claims:
            undeclared[d["ruleID"]] += 1
            continue
        for c in claims:
            k = (d["ruleID"], c["effect"])
            agg[k]["est"].add(c["established"])
            agg[k]["sup"].add(c["supports"])
            agg[k]["n"] += 1

    live, const = [], []
    for k, v in sorted(agg.items()):
        (live if len(v["est"]) > 1 or len(v["sup"]) > 1 else const).append((k, v))

    print(f"{len(live)} claim(s) discriminate, {len(const)} constant "
          f"across {len(diags)} finding(s)\n")
    for (rule, effect), v in live:
        print(f"  VARIES   {rule:7} est={sorted(v['est'])} "
              f"sup={sorted(v['sup'])}  {effect[:52]}")
    print()
    for (rule, effect), v in const:
        print(f"  constant {rule:7} est={str(next(iter(v['est']))):5} "
              f"sup={next(iter(v['sup'])):13} n={v['n']:5}  {effect[:52]}")

    if undeclared:
        print(f"\nUNDECLARED — these skipped the contract: {dict(undeclared)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
