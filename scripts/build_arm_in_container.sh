#!/bin/bash
# Runs INSIDE the qnx-build container. ARM64 (aarch64le) build: test programs + minimal QNX VM image.
set -e
PROJ="$HOME/Downloads/JobAgent/qnx"
source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
for f in aarch64le/boot/sys/startup-qemu-virt aarch64le/sbin/devb-virtio aarch64le/lib/dll/devs-vtnet_mmio.so; do
  [ -e "$QNX_TARGET/$f" ] && echo "found $f" || { echo "MISSING $QNX_TARGET/$f"; exit 2; }
done
echo "== Compiling test programs (aarch64le QNX target)"
mkdir -p "$PROJ/build-arm"
qcc -Vgcc_ntoaarch64le -O2 -Wall -o "$PROJ/build-arm/hello"    "$PROJ/src/hello.c"
qcc -Vgcc_ntoaarch64le -O2 -Wall -o "$PROJ/build-arm/timer200" "$PROJ/src/timer200.c" -lm
file "$PROJ/build-arm/hello" "$PROJ/build-arm/timer200"
echo "== Building minimal ARM64 QNX VM image with mkqnximage"
mkdir -p "$PROJ/vm-arm"; cd "$PROJ/vm-arm"
mkqnximage --type=qemu --arch=aarch64le --hostname=qnxvg-arm --ip=dhcp \
    --cpu=2 --ram=1G --ssh-ident="$PROJ/keys/qnx_vm_key.pub" --build
ls -la output | head -30
echo "BUILD_OK"
