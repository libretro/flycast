#include "reios.h"

#include "deps/libelf/elf.h"

#include "hw/sh4/sh4_mem.h"
#include "deps/coreio/coreio.h"

bool reios_loadElf(const std::string& elf, u32* entry)
{
   void *elfFile;
   u8 *buf = NULL;
   int i;
   bool phys = false;
   size_t size;
   bool ok = true;
   core_file* f = core_fopen(elf.c_str());
   if (!f)
      return false;

   size = core_fsize(f);
   if (size > 16 * 1024 * 1024)
   {
      core_fclose(f);
      return false;
   }

   /* Parsed where it is when the file is mapped, else read once. */
   elfFile = (void*)core_fmap(f, NULL);
   if (!elfFile)
   {
      buf = (u8*)malloc(size);
      if (!buf || core_fread_at(f, 0, buf, size) != size)
      {
         free(buf);
         core_fclose(f);
         return false;
      }
      elfFile = buf;
   }

   /* What the file says about itself is checked against the file before it
    * is acted on: that it is long enough to have a header at all, that it
    * is the 32-bit ELF a Dreamcast program is, and that its table of
    * program headers lies inside it. */
   if (size < sizeof(struct Elf32_Header) || elf_checkFile(elfFile) != 0)
      ok = false;
   else
   {
      const struct Elf32_Header* h = (const struct Elf32_Header*)elfFile;

      if (h->e_ident[EI_CLASS] != ELFCLASS32
            || h->e_phentsize != sizeof(struct Elf32_Phdr)
            || h->e_phoff > size
            || (size_t)h->e_phnum * sizeof(struct Elf32_Phdr) > size - h->e_phoff)
         ok = false;
   }

   for (i = 0; ok && i < elf_getNumProgramHeaders(elfFile); i++)
   {
      /* Load that section, if it is one that is loaded */
      uint64_t dest, src, offset, len, memlen;
      if (elf_getProgramHeaderType(elfFile, i) != PT_LOAD)
         continue;
      if (phys)
         dest = elf_getProgramHeaderPaddr(elfFile, i);
      else
         dest = elf_getProgramHeaderVaddr(elfFile, i);
      len = elf_getProgramHeaderFileSize(elfFile, i);
      memlen = elf_getProgramHeaderMemorySize(elfFile, i);
      offset = elf_getProgramHeaderOffset(elfFile, i);
      src = (uint64_t)(uintptr_t)elfFile + offset;

      /* The bytes to load have to be in the file... */
      if (offset > size || len > size - offset)
      {
         WARN_LOG(REIOS, "Section %d lies outside the file", i);
         ok = false;
         break;
      }
      if (memlen < len)
         memlen = len;
      /* ...and where they go, with the zeroes after them, has to be in
       * main memory, all of it: GetMemPtr() looks only at where it starts. */
      u8* ptr = dest <= 0xFFFFFFFFu ? GetMemPtr((u32)dest, (u32)len) : NULL;
      if (ptr == NULL || memlen > RAM_SIZE - ((u32)dest & RAM_MASK))
      {
         WARN_LOG(REIOS, "Invalid load address for section %d: %08lx", i, (unsigned long)dest);
         ok = false;
         break;
      }
      DEBUG_LOG(REIOS, "Loading section %d to %08lx - %08lx", i, (unsigned long)dest, (unsigned long)(dest + len - 1));

      memcpy((void*)ptr, (void*)(uintptr_t)src, len);
      memset((void*)(ptr + len), 0, memlen - len);
   }

   /* It starts where it says, if that is in main memory */
   if (ok)
   {
      uint64_t start = elf_getEntryPoint(elfFile);

      if (start <= 0xFFFFFFFFu && GetMemPtr((u32)start, 4) != NULL)
         *entry = (u32)start;
   }

   free(buf);
   core_fclose(f);
   return ok;
}
