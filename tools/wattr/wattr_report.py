#!/usr/bin/env python3
"""Turn a write-attribution trace into a per-symbol sharing verdict.

The distinction the trace exists to make, and that no static analysis can:

  FALSE_SHARING   one line, DIFFERENT granules, different threads.
                  Nobody races; the line still ping-pongs. This is the claim
                  FL002 makes, and a race detector reports none of it.
  TRUE_SHARING    one granule, several threads. Contention on the field
                  itself -- real, but a different rule's business.
  PRIVATE         one thread. Whatever the layout says, it costs nothing.

  wattr_report.py <trace.tsv> <binary> [--json]
"""
import collections
import json
import subprocess
import sys


LINE = 64


def load_symbols(binary):
    """Link-time (addr, size, name) for data symbols, sorted by address."""
    out = subprocess.run(["nm", "-S", "--defined-only", binary],
                         capture_output=True, text=True).stdout
    syms = []
    for ln in out.splitlines():
        parts = ln.split()
        # "addr size type name" -- size only present with -S
        if len(parts) == 4 and parts[2].upper() in ("B", "D", "G", "R", "V"):
            try:
                syms.append((int(parts[0], 16), int(parts[1], 16), parts[3]))
            except ValueError:
                continue
    syms.sort()
    return syms


def resolve(syms, addr):
    lo, hi = 0, len(syms) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            best = syms[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    if best and best[0] <= addr < best[0] + max(best[1], 1):
        return best[2], addr - best[0]
    return None, 0


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip())
        return 2
    trace, binary = sys.argv[1], sys.argv[2]

    base = 0
    rows = []
    allocs = []
    with open(trace) as fh:
        for ln in fh:
            if ln.startswith("# base"):
                base = int(ln.split()[-1], 16)
                continue
            if ln.startswith("@\t"):
                _, b, sz, ra = ln.split("\t")
                allocs.append((int(b, 16), int(sz), ra.strip()))
                continue
            if ln.startswith("#"):
                continue
            line, off, mask, ntids, writes = ln.split("\t")
            rows.append((int(line, 16), int(off), int(mask), int(ntids),
                         int(writes)))
    allocs.sort()

    syms = load_symbols(binary)
    # Group by cache line, carrying each granule's writer set.
    lines = collections.defaultdict(dict)
    for line, off, mask, _n, writes in rows:
        lines[line][off] = (mask, writes)

    verdicts = []
    for line, granules in sorted(lines.items()):
        # Resolve each granule, not the line: a line begins before the first
        # object on it, and which symbols share the line IS the finding.
        # Static addresses need the bias removed; heap and stack do not
        # resolve, which is itself informative -- a per-request object has
        # no symbol, and that is the handoff case.
        names = []
        for off in sorted(granules):
            n, _o = resolve(syms, (line + off) - base)
            if n and n not in names:
                names.append(n)
        if not names:
            # Not a global: attribute to the allocation(s) covering the line.
            # Several matches mean the address was recycled, so the object it
            # belonged to at write time is unknowable without timestamps --
            # reported as reuse rather than guessed at.
            sites = set()
            for b, sz, ra in allocs:
                if b > line + LINE:
                    break
                if b <= line < b + sz or b < line + LINE <= b + sz:
                    sites.add(ra)
            if len(sites) == 1:
                names = [f"heap@{sites.pop()}"]
            elif len(sites) > 1:
                names = [f"heap@POOL_REUSE({len(sites)} sites)"]
        name = "+".join(names) if names else None
        off = 0
        union = 0
        for m, _ in granules.values():
            union |= m
        multi_granule = any(bin(m).count("1") > 1 for m, _ in granules.values())
        distinct = bin(union).count("1")

        if distinct <= 1:
            verdict = "PRIVATE"
        elif len(granules) > 1 and not multi_granule:
            # Several granules, each single-writer, different threads overall.
            verdict = "FALSE_SHARING"
        elif multi_granule:
            verdict = "TRUE_SHARING"
        else:
            verdict = "PRIVATE"
        verdicts.append({
            "line": hex(line),
            "symbol": name or "<anon>",
            "symbol_offset": off,
            "granules": len(granules),
            "threads": distinct,
            "writes": sum(w for _, w in granules.values()),
            "verdict": verdict,
        })

    if "--json" in sys.argv:
        json.dump(verdicts, sys.stdout, indent=2)
        return 0

    counts = collections.Counter(v["verdict"] for v in verdicts)
    print(f"{len(verdicts)} line(s): " +
          ", ".join(f"{k}={v}" for k, v in sorted(counts.items())) + "\n")
    print(f"{'verdict':16} {'symbol':32} {'gran':>5} {'thr':>4} {'writes':>10}")
    for v in sorted(verdicts, key=lambda x: -x["writes"]):
        if v["verdict"] == "PRIVATE":
            continue
        print(f"{v['verdict']:16} {v['symbol'][:32]:32} "
              f"{v['granules']:5} {v['threads']:4} {v['writes']:10}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
