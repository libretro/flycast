/* See vk_heap.h. */

#include <stdlib.h>
#include <string.h>

#include "vk_heap.h"

#define VK_HEAP_MIN_SPANS 16

/* alignment is a power of two; Vulkan says so. */
#define VK_HEAP_ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((a) - 1))

/* The memory types an allocation may come from, best first: those that
 * have the preferred flags as well, then the others that meet the
 * requirement - each group in the driver's own order, which is its order
 * of preference. Returns how many; *with_preferred is how many of them
 * are of the first group. */
static unsigned vk_heap_types(const vk_heap_t *heap, uint32_t type_bits,
      VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
      unsigned char *types, unsigned *with_preferred)
{
   const VkMemoryPropertyFlags both = required | preferred;
   unsigned count = 0;
   unsigned i;

   if (preferred)
      for (i = 0; i < heap->props.memoryTypeCount && i < VK_MAX_MEMORY_TYPES; i++)
         if ((type_bits & (1u << i))
               && (heap->props.memoryTypes[i].propertyFlags & both) == both)
            types[count++] = (unsigned char)i;
   *with_preferred = count;
   for (i = 0; i < heap->props.memoryTypeCount && i < VK_MAX_MEMORY_TYPES; i++)
   {
      const VkMemoryPropertyFlags flags = heap->props.memoryTypes[i].propertyFlags;

      if (!(type_bits & (1u << i)) || (flags & required) != required)
         continue;
      if (preferred && (flags & both) == both)
         continue;      /* in the first group already */
      types[count++] = (unsigned char)i;
   }
   return count;
}

/* What one block of a memory type is: the heap's block size, but no more
 * than an eighth of the driver's heap that type is in - a block sized by
 * the largest heap can be most of a small one - and no less than what
 * is asked for. */
static VkDeviceSize vk_heap_block_size(const vk_heap_t *heap, unsigned type,
      VkDeviceSize need)
{
   const unsigned heap_index = heap->props.memoryTypes[type].heapIndex;
   VkDeviceSize size         = heap->block_size;

   if (heap_index < heap->props.memoryHeapCount)
   {
      const VkDeviceSize eighth = heap->props.memoryHeaps[heap_index].size / 8u;

      if (eighth && eighth < size)
         size = eighth;
   }
   if (size < need)
      size = need;
   return VK_HEAP_ALIGN_UP(size, (VkDeviceSize)(64u * 1024u));
}

/* Makes room for one more span at index at. */
static int vk_heap_block_insert(vk_heap_block_t *b, unsigned at,
      VkDeviceSize offset, VkDeviceSize size)
{
   if (b->span_count == b->span_capacity)
   {
      const unsigned  cap   = b->span_capacity ? b->span_capacity * 2u : VK_HEAP_MIN_SPANS;
      vk_heap_span_t *grown = (vk_heap_span_t*)realloc(b->spans, cap * sizeof(vk_heap_span_t));

      if (!grown)
         return 0;
      b->spans         = grown;
      b->span_capacity = cap;
   }
   if (at < b->span_count)
      memmove(&b->spans[at + 1], &b->spans[at],
            (b->span_count - at) * sizeof(vk_heap_span_t));
   b->spans[at].offset = offset;
   b->spans[at].size   = size;
   b->span_count++;
   return 1;
}

static void vk_heap_block_remove(vk_heap_block_t *b, unsigned at)
{
   b->span_count--;
   if (at < b->span_count)
      memmove(&b->spans[at], &b->spans[at + 1],
            (b->span_count - at) * sizeof(vk_heap_span_t));
}

static void vk_heap_block_release(vk_heap_t *heap, vk_heap_block_t *b)
{
   if (b->mapped)
      heap->fns.unmap_memory(heap->device, b->memory);
   heap->fns.free_memory(heap->device, b->memory, NULL);
   free(b->spans);
   heap->bytes_reserved -= b->size;
   /* A hole, not a gap closed up: every live allocation carries the
    * index of its block. The slot is reused by the next block. */
   memset(b, 0, sizeof(*b));
}

