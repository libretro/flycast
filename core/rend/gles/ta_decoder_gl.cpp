/* GPU-side TA processing in OpenGL 4.3: see ta_decoder_gl.h */
#include "gles.h"
#include "ta_decoder_gl.h"
#include "glprogram_cache.h"
#include "rend/ta_gpu.h"
#include "stdclass.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

extern int screen_height;

/* The kernels, by TAG_K_*: SPIR-V for OpenGL, and the header's text, made
 * from rend/ta_gpu.h when the core was built (tools/rend/kernels_spv.sh,
 * which checks that they still are its) */
#include "rend/ta_gpu_gl_spv.inc"
#include "rend/ta_gpu_glsl.inc"
static_assert(sizeof(ta_gpu_gl_spv) / sizeof(ta_gpu_gl_spv[0]) == TAG_KERNELS + 1, "a kernel for each TAG_K_*");
// (glDrawElementsIndirect()'s command: count, instances, first index, base vertex, base instance)
static_assert(TAG_C_WORDS == 5, "a record is an indexed indirect draw");

#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT 0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT
#define GL_MAP_COHERENT_BIT 0x0080
#endif
#ifndef GL_CLIENT_STORAGE_BIT
#define GL_CLIENT_STORAGE_BIT 0x0200
#endif
#ifndef GL_PARAMETER_BUFFER_ARB
#define GL_PARAMETER_BUFFER_ARB 0x80EE
#endif
#ifndef GL_SHADER_BINARY_FORMAT_SPIR_V_ARB
#define GL_SHADER_BINARY_FORMAT_SPIR_V_ARB 0x9551
#endif
#ifndef GL_DRAW_INDIRECT_BUFFER
#define GL_DRAW_INDIRECT_BUFFER 0x8F3F
#endif
#ifndef GL_DISPATCH_INDIRECT_BUFFER
#define GL_DISPATCH_INDIRECT_BUFFER 0x90EE
#endif
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_COMMAND_BARRIER_BIT
#define GL_COMMAND_BARRIER_BIT 0x00000040
#endif
#ifndef GL_SHADER_STORAGE_BARRIER_BIT
#define GL_SHADER_STORAGE_BARRIER_BIT 0x00002000
#endif
#ifndef GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT
#define GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT 0x00000001
#endif
#ifndef GL_ELEMENT_ARRAY_BARRIER_BIT
#define GL_ELEMENT_ARRAY_BARRIER_BIT 0x00000002
#endif
#ifndef GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS
#define GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS 0x90DB
#endif
#ifndef GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS
#define GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS 0x90DD
#endif
#ifndef GL_MAX_SHADER_STORAGE_BLOCK_SIZE
#define GL_MAX_SHADER_STORAGE_BLOCK_SIZE 0x90DE
#endif
#ifndef GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT
#define GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT 0x90DF
#endif
#ifndef GL_PROGRAM_BINARY_RETRIEVABLE_HINT
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8257
#endif
#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8E28
#endif
#ifndef GL_R32UI
#define GL_R32UI 0x8236
#endif

/* What this needs of OpenGL 4.3 and later that the state manager (glsm)
 * has no function for: taken from the frontend by name when it is set up */
