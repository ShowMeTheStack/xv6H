# shared by the night scripts: wait for a run, and notice when it will never finish.
# A run leaves no `done' file if the box reboots under it (the owner's: 30 Sep 23:50,
# which killed the grand tour at 120 of 300) -- waitrun then waited for ever (night 9,
# night 12). If the run's unit is gone and no `done' appeared, say so and go on.
H=${H:-build@buildbox}; R=${R:-'~/xv6h-runs'}
log() { echo "=== $(date +%H:%M:%S) $*"; }
# a timing stage must start on a box with nothing else on it: wait until the one-minute
# load average is below 3 (the day-5 adoption probes ran in the wake of a twenty-job stage
# and measured the box, not the mechanism)
waitquiet() {
  while :; do
    l=$(ssh -o BatchMode=yes -o ConnectTimeout=25 $H "cut -d' ' -f1 /proc/loadavg" 2>/dev/null)
    case "$l" in ''|'?') sleep 30; continue;; esac
    if awk -v l="$l" 'BEGIN{exit !(l < 3.0)}'; then log "box quiet (load $l)"; return 0; fi
    log "box busy (load $l); waiting"; sleep 60
  done
}
waitrun() {
  local name=$1 gone=0
  while :; do
    if ssh -o BatchMode=yes -o ConnectTimeout=25 $H "test -f $R/$name/done" 2>/dev/null; then
      log "$name done: $(tools/remote.sh status $name 2>/dev/null | tr '\n' ' ' | cut -c1-200)"
      return 0
    fi
    # a box that does not answer is not a run that died: only an answer that says the unit
    # is gone counts (night 16 declared the matrix dead after three ssh timeouts, and lost
    # its Linux stage the same way)
    units=$(ssh -o BatchMode=yes -o ConnectTimeout=25 $H "systemctl --user list-units 'xv6h-$name.service' --no-legend; echo __answered__" 2>/dev/null)
    if ! echo "$units" | grep -q __answered__; then
      log "$name: the box did not answer; still waiting"
    elif echo "$units" | grep -q "xv6h-$name"; then
      gone=0
    else
      gone=$((gone + 1))
      if [ $gone -ge 3 ]; then
        log "$name DIED (no unit, no done): $(ssh -o BatchMode=yes $H "tail -2 $R/$name/results.out 2>/dev/null | tr '\n' ' ' | cut -c1-160")"
        return 1
      fi
    fi
    sleep 60
  done
}

# a stage, started and waited for; a run that died with the box (a power cut takes the build box
# down now and then) is started once more from scratch, or resumed for a queue stage
#   stage NAME script|queue ARGS...
stage() {
  local name=$1 kind=$2; shift 2
  tools/remote.sh $kind $name "$@"
  if ! waitrun $name; then
    log "$name: starting again after the box went down"
    sleep 120
    if [ "$kind" = queue ]; then tools/remote.sh resume $name "$@"; else tools/remote.sh $kind $name "$@"; fi
    waitrun $name || log "$name: died twice; going on"
  fi
}
