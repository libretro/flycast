// OpenGL programs made without a frame waiting for one: glprogram_cache.h
#include "gles.h"
#include "glprogram_cache.h"
#include "stdclass.h"

#include <streams/file_stream.h>
#include <algorithm>
#include <cstring>
#include <vector>

#ifndef GL_PROGRAM_BINARY_LENGTH
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#endif
#ifndef GL_NUM_PROGRAM_BINARY_FORMATS
#define GL_NUM_PROGRAM_BINARY_FORMATS 0x87FE
#endif
#ifndef GL_PROGRAM_BINARY_RETRIEVABLE_HINT
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8257
#endif
#ifndef GL_COMPLETION_STATUS_KHR
#define GL_COMPLETION_STATUS_KHR 0x91B1
#endif
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif

static const char *const CacheFile = "gl_programs.cache";
static const u32 CacheMagic = 0x504C4746;	// "FGLP"
static const u32 CacheVersion = 1;
// (a key list or a file larger than this is not one this wrote)
static const u32 MostKeys = 1u << 16;
static const int64_t MostBytes = 256 << 20;

// (OpenGL ES 2's headers have their own name for the calling convention)
#if defined(APIENTRY)
#define GLPROG_APIENTRY APIENTRY
#elif defined(GL_APIENTRY)
#define GLPROG_APIENTRY GL_APIENTRY
#else
#define GLPROG_APIENTRY
#endif
typedef void (GLPROG_APIENTRY *ProgramBinaryFn)(GLuint program, GLenum format, const void *binary, GLsizei length);
typedef void (GLPROG_APIENTRY *GetProgramBinaryFn)(GLuint program, GLsizei bufSize, GLsizei *length, GLenum *format, void *binary);
typedef void (GLPROG_APIENTRY *ProgramParameteriFn)(GLuint program, GLenum pname, GLint value);
typedef void (GLPROG_APIENTRY *MaxThreadsFn)(GLuint count);

namespace {

struct Entry
{
	u64 hash;
	u32 format;
	u32 length;
	size_t at;		// in Cache::data
};

struct Fresh
{
	u64 hash;
	GLuint program;
};

struct Cache
{
	bool open = false;
	bool binaries = false;
	bool parallel = false;		// the extension
	bool background = false;	// ...and seen to work
	bool started = false;
	bool lost = false;			// the context went: its programs are not asked for anything
	bool dirty = false;
	bool failed = false;		// the last glprog_make(): see glprog_failed()
	u64 driver = 0;
	ProgramBinaryFn programBinary = nullptr;
	GetProgramBinaryFn getProgramBinary = nullptr;
	ProgramParameteriFn programParameteri = nullptr;

	std::vector<u8> data;		// the file as read: the binaries are in it
	std::vector<Entry> entries;	// by hash
	std::vector<u32> known[GLPROG_LISTS];	// from the file
	std::vector<u32> noted[GLPROG_LISTS];	// this run's, not in known
	std::vector<Fresh> fresh;	// compiled this run, to be kept

