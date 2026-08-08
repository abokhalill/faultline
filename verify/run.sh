#!/usr/bin/env bash
# internal regression harness — intentionally untracked, never committed.
# each case is the live repro behind a specific fix; a FAIL here means
# that commit's bug is back. run before every commit, after the suites.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LSHAZ="$ROOT/build/lshaz"
CASES="$ROOT/verify/cases"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0; fail=0
ok()  { echo "PASS  $1"; pass=$((pass+1)); }
bad() { echo "FAIL  $1 — $2"; fail=$((fail+1)); }

# scan one file, print "RULE:line:Severity" sorted
lines() { # <file> <std> [extra scan args...]
  local f="$1" std="$2"; shift 2
  "$LSHAZ" scan "$f" --no-ir -f json "$@" -- -std="$std" 2>/dev/null | python3 -c '
import json,sys
for d in json.load(sys.stdin)["diagnostics"]:
    print("%s:%d:%s" % (d["ruleID"], d["location"]["line"], d["severity"]))' | sort
}

expect_eq() { # <name> <actual> <expected>
  if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got [$2] want [$3]"; fi
}
expect_has() { # <name> <haystack> <needle>
  case "$2" in *"$3"*) ok "$1";; *) bad "$1" "missing [$3]";; esac
}

echo "== suites =="
for t in analysis_ground_truth_test output_contract_test pipeline_unit_test scan_e2e_test; do
  if "$ROOT/build/$t" >/dev/null 2>&1; then ok "suite:$t"; else bad "suite:$t" "nonzero exit"; fi
done

echo "== regressions =="

# 1e2ac0a: constant stores are seq_cst
expect_eq fl010_cxx_orders \
  "$(lines "$CASES/fl010_cxx_orders.cpp" c++20 --rule FL010 | tr '\n' ' ')" \
  "FL010:11:High FL010:12:High FL010:16:High FL010:22:High "

# 43ddaba: C atomic forms, positives and negatives
# x86-64: only the seq_cst STORE costs anything (XCHG vs plain MOV). A load
# is already a MOV and an RMW is LOCK-prefixed at every ordering, so neither
# can be weakened into different machine code.
expect_eq fl010_c_atomics \
  "$(lines "$CASES/fl010_c_atomics.c" c11 --rule FL010 | tr '\n' ' ')" \
  "FL010:14:High FL010:15:High FL010:18:High "
# arm64: LDAR and the LDAXR/STLXR pair are genuinely costlier than relaxed,
# so the RMWs at 13 and 19 are real there. Also keeps C11/GNU RMW forms
# under test, which is what this fixture was added for.
expect_eq fl010_c_atomics_arm \
  "$(lines "$CASES/fl010_c_atomics.c" c11 --rule FL010 --target-arch arm64 | tr '\n' ' ')" \
  "FL010:13:High FL010:14:Critical FL010:15:Critical FL010:18:Critical FL010:19:High "
expect_has fl011_c_owner \
  "$("$LSHAZ" scan "$CASES/fl010_c_atomics.c" --rule FL011 --no-ir -f json -- -std=c11 2>/dev/null)" \
  '"type_name": "counters"'

# 9c1a04f: transitive hotness through prototype
expect_has hotprop "$(lines "$CASES/hotprop.cpp" c++20 --rule FL010)" "FL010:11:"

# 1990d1a: FAM struct fully analyzed
fam="$(lines "$CASES/fam_ring.c" c11)"
expect_has fam_fl001 "$fam" "FL001:4:"
expect_has fam_fl002 "$fam" "FL002:4:"
expect_has fam_fl041 "$fam" "FL041:4:"

# d9003aa: namespaced global writes counted
nsjson="$("$LSHAZ" scan "$CASES/ns_writes.cpp" --rule FL040 --no-ir -f json -- -std=c++20 2>/dev/null)"
# namespaced globals stay visible; severity follows whether the writers can
# actually run concurrently, not the assumption that they do.
expect_has ns_writes_assumed "$(lines "$CASES/ns_writes.cpp" c++20 --rule FL040)" "FL040:10:Medium"
expect_has ns_writes_evidenced "$(lines "$CASES/ns_writes.cpp" c++20 --rule FL040)" "FL040:16:High"
expect_has ns_writes_count "$nsjson" '"global_write_count": "3"'

