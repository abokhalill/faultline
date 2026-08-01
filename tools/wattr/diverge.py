#!/usr/bin/env python3
"""Confront lshaz's sharing predictions with what the program actually did.

lshaz reasons about types; sharing is a property of objects. The join is
exact rather than heuristic because the static side now emits the linker
names of each type's global instances (`global_instances`), which is the
key a write-attribution trace reports under.

  CONFIRMED   predicted, and >1 thread wrote the line
  REFUTED     predicted, but the line is single-threaded at runtime
  MISSED      runtime saw false sharing, no finding named the object
  UNOBSERVED  predicted, but the object was never written in this run

UNOBSERVED is not a false positive: it means the workload did not exercise
it. Reporting the two separately is the difference between measuring the
analyser and measuring the workload.

  diverge.py <findings.json> <trace.tsv> <binary>
"""
import json
import subprocess
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from wattr_report import load_symbols, resolve  # noqa: E402


def observed_by_symbol(trace, binary):
    base, rows = 0, []
    with open(trace) as fh:
        for ln in fh:
            if ln.startswith("# base"):
                base = int(ln.split()[-1], 16)
            elif not ln.startswith("#"):
                a, off, mask, n, w = ln.split("\t")
                rows.append((int(a, 16), int(off), int(mask), int(w)))

    syms = load_symbols(binary)
    per_sym = {}
    lines = {}
    for line, off, mask, w in rows:
        lines.setdefault(line, {})[off] = (mask, w)
    for line, granules in lines.items():
        union, multi = 0, False
        names = []
        for off, (m, _w) in sorted(granules.items()):
            union |= m
            if bin(m).count("1") > 1:
                multi = True
            n, _o = resolve(syms, (line + off) - base)
            if n and n not in names:
                names.append(n)
        threads = bin(union).count("1")
        verdict = ("TRUE_SHARING" if multi and threads > 1 else
                   "FALSE_SHARING" if threads > 1 else "PRIVATE")
        for n in names:
            prev = per_sym.get(n)
            # Keep the strongest observation for a symbol.
            rank = {"PRIVATE": 0, "FALSE_SHARING": 1, "TRUE_SHARING": 2}
            if not prev or rank[verdict] > rank[prev[0]]:
                per_sym[n] = (verdict, threads,
                              sum(w for _m, w in granules.values()))
    return per_sym


def main():
    if len(sys.argv) != 4:
        print(__doc__.strip())
        return 2
    findings, trace, binary = sys.argv[1:4]

    diags = json.load(open(findings))["diagnostics"]
    obs = observed_by_symbol(trace, binary)

    rows, claimed = [], set()
    for d in diags:
        if d.get("ruleID") not in ("FL002", "FL041") or d.get("suppressed"):
            continue
        ev = d.get("structuralEvidence", {})
        names = [n for n in ev.get("global_instances", "").split(";") if n]
        ty = ev.get("type_name", "?")
        if not names:
            # No global instance: the object is per-request or per-thread and
            # a symbol-keyed trace cannot see it. Say so rather than score it.
            rows.append(("NO-GLOBAL-INSTANCE", ty, "-", d["severity"], 0))
            continue
        for n in names:
            claimed.add(n)
            if n not in obs:
                rows.append(("UNOBSERVED", ty, n, d["severity"], 0))
            else:
                v, threads, w = obs[n]
                verdict = "REFUTED" if v == "PRIVATE" else "CONFIRMED"
                rows.append((f"{verdict} ({v})", ty, n, d["severity"], w))

    for n, (v, threads, w) in sorted(obs.items()):
        if v == "FALSE_SHARING" and n not in claimed:
            rows.append(("MISSED", "-", n, "-", w))

    print(f"{'outcome':28} {'type':26} {'symbol':22} {'sev':9} {'writes':>10}")
    for outcome, ty, sym, sev, w in sorted(rows):
        print(f"{outcome:28} {ty[:26]:26} {sym[:22]:22} {sev:9} {w:10}")

    n_conf = sum(1 for r in rows if r[0].startswith("CONFIRMED"))
    n_ref = sum(1 for r in rows if r[0].startswith("REFUTED"))
    n_miss = sum(1 for r in rows if r[0] == "MISSED")
    print(f"\nconfirmed={n_conf} refuted={n_ref} missed={n_miss}")
    if n_conf + n_ref:
        print(f"observed precision = {n_conf}/{n_conf + n_ref} = "
              f"{n_conf / (n_conf + n_ref):.2f}  (this workload only)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
