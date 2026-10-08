#ifndef CORE_DEPS_COREIO_COREIO_H
#define CORE_DEPS_COREIO_COREIO_H

#include <stddef.h>
#include <stdint.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef void* core_file;

/* Read-only content files. A plain path is memory-mapped where the
 * platform allows, so reads are copies out of the page cache with no
 * syscall per sector; otherwise it is read through the libretro VFS.
 * A path of the form archive.zip#member (or .7z) is a member of that
 * archive, served from the archive's own bytes. */
core_file* core_fopen(const char* filename);
size_t core_fseek(core_file* fc, size_t offs, size_t origin);
int core_fread(core_file* fc, void* buff, size_t len);
/* Positioned read: one call, no cursor movement for the memory-backed
 * forms. Returns bytes read. */
size_t core_fread_at(core_file* fc, uint64_t offset, void* buff, size_t len);
int core_fclose(core_file* fc);
size_t core_fsize(core_file* fc);
size_t core_ftell(core_file* fc);
/* The whole file as addressable bytes when it is mapped or already in
 * memory, else NULL. Valid until core_fclose(). */
const uint8_t* core_fmap(core_file* fc, size_t* len);
/* @len bytes at @offset where they already are - in the mapping, in
 * memory, or in the piece of a large archive member decoded around them -
 * or NULL when they have to be read. Valid until the next call on the
 * file: for the mapped and in-memory forms, which core_fmap() also
 * answers for, until core_fclose(). */
const uint8_t* core_fview(core_file* fc, uint64_t offset, size_t len);
/* Ask the OS to start bringing [offset, offset + len) in, ahead of the
 * reads that follow. A hint; returns at once. */
void core_fprefetch(core_file* fc, uint64_t offset, size_t len);

/* Close the archive kept open for archive#member paths, once no member
 * file refers to it any more. */
void core_archive_release(void);

RETRO_END_DECLS

#endif
