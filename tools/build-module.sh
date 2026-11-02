#!/bin/sh
# Build the recomp module in the current folder, from your own disc, into build-brick/<id>/: what Deadcast
# installs.
#   sdk/tools/build-module.sh /path/to/disc.chd [ninja arguments]
# Needs the toolchain and the device's libraries (ui/toolchain/toolchain.sh and devlibs.sh; TG5040_ROOT and
# TG5040_DEVLIBS if not in build-brick), the host's curl and Vulkan headers, Python 3.11+, glslc and llvm-strip.
# GEN_OPT=-O2 (or -O3) compiles the translated code at that level instead of -O1.
set -e
SDK=$(cd "$(dirname "$0")/.." && pwd)
MODULE=$PWD
DISC=${1:?usage: sdk/tools/build-module.sh DISC [ninja arguments]}
shift
[ -f module.cfg ] || { echo "build-module: no module.cfg in this folder"; exit 2; }
M=$(sed -n 's/^id *= *//p' module.cfg)
EXEC=$(sed -n 's/^exec *= *//p' module.cfg)
TC=${TG5040_ROOT:-$PWD/build-brick/toolchain}
DEVLIBS=${TG5040_DEVLIBS:-$PWD/build-brick/devlibs}
[ -d "$TC/aarch64-nextui-linux-gnu/libc" ] || { echo "build-module: no toolchain (ui/toolchain/toolchain.sh)"; exit 2; }
for lib in libvulkan.so.1 libcurl.so.4; do
    [ -f "$DEVLIBS/$lib" ] || { echo "build-module: no $DEVLIBS/$lib (ui/toolchain/devlibs.sh SERIAL)"; exit 2; }
done
[ -n "$(ls -A "$SDK/ui/third_party/freetype" 2>/dev/null)" ] || { echo "build-module: submodules missing (git submodule update --init --recursive)"; exit 2; }

# the translator, for this machine
H=build-brick/host
mkdir -p build-brick
cmake -S "$SDK" -B "$H" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDREAM_RENDERER=OFF \
    -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON > "$H.configure.log"
# and libchdr, through which dc.py reads the disc
ninja -C "$H" dream-translate chdr
TRANSLATE="$PWD/$H/engine/translator/dream-translate"
LIBCHDR="$(find "$PWD/$H" -name 'libchdr.so*' -type f | head -n 1)"

# the disc: checked against the module's game config, its boot file and code overlays extracted
# (game/extracted: the game's own code, never in git)
DC_PORT="$MODULE" DCDISC_LIBCHDR="$LIBCHDR" python3 "$SDK/tools/dc.py" setup --disc "$DISC"

# the module, for the handheld; pkg-config sees the sysroot's packages only, never this machine's
SYSROOT="$TC/aarch64-nextui-linux-gnu/libc"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
unset PKG_CONFIG_PATH
B=build-brick/build
mkdir -p build-brick/include
rm -rf build-brick/include/curl build-brick/include/vulkan build-brick/include/vk_video
cp -r /usr/include/curl /usr/include/vulkan /usr/include/vk_video build-brick/include/
cmake -S "$SDK" -B "$B" -G Ninja \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG" -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG" \
    -DCMAKE_TOOLCHAIN_FILE="$SDK/ui/toolchain/tg5040.cmake" \
    -DTG5040_ROOT="$TC" \
    -DCMAKE_BUILD_TYPE=Release \
    -DDREAMCOMP_NEXTUI=ON \
    -DDREAMCOMP_MODULE_DIR="$MODULE" \
    -DDREAM_RENDERER=ON \
    -DVulkan_INCLUDE_DIR="$PWD/build-brick/include" \
    -DVulkan_LIBRARY="$DEVLIBS/libvulkan.so.1" \
    -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON \
    -DDREAM_TRANSLATE_EXECUTABLE="$TRANSLATE" \
    -DDREAM_GEN_OPT="${GEN_OPT:--O1}" \
    -DCURL_INCLUDE_DIR="$PWD/build-brick/include" \
    -DCURL_LIBRARY="$DEVLIBS/libcurl.so.4" > "$B.configure.log"
ninja -C "$B" "$@" "${M}_boot"

# as Deadcast installs it
OUT="build-brick/$M"
rm -rf "$OUT"
mkdir -p "$OUT/licenses"
llvm-strip -o "$OUT/$EXEC" "$B/module/$EXEC"
chmod +x "$OUT/$EXEC"
cp module.cfg store.json "$OUT/"
cp "game/$M.toml" "$OUT/"
[ -f "game/$M-pipelines.txt" ] && cp "game/$M-pipelines.txt" "$OUT/"
cp "$SDK/LICENSE" "$OUT/licenses/deadcast-recomp-GPL-2.0.txt"
cp "$SDK/THIRD_PARTY_NOTICES.md" "$OUT/licenses/THIRD_PARTY_NOTICES.md"
cp "$SDK/engine/third_party/libchdr/LICENSE.txt" "$OUT/licenses/libchdr-BSD-3-Clause.txt"
cp "$SDK/engine/third_party/libchdr/deps/lzma-26.02/LICENSE" "$OUT/licenses/lzma.txt"
cp "$SDK/engine/third_party/tomlplusplus/LICENSE" "$OUT/licenses/tomlplusplus-MIT.txt"
cp "$SDK/ui/LICENSE" "$OUT/licenses/deadui-MIT.txt"
cp "$SDK/ui/fonts/NotoSans-OFL.txt" "$OUT/licenses/NotoSans-OFL-1.1.txt"
echo "build-module: $OUT ($(du -sh "$OUT" | cut -f1))"
