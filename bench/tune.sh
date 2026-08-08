#!/bin/bash
# Determinism tuning for the lshaz measurement rig.
# Every setting here exists to remove a variance source that would otherwise
# swamp a sub-1% effect. Re-run after every reboot (sysfs state is not durable).

fail() { echo "TUNE-FAIL: $*" >&2; exit 1; }

# --- frequency ------------------------------------------------------------
# Turbo on a desktop part in a rack is thermally governed and drifts with
# ambient + duty cycle. Base clock is the only repeatable operating point.
echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo || fail "no_turbo"
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

# --- perf -----------------------------------------------------------------
sysctl -qw kernel.perf_event_paranoid=-1 || fail "paranoid"
sysctl -qw kernel.kptr_restrict=0
sysctl -qw kernel.nmi_watchdog=0          # frees a fixed-function PMU counter
sysctl -qw kernel.randomize_va_space=0    # this campaign is layout-sensitive

# --- interrupts -----------------------------------------------------------
# isolcpus keeps the scheduler off 1,2,3(+7,8,9) but says nothing about IRQs.
# An unpinned NIC interrupt on a redis core is worth more jitter than
# everything above combined.
systemctl stop irqbalance    2>/dev/null
systemctl disable irqbalance 2>/dev/null
HOUSEKEEPING=0,4,5,6,10,11
HOUSEKEEPING_MASK=c71   # hex; default_smp_affinity is a mask, not a list
echo $HOUSEKEEPING_MASK > /proc/irq/default_smp_affinity 2>/dev/null
for i in /proc/irq/*/smp_affinity_list; do
    echo $HOUSEKEEPING > "$i" 2>/dev/null
done

# --- verify ---------------------------------------------------------------
echo "=== VERIFY ==="
echo "no_turbo:      $(cat /sys/devices/system/cpu/intel_pstate/no_turbo)  (want 1)"
echo "governor:      $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)  (want performance)"
echo "isolated:      $(cat /sys/devices/system/cpu/isolated)  (want 1-3,7-9)"
echo "nohz_full:     $(cat /sys/devices/system/cpu/nohz_full 2>/dev/null)"
echo "thp:           $(cat /sys/kernel/mm/transparent_hugepage/enabled)"
echo "paranoid:      $(sysctl -n kernel.perf_event_paranoid)  (want -1)"
echo "aslr:          $(sysctl -n kernel.randomize_va_space)  (want 0)"
echo "nmi_watchdog:  $(sysctl -n kernel.nmi_watchdog)  (want 0)"
echo "swap:          $(swapon --show | wc -l) entries (want 0)"
echo "irqs still on isolated cores:"
CNT=0
for i in /proc/irq/*/smp_affinity_list; do
    v=$(cat "$i" 2>/dev/null)
    case "$v" in *1*|*2*|*3*) CNT=$((CNT+1));; esac
done
echo "  $CNT irq(s) with an isolated cpu in affinity (some are unmovable per-cpu IRQs)"
echo "steady-state freq sample (should be flat at base):"
for c in 0 1 2 3 4 5; do
    printf "  cpu%d %s MHz\n" "$c" \
      "$(awk -v c=$c '/^processor/{p=$3} /^cpu MHz/{if(p==c){printf "%.0f", $4; exit}}' /proc/cpuinfo)"
done
