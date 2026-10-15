/* Textures made on the GPU from their bytes in video memory: see
 * gl_texdecoder.h */
#include "gles.h"
#include "gl_texdecoder.h"
#include "glprogram_cache.h"
#include "stdclass.h"
#include <string>
#include <algorithm>

/* The kernels' text (rend/texdecode.h), and their SPIR-V for OpenGL 4.6:
 * tools/rend/kernels_spv.sh made it, and checks that it is the header's */
#include "rend/texdecode_gl.inc"

#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#endif
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT 0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT
#define GL_MAP_COHERENT_BIT 0x0080
#endif
#ifndef GL_MAP_WRITE_BIT
#define GL_MAP_WRITE_BIT 0x0002
#endif
#ifndef GL_TEXTURE_FETCH_BARRIER_BIT
#define GL_TEXTURE_FETCH_BARRIER_BIT 0x00000008
#endif
#ifndef GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT
#define GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT 0x00004000
#endif
#ifndef GL_SHADER_IMAGE_ACCESS_BARRIER_BIT
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x00000020
#endif
#ifndef GL_SHADER_BINARY_FORMAT_SPIR_V
#define GL_SHADER_BINARY_FORMAT_SPIR_V 0x9551
#endif
#ifndef GL_PROGRAM_BINARY_RETRIEVABLE_HINT
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8257
#endif
#ifndef GL_PROGRAM_BINARY_LENGTH
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#endif
#ifndef GL_NUM_PROGRAM_BINARY_FORMATS
#define GL_NUM_PROGRAM_BINARY_FORMATS 0x87FE
#endif
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#endif
#ifndef GL_ALREADY_SIGNALED
#define GL_ALREADY_SIGNALED 0x911A
#endif
#ifndef GL_CONDITION_SATISFIED
#define GL_CONDITION_SATISFIED 0x911C
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_R32UI
#define GL_R32UI 0x8236
#endif
#ifndef GL_RED_INTEGER
#define GL_RED_INTEGER 0x8D94
#endif
#ifndef GL_RGBA8UI
#define GL_RGBA8UI 0x8D7C
#endif
#ifndef GL_R8UI
#define GL_R8UI 0x8232
#endif
#ifndef GL_WRITE_ONLY
#define GL_WRITE_ONLY 0x88B9
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_UNSIGNED_SHORT_1_5_5_5_REV
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4_REV
#define GL_UNSIGNED_SHORT_4_4_4_4_REV 0x8365
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif

GLTexDecoder glTexDecoder;

#ifndef HAVE_OPENGLES2
#ifndef APIENTRY
#define APIENTRY
#endif
/* What glsm does not wrap: the entry points the frontend's loader
 * (glsym/glsym_gl.c) has made, which the glsm header does not name */
extern "C" {
extern void (APIENTRY *__rglgen_glDispatchCompute)(GLuint, GLuint, GLuint);
extern void (APIENTRY *__rglgen_glShaderBinary)(GLsizei, const GLuint *, GLenum, const void *, GLsizei);
extern void (APIENTRY *__rglgen_glSpecializeShader)(GLuint, const GLchar *, GLuint, const GLuint *, const GLuint *);
}
#define glDispatchCompute __rglgen_glDispatchCompute
#define glShaderBinary __rglgen_glShaderBinary
#define glSpecializeShader __rglgen_glSpecializeShader
#endif

/* The binding points and the texture unit the kernels use: above what the
 * renderers use (their storage buffers 0 and 1, image unit 4, texture
 * units 0 to 5) */
#define TEXD_GL_BINDING 7

/* The format, type and internal format of a texture of a type (by
 * TextureType), and how its planar texels go to it as they are */
static void formatOf(TextureType type, GLenum& format, GLenum& gltype)
{
	switch (type)
	{
	case TextureType::_565: format = GL_RGB; gltype = GL_UNSIGNED_SHORT_5_6_5; break;
	case TextureType::_5551: format = GL_BGRA; gltype = GL_UNSIGNED_SHORT_1_5_5_5_REV; break;
	default: format = GL_BGRA; gltype = GL_UNSIGNED_SHORT_4_4_4_4_REV; break;
	}
}

