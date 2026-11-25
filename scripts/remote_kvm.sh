#!/bin/bash
# Runs ON the Google Cloud VM (Ubuntu 22.04, nested virtualization). Boots the same QNX 8.0 x86_64 image
# under QEMU + KVM and runs the same tests as on the Mac. Results go to ~/kvm_results.
set -u
cd ~/qnxkvm || exit 1
log() { echo; echo "######## $* ($(date +%H:%M:%S))"; }
log "host"; uname -a; nproc; ls -la /dev/kvm || { echo "NO /dev/kvm: nested virtualization not enabled"; exit 2; }
log "install qemu"; sudo apt-get update -qq && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq qemu-system-x86 >/dev/null && qemu-system-x86_64 --version | head -1
sudo chmod 666 /dev/kvm
chmod 600 keys/qnx_vm_key
SSH=(ssh -i keys/qnx_vm_key -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o ServerAliveInterval=15 -o LogLevel=ERROR root@127.0.0.1)
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
"${SCP[@]}" build/vibguard build/timer200 build/hello root@127.0.0.1:/tmp/k/
"${SCP[@]}" data/wt901_20261005_121751.csv root@127.0.0.1:/tmp/k/data/
R=~/kvm_results; rm -rf $R; mkdir -p $R
log "hello"; "${SSH[@]}" /tmp/k/hello | tee $R/hello.txt
log "timer200 default"; "${SSH[@]}" '/tmp/k/timer200 5000 1000 0 /tmp/k/td.csv' > $R/timer_default.txt; tail -6 $R/timer_default.txt
log "timer200 FIFO 63"; "${SSH[@]}" '/tmp/k/timer200 5000 1000 63 /tmp/k/tf.csv' > $R/timer_fifo63.txt; tail -6 $R/timer_fifo63.txt
N='-N data/wt901_20261005_121751.csv'
for run in "S0:-s -c 0 $N -n 12000" "S1:-s -c 1 $N -n 12000" "L0:-s -c 1 $N -n 6000" "L1:-s -c 1 $N -n 6000 -l 2 -g 15" "L2:-s -c 1 $N -n 6000 -l 2 -g 15 -R"; do
  name=${run%%:*}; args=${run#*:}
  log "vibguard $name: $args"
  "${SSH[@]}" "cd /tmp/k && mkdir -p $name && ./vibguard $args -q -o $name" | tail -14
  mkdir -p $R/$name; "${SCP[@]}" "root@127.0.0.1:/tmp/k/$name/*" $R/$name/
done
"${SCP[@]}" root@127.0.0.1:/tmp/k/td.csv root@127.0.0.1:/tmp/k/tf.csv $R/ || true
kill "$(cat qemu.pid)" 2>/dev/null
echo REMOTE_OK
