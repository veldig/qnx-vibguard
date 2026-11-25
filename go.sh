#!/bin/bash
# QNX hackathon Step 1: one-shot setup + build + boot + tests, for macOS (Apple Silicon).
# Run:  bash ~/Downloads/JobAgent/qnx/go.sh
# Everything is logged to ~/Downloads/JobAgent/qnx/logs/latest.log so Claude can read it.
set -u
PROJ="$HOME/Downloads/JobAgent/qnx"
cd "$PROJ" || exit 1
mkdir -p logs results build
LOG="logs/go-$(date +%Y%m%d-%H%M%S).log"
exec > >(tee "$LOG") 2>&1
ln -sf "$(basename "$LOG")" logs/latest.log

step() { echo; echo "######## $* ($(date +%H:%M:%S))"; }
fail() { echo; echo "FAILED: $*"; echo "STATUS=FAILED"; exit 1; }

SSH_PORT=2222
KEY="$PROJ/keys/qnx_vm_key"
SSH_OPTS=(-i "$KEY" -p $SSH_PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR)
SCP_OPTS=(-i "$KEY" -P $SSH_PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)

step "0. Host info"
sw_vers; uname -m; df -h "$HOME" | tail -1
[ -d "$HOME/qnx800" ] || fail "~/qnx800 not found"

step "1. QEMU (Homebrew)"
BREW=""
for b in /opt/homebrew/bin/brew /usr/local/bin/brew; do [ -x "$b" ] && BREW="$b" && break; done
if ! command -v qemu-system-x86_64 >/dev/null 2>&1 && [ ! -x /opt/homebrew/bin/qemu-system-x86_64 ]; then
  [ -n "$BREW" ] || fail "Homebrew not installed. Install it from https://brew.sh (paste the one-line installer in Terminal), then rerun this script."
  "$BREW" install qemu || fail "brew install qemu"
fi
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
qemu-system-x86_64 --version | head -1 || fail "qemu-system-x86_64 missing"

step "2. Docker"
# Docker Desktop's CLI and its credential helper (docker-credential-desktop) live here
export PATH="$PATH:/Applications/Docker.app/Contents/Resources/bin:$HOME/.docker/bin"
DOCKER="$(command -v docker || echo /Applications/Docker.app/Contents/Resources/bin/docker)"
echo "docker CLI: $DOCKER"; command -v docker-credential-desktop || echo "note: docker-credential-desktop not on PATH"
[ -x "$DOCKER" ] || fail "docker CLI not found (is Docker Desktop installed?)"
if ! "$DOCKER" info >/dev/null 2>&1; then
  echo "Starting Docker Desktop..."; open -a Docker
  for i in $(seq 1 60); do "$DOCKER" info >/dev/null 2>&1 && break; sleep 3; done
fi
"$DOCKER" info >/dev/null 2>&1 || fail "Docker is not running"
"$DOCKER" version --format 'Docker {{.Server.Version}} ({{.Server.Os}}/{{.Server.Arch}})'

step "3. SSH key for the QNX VM (project-local, not ~/.ssh)"
[ -f "$KEY" ] || ssh-keygen -q -t ed25519 -N '' -f "$KEY" -C qnx-hackathon-vm || fail "ssh-keygen"
ls -la keys

step "4. Build container image qnx800-build (first time can take several minutes)"
if ! "$DOCKER" image inspect qnx800-build >/dev/null 2>&1; then
  "$DOCKER" build --platform linux/amd64 -t qnx800-build \
    --build-arg USER_NAME="$(id -un)" --build-arg USER_ID="$(id -u)" --build-arg GROUP_ID="$(id -g)" \
    docker || fail "docker build"
fi
if ! "$DOCKER" ps -a --format '{{.Names}}' | grep -qx qnx-build; then
  "$DOCKER" run -d --name qnx-build --platform linux/amd64 -v "$HOME:/home/$(id -un)" qnx800-build sleep infinity || fail "docker run"
fi
"$DOCKER" start qnx-build >/dev/null

step "5. Compile hello + timer200 and build minimal QNX VM image (inside container)"
"$DOCKER" exec qnx-build bash "/home/$(id -un)/Downloads/JobAgent/qnx/scripts/build_in_container.sh" || fail "build_in_container.sh"

step "6. Boot QNX VM in QEMU (x86_64 emulated, headless)"
if [ -f vm/qemu.pid ] && kill -0 "$(cat vm/qemu.pid)" 2>/dev/null; then kill "$(cat vm/qemu.pid)"; sleep 2; fi
EXTRA=""; [ -f vm/output/build/qemu_options ] && EXTRA="$(cat vm/output/build/qemu_options)"
: > logs/qemu_serial.log
qemu-system-x86_64 -machine pc -accel tcg,thread=multi -cpu max -smp 2 -m 1G \
  -drive file=vm/output/disk-qemu.vmdk,if=ide,id=drv0 \
  -kernel vm/output/ifs.bin \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:${SSH_PORT}-:22 -device virtio-net-pci,netdev=net0 \
  -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
  -display none -serial file:logs/qemu_serial.log -pidfile vm/qemu.pid -daemonize $EXTRA \
  || fail "qemu failed to start"
echo "QEMU pid $(cat vm/qemu.pid)"

step "7. Wait for QNX to boot and accept SSH (up to 5 min)"
ok=0
for i in $(seq 1 100); do
  if ssh "${SSH_OPTS[@]}" root@127.0.0.1 true 2>/dev/null; then ok=1; break; fi
  sleep 3
done
echo "--- serial console (last 40 lines) ---"; tail -40 logs/qemu_serial.log
[ $ok = 1 ] || fail "VM did not accept SSH"
ssh "${SSH_OPTS[@]}" root@127.0.0.1 'uname -a; pidin info | head -5'

step "8. Copy and run hello world"
scp "${SCP_OPTS[@]}" build/hello build/timer200 root@127.0.0.1:/tmp/ || fail "scp"
ssh "${SSH_OPTS[@]}" root@127.0.0.1 '/tmp/hello' | tee results/hello.txt

step "9. 200 Hz timer test, default priority (1000 samples = 5 s)"
ssh "${SSH_OPTS[@]}" root@127.0.0.1 '/tmp/timer200 5000 1000 0 /tmp/timer_default.csv' > results/timer_default.txt
tail -8 results/timer_default.txt

step "10. 200 Hz timer test, SCHED_FIFO priority 63"
ssh "${SSH_OPTS[@]}" root@127.0.0.1 '/tmp/timer200 5000 1000 63 /tmp/timer_fifo63.csv' > results/timer_fifo63.txt
tail -8 results/timer_fifo63.txt
scp "${SCP_OPTS[@]}" root@127.0.0.1:/tmp/timer_default.csv root@127.0.0.1:/tmp/timer_fifo63.csv results/ || true

echo; echo "VM is still running. Stop it with: kill \$(cat $PROJ/vm/qemu.pid)"
echo "STATUS=OK"
