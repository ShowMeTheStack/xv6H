#!/bin/bash
# night 24: every result in the paper and the supplement, on the one frozen build freeze24
# (commit 808f863: d5cc446's hypervisor, defect 55 fixed). Resumable as night 21 was.
# Runs ON THE BUILD BOX as the user service xv6h-night24 (enabled: it starts again at boot, and
# systemd restarts it if it dies; a cron watchdog starts it if it is not running). Every stage is idempotent: ~/xv6h-runs/night24.state lists the
# stages finished; after a reboot a finished stage is skipped, a queue that died is resumed where
# it stopped (qrun --resume), and a script that died is started again. Nothing here needs the
# laptop.
#
# Lane A (the latency tables, on as quiet a box as it gets within 30 min; each run's load.log
# says what it was), then two lanes at once: lane 1 the matrix, the Linux suite x20 and the Linux
# crossings x100; lane 2 the endurance loops two at a time.
set -u
cd "$(dirname "$0")/.."
export H=local SEED=freeze24 QUIET=5.0 LXTAG=-freeze21 HOST=local
. tools/nightlib.sh
ST=$(eval echo $R)/night24.state
touch "$ST"
log "night 24 on $(head -1 BUILDSTAMP 2>/dev/null) ($(sed -n 2p BUILDSTAMP 2>/dev/null))"

# a quiet box if it comes within 30 minutes; otherwise go on (the load is recorded)
waitquiet30() {
  local t=0
  while [ $t -lt 30 ]; do
    l=$(cut -d' ' -f1 /proc/loadavg)
    if awk -v l="$l" -v q="$QUIET" 'BEGIN{exit !(l < q)}'; then log "box quiet (load $l)"; return 0; fi
    sleep 60; t=$((t + 1))
  done
  log "box not quiet after 30 min (load $(cut -d' ' -f1 /proc/loadavg)); going on, the load is in load.log"
}

# a resumable stage:  istage NAME script|queue ARGS...
istage() {
  local name=$1 kind=$2; shift 2
  local d=$(eval echo $R)/$name
  if grep -qx "$name" "$ST"; then log "$name: finished before, skipped"; return 0; fi
  [ "${QWAIT:-0}" = 1 ] && waitquiet30
  local tries=0
  while [ $tries -lt 3 ]; do
    if systemctl --user is-active --quiet "xv6h-$name"; then
      log "$name: still running; waiting"
    elif [ -f "$d/done" ] && [ $tries -eq 0 ] && [ -d "$d" ] && [ "$(sed -n 1p $d/BUILDSTAMP 2>/dev/null)" = "$(head -1 BUILDSTAMP)" ]; then
      log "$name: had finished; recorded"
      echo "$name" >> "$ST"; return 0
    elif [ "$kind" = queue ] && [ -f "$d/qresults.txt" ] && [ ! -f "$d/done" ]; then
      log "$name: resuming the queue where it stopped"
      tools/remote.sh resume "$name" "$@"
    else
      tools/remote.sh "$kind" "$name" "$@"
    fi
    if waitrun "$name"; then
      echo "$name" >> "$ST"
      log "$name: finished"
      return 0
    fi
    tries=$((tries + 1))
    log "$name: died (attempt $tries); again in 2 min"
    sleep 120
  done
  log "$name: died three times; going on without it"
  echo "$name FAILED" >> "$ST"
}


# a resumable KVM stage, on the final KVM stack (kvm/build-final.sh):  kstage NAME "ENV" SCRIPT ARGS...
KF=$HOME/xv6h-kvm-final
kstage() {
  local name=$1 env=$2; shift 2
  if grep -qx "$name" "$ST"; then log "$name: finished before, skipped"; return 0; fi
  [ "${QWAIT:-0}" = 1 ] && waitquiet30
  mkdir -p $KF/runs
  local tries=0
  while [ $tries -lt 3 ]; do
    log "$name: kvm/$*  ($env)"
    ( cd $KF/src && cat /proc/loadavg > $KF/runs/$name.load && \
      export HOSTKERNEL=$KF/build-host/arch/riscv/boot/Image HOSTINITRD=$KF/host-initramfs.cpio.gz && \
      { [ -z "$env" ] || eval "export $env"; } && \
      timeout 4h python3 -u "$@" > $KF/runs/$name.out 2>&1; echo $? > $KF/runs/$name.exit; cat /proc/loadavg >> $KF/runs/$name.load )
    if [ "$(cat $KF/runs/$name.exit)" = 0 ]; then echo "$name" >> "$ST"; log "$name: finished"; return 0; fi
    tries=$((tries + 1)); log "$name: exit $(cat $KF/runs/$name.exit) (attempt $tries); again in 2 min"; sleep 120
  done
  log "$name: failed three times; going on without it"; echo "$name FAILED" >> "$ST"
}

