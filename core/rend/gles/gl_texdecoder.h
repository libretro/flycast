/* Textures made on the GPU from their bytes in video memory, for the
 * OpenGL renderers: what vulkan/tex_decoder.h and dx11/dx11_texdecoder.h
 * are for the Vulkan and Direct3D 11 ones.
 *
 * The texture's bytes - the texels, the codebook of a compressed texture,
 * the part of the palette a paletted one uses - are copied as they are
 * into a ring, and a kernel of rend/texdecode.h makes the texels from them
 * into the texture of the texture pool's it was given (gles.h,
 * GLTexturePool): the same text the CPU's converters are tested against.
 *  - OpenGL 4.3 and later: the ring is a buffer the kernels read as a
 *    shader storage buffer. 32-bit texels and palette indices are written
 *    by compute kernels through rgba8ui and r8ui images of the level;
 *    16-bit ones are drawn into it through a framebuffer, by a fragment
 *    shader, as the 16-bit formats cannot be images.
 *  - OpenGL 3.x: no storage buffers and no compute: the ring goes to an
 *    unsigned integer texture once a frame, and every level is drawn.
 *  - A planar 16-bit texture needs no kernel: its rows go from the ring to
 *    the texture as they are, as 5-6-5, 1-5-5-5 reversed (BGRA) and
 *    4-4-4-4 reversed (BGRA), which are the PowerVR's bits in its order.
 * The ring is one buffer made when the renderer starts, a share of the
 * video memory. With OpenGL 4.4 (or ARB_buffer_storage) it stays mapped,
 * written in place, and its pieces are taken back when the fence of the
 * frame that used them, polled, says that frame is done - nothing waits
 * for one: a texture there is no room for is converted on the CPU, as
 * before. Without, the frame's bytes are written to memory of this side's
 * and go to the buffer (or the texture) in one call when it is flushed.
 *
 * What a frame asks for is recorded at the end of its processing
 * (gl_tex_flush(), from ProcessFrame()), before its draws. The kernels are
 * made when the renderer starts, and never while a game runs: from their
 * SPIR-V with OpenGL 4.6 (ARB_gl_spirv), or compiled from their text, as
 * program binaries kept in the frontend's system directory the next time,
 * where the driver can (glGetProgramBinary), by its name and the text's.
 * OpenGL ES (the GLES builds) and OpenGL 2 convert on the CPU, as before. */
#pragma once
#include "types.h"
#include "rend/TexCache.h"
#include "rend/texdecode.h"
#include <glsm/glsmsym.h>

class GLTexDecoder
{
public:
	/* With the context current, the renderer's resources made */
	void Init();
	void Term();
	bool Ready() const { return ready; }

	/* Whether a texture of @type, made into TEXD_OUT8, 16 or 32 (@out),
	 * can be made by the kernels */
	bool Takes(TextureType type, u32 out) const;
	/* Whether a planar 16-bit texture of @type goes to its texture as it
	 * is (through the ring) */
	bool Native(TextureType type) const { return ready && (unsigned)type < 3 && native[(int)type]; }

	/* @bytes of the ring for a texture of @levels levels to make (0 for a
	 * copy), and where they are (a multiple of 16); null if there is no
	 * room. What was asked for before goes to the GPU first if the lists
	 * have no room for this. */
	u8 *Begin(u32 bytes, u32 levels, u32 *offset);
	/* The plan's levels made into @texture (of @type) from the bytes at
	 * @offset; what Cancel() takes */
	u32 AddDecode(GLuint texture, TextureType type, const texd_plan& plan, u32 offset);
	/* @width by @height texels into level 0 of @texture, rows of @rowLength
	 * from @offset */
	u32 AddCopy(GLuint texture, TextureType type, u32 offset, u32 rowLength, u32 width, u32 height);
	u32 Serial() const { return serial; }
	/* What was added under @serial as @job is not done after all */
	void Cancel(u32 job, u32 serial);

	/* What the frame asked for, recorded */
	void Flush();

	// what has been done so far, for measuring
	u64 statBytes = 0;
	u64 statLevels = 0;
	u64 statCopies = 0;
	u64 statFull = 0;

private:
	enum { MAX_JOBS = 2048, MAX_LEVELS = 4096, MAX_FENCES = 8 };
	enum { PROGRAM_32, PROGRAM_8, PROGRAM_DRAW, PROGRAMS };
	struct Job
	{
		GLuint texture;
		u32 first, count;	// levels; a copy has none
		u32 offset, rowLength, width, height;	// a copy's
		u8 type;
		bool copy;
		bool cancelled;
	};
	struct Level
	{
		u32 pc[6];
		u32 mip;
	};
	struct Pending
	{
		void *fence;		// GLsync
		u64 end;
	};

	bool MakePrograms();
	GLuint Link(const u32 *spv, const size_t *spvSize, const char *const *sources, int stages, bool keep);
	void Upload(u64 from, u64 to);
	void Reclaim();

	bool ready = false;
	bool compute = false;		// OpenGL 4.3: compute kernels, the ring a storage buffer
	bool persistent = false;	// the ring stays mapped
	bool fences = false;
	bool takes[5] = {};
	bool native[3] = {};

	GLuint ring = 0;			// the buffer
	GLuint ringTexture = 0;		// OpenGL 3: what the kernels read the ring from
	u32 ringRows = 0;
	u8 *mapped = nullptr;		// the ring, mapped; or this side's memory for it
	u8 *shadow = nullptr;
	u64 size = 0;
	u64 head = 0, tail = 0, flushed = 0;	// bytes handed out, given back, and up to where they went
	Pending pending[MAX_FENCES];
	u32 pendingCount = 0;

	GLuint programs[PROGRAMS] = {};
	GLint locations[PROGRAMS][2] = {};
	GLuint framebuffer = 0;
	GLuint vertexArray = 0;

	Job jobs[MAX_JOBS];
	Level levels[MAX_LEVELS];
	u32 jobCount = 0, levelCount = 0;
	u32 serial = 1;
};

extern GLTexDecoder glTexDecoder;