static struct
{
	void (APIENTRYP shaderBinary)(GLsizei count, const GLuint *shaders, GLenum format, const void *binary, GLsizei length);
	void (APIENTRYP specializeShader)(GLuint shader, const GLchar *entry, GLuint n, const GLuint *index, const GLuint *value);
	void (APIENTRYP queryCounter)(GLuint id, GLenum target);
	void (APIENTRYP getQueryObjectiv)(GLuint id, GLenum pname, GLint *params);
	void (APIENTRYP getQueryObjectui64v)(GLuint id, GLenum pname, GLuint64 *params);
	void (APIENTRYP clearBufferSubData)(GLenum target, GLenum internalformat, GLintptr offset, GLsizeiptr size, GLenum format,
			GLenum type, const void *data);
	void (APIENTRYP bindBufferRange)(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
	void (APIENTRYP dispatchCompute)(GLuint x, GLuint y, GLuint z);
	void (APIENTRYP dispatchComputeIndirect)(GLintptr indirect);
	void (APIENTRYP drawElementsIndirect)(GLenum mode, GLenum type, const void *indirect);
	void (APIENTRYP multiDrawElementsIndirect)(GLenum mode, GLenum type, const void *indirect, GLsizei drawcount, GLsizei stride);
	// (OpenGL 4.6, ARB_indirect_parameters; null without)
	void (APIENTRYP multiDrawElementsIndirectCount)(GLenum mode, GLenum type, const void *indirect, GLintptr drawcount,
			GLsizei maxdrawcount, GLsizei stride);
	void (APIENTRYP vertexAttribDivisor)(GLuint index, GLuint divisor);
	void (APIENTRYP vertexAttribI4ui)(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w);
} gx;

template <typename T>
static bool gl_function(T& fn, retro_get_proc_address_t get, const char *name, const char *alternative = nullptr)
{
	fn = (T)get(name);
	if (fn == nullptr && alternative != nullptr)
		fn = (T)get(alternative);
	return fn != nullptr;
}

/* The frames whose tables (and sorted indices) can be in flight at once: a
 * frame that finds its piece of the ring still the GPU's is parsed */
#define TA_GL_FRAMES 3

// The buffers, in the kernels' bindings' order from TAG_GL_BINDING + 1 (the frame's data is TAG_GL_BINDING)
enum { B_TAB, B_VTX, B_MOD, B_IDX, B_WORK, B_REC, B_CMD, B_FRM, B_TRD, B_TRP, B_COUNT };

struct GlTaDecoder::Resources
{
	GLuint pool = 0;		// the TA contexts' data
	u8 *poolMap = nullptr;
	GLuint buffers[B_COUNT] = {};
	u32 *tables = nullptr;		// mapped: a piece for each frame
	u32 tableWords = 0;
	u32 slice = 0, slices = 0;
	TaGpuLayout layout;
	GLuint programs[TAG_KERNELS + 1] = {};
	GLuint sorted = 0;		// (TA_GPU_SORT) the sorted indices of each frame of the ring
	u8 *sortedMap = nullptr;
	GLintptr sortSlot = 0;		// bytes; 0: none
	GLuint instances = 0;
	GLsync fences[TA_GL_FRAMES] = {};
	u32 slotSerial[TA_GL_FRAMES] = {};
	GLuint queries[2 * TA_GL_FRAMES] = {};	// GPU time, read once the frame's fence has signalled
	bool timed[TA_GL_FRAMES] = {};
	GLintptr shadowCommands = 0;	// bytes
	u32 trParamsBytes = 0;

	~Resources()
	{
		for (int i = 0; i < TA_GL_FRAMES; i++)
			if (fences[i] != nullptr)
				glDeleteSync(fences[i]);
		if (queries[0] != 0)
			glDeleteQueries(2 * TA_GL_FRAMES, queries);
		for (int k = 1; k <= TAG_KERNELS; k++)
			if (programs[k] != 0)
				glDeleteProgram(programs[k]);
		// (a persistently mapped buffer is unmapped as it goes)
		glDeleteBuffers(B_COUNT, buffers);
		if (pool != 0)
			glDeleteBuffers(1, &pool);
		if (sorted != 0)
			glDeleteBuffers(1, &sorted);
		if (instances != 0)
			glDeleteBuffers(1, &instances);
	}
};

GlTaDecoder::GlTaDecoder() {}
GlTaDecoder::~GlTaDecoder() {}

static bool gl_has_extension(const char *name)
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

static bool gl_version_at_least(int major, int minor)
{
	return gl.gl_major > major || (gl.gl_major == major && gl.gl_minor >= minor);
}

static bool program_linked(GLuint program)
{
	GLint ok = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	return ok != 0;
}

// a compute shader made into a program; 0 if it is not one
static GLuint link_compute(GLuint shader)
{
	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[1024];
		log[0] = 0;
		glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
		WARN_LOG(RENDERER, "OpenGL: a TA processing kernel did not compile: %s", log);
		glDeleteShader(shader);
		return 0;
	}
	const GLuint program = glCreateProgram();
	glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
	glAttachShader(program, shader);
	glLinkProgram(program);
	glDetachShader(program, shader);
	glDeleteShader(shader);
	if (!program_linked(program))
	{
		glDeleteProgram(program);
		return 0;
	}
	return program;
}

/* The kernels from SPIR-V (OpenGL 4.6, ARB_gl_spirv): the driver makes its
 * own code of them now */
static bool kernels_from_spirv(GLuint *programs)
{
	if (gx.shaderBinary == nullptr || gx.specializeShader == nullptr)
		return false;
	for (int k = 1; k <= TAG_KERNELS; k++)
	{
		const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
		gx.shaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V_ARB, ta_gpu_gl_spv[k], (GLsizei)ta_gpu_gl_spv_size[k]);
		gx.specializeShader(shader, "main", 0, nullptr, nullptr);
		programs[k] = link_compute(shader);
		if (programs[k] == 0)
			return false;
	}
	return true;
}

/* ---- The kernels from their text, through the programs kept on disk
 * (glprogram_cache.h): compiled the first time, taken from there after ---- */

