#!/usr/bin/env bash
# Acceptance gate for a rented measurement box. Run before tuning, before any
# experiment, before trusting a single null result.
#
# The instrument is the thing under test here, not the machine. Two false nulls
# this campaign came from broken instruments that looked like clean results:
# `perf script -F cpu` returning zero rows from 55,972 samples, and a runtime
# IDIV burying the entire quantity under measurement. A box that cannot
# reproduce a known positive cannot be trusted to report a negative.
set -u

pass=0; fail=0
ok()   { printf "  \033[32mok\033[0m    %s\n" "$1"; pass=$((pass+1)); }
bad()  { printf "  \033[31mFAIL\033[0m  %s\n" "$1"; fail=$((fail+1)); }
note() { printf "        %s\n" "$1"; }

echo "=== 1. identity ==="
model=$(lscpu | sed -n 's/^Model name: *//p' | head -1)
sockets=$(lscpu | sed -n 's/^Socket(s): *//p')
numa=$(lscpu | sed -n 's/^NUMA node(s): *//p')
note "$model"
# Single socket is a legitimate rig for everything except FL060 and the
# cross-socket sweep, so report the capability rather than failing on it.
ok "sockets=$sockets, numa nodes=$numa"
[ "${numa:-0}" -ge 2 ] || note "single node: FL060 and numa_sweep.sh unavailable here"

echo "=== 2. perf capability ==="
# Sampling and counting are separate privileges and some hosts grant only one.
# Sampling is what this campaign needs: every harness times with clock_gettime
# and every attribution comes from c2c. Counting is a nice-to-have, so it warns
# rather than fails — calling a sampling-capable box "no PMU access" would send
# a usable machine back.
if perf stat -a -e cycles -- true 2>&1 | grep -qE "not supported|not counted"; then
    note "warn: hardware counting unavailable (no cycles/instructions/IPC)"
    note "      use valgrind --tool=cachegrind for deterministic counts instead"
else
    ok "perf counts hardware events"
fi

echo "=== 3. coherence events exist ==="
if perf list 2>/dev/null | grep -qi xsnp; then
    ok "xsnp/HITM events present"
    perf list 2>/dev/null | grep -i xsnp | head -3 | sed 's/^/        /'
else
    bad "no xsnp events — perf c2c cannot attribute coherence"
fi

echo "=== 4. known positive: c2c must SEE false sharing ==="
# rmw_cost in shared mode is false sharing by construction. On a working
# instrument this concentrates HITM on one line across two byte offsets.
if [ ! -x ./rmw_cost ]; then
    bad "./rmw_cost not built — cannot validate the instrument"
else
    ./rmw_cost 2 0 60000000 shared 0,1 >/dev/null 2>&1 &
    kp=$!
    sleep 1
    perf c2c record -a -o /tmp/accept_c2c.data -- sleep 6 >/dev/null 2>&1
    wait $kp 2>/dev/null
    rpt=$(perf c2c report -i /tmp/accept_c2c.data --stdio 2>/dev/null)
    # Count HITM, do not read the "LLC Misses to Remote" percentages: on a
    # single-socket box those are zero by construction and a working
    # instrument reads as blind.
    loc=$(echo "$rpt" | sed -n 's/.*Load Local HITM *: *\([0-9]*\).*/\1/p'  | head -1)
    rem=$(echo "$rpt" | sed -n 's/.*Load Remote HITM *: *\([0-9]*\).*/\1/p' | head -1)
    tot=$(( ${loc:-0} + ${rem:-0} ))
    if [ "$tot" -gt 0 ]; then
        ok "c2c sees the known positive: ${loc:-0} local + ${rem:-0} remote HITM"
        note "verify by eye: HITM should concentrate on ONE line, two offsets"
    else
        bad "zero HITM on a known positive — INSTRUMENT IS BLIND, do not trust nulls"
    fi
fi

echo
echo "=== $pass passed, $fail failed ==="
[ "$fail" -eq 0 ] || {
    echo "Do not tune, do not measure, do not believe a null from this box."
    exit 1
}
