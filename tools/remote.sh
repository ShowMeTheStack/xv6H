#!/bin/bash
#
# run the test harness on the build box (32 cores), in a named copy of
# this tree, so that several versions can be tested at the same time
# while the laptop keeps working:
#
#   tools/remote.sh run NAME SUITE [test-hyp.py args...]   # sync, build, start in tmux
#   tools/remote.sh queue NAME [tools/qrun.py args...]      # sync, build, the whole matrix as a queue
#   tools/remote.sh status [NAME]                           # progress of a run, or of all runs
#   tools/remote.sh wait NAME                               # block until NAME is done, print the summary
#   tools/remote.sh log NAME                                # the full results file
#   tools/remote.sh script NAME FILE.py [args...]            # sync, build, run a python script (test-hyp helpers available)
#   tools/remote.sh kill NAME                               # stop a run
#   tools/remote.sh rm NAME                                 # delete a run's copy
#
# the copy lives in ~/xv6h-runs/NAME on the box; the Linux artifacts come
# from ~/xv6h-linux/out there. results: ~/xv6h-runs/NAME/results.out and
# a "done" file holding the exit status.
#
set -e
HOST=${HOST:-build@buildbox}
RUNS='~/xv6h-runs'
HERE="$(cd "$(dirname "$0")/.." && pwd)"
cmd=$1; shift || true

# the link to the build box is a few hundred kilobytes a second, so a
# full copy of the tree costs minutes and five of them cost half an
# hour. the tree goes over the wire once, into a seed directory that
# rsync then updates incrementally, and each run is a local copy of
# that seed -- which is a disk-to-disk copy on a machine with 32 cores.
sync_tree() { # NAME
  local name=$1
  ssh -o BatchMode=yes $HOST "mkdir -p $RUNS/.seed"
  # --delete-excluded as well as --delete: a file that becomes
  # excluded (a host binary built for the laptop, say) must go from
  # the remote tree too, or make will find it, decide it is current
  # and try to run it.
  # '*.d' below is the dependency files -- but it matches a directory
  # called init.d or rc.d just as happily, so directories ending in .d
  # are included first: a trailing slash makes a pattern match only
  # those. that cost a whole matrix run once.
  #
  # and '*.img' is excluded wholesale rather than by a list of the names
  # images have had. The list was fs.img, *-fs.img, test-*.img, mig*.img,
  # guest[0-9]*.img, guest/*.img, ext4check.img -- and a scratch script
  # that named its disks ad<pid>-a.img matched none of them, so a sync
  # spent a slow link pushing 2.3 GB of them to the build box. Nothing
  # tracked in this tree is a .img (.gitignore excludes them all) and the
  # box builds its own file system image, so there is nothing to lose by
  # excluding the extension.
  rsync -a --delete --delete-excluded \
    --include '*.d/' --exclude '*.o' --exclude '*.d' --exclude '*.asm' --exclude '*.sym' \
    --exclude '*.img' --exclude '*.sock' --exclude 'user/_*' --exclude 'user/usys.S' \
    --exclude 'qlogs' --exclude 'qresults.txt' \
    --exclude 'ext4check.mnt' --exclude '*.cow' --exclude '*.log' \
    --exclude 'build' --exclude '__pycache__' \
    --exclude 'kernel/kernel' --exclude 'mkfs/mkfs' --exclude 'mkfs/mkext4' --exclude 'mkfs/mkpasswd' --exclude 'tls/obj' --exclude 'tls/libbearssl.a' \
    --exclude 'guest/kernel' --exclude 'guest/hello' --exclude 'guest/hello.bin' \
    --exclude 'guest/fdguest' --exclude 'guest/vguest' --exclude 'guest/badguest' \
    --exclude 'linux/Image*' --exclude 'user/tls_date.h' \
    --exclude 'results.out' --exclude 'done' --exclude 'test-hyp.log' --exclude '.git' \
    "$HERE/" "$HOST:$RUNS/.seed/"
  ssh -o BatchMode=yes $HOST "rm -rf $RUNS/$name && cp -a $RUNS/.seed $RUNS/$name && \
    cd $RUNS/$name && mkdir -p linux && ln -sf ~/xv6h-linux/out/Image-hf linux/Image && ln -sf ~/xv6h-linux/out/linux.img linux/linux.img && rm -f done results.out"
}