	// counts, for the log
	u32 fromDisk = 0, compiled = 0, inFrames = 0, generalDraws = 0, refused = 0;
};

Cache cache;

u64 fnv1a(u64 h, const void *p, size_t n)
{
	const u8 *s = (const u8 *)p;
	for (size_t i = 0; i < n; i++)
		h = (h ^ s[i]) * 0x100000001B3ull;
	return h;
}

u64 fnv1a(u64 h, const char *s)
{
	return fnv1a(h, s, strlen(s));
}

const u64 FnvBasis = 0xCBF29CE484222325ull;

bool has_extension(const char *name)
{
#ifndef HAVE_OPENGLES2
	if (gl.gl_major >= 3)
	{
		GLint n = 0;
		glGetIntegerv(GL_NUM_EXTENSIONS, &n);
		for (GLint i = 0; i < n; i++)
		{
			const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
			if (e != nullptr && !strcmp(e, name))
				return true;
		}
		return false;
	}
#endif
	const char *all = (const char *)glGetString(GL_EXTENSIONS);
	if (all == nullptr)
		return false;
	const size_t len = strlen(name);
	for (const char *p = strstr(all, name); p != nullptr; p = strstr(p + 1, name))
		if ((p == all || p[-1] == ' ') && (p[len] == ' ' || p[len] == 0))
			return true;
	return false;
}

bool version_at_least(int major, int minor)
{
	return gl.gl_major > major || (gl.gl_major == major && gl.gl_minor >= minor);
}

// (the reads of the file: bounds checked)
struct Reader
{
	const u8 *p;
	size_t left;
	bool ok;
	Reader(const u8 *p, size_t left) : p(p), left(left), ok(true) {}
	template<typename T> T get()
	{
		T v = T();
		if (left < sizeof(T))
		{
			ok = false;
			left = 0;
			return v;
		}
		memcpy(&v, p, sizeof(T));
		p += sizeof(T);
		left -= sizeof(T);
		return v;
	}
};

template<typename T> void put(std::vector<u8>& out, T v)
{
	const size_t at = out.size();
	out.resize(at + sizeof(T));
	memcpy(&out[at], &v, sizeof(T));
}

/* The file: magic, version, the driver's key; for each list the number of
 * its keys and the keys; the number of binaries and each one's hash,
 * format, length and bytes; a hash of all that went before. A file of
 * another driver has its keys taken and its binaries left. */
void read_file()
{
	const std::string path = get_writable_data_path(CacheFile);
	RFILE *f = filestream_open(path.c_str(), RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (f == nullptr)
		return;
	const int64_t size = filestream_get_size(f);
	std::vector<u8> data(size > 24 && size < MostBytes ? (size_t)size : 0);
	const bool read = !data.empty() && filestream_read(f, data.data(), (int64_t)data.size()) == (int64_t)data.size();
	filestream_close(f);
	if (!read)
		return;
	u64 sum;
	memcpy(&sum, &data[data.size() - 8], 8);
	if (fnv1a(FnvBasis, data.data(), data.size() - 8) != sum)
	{
		WARN_LOG(RENDERER, "OpenGL: %s is not whole, and is made again", path.c_str());
		return;
	}
	Reader r(data.data(), data.size() - 8);
	if (r.get<u32>() != CacheMagic || r.get<u32>() != CacheVersion)
		return;
	const u64 driver = r.get<u64>();
	const u32 lists = r.get<u32>();
	for (u32 l = 0; l < lists && r.ok; l++)
	{
		const u32 n = r.get<u32>();
		if (n > MostKeys || (size_t)n * 4 > r.left)
			return;
		std::vector<u32> keys(n);
		if (n != 0)
			memcpy(keys.data(), r.p, (size_t)n * 4);
		r.p += (size_t)n * 4;
		r.left -= (size_t)n * 4;
		if (l < GLPROG_LISTS)
			cache.known[l].swap(keys);
	}
	if (!r.ok || driver != cache.driver || !cache.binaries)
		return;
	const u32 n = r.get<u32>();
	std::vector<Entry> entries;
	entries.reserve(std::min<u32>(n, MostKeys));
	for (u32 i = 0; i < n && r.ok; i++)
	{
		Entry e;
		e.hash = r.get<u64>();
		e.format = r.get<u32>();
		e.length = r.get<u32>();
		if (!r.ok || e.length == 0 || e.length > r.left)
			return;
		e.at = (size_t)(r.p - data.data());
		r.p += e.length;
		r.left -= e.length;
		entries.push_back(e);
	}
	if (!r.ok)
		return;
	std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.hash < b.hash; });
	cache.entries.swap(entries);
	cache.data.swap(data);
}

const Entry *find(u64 hash)
{
	auto it = std::lower_bound(cache.entries.begin(), cache.entries.end(), hash,
			[](const Entry& e, u64 h) { return e.hash < h; });
	return it != cache.entries.end() && it->hash == hash ? &*it : nullptr;
}

// the program from the file: false if it is not there or the driver will not take it
bool from_disk(GLuint program, u64 hash)
{
	if (!cache.binaries)
		return false;
	const Entry *e = find(hash);
	if (e == nullptr)
		return false;
	cache.programBinary(program, (GLenum)e->format, &cache.data[e->at], (GLsizei)e->length);
	GLint ok = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		// (made by an older build of the same driver version, say: compiled)
		cache.refused++;
		return false;
	}
	cache.fromDisk++;
	return true;
}

void count_made(GLuint program, u64 hash)
{
	cache.compiled++;
	if (cache.started)
		cache.inFrames++;
	if (cache.binaries)
	{
		const Fresh f = { hash, program };
		cache.dirty = true;
		/* (a program's name is given again once the program is deleted:
		 * an older one of the name is gone) */
		for (Fresh& o : cache.fresh)
			if (o.program == program)
			{
				o = f;
				return;
			}
		if (cache.fresh.size() == cache.fresh.capacity() && cache.started)
			return;	// (no allocation in a frame: kept no more this run)
		cache.fresh.push_back(f);
	}
}

