#!/usr/bin/env bash
# The inverse of everything else in this directory.
#
# Every other harness asks: we flagged this, does it cost anything? That is
# precision. This asks the question we have never asked — here is real
# coherence cost in a running binary, did lshaz find it? Without it the
# false-negative rate is unknown, and that is the number the project is
# actually graded on.
#
#   recall.sh <binary> <lshaz-json> [seconds]
#
# Emits every source line with measured HITM, joined against the findings, so
# each row is either a hit or a miss. Rows we missed are the output that
# matters; they are candidate rules we do not have.
set -u

BIN=${1:?binary under load}
FINDINGS=${2:?lshaz --format json output}
SECS=${3:-10}

command -v perf >/dev/null || { echo "perf missing"; exit 2; }
[ -r "$FINDINGS" ] || { echo "cannot read $FINDINGS"; exit 2; }

PID=$(pgrep -x "$(basename "$BIN")" | head -1)
[ -n "$PID" ] || { echo "no running process named $(basename "$BIN")"; exit 2; }
echo "sampling pid $PID for ${SECS}s"

DATA=$(mktemp /tmp/recall.XXXX.data)
perf c2c record -a -o "$DATA" -- sleep "$SECS" >/dev/null 2>&1

# --stdio report, full symbol detail. c2c's own summary is percentages; we
# need per-line attribution, so the shared-cacheline table is what we parse.
perf c2c report -i "$DATA" --stdio --full-symbols -g 2>/dev/null > "$DATA.txt"

TOTAL=$(sed -n 's/.*Load Local HITM *: *\([0-9]*\).*/\1/p' "$DATA.txt" | head -1)
REMOTE=$(sed -n 's/.*Load Remote HITM *: *\([0-9]*\).*/\1/p' "$DATA.txt" | head -1)
echo "HITM observed: ${TOTAL:-0} local, ${REMOTE:-0} remote"
if [ "$(( ${TOTAL:-0} + ${REMOTE:-0} ))" -eq 0 ]; then
    # Not a pass. A workload with no coherence cost measures nothing about
    # recall; it means this workload cannot answer the question.
    echo "NO HITM UNDER THIS WORKLOAD — inconclusive, not a clean bill"
    exit 3
fi

python3 - "$DATA.txt" "$FINDINGS" "$BIN" <<'PY'
import json, re, subprocess, sys, collections
report, findings_path, binary = sys.argv[1], sys.argv[2], sys.argv[3]
target = binary.split('/')[-1]

# The Pareto section alternates a cacheline header carrying absolute HITM
# counts with contributing rows carrying percentages of it. Attribute each row
# its share, so a symbol's number is real HITM rather than a row count.
line_hdr = re.compile(r'^\s+\d+\s+(\d+)\s+(\d+)\s+\d+\s+\d+\s+\d+\s+0x[0-9a-f]+\s*$')
hot = collections.Counter()
where = {}
cur = 0
for ln in open(report, errors='ignore'):
    m = line_hdr.match(ln.rstrip())
    if m:
        cur = int(m.group(1)) + int(m.group(2))   # remote + local
        continue
    if cur == 0 or '%' not in ln:
        continue
    parts = ln.split()
    # Symbol sits directly after the [.]/[k] dso marker; object and
    # source:line follow it.
    try:
        i = next(j for j, p in enumerate(parts) if p in ('[.]', '[k]'))
    except StopIteration:
        continue
    if i + 1 >= len(parts):
        continue
    # Userspace only, and only the binary under test. Kernel coherence traffic
    # dominates every server workload we have measured, and lshaz neither
    # analyses the kernel nor claims to — scoring against it measures nothing.
    if parts[i] != '[.]':
        continue
    obj = parts[i + 2] if i + 2 < len(parts) else ''
    if target and obj != target:
        continue
    sym = parts[i + 1]
    pct = [p for p in parts[:i] if p.endswith('%')]
    if len(pct) < 2:
        continue
    share = (float(pct[0].rstrip('%')) + float(pct[1].rstrip('%'))) / 100.0
    hot[sym] += int(cur * share)
    if i + 3 < len(parts):
        where.setdefault(sym, parts[i + 3])

d = json.load(open(findings_path))
found = d.get('findings', d.get('diagnostics', []))
flagged = set()
for f in found:
    fn = f.get('functionName') or ''
    if fn:
        flagged.add(fn.split('::')[-1])
    for v in f.get('structuralEvidence', {}).values():
        if isinstance(v, str):
            flagged.add(v.split('::')[-1])

if not hot:
    print(f"\nNo HITM attributable to '{target}' — all of it is kernel or other objects.")
    print("Recall is UNANSWERABLE for this workload: there is no userspace")
    print("coherence cost to have found or missed. That is a result about the")
    print("workload, not a score for the analyser.")
    sys.exit(3)

print(f"\n{'symbol':<34} {'hitm':>8}  {'site':<20} verdict")
print('-' * 76)
hits = misses = 0
hit_w = miss_w = 0
for sym, n in hot.most_common(40):
    base = sym.split('+')[0].split('@')[0]
    # Exact match only. Substring matching scored kfree as FOUND because a
    # finding ended in "free", which inflates recall with nonsense.
    ok = base in flagged
    if ok: hits += 1;  hit_w  += n
    else:  misses += 1; miss_w += n
    print(f"{base:<34} {n:>8}  {where.get(sym,'?'):<20} {'FOUND' if ok else 'MISSED'}")

tot = hits + misses
if tot:
    # Weighted is the honest figure: missing one symbol that carries most of
    # the coherence traffic is not the same as missing a long tail.
    print(f"\nrecall by symbol : {hits}/{tot} = {100*hits/tot:.0f}%")
    if hit_w + miss_w:
        print(f"recall by HITM   : {hit_w}/{hit_w+miss_w} = "
              f"{100*hit_w/(hit_w+miss_w):.0f}%")
    print("MISSED rows are the output that matters: measured cost, no rule.")
PY
