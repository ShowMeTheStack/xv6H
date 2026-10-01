# shared by the night scripts: wait for a run, and notice when it will never finish.
# A run leaves no `done' file if the box reboots under it (the owner's: 30 Sep 23:50,
# which killed the grand tour at 120 of 300) -- waitrun then waited for ever (night 9,
# night 12). If the run's unit is gone and no `done' appeared, say so and go on.
H=${H:-build@buildbox}; R=${R:-'~/xv6h-runs'}
log() { echo "=== $(date +%H:%M:%S) $*"; }
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
