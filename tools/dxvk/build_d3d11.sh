#!/usr/bin/env bash
# Builds a 64-bit d3d11.dll from DXVK 1.10.3 that does not require VK_EXT_transform_feedback.
#
# The Mali-G72 Vulkan wrapper advertises VK_EXT_transform_feedback and reports transformFeedback/geometryStreams as
# supported, but vkCreateDevice then fails with VK_ERROR_FEATURE_NOT_PRESENT (-8). Stock DXVK 1.10.3 demands both for
# feature level 10_0, so D3D11 device creation always fails. Here the feature is no longer requested and the extension
# is no longer enabled; only geometry shader stream output is lost.
#
# usage: build_d3d11.sh <work dir> <output d3d11.dll>
set -euo pipefail

WORK="$(realpath -m "$1")"
OUT="$(realpath -m "$2")"
SRC="$WORK/dxvk"

rm -rf "$WORK"
mkdir -p "$WORK"
git clone -q --depth 1 --branch v1.10.3 --recurse-submodules --shallow-submodules https://github.com/doitsujin/dxvk.git "$SRC"

cd "$SRC"

# 1. FL10_0 must not ask for transform feedback.
sed -i '/enabled\.extTransformFeedback\.transformFeedback  *= VK_TRUE;/d; /enabled\.extTransformFeedback\.geometryStreams  *= VK_TRUE;/d' src/d3d11/d3d11_device.cpp
# 2. Never enable the extension, so no transform feedback feature struct reaches vkCreateDevice.
sed -i 's/\(DxvkExt extTransformFeedback  *= { VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME, *\)DxvkExtMode::Optional/\1DxvkExtMode::Disabled/' src/dxvk/dxvk_extensions.h

grep -q "extTransformFeedback.transformFeedback  *= VK_TRUE" src/d3d11/d3d11_device.cpp && { echo "patch 1 not applied"; exit 1; }
grep -q "VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME, *DxvkExtMode::Disabled" src/dxvk/dxvk_extensions.h || { echo "patch 2 not applied"; exit 1; }

cat > crossfile.txt <<'CROSS'
[binaries]
c = 'x86_64-w64-mingw32-gcc-posix'
cpp = 'x86_64-w64-mingw32-g++-posix'
ar = 'x86_64-w64-mingw32-ar'
strip = 'x86_64-w64-mingw32-strip'
windres = 'x86_64-w64-mingw32-windres'

[built-in options]
# DXVK 1.10.3 predates GCC 13, which no longer pulls in <cstdint> implicitly.
cpp_args = ['-include', 'cstdint']

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
CROSS

meson setup --cross-file crossfile.txt --buildtype release --strip -Denable_d3d9=false -Denable_d3d10=false build
ninja -C build src/d3d11/d3d11.dll

mkdir -p "$(dirname "$OUT")"
cp build/src/d3d11/d3d11.dll "$OUT"
ls -la "$OUT"
