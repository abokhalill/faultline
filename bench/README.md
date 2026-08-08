# bench — hardware validation rig

lshaz rules assert hardware mechanisms. This directory measures whether those
mechanisms produce the cost the rule claims, and at what threshold. Every
constant in `docs/measured-constants.md` was produced here.

The rig exists because a rule can be structurally correct and still describe
an effect that never fires. FL002 asserted MESI ping-pong from co-located
fields plus distinct writers; on redis that configuration turned out to
generate exactly zero coherence traffic, and the false positive reached a
maintainer before anyone measured it.

## Requirements

Bare metal. Not a VM, not a cloud instance short of `.metal` — `perf c2c`
needs PEBS (Intel) or IBS (AMD), and hypervisors do not pass either through.
Verify in one command:

    perf list | grep -i hitm      # empty => this host cannot do coherence work

## Use

    sudo ./tune.sh          # pins the machine to a repeatable operating point
    ./preflight.sh          # non-zero on drift; gate every measurement on this

`tune.sh` is not idempotent across reboots by accident of design — every
setting it writes lives in sysfs or sysctl and is lost on restart. Install it
as a boot unit, and run `preflight.sh` before each measurement anyway. An
untuned run produces numbers that look exactly like tuned ones.

## Validate the instrument before believing a null result

`rmw_cost.c` in `shared` mode is a known-positive: two threads writing
adjacent 8-byte counters on one line. `perf c2c` must attribute ~99% of HITM
to that line at byte offsets 0x0 and 0x8. If it does not, the invocation is
wrong and any "no contention found" elsewhere is meaningless.

    gcc -O2 -pthread -o rmw_cost rmw_cost.c
    perf c2c record -o ctl.data -- ./rmw_cost 4 0 30000000 shared 1,2,3,4
    perf c2c report -i ctl.data --stdio | head -45

## Locating a field at runtime

Link-time addresses from `nm` are not runtime addresses: most binaries are
PIE. Resolve through the live process or every downstream measurement targets
memory that holds something else.

    gdb -p $PID -batch -ex 'p &server.stat_net_input_bytes'

Exact write counts come from a hardware watchpoint, not sampling — a sampled
zero is not a zero:

    perf stat -e mem:0xADDR:w -p $PID -- sleep 5

## Traps that produced wrong answers here

- `perf script -F cpu` on `mem-stores` samples suppresses *all* output rather
  than the one field. 55,972 samples reported as zero.
- gcc elides a dependent-multiply spin loop at `-O2`. Watch for a cost that
  does not scale with the work parameter; `rmw_cost.c` defeats it with an
  empty asm barrier.
- Pinning thread *i* to CPU *i* puts thread 0 on the housekeeping core.
  Placement must be explicit and fatal on mismatch.
- `/proc/irq/default_smp_affinity` takes a hex mask; writing a CPU list
  silently does nothing.
