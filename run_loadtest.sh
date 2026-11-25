#!/bin/bash
# Noisy-neighbour test on the QNX x86_64 VM: does priority-based scheduling protect the 200 Hz loop?
# Run:  bash ~/Downloads/JobAgent/qnx/run_loadtest.sh
set -u
PROJ="$HOME/Downloads/JobAgent/qnx"
cd "$PROJ" || exit 1
mkdir -p logs results
LOG="logs/loadtest-$(date +%Y%m%d-%H%M%S).log"
exec > >(tee "$LOG") 2>&1
ln -sf "$(basename "$LOG")" logs/latest.log
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH:/Applications/Docker.app/Contents/Resources/bin"
step() { echo; echo "######## $* ($(date +%H:%M:%S))"; }
fail() { echo; echo "FAILED: $*"; echo "STATUS=FAILED"; exit 1; }
SSH_PORT=2222
KEY="$PROJ/keys/qnx_vm_key"
SSH_OPTS=(-i "$KEY" -p $SSH_PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o ServerAliveInterval=15 -o ServerAliveCountMax=8 -o LogLevel=ERROR)
SCP_OPTS=(-i "$KEY" -P $SSH_PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)

step "1. Compile vibguard (QNX qcc, inside container)"
docker start qnx-build >/dev/null || fail "container qnx-build missing; run go.sh first"
docker exec qnx-build bash "/home/$(id -un)/Downloads/JobAgent/qnx/scripts/build_vibguard.sh" || fail "build"

step "2. VM check + copy"
ssh "${SSH_OPTS[@]}" root@127.0.0.1 'uname -a; pidin info' || fail "VM not reachable: run run_vibguard.sh (it boots the VM)"
ssh "${SSH_OPTS[@]}" root@127.0.0.1 'rm -rf /tmp/lt; mkdir -p /tmp/lt/data /tmp/lt/L0 /tmp/lt/L1 /tmp/lt/L2'
scp "${SCP_OPTS[@]}" build/vibguard root@127.0.0.1:/tmp/lt/ || fail scp
scp "${SCP_OPTS[@]}" data/wt901_20261005_121751.csv root@127.0.0.1:/tmp/lt/data/ || fail scp

ARGS='-s -c 1 -N data/wt901_20261005_121751.csv -n 6000 -q'
step "3. L0: no load, vibguard SCHED_FIFO 50/45/40 (30 s)"
ssh "${SSH_OPTS[@]}" root@127.0.0.1 "cd /tmp/lt && ./vibguard $ARGS -o L0" || fail L0
step "4. L1: 2 CPU hogs at FIFO 15, vibguard SCHED_FIFO 50/45/40 (30 s)"
ssh "${SSH_OPTS[@]}" root@127.0.0.1 "cd /tmp/lt && ./vibguard $ARGS -l 2 -g 15 -o L1" || fail L1
step "5. L2: 2 CPU hogs at FIFO 15, vibguard at DEFAULT priority (30 s, the shell may freeze while hogs run)"
ssh "${SSH_OPTS[@]}" root@127.0.0.1 "cd /tmp/lt && ./vibguard $ARGS -l 2 -g 15 -R -o L2" || fail L2

step "6. Collect"
for r in L0 L1 L2; do
  rm -rf "results/load_$r"; mkdir -p "results/load_$r"
  scp "${SCP_OPTS[@]}" "root@127.0.0.1:/tmp/lt/$r/*" "results/load_$r/" || fail "collect $r"
  echo "== $r"; cat "results/load_$r/vibguard_summary.txt"
done
echo "STATUS=OK"
