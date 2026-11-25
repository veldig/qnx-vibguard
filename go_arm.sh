#!/bin/bash
# ARM64 QNX VM on Apple Silicon. Tries Apple's Hypervisor.framework (HVF, near-native), falls back to emulation.
# Run:  bash ~/Downloads/JobAgent/qnx/go_arm.sh     (go.sh must have run once: it creates the container + key)
set -u
PROJ="$HOME/Downloads/JobAgent/qnx"
cd "$PROJ" || exit 1
mkdir -p logs results
LOG="logs/arm-$(date +%Y%m%d-%H%M%S).log"
exec > >(tee "$LOG") 2>&1
ln -sf "$(basename "$LOG")" logs/latest.log
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH:/Applications/Docker.app/Contents/Resources/bin"
step() { echo; echo "######## $* ($(date +%H:%M:%S))"; }
fail() { echo; echo "FAILED: $*"; echo "STATUS=FAILED"; exit 1; }
SSH_PORT=2223
KEY="$PROJ/keys/qnx_vm_key"
SSH_OPTS=(-i "$KEY" -p $SSH_PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR)
SCP_OPTS=(-i "$KEY" -P $SSH_PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)

step "1. Build ARM64 programs + image (inside container)"
if [ "${SKIP_BUILD:-0}" = 1 ] && [ -f vm-arm/output/ifs.bin ]; then echo "skipping build"; else
docker start qnx-build >/dev/null || fail "container qnx-build missing; run go.sh first"
docker exec qnx-build bash "/home/$(id -un)/Downloads/JobAgent/qnx/scripts/build_arm_in_container.sh" || fail "ARM build"
fi
qemu-system-aarch64 --version | head -1

boot() {  # $1 = hvf | tcg
  [ -f vm-arm/qemu.pid ] && kill "$(cat vm-arm/qemu.pid)" 2>/dev/null; sleep 1
  : > logs/qemu_arm_serial.log
  if [ "$1" = hvf ]; then MACH="-machine virt,gic-version=2 -accel hvf -cpu host"; else MACH="-machine virt -accel tcg,thread=multi -cpu cortex-a57"; fi
  echo "Booting with: $MACH"
  # Device order matters: mkqnximage expects disk, then net, then rng in the top virtio-mmio slots.
  qemu-system-aarch64 $MACH -smp 2 -m 1G \
    -drive file=vm-arm/output/disk-qemu.vmdk,if=none,id=drv0 -device virtio-blk-device,drive=drv0 \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:${SSH_PORT}-:22 -device virtio-net-device,netdev=net0 \
    -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-device,rng=rng0 \
    -kernel vm-arm/output/ifs.bin -display none -serial file:logs/qemu_arm_serial.log \
    -pidfile vm-arm/qemu.pid -daemonize || return 1
  for i in $(seq 1 40); do ssh "${SSH_OPTS[@]}" root@127.0.0.1 true 2>/dev/null && return 0; sleep 3; done
  return 1
}

step "2. Boot ARM64 QNX with HVF (hardware virtualization)"
MODE=hvf
if ! boot hvf; then
  echo "--- HVF serial log ---"; tail -40 logs/qemu_arm_serial.log
  step "2b. HVF did not come up; retrying with emulation (TCG)"
  MODE=tcg
  boot tcg || { echo "--- TCG serial log ---"; tail -40 logs/qemu_arm_serial.log; fail "ARM VM did not accept SSH"; }
fi
echo "Booted with MODE=$MODE"
tail -25 logs/qemu_arm_serial.log
ssh "${SSH_OPTS[@]}" root@127.0.0.1 'uname -a; pidin info | head -5'

step "3. hello + 200 Hz timer tests (ARM64, $MODE)"
scp "${SCP_OPTS[@]}" build-arm/hello build-arm/timer200 root@127.0.0.1:/tmp/ || fail scp
ssh "${SSH_OPTS[@]}" root@127.0.0.1 /tmp/hello | tee results/arm_${MODE}_hello.txt
ssh "${SSH_OPTS[@]}" root@127.0.0.1 '/tmp/timer200 5000 1000 0 /tmp/d.csv'  > results/arm_${MODE}_timer_default.txt; tail -6 results/arm_${MODE}_timer_default.txt
ssh "${SSH_OPTS[@]}" root@127.0.0.1 '/tmp/timer200 5000 1000 63 /tmp/f.csv' > results/arm_${MODE}_timer_fifo63.txt; tail -6 results/arm_${MODE}_timer_fifo63.txt
scp "${SCP_OPTS[@]}" root@127.0.0.1:/tmp/d.csv results/arm_${MODE}_timer_default.csv || true
scp "${SCP_OPTS[@]}" root@127.0.0.1:/tmp/f.csv results/arm_${MODE}_timer_fifo63.csv || true
echo; echo "ARM VM still running. Stop: kill \$(cat $PROJ/vm-arm/qemu.pid)"
echo "STATUS=OK MODE=$MODE"