static bool kernels_from_text(GLuint *programs)
{
	const size_t lines = sizeof(ta_gpu_glsl) / sizeof(ta_gpu_glsl[0]);
	std::vector<const char *> source(lines + 1);
	for (size_t i = 0; i < lines; i++)
		source[i + 1] = ta_gpu_glsl[i];
	for (int k = 1; k <= TAG_KERNELS; k++)
	{
		char prefix[128];
		snprintf(prefix, sizeof(prefix), "#version 430\n#define TAG_SHADER_GLSL\n#define TAG_SHADER_GL\n#define TAG_KERNEL %d\n", k);
		source[0] = prefix;
		programs[k] = glprog_make_compute(source.data(), (u32)source.size());
		if (programs[k] == 0)
			return false;
	}
	return true;
}

static void delete_programs(GLuint *programs)
{
	for (int k = 1; k <= TAG_KERNELS; k++)
		if (programs[k] != 0)
		{
			glDeleteProgram(programs[k]);
			programs[k] = 0;
		}
}

/* ---- */

// An immutable buffer of @bytes: for the GPU only (flags 0), or mapped persistently
static GLuint make_buffer(GLsizeiptr bytes, GLbitfield flags, void **map, const void *data = nullptr)
{
	GLuint b = 0;
	glGenBuffers(1, &b);
	glBindBuffer(GL_COPY_WRITE_BUFFER, b);
	glBufferStorage(GL_COPY_WRITE_BUFFER, bytes, data, flags);
	if (map != nullptr)
		*map = glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, bytes, flags & ~GL_CLIENT_STORAGE_BIT);
	glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
	return b;
}

/* Whether reading the mapped memory goes at about the speed of ordinary
 * memory: write-combined memory, which some drivers give a persistent
 * mapping, is many times slower to read, and the SH4 reads every control
 * word back from it (and a save state all of it) */
static bool mapping_reads_fast(const u8 *map, size_t bytes)
{
	std::vector<u8> plain(bytes, 1);
	u8 *m = const_cast<u8 *>(map);
	memset(m, 1, bytes);
	auto sum = [](const u8 *p, size_t n) {
		u64 s = 0;
		for (size_t i = 0; i < n; i += 64)
			s += *(const volatile u64 *)(p + i);
		return s;
	};
	u64 dummy = 0;
	const auto t0 = std::chrono::steady_clock::now();
	for (int r = 0; r < 4; r++)
		dummy += sum(plain.data(), bytes);
	const auto t1 = std::chrono::steady_clock::now();
	for (int r = 0; r < 4; r++)
		dummy += sum(map, bytes);
	const auto t2 = std::chrono::steady_clock::now();
	memset(m, 0, bytes);
	const double plainUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
	const double mapUs = std::chrono::duration<double, std::micro>(t2 - t1).count();
	INFO_LOG(RENDERER, "OpenGL: the TA contexts' mapped memory reads in %.0f us, ordinary memory in %.0f us (%llu)",
			mapUs, plainUs, (unsigned long long)(dummy & 1));
	return mapUs <= 4 * plainUs + 200;
}

bool GlTaDecoder::ContextCanDo()
{
	return !gl.is_gles && gl_version_at_least(4, 3) && (gl_version_at_least(4, 4) || gl_has_extension("GL_ARB_buffer_storage"));
}