static bool hasExtension(const char *name)
{
#ifndef HAVE_OPENGLES2
	for (GLuint i = 0; ; i++)
	{
		const char *extension = (const char *)glGetStringi(GL_EXTENSIONS, i);
		if (extension == nullptr)
			break;
		if (!strcmp(extension, name))
			return true;
	}
	while (glGetError() != GL_NO_ERROR)
		;
#endif
	return false;
}

static bool versionAtLeast(int major, int minor)
{
	return gl.gl_major > major || (gl.gl_major == major && gl.gl_minor >= minor);
}

void GLTexDecoder::Init()
{
	Term();
#if !defined(HAVE_OPENGLES2) && !defined(MSB_FIRST)
	/* Desktop OpenGL 3 and later. (The kernels read the bytes little-endian,
	 * which a big-endian machine does not have them.) */
	if (gl.is_gles || gl.gl_major < 3)
	{
		INFO_LOG(RENDERER, "OpenGL: textures are converted on the CPU");
		return;
	}
	compute = versionAtLeast(4, 3);
	persistent = compute && (versionAtLeast(4, 4) || hasExtension("GL_ARB_buffer_storage"));
	fences = versionAtLeast(3, 2) || hasExtension("GL_ARB_sync");

	/* The ring: what textures take as they are in video memory - a few
	 * hundred KB in most frames, megabytes when a scene is loaded - a share
	 * of the video memory, 4 to 16 MB */
	u64 bytes = (u64)gl.vram_budget.available_kib() * 1024 / 256;
	bytes = std::max<u64>(bytes, 4 << 20);
	bytes = std::min<u64>(bytes, 16 << 20);
	if (!compute)
	{
		// (as a texture of TEXD_GL_RING_WIDTH words a row: as many rows as the driver has)
		GLint most = 0;
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &most);
		ringRows = (u32)std::min<u64>(bytes / (TEXD_GL_RING_WIDTH * 4), (u64)std::max(most, 0));
		bytes = (u64)ringRows * TEXD_GL_RING_WIDTH * 4;
	}
	size = bytes & ~15ull;
	if (size < (1u << 20))
	{
		INFO_LOG(RENDERER, "OpenGL: textures are converted on the CPU (no room for a ring)");
		return;
	}
	while (glGetError() != GL_NO_ERROR)
		;
	if (compute)
	{
		glGenBuffers(1, &ring);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, ring);
		if (persistent)
		{
			glBufferStorage(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)size, nullptr, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
			mapped = (u8 *)glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)size, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
			if (mapped == nullptr || glGetError() != GL_NO_ERROR)
			{
				// (written through memory of this side's after all)
				mapped = nullptr;
				persistent = false;
				glDeleteBuffers(1, &ring);
				glGenBuffers(1, &ring);
				glBindBuffer(GL_SHADER_STORAGE_BUFFER, ring);
			}
		}
		if (!persistent)
			glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)size, nullptr, GL_DYNAMIC_DRAW);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	}
	else
	{
		glGenTextures(1, &ringTexture);
		glActiveTexture(GL_TEXTURE0 + TEXD_GL_BINDING);
		glBindTexture(GL_TEXTURE_2D, ringTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, TEXD_GL_RING_WIDTH, ringRows, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		glBindTexture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE0);
	}
	if (!persistent)
	{
		shadow = (u8 *)malloc((size_t)size);
		mapped = shadow;
	}
	glGenFramebuffers(1, &framebuffer);
	glGenVertexArrays(1, &vertexArray);
	if (mapped == nullptr || glGetError() != GL_NO_ERROR || !MakePrograms())
	{
		WARN_LOG(RENDERER, "OpenGL: the texture kernels could not be made: textures are converted on the CPU");
		Term();
		return;
	}

	/* What can be drawn into: a framebuffer with a level of a texture of
	 * each format, tried once now */
	static const GLenum internal[5] = { GL_RGB565, GL_RGB5_A1, GL_RGBA4, GL_RGBA8, GL_R8 };
	GLint was = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &was);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	for (int t = 0; t < 5; t++)
	{
		GLuint tex = 0;
		glGenTextures(1, &tex);
		glcache.BindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, internal[t], 8, 8, 0, t == 4 ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
		const bool drawn = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glcache.DeleteTextures(1, &tex);
		// (32-bit texels and indices by the compute kernels where there are any)
		takes[t] = t >= 3 && compute ? true : drawn;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, was);
	native[0] = native[1] = native[2] = true;
	while (glGetError() != GL_NO_ERROR)
		;
	head = tail = flushed = 0;
	jobCount = levelCount = pendingCount = 0;
	ready = true;
	NOTICE_LOG(RENDERER, "OpenGL: textures are made on the GPU (%s, a ring of %u KB %s; 16-bit drawn %d%d%d, 32-bit %d, 8-bit %d)",
			compute ? "compute kernels" : "drawn", (u32)(size >> 10), persistent ? "mapped" : "written once a frame",
			takes[0], takes[1], takes[2], takes[3], takes[4]);
