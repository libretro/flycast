#!/bin/sh
# Link-closure check for the statically linked core.
#
#   sh tools/static-link-check.sh <retroarch-tree>
#
# A static build (STATIC_LINKING=1, as libnx) leaves the libretro-common
# units to the frontend. This builds the core that way in a worktree,
# lists every symbol its archive still needs, and checks that each one is
# defined either in the archive itself or in <retroarch-tree>'s built
# objects. A unit the core needs and compiles only in its shared build is
# reported by name.
#
# Needs a RetroArch tree that has been built (its obj-*/ directory is
# what is searched). Fails rather than skips when there is none.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
RA=${1:?retroarch tree}
RA_OBJ=$(ls -d "$RA"/obj-* 2>/dev/null | head -n 1)
[ -n "$RA_OBJ" ] || { echo "$RA has no built objects" >&2; exit 1; }

WORK=${TMPDIR:-/tmp}/static_link_check.$$
trap 'cd "$ROOT"; git worktree remove --force "$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT
git -C "$ROOT" worktree add -q "$WORK" HEAD
git -C "$ROOT" diff | (cd "$WORK" && git apply --allow-empty) 2>/dev/null || true

make -C "$WORK" -j"${JOBS:-2}" STATIC_LINKING=1 >"$WORK/build.log" 2>&1 \
   || { tail -n 30 "$WORK/build.log"; echo "FAIL: static build"; exit 1; }
A=$(ls "$WORK"/*.a)

nm -g --defined-only "$A" 2>/dev/null | awk '$2 ~ /[TDBRW]/ {print $3}' | sort -u > "$WORK/have"
find "$RA_OBJ" -name '*.o' -exec nm -g --defined-only {} + 2>/dev/null \
   | awk '$2 ~ /[TDBRW]/ {print $3}' | sort -u >> "$WORK/have"
sort -u -o "$WORK/have" "$WORK/have"
# Only the symbols the core's own headers name: libc, GL and the C++
# runtime come from the toolchain.
nm -u "$A" 2>/dev/null | awk '{print $2}' | sort -u \
   | grep -E '^(core_|archive_|chd_image_|rchd_|r7z_|rzip_|rinflate_|rzstd_|rflac_|filestream_|retro_vfs_|path_|fill_pathname|string_|strlcpy|strlcat|encoding_|cpu_features_|retro_opendir|retro_readdir|retro_closedir|retro_dirent|sthread_|slock_|scond_|retro_eventcount|retro_spsc|memalign_|rtime_|utf8|utf16)' \
   > "$WORK/need" || true
MISSING=$(comm -23 "$WORK/need" "$WORK/have")
if [ -n "$MISSING" ]; then
   echo "FAIL: the static core needs symbols nothing provides:"
   echo "$MISSING" | sed 's/^/   /'
   exit 1
fi
echo "static-link-check: PASS ($(wc -l < "$WORK/need" | tr -d ' ') symbols resolved)"