bool GlTaDecoder::Init()
{
#ifdef MSB_FIRST
	// (the kernels read the blocks little-endian)
	failed = true;
	return false;
#endif
	auto no = [this](const char *why) {
		INFO_LOG(RENDERER, "OpenGL: TA processing stays on the CPU (%s)", why);
		failed = true;
		return false;
	};
	if (gl.is_gles || !gl_version_at_least(4, 3))
		return no("OpenGL 4.3 is needed");
	// (what errors others left, so that those found below are this one's)
	for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++)
		;
	if (gl.index_type != GL_UNSIGNED_INT)
		return no("16-bit indices");
	/* The frame's data where the SH4 writes it, read by the GPU there: a
	 * persistent mapping. (Without one each frame's data would be copied
	 * up as well; it stays on the CPU instead.) */
	if (!(gl_version_at_least(4, 4) || gl_has_extension("GL_ARB_buffer_storage")))
		return no("no persistently mapped buffers: OpenGL 4.4 or ARB_buffer_storage");
	glsm_ctx_proc_address_t proc = { nullptr };
	if (!glsm_ctl(GLSM_CTL_PROC_ADDRESS_GET, &proc) || proc.addr == nullptr)
		return no("no way to its functions");
	const retro_get_proc_address_t get = proc.addr;
	if (!gl_function(gx.queryCounter, get, "glQueryCounter") || !gl_function(gx.getQueryObjectiv, get, "glGetQueryObjectiv")
			|| !gl_function(gx.getQueryObjectui64v, get, "glGetQueryObjectui64v")
			|| !gl_function(gx.clearBufferSubData, get, "glClearBufferSubData") || !gl_function(gx.bindBufferRange, get, "glBindBufferRange")
			|| !gl_function(gx.dispatchCompute, get, "glDispatchCompute") || !gl_function(gx.dispatchComputeIndirect, get, "glDispatchComputeIndirect")
			|| !gl_function(gx.drawElementsIndirect, get, "glDrawElementsIndirect")
			|| !gl_function(gx.multiDrawElementsIndirect, get, "glMultiDrawElementsIndirect")
			|| !gl_function(gx.vertexAttribDivisor, get, "glVertexAttribDivisor") || !gl_function(gx.vertexAttribI4ui, get, "glVertexAttribI4ui"))
		return no("a function of OpenGL 4.3 is missing");
	gx.shaderBinary = nullptr;
	gx.specializeShader = nullptr;
	if (gl_version_at_least(4, 6) || gl_has_extension("GL_ARB_gl_spirv"))
	{
		gl_function(gx.shaderBinary, get, "glShaderBinary");
		gl_function(gx.specializeShader, get, gl_version_at_least(4, 6) ? "glSpecializeShader" : "glSpecializeShaderARB", "glSpecializeShaderARB");
	}
	/* Indirect draws of several at once (OpenGL 4.3), with their count from
	 * the GPU where there is ARB_indirect_parameters (OpenGL 4.6) */
	gx.multiDrawElementsIndirectCount = nullptr;
	if (gl_version_at_least(4, 6))
		gl_function(gx.multiDrawElementsIndirectCount, get, "glMultiDrawElementsIndirectCount", "glMultiDrawElementsIndirectCountARB");
	else if (gl_has_extension("GL_ARB_indirect_parameters"))
		gl_function(gx.multiDrawElementsIndirectCount, get, "glMultiDrawElementsIndirectCountARB");
	GLint blocks = 0, bindings = 0, align = 0;
	GLint64 blockSize = 0;
	glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &blocks);
	glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &bindings);
	glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &align);
	glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &blockSize);
	// (and the prepass's kernel's sorted indices after them)
	if (blocks < B_COUNT + 1 || bindings < TAG_GL_BINDING + B_COUNT + 2)
		return no("not enough storage buffers for the kernels");
	if (align <= 0 || align > 4096)
		align = 256;

	TaGpuSizes sz;
	ta_gpu_sizes(&sz);
	std::unique_ptr<Resources> r(new Resources());
	const u32 strips = sz.list[0] + sz.list[1] + sz.list[2];
	u32 work[9];
	const u32 workWords = tag_work_layout(sz.verts, strips, sz.headers, work);
	r->slice = ta_data_size();
	/* The contexts: every frame in flight, one being drawn, and the few the
	 * game writes to at a time and renders to textures with; and spares, as
	 * many as a thirty-second of the memory there is to use allows, up to
	 * four more (Vulkan's rule): a frame that finds them all taken is drawn
	 * from the CPU's parse */
	const u64 budget = (u64)gl.vram_budget.available_kib() * 1024 / 32;
	const u32 spares = (u32)std::min<u64>(budget / r->slice, 4);
	r->slices = std::min<u32>(TA_GL_FRAMES + 4 + spares, 16);
	const u32 alignWords = (u32)align / 4;
	r->tableWords = (sz.table_words + alignWords - 1) / alignWords * alignWords;
	r->tableWords = (r->tableWords + 63) & ~63u;
	const u32 trdSlots = sz.verts + sz.headers;
	const u64 most = blockSize > 0 ? (u64)blockSize : 0;
	if ((u64)r->slice > most || (u64)sz.verts * TAG_V_WORDS * 4 > most || (u64)workWords * 4 > most
			|| (u64)trdSlots * TAG_C_WORDS * 4 > most || (u64)sz.headers * 2 * TAG_C_WORDS * 4 > most)
		return no("buffers larger than the driver's storage blocks");

	// the kernels: from SPIR-V; else from the text, kept for the next time
	const auto t0 = std::chrono::steady_clock::now();
	const char *made = "SPIR-V";
	bool kernels = kernels_from_spirv(r->programs);
	if (!kernels)
	{
		delete_programs(r->programs);
		made = "text, or the programs kept";
		kernels = kernels_from_text(r->programs);
	}
	if (!kernels)
		return no("the kernels did not compile");
	const double kernelMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

	/* The contexts' data: read back by the SH4 and by save states, so
	 * memory of the client's that reads at the speed of memory */
	const GLbitfield mapped = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
	void *map = nullptr;
	r->pool = make_buffer((GLsizeiptr)r->slice * r->slices, mapped | GL_CLIENT_STORAGE_BIT, &map);
	r->poolMap = (u8 *)map;
	if (r->poolMap == nullptr)
		return no("the TA contexts' buffer could not be mapped");
	if (!mapping_reads_fast(r->poolMap, 1 << 20))
		return no("its mapped memory is slow to read (write-combined)");
	r->buffers[B_TAB] = make_buffer((GLsizeiptr)r->tableWords * 4 * TA_GL_FRAMES, mapped | GL_CLIENT_STORAGE_BIT, &map);
	r->tables = (u32 *)map;
	if (r->tables == nullptr)
		return no("the tables could not be mapped");
	r->buffers[B_VTX] = make_buffer((GLsizeiptr)sz.verts * TAG_V_WORDS * 4, 0, nullptr);
	r->buffers[B_MOD] = make_buffer((GLsizeiptr)sz.modtrig * 9 * 4 + 4, 0, nullptr);
	r->buffers[B_IDX] = make_buffer((GLsizeiptr)sz.idx * 4, 0, nullptr);
	r->buffers[B_WORK] = make_buffer((GLsizeiptr)workWords * 4, 0, nullptr);
	r->buffers[B_REC] = make_buffer((GLsizeiptr)strips * TAG_C_WORDS * 4, 0, nullptr);
	// (and a second command for each, for shadows: TAG_T_SHD_CMD)
	r->buffers[B_CMD] = make_buffer((GLsizeiptr)sz.headers * 2 * TAG_C_WORDS * 4, 0, nullptr);
	r->buffers[B_FRM] = make_buffer(TAG_O_WORDS * 4, 0, nullptr);
	r->buffers[B_TRD] = make_buffer((GLsizeiptr)trdSlots * TAG_C_WORDS * 4, 0, nullptr);
	r->trParamsBytes = sz.list[2] * TAG_P_WORDS * 4;
	r->buffers[B_TRP] = make_buffer(r->trParamsBytes, 0, nullptr);
	// the per-pixel renderer's strip numbers (TA_GPU_INSTANCE): 0, 1, 2...
	{
		std::vector<u32> numbers(sz.list[2]);
		for (u32 i = 0; i < sz.list[2]; i++)
			numbers[i] = i;
		r->instances = make_buffer((GLsizeiptr)numbers.size() * 4, 0, nullptr, numbers.data());
	}
	/* The sorted indices: three a triangle, for as many triangles as a frame
	 * can have indices, and as many again for the depth prepass, and the
	 * prepass's checks (TAG_T_PREP), which the CPU writes once and the GPU
	 * reads once - but for the prepass's triangles the prepass's kernel
	 * makes nothing of (TAG_K_PREPASS) */
	if (sortWanted)
	{
		const GLintptr slot = ((GLintptr)sz.idx * 2 * 3 * 4 + 255) & ~(GLintptr)255;
		r->sorted = make_buffer(slot * TA_GL_FRAMES, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT, &map);
		r->sortedMap = (u8 *)map;
		r->sortSlot = r->sortedMap != nullptr ? slot : 0;
	}
	glGenQueries(2 * TA_GL_FRAMES, r->queries);
	if (glGetError() != GL_NO_ERROR)
		return no("its buffers could not be made");

	r->layout.words[0] = 0;		// (each slice is bound from its start)
	for (int i = 0; i < 9; i++)
		r->layout.words[1 + i] = work[i];
	r->layout.words[10] = 0;
	r->layout.words[11] = sz.list[0] * TAG_C_WORDS;
	r->layout.words[12] = (sz.list[0] + sz.list[1]) * TAG_C_WORDS;
	r->shadowCommands = (GLintptr)sz.headers * TAG_C_WORDS * 4;

	if (!tactx_gpu_pool_attach(r->poolMap, r->slice, r->slices))
		return no("the TA contexts' pool");
	ta_set_gpu_processing(true);
	if (!ta_gpu_processing())
	{
		tactx_gpu_pool_detach();
		return no("the header pass's memory");
	}
	if (r->sortSlot != 0 && !ta_gpu_sort_reserve(true))
		r->sortSlot = 0;
	draw.vertices = r->buffers[B_VTX];
	draw.indices = r->buffers[B_IDX];
	draw.modtrig = r->buffers[B_MOD];
	draw.trParams = r->buffers[B_TRP];
	draw.frame = r->buffers[B_FRM];
	draw.instances = r->instances;
	draw.sorted = r->sortSlot != 0 ? r->sorted : 0;
	draw.sortedOffset = 0;
	draw.sortedMap = nullptr;
	sortable = r->sortSlot != 0;
	serial = 0;
	done = 0;
	index = 0;
	res = std::move(r);
	ready = true;
	NOTICE_LOG(RENDERER, "OpenGL: TA processing on the GPU (%u frames of %u MB mapped, %u KB of tables a frame; "
			"the kernels from %s in %.1f ms; translucent groups drawn %s%s)",
			res->slices, res->slice >> 20, res->tableWords / 256, made, kernelMs,
			gx.multiDrawElementsIndirectCount != nullptr ? "with their count" : "whole",
			res->sortSlot != 0 ? "; translucent triangles sorted from the frame's depths" : "");
	return true;
}

