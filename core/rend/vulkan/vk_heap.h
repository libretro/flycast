/*
 * Vulkan device memory, taken from the driver in large blocks and handed
 * out as offsets inside them. In C.
 *
 * It replaces the Vulkan Memory Allocator, which is twenty thousand lines
 * of C++ header doing a great deal this renderer never asks of it. What
 * the renderer asks is small: memory for an image or a buffer, the pointer
 * if it is host-visible, and to give it back. So that is all there is.
 *
 *  - Blocks come from the driver, are large, and stay for the life of the
 *    heap; a frame that allocates is an offset inside a VkDeviceMemory
 *    that already exists, not a call into the driver.
 *  - An allocation is the lowest free span of its block that fits, at the
 *    alignment the requirement asks for. Each block keeps its free spans
 *    in an array sorted by offset, so freeing is a binary search and a
 *    merge with the span on either side: no list to walk, nothing
 *    allocated per allocation, and no slivers left behind.
 *  - A host-visible block is mapped once when it is taken and stays
 *    mapped. An allocation in it comes with its pointer; nothing maps or
 *    unmaps per use.
 *  - Buffers and images whose tiling is optimal never share a block, which
 *    is how bufferImageGranularity is honoured without padding anything.
 *
 * The Vulkan entry points are passed in rather than linked against, so
 * tools/vkheap can run the whole thing against a stub driver.
 *
 * Not thread safe; the renderer is one thread.
 */

#ifndef FLYCAST_VK_HEAP_H
#define FLYCAST_VK_HEAP_H

#include <stddef.h>
#include <stdint.h>

/* Only the types are needed. Where the Vulkan headers are in already
 * they are left as they were set up. */
#ifndef VULKAN_CORE_H_
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The entry points the heap uses. */
typedef struct vk_heap_fns
{
   PFN_vkAllocateMemory allocate_memory;
   PFN_vkFreeMemory     free_memory;
   PFN_vkMapMemory      map_memory;
   PFN_vkUnmapMemory    unmap_memory;
} vk_heap_fns_t;

/* Where something ended up. offset is inside memory; mapped already has
 * offset added, and is NULL when the memory is not host-visible. */
typedef struct vk_heap_alloc
{
   VkDeviceMemory memory;
   VkDeviceSize   offset;
   VkDeviceSize   size;
   void          *mapped;
   unsigned       block;
} vk_heap_alloc_t;

/* A span of free memory inside a block. */
typedef struct vk_heap_span
{
   VkDeviceSize offset;
   VkDeviceSize size;
} vk_heap_span_t;

typedef struct vk_heap_block
{
   VkDeviceMemory  memory;
   void           *mapped;        /* the whole block, or NULL */
   VkDeviceSize    size;
   VkDeviceSize    used;
   /* No free span is larger than this. Exact after a search that found
    * nothing, an upper bound otherwise; a full block is skipped on it
    * without its spans being read. */
   VkDeviceSize    max_free;
   vk_heap_span_t *spans;         /* free, sorted by offset, never adjacent */
   unsigned        span_count;
   unsigned        span_capacity;
   unsigned        type;          /* memory type index */
   unsigned        linear;        /* buffers and linear images, or optimal images */
} vk_heap_block_t;

#define VK_HEAP_MAX_BLOCKS 64

typedef struct vk_heap
{
   VkDevice                         device;
   vk_heap_fns_t                    fns;
   VkPhysicalDeviceMemoryProperties props;
   VkDeviceSize                     block_size;
   VkDeviceSize                     max_bytes;      /* never asks past this; 0: no limit */
   VkDeviceSize                     bytes_reserved; /* asked of the driver */
   VkDeviceSize                     bytes_used;     /* handed out */
   vk_heap_block_t                  blocks[VK_HEAP_MAX_BLOCKS];
   unsigned                         block_count;
   VkResult                         last_error;     /* why the last block could not be had */
} vk_heap_t;

/* block_size is what one VkDeviceMemory is; anything larger gets a block
 * of its own. Returns 0 on failure. */
int  vk_heap_init(vk_heap_t *heap, VkDevice device,
      const VkPhysicalDeviceMemoryProperties *props,
      const vk_heap_fns_t *fns, VkDeviceSize block_size,
      VkDeviceSize max_bytes);

/* Gives every block back to the driver. */
void vk_heap_shutdown(vk_heap_t *heap);

/* Takes blocks from the driver now, so that later allocations of this
 * kind are offsets. Arguments as for vk_heap_alloc. Returns how many
 * blocks were taken. */
unsigned vk_heap_reserve(vk_heap_t *heap, uint32_t type_bits,
      VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
      int linear, unsigned blocks);

/* Finds room for something with these requirements. required is what the
 * memory must be; preferred is tried first and dropped when no memory
 * type has it or the driver has none of it left. Every memory type that
 * will do is tried, in that order. linear is non-zero for a buffer or an
 * image with linear tiling.
 *
 * With VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT in required, out->mapped is an
 * address: memory that could not be mapped is not handed out. Where the
 * bit is only in preferred, or not asked for, out->mapped may be NULL.
 *
 * Returns 0 when there is no room and no block could be taken;
 * heap->last_error is then what the driver last said. */
int  vk_heap_alloc(vk_heap_t *heap, const VkMemoryRequirements *req,
      VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
      int linear, vk_heap_alloc_t *out);

void vk_heap_free(vk_heap_t *heap, const vk_heap_alloc_t *alloc);

/* Gives the blocks that are empty back to the driver. Returns how many. */
unsigned vk_heap_trim(vk_heap_t *heap);

/* The same, but for the last block of each kind (memory type, and linear
 * or not): where what was large has become small - a render size turned
 * down - and the room it took is not coming back into use, while the next
 * allocation of every kind still finds a block and is an offset. Returns
 * how many blocks were given back. */
unsigned vk_heap_trim_spare(vk_heap_t *heap);

#ifdef __cplusplus
}
#endif

#endif
