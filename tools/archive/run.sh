#!/bin/sh
# Archive and coreio regression test.
#
#   sh tools/archive/run.sh
#
# Packages the GD-ROM fixture of tools/chd/make_fixture.py as stored and
# deflated zips, a solid and a single-member 7z (tools/archive/make_fixture.py,
# needs python3 with py7zr), then checks every member through the archive
# API and through core_fopen("archive#member") against the source files,
# with archive_test built under AddressSanitizer + UBSan with leak detection.
# The test is built twice: with HAVE_MMAP, so stored members are served out
# of the mapping, and without, so they are read in place and deflated
# members are inflated from chunked reads.
#
# Then load_test loads the built core (default: flycast_libretro.so, or the
# path given as the first argument) and runs retro_load_game() on the same
# fixture as a plain .gdi and inside each archive, and on an archive with
# no disc image in it, which has to be refused.
#
# rar_test runs libretro-common's RAR reader over the archives of
# tools/archive/rar - LZ and PPMd blocks, filters, Unicode names - and over
# mangled copies of them.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
CORE=${1:-$ROOT/flycast_libretro.so}
[ -f "$CORE" ] || { echo "$CORE not found: build the core first" >&2; exit 1; }

command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 1; }
python3 -c 'import py7zr' 2>/dev/null \
   || { echo "python3 module py7zr not found (pip install py7zr)" >&2; exit 1; }

CC=${CC:-cc}
L=core/libretro-common
WORK=${TMPDIR:-/tmp}/archive_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

SOURCES="tools/archive/archive_test.c core/archive/archive.c \
   core/deps/coreio/coreio.c \
   $L/encodings/encoding_deflate.c $L/encodings/encoding_crc32.c \
   $L/encodings/encoding_utf.c \
   $L/formats/7z/r7z_archive.c $L/formats/7z/r7z_lzma.c \
   $L/formats/7z/r7z_lzma2.c $L/formats/7z/r7z_lzma_stream.c \
   $L/formats/7z/r7z_bcj2.c $L/formats/7z/r7z_filters.c \
   $L/formats/zip/rzip_archive.c \
   $L/formats/rar/rrar_archive.c $L/formats/rar/rrar_ppmd7.c \
   $L/features/features_cpu.c \
   $L/rthreads/rthreads.c $L/rthreads/retro_eventcount.c \
   $L/streams/file_stream.c $L/vfs/vfs_implementation.c \
   $L/file/file_path.c $L/file/file_path_io.c $L/file/retro_dirent.c \
   $L/compat/compat_strl.c $L/compat/fopen_utf8.c \
   $L/compat/compat_strcasestr.c \
   $L/string/stdstring.c $L/string/rstrtod.c $L/time/rtime.c \
   $L/memmap/memalign.c"

# The _seek builds read every deflated zip member through the index a
# large one is read through (ARCHIVE_SEEK_MIN lowered to take them all):
# mapped, where a thread of the archive's makes the index while the test
# reads, and not mapped, where the reader makes it as it goes. The mapped
# one is built for TSan as well.
for mode in mmap nommap mmap_seek nommap_seek; do
   case $mode in
      mmap)        defs=-DHAVE_MMAP ;;
      nommap)      defs= ;;
      mmap_seek)   defs="-DHAVE_MMAP -DARCHIVE_SEEK_MIN=4096" ;;
      nommap_seek) defs=-DARCHIVE_SEEK_MIN=4096 ;;
   esac
   $CC -O1 -g $defs -DHAVE_THREADS -fsanitize=address,undefined \
      -fno-sanitize-recover=undefined -I$L/include -Icore \
      -o "$WORK/archive_test_$mode" $SOURCES -lm -lpthread
done
$CC -O1 -g -DHAVE_MMAP -DARCHIVE_SEEK_MIN=4096 -DHAVE_THREADS -fsanitize=thread \
   -I$L/include -Icore -o "$WORK/archive_test_tsan" $SOURCES -lm -lpthread

# libretro-common's index itself (rzip_seek), against whole extraction
$CC -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -I$L/include -o "$WORK/seek_test" tools/archive/seek_test.c \
   $L/formats/zip/rzip_archive.c $L/encodings/encoding_deflate.c \
   $L/encodings/encoding_crc32.c $L/features/features_cpu.c -lm -lpthread

# libretro-common's RAR reader on real archives (tools/archive/rar), whole
# and mangled
$CC -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
   -I$L/include -o "$WORK/rar_test" tools/archive/rar_test.c \
   $L/formats/rar/rrar_archive.c $L/formats/rar/rrar_ppmd7.c \
   $L/encodings/encoding_crc32.c $L/features/features_cpu.c \
   $L/compat/compat_strl.c -lm -lpthread

case $(uname -s) in
   MINGW*|MSYS*) LDL= ;;
   *)            LDL=-ldl ;;
esac
$CC -O1 -g -I$L/include -o "$WORK/load_test" tools/archive/load_test.c \
   $L/compat/compat_strl.c $LDL

python3 tools/archive/make_fixture.py "$WORK/fx"
mkdir -p "$WORK/fx/dc"

export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1
fail=0
for mode in mmap nommap mmap_seek nommap_seek; do
   "$WORK/archive_test_$mode" "$WORK/fx" | tail -1 | grep -q PASS \
      || { echo "FAIL: $mode build"; fail=1; }
done
TSAN_OPTIONS=halt_on_error=1 "$WORK/archive_test_tsan" "$WORK/fx" | tail -1 | grep -q PASS \
   || { echo "FAIL: indexed members under TSan"; fail=1; }
for z in deflate.zip subdir.zip; do
   "$WORK/seek_test" "$WORK/fx/$z" | tail -1 | grep -q "seek_test: ok" \
      || { echo "FAIL: seek_test $z"; fail=1; }
done

"$WORK/rar_test" tools/archive/rar | tail -1 | grep -q PASS \
   || { echo "FAIL: rar_test"; fail=1; }

"$WORK/load_test" "$CORE" "$WORK/fx" > "$WORK/load.log" 2>&1 \
   && tail -1 "$WORK/load.log" | grep -q PASS \
   || { cat "$WORK/load.log"; echo "FAIL: load_test"; fail=1; }

if [ $fail = 0 ]; then
   echo "archive_test: PASS (mmap and no-mmap builds, indexed members with and without a thread and under TSan, rar reader, core loads)"
else
   exit 1
fi