void GlTaDecoder::Log(const char *when)
{
	u32 shortFrames, allocated;
	tactx_gpu_pool_stats(&shortFrames, &allocated);
	NOTICE_LOG(RENDERER, "OpenGL: TA processing%s: %llu frames decoded and drawn on the GPU, %llu parsed on the CPU "
			"(%llu without a table, %llu with the GPU behind, %llu frame skipping, %llu sorted on the CPU, %llu the header pass leaves to the parse, "
			"%llu might run out of room); GPU time mean %.3f ms, max %.3f ms; pool: %u frames found it full, %u contexts allocated; "
			"%llu with a sorted pass (the sort mean %.1f us, max %.1f us), %llu with shadows the GPU decides",
			when, (unsigned long long)statFrames, (unsigned long long)statSkipped,
			(unsigned long long)statWhy[TA_GPU_WHY_TABLE], (unsigned long long)statBusy, (unsigned long long)statWhy[TA_GPU_WHY_PARSE],
			(unsigned long long)statWhy[TA_GPU_WHY_SORT], (unsigned long long)statWhy[TA_GPU_WHY_WALK],
			(unsigned long long)statWhy[TA_GPU_WHY_ROOM],
			statGpuTimed ? statGpuMs / statGpuTimed : 0.0, statGpuMaxMs, shortFrames, allocated,
			(unsigned long long)statSorted, statSorted ? statSortUs / statSorted : 0.0, statSortMaxUs,
			(unsigned long long)statShadowed);
}

