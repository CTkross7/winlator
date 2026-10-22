#!/usr/bin/env bash
# Builds a 64-bit d3d11.dll from DXVK 1.10.3 that does not require VK_EXT_transform_feedback.
#
# The Mali-G72 Vulkan wrapper advertises VK_EXT_transform_feedback and reports transformFeedback/geometryStreams as
# supported, but vkCreateDevice then fails with VK_ERROR_FEATURE_NOT_PRESENT (-8). Stock DXVK 1.10.3 demands both for
# feature level 10_0, so D3D11 device creation always fails. Here the feature is no longer requested and the extension
# is no longer enabled; only geometry shader stream output is lost. The Wine 9.0 child window limitation is worked
# around with an overlay window, see ctblox.patch.
#
# usage: build_d3d11.sh <work dir> <output d3d11.dll>
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(realpath -m "$1")"
OUT="$(realpath -m "$2")"
SRC="$WORK/dxvk"

rm -rf "$WORK"
mkdir -p "$WORK"
git clone -q --depth 1 --branch v1.10.3 --recurse-submodules --shallow-submodules https://github.com/doitsujin/dxvk.git "$SRC"

cd "$SRC"

# ctblox.patch contains both fixes (see the header comment above and the notes below):
#  1. FL10_0 no longer asks for transform feedback and the extension is never enabled.
#  2. When vkCreateWin32SurfaceKHR fails (Wine 9.0 rejects child windows and windows that have children), the presenter
#     renders into a child-less popup window that is kept on top of the real window's client area.
git apply --whitespace=nowarn "$SCRIPT_DIR/ctblox.patch"

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
