#!/bin/bash
# Runs INSIDE the qnx-build container: compiles vibguard for x86_64 and aarch64 QNX.
set -e
PROJ="$HOME/Downloads/JobAgent/qnx"
source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
mkdir -p "$PROJ/build" "$PROJ/build-arm"
qcc -Vgcc_ntox86_64    -O2 -Wall -o "$PROJ/build/vibguard"     "$PROJ/src/vibguard.c" -lm
qcc -Vgcc_ntoaarch64le -O2 -Wall -o "$PROJ/build-arm/vibguard" "$PROJ/src/vibguard.c" -lm
file "$PROJ/build/vibguard" "$PROJ/build-arm/vibguard"
echo VIBGUARD_BUILD_OK
