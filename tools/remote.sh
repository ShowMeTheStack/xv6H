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
#   tools/remote.sh resume NAME [qrun args...]               # continue a queue where it stopped (same args)
#   LXTAG=-618 tools/remote.sh run ...                       # the Linux image of another kernel build (linux/build.sh TAG)
#   SEED=NAME tools/remote.sh ...                            # reuse that run's synced tree instead of the working tree
#   tools/remote.sh clean                                    # strip every finished run to its results and logs (disk)
#   tools/remote.sh rm NAME                                 # delete a run's copy
#
# each run is a user systemd unit xv6h-NAME (linger is on: it survives the ssh session that
# started it -- two tmux-started runs died the moment other sessions came and went);
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
  # SEED=OTHER: copy that run's tree instead of syncing the working tree, so a
  # script that queues stages over a night cannot pick up an edit made
  # between them (night 4 lost five stages to a half-made edit that way)
  if [ -n "${SEED:-}" ]; then
    ssh -o BatchMode=yes $HOST "rm -rf $RUNS/$name && cp -a $RUNS/$SEED $RUNS/$name && cd $RUNS/$name && \
      rm -rf qlogs qresults.txt results.out done build.out run_script.py && \
      mkdir -p linux && ln -sf ~/xv6h-linux/out${LXTAG:-}/${LXIMAGE:-Image-hf} linux/Image && ln -sf ~/xv6h-linux/out/linux.img linux/linux.img"
    return
  fi
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
    cd $RUNS/$name && mkdir -p linux && ln -sf ~/xv6h-linux/out${LXTAG:-}/${LXIMAGE:-Image-hf} linux/Image && ln -sf ~/xv6h-linux/out/linux.img linux/linux.img && rm -f done results.out"
}

case "$cmd" in
run)
  name=$1; shift; suite=$1; shift || true
  [ -n "$name" ] && [ -n "$suite" ] || { echo "usage: remote.sh run NAME SUITE [args]"; exit 1; }
  sync_tree "$name"
  ssh -o BatchMode=yes $HOST "cd $RUNS/$name && systemctl --user stop xv6h-$name 2>/dev/null; systemctl --user reset-failed xv6h-$name 2>/dev/null; \
    systemd-run --user --collect --quiet --unit xv6h-$name bash -c \"cd $RUNS/$name && (make -s > build.out 2>&1 && ./test-hyp.py $suite $* > results.out 2>&1; echo \\\$? > done)\""
  echo "started $name: ./test-hyp.py $suite $* on $HOST (tools/remote.sh status $name)"
  ;;
script)
  name=$1; shift; script=$1; shift || true
  [ -n "$name" ] && [ -f "$script" ] || { echo "usage: remote.sh script NAME FILE.py [args]"; exit 1; }
  sync_tree "$name"
  scp -q "$script" "$HOST:$RUNS/$name/run_script.py"
  ssh -o BatchMode=yes $HOST "cd $RUNS/$name && systemctl --user stop xv6h-$name 2>/dev/null; systemctl --user reset-failed xv6h-$name 2>/dev/null; \
    systemd-run --user --collect --quiet --unit xv6h-$name bash -c \"cd $RUNS/$name && (make -s > build.out 2>&1 && rm -f fs.img && make -s fs.img HOSTFS_MB=192 HOSTFILES='linux/Image linux/linux.img' >> build.out 2>&1 && python3 run_script.py $* > results.out 2>&1; echo \\\$? > done)\""
  echo "started $name: $script $* on $HOST"
  ;;
queue)
  name=$1; shift || true
  [ -n "$name" ] || { echo "usage: remote.sh queue NAME [qrun args...]"; exit 1; }
  sync_tree "$name"
  # qrun builds once itself and then runs every test as its own job, so
  # the build here is only the file system image the jobs copy.
  ssh -o BatchMode=yes $HOST "cd $RUNS/$name && systemctl --user stop xv6h-$name 2>/dev/null; systemctl --user reset-failed xv6h-$name 2>/dev/null; \
    systemd-run --user --collect --quiet --unit xv6h-$name bash -c \"cd $RUNS/$name && (python3 tools/qrun.py $* > results.out 2>&1; echo \\\$? > done)\""
  echo "started $name: tools/qrun.py $* on $HOST (tools/remote.sh status $name)"
  ;;

build)
  # sync and build only (kernel, guests, user programs): a compile check
  # that leaves the box's queues alone
  name=$1; shift || true
  [ -n "$name" ] || { echo "usage: remote.sh build NAME"; exit 1; }
  sync_tree "$name"
  ssh -o BatchMode=yes $HOST "cd $RUNS/$name && nice -n 15 make -s > build.out 2>&1; echo \$? > done; cat build.out | tail -40; echo build-exit \$(cat done)"
  ;;
clean)
  # strip finished (or dead) runs on the box to their results and logs: a run is a
  # copy of the tree plus disk images, and 487 of them filled the box's disk (30 Sep).
  # Every log stays (q*.log per slot, console*.log of the probes) and bench/ (the campaign's
  # per-machine JSON): the guest's view when a job hangs -- defect 24 was read from a slot
  # log; night 10's grand tour and campaign details were lost to a narrower keep list
  ssh -o BatchMode=yes $HOST "cd $RUNS; active=\$(systemctl --user list-units 'xv6h-*' --no-legend | awk '{print \$1}' | sed 's/xv6h-//; s/.service//'); n=0; \
    for d in */; do d=\${d%/}; [ \"\$d\" = .seed ] && continue; echo \"\$active\" | grep -qx \"\$d\" && continue; \
    find \"\$d\" -mindepth 1 -maxdepth 1 ! -name results.out ! -name qresults.txt ! -name qlogs ! -name done ! -name build.out ! -name console.log ! -name test-hyp.log ! -name run_script.py ! -name '*.json' ! -name '*.md' ! -name '*.txt' ! -name '*.log' ! -name bench -exec rm -rf {} + 2>/dev/null; n=\$((n+1)); done; \
    echo \"stripped \$n runs\"; df -h ~ | tail -1"
  ;;
resume)
  # continue a queue in its existing copy (qrun --resume believes qresults.txt):
  # no sync, no rm -- the copy is the run (n3-lx100 died at 1 013 of 2 400)
  name=$1; shift || true
  [ -n "$name" ] || { echo "usage: remote.sh resume NAME [qrun args...]"; exit 1; }
  ssh -o BatchMode=yes $HOST "cd $RUNS/$name && rm -f done && systemctl --user stop xv6h-$name 2>/dev/null; systemctl --user reset-failed xv6h-$name 2>/dev/null; \
    systemd-run --user --collect --quiet --unit xv6h-$name bash -c \"cd $RUNS/$name && (python3 tools/qrun.py --resume $* >> results.out 2>&1; echo \\\$? > done)\""
  echo "resumed $name: tools/qrun.py --resume $* on $HOST"
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
  ssh -o BatchMode=yes $HOST "systemctl --user stop xv6h-$1 2>/dev/null; tmux kill-session -t xv6h-$1 2>/dev/null; pkill -f 'xv6h-runs/$1/' ; echo killed" 
  ;;
rm)
  ssh -o BatchMode=yes $HOST "tmux kill-session -t xv6h-$1 2>/dev/null; rm -rf $RUNS/$1"
  ;;
*)
  sed -n 2,18p "$0"; exit 1;;
esac