/* One block from the driver, whole and mapped if it can be. need_map: it
 * is for something that will be written through its address, and a block
 * that cannot be mapped is no use - it goes back to the driver and this
 * fails, where it used to be handed out with no address. Returns the
 * block's index, or -1 with heap->last_error saying why. */
static int vk_heap_add_block(vk_heap_t *heap, unsigned type, int linear,
      VkDeviceSize size, int need_map)
{
   VkMemoryAllocateInfo mai;
   vk_heap_block_t *b;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   void *mapped          = NULL;
   VkResult result;
   unsigned slot;

   /* A slot trim left empty, if there is one. */
   for (slot = 0; slot < heap->block_count; slot++)
      if (!heap->blocks[slot].memory)
         break;
   if (slot >= VK_HEAP_MAX_BLOCKS
         || (heap->max_bytes && heap->bytes_reserved + size > heap->max_bytes))
   {
      heap->last_error = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      return -1;
   }

   memset(&mai, 0, sizeof(mai));
   mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   mai.allocationSize  = size;
   mai.memoryTypeIndex = type;
   result = heap->fns.allocate_memory(heap->device, &mai, NULL, &memory);
   if (result != VK_SUCCESS)
   {
      heap->last_error = result;
      return -1;
   }

   /* Mapped once, for the life of the block: nothing maps per use. */
   if (heap->props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
   {
      result = heap->fns.map_memory(heap->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
      if (result != VK_SUCCESS)
      {
         mapped = NULL;
         if (need_map)
         {
            heap->fns.free_memory(heap->device, memory, NULL);
            heap->last_error = result;
            return -1;
         }
         /* (kept, for what never looks at its address) */
      }
   }

   b = &heap->blocks[slot];
   memset(b, 0, sizeof(*b));
   if (!vk_heap_block_insert(b, 0, 0, size))
   {
      if (mapped)
         heap->fns.unmap_memory(heap->device, memory);
      heap->fns.free_memory(heap->device, memory, NULL);
      heap->last_error = VK_ERROR_OUT_OF_HOST_MEMORY;
      return -1;
   }
   b->memory   = memory;
   b->mapped   = mapped;
   b->size     = size;
   b->max_free = size;
   b->type     = type;
   b->linear   = linear ? 1u : 0u;

   if (slot == heap->block_count)
      heap->block_count++;
   heap->bytes_reserved += size;
   return (int)slot;
}

int vk_heap_init(vk_heap_t *heap, VkDevice device,
      const VkPhysicalDeviceMemoryProperties *props,
      const vk_heap_fns_t *fns, VkDeviceSize block_size,
      VkDeviceSize max_bytes)
{
   if (!heap || !props || !fns || !fns->allocate_memory || !fns->free_memory
         || !fns->map_memory || !fns->unmap_memory)
      return 0;
   memset(heap, 0, sizeof(*heap));
   heap->device     = device;
   heap->props      = *props;
   heap->fns        = *fns;
   heap->block_size = block_size ? block_size : (64u * 1024u * 1024u);
   heap->max_bytes  = max_bytes;
   return 1;
}

void vk_heap_shutdown(vk_heap_t *heap)
{
   unsigned i;

   for (i = 0; i < heap->block_count; i++)
      if (heap->blocks[i].memory)
         vk_heap_block_release(heap, &heap->blocks[i]);
   heap->block_count    = 0;
   heap->bytes_reserved = 0;
   heap->bytes_used     = 0;
}

unsigned vk_heap_trim(vk_heap_t *heap)
{
   unsigned freed = 0;
   unsigned i;

   for (i = 0; i < heap->block_count; i++)
   {
      vk_heap_block_t *b = &heap->blocks[i];

      if (!b->memory || b->used != 0)
         continue;
      vk_heap_block_release(heap, b);
      freed++;
   }
   return freed;
}

/* The lowest span of the block that fits, at the alignment asked for.
 * What is left over on either side stays free. */
static int vk_heap_block_alloc(vk_heap_t *heap, vk_heap_block_t *b, unsigned index,
      VkDeviceSize size, VkDeviceSize align, vk_heap_alloc_t *out)
{
   VkDeviceSize largest = 0;
   unsigned i;

   if (size > b->max_free)
      return 0;

   for (i = 0; i < b->span_count; i++)
   {
      const VkDeviceSize start     = b->spans[i].offset;
      const VkDeviceSize span_size = b->spans[i].size;
      const VkDeviceSize aligned   = VK_HEAP_ALIGN_UP(start, align);
      const VkDeviceSize head      = aligned - start;
      VkDeviceSize tail;

      if (span_size > largest)
         largest = span_size;
      if (head + size > span_size)
         continue;
      tail = span_size - head - size;

      if (head && tail)
      {
         /* The head stays where it is; the tail goes in after it. */
         if (!vk_heap_block_insert(b, i + 1, aligned + size, tail))
            return 0;
         b->spans[i].size = head;
      }
      else if (head)
         b->spans[i].size = head;
      else if (tail)
      {
         b->spans[i].offset = aligned + size;
         b->spans[i].size   = tail;
      }
      else
         vk_heap_block_remove(b, i);

      b->used          += size;
      heap->bytes_used += size;

      out->memory = b->memory;
      out->offset = aligned;
      out->size   = size;
      out->mapped = b->mapped ? (void*)((unsigned char*)b->mapped + aligned) : NULL;
      out->block  = index;
      return 1;
   }

   /* Every span was looked at and none fits: now the bound is exact. */
   b->max_free = largest;
   return 0;
}

/* An allocation from the blocks there are of one memory type and kind. */
static int vk_heap_alloc_existing(vk_heap_t *heap, const VkMemoryRequirements *req,
      unsigned type, unsigned linear, int need_map, vk_heap_alloc_t *out)
{
   const VkDeviceSize align = req->alignment ? req->alignment : 1;
   unsigned i;

   for (i = 0; i < heap->block_count; i++)
   {
      vk_heap_block_t *b = &heap->blocks[i];

      if (!b->memory || b->type != type || b->linear != linear)
         continue;
      if (need_map && !b->mapped)
         continue;
      if (vk_heap_block_alloc(heap, b, i, req->size, align, out))
         return 1;
   }
   return 0;
}

/* An allocation from a new block of one memory type: the only thing here
 * that asks the driver for memory. */
static int vk_heap_alloc_new(vk_heap_t *heap, const VkMemoryRequirements *req,
      unsigned type, unsigned linear, int need_map, vk_heap_alloc_t *out)
{
   const VkDeviceSize align = req->alignment ? req->alignment : 1;
   const int slot = vk_heap_add_block(heap, type, (int)linear,
         vk_heap_block_size(heap, type, req->size), need_map);

   if (slot < 0)
      return 0;
   return vk_heap_block_alloc(heap, &heap->blocks[slot], (unsigned)slot,
         req->size, align, out);
}

unsigned vk_heap_reserve(vk_heap_t *heap, uint32_t type_bits,
      VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
      int linear, unsigned blocks)
{
   const int need_map = (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
   unsigned char types[VK_MAX_MEMORY_TYPES];
   unsigned with_preferred;
   unsigned count = vk_heap_types(heap, type_bits, required, preferred, types, &with_preferred);
   unsigned made  = 0;
   unsigned i;

   /* The best type first; whatever the driver would not give of it comes
    * from the next that will do. */
   for (i = 0; i < count && made < blocks; i++)
      while (made < blocks && vk_heap_add_block(heap, types[i], linear,
               vk_heap_block_size(heap, types[i], 0), need_map) >= 0)
         made++;
   return made;
}

int vk_heap_alloc(vk_heap_t *heap, const VkMemoryRequirements *req,
      VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
      int linear, vk_heap_alloc_t *out)
{
   const unsigned kind = linear ? 1u : 0u;
   /* Asked for as a requirement, the memory is written through its
    * address: the allocation is mapped, or it fails. */
   const int need_map  = (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
   unsigned char types[VK_MAX_MEMORY_TYPES];
   unsigned with_preferred;
   unsigned count = vk_heap_types(heap, req->memoryTypeBits, required, preferred,
         types, &with_preferred);
   unsigned first = 0;
   unsigned end   = with_preferred;
   unsigned i;

   if (!count)
   {
      heap->last_error = VK_ERROR_FEATURE_NOT_PRESENT;
      return 0;
   }

   /* preferred is a preference. A type that has it may not exist, and
    * where it does the driver may have none of it left long before the
    * device runs out: device-local memory that is also host-visible is a
    * window of 256 MB or less on a discrete card without resizable BAR.
    * So: every type that has it, in the blocks there are and then in a
    * new one; then, the same way, every type that only meets the
    * requirement. All of them, not the first of each - a driver can be
    * out of one type and have another that would do. */
   for (;;)
   {
      for (i = first; i < end; i++)
         if (vk_heap_alloc_existing(heap, req, types[i], kind, need_map, out))
            return 1;
      for (i = first; i < end; i++)
         if (vk_heap_alloc_new(heap, req, types[i], kind, need_map, out))
            return 1;
      if (end == count)
         break;
      first = end;
      end   = count;
   }

   /* No room and no new block anywhere. Empty blocks of other kinds are
    * given back, and every type is asked once more. */
   if (vk_heap_trim(heap))
      for (i = 0; i < count; i++)
         if (vk_heap_alloc_new(heap, req, types[i], kind, need_map, out))
            return 1;
   return 0;
}

void vk_heap_free(vk_heap_t *heap, const vk_heap_alloc_t *alloc)
{
   vk_heap_block_t *b;
   VkDeviceSize offset;
   VkDeviceSize size;
   unsigned lo;
   unsigned hi;
   int joins_prev;
   int joins_next;

   if (!alloc || alloc->memory == VK_NULL_HANDLE || alloc->block >= heap->block_count)
      return;
   b = &heap->blocks[alloc->block];
   if (b->memory != alloc->memory)
      return;

   offset = alloc->offset;
   size   = alloc->size;
   b->used          -= size;
   heap->bytes_used -= size;

   /* The first free span that starts after this one. */
   lo = 0;
   hi = b->span_count;
   while (lo < hi)
   {
      const unsigned mid = lo + (hi - lo) / 2u;

      if (b->spans[mid].offset < offset)
         lo = mid + 1;
      else
         hi = mid;
   }

   /* Joined to whichever neighbours it touches, so the spans stay sorted
    * and never adjacent. */
   joins_prev = lo > 0 && b->spans[lo - 1].offset + b->spans[lo - 1].size == offset;
   joins_next = lo < b->span_count && offset + size == b->spans[lo].offset;

   if (joins_prev && joins_next)
   {
      b->spans[lo - 1].size += size + b->spans[lo].size;
      size = b->spans[lo - 1].size;
      vk_heap_block_remove(b, lo);
   }
   else if (joins_prev)
   {
      b->spans[lo - 1].size += size;
      size = b->spans[lo - 1].size;
   }
   else if (joins_next)
   {
      b->spans[lo].offset = offset;
      b->spans[lo].size  += size;
      size = b->spans[lo].size;
   }
   else if (!vk_heap_block_insert(b, lo, offset, size))
   {
      /* Out of host memory for the span array: the span is lost to the
       * heap until the block is released, which is better than a span
       * written where there is no room for it. */
      return;
   }

   if (size > b->max_free)
      b->max_free = size;
}
