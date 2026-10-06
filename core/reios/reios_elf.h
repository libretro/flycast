#include "types.h"

/* Load the ELF file into main memory. @entry gets the address it says it
 * starts at. */
bool reios_loadElf(const std::string& elf, u32* entry);