#endif
}

void GLTexDecoder::Term()
{
	if (ready)
		NOTICE_LOG(RENDERER, "OpenGL: textures made on the GPU: %u levels, %u copied as they are, %u MB of bytes; %u not, for want of room",
				(u32)statLevels, (u32)statCopies, (u32)(statBytes >> 20), (u32)statFull);
#ifndef HAVE_OPENGLES2
	for (u32 i = 0; i < pendingCount; i++)
		glDeleteSync((GLsync)pending[i].fence);
	pendingCount = 0;
	for (GLuint& p : programs)
	{
		if (p != 0)
			glDeleteProgram(p);
		p = 0;
	}
	if (mapped != nullptr && persistent && ring != 0)
	{
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, ring);
		glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	}
	if (ring != 0)
		glDeleteBuffers(1, &ring);
	if (ringTexture != 0)
		glDeleteTextures(1, &ringTexture);
	if (framebuffer != 0)
		glDeleteFramebuffers(1, &framebuffer);
	if (vertexArray != 0)
		glDeleteVertexArrays(1, &vertexArray);
#endif
	ring = ringTexture = framebuffer = vertexArray = 0;
	free(shadow);
	shadow = nullptr;
	mapped = nullptr;
	ready = compute = persistent = fences = false;
	for (bool& t : takes)
		t = false;
	native[0] = native[1] = native[2] = false;
	jobCount = levelCount = 0;
	size = head = tail = flushed = 0;
}

bool GLTexDecoder::Takes(TextureType type, u32 out) const
{
	if (!ready || (unsigned)type >= 5 || !takes[(int)type])
		return false;
	// (the 32-bit and 8-bit kinds are made into their own types)
	return out == TEXD_OUT16 ? (unsigned)type < 3 : out == TEXD_OUT32 ? type == TextureType::_8888 : type == TextureType::_8;
}

/* ---- The programs ---- */

/* A program of @stages shaders, from SPIR-V if @spv, from text otherwise.
 * 0 if it does not link. */
