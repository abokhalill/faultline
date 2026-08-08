#!/bin/bash
# Refuse to measure on a box that is not in the documented tuned state.
# Every setting below vanishes on reboot and an untuned run is
# indistinguishable from a tuned one in the output. Source this, or call it,
# before any measurement; non-zero exit means the numbers would be garbage.

fail=0
chk() { # <label> <actual> <want>
    if [ "$2" = "$3" ]; then
        printf "  ok    %-14s %s\n" "$1" "$2"
    else
        printf "  DRIFT %-14s got '%s' want '%s'\n" "$1" "$2" "$3"; fail=1
    fi
}

echo "=== lshaz bench preflight ==="
chk no_turbo     "$(cat /sys/devices/system/cpu/intel_pstate/no_turbo)"        1
chk governor     "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)" performance
chk isolated     "$(cat /sys/devices/system/cpu/isolated)"                     1-3,7-9
chk paranoid     "$(sysctl -n kernel.perf_event_paranoid)"                     -1
chk aslr         "$(sysctl -n kernel.randomize_va_space)"                      0
chk nmi_watchdog "$(sysctl -n kernel.nmi_watchdog)"                            0
chk thp          "$(sed -n 's/.*\[\(.*\)\].*/\1/p' /sys/kernel/mm/transparent_hugepage/enabled)" never
# hex mask, not a cpu list: bits {0,4,5,6,10,11} = 0xc71. Writing a list here
# silently does nothing, which is how it drifted unnoticed.
chk irq_default  "$(sed 's/,//g;s/^0*//' /proc/irq/default_smp_affinity)"      c71
chk swap         "$(swapon --show | wc -l)"                                    0

# Any movable IRQ left on an isolated core lands a hardirq mid-measurement.
moved=0
for i in /proc/irq/*/smp_affinity_list; do
    case "$(cat "$i" 2>/dev/null)" in
        0-11|*1-3*) moved=$((moved+1));;
    esac
done
printf "  info  %-14s %s movable irq(s) still spanning isolated cores\n" \
       "irq_spread" "$moved"

if [ "$fail" -ne 0 ]; then
    echo "PREFLIGHT FAILED — run tune.sh, do not trust any measurement until green" >&2
    exit 1
fi
echo "PREFLIGHT OK"
