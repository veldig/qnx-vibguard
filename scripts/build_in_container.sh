#!/bin/bash
# Runs INSIDE the qnx-build container. Builds the QNX test programs and a minimal QNX VM image.
set -e
PROJ="$HOME/Downloads/JobAgent/qnx"
source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
echo "QNX_HOST=$QNX_HOST"
echo "QNX_TARGET=$QNX_TARGET"

echo "== Compiling test programs (x86_64 QNX target)"
mkdir -p "$PROJ/build"
qcc -Vgcc_ntox86_64 -O2 -Wall -o "$PROJ/build/hello"    "$PROJ/src/hello.c"
qcc -Vgcc_ntox86_64 -O2 -Wall -o "$PROJ/build/timer200" "$PROJ/src/timer200.c" -lm
file "$PROJ/build/hello" "$PROJ/build/timer200"

echo "== Building minimal QNX VM image with mkqnximage"
cd "$PROJ/vm"
mkqnximage --type=qemu --arch=x86_64 --hostname=qnxvg --ip=dhcp \
    --cpu=2 --ram=1G --ssh-ident="$PROJ/keys/qnx_vm_key.pub" --build
ls -la output | head -30
echo "BUILD_OK"