GLuint compile(GLenum type, const char *const *pieces, GLsizei count)
{
	const GLuint shader = glCreateShader(type);
	glShaderSource(shader, count, (const GLchar **)pieces, nullptr);
	glCompileShader(shader);
	return shader;
}

void log_failure(GLuint program, GLuint vs, GLuint fs)
{
	char log[2048];
	const GLuint shaders[] = { vs, fs };
	for (GLuint s : shaders)
	{
		if (s == 0)
			continue;
		GLint ok = 0;
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok)
		{
			log[0] = 0;
			glGetShaderInfoLog(s, sizeof(log), nullptr, log);
			WARN_LOG(RENDERER, "OpenGL: a shader did not compile: %s", log);
		}
	}
	log[0] = 0;
	glGetProgramInfoLog(program, sizeof(log), nullptr, log);
	WARN_LOG(RENDERER, "OpenGL: a program did not link: %s", log);
}

} // namespace

void glprog_open()
{
	glprog_close();
	cache.open = true;

	// what the programs kept are good for: this driver
	u64 h = FnvBasis;
	const GLenum names[] = { GL_VENDOR, GL_RENDERER, GL_VERSION };
	for (GLenum n : names)
	{
		const char *s = (const char *)glGetString(n);
		h = fnv1a(h, s != nullptr ? s : "");
		h = fnv1a(h, "|");
	}
	cache.driver = h;

	glsm_ctx_proc_address_t proc = { nullptr };
	const bool procs = glsm_ctl(GLSM_CTL_PROC_ADDRESS_GET, &proc) && proc.addr != nullptr;
	if (procs && (gl.is_gles ? gl.gl_major >= 3 : (version_at_least(4, 1) || has_extension("GL_ARB_get_program_binary"))))
	{
		GLint formats = 0;
		glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
		cache.programBinary = (ProgramBinaryFn)proc.addr("glProgramBinary");
		cache.getProgramBinary = (GetProgramBinaryFn)proc.addr("glGetProgramBinary");
		cache.programParameteri = (ProgramParameteriFn)proc.addr("glProgramParameteri");
		cache.binaries = formats > 0 && cache.programBinary != nullptr && cache.getProgramBinary != nullptr
				&& cache.programParameteri != nullptr;
	}
	if (procs)
	{
		MaxThreadsFn maxThreads = nullptr;
		if (has_extension("GL_KHR_parallel_shader_compile"))
			maxThreads = (MaxThreadsFn)proc.addr("glMaxShaderCompilerThreadsKHR");
		else if (has_extension("GL_ARB_parallel_shader_compile"))
			maxThreads = (MaxThreadsFn)proc.addr("glMaxShaderCompilerThreadsARB");
		if (maxThreads != nullptr)
		{
			// (as many threads as the driver likes)
			maxThreads(0xFFFFFFFFu);
			cache.parallel = true;
		}
	}
	while (glGetError() != GL_NO_ERROR)
		;
	read_file();
	for (int l = 0; l < GLPROG_LISTS; l++)
		cache.noted[l].reserve(1024);
	cache.fresh.reserve(1024);
	INFO_LOG(RENDERER, "OpenGL: programs %s, %u kept; the driver makes them %s",
			cache.binaries ? "kept on disk" : "not kept (no binaries)", (unsigned)cache.entries.size(),
			cache.parallel ? "in the background, if it can" : "as it is asked");
}

void glprog_started()
{
	cache.started = true;
}

bool glprog_binaries()
{
	return cache.binaries;
}

bool glprog_background()
{
	return cache.background;
}

