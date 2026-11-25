#!/bin/bash
# Runs ON the Google Cloud VM. Fixed noisy-neighbour load test under KVM (see README).
# Each test is 20 s (4000 samples). While vibguard runs, a shell loop samples pidin (thread prio + state).
# If a prio-10 shell can run pidin while FIFO 15 hogs fill both CPUs, that is itself evidence.
set -u
cd ~/qnxkvm || exit 1
log() { echo; echo "######## $* ($(date +%H:%M:%S))"; }
log "host"; uname -a; ls -la /dev/kvm || exit 2
log "install qemu"; sudo apt-get update -qq && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq qemu-system-x86 >/dev/null
sudo chmod 666 /dev/kvm; chmod 600 keys/qnx_vm_key
SSH=(ssh -i keys/qnx_vm_key -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o ServerAliveInterval=15 -o ServerAliveCountMax=20 -o LogLevel=ERROR root@127.0.0.1)
SCP=(scp -i keys/qnx_vm_key -P 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
log "boot QNX 8.0 under KVM"
qemu-system-x86_64 -machine pc -enable-kvm -cpu host -smp 2 -m 1G \
  -drive file=vm/output/disk-qemu.vmdk,if=ide,id=drv0 -kernel vm/output/ifs.bin \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 -device virtio-net-pci,netdev=net0 \
  -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
  -display none -serial file:qemu_serial.log -pidfile qemu.pid -daemonize || exit 3
for i in $(seq 1 60); do "${SSH[@]}" true 2>/dev/null && break; sleep 2; done
"${SSH[@]}" 'uname -a; pidin info' || { tail -30 qemu_serial.log; exit 4; }
"${SSH[@]}" 'rm -rf /tmp/k; mkdir -p /tmp/k/data'
"${SCP[@]}" build/vibguard root@127.0.0.1:/tmp/k/
"${SCP[@]}" data/wt901_20261005_121751.csv root@127.0.0.1:/tmp/k/data/
R=~/kvm_results; rm -rf $R; mkdir -p $R
N='-N data/wt901_20261005_121751.csv'
# Fixed load test (hogs now start after acquisition is running). 30 s each.
# L0 no load | L1 hogs FIFO 15, vibguard FIFO 50/45/40 | L2 hogs FIFO 15, vibguard default 10
# L3 control: hogs FIFO 60, above vibguard's FIFO 50/45/40 (strict priority predicts starvation)
for run in "L0:-s -c 1 $N -n 6000" "L1:-s -c 1 $N -n 6000 -l 2 -g 15" "L2:-s -c 1 $N -n 6000 -l 2 -g 15 -R" "L3:-s -c 1 $N -n 6000 -l 2 -g 60"; do
  name=${run%%:*}; args=${run#*:}
  log "vibguard $name: $args"
  "${SSH[@]}" "cd /tmp/k && mkdir -p $name && ./vibguard $args -q -o $name" | tail -16
  mkdir -p $R/$name; "${SCP[@]}" "root@127.0.0.1:/tmp/k/$name/*" $R/$name/
done
"${SSH[@]}" 'pidin -p procnto-smp-instr | head -5; pidin -p procnto | head -5; use -i /proc/boot/procnto-smp-instr 2>/dev/null | head -5; ls /proc/boot' > $R/kernel_info.txt 2>&1
kill "$(cat qemu.pid)" 2>/dev/null
echo REMOTE_OK