void GlTaDecoder::Term()
{
	if (!ready)
		return;
	Log("");
	/* The frames in the pool go on in memory of their own; the buffers go
	 * now, and OpenGL keeps what the GPU still reads until it is done */
	ta_set_gpu_processing(false);
	tactx_gpu_pool_detach();
	BindIndirect(0, false);
	BindIndirect(0, true);
	res.reset();
	ready = false;
	sortable = false;
	prepared = false;
	recorded = false;
	draw = GlTaDraw();
}

void GlTaDecoder::Setup(bool wanted)
{
	if (!wanted)
	{
		// (turned off: and tried again when it is turned on)
		Term();
		failed = false;
		return;
	}
	if (ready || failed)
		return;
	const auto start = std::chrono::steady_clock::now();
	if (Init())
		NOTICE_LOG(RENDERER, "OpenGL: TA processing set up in %.1f ms",
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
	else
		res.reset();
}

/* Every frame up to the newest whose fence has signalled is done: a fence
 * signals once all that was given before it is */
void GlTaDecoder::PollDone()
{
	Resources& r = *res;
	for (int s = 0; s < TA_GL_FRAMES; s++)
	{
		if (r.fences[s] == nullptr)
			continue;
		const GLenum st = glClientWaitSync(r.fences[s], 0, 0);
		if (st != GL_ALREADY_SIGNALED && st != GL_CONDITION_SATISFIED)
			continue;
		glDeleteSync(r.fences[s]);
		r.fences[s] = nullptr;
		if ((s32)(r.slotSerial[s] - done) > 0)
			done = r.slotSerial[s];
		if (r.timed[s])
		{
			r.timed[s] = false;
			GLint available = 0;
			gx.getQueryObjectiv(r.queries[2 * s + 1], GL_QUERY_RESULT_AVAILABLE, &available);
			if (available)
			{
				GLuint64 t0 = 0, t1 = 0;
				gx.getQueryObjectui64v(r.queries[2 * s], GL_QUERY_RESULT, &t0);
				gx.getQueryObjectui64v(r.queries[2 * s + 1], GL_QUERY_RESULT, &t1);
				const double ms = (double)(t1 - t0) / 1e6;
				statGpuMs += ms;
				statGpuMaxMs = std::max(statGpuMaxMs, ms);
				statGpuTimed++;
			}
		}
	}
	tactx_gpu_done(done);
}

void GlTaDecoder::BeginFrame()
{
	if (!ready)
		return;
	prepared = false;
	recorded = false;
	if (++serial == 0)
		serial = 1;
	PollDone();
	index = (index + 1) % TA_GL_FRAMES;
	// (its fence not yet signalled: its tables and sorted indices may still be read)
	busy = res->fences[index] != nullptr;
	if (sortable)
	{
		draw.sortedOffset = (GLintptr)index * res->sortSlot;
		draw.sortedMap = (u32 *)(res->sortedMap + draw.sortedOffset);
	}
}

bool GlTaDecoder::Prepare(TA_context *ctx, u32 want, bool *rv)
{
	prepared = false;
	if (ctx->pool < 0 || ctx->rend.isRenderFramebuffer || busy)
	{
		// (a frame that found the pool full is in memory the GPU cannot read)
		statSkipped++;
		if (busy && ctx->pool >= 0 && !ctx->rend.isRenderFramebuffer)
			statBusy++;
		else
			statWhy[TA_GPU_WHY_TABLE]++;
		return false;
	}
	flags = want;
	prepared = ta_gpu_prepare(ctx, &res->layout, res->tables + (size_t)index * res->tableWords, res->tableWords, &frame, flags);
	if (!prepared)
	{
		statSkipped++;
		statWhy[frame.why < TA_GPU_WHY_COUNT ? frame.why : TA_GPU_WHY_WALK]++;
		return false;
	}
	*rv = frame.rv;
	if (ta_gpu_sorted_indices() != 0)
	{
		statSorted++;
		const double us = ta_gpu_sorted_ns() / 1e3;
		statSortUs += us;
		statSortMaxUs = std::max(statSortMaxUs, us);
	}
	if (frame.shadows != 0)
		statShadowed++;
	if (!frame.rv)
		prepared = false;
	return true;
}

static void clear_words(GLuint buffer, GLintptr at, GLsizeiptr bytes, u32 value)
{
	glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
	gx.clearBufferSubData(GL_COPY_WRITE_BUFFER, GL_R32UI, at, bytes, GL_RED_INTEGER, GL_UNSIGNED_INT, &value);
}

void GlTaDecoder::Record(TA_context *ctx)
{
	if (!prepared)
		return;
	prepared = false;
	Resources& r = *res;
	if (r.queries[0] != 0)
		gx.queryCounter(r.queries[2 * index], GL_TIMESTAMP);

	/* The sorted translucent indices (TA_GPU_SORT), and the prepass's checks
	 * after them, are the CPU's: written now, before the kernel that reads
	 * them is given (the mapping is coherent) */
	const u32 checks = draw.sortedMap != nullptr && ta_gpu_sorted_indices() != 0 ? ta_gpu_prepass_checks() : 0;
	WriteSorted();

	// what is not written this frame is left from the one before: these are
	const GLuint frm = r.buffers[B_FRM];
	clear_words(frm, 0, TAG_O_WORDS * 4, 0);
	clear_words(frm, TAG_O_ZMAX * 4, 4, 0x3F800000u);	// fZ_max starts at 1
	if (frame.slots != 0)
		clear_words(r.buffers[B_WORK], (GLintptr)r.layout.words[9] * 4, (GLsizeiptr)frame.slots * 4, 0);
	if (frame.trd != 0)
		clear_words(r.buffers[B_TRD], 0, (GLsizeiptr)frame.trd * TAG_C_WORDS * 4, 0);
	glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

	gx.bindBufferRange(GL_SHADER_STORAGE_BUFFER, TAG_GL_BINDING, r.pool, (GLintptr)ctx->pool * r.slice, r.slice);
	gx.bindBufferRange(GL_SHADER_STORAGE_BUFFER, TAG_GL_BINDING + 1 + B_TAB, r.buffers[B_TAB],
			(GLintptr)index * r.tableWords * 4, (GLsizeiptr)r.tableWords * 4);
	for (int b = B_VTX; b < B_COUNT; b++)
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TAG_GL_BINDING + 1 + b, r.buffers[b]);
	// (the prepass's kernel's: after the others)
	if (checks != 0)
		gx.bindBufferRange(GL_SHADER_STORAGE_BUFFER, TAG_GL_BINDING + 1 + B_COUNT, draw.sorted, draw.sortedOffset, (GLsizeiptr)r.sortSlot);
	glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, frm);

	const GLbitfield step = GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT;
	auto dispatch = [&](int k, u32 groups) {
		glcache.UseProgram(r.programs[k]);
		if (groups != 0)
			gx.dispatchCompute(groups, 1, 1);
		glMemoryBarrier(step);
	};
	auto indirect = [&](int k, u32 at) {
		glcache.UseProgram(r.programs[k]);
		gx.dispatchComputeIndirect((GLintptr)at * 4);
		glMemoryBarrier(step);
	};
	const u32 scanGroups = (frame.verts + TAG_SCAN_N - 1) / TAG_SCAN_N;
	dispatch(TAG_K_UNPACK, (frame.verts + frame.modtrig + 63) / 64);
	// (the vertices as unpacked, before TAG_K_PROVOKE moves colours)
	if (checks != 0)
		dispatch(TAG_K_PREPASS, std::min(checks, 65535u));
	dispatch(TAG_K_COUNT, scanGroups);
	dispatch(TAG_K_COUNTSUM, 1);
	dispatch(TAG_K_ENDS, scanGroups);
	indirect(TAG_K_SUMMARY, TAG_O_ARGS64);
	indirect(TAG_K_GROUPS, TAG_O_ARGSCH);
	dispatch(TAG_K_GROUPSUM, 1);
	indirect(TAG_K_STATES, TAG_O_ARGSCH);
	indirect(TAG_K_EMIT, TAG_O_ARGS64);
	dispatch(TAG_K_FINISH, (frame.slots + 63) / 64);
	// (a workgroup for each group named, up to the most a dispatch can have: then each takes several)
	if (frame.shadows != 0)
		dispatch(TAG_K_SHADOW, std::min(frame.shadows, 65535u));
	if (frame.trd != 0)
		indirect(TAG_K_TRDRAW, TAG_O_ARGS64);
	if (screen_height > 480)
		indirect(TAG_K_BLEED, TAG_O_ARGS1);
	if (flags & TA_GPU_PROVOKE)
		indirect(TAG_K_PROVOKE, TAG_O_ARGS1);
	glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0);
	// for whatever draws from them (and the renderers' copy of 1/fZ_max out of the frame's words)
	glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT | GL_COMMAND_BARRIER_BIT
			| GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
	if (r.queries[0] != 0)
	{
		gx.queryCounter(r.queries[2 * index + 1], GL_TIMESTAMP);
		r.timed[index] = true;
	}
	// the context's slice is read until this frame is done
	ctx->gpu_serial = serial;
	recorded = true;
	if (++statFrames % 600 == 0)
		Log(" so far");
}