u32 glprog_make(const char *vertex, const char *fragment, bool background)
{
	u64 hash = fnv1a(FnvBasis, "graphics|");
	hash = fnv1a(hash, vertex);
	hash = fnv1a(hash, "|");
	hash = fnv1a(hash, fragment);

	cache.failed = false;
	const GLuint program = glCreateProgram();
	if (from_disk(program, hash))
		return program;

	const GLuint vs = compile(GL_VERTEX_SHADER, &vertex, 1);
	const GLuint fs = compile(GL_FRAGMENT_SHADER, &fragment, 1);
	glAttachShader(program, vs);
	glAttachShader(program, fs);

	/* Bind vertex attribute to VBO inputs */
	glBindAttribLocation(program, VERTEX_POS_ARRAY,      "in_pos");
	glBindAttribLocation(program, VERTEX_COL_BASE_ARRAY, "in_base");
	glBindAttribLocation(program, VERTEX_COL_OFFS_ARRAY, "in_offs");
	glBindAttribLocation(program, VERTEX_UV_ARRAY,       "in_uv");
	glBindAttribLocation(program, VERTEX_COL_BASE1_ARRAY, "in_base1");
	glBindAttribLocation(program, VERTEX_COL_OFFS1_ARRAY, "in_offs1");
	glBindAttribLocation(program, VERTEX_UV1_ARRAY,       "in_uv1");
	glBindAttribLocation(program, VERTEX_PP_ARRAY,        "in_pp");
#ifndef HAVE_OPENGLES
	glBindFragDataLocation(program, 0, "FragColor");
#endif
	if (cache.binaries)
		cache.programParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
	glLinkProgram(program);
	if (background && cache.parallel)
	{
		/* (whether the driver really does it in the background: not done
		 * as soon as it is asked) */
		GLint done = 0;
		glGetProgramiv(program, GL_COMPLETION_STATUS_KHR, &done);
		if (!done)
			cache.background = true;
	}
	else
	{
		GLint ok = 0, said = 0;
		glGetProgramiv(program, GL_LINK_STATUS, &ok);
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &said);
		/* (a driver that does not say why - an OpenGL that does nothing,
		 * headless.sh's - has its program taken as it is, as ever) */
		if (!ok && said > 0)
		{
			log_failure(program, vs, fs);
			WARN_LOG(RENDERER, "VERTEX:\n%s\nFRAGMENT:\n%s\n", vertex, fragment);
			glDeleteShader(vs);
			glDeleteShader(fs);
			glDeleteProgram(program);
			cache.failed = true;
			return 0;
		}
	}
	// (they go with the program)
	glDeleteShader(vs);
	glDeleteShader(fs);
	count_made(program, hash);
	return program;
}

u32 glprog_make_compute(const char *const *pieces, u32 count)
{
	u64 hash = fnv1a(FnvBasis, "compute|");
	for (u32 i = 0; i < count; i++)
		hash = fnv1a(hash, pieces[i]);

	const GLuint program = glCreateProgram();
	if (from_disk(program, hash))
		return program;
	const GLuint cs = compile(GL_COMPUTE_SHADER, pieces, (GLsizei)count);
	glAttachShader(program, cs);
	if (cache.binaries)
		cache.programParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
	glLinkProgram(program);
	GLint ok = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		log_failure(program, cs, 0);
		glDeleteShader(cs);
		glDeleteProgram(program);
		return 0;
	}
	glDetachShader(program, cs);
	glDeleteShader(cs);
	count_made(program, hash);
	return program;
}

bool glprog_failed()
{
	return cache.failed;
}

GlProgramStatus glprog_poll(u32 program)
{
	GLint v = 0;
	if (cache.parallel)
	{
		glGetProgramiv(program, GL_COMPLETION_STATUS_KHR, &v);
		if (!v)
			return GLPROG_PENDING;
	}
	glGetProgramiv(program, GL_LINK_STATUS, &v);
	return v ? GLPROG_READY : GLPROG_FAILED;
}

bool glprog_wait(u32 program)
{
	GLint ok = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (!ok)
		log_failure(program, 0, 0);
	return ok != 0;
}

void glprog_note(GlProgramList list, u32 key)
{
	if (!cache.open)
		return;
	std::vector<u32>& n = cache.noted[list];
	if (n.size() == n.capacity() && cache.started)
		return;	// (no allocation in a frame)
	const std::vector<u32>& k = cache.known[list];
	if (std::binary_search(k.begin(), k.end(), key))
		return;
	n.push_back(key);
	cache.dirty = true;
}

const u32 *glprog_known(GlProgramList list, u32 *count)
{
	*count = (u32)cache.known[list].size();
	return cache.known[list].data();
}

void glprog_general_used()
{
	cache.generalDraws++;
}