case "$cmd" in
run)
  name=$1; shift; suite=$1; shift || true
  [ -n "$name" ] && [ -n "$suite" ] || { echo "usage: remote.sh run NAME SUITE [args]"; exit 1; }
  sync_tree "$name"
  ssh -o BatchMode=yes $HOST "cd $RUNS/$name && tmux kill-session -t xv6h-$name 2>/dev/null; \
    tmux new-session -d -s xv6h-$name \"cd $RUNS/$name && (make -s > build.out 2>&1 && ./test-hyp.py $suite $* > results.out 2>&1; echo \\\$? > done)\""
  echo "started $name: ./test-hyp.py $suite $* on $HOST (tools/remote.sh status $name)"
  ;;
script)
  name=$1; shift; script=$1; shift || true
  [ -n "$name" ] && [ -f "$script" ] || { echo "usage: remote.sh script NAME FILE.py [args]"; exit 1; }
  sync_tree "$name"
  scp -q "$script" "$HOST:$RUNS/$name/run_script.py"
  ssh -o BatchMode=yes $HOST "cd $RUNS/$name && tmux kill-session -t xv6h-$name 2>/dev/null; \
    tmux new-session -d -s xv6h-$name \"cd $RUNS/$name && (make -s > build.out 2>&1 && rm -f fs.img && make -s fs.img HOSTFS_MB=192 HOSTFILES='linux/Image linux/linux.img' >> build.out 2>&1 && python3 run_script.py $* > results.out 2>&1; echo \\\$? > done)\""
  echo "started $name: $script $* on $HOST"
  ;;
queue)
  name=$1; shift || true
  [ -n "$name" ] || { echo "usage: remote.sh queue NAME [qrun args...]"; exit 1; }
  sync_tree "$name"
  # qrun builds once itself and then runs every test as its own job, so
  # the build here is only the file system image the jobs copy.
  ssh -o BatchMode=yes $HOST "cd $RUNS/$name && tmux kill-session -t xv6h-$name 2>/dev/null; \
    tmux new-session -d -s xv6h-$name \"cd $RUNS/$name && (python3 tools/qrun.py $* > results.out 2>&1; echo \\\$? > done)\""
  echo "started $name: tools/qrun.py $* on $HOST (tools/remote.sh status $name)"
  ;;

status)
  name=$1
  if [ -n "$name" ]; then
    ssh -o BatchMode=yes $HOST "cd $RUNS/$name 2>/dev/null || exit 1; \
      if [ -f done ]; then echo \"$name: done, exit \$(cat done)\"; else echo \"$name: running\"; fi; \
      grep -c ': PASS' results.out 2>/dev/null | sed 's/^/  passed: /'; grep ': FAIL' results.out 2>/dev/null | cut -c1-160; \
      grep 'failure(s)' results.out 2>/dev/null; tail -1 results.out 2>/dev/null | grep '^== [a-z0-9-]*$' | sed 's/^/  now: /'; \
      grep -i error build.out 2>/dev/null | head -3"
  else
    ssh -o BatchMode=yes $HOST "for d in $RUNS/*/; do n=\$(basename \$d); if [ -f \$d/done ]; then s=\"done, exit \$(cat \$d/done)\"; else s=running; fi; \
      p=\$(grep -c ': PASS' \$d/results.out 2>/dev/null); f=\$(grep -c ': FAIL' \$d/results.out 2>/dev/null); echo \"\$n: \$s, \$p passed, \$f failed\"; done"
  fi
  ;;
wait)
  name=$1
  until ssh -o BatchMode=yes $HOST "test -f $RUNS/$name/done"; do sleep 30; done
  "$0" status "$name"
  ;;
log)
  ssh -o BatchMode=yes $HOST "cat $RUNS/$1/results.out"
  ;;
kill)
  ssh -o BatchMode=yes $HOST "tmux kill-session -t xv6h-$1 2>/dev/null; pkill -f 'xv6h-runs/$1/' ; echo killed" 
  ;;
rm)
  ssh -o BatchMode=yes $HOST "tmux kill-session -t xv6h-$1 2>/dev/null; rm -rf $RUNS/$1"
  ;;
*)
  sed -n 2,18p "$0"; exit 1;;
esac