GLuint GLTexDecoder::Link(const u32 *spv, const size_t *spvSize, const char *const *sources, int stages, bool keep)
{
#ifndef HAVE_OPENGLES2
	static const GLenum computeStage[] = { GL_COMPUTE_SHADER };
	static const GLenum drawStages[] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	const GLenum *kinds = stages == 1 ? computeStage : drawStages;
	GLuint program = glCreateProgram();
	GLuint shaders[2] = {};
	bool ok = true;
	for (int i = 0; i < stages && ok; i++)
	{
		shaders[i] = glCreateShader(kinds[i]);
		if (spv != nullptr)
		{
			glShaderBinary(1, &shaders[i], GL_SHADER_BINARY_FORMAT_SPIR_V, (const u8 *)spv + (i == 0 ? 0 : spvSize[0]), (GLsizei)spvSize[i]);
			glSpecializeShader(shaders[i], "main", 0, nullptr, nullptr);
		}
		else
		{
			const GLchar *text = sources[i];
			glShaderSource(shaders[i], 1, &text, nullptr);
			glCompileShader(shaders[i]);
		}
		GLint status = GL_FALSE;
		glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &status);
		if (status != GL_TRUE)
		{
			char log[1024] = "";
			glGetShaderInfoLog(shaders[i], sizeof(log), nullptr, log);
			WARN_LOG(RENDERER, "OpenGL: a texture kernel did not compile: %s", log);
			ok = false;
			break;
		}
		glAttachShader(program, shaders[i]);
	}
	if (ok)
	{
		if (keep)
			glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
		glLinkProgram(program);
		GLint status = GL_FALSE;
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		ok = status == GL_TRUE;
	}
	for (int i = 0; i < stages; i++)
		if (shaders[i] != 0)
		{
			if (ok)
				glDetachShader(program, shaders[i]);
			glDeleteShader(shaders[i]);
		}
	if (!ok)
	{
		glDeleteProgram(program);
		return 0;
	}
	return program;
#else
	return 0;
#endif
}

/* The programs, all of them now: from SPIR-V with OpenGL 4.6; else from
 * their text, through the programs kept on disk (glprogram_cache.h) */
bool GLTexDecoder::MakePrograms()
{
#ifndef HAVE_OPENGLES2
	const bool spirv = compute && (versionAtLeast(4, 6) || hasExtension("GL_ARB_gl_spirv"));
	if (spirv)
	{
		const size_t s32[1] = { texdecode_glspv_size[1] }, s8[1] = { texdecode_glspv_size[2] };
		programs[PROGRAM_32] = Link(texdecode_glspv[1], s32, nullptr, 1, false);
		programs[PROGRAM_8] = Link(texdecode_glspv[2], s8, nullptr, 1, false);
		// (the two stages' words one after the other)
		static u32 draw[sizeof(texdecode_glspv_3) / 4 + sizeof(texdecode_glspv_4) / 4];
		memcpy(draw, texdecode_glspv_3, sizeof(texdecode_glspv_3));
		memcpy(draw + sizeof(texdecode_glspv_3) / 4, texdecode_glspv_4, sizeof(texdecode_glspv_4));
		const size_t sd[2] = { sizeof(texdecode_glspv_3), sizeof(texdecode_glspv_4) };
		programs[PROGRAM_DRAW] = Link(draw, sd, nullptr, 2, false);
		if (programs[PROGRAM_32] != 0 && programs[PROGRAM_8] != 0 && programs[PROGRAM_DRAW] != 0)
		{
			for (int p = 0; p < PROGRAMS; p++)
			{
				// (the locations the kernels give their uniforms: SPIR-V has no names)
				locations[p][0] = 0;
				locations[p][1] = 1;
			}
			NOTICE_LOG(RENDERER, "OpenGL: texture kernels from SPIR-V");
			return true;
		}
		for (GLuint& p : programs)
		{
			if (p != 0)
				glDeleteProgram(p);
			p = 0;
		}
		WARN_LOG(RENDERER, "OpenGL: the texture kernels' SPIR-V was refused: compiled instead");
	}

	/* The text of each: the version this context has, then the header */
	std::string head;
	if (compute)
		head = "#version 430\n";
	else if (versionAtLeast(3, 3))
		head = "#version 330\n";
	else if (versionAtLeast(3, 2))
		head = "#version 150\n";
	else
		head = "#version 130\n";
	head += "#define TEXD_SHADER_GLSL\n";
	head += compute ? "#define TEXD_GL_RING 1\n" : "#define TEXD_GL_RING 2\n";
	const std::string text[4] = {
		head + "#define TEXD_KERNEL_GL 1\n" + texdecode_glsl,
		head + "#define TEXD_KERNEL_GL 2\n" + texdecode_glsl,
		head + "#define TEXD_KERNEL_GL 3\n" + texdecode_glsl,
		head + "#define TEXD_KERNEL_GL 4\n" + texdecode_glsl,
	};

	/* Through the programs kept on disk (glprogram_cache.h): compiled the
	 * first time, taken from there after, with the renderers' own */
	programs[PROGRAM_DRAW] = glprog_make(text[2].c_str(), text[3].c_str());
	if (glprog_failed())
		programs[PROGRAM_DRAW] = 0;
	if (compute)
	{
		const char *k32 = text[0].c_str(), *k8 = text[1].c_str();
		programs[PROGRAM_32] = glprog_make_compute(&k32, 1);
		programs[PROGRAM_8] = glprog_make_compute(&k8, 1);
	}
	if (programs[PROGRAM_DRAW] == 0 || (compute && (programs[PROGRAM_32] == 0 || programs[PROGRAM_8] == 0)))
		return false;
	NOTICE_LOG(RENDERER, "OpenGL: texture kernels from their text, or the programs kept");
	for (int p = 0; p < PROGRAMS; p++)
	{
		if (programs[p] == 0)
			continue;
		locations[p][0] = glGetUniformLocation(programs[p], "texd_lv0");
		locations[p][1] = glGetUniformLocation(programs[p], "texd_lv1");
		if (!compute)
		{
			glcache.UseProgram(programs[p]);
			glUniform1i(glGetUniformLocation(programs[p], "texd_ring"), TEXD_GL_BINDING);
		}
	}
	return true;
#else
	return false;
#endif
}