# 947ceeb: FL091 entity joins (exactly two compounds)
n91="$(lines "$CASES/ix_compound.cpp" c++20 | grep -c '^FL091:')"
expect_eq fl091_count "$n91" "2"

# d52b726 + 904777a: deliberate layout demotes; unmitigated stays Critical
mit="$(lines "$CASES/mitigated.c" c11 --rule FL002)"
expect_has mitigated_aligned "$mit" "FL002:6:Medium"
expect_has mitigated_padded  "$mit" "FL002:13:Medium"
expect_has mitigated_control "$mit" "FL002:20:Critical"

# FL090 follows the same demotion contract as its components
amp="$(lines "$CASES/mitigated.c" c11 --rule FL090)"
expect_has fl090_mitigated "$amp" "FL090:26:Medium"
expect_has fl090_control   "$amp" "FL090:35:Critical"
# a compound needs a sharing route independent of "contains an atomic".
# 79 has the same geometry as 35 and is written only from ordinary
# functions, so it must not compound.
expect_eq fl090_unshared "$(echo "$amp" | tr '\n' ' ')" \
  "FL090:26:Medium FL090:35:Critical "

# pair co-residability: >64B-separated ATOMICS are not a pair under any shift.
# That is the assertion, and it still holds -- `separated` reports
# atomic_pairs_same_line=0 and never reaches Critical.
#
# It does now report Medium for mutable pairs involving the 64B pad: c1|pad,
# pad|c2, pad|data, c2|data all genuinely share a line, and co-residency is
# an intersection of line ranges rather than a property of the span covering
# both fields. The old expectation encoded that older, wrong geometry.
# True but low value, because padding is never written -- a fact FL002 cannot
# establish per-TU, which is why the severity ladder and not the pair test is
# where it gets demoted.
expect_eq pad_sep_pairs \
  "$(lines "$CASES/pad_sep.c" c11 --rule FL002 | tr '\n' ' ')" \
  "FL002:14:Critical FL002:6:Medium "

# recall canary: all four false-sharing shapes must fire. atomicity is not
# the gate, and an array's elements pair with each other. both were silent.
expect_eq fs_shapes \
  "$(lines "$CASES/fs_shapes.c" c11 --rule FL002 | tr '\n' ' ')" \
  "FL002:10:Medium FL002:12:Medium FL002:14:Medium FL002:16:High "

# a constant-returning switch is a lookup, not a branch tree: the BTB
# mechanism does not apply. Same case count with real work must still fire.
# Medium, not High: bench/btb_cost.c measures predictable indirect dispatch
# flat from 2 to 4096 targets, so case count cannot carry severity and the
# misprediction term stays unestablished without a profile.
expect_eq fl050_lookup_switch \
  "$(lines "$CASES/lookup_switch.c" c11 --rule FL050 | tr '\n' ' ')" \
  "FL050:26:Medium "

# FL040 Critical needs sustained pressure: loop write or >=4 sites;
# and a single in-loop site is never write-once
expect_eq fl040_pressure \
  "$(lines "$CASES/fl040_pressure.c" c11 --rule FL040 | tr '\n' ' ')" \
  "FL040:12:Critical FL040:6:High FL040:9:Critical "

# write evidence: unwritten pair demotes to High; written pair carries
# the distinct-writers escalation
expect_has fl002_unwritten "$mit" "FL002:43:High"
expect_has fl002_writers \
  "$("$LSHAZ" scan "$CASES/mitigated.c" --rule FL002 --no-ir -f json -- -std=c11 2>/dev/null)" \
  "written from distinct functions in this TU"

