#!/bin/sh
# Windows build cross-compiled from Linux (MinGW-w64 i686). Needs the SDL2
# "devel ... mingw" package from https://github.com/libsdl-org/SDL/releases.
#   SDL2_MINGW=<SDL2-2.x.y/i686-w64-mingw32> desktop/tools/build_mingw32.sh [build_dir]
# Gives <build_dir>/cmake/thug.exe; ship it with SDL2.dll from $SDL2_MINGW/bin.
set -e
HERE=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$HERE/build-mingw32}; [ $# -gt 0 ] && shift
SRC=$OUT/src
mkdir -p "$SRC"
# Case-fixed copy of the sources (the Linux file system is case-sensitive).
rm -rf "$SRC/Code" "$SRC/vita"; cp -a "$HERE/Code" "$HERE/vita" "$SRC/"
python3 "$HERE/desktop/tools/case_shadow.py" "$OUT/case_shadow" "$SRC/Code" "$SRC/Code/Sk" "$SRC/Code/Gfx/Vita" "$SRC/vita/src" "$HERE/desktop/shim" "$HERE/desktop/src"
cmake -S "$HERE/desktop" -B "$OUT/cmake" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$HERE/desktop/tools/mingw32.cmake" \
  -DTHUG_SRC_ROOT="$SRC" -DTHUG_CASE_SHADOW="$OUT/case_shadow" \
  -DSDL2_INCLUDE_DIRS="$SDL2_MINGW/include/SDL2" \
  -DSDL2_LIBRARIES="mingw32;$SDL2_MINGW/lib/libSDL2.dll.a" \
  "$@"
cmake --build "$OUT/cmake" -- -k 0
cp "$SDL2_MINGW/bin/SDL2.dll" "$OUT/cmake/"
