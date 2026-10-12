#!/bin/bash
# run "$@" while sampling the box's load average every 30 s into load.log: every latency is
# reported with the load it was measured at
( while :; do echo "$(date +%s) $(cut -d' ' -f1-3 /proc/loadavg)" >> load.log; sleep 30; done ) &
L=$!
"$@"; r=$?
kill $L 2>/dev/null
exit $r
