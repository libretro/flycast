#!/bin/sh
# CHD read regression test.
#
#   sh tools/chd/run.sh
#
# Writes synthetic images with tools/chd/make_fixture.py -- a two-track CD
# (CUE + BIN) and a three-track GD-ROM (GDI) -- compresses the CD with each
# CD codec family chdman offers (zstd cdzs, LZMA cdlz, zlib cdzl, FLAC cdfl)
# and as a parent/child pair, and the GD-ROM with zstd and LZMA, then checks
# every frame of each CHD against the files it came from with
# chd_image_test, built under AddressSanitizer + UBSan with leak detection.
#
# The preprocessor flags are read back out of the core's own build, so the
# test decodes with exactly the codec set the core is built with.
#
# Needs chdman (MAME tools) and python3. Fails rather than skips when either
# is missing: a CHD check that quietly does nothing is how a codec stops
# working without anyone noticing.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

command -v chdman >/dev/null || { echo "chdman not found" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 1; }

CC=${CC:-cc}
DEFS=$(make -n -B core/imgread/chd_image.o | grep -o -- '-D[A-Za-z0-9_]*' \
       | grep -v GIT_VERSION | sort -u | tr '\n' ' ')
case "$DEFS" in
   *-DHAVE_CHD*) ;;
   *) echo "core build does not define HAVE_CHD" >&2; exit 1 ;;
esac

L=core/libretro-common
WORK=${TMPDIR:-/tmp}/chd_image_test.$$
mkdir -p "$WORK/gd"
trap 'rm -rf "$WORK"' EXIT

# Built twice: with the core's flags the decoder is lent the mapped file,
# without HAVE_MMAP every request is read into the bounce buffer.
NOMAP_DEFS=$(echo "$DEFS" | sed 's/-DHAVE_MMAP //')
for v in map nomap; do
if [ $v = map ]; then VDEFS=$DEFS; else VDEFS=$NOMAP_DEFS; fi
$CC -O1 -g $VDEFS -fsanitize=address,undefined \
   -fno-sanitize-recover=undefined -I$L/include -Icore \
   -o "$WORK/chd_image_test_$v" \
   tools/chd/chd_image_test.c core/imgread/chd_image.c \
   core/deps/coreio/coreio.c core/archive/archive.c \
   $L/formats/chd/rchd.c $L/encodings/encoding_huffman.c \
   $L/encodings/encoding_rzstd.c $L/encodings/encoding_deflate.c \
   $L/encodings/encoding_crc32.c $L/formats/7z/r7z_lzma.c \
   $L/formats/7z/r7z_archive.c $L/formats/7z/r7z_lzma2.c \
   $L/formats/7z/r7z_lzma_stream.c $L/formats/7z/r7z_bcj2.c \
   $L/formats/7z/r7z_filters.c $L/formats/zip/rzip_archive.c \
   $L/formats/flac/rflac.c $L/features/features_cpu.c \
   $L/streams/file_stream.c $L/vfs/vfs_implementation.c \
   $L/file/file_path.c $L/file/file_path_io.c $L/file/retro_dirent.c \
   $L/compat/compat_strl.c $L/compat/fopen_utf8.c \
   $L/compat/compat_strcasestr.c $L/encodings/encoding_utf.c \
   $L/string/stdstring.c $L/string/rstrtod.c $L/time/rtime.c \
   $L/memmap/memalign.c \
   -lm -lpthread
done

python3 tools/chd/make_fixture.py "$WORK/base"
python3 tools/chd/make_fixture.py "$WORK/child" --variant 1
python3 tools/chd/make_fixture.py "$WORK/gd/disc" --gdi

for c in cdzs cdlz cdzl cdfl; do
   case $c in
      cdzs) name="CD Zstandard" ;;
      cdlz) name="CD LZMA" ;;
      cdzl) name="CD Deflate" ;;
      cdfl) name="CD FLAC" ;;
   esac
   chdman createcd -f -c "$c" -i "$WORK/base.cue" -o "$WORK/$c.chd" \
      >/dev/null 2>&1
   # A codec that fell back to storing hunks uncompressed would leave
   # nothing for this test to decode, so its row in the hunk table has to
   # be there.
   chdman info -v -i "$WORK/$c.chd" | grep -q "%  $name" \
      || { echo "$c.chd holds no $name hunks" >&2; exit 1; }
done
for c in cdzs cdlz; do
   chdman createcd -f -c "$c" -i "$WORK/gd/disc.gdi" -o "$WORK/gd_$c.chd" \
      >/dev/null 2>&1
   chdman info -v -i "$WORK/gd_$c.chd" | grep -q "Tag='CHGD'" \
      || { echo "gd_$c.chd has no GD-ROM track metadata" >&2; exit 1; }
done

# The child stores the hunks it changed and copies the rest from the
# parent, so most of its frames are decoded out of parent.chd.
mkdir -p "$WORK/chain"
chdman createcd -f -c cdzs -i "$WORK/base.cue" -o "$WORK/chain/parent.chd" \
   >/dev/null 2>&1
chdman createcd -f -c cdzs,cdlz -i "$WORK/child.cue" \
   -op "$WORK/chain/parent.chd" -o "$WORK/chain/child.chd" >/dev/null 2>&1
chdman info -v -i "$WORK/chain/child.chd" | grep -q "%  Copy from parent" \
   || { echo "child.chd copies nothing from its parent" >&2; exit 1; }

export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1
fail=0
check() {
   for v in map nomap; do
      "$WORK/chd_image_test_$v" "$@" | tail -1 | grep -q PASS \
         || { echo "FAIL ($v): $1"; fail=1; }
   done
}
for c in cdzs cdlz cdzl cdfl; do
   check "$WORK/$c.chd" "$WORK/base.bin"
done
for c in cdzs cdlz; do
   check "$WORK/gd_$c.chd" "$WORK/gd/track01.bin" "$WORK/gd/track02.raw" \
      "$WORK/gd/track03.bin"
done
check "$WORK/chain/child.chd" "$WORK/child.bin"

if [ $fail = 0 ]; then
   echo "chd_image_test: all images PASS (mapped and bounce-buffer builds)"
else
   exit 1
fi
