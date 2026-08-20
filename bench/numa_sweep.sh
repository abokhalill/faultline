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

ITERS=${ITERS:-40000000}
SPINS=${SPINS:-"0 8 40 200 1000 5000"}

command -v numactl >/dev/null || { echo "numactl missing"; exit 2; }
[ -x ./rmw_cost ] || { echo "build rmw_cost first"; exit 2; }

node_cpus() { numactl --hardware | sed -n "s/^node $1 cpus: //p"; }

N0=$(node_cpus 0); N1=$(node_cpus 1)
[ -n "$N0" ] && [ -n "$N1" ] || { echo "need two NUMA nodes; got '$N0' / '$N1'"; exit 2; }

# Physical cores only. SMT siblings share L1/L2, which is a different mechanism
# than cross-core coherence and would quietly read as "cheap".
phys() {
    for c in $1; do
        sib=$(cut -d, -f1 < /sys/devices/system/cpu/cpu$c/topology/thread_siblings_list 2>/dev/null)
        [ "$sib" = "$c" ] && echo "$c"
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
printf "%-6s %-14s %10s %10s %10s\n" "spin" "topology" "shared" "private" "delta"

for s in $SPINS; do
    for topo in intra cross; do
        cpus=$INTRA; [ "$topo" = cross ] && cpus=$CROSS
        sh=$(./rmw_cost 2 "$s" "$ITERS" shared  "$cpus" | sed -n 's/.*mean_ns=\([0-9.]*\).*/\1/p')
        pv=$(./rmw_cost 2 "$s" "$ITERS" private "$cpus" | sed -n 's/.*mean_ns=\([0-9.]*\).*/\1/p')
        d=$(awk -v a="$sh" -v b="$pv" 'BEGIN{printf "%.3f", a-b}')
        printf "%-6s %-14s %10s %10s %10s\n" "$s" "$topo" "$sh" "$pv" "$d"
    done
done

echo
echo "delta is the coherence cost; identical spin work cancels between arms."
echo "cross/intra ratio at the dense end is the number the table is missing."
