#!/bin/sh
# Libraries the Brick has and the toolchain's sysroot has not, pulled over adb so a build can link against
# them, into TG5040_DEVLIBS (default build-brick/devlibs), run from the project's root. Never shipped: the
# device has them.
#   devlibs.sh SERIAL        (adb devices -l)
set -e
SERIAL=${1:?usage: devlibs.sh SERIAL}
OUT=${TG5040_DEVLIBS:-build-brick/devlibs}
mkdir -p "$OUT"
adb -s "$SERIAL" pull /usr/lib/libvulkan.so.1 $OUT/libvulkan.so.1 > /dev/null
echo "devlibs: $OUT/libvulkan.so.1 ($(wc -c < $OUT/libvulkan.so.1) bytes)"
