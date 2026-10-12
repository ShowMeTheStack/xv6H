#!/bin/bash
# The follow-up pass: the measurements the first pass (tools/night24.sh) left open, on the same
# frozen build (808f863). The seed freeze24p is that build with harness changes only
# (tools/hunt3.py's stall dump, tools/campaign.py's kill, tools/scenario.py's look inside,
# tools/stockcross.py and tools/smptime.py new); its HARNESS file lists them. The seed freeze21
# is an earlier build, used for the masking A/B comparison only.
# Runs ON THE BUILD BOX as the user service xv6h-post24, resumable like tools/night24.sh
# (state ~/xv6h-runs/post24.state, log post24.log).
#
# Quiet first: the masking A/B, the consolidation, linux-disk-smp timed three ways, the stock
# crossing's listing. Then, side by side: the ring and hunt3 again, and mm-16g and mm-512g x3.
set -u
cd "$(dirname "$0")/.."
export H=local SEED=freeze24p QUIET=5.0 LXTAG=-freeze21 HOST=local
. tools/nightlib.sh
ST=$(eval echo $R)/post24.state
touch "$ST"
log "follow-up pass on $(head -1 BUILDSTAMP 2>/dev/null) ($(sed -n 2p BUILDSTAMP 2>/dev/null)); harness: $(tr '\n' ' ' < HARNESS 2>/dev/null)"

waitquiet30() {
  local t=0
  while [ $t -lt 30 ]; do
    l=$(cut -d' ' -f1 /proc/loadavg)
    if awk -v l="$l" -v q="$QUIET" 'BEGIN{exit !(l < q)}'; then log "box quiet (load $l)"; return 0; fi
    sleep 60; t=$((t + 1))
  done
  log "box not quiet after 30 min (load $(cut -d' ' -f1 /proc/loadavg)); going on, the load is in load.log"
}

istage() {
  local name=$1 kind=$2; shift 2
  local d=$(eval echo $R)/$name
  if grep -qx "$name" "$ST"; then log "$name: finished before, skipped"; return 0; fi
  [ "${QWAIT:-0}" = 1 ] && waitquiet30
  local tries=0
  while [ $tries -lt 3 ]; do
    if systemctl --user is-active --quiet "xv6h-$name"; then
      log "$name: still running; waiting"
    elif [ -f "$d/done" ] && [ $tries -eq 0 ]; then
      log "$name: had finished; recorded"
      echo "$name" >> "$ST"; return 0
    elif [ "$kind" = queue ] && [ -f "$d/qresults.txt" ] && [ ! -f "$d/done" ]; then
      log "$name: resuming the queue where it stopped"
      tools/remote.sh resume "$name" "$@"
    else
      tools/remote.sh "$kind" "$name" "$@"
    fi
    if waitrun "$name"; then
      echo "$name" >> "$ST"; log "$name: finished"; return 0
    fi
    tries=$((tries + 1))
    log "$name: died (attempt $tries); again in 2 min"
    sleep 120
  done
  log "$name: died three times; going on without it"
  echo "$name FAILED" >> "$ST"
}

# ---- quiet: one at a time ----
SEED=freeze21 QWAIT=1 istage p24-mask-a21 script tools/maskfloor.py 5
QWAIT=1 istage p24-mask-a24 script tools/maskfloor.py 5
SEED=freeze21 QWAIT=1 istage p24-mask-b21 script tools/maskfloor.py 5
QWAIT=1 istage p24-mask-b24 script tools/maskfloor.py 5
QWAIT=1 istage p24-scen-con script tools/scenario.py consolidate
QWAIT=1 istage p24-smptime script tools/smptime.py 10
LXTAG=-stock QWAIT=1 istage p24-stockcross script tools/stockcross.py 3

# ---- side by side ----
istage p24-ring script tools/grandtour.py 100 &
istage p24-hunt3 script tools/hunt3.py 1000 &
( istage p24-mm16 queue -t mm-16g -r 3 --slots 1
  istage p24-mm512 queue -t mm-512g -r 3 --slots 1 ) &
wait
log "follow-up pass done"
touch "$(eval echo $R)/post24.done"