/* ---- The ring ---- */

void GLTexDecoder::Reclaim()
{
#ifndef HAVE_OPENGLES2
	// (the frames are done in the order they went)
	u32 done = 0;
	while (done < pendingCount)
	{
		const GLenum r = glClientWaitSync((GLsync)pending[done].fence, 0, 0);
		if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED)
			break;
		glDeleteSync((GLsync)pending[done].fence);
		tail = pending[done].end;
		done++;
	}
	if (done != 0)
	{
		for (u32 i = done; i < pendingCount; i++)
			pending[i - done] = pending[i];
		pendingCount -= done;
	}
#endif
}

u8 *GLTexDecoder::Begin(u32 bytes, u32 count, u32 *offset)
{
	bytes = (bytes + 15) & ~15u;
	if (!ready || bytes > size || count > MAX_LEVELS)
	{
		statFull++;
		return nullptr;
	}
	if (jobCount >= MAX_JOBS || levelCount + count > MAX_LEVELS)
		/* What the lists have goes to the GPU first, its bytes with it: the
		 * bytes are handed out after that */
		Flush();
	for (int attempt = 0; attempt < 2; attempt++)
	{
		const u64 at = head % size;
		const u64 skip = at + bytes > size ? size - at : 0;
		if (head + skip + bytes - tail <= size)
		{
			head += skip;
			*offset = (u32)(head % size);
			head += bytes;
			statBytes += bytes;
			return mapped + *offset;
		}
		if (attempt == 0 && persistent && fences)
			Reclaim();
	}
	statFull++;
	return nullptr;
}

u32 GLTexDecoder::AddDecode(GLuint texture, TextureType type, const texd_plan& plan, u32 offset)
{
	Job& job = jobs[jobCount];
	job.texture = texture;
	job.first = levelCount;
	job.count = plan.levels;
	job.type = (u8)type;
	job.copy = false;
	job.cancelled = false;
	const u32 d3d16 = plan.out != TEXD_OUT16 ? 0
			: TEXD_D3D16(type == TextureType::_5551 ? TEXD_D3D_5551 : type == TextureType::_4444 ? TEXD_D3D_4444 : TEXD_D3D_565);
	for (u32 i = 0; i < plan.levels; i++)
	{
		const texd_level& lv = plan.level[i];
		Level& l = levels[levelCount++];
		l.pc[0] = offset / 4;
		l.pc[1] = lv.data;
		l.pc[2] = plan.pal;
		l.pc[3] = lv.wh;
		l.pc[4] = lv.sr;
		l.pc[5] = lv.mode | d3d16;
		l.mip = lv.mip;
	}
	statLevels += plan.levels;
	return jobCount++;
}

