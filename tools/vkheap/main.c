/* core/rend/vulkan/vk_heap.c - the suballocator that replaces VMA. No
 * GPU: the Vulkan entry points are stubs that count calls and hand out
 * fake handles, so what is tested is the arithmetic - which is the part
 * that decides whether a frame allocates. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_heap.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); fails++; } } while (0)

static int allocations;   /* how many times the driver was asked */

/* A memory type whose driver allocations stop succeeding once this many
 * bytes are taken from it: the PCI BAR window. -1 refuses nothing. */
static int          refuse_type = -1;
static VkDeviceSize refuse_after;
static VkDeviceSize refuse_taken;
static int frees;
static int maps;

/* A memory type whose blocks the driver will not map: -1 maps all. The
 * stub remembers which type each of its handles came from. */
static int           fail_map_type = -1;
static unsigned char handle_type[256];

static VkResult stub_allocate(VkDevice d, const VkMemoryAllocateInfo *ai,
      const VkAllocationCallbacks *cb, VkDeviceMemory *out)
{
   (void)d; (void)cb;
   if ((int)ai->memoryTypeIndex == refuse_type)
   {
      if (refuse_taken + ai->allocationSize > refuse_after)
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      refuse_taken += ai->allocationSize;
   }
   allocations++;
   handle_type[allocations & 255] = (unsigned char)ai->memoryTypeIndex;
   *out = (VkDeviceMemory)(uintptr_t)(0x1000 + allocations);
   return VK_SUCCESS;
}

static void stub_free(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *cb)
{
   (void)d; (void)m; (void)cb;
   frees++;
}

/* What the stub handed out for each fake VkDeviceMemory, so unmap can
 * give it back - otherwise the test leaks and the leak is the test's. */
static VkDeviceMemory mapped_handles[VK_HEAP_MAX_BLOCKS];
static void          *mapped_ptrs[VK_HEAP_MAX_BLOCKS];
static unsigned       mapped_n;

static VkResult stub_map(VkDevice d, VkDeviceMemory m, VkDeviceSize off,
      VkDeviceSize size, VkMemoryMapFlags f, void **out)
{
   (void)d; (void)off; (void)f;
   maps++;
   if (fail_map_type >= 0
         && handle_type[((uintptr_t)m - 0x1000) & 255] == (unsigned char)fail_map_type)
   {
      *out = NULL;
      return VK_ERROR_MEMORY_MAP_FAILED;
   }
   *out = malloc(size == VK_WHOLE_SIZE ? (4u * 1024u * 1024u) : (size_t)size);
   if (!*out)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (mapped_n < VK_HEAP_MAX_BLOCKS)
   {
      mapped_handles[mapped_n] = m;
      mapped_ptrs[mapped_n]    = *out;
      mapped_n++;
   }
   return VK_SUCCESS;
}

static void stub_unmap(VkDevice d, VkDeviceMemory m)
{
   unsigned i;
   (void)d;
   for (i = 0; i < mapped_n; i++)
   {
      if (mapped_handles[i] != m)
         continue;
      free(mapped_ptrs[i]);
      mapped_handles[i] = mapped_handles[mapped_n - 1];
      mapped_ptrs[i]    = mapped_ptrs[mapped_n - 1];
      mapped_n--;
      return;
   }
}

static void fill_props(VkPhysicalDeviceMemoryProperties *p)
{
   memset(p, 0, sizeof(*p));
   p->memoryHeapCount = 1;
   p->memoryHeaps[0].size = 1024u * 1024u * 1024u;
   p->memoryTypeCount = 2;
   /* 0: device local. 1: host visible and coherent. */
   p->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   p->memoryTypes[0].heapIndex = 0;
   p->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   p->memoryTypes[1].heapIndex = 0;
}

static void req(VkMemoryRequirements *r, VkDeviceSize size, VkDeviceSize align, uint32_t bits)
{
   r->size = size;
   r->alignment = align;
   r->memoryTypeBits = bits;
}

