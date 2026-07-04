#ifndef __CORETYPES_H__
#define __CORETYPES_H__

#include <stdint.h>
#include <stdio.h>

#ifdef USE_LIBRETRO_VFS
#include <streams/file_stream_transforms.h>
#endif

#define ARRAY_LENGTH(x) (sizeof(x)/sizeof(x[0]))

typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint16_t UINT16;
typedef uint8_t UINT8;

typedef int64_t INT64;
typedef int32_t INT32;
typedef int16_t INT16;
typedef int8_t INT8;

/* Route libchdr file I/O through libretro-common filestream (libretro VFS).
 * RETRO_VFS_SEEK_POSITION_{START,CURRENT,END} share the values of
 * SEEK_{SET,CUR,END}, so whence passes straight through. */
#include <streams/file_stream.h>
#define core_file RFILE
#define core_fopen(path) filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE)
#define core_fseek(fc, offs, whence) filestream_seek(fc, offs, whence)
#define core_ftell(fc) filestream_tell(fc)
#define core_fread(fc, buff, len) filestream_read(fc, buff, len)
#define core_fclose filestream_close

static UINT64 core_fsize(core_file *f)
{
    UINT64 rv;
    UINT64 p = core_ftell(f);
    core_fseek(f, 0, SEEK_END);
    rv = core_ftell(f);
    core_fseek(f, p, SEEK_SET);
    return rv;
}

#endif
