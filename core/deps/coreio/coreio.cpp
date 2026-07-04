
#include "coreio.h"

/* All content file I/O routes through libretro-common's filestream, which uses
 * the frontend-provided libretro VFS when available (see the
 * RETRO_ENVIRONMENT_GET_VFS_INTERFACE wiring in retro_set_environment) and the
 * local UTF-8-safe implementation otherwise. */
#include <streams/file_stream.h>

#include <string>

struct CORE_FILE {
	RFILE* f;
	std::string path;
	size_t seek_ptr;
};

core_file* core_fopen(const char* filename)
{
	CORE_FILE* rv = new CORE_FILE();
	rv->f = filestream_open(filename, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	rv->path = filename;
	rv->seek_ptr = 0;

	if (!rv->f) {
		delete rv;
		return 0;
	}

	core_fseek((core_file*)rv, 0, SEEK_SET);
	return (core_file*)rv;
}

size_t core_fseek(core_file* fc, size_t offs, size_t origin) {
	CORE_FILE* f = (CORE_FILE*)fc;

	if (origin == SEEK_SET)
		f->seek_ptr = offs;
	else if (origin == SEEK_CUR)
		f->seek_ptr += offs;
	else
		die("Invalid code path");

	if (f->f)
		filestream_seek(f->f, f->seek_ptr, RETRO_VFS_SEEK_POSITION_START);

	return 0;
}

size_t core_ftell(core_file* fc)
{
	CORE_FILE* f = (CORE_FILE*)fc;
	return f->seek_ptr;
}

int core_fread(core_file* fc, void* buff, size_t len)
{
	CORE_FILE* f = (CORE_FILE*)fc;

	if (f->f)
		filestream_read(f->f, buff, len);

	f->seek_ptr += len;

	return len;
}

int core_fclose(core_file* fc)
{
	CORE_FILE* f = (CORE_FILE*)fc;

	if (f->f)
		filestream_close(f->f);

	delete f;

	return 0;
}

size_t core_fsize(core_file* fc)
{
	CORE_FILE* f = (CORE_FILE*)fc;

	if (f->f)
		return (size_t)filestream_get_size(f->f);
	return 0;
}