int main(void)
{
   VkPhysicalDeviceMemoryProperties props;
   vk_heap_fns_t fns;
   vk_heap_t heap;
   VkMemoryRequirements r;
   vk_heap_alloc_t a[64];
   int i;

   fill_props(&props);
   memset(&fns, 0, sizeof(fns));
   fns.allocate_memory = stub_allocate;
   fns.free_memory     = stub_free;
   fns.map_memory      = stub_map;
   fns.unmap_memory    = stub_unmap;

   CHECK(vk_heap_init(&heap, (VkDevice)1, &props, &fns, 1024 * 1024, 0) != 0, "init");

   /* Reserving takes blocks now. */
   allocations = 0;
   CHECK(vk_heap_reserve(&heap, 0x3u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, 2) == 2,
         "reserve takes the blocks it was asked for");
   CHECK(allocations == 2, "two driver allocations, no more");

   /* And then nothing does. This is the whole point. */
   allocations = 0;
   req(&r, 64 * 1024, 256, 0x1u);
   for (i = 0; i < 16; i++)
      CHECK(vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &a[i]) != 0,
            "allocation out of the reserved blocks");
   CHECK(allocations == 0, "sixteen allocations, nothing asked of the driver");

   /* Offsets do not overlap, and honour alignment. */
   for (i = 0; i < 16; i++)
   {
      int j;
      CHECK((a[i].offset % 256) == 0, "aligned");
      for (j = i + 1; j < 16; j++)
      {
         const int same = (a[i].memory == a[j].memory);
         const int apart = (a[i].offset + a[i].size <= a[j].offset)
            || (a[j].offset + a[j].size <= a[i].offset);
         CHECK(!same || apart, "two allocations never overlap");
      }
   }

   /* Freeing and re-taking the same size reuses the space rather than
    * growing: the churn case, which is what put the card on the floor. */
   for (i = 0; i < 16; i++)
      vk_heap_free(&heap, &a[i]);
   CHECK(heap.bytes_used == 0, "everything given back");

   allocations = 0;
   for (i = 0; i < 16; i++)
      CHECK(vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &a[i]) != 0,
            "re-taken");
   CHECK(allocations == 0, "a free and re-take asks the driver for nothing");

   /* Fragmentation: free every other one, then ask for something that
    * needs two adjacent - it must come from the merged space, still
    * without the driver. */
   for (i = 0; i < 16; i += 2)
      vk_heap_free(&heap, &a[i]);
   for (i = 1; i < 16; i += 2)
      vk_heap_free(&heap, &a[i]);
   allocations = 0;
   req(&r, 512 * 1024, 256, 0x1u);
   CHECK(vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &a[0]) != 0,
         "a large allocation after freeing everything");
   CHECK(allocations == 0, "freed spans merged, so no new block");
   vk_heap_free(&heap, &a[0]);

   /* Host visible memory is mapped once, per block, and an allocation's
    * pointer is inside that mapping. */
   maps = 0;
   req(&r, 4096, 64, 0x2u);
   CHECK(vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0, 1, &a[0]) != 0,
         "host visible allocation");
   CHECK(a[0].mapped != NULL, "and it is mapped");
   CHECK(maps == 1, "the block was mapped once");
   maps = 0;
   CHECK(vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0, 1, &a[1]) != 0,
         "a second one");
   CHECK(maps == 0, "no second mapping");
   CHECK(a[1].mapped != a[0].mapped, "different offsets, different pointers");

   /* Something bigger than a block gets its own. */
   allocations = 0;
   req(&r, 4u * 1024u * 1024u, 256, 0x1u);
   CHECK(vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &a[2]) != 0,
         "an allocation larger than the block size");
   CHECK(allocations == 1, "which takes exactly one block");

   /* A requirement no memory type satisfies fails rather than pretending. */
   req(&r, 1024, 256, 0x0u);
   CHECK(vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &a[3]) == 0,
         "no type, no allocation");

   /* A ceiling is a ceiling: with one set, the heap refuses rather than
    * taking another block, which is the difference between a missing
    * texture and a card with nothing left on it. */
   {
      vk_heap_t capped;
      vk_heap_alloc_t c;
      int taken = 0;
      memset(&capped, 0, sizeof(capped));
      CHECK(vk_heap_init(&capped, (VkDevice)1, &props, &fns,
            1024 * 1024, 4 * 1024 * 1024) != 0, "init with a ceiling");
      req(&r, 1024 * 1024, 256, 0x1u);
      while (vk_heap_alloc(&capped, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &c))
      {
         taken++;
         if (taken > 64)
            break;
      }
      CHECK(taken == 4, "four megabytes of ceiling is four one-megabyte blocks");
      CHECK(capped.bytes_reserved <= 4u * 1024u * 1024u, "and it never went past it");
      vk_heap_shutdown(&capped);
   }

   /* Trim gives empty blocks back, and the block indices live
    * allocations carry keep pointing at the right memory afterwards -
    * the reason trim leaves holes instead of closing the array up. */
   {
      vk_heap_t t2;
      vk_heap_alloc_t keep[4];
      vk_heap_alloc_t drop[4];
      VkDeviceMemory keep_mem[4];
      VkDeviceSize keep_off[4];
      int k;

      memset(&t2, 0, sizeof(t2));
      CHECK(vk_heap_init(&t2, (VkDevice)1, &props, &fns, 1024 * 1024, 0) != 0, "trim: init");

      /* Four blocks' worth, alternating between ones kept and ones
       * dropped, so trim has holes to make in the middle. */
      req(&r, 1024 * 1024, 256, 0x1u);
      for (k = 0; k < 4; k++)
      {
         CHECK(vk_heap_alloc(&t2, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &keep[k]) != 0, "trim: keep");
         CHECK(vk_heap_alloc(&t2, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &drop[k]) != 0, "trim: drop");
         keep_mem[k] = keep[k].memory;
         keep_off[k] = keep[k].offset;
      }
      for (k = 0; k < 4; k++)
         vk_heap_free(&t2, &drop[k]);

      CHECK(vk_heap_trim(&t2) == 4, "trim gave back the four empty blocks");

      for (k = 0; k < 4; k++)
      {
         CHECK(keep[k].memory == keep_mem[k], "a live allocation still names its own memory");
         CHECK(keep[k].offset == keep_off[k], "at its own offset");
         CHECK(keep[k].block < t2.block_count, "and its block index is still in range");
      }

      /* And the freed room is usable again. */
      CHECK(vk_heap_alloc(&t2, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &drop[0]) != 0,
            "the trimmed room comes back");

      /* Freeing the ones kept still works, which is what a stale block
       * index would break. */
      for (k = 0; k < 4; k++)
         vk_heap_free(&t2, &keep[k]);
      vk_heap_free(&t2, &drop[0]);
      CHECK(t2.bytes_used == 0, "everything given back after a trim");
      vk_heap_shutdown(&t2);
   }

   /* The device-local host-visible type is a preference, not a
    * requirement. Laid out the way a discrete card without resizable BAR
    * reports it - device memory, system memory, and a small BAR window
    * that is device local and host visible - and the GS device's own
    * startup run against it: 64 MB blocks under a 256 MB ceiling, two
    * device blocks and one host block reserved, a 64 MB upload buffer and
    * then the 32 MB vertex buffer, both preferring the BAR. The driver
    * gives the BAR one block and refuses the second; the vertex buffer
    * must land in system memory instead of failing the device. */
   {
      VkPhysicalDeviceMemoryProperties bar;
      vk_heap_t h3;
      vk_heap_alloc_t up, vtx, idx;
      const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
         | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      const VkDeviceSize mb = 1024u * 1024u;

      memset(&bar, 0, sizeof(bar));
      bar.memoryHeapCount = 3;
      bar.memoryHeaps[0].size = 6144u * mb;
      bar.memoryHeaps[1].size = 16384u * mb;
      bar.memoryHeaps[2].size = 214u * mb;
      bar.memoryTypeCount = 3;
      bar.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      bar.memoryTypes[0].heapIndex     = 0;
      bar.memoryTypes[1].propertyFlags = hv;
      bar.memoryTypes[1].heapIndex     = 1;
      bar.memoryTypes[2].propertyFlags = hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      bar.memoryTypes[2].heapIndex     = 2;

      refuse_type  = 2;
      refuse_after = 64u * mb;
      refuse_taken = 0;

      memset(&h3, 0, sizeof(h3));
      CHECK(vk_heap_init(&h3, (VkDevice)1, &bar, &fns, 64u * mb, 256u * mb) != 0, "bar: init");
      CHECK(vk_heap_reserve(&h3, ~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, 2) == 2, "bar: device reserve");
      CHECK(vk_heap_reserve(&h3, ~0u, hv, 0, 1, 1) == 1, "bar: host reserve");

      req(&r, 64u * mb, 256, 0x7u);
      CHECK(vk_heap_alloc(&h3, &r, hv, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, &up) != 0, "bar: upload buffer");
      CHECK(h3.blocks[up.block].type == 2, "bar: the upload buffer takes the BAR it prefers");

      req(&r, 32u * mb, 256, 0x7u);
      CHECK(vk_heap_alloc(&h3, &r, hv, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, &vtx) != 0,
            "bar: the vertex buffer is allocated after the BAR refuses");
      CHECK(h3.blocks[vtx.block].type == 1 && vtx.mapped != NULL, "bar: in mapped system memory");
      CHECK(h3.block_count == 4, "bar: out of the reserved host block, with the reservations kept");

      req(&r, 16u * mb, 256, 0x7u);
      CHECK(vk_heap_alloc(&h3, &r, hv, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, &idx) != 0,
            "bar: and the index buffer after it");

      vk_heap_free(&h3, &idx);
      vk_heap_free(&h3, &vtx);
      vk_heap_free(&h3, &up);
      vk_heap_shutdown(&h3);
      refuse_type = -1;
   }


   /* Buffers and optimal images never share a block, whatever the memory
    * type: that is how bufferImageGranularity is honoured. */
   {
      vk_heap_t k;
      vk_heap_alloc_t img, buf, img2;

      memset(&k, 0, sizeof(k));
      CHECK(vk_heap_init(&k, (VkDevice)1, &props, &fns, 1024 * 1024, 0) != 0, "kinds: init");
      req(&r, 4096, 256, 0x1u);
      CHECK(vk_heap_alloc(&k, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &img) != 0, "kinds: image");
      CHECK(vk_heap_alloc(&k, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 1, &buf) != 0, "kinds: buffer");
      CHECK(vk_heap_alloc(&k, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &img2) != 0, "kinds: second image");
      CHECK(img.memory != buf.memory, "a buffer does not go in an image's block");
      CHECK(img.memory == img2.memory, "two images share one");
      vk_heap_free(&k, &img);
      vk_heap_free(&k, &buf);
      vk_heap_free(&k, &img2);
      vk_heap_shutdown(&k);
   }

   /* Churn, the way a texture cache does it: thousands of allocations of
    * mixed sizes and alignments, freed in no particular order. Checked
    * against a list kept on the side: nothing overlaps, every offset is
    * aligned, and after each free the block's spans are still sorted,
    * never adjacent, and add up with what is in use to the whole block. */
   {
      enum { LIVE = 600, ROUNDS = 60000 };
      static vk_heap_alloc_t live[LIVE];
      static int used[LIVE];
      vk_heap_t c;
      unsigned seed = 1;
      int n, ok = 1;

      memset(&c, 0, sizeof(c));
      memset(used, 0, sizeof(used));
      CHECK(vk_heap_init(&c, (VkDevice)1, &props, &fns, 8u * 1024u * 1024u, 0) != 0, "churn: init");
      for (n = 0; n < ROUNDS && ok; n++)
      {
         unsigned slot, b, sp;

         seed = seed * 1664525u + 1013904223u;
         slot = (seed >> 8) % LIVE;
         if (used[slot])
         {
            vk_heap_free(&c, &live[slot]);
            used[slot] = 0;
         }
         else
         {
            const VkDeviceSize align = (VkDeviceSize)1 << ((seed >> 20) % 13);
            const VkDeviceSize size  = 16 + ((seed >> 4) % 60000);
            int j;

            req(&r, size, align, 0x1u);
            if (!vk_heap_alloc(&c, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &live[slot]))
            {
               ok = 0;
               break;
            }
            used[slot] = 1;
            if (live[slot].offset % align)
               ok = 0;
            for (j = 0; j < LIVE && ok; j++)
            {
               if (j == (int)slot || !used[j] || live[j].memory != live[slot].memory)
                  continue;
               if (!(live[j].offset + live[j].size <= live[slot].offset
                     || live[slot].offset + live[slot].size <= live[j].offset))
                  ok = 0;
            }
         }
         if (n % 97)
            continue;
         for (b = 0; b < c.block_count && ok; b++)
         {
            const vk_heap_block_t *blk = &c.blocks[b];
            VkDeviceSize free_bytes = 0;

            if (!blk->memory)
               continue;
            for (sp = 0; sp < blk->span_count; sp++)
            {
               free_bytes += blk->spans[sp].size;
               if (blk->spans[sp].size == 0 || blk->spans[sp].size > blk->max_free)
                  ok = 0;
               if (sp + 1 < blk->span_count
                     && blk->spans[sp].offset + blk->spans[sp].size >= blk->spans[sp + 1].offset)
                  ok = 0;
            }
            if (free_bytes + blk->used != blk->size)
               ok = 0;
         }
      }
      CHECK(ok, "churn: allocations aligned and apart, spans sorted, merged and accounted for");
      for (n = 0; n < LIVE; n++)
         if (used[n])
            vk_heap_free(&c, &live[n]);
      CHECK(c.bytes_used == 0, "churn: everything given back");
      for (n = 0; n < (int)c.block_count; n++)
         CHECK(!c.blocks[n].memory || c.blocks[n].span_count == 1,
               "churn: each block is one free span again");
      vk_heap_shutdown(&c);
   }

   /* A block the driver will not map. Something that asked for host
    * visible memory as a requirement writes through the address: it gets
    * one, from another type that will do, or it is told there is none -
    * never a success with no address. And the block that could not be
    * mapped goes back to the driver. */
   {
      VkPhysicalDeviceMemoryProperties two;
      vk_heap_t m;
      vk_heap_alloc_t got, got2;
      const unsigned mapped_before = mapped_n;
      const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
         | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

      /* one host-visible type, and it will not map */
      CHECK(vk_heap_init(&m, (VkDevice)1, &props, &fns, 1024 * 1024, 0) != 0, "map: init");
      fail_map_type = 1;
      allocations = frees = 0;
      memset(&got, 0, sizeof(got));
      req(&r, 4096, 16, 0x3u);
      CHECK(vk_heap_alloc(&m, &r, hv, 0, 1, &got) == 0,
            "map: memory that cannot be mapped is not handed out as host visible");
      CHECK(allocations == 1 && frees == 1, "map: the block that could not be mapped went back to the driver");
      CHECK(m.bytes_reserved == 0 && m.bytes_used == 0, "map: and nothing is counted as held");
      CHECK(m.last_error == VK_ERROR_MEMORY_MAP_FAILED, "map: the reason is the driver's");
      CHECK(vk_heap_reserve(&m, 0x3u, hv, 0, 1, 2) == 0, "map: nor is such a block reserved");
      vk_heap_shutdown(&m);

      /* two host-visible types: the second maps */
      memset(&two, 0, sizeof(two));
      two.memoryHeapCount = 1;
      two.memoryHeaps[0].size = 1024u * 1024u * 1024u;
      two.memoryTypeCount = 3;
      two.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      two.memoryTypes[1].propertyFlags = hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      two.memoryTypes[2].propertyFlags = hv;
      CHECK(vk_heap_init(&m, (VkDevice)1, &two, &fns, 1024 * 1024, 0) != 0, "map: init, three types");
      allocations = frees = 0;
      req(&r, 4096, 16, 0x7u);
      CHECK(vk_heap_alloc(&m, &r, hv, 0, 1, &got) != 0, "map: the next host-visible type is used");
      CHECK(got.mapped != NULL, "map: and what it gives has an address");
      CHECK(allocations == 2 && frees == 1, "map: the first type's block was given back");
      CHECK(m.blocks[got.block].type == 2, "map: it is of the type that maps");

      /* What never looks at the address can have the unmappable type... */
      allocations = frees = 0;
      req(&r, 4096, 16, 0x2u);
      CHECK(vk_heap_alloc(&m, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 1, &got2) != 0,
            "map: device-local memory does not need to map");
      CHECK(got2.mapped == NULL && allocations == 1 && frees == 0, "map: it is kept, unmapped");
      /* ...and that block is then not given to something that does. */
      vk_heap_free(&m, &got);
      req(&r, 512 * 1024, 16, 0x6u);
      CHECK(vk_heap_alloc(&m, &r, hv, 0, 1, &got) != 0 && got.mapped != NULL
            && got.block != got2.block,
            "map: an unmapped block is passed over for host-visible use");
      vk_heap_free(&m, &got);
      vk_heap_free(&m, &got2);
      fail_map_type = -1;
      vk_heap_shutdown(&m);
      CHECK(mapped_n == mapped_before, "map: every mapping undone");
   }

   /* Two memory types that will both do. The driver has none of the first
    * left: the second is used, where only the first was ever tried. And
    * room in a block there already is - of either - is taken before the
    * driver is asked for another. */
   {
      VkPhysicalDeviceMemoryProperties two;
      vk_heap_t t;
      vk_heap_alloc_t x, y, z;
      const VkDeviceSize mb = 1024u * 1024u;

      memset(&two, 0, sizeof(two));
      two.memoryHeapCount = 2;
      two.memoryHeaps[0].size = 1024u * mb;
      two.memoryHeaps[1].size = 16u * mb;       /* a small heap: its blocks are an eighth of it */
      two.memoryTypeCount = 2;
      two.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      two.memoryTypes[0].heapIndex = 0;
      two.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
      two.memoryTypes[1].heapIndex = 1;
      CHECK(vk_heap_init(&t, (VkDevice)1, &two, &fns, 8 * 1024 * 1024, 0) != 0, "types: init");

      refuse_type  = 0;
      refuse_after = 0;
      refuse_taken = 0;
      allocations  = 0;
      req(&r, 64 * 1024, 16, 0x3u);
      CHECK(vk_heap_alloc(&t, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &x) != 0,
            "types: the first type refused, the second taken");
      CHECK(t.blocks[x.block].type == 1, "types: it is the second");
      CHECK(t.blocks[x.block].size == 2 * mb, "types: its block is an eighth of its own 16 MB heap, not 8 MB");

      /* the first type gives memory again: a block of each */
      refuse_type = -1;
      req(&r, 4 * mb, 16, 0x1u);
      CHECK(vk_heap_alloc(&t, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &y) != 0, "types: a block of the first");
      allocations = 0;
      /* too big for what is left of the first type's block; fits the second's */
      req(&r, 1 * mb, 16, 0x3u);
      vk_heap_free(&t, &y);
      req(&r, 7 * mb, 16, 0x1u);
      CHECK(vk_heap_alloc(&t, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &y) != 0, "types: most of the first's block");
      req(&r, 1536 * 1024, 16, 0x3u);
      CHECK(vk_heap_alloc(&t, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, &z) != 0, "types: room found");
      CHECK(allocations == 0, "types: in a block there already was, of the second type, with no new block of the first");
      CHECK(t.blocks[z.block].type == 1, "types: (the second type's)");
      vk_heap_free(&t, &x);
      vk_heap_free(&t, &y);
      vk_heap_free(&t, &z);
      vk_heap_shutdown(&t);
   }

   frees = 0;
   vk_heap_shutdown(&heap);
   CHECK(frees > 0, "shutdown gives the blocks back");
   CHECK(heap.block_count == 0, "and forgets them");
   CHECK(mapped_n == 0, "and unmapped every block it had mapped");

   printf(fails ? "vkheap: FAILED (%d)\n" : "vkheap: ok\n", fails);
   return fails != 0;
}