void GlTaDecoder::EndFrame()
{
	if (!ready)
		return;
	BindIndirect(0, false);
	BindIndirect(0, true);
	if (!recorded)
		return;
	recorded = false;
	Resources& r = *res;
	r.fences[index] = (GLsync)glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	r.slotSerial[index] = serial;
}

void GlTaDecoder::BindIndirect(GLuint buffer, bool parameter)
{
	GLuint& bound = parameter ? boundParameter : boundIndirect;
	if (bound == buffer)
		return;
	bound = buffer;
	glBindBuffer(parameter ? GL_PARAMETER_BUFFER_ARB : GL_DRAW_INDIRECT_BUFFER, buffer);
}

void GlTaDecoder::DrawGroup(u32 listType, u32 first, u32 count, bool shadows)
{
	const Resources& r = *res;
	const GLsizei stride = TAG_C_WORDS * 4;
	if (listType != ListType_Translucent || shadows)
	{
		BindIndirect(r.buffers[B_CMD], false);
		gx.drawElementsIndirect(GL_TRIANGLE_STRIP, GL_UNSIGNED_INT,
				(const void *)((shadows ? r.shadowCommands : 0) + (GLintptr)first * stride));
	}
	else if (gx.multiDrawElementsIndirectCount != nullptr)
	{
		BindIndirect(r.buffers[B_TRD], false);
		BindIndirect(r.buffers[B_TRD], true);
		gx.multiDrawElementsIndirectCount(GL_TRIANGLE_STRIP, GL_UNSIGNED_INT, (const void *)((GLintptr)(first + 1) * stride), (GLintptr)first * stride,
				(GLsizei)count, stride);
	}
	else
	{
		BindIndirect(r.buffers[B_TRD], false);
		gx.multiDrawElementsIndirect(GL_TRIANGLE_STRIP, GL_UNSIGNED_INT, (const void *)((GLintptr)(first + 1) * stride),
				(GLsizei)count, stride);
	}
}

void GlTaDecoder::BindGeometry()
{
	glBindBuffer(GL_ARRAY_BUFFER, draw.vertices);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, draw.indices);
}

void GlTaDecoder::BindModVols()
{
	glBindBuffer(GL_ARRAY_BUFFER, draw.modtrig);
}

void GlTaDecoder::WriteSorted()
{
	if (draw.sortedMap != nullptr && ta_gpu_sorted_indices() != 0)
		ta_gpu_sorted_write(draw.sortedMap);
}

GLintptr GlTaDecoder::BindSorted()
{
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, draw.sorted);
	return draw.sortedOffset;
}