u32 GLTexDecoder::AddCopy(GLuint texture, TextureType type, u32 offset, u32 rowLength, u32 width, u32 height)
{
	Job& job = jobs[jobCount];
	job.texture = texture;
	job.first = job.count = 0;
	job.offset = offset;
	job.rowLength = rowLength;
	job.width = width;
	job.height = height;
	job.type = (u8)type;
	job.copy = true;
	job.cancelled = false;
	statCopies++;
	return jobCount++;
}

void GLTexDecoder::Cancel(u32 job, u32 serial)
{
	if (serial == this->serial && job < jobCount)
		jobs[job].cancelled = true;
}

/* The ring's bytes from @from to @to (counts, not offsets) to where the
 * kernels read them, written through this side's memory */
void GLTexDecoder::Upload(u64 from, u64 to)
{
#ifndef HAVE_OPENGLES2
	while (from < to)
	{
		const u64 at = from % size;
		const u64 n = std::min(to - from, size - at);
		if (compute)
			glBufferSubData(GL_SHADER_STORAGE_BUFFER, (GLintptr)at, (GLsizeiptr)n, shadow + at);
		else
		{
			// (whole rows: what the row has before is what it had, and is copied again)
			const u32 row = TEXD_GL_RING_WIDTH * 4;
			const u32 first = (u32)(at / row), last = (u32)((at + n + row - 1) / row);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, first, TEXD_GL_RING_WIDTH, last - first, GL_RED_INTEGER, GL_UNSIGNED_INT, shadow + (size_t)first * row);
		}
		from += n;
	}
#endif
}

