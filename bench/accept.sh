#!/usr/bin/env bash
# Acceptance gate for a rented measurement box. Run before tuning, before any
# experiment, before trusting a single null result.
#
# The instrument is the thing under test here, not the machine. Three false
# nulls this campaign came from instruments that looked like clean results:
# `perf script -F cpu` returning zero rows from 55,972 samples, a runtime IDIV
# burying the quantity under measurement, and c2c reporting zero HITM on pure
# false sharing because AMD files that signal elsewhere. Hence every check
# contrasts two arms rather than reading one counter.
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
# rather than fails. Calling a sampling-capable box "no PMU access" would send
# a usable machine back.
if perf stat -a -e cycles -- true 2>&1 | grep -qE "not supported|not counted"; then
    note "warn: hardware counting unavailable (no cycles/instructions/IPC)"
    note "      use valgrind --tool=cachegrind for deterministic counts instead"
else
    ok "perf counts hardware events"
fi

echo "=== 3. coherence attribution path exists ==="
# Intel carries this on xsnp and fills the HITM columns; AMD routes c2c through
# ibs_op and leaves them at zero. Requiring xsnp by name rejects working AMD.
have_path=0
if perf list 2>/dev/null | grep -qi xsnp; then
    ok "xsnp events present (HITM columns expected to populate)"
    have_path=1
fi
if [ -d /sys/bus/event_source/devices/ibs_op ]; then
    ok "ibs_op present (c2c attributes through IBS; HITM stays zero)"
    have_path=1
fi
[ "$have_path" -eq 1 ] || bad "neither xsnp nor ibs_op, c2c cannot attribute coherence"

echo "=== 4. known positive AND negative: c2c must SEPARATE them ==="
# stripe_cost, not rmw_cost: c2c attributes load and store ops, and a
# LOCK-prefixed RMW reads 1% against a 0% control on IBS.
#
# Relaunch until the window closes. The packed arm runs ~80x longer for the
# same count, so no single count keeps both arms alive through it.
c2c_run() {
    local stop=/tmp/accept_stop.$$
    touch "$stop"
    ( while [ -e "$stop" ]; do ./stripe_cost 4 "$1" 20000000 0,1,2,3 >/dev/null 2>&1; done ) &
    local kp=$!
    sleep 1
    perf c2c record -a -o "$2" -- sleep 6 >/dev/null 2>&1
    rm -f "$stop"
    wait $kp 2>/dev/null
    perf c2c report -i "$2" --stdio 2>/dev/null
}
field() {
    local v
    v=$(echo "$1" | sed -n "s/.*$2 *: *\([0-9][0-9]*\).*/\1/p" | head -1)
    echo "${v:-0}"
}

[ -x ./stripe_cost ] || cc -O2 -pthread -o ./stripe_cost stripe_cost.c 2>/dev/null
if [ ! -x ./stripe_cost ]; then
    bad "./stripe_cost not built, cannot validate the instrument"
else
    # Packed: four threads per 64B line. Padded: one line each, same work.
    pos=$(c2c_run 8  /tmp/accept_c2c_pos.data)
    neg=$(c2c_run 64 /tmp/accept_c2c_neg.data)

    separated=0
    for arm in pos neg; do
        eval "rpt=\$$arm"
        eval "${arm}_hitm=\$(( $(field "$rpt" 'Load Local HITM')  \
                            + $(field "$rpt" 'Load Remote HITM') ))"
        ld=$(field "$rpt" 'Load Operations')
        llc=$(field "$rpt" 'Load LLC hit')
        eval "${arm}_frac=$(( ld > 0 ? 100 * llc / ld : 0 ))"
    done

    # Tenfold clears the jitter between repeats of one arm.
    if [ "$pos_hitm" -gt 0 ] && [ "$pos_hitm" -ge $(( 10 * neg_hitm + 1 )) ]; then
        ok "HITM separates the arms: $pos_hitm packed vs $neg_hitm padded"
        separated=1
    fi
    # The packed arm's working set is four lines and should stay L1-resident,
    # so loads reaching LLC are the coherence traffic.
    if [ "$pos_frac" -ge 50 ] && [ "$pos_frac" -ge $(( 5 * neg_frac + 1 )) ]; then
        ok "LLC-hit fraction separates the arms: ${pos_frac}% packed vs ${neg_frac}% padded"
        separated=1
    fi

    if [ "$separated" -eq 1 ]; then
        note "verify by eye: the packed arm should concentrate on ONE line"
    else
        bad "no discriminator separates a known positive from its control."
        note "      HITM ${pos_hitm}/${neg_hitm}, LLC-hit ${pos_frac}%/${neg_frac}%"
        note "      INSTRUMENT IS BLIND, do not trust nulls from this box"
    fi
fi

echo
echo "=== $pass passed, $fail failed ==="
[ "$fail" -eq 0 ] || {
    echo "Do not tune, do not measure, do not believe a null from this box."
    exit 1
}
