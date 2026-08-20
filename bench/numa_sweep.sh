#!/usr/bin/env bash
# The measurement no single-socket machine can make.
#
# measured-constants.md currently hedges: "Cross-socket and multi-CCD coherence
# are more expensive; these figures are a lower bound for those topologies."
# That is an admission, not a number. Same rmw_cost harness, same spacing sweep,
# run twice — both threads inside one socket, then one per socket — turns the
# hedge into a multiplier.
#
# Placement is discovered from numactl rather than assumed: CPU numbering is
# not reliably socket-major, and guessing lands both threads on one node while
# claiming otherwise.
set -u

# Per-op time varies ~4 orders of magnitude across this sweep, so a fixed
# iteration count either takes hours at the sparse end or measures noise at the
# dense end. Calibrate each point and spend a fixed wall-clock budget instead.
BUDGET_S=${BUDGET_S:-3}
SPINS=${SPINS:-"0 8 40 200 1000 5000"}

iters_for() { # <spin> <cpulist>
    local probe ns
    probe=$(./rmw_cost 2 "$1" 20000 shared "$2" 2>/dev/null \
            | sed -n 's/.*mean_ns=\([0-9.]*\).*/\1/p')
    [ -n "$probe" ] || { echo 1000000; return; }
    ns=$(awk -v b="$BUDGET_S" -v p="$probe" 'BEGIN{
        n = b * 1e9 / p
        if (n < 200000)   n = 200000
        if (n > 50000000) n = 50000000
        printf "%d", n }')
    echo "$ns"
}

command -v numactl >/dev/null || { echo "numactl missing"; exit 2; }
[ -x ./rmw_cost ] || { echo "build rmw_cost first"; exit 2; }

node_cpus() { numactl --hardware | sed -n "s/^node $1 cpus: //p"; }

N0=$(node_cpus 0); N1=$(node_cpus 1)
[ -n "$N0" ] && [ -n "$N1" ] || { echo "need two NUMA nodes; got '$N0' / '$N1'"; exit 2; }

# Isolated cores only, and physical ones at that. Housekeeping cores carry the
# IRQs we deliberately pinned there; measuring on them reads ~23ns for an
# uncontended lock add that should cost 2-5ns, and the noise buries the
# cross-socket signal entirely. SMT siblings are excluded separately — they
# share L1/L2, which is a different mechanism that reads as "cheap".
ISO=$(cat /sys/devices/system/cpu/isolated 2>/dev/null)
[ -n "$ISO" ] || echo "WARNING: no isolated cpus; run tune.sh or results are noise" >&2
iso_expanded=$(for c in $(echo "$ISO" | tr ',' ' '); do
    case "$c" in *-*) seq ${c%-*} ${c#*-};; *) echo "$c";; esac
done)

phys() {
    for c in $1; do
        sib=$(cut -d, -f1 < /sys/devices/system/cpu/cpu$c/topology/thread_siblings_list 2>/dev/null)
        [ "$sib" = "$c" ] || continue
        [ -z "$ISO" ] && { echo "$c"; continue; }
        echo "$iso_expanded" | grep -qx "$c" && echo "$c"
    done
}
P0=($(phys "$N0")); P1=($(phys "$N1"))
[ ${#P0[@]} -ge 2 ] || { echo "node0 needs 2 physical cores"; exit 2; }
[ ${#P1[@]} -ge 1 ] || { echo "node1 needs 1 physical core"; exit 2; }

INTRA="${P0[0]},${P0[1]}"
CROSS="${P0[0]},${P1[0]}"

echo "node0 cpus: $N0"
echo "node1 cpus: $N1"
echo "intra-socket pair: $INTRA     cross-socket pair: $CROSS"
echo
printf "%-6s %-9s %10s %12s %12s %10s\n" "spin" "topology" "iters" "shared" "private" "delta"

for s in $SPINS; do
    n=$(iters_for "$s" "$INTRA")
    for topo in intra cross; do
        cpus=$INTRA; [ "$topo" = cross ] && cpus=$CROSS
        # Min of 3. Cost is a floor: interference only ever adds, so the
        # minimum is the closest estimate of the mechanism alone.
        best_sh=""; best_pv=""
        for r in 1 2 3; do
            v=$(./rmw_cost 2 "$s" "$n" shared "$cpus" | sed -n 's/.*mean_ns=\([0-9.]*\).*/\1/p')
            best_sh=$(awk -v a="$best_sh" -v b="$v" 'BEGIN{print (a==""||b<a)?b:a}')
            v=$(./rmw_cost 2 "$s" "$n" private "$cpus" | sed -n 's/.*mean_ns=\([0-9.]*\).*/\1/p')
            best_pv=$(awk -v a="$best_pv" -v b="$v" 'BEGIN{print (a==""||b<a)?b:a}')
        done
        d=$(awk -v a="$best_sh" -v b="$best_pv" 'BEGIN{printf "%.3f", a-b}')
        printf "%-6s %-9s %10s %12s %12s %10s\n" "$s" "$topo" "$n" "$best_sh" "$best_pv" "$d"
    done
done

echo
echo "delta is the coherence cost; identical spin work cancels between arms."
echo "cross/intra ratio at the dense end is the number the table is missing."
