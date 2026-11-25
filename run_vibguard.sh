#!/bin/bash
# Build vibguard with the QNX toolchain, run it in real time on the QNX x86_64 VM, collect logs.
# Run:  bash ~/Downloads/JobAgent/qnx/run_vibguard.sh
set -u
PROJ="$HOME/Downloads/JobAgent/qnx"
cd "$PROJ" || exit 1
mkdir -p logs results
LOG="logs/vibguard-$(date +%Y%m%d-%H%M%S).log"
exec > >(tee "$LOG") 2>&1
ln -sf "$(basename "$LOG")" logs/latest.log
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH:/Applications/Docker.app/Contents/Resources/bin"
step() { echo; echo "######## $* ($(date +%H:%M:%S))"; }
fail() { echo; echo "FAILED: $*"; echo "STATUS=FAILED"; exit 1; }
SSH_PORT=2222
KEY="$PROJ/keys/qnx_vm_key"
SSH_OPTS=(-i "$KEY" -p $SSH_PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR)
SCP_OPTS=(-i "$KEY" -P $SSH_PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)

step "1. Compile vibguard (QNX qcc, inside container)"
docker start qnx-build >/dev/null || fail "container qnx-build missing; run go.sh first"
docker exec qnx-build bash "/home/$(id -un)/Downloads/JobAgent/qnx/scripts/build_vibguard.sh" || fail "build"

step "2. Make sure the x86_64 QNX VM is up"
if ! ssh "${SSH_OPTS[@]}" root@127.0.0.1 true 2>/dev/null; then
  echo "VM not reachable, booting it"
  [ -f vm/qemu.pid ] && kill "$(cat vm/qemu.pid)" 2>/dev/null; sleep 1
  : > logs/qemu_serial.log
  qemu-system-x86_64 -machine pc -accel tcg,thread=multi -cpu max -smp 2 -m 1G \
    -drive file=vm/output/disk-qemu.vmdk,if=ide,id=drv0 -kernel vm/output/ifs.bin \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:${SSH_PORT}-:22 -device virtio-net-pci,netdev=net0 \
    -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
    -display none -serial file:logs/qemu_serial.log -pidfile vm/qemu.pid -daemonize || fail "qemu"
  for i in $(seq 1 100); do ssh "${SSH_OPTS[@]}" root@127.0.0.1 true 2>/dev/null && break; sleep 3; done
fi
ssh "${SSH_OPTS[@]}" root@127.0.0.1 'uname -a' || fail "VM not reachable"

step "3. Copy vibguard + recordings to the VM"
ssh "${SSH_OPTS[@]}" root@127.0.0.1 'rm -rf /tmp/vg; mkdir -p /tmp/vg/data /tmp/vg/outA /tmp/vg/outS0 /tmp/vg/outS1'
scp "${SCP_OPTS[@]}" build/vibguard root@127.0.0.1:/tmp/vg/ || fail scp
scp "${SCP_OPTS[@]}" data/wt901_20261005_121751.csv root@127.0.0.1:/tmp/vg/data/ || fail scp

step "4. Run A: replay wt901_20261005_121751.csv, first 40 s, real time (200 Hz)"
ssh "${SSH_OPTS[@]}" root@127.0.0.1 'cd /tmp/vg && ./vibguard -f data/wt901_20261005_121751.csv -n 8000 -o outA' || fail "run A"

step "5. Run S0: simulated SCARA joint, OPEN loop baseline, 60 s"
ssh "${SSH_OPTS[@]}" root@127.0.0.1 'cd /tmp/vg && ./vibguard -s -c 0 -N data/wt901_20261005_121751.csv -n 12000 -o outS0' || fail "run S0"

step "6. Run S1: simulated SCARA joint, CLOSED loop (vibguard active), 60 s"
ssh "${SSH_OPTS[@]}" root@127.0.0.1 'cd /tmp/vg && ./vibguard -s -c 1 -N data/wt901_20261005_121751.csv -n 12000 -o outS1' || fail "run S1"

step "7. Collect results"
for r in A S0 S1; do
  rm -rf "results/vibguard_run$r"; mkdir -p "results/vibguard_run$r"
  scp "${SCP_OPTS[@]}" "root@127.0.0.1:/tmp/vg/out$r/*" "results/vibguard_run$r/" || fail "collect $r"
done
for r in A S0 S1; do echo "== $r"; cat "results/vibguard_run$r/vibguard_summary.txt"; done
echo "STATUS=OK"