# cf8ab03: jobs-invariance with same-line twin symbols
twdb="$TMP/twins_cc.json"
python3 - "$CASES/twins" "$twdb" <<'PY'
import json,sys,os
d,out=sys.argv[1],sys.argv[2]
cmds=[{"directory":d,"command":f"gcc -std=c11 -c {f}","file":os.path.join(d,f)}
      for f in sorted(os.listdir(d)) if f.endswith(".c")]
json.dump(cmds,open(out,"w"))
PY
"$LSHAZ" scan "$twdb" --no-ir-cache -f json --jobs 1 -o "$TMP/tw1.json" 2>/dev/null
"$LSHAZ" scan "$twdb" --no-ir-cache -f json --jobs 4 -o "$TMP/tw4.json" 2>/dev/null
h1="$(grep -v '"timestamp"' "$TMP/tw1.json" | md5sum | cut -d' ' -f1)"
h4="$(grep -v '"timestamp"' "$TMP/tw4.json" | md5sum | cut -d' ' -f1)"
expect_eq twins_jobs_invariance "$h1" "$h4"

# distinct same-line struct types keep distinct findings (identity, not location)
ntw="$(grep -c '"ruleID": "FL002"' "$TMP/tw4.json")"
expect_eq twins_identity "$ntw" "4"

# fc5b05c: broken TU must not disable IR refinement
btdb="$TMP/broken_cc.json"
python3 - "$CASES/brokentu" "$btdb" <<'PY'
import json,sys,os
d,out=sys.argv[1],sys.argv[2]
cmds=[{"directory":d,"command":f"gcc -std=c11 -c {f}","file":os.path.join(d,f)}
      for f in sorted(os.listdir(d)) if f.endswith(".c")]
json.dump(cmds,open(out,"w"))
PY
expect_has brokentu_ir \
  "$("$LSHAZ" scan "$btdb" --no-ir-cache -f json --jobs 2 2>/dev/null)" \
  "IR confirmed"

# df13344/aab2cd4: calibration loop round-trip
LD="$TMP/loop"; mkdir -p "$LD"
"$LSHAZ" scan "$CASES/loop_fl021.cpp" --rule FL021 --no-ir -f json -o "$LD/scan.json" -- -std=c++20 2>/dev/null
"$LSHAZ" exp "$LD/scan.json" -o "$LD/exp" --rule FL021 >/dev/null 2>&1
ED="$(ls -d "$LD"/exp/*/ 2>/dev/null | head -1)"
if [ -z "$ED" ]; then bad loop_expgen "no bundle"; else
  python3 - "$ED" <<'PY'
import struct,random,os,sys
ed=sys.argv[1]; random.seed(3); os.makedirs(ed+"/results",exist_ok=True)
for arm in ("treatment","control"):
    t=0
    with open(f"{ed}/results/{arm}_samples.bin","wb") as f:
        for _ in range(100000):
            d=max(1,int(random.gauss(600,40))); f.write(struct.pack("<QQ",t,t+d)); t+=d+10
open(ed+"/results/env.json","w").write(
 '{"governor":"performance","turbo_disabled":true,"cpu_model":"x","kernel":"0","cores":[2,3]}')
PY
  # both directions or the test is vacuous: present without the store,
  # absent with it.
  nbefore="$(lines "$CASES/loop_fl021.cpp" c++20 --rule FL021 | grep -c FL021)"
  for i in 1 2 3; do "$LSHAZ" feedback "$ED" --store "$LD/cal.json" >/dev/null 2>&1; done
  nafter="$(lines "$CASES/loop_fl021.cpp" c++20 --rule FL021 --calibration-store "$LD/cal.json" | grep -c FL021)"
  expect_eq loop_suppression "$nbefore/$nafter" "1/0"
fi

# 41434f6: corrupt store is a hard error in single-file mode
echo '{"version":1,"records":[{broken' > "$TMP/corrupt.json"
"$LSHAZ" scan "$CASES/ns_writes.cpp" --calibration-store "$TMP/corrupt.json" -f json -o /dev/null -- -std=c++20 2>/dev/null
expect_eq corrupt_store_exit "$?" "3"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