# ---- lane A, one stage at a time on a quiet box: every measurement whose number depends on it ----
QWAIT=1 istage n24-zed-boot script tools/zedboard.py host guest linux
QWAIT=1 istage n24-zed-revise script tools/zedboard.py revise rounds=10
QWAIT=1 istage n24-mask script tools/maskfloor.py 5
QWAIT=1 istage n24-maskboot script tools/maskboot.py 5
QWAIT=1 istage n24-adopt-xv6 script tools/adoptprobe.py xv6 20
QWAIT=1 istage n24-adopt-linux script tools/adoptprobe.py linux 20
QWAIT=1 istage n24-adopt-lxvic script tools/adoptprobe.py linuxvic 20
QWAIT=1 istage n24-adopt-xmap script tools/adoptprobe.py xv6+map 20
QWAIT=1 istage n24-revload script tools/revload.py 5
QWAIT=1 istage n24-scen-up script tools/scenario.py upgrade
QWAIT=1 istage n24-scen-con script tools/scenario.py consolidate
QWAIT=1 istage n24-tail script tools/taillat.py 5 500 all split
QWAIT=1 istage n24-tmig script tools/taillat_mig.py same 25
QWAIT=1 istage n24-tmig-tb script tools/taillat_mig.py timebase 10
QWAIT=1 istage n24-tmig-intc script tools/taillat_mig.py intc 10
QWAIT=1 istage n24-control script tools/control.py 3
QWAIT=1 istage n24-revcost script tools/revcost.py 12
QWAIT=1 istage n24-tiercost script tools/tiercost.py 5
QWAIT=1 istage n24-tiercross script tools/tiercross.py 4
QWAIT=1 istage n24-idlecost script tools/idlecost.py 10
QWAIT=1 istage n24-heldline script tools/heldline.py 16
QWAIT=1 istage n24-obstime-xv6 script tools/obstime.py xv6
QWAIT=1 istage n24-obstime-linux script tools/obstime.py linux
QWAIT=1 istage n24-scale-console script tools/scale.py console xv6 20
QWAIT=1 istage n24-scale-vcpus-xv6 script tools/scale.py vcpus xv6 20
QWAIT=1 istage n24-scale-vcpus-linux script tools/scale.py vcpus linux 20
QWAIT=1 istage n24-scale-ram-xv6 script tools/scale.py ram xv6 10
QWAIT=1 istage n24-scale-ram-linux script tools/scale.py ram linux 10
QWAIT=1 istage n24-scale-load script tools/scale.py load xv6 20
QWAIT=1 istage n24-scale-devices script tools/scale.py devices xv6 20
QWAIT=1 istage n24-migscale script tools/migscale.py 3
QWAIT=1 istage n24-qemubase script tools/qemubase.py 3
QWAIT=1 istage n24-bench-hyp1 script bench/hyp.py
QWAIT=1 istage n24-bench-hyp2 script bench/hyp.py
QWAIT=1 istage n24-bench-hyp3 script bench/hyp.py
QWAIT=1 istage n24-bench-fs script bench/run.py
QWAIT=1 istage n24-fuzzco script tools/fuzzco.py 20 300000
QWAIT=1 istage n24-alias script tools/aliasstress.py 200
QWAIT=1 istage n24-migmac script tools/migmac.py 64 20
QWAIT=1 istage n24-migreplay script tools/migreplay.py keyed,plain 16
QWAIT=1 istage n24-disksmp queue -t linux-disk-smp -r 20 --slots 1

# ---- lanes B and C at once: the pass/fail queues, and the endurance loops two at a time ----
(
  istage n24-timeline queue --repeat 20 -t migrate --slots 6
  istage n24-matrix queue --slots 6
  istage n24-linux queue -s linux -r 20 --slots 8
  istage n24-lxmig queue -t snap-lx-migrate -r 100 --slots 8
  istage n24-xlive queue -t guest-revise -r 20 --slots 4
  istage n24-revguest queue -t guest-revguest -r 20 --slots 2
  istage n24-multi20 queue --repeat 20 -t migrate-multi --slots 6
  istage n24-flush1 run guest -t guest-revise-flushstorm
  istage n24-flush2 run guest -t guest-revise-flushstorm
  LXTAG=-618 istage n24-lx618 run linux
  LXTAG=-618 istage n24-lxs618 run snap -t snap-lx-migrate-
  LXTAG=-stock istage n24-stock-boot queue -t linux-boot -r 20 --slots 6
  LXTAG=-stock istage n24-stock-cross queue -t snap-lx-migrate-intc-legacy -r 20 --slots 4
  istage n24-campaign script tools/campaign.py
) &
L1=$!
(
  istage n24-hunt4 script tools/hunt4.py 500 &
  istage n24-hunt2 script tools/hunt2.py 1000 &
  wait
  istage n24-hunt3 script tools/hunt3.py 1000 &
  istage n24-ring script tools/grandtour.py 100 &
  wait
) &
L2=$!
wait $L1 $L2

# ---- the KVM stack, one stage at a time on a quiet box ----
QWAIT=1 kstage n24-k-proto "" t_proto.py
QWAIT=1 kstage n24-k-devmap "" t_devmap.py 20 devmap,irqmap,intc
QWAIT=1 kstage n24-k-intc-poll "" t_devmap.py 10 intc
QWAIT=1 kstage n24-k-intc-idle "IDLE_BEFORE=10" t_devmap.py 10 intc
QWAIT=1 kstage n24-k-inflight "" t_inflight.py 20
QWAIT=1 kstage n24-k-timebase "" t_timebase.py 8
QWAIT=1 kstage n24-k-vector "" t_vector.py 20
QWAIT=1 kstage n24-k-migrate "" t_migrate.py
QWAIT=1 kstage n24-k-migrate-plic "INTC=plic" t_migrate.py
QWAIT=1 kstage n24-k-migrate-arrive "ARRIVE='intc plic;timebase 1000000'" t_migrate.py
QWAIT=1 kstage n24-k-migbad "" t_migrate_bad.py
QWAIT=1 kstage n24-k-cross-intc "" t_cross.py intc 20
QWAIT=1 kstage n24-k-cross-tb "" t_cross.py timebase 20
QWAIT=1 kstage n24-k-cross-alt "" t_cross.py alt 300
QWAIT=1 kstage n24-k-imsicfile "" t_imsicfile.py 20
QWAIT=1 kstage n24-k-idle "IDLETAG=-n24" t_idle.py 10
QWAIT=1 kstage n24-k-mask "" t_mask.py 5
log "night 24 done"
touch "$(eval echo $R)/night24.done"