void glprog_save(bool contextGoing)
{
	if (!cache.open || !cache.dirty)
	{
		if (contextGoing)
		{
			cache.lost = true;
			cache.fresh.clear();
		}
		return;
	}
	std::vector<u8> out;
	put<u32>(out, CacheMagic);
	put<u32>(out, CacheVersion);
	put<u64>(out, cache.driver);
	put<u32>(out, GLPROG_LISTS);
	for (int l = 0; l < GLPROG_LISTS; l++)
	{
		std::vector<u32> keys = cache.known[l];
		keys.insert(keys.end(), cache.noted[l].begin(), cache.noted[l].end());
		std::sort(keys.begin(), keys.end());
		keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
		if (keys.size() > MostKeys)
			keys.resize(MostKeys);
		put<u32>(out, (u32)keys.size());
		const size_t at = out.size();
		out.resize(at + keys.size() * 4);
		if (!keys.empty())
			memcpy(&out[at], keys.data(), keys.size() * 4);
		cache.known[l].swap(keys);
		cache.noted[l].clear();
	}
	// the binaries: those made this run (where the programs are still there), and the file's others
	const size_t countAt = out.size();
	put<u32>(out, 0);
	u32 count = 0;
	std::vector<u64> written;
	if (cache.binaries && !cache.lost)
		for (const Fresh& f : cache.fresh)
		{
			if (!glIsProgram(f.program) || glprog_poll(f.program) != GLPROG_READY)
				continue;
			GLint length = 0;
			glGetProgramiv(f.program, GL_PROGRAM_BINARY_LENGTH, &length);
			if (length <= 0)
				continue;
			const size_t at = out.size();
			out.resize(at + 16 + (size_t)length);
			GLenum format = 0;
			GLsizei got = 0;
			cache.getProgramBinary(f.program, length, &got, &format, &out[at + 16]);
			if (got <= 0 || got > length)
			{
				out.resize(at);
				continue;
			}
			const u32 f32 = (u32)format, l32 = (u32)got;
			memcpy(&out[at], &f.hash, 8);
			memcpy(&out[at + 8], &f32, 4);
			memcpy(&out[at + 12], &l32, 4);
			out.resize(at + 16 + (size_t)got);
			written.push_back(f.hash);
			count++;
		}
	std::sort(written.begin(), written.end());
	for (const Entry& e : cache.entries)
	{
		if (std::binary_search(written.begin(), written.end(), e.hash))
			continue;
		const size_t at = out.size();
		out.resize(at + 16 + e.length);
		memcpy(&out[at], &e.hash, 8);
		memcpy(&out[at + 8], &e.format, 4);
		memcpy(&out[at + 12], &e.length, 4);
		memcpy(&out[at + 16], &cache.data[e.at], e.length);
		count++;
	}
	memcpy(&out[countAt], &count, 4);
	put<u64>(out, fnv1a(FnvBasis, out.data(), out.size()));
	while (glGetError() != GL_NO_ERROR)
		;

	const std::string path = get_writable_data_path(CacheFile);
	RFILE *f = filestream_open(path.c_str(), RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (f != nullptr)
	{
		const bool ok = filestream_write(f, out.data(), (int64_t)out.size()) == (int64_t)out.size();
		filestream_close(f);
		INFO_LOG(RENDERER, "OpenGL: %u programs kept in %s (%u bytes)%s", count, path.c_str(), (unsigned)out.size(),
				ok ? "" : ": could not be written");
	}

	// what is in the file now is what is looked in from now on
	cache.entries.clear();
	cache.data.clear();
	if (cache.binaries)
	{
		Reader r(out.data() + countAt + 4, out.size() - countAt - 12);
		for (u32 i = 0; i < count && r.ok; i++)
		{
			Entry e;
			e.hash = r.get<u64>();
			e.format = r.get<u32>();
			e.length = r.get<u32>();
			e.at = (size_t)(r.p - out.data());
			r.p += e.length;
			r.left -= e.length;
			cache.entries.push_back(e);
		}
		std::sort(cache.entries.begin(), cache.entries.end(), [](const Entry& a, const Entry& b) { return a.hash < b.hash; });
		cache.data.swap(out);
	}
	cache.fresh.clear();
	cache.dirty = false;
	if (contextGoing)
		cache.lost = true;
}

void glprog_log(const char *who)
{
	if (!cache.open)
		return;
	NOTICE_LOG(RENDERER, "OpenGL programs (%s): %u from disk, %u compiled (%u of them in frames), %u turned down by the driver; "
			"%u polygons drawn with the general program while theirs was not there%s",
			who, cache.fromDisk, cache.compiled, cache.inFrames, cache.refused, cache.generalDraws,
			cache.parallel ? (cache.background ? "; the driver compiles in the background" : "; the driver compiles as it is asked") : "");
}

void glprog_close()
{
	if (!cache.open)
		return;
	glprog_save();
	cache = Cache();
}