void GLTexDecoder::Flush()
{
#ifndef HAVE_OPENGLES2
	if (!ready)
		return;
	serial++;
	u32 live = 0;
	for (u32 i = 0; i < jobCount; i++)
		live += !jobs[i].cancelled;
	if (live == 0)
	{
		jobCount = levelCount = 0;
		if (!persistent)
			head = tail = flushed = 0;
		return;
	}

	GLint wasFramebuffer = 0, wasArray = 0, wasUnit = 0, viewport[4] = {};
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &wasFramebuffer);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &wasArray);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &wasUnit);
	glGetIntegerv(GL_VIEWPORT, viewport);
	glActiveTexture(GL_TEXTURE0 + TEXD_GL_BINDING);

	// the frame's bytes where the kernels read them
	if (compute)
	{
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, ring);
		if (!persistent)
			Upload(flushed, head);
		else
			// (written through the mapping: seen by what is recorded after this)
			glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TEXD_GL_BINDING, ring);
	}
	else
	{
		glBindTexture(GL_TEXTURE_2D, ringTexture);
		Upload(flushed, head);
	}

	/* The planar textures copied as they are: from the ring itself where it
	 * is a buffer, from this side's memory where it is not */
	bool copies = false;
	for (u32 i = 0; i < jobCount; i++)
	{
		const Job& job = jobs[i];
		if (job.cancelled || !job.copy)
			continue;
		if (!copies)
		{
			copies = true;
			if (compute)
				glBindBuffer(GL_PIXEL_UNPACK_BUFFER, ring);
		}
		GLenum format, type;
		formatOf((TextureType)job.type, format, type);
		glBindTexture(GL_TEXTURE_2D, job.texture);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, job.rowLength);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, job.width, job.height, format, type,
				compute ? (const void *)(uintptr_t)job.offset : (const void *)(shadow + job.offset));
	}
	if (copies)
	{
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		if (compute)
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	}
	if (!compute)
		glBindTexture(GL_TEXTURE_2D, ringTexture);
	else
		glBindTexture(GL_TEXTURE_2D, 0);

	// 32-bit texels and indices by the compute kernels
	bool computed = false;
	if (compute)
		for (int p = PROGRAM_32; p <= PROGRAM_8; p++)
		{
			bool bound = false;
			for (u32 i = 0; i < jobCount; i++)
			{
				const Job& job = jobs[i];
				const u32 out = p == PROGRAM_32 ? TEXD_OUT32 : TEXD_OUT8;
				if (job.cancelled || job.copy || ((levels[job.first].pc[5] >> 6) & 3) != out)
					continue;
				if (!bound)
				{
					glcache.UseProgram(programs[p]);
					bound = true;
				}
				for (u32 l = job.first; l < job.first + job.count; l++)
				{
					const Level& lv = levels[l];
					glUniform4ui(locations[p][0], lv.pc[0], lv.pc[1], lv.pc[2], lv.pc[3]);
					glUniform2ui(locations[p][1], lv.pc[4], lv.pc[5]);
					glBindImageTexture(TEXD_GL_BINDING, job.texture, lv.mip, GL_FALSE, 0, GL_WRITE_ONLY,
							out == TEXD_OUT32 ? GL_RGBA8UI : GL_R8UI);
					glDispatchCompute(((lv.pc[3] & 0xFFFF) + 7) / 8, ((lv.pc[3] >> 16) + 7) / 8, 1);
				}
				computed = true;
			}
		}
	if (computed)
	{
		glBindImageTexture(TEXD_GL_BINDING, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8UI);
		glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	}

	// and the rest drawn
	bool drawn = false;
	for (u32 i = 0; i < jobCount; i++)
	{
		const Job& job = jobs[i];
		if (job.cancelled || job.copy)
			continue;
		const u32 out = (levels[job.first].pc[5] >> 6) & 3;
		if (compute && out != TEXD_OUT16)
			continue;
		if (!drawn)
		{
			drawn = true;
			glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
			glBindVertexArray(vertexArray);
			glcache.UseProgram(programs[PROGRAM_DRAW]);
			glcache.Disable(GL_BLEND);
			glcache.Disable(GL_DEPTH_TEST);
			glcache.Disable(GL_STENCIL_TEST);
			glcache.Disable(GL_SCISSOR_TEST);
			glcache.Disable(GL_CULL_FACE);
			glDisable(GL_DITHER);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		}
		for (u32 l = job.first; l < job.first + job.count; l++)
		{
			const Level& lv = levels[l];
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, job.texture, lv.mip);
			glViewport(0, 0, lv.pc[3] & 0xFFFF, lv.pc[3] >> 16);
			glUniform4ui(locations[PROGRAM_DRAW][0], lv.pc[0], lv.pc[1], lv.pc[2], lv.pc[3]);
			glUniform2ui(locations[PROGRAM_DRAW][1], lv.pc[4], lv.pc[5]);
			glDrawArrays(GL_TRIANGLES, 0, 3);
		}
	}
	if (drawn)
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glBindVertexArray(wasArray);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, wasFramebuffer);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	glActiveTexture(wasUnit);

	/* What the ring had up to here is the GPU's until this frame is done:
	 * its fence. Without a mapping the bytes went with the calls above, and
	 * the ring starts again with the next frame. */
	if (persistent)
	{
		flushed = head;
		if (fences)
		{
			if (pendingCount == MAX_FENCES)
			{
				/* (the newest fence goes, and this one, later, stands for
				 * what it stood for too) */
				pendingCount--;
				glDeleteSync((GLsync)pending[pendingCount].fence);
			}
			pending[pendingCount].fence = (void *)glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			pending[pendingCount].end = head;
			pendingCount++;
		}
	}
	else
		head = tail = flushed = 0;
	jobCount = levelCount = 0;
#endif
}
