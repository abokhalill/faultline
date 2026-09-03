#!/bin/bash
# Determinism tuning for the lshaz measurement rig.
# Every setting here removes a variance source that would otherwise swamp a
# sub-1% effect. Re-run after every reboot, sysfs state is not durable.
#
# Topology is derived, never assumed. The previous version hardcoded a 6-core
# single-socket layout; on a dual-socket box those constants isolate the wrong
# cores while still reporting success.

fail() { echo "TUNE-FAIL: $*" >&2; exit 1; }

# --- topology -------------------------------------------------------------
# Isolate whole physical cores with their SMT siblings. Isolating one thread of
# a pair leaves the sibling schedulable, and the two share L1/L2, the noise
# arrives anyway, through the cache instead of the runqueue.
nodes=$(ls -d /sys/devices/system/node/node[0-9]* 2>/dev/null | sed 's/.*node//' | sort -n)
[ -n "$nodes" ] || nodes=0
ISOLATED=""; HOUSEKEEPING=""
RESERVE=${RESERVE:-2}          # physical cores per node left for the OS

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
        if [ $i -lt $RESERVE ]; then
            HOUSEKEEPING="$HOUSEKEEPING $sibs"
        else
            ISOLATED="$ISOLATED $sibs"
        fi
        i=$((i+1))
    done
done

csv()  { echo $* | tr ' ' '\n' | sort -n | uniq | paste -sd,; }
HK=$(csv $HOUSEKEEPING); ISO=$(csv $ISOLATED)
[ -n "$HK" ] || fail "no housekeeping cpus derived"

# default_smp_affinity takes a hex mask, not a list. Writing a list here looks
# like it worked and silently never applies. It cost a whole session once.
HK_MASK=$(python3 -c "
m=0
for c in '$HK'.split(','):
    m |= 1 << int(c)
print('%x' % m)")

echo "topology: $(echo $nodes | wc -w) node(s), housekeeping=$HK, isolated=$ISO"
echo "isolcpus line for GRUB:"
echo "  isolcpus=$ISO nohz_full=$ISO rcu_nocbs=$ISO"

# --- frequency ------------------------------------------------------------
# Turbo is thermally governed and drifts with ambient and duty cycle. Base
# clock is the only repeatable operating point.
echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null \
  || echo 0 > /sys/devices/system/cpu/cpufreq/boost 2>/dev/null \
  || echo "warn: no turbo control found" >&2
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$g" 2>/dev/null
done

# --- memory ---------------------------------------------------------------
# khugepaged compaction stalls are multi-millisecond and land arbitrarily
# inside a measurement window.
echo never > /sys/kernel/mm/transparent_hugepage/enabled || fail "thp"
echo never > /sys/kernel/mm/transparent_hugepage/defrag
echo 0 > /sys/kernel/mm/ksm/run 2>/dev/null
swapoff -a 2>/dev/null

# NUMA balancing migrates pages mid-run, which is the thing FL060 measures.
sysctl -qw kernel.numa_balancing=0 2>/dev/null

# --- perf -----------------------------------------------------------------
sysctl -qw kernel.perf_event_paranoid=-1 || fail "paranoid"
sysctl -qw kernel.kptr_restrict=0
sysctl -qw kernel.nmi_watchdog=0          # frees a fixed-function PMU counter
sysctl -qw kernel.randomize_va_space=0    # this campaign is layout-sensitive

# --- interrupts -----------------------------------------------------------
# isolcpus keeps the scheduler off those cores but says nothing about IRQs. An
# unpinned NIC interrupt on a measurement core outweighs everything above.
systemctl stop irqbalance    2>/dev/null
systemctl disable irqbalance 2>/dev/null
echo $HK_MASK > /proc/irq/default_smp_affinity 2>/dev/null
for i in /proc/irq/*/smp_affinity_list; do
    echo $HK > "$i" 2>/dev/null
done

# --- verify ---------------------------------------------------------------
echo "=== VERIFY ==="
printf "  %-14s %s\n" "isolated" "$(cat /sys/devices/system/cpu/isolated) (want $ISO)"
printf "  %-14s %s\n" "nohz_full" "$(cat /sys/devices/system/cpu/nohz_full 2>/dev/null)"
printf "  %-14s %s\n" "governor" "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
printf "  %-14s %s\n" "thp" "$(cat /sys/kernel/mm/transparent_hugepage/enabled)"
printf "  %-14s %s\n" "paranoid" "$(sysctl -n kernel.perf_event_paranoid) (want -1)"
printf "  %-14s %s\n" "aslr" "$(sysctl -n kernel.randomize_va_space) (want 0)"
printf "  %-14s %s\n" "numa_balancing" "$(sysctl -n kernel.numa_balancing 2>/dev/null) (want 0)"
printf "  %-14s %s\n" "irq_mask" "$(cat /proc/irq/default_smp_affinity) (want $HK_MASK)"

# Exact set membership, not a substring match: the old test matched cpu "1"
# inside "13" and "21" and reported IRQs that were never there.
CNT=0
for i in /proc/irq/*/smp_affinity_list; do
    v=$(cat "$i" 2>/dev/null) || continue
    for c in $(echo "$v" | tr ',' ' '); do
        case "$c" in *-*) c=$(seq ${c%-*} ${c#*-});; esac
        for cpu in $c; do
            case ",$ISO," in *",$cpu,"*) CNT=$((CNT+1)); break 2;; esac
        done
    done
done
echo "  $CNT irq(s) still reaching an isolated cpu (some per-cpu IRQs are unmovable)"
