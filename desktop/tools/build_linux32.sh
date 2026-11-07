#!/bin/sh
# Linux 32-bit test build (used to check the port headlessly; players use the
# Windows build). Needs gcc-multilib, a 32-bit SDL2 and 32-bit libGL.
#   SDL2_ROOT=<folder with usr/include/SDL2 and usr/lib/i386-linux-gnu> desktop/tools/build_linux32.sh [build_dir]
set -e
HERE=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$HERE/build-linux32}; [ $# -gt 0 ] && shift
SRC=$OUT/src
mkdir -p "$SRC"
# Case-fixed copy of the sources: the engine includes <core/defines.h> for Code/Core/Defines.h.
rm -rf "$SRC/Code" "$SRC/vita"; cp -a "$HERE/Code" "$HERE/vita" "$SRC/"
python3 "$HERE/desktop/tools/case_shadow.py" "$OUT/case_shadow" "$SRC/Code" "$SRC/Code/Sk" "$SRC/Code/Gfx/Vita" "$SRC/vita/src" "$HERE/desktop/shim" "$HERE/desktop/src"
SDL=${SDL2_ROOT:-/}
cmake -S "$HERE/desktop" -B "$OUT/cmake" -G Ninja \
  -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_EXE_LINKER_FLAGS=-m32 \
  -DTHUG_SRC_ROOT="$SRC" -DTHUG_CASE_SHADOW="$OUT/case_shadow" \
  -DSDL2_INCLUDE_DIRS="$SDL/usr/include/SDL2;$SDL/usr/include/i386-linux-gnu/SDL2;$SDL/usr/include/i386-linux-gnu" \
  -DSDL2_LIBRARIES="$SDL/usr/lib/i386-linux-gnu/libSDL2.so" \
  -DOPENGL_opengl_LIBRARY=/usr/lib/i386-linux-gnu/libGL.so.1 -DOPENGL_glx_LIBRARY=/usr/lib/i386-linux-gnu/libGLX.so.0 \
  -DOPENGL_gl_LIBRARY=/usr/lib/i386-linux-gnu/libGL.so.1 -DOPENGL_INCLUDE_DIR=/usr/include \
  "$@"
cmake --build "$OUT/cmake" -- -k 0
