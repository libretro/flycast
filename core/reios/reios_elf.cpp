#include "reios.h"

#include "deps/libelf/elf.h"

#include "hw/sh4/sh4_mem.h"
#include "deps/coreio/coreio.h"

bool reios_loadElf(const std::string& elf)
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

   if (elf_checkFile(elfFile) != 0)
      ok = false;

   for (i = 0; ok && i < elf_getNumProgramHeaders(elfFile); i++)
   {
      /* Load that section */
      uint64_t dest, src;
      size_t len;
      if (phys)
         dest = elf_getProgramHeaderPaddr(elfFile, i);
      else
         dest = elf_getProgramHeaderVaddr(elfFile, i);
      len = elf_getProgramHeaderFileSize(elfFile, i);
      src = (uint64_t)(uintptr_t)elfFile + elf_getProgramHeaderOffset(elfFile, i);

      u8* ptr = GetMemPtr(dest, len);
      if (ptr == NULL)
      {
         WARN_LOG(REIOS, "Invalid load address for section %d: %08lx", i, dest);
         continue;
      }
      DEBUG_LOG(REIOS, "Loading section %d to %08lx - %08lx", i, dest, dest + len - 1);

      memcpy((void*)ptr, (void*)(uintptr_t)src, len);
      ptr += len;
      memset((void*)ptr, 0, elf_getProgramHeaderMemorySize(elfFile, i) - len);
   }

   free(buf);
   core_fclose(f);
   return ok;
}
