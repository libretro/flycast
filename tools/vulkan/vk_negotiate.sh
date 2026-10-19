#!/bin/sh
# The Vulkan device, negotiated with the core as RetroArch does (v2: made
# through the frontend's vkCreateDevice wrapper, which adds what the
# frontend needs, e.g. for Driver Pacing) and as a frontend without v2
# does (v1: create_device), under the validation layer. See vk_negotiate.c.
#
#   sh tools/vulkan/vk_negotiate.sh [core.so]
#
# Needs the core built with Vulkan and a Vulkan driver (lavapipe will do:
# VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json). No BIOS or game:
# the core asks for its context before booting, and the device is made
# whether the boot then succeeds or not.
set -e
cd "$(dirname "$0")/../.."
CORE=$(readlink -f "${1:-flycast_libretro.so}")
WORK=${TMPDIR:-/tmp}/vk_negotiate.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
${CC:-cc} -O1 -g -Wall -I./core/libretro-common/include -I./core/deps/khronos \
   -o "$WORK/vk_negotiate" tools/vulkan/vk_negotiate.c -ldl
head -c 4096 /dev/zero > "$WORK/blank.cdi"
for offer in 2 1; do
   expect=0
   [ "$offer" -ge 2 ] && expect=1
   VKN_OFFER=$offer VKN_EXPECT_WRAPPER=$expect VKN_LOAD_MAY_FAIL=1 VKN_DIR="$WORK" \
      "$WORK/vk_negotiate" "$CORE" "$WORK/blank.cdi" 2>"$WORK/log.txt" || { cat "$WORK/log.txt"; exit 1; }
done
