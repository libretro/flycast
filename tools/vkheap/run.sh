#!/bin/sh
# The Vulkan device-memory suballocator that replaces VMA
# (core/rend/vulkan/vk_heap.c), built and run without a GPU: the driver
# entry points are stubs, so what runs is the arithmetic.
#
#   sh tools/vkheap/run.sh
#
# Built as C89 (gnu89 only because Vulkan's own headers use // comments),
# once plain and once under ASan + UBSan.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${TMPDIR:-/tmp}/vkheap_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

for san in "" "-fsanitize=address,undefined -fno-sanitize-recover=undefined"; do
   ${CC:-cc} -std=gnu89 -Wall -Wextra -O2 -g $san \
      -I "$ROOT/core/rend/vulkan" -I "$ROOT/core/deps/khronos" \
      -o "$WORK/vkheap_test" \
      "$ROOT/tools/vkheap/main.c" "$ROOT/core/rend/vulkan/vk_heap.c"
   "$WORK/vkheap_test"
done
