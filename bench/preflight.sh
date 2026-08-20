#!/bin/bash
# Refuse to measure on a box that is not in the documented tuned state.
# Every setting below vanishes on reboot and an untuned run is
# indistinguishable from a tuned one in the output. Source this, or call it,
# before any measurement; non-zero exit means the numbers would be garbage.
#
# Expectations are derived from the machine, not written down. A hardcoded
# core list passes on the box it was written for and quietly misvalidates
# every other one.

fail=0
chk() { # <label> <actual> <want>
    if [ "$2" = "$3" ]; then
        printf "  ok    %-14s %s\n" "$1" "$2"
    else
        printf "  DRIFT %-14s got '%s' want '%s'\n" "$1" "$2" "$3"; fail=1
    fi
}

# Same derivation tune.sh uses, so the two cannot disagree about which cores
# are supposed to be quiet.
RESERVE=${RESERVE:-2}
nodes=$(ls -d /sys/devices/system/node/node[0-9]* 2>/dev/null | sed 's/.*node//' | sort -n)
[ -n "$nodes" ] || nodes=0
ISOLATED=""; HOUSEKEEPING=""
for n in $nodes; do
    phys=""
    for c in $(cat /sys/devices/system/node/node$n/cpulist | tr ',' ' '); do
        case "$c" in *-*) c=$(seq ${c%-*} ${c#*-});; esac
        for cpu in $c; do
            sl=/sys/devices/system/cpu/cpu$cpu/topology/thread_siblings_list
            [ -r "$sl" ] || continue
            [ "$(cut -d, -f1 < $sl)" = "$cpu" ] && phys="$phys $cpu"
        done
    done
    i=0
    for cpu in $phys; do
        sibs=$(tr ',' ' ' < /sys/devices/system/cpu/cpu$cpu/topology/thread_siblings_list)
        if [ $i -lt $RESERVE ]; then HOUSEKEEPING="$HOUSEKEEPING $sibs"
        else ISOLATED="$ISOLATED $sibs"; fi
        i=$((i+1))
    done
done
csv() { echo $* | tr ' ' '\n' | sort -n | uniq | paste -sd,; }
# The kernel prints cpu sets in range form ("8-15,24-31"); we derive them
# expanded. Compare as sets or an identical set reads as DRIFT.
expand() {
    for c in $(echo "$1" | tr ',' ' '); do
        case "$c" in *-*) seq ${c%-*} ${c#*-};; *) echo "$c";; esac
    done | sort -n | uniq | paste -sd,
}
HK=$(csv $HOUSEKEEPING); ISO=$(csv $ISOLATED)
HK_MASK=$(python3 -c "
m=0
for c in '$HK'.split(','):
    m |= 1 << int(c)
print('%x' % m)")

echo "=== lshaz bench preflight ==="
echo "  derived        housekeeping=$HK isolated=$ISO"

turbo=$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null \
        || cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null | tr 1 9 | tr 0 1 | tr 9 0)
chk no_turbo     "$turbo"                                                      1
chk governor     "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)" performance
chk isolated     "$(expand "$(cat /sys/devices/system/cpu/isolated)")"         "$ISO"
chk paranoid     "$(sysctl -n kernel.perf_event_paranoid)"                     -1
chk aslr         "$(sysctl -n kernel.randomize_va_space)"                      0
chk nmi_watchdog "$(sysctl -n kernel.nmi_watchdog)"                            0
chk numa_balance "$(sysctl -n kernel.numa_balancing 2>/dev/null)"              0
chk thp          "$(sed -n 's/.*\[\(.*\)\].*/\1/p' /sys/kernel/mm/transparent_hugepage/enabled)" never
# hex mask, not a cpu list. Writing a list here silently does nothing, which is
# how it drifted unnoticed for a whole session.
chk irq_default  "$(sed 's/,//g;s/^0*//' /proc/irq/default_smp_affinity)"      "$HK_MASK"
chk swap         "$(swapon --show | wc -l)"                                    0

# Exact set membership. Matching "1" as a substring finds it inside 13 and 21.
moved=0
for i in /proc/irq/*/smp_affinity_list; do
    v=$(cat "$i" 2>/dev/null) || continue
    for c in $(echo "$v" | tr ',' ' '); do
        case "$c" in *-*) c=$(seq ${c%-*} ${c#*-});; esac
        for cpu in $c; do
            case ",$ISO," in *",$cpu,"*) moved=$((moved+1)); break 2;; esac
        done
    done
done
printf "  info  %-14s %s movable irq(s) still reaching isolated cores\n" \
       "irq_spread" "$moved"

if [ "$fail" -ne 0 ]; then
    echo "PREFLIGHT FAILED — run tune.sh, do not trust any measurement until green" >&2
    exit 1
fi
echo "PREFLIGHT OK"
