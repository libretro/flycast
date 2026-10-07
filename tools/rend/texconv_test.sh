#!/bin/sh
# The texture converters, byte for byte against upstream's.
#
#   sh tools/rend/texconv_test.sh
#
# Needs the core built for this machine first (it links core/rend/TexCache.o).
# See texconv_test.cpp.
set -e
cd "$(dirname "$0")/../.."
[ -f core/rend/TexCache.o ] || { echo "build the core first: core/rend/TexCache.o is not there" >&2; exit 1; }
WORK=${TMPDIR:-/tmp}/texconv_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
${CXX:-g++} -I./core/libretro -I./core -I./core/deps -I./core/libretro-common/include -I./core/deps/khronos \
   -DTARGET_LINUX_x64 -DHOST_CPU=0x20000004 -D__LIBRETRO__ -DHAVE_GLSYM_PRIVATE -DNO_VERIFY -DNDEBUG \
   -DHAVE_GL3 -DHAVE_GL4 -DCORE -DHAVE_TEXUPSCALE -DHAVE_OPENGL -DHAVE_VULKAN -O2 -fpermissive -fopenmp -w \
   -o "$WORK/texconv_test" tools/rend/texconv_test.cpp core/rend/TexCache.o -Wl,--unresolved-symbols=ignore-all
(echo "# random"; "$WORK/texconv_test"; echo "# ramp"; "$WORK/texconv_test" ramp) > "$WORK/out.txt"
if cmp -s "$WORK/out.txt" tools/rend/texconv_expected.txt; then
   echo "texture converters: $(grep -vc '^#' "$WORK/out.txt") results, all as upstream's"
else
   echo "FAIL: texture converters differ from upstream's:" >&2
   diff "$WORK/out.txt" tools/rend/texconv_expected.txt | head -20 >&2
   exit 1
fi
