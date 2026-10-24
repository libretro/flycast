#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
/*
 * abuffer.cpp
 *
 *  Created on: May 26, 2018
 *      Author: raph
 */
#include "gl4.h"

GLuint pixels_buffer;
static GLint64 pixels_buffer_size;	// (bytes)
GLuint pixels_pointers;
GLuint atomic_buffer;
gl4PipelineShader g_abuffer_final_shader;
gl4PipelineShader g_abuffer_clear_shader;
gl4PipelineShader g_abuffer_tr_modvol_shaders[ModeCount];
static GLuint g_quadBuffer = 0;
static GLuint g_quadVertexArray = 0;

#include "rend/oit_size.h"
#include <algorithm>

#ifndef GL_COPY_READ_BUFFER
#define GL_COPY_READ_BUFFER 0x8F36
#endif
#ifndef GL_COPY_WRITE_BUFFER
#define GL_COPY_WRITE_BUFFER 0x8F37
#endif
#ifndef GL_R32UI
#define GL_R32UI 0x8236
#endif
#ifndef GL_RED_INTEGER
#define GL_RED_INTEGER 0x8D94
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

#ifndef APIENTRY
#define APIENTRY
#endif
/* What glsm does not wrap: the entry points the frontend's loader
 * (glsym/glsym_gl.c) has made, which the glsm header does not name */
extern "C" {
extern void (APIENTRY *__rglgen_glClearBufferSubData)(GLenum, GLenum, GLintptr, GLsizeiptr, GLenum, GLenum, const void *);
extern void (APIENTRY *__rglgen_glCopyBufferSubData)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr);
}
#define glClearBufferSubData __rglgen_glClearBufferSubData
#define glCopyBufferSubData __rglgen_glCopyBufferSubData

/* The fragments a frame asks for, counted by the shaders (the counter goes
 * up for a fragment that finds no room too), read back without waiting:
 * after each band the counter is copied, on the GPU, into this frame's
 * slot of a small buffer, and a fence is put after the frame; a frame
 * whose fence has passed, polled before a frame is drawn, has its counts
 * read. Three frames back is the earliest a count is seen. The band
 * being drawn is gl4band (gl4.h). */
gl4Band gl4band;
enum { OIT_COUNT_SLOTS = 4, OIT_MAX_BANDS = 64 };
static GLuint count_buffer;
static void *count_fence[OIT_COUNT_SLOTS];	// GLsync; null when the slot is free
static u32 count_bands[OIT_COUNT_SLOTS];
static bool count_rtt[OIT_COUNT_SLOTS];	// the slot's frame was a render to a texture
static u32 count_head;		// the slot the frame being drawn uses
// how many bands frames are drawn in: the screen's [0], renders to a texture [1]
static u32 oit_bands[2] = { 1, 1 };
static u32 oit_roomy_frames[2];

#define MAX_PIXELS_PER_FRAGMENT "32"

static const char *final_shader_source = SHADER_HEADER
"#define MAX_PIXELS_PER_FRAGMENT " MAX_PIXELS_PER_FRAGMENT
R"(
layout(binding = 0) uniform sampler2D tex;
uniform highp float shade_scale_factor;

out vec4 FragColor;

uint pixel_list[MAX_PIXELS_PER_FRAGMENT];


int fillAndSortFragmentArray(ivec2 coords)
{
	// Load fragments into a local memory array for sorting
	uint idx = imageLoad(abufferPointerImg, coords).x;
	int count = 0;
	for (; idx != EOL && count < MAX_PIXELS_PER_FRAGMENT; count++)
	{
		const Pixel p = pixels[idx];
		// Insertion sort. The entry before the first one is not looked at:
		// this used to read pixel_list[-1], and the pixel buffer at whatever
		// index that held, before finding out there was nothing there.
		int j = count - 1;
		while (j >= 0)
		{
			const Pixel jp = pixels[pixel_list[j]];
			if (!(jp.depth > p.depth
					|| (jp.depth == p.depth && getPolyNumber(jp) > getPolyNumber(p))))
				break;
			pixel_list[j + 1] = pixel_list[j];
			j--;
		}
		pixel_list[j + 1] = idx;
		idx = p.next;
	}
	return count;
}

// Blend fragments back-to-front
vec4 resolveAlphaBlend(ivec2 coords) {
	
	// Copy and sort fragments into a local array
	int num_frag = fillAndSortFragmentArray(coords);
	
	vec4 finalColor = texture(tex, gl_FragCoord.xy / textureSize(tex, 0));
	vec4 secondaryBuffer = vec4(0.0); // Secondary accumulation buffer
	
	for (int i = 0; i < num_frag; i++)
	{
		const Pixel pixel = pixels[pixel_list[i]];
		const PolyParam pp = tr_poly_params[getPolyNumber(pixel)];
		bool area1 = false;
		bool shadowed = false;
		if (isShadowed(pixel))
		{
			if (isTwoVolumes(pp))
				area1 = true;
			else
				shadowed = true;
		}
		vec4 srcColor;
		if (getSrcSelect(pp, area1))
			srcColor = secondaryBuffer;
		else
		{
			srcColor = unpackUnorm4x8(pixel.color);
			if (shadowed)
				srcColor.rgb *= shade_scale_factor;
		}
		vec4 dstColor = getDstSelect(pp, area1) ? secondaryBuffer : finalColor;
		vec4 srcCoef;
		vec4 dstCoef;
		
		int srcBlend = getSrcBlendFunc(pp, area1);
		switch (srcBlend)
		{
			case ZERO:
				srcCoef = vec4(0.0);
				break;
			case ONE:
				srcCoef = vec4(1.0);
				break;
			case OTHER_COLOR:
				// the colour of what it is blended onto - which, when that is the second accumulation buffer, is that buffer's
				srcCoef = dstColor;
				break;
			case INVERSE_OTHER_COLOR:
				srcCoef = vec4(1.0) - dstColor;
				break;
			case SRC_ALPHA:
				srcCoef = vec4(srcColor.a);
				break;
			case INVERSE_SRC_ALPHA:
				srcCoef = vec4(1.0 - srcColor.a);
				break;
			case DST_ALPHA:
				srcCoef = vec4(dstColor.a);
				break;
			case INVERSE_DST_ALPHA:
				srcCoef = vec4(1.0 - dstColor.a);
				break;
		}
		int dstBlend = getDstBlendFunc(pp, area1);
		switch (dstBlend)
		{
			case ZERO:
				dstCoef = vec4(0.0);
				break;
			case ONE:
				dstCoef = vec4(1.0);
				break;
			case OTHER_COLOR:
				dstCoef = srcColor;
				break;
			case INVERSE_OTHER_COLOR:
				dstCoef = vec4(1.0) - srcColor;
				break;
			case SRC_ALPHA:
				dstCoef = vec4(srcColor.a);
				break;
			case INVERSE_SRC_ALPHA:
				dstCoef = vec4(1.0 - srcColor.a);
				break;
			case DST_ALPHA:
				dstCoef = vec4(dstColor.a);
				break;
			case INVERSE_DST_ALPHA:
				dstCoef = vec4(1.0 - dstColor.a);
				break;
		}
		const vec4 result = clamp(dstColor * dstCoef + srcColor * srcCoef, 0.0, 1.0);
		if (getDstSelect(pp, area1))
			secondaryBuffer = result;
		else
			finalColor = result;
	}
	
	return finalColor;
	
}

void main(void)
{
	ivec2 coords = ivec2(gl_FragCoord.xy);
	// Compute and output final color for the frame buffer
	// Visualize the number of layers in use
	//FragColor = vec4(float(fillAndSortFragmentArray(coords)) / MAX_PIXELS_PER_FRAGMENT * 4, 0, 0, 1);
	FragColor = resolveAlphaBlend(coords);
}
)";

static const char *clear_shader_source = SHADER_HEADER
R"(
void main(void)
{
	ivec2 coords = ivec2(gl_FragCoord.xy);

	// Reset pointers
	imageStore(abufferPointerImg, coords, uvec4(EOL));

	// Discard fragment so nothing is written to the framebuffer
	discard;
}
)";

static const char *tr_modvol_shader_source = SHADER_HEADER
"#define MAX_PIXELS_PER_FRAGMENT " MAX_PIXELS_PER_FRAGMENT
R"(
#define MV_MODE %d

// Must match ModifierVolumeMode enum values
#define MV_XOR		 0
#define MV_OR		 1
#define MV_INCLUSION 2
#define MV_EXCLUSION 3

void main(void)
{
	ivec2 coords = ivec2(gl_FragCoord.xy);
	
	uint idx = imageLoad(abufferPointerImg, coords).x;
	int list_len = 0;
	while (idx != EOL && list_len < MAX_PIXELS_PER_FRAGMENT)
	{
		const Pixel pixel = pixels[idx];
		const PolyParam pp = tr_poly_params[getPolyNumber(pixel)];
		if (getShadowEnable(pp))
		{
#if MV_MODE == MV_XOR
			if (FRAG_DEPTH >= pixel.depth)
				atomicXor(pixels[idx].seq_num, SHADOW_STENCIL);
#elif MV_MODE == MV_OR
			if (FRAG_DEPTH >= pixel.depth)
				atomicOr(pixels[idx].seq_num, SHADOW_STENCIL);
#elif MV_MODE == MV_INCLUSION
			uint prev_val = atomicAnd(pixels[idx].seq_num, ~(SHADOW_STENCIL));
			if ((prev_val & (SHADOW_STENCIL|SHADOW_ACC)) == SHADOW_STENCIL)
				pixels[idx].seq_num = bitfieldInsert(pixel.seq_num, 1u, 31, 1);
#elif MV_MODE == MV_EXCLUSION
			uint prev_val = atomicAnd(pixels[idx].seq_num, ~(SHADOW_STENCIL|SHADOW_ACC));
			if ((prev_val & (SHADOW_STENCIL|SHADOW_ACC)) == SHADOW_ACC)
				pixels[idx].seq_num = bitfieldInsert(pixel.seq_num, 1u, 31, 1);
#endif
		}
		idx = pixel.next;
		list_len++;
	}
	
	discard;
}
)";

static const char* VertexShaderSource =
R"(#version 430

in highp vec3 in_pos;

void main()
{
	gl_Position = vec4(in_pos, 1.0);
}
)";

void initABuffer()
{
	if (max_image_width > 0 && max_image_height > 0)
	{
		if (pixels_pointers == 0)
			pixels_pointers = glcache.GenTexture();
		glActiveTexture(GL_TEXTURE4);
		glBindTexture(GL_TEXTURE_2D, pixels_pointers);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, max_image_width, max_image_height, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, 0);
		glBindImageTexture(4, pixels_pointers, 0, false, 0,  GL_READ_WRITE, GL_R32UI);
		glCheck();
	}

	/* The fragments, for the screen's size (rend/oit_size.h): made again
	 * when that changes it, which a render to a texture does not */
	GLint64 most;
	glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &most);
	const GLint64 size = (GLint64)oit_buffer_bytes(screen_width, screen_height, gl.vram_budget.available_kib(), (u64)std::max(most, (GLint64)0));
	if (pixels_buffer != 0 && size != pixels_buffer_size)
	{
		glDeleteBuffers(1, &pixels_buffer);
		pixels_buffer = 0;
	}
	if (pixels_buffer == 0 )
	{
		pixels_buffer_size = size;
		// Create the buffer
		glGenBuffers(1, &pixels_buffer);
		// Bind it
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, pixels_buffer);
		// Declare storage
		glBufferData(GL_SHADER_STORAGE_BUFFER, size, NULL, GL_DYNAMIC_COPY);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, pixels_buffer);
		glCheck();
	}

	if (atomic_buffer == 0 )
	{
		// Create the buffer
		glGenBuffers(1, &atomic_buffer);
		// Bind it
		glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, atomic_buffer);
		// Declare storage
		glBufferData(GL_ATOMIC_COUNTER_BUFFER, 4, NULL, GL_DYNAMIC_COPY);
		glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 0, atomic_buffer);
		GLint zero = 0;
		glBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, sizeof(GLint), &zero);
		glCheck();
	}
	if (count_buffer == 0)
	{
		glGenBuffers(1, &count_buffer);
		glBindBuffer(GL_COPY_WRITE_BUFFER, count_buffer);
		glBufferData(GL_COPY_WRITE_BUFFER, OIT_COUNT_SLOTS * OIT_MAX_BANDS * sizeof(GLuint), NULL, GL_DYNAMIC_READ);
		glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
		glCheck();
	}

	if (g_abuffer_final_shader.program == 0)
		gl4CompilePipelineShader(&g_abuffer_final_shader, final_shader_source, VertexShaderSource);
	if (g_abuffer_clear_shader.program == 0)
		gl4CompilePipelineShader(&g_abuffer_clear_shader, clear_shader_source, VertexShaderSource);
	if (g_abuffer_tr_modvol_shaders[0].program == 0)
	{
		char source[16384];
		for (int mode = 0; mode < ModeCount; mode++)
		{
			sprintf(source, tr_modvol_shader_source, mode);
			gl4CompilePipelineShader(&g_abuffer_tr_modvol_shaders[mode], source, VertexShaderSource);
		}
	}

	if (g_quadVertexArray == 0)
		glGenVertexArrays(1, &g_quadVertexArray);
	if (g_quadBuffer == 0)
	{
		glBindVertexArray(g_quadVertexArray);
		glGenBuffers(1, &g_quadBuffer);
		glBindBuffer(GL_ARRAY_BUFFER, g_quadBuffer); glCheck();

		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, (void*)0);

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); glCheck();
		glBindVertexArray(0);
	}
	glCheck();

	// Clear A-buffer pointers
	glcache.UseProgram(g_abuffer_clear_shader.program);
	gl4ShaderUniforms.Set(&g_abuffer_clear_shader);

	abufferDrawQuad();
	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	glCheck();
}

void termABuffer()
{
   if (pixels_pointers != 0)
   {
	  glcache.DeleteTextures(1, &pixels_pointers);
	  pixels_pointers = 0;
   }
   if (pixels_buffer != 0)
   {
	  glDeleteBuffers(1, &pixels_buffer);
	  pixels_buffer = 0;
   }
   if (atomic_buffer != 0)
   {
	  glDeleteBuffers(1, &atomic_buffer);
	  atomic_buffer = 0;
   }
   if (count_buffer != 0)
   {
	  glDeleteBuffers(1, &count_buffer);
	  count_buffer = 0;
   }
   for (u32 i = 0; i < OIT_COUNT_SLOTS; i++)
	  if (count_fence[i] != nullptr)
	  {
		 glDeleteSync((GLsync)count_fence[i]);
		 count_fence[i] = nullptr;
	  }
   count_head = 0;
   oit_bands[0] = oit_bands[1] = 1;
   oit_roomy_frames[0] = oit_roomy_frames[1] = 0;
   gl4band.height = 0;
   if (g_quadVertexArray != 0)
   {
	  glDeleteVertexArrays(1, &g_quadVertexArray);
	  g_quadVertexArray = 0;
   }
   if (g_quadBuffer != 0)
   {
	  glDeleteBuffers(1, &g_quadBuffer);
	  g_quadBuffer = 0;
   }
	glDeleteProgram(g_abuffer_final_shader.program);
	g_abuffer_final_shader.program = 0;
	glDeleteProgram(g_abuffer_clear_shader.program);
	g_abuffer_clear_shader.program = 0;
	for (int mode = 0; mode < ModeCount; mode++)
	{
		glDeleteProgram(g_abuffer_tr_modvol_shaders[mode].program);
		g_abuffer_tr_modvol_shaders[mode].program = 0;
	}
}

void reshapeABuffer(int w, int h)
{
	if (pixels_pointers != 0)
	{
		glcache.DeleteTextures(1, &pixels_pointers);
		pixels_pointers = 0;
	}

	initABuffer();
}

void abufferDrawQuad()
{
	glBindVertexArray(g_quadVertexArray);

	float vertices[] = {
			-1,  1, 1,
			-1, -1, 1,
			 1,  1, 1,
			 1, -1, 1,
	};
	GLushort indices[] = { 0, 1, 2, 1, 3 };

	glBindBuffer(GL_ARRAY_BUFFER, g_quadBuffer); glCheck();
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW); glCheck();

	glDrawElements(GL_TRIANGLE_STRIP, 5, GL_UNSIGNED_SHORT, indices); glCheck();
	glBindVertexArray(0);
	glCheck();
}

/* The translucent modifier volumes' draws change the lists from their
 * shaders (tr_modvol_shader_source), and nothing orders what the shaders of
 * two draws do to the same fragment but a memory barrier between them: a
 * GPU runs draws at once, in any order, where a software device runs them
 * one after the other. They go through here. Built with RACE_TEST
 * (tools/rend/speckle_test.sh; never in a release) the draws between two
 * barriers are drawn a triangle at a time, the draws' triangles taken in
 * turn, the last draw's first: one draw's shaders run while another's are
 * half done, as on a GPU, and a picture that changes then depends on an
 * order nothing keeps. */
namespace {
struct ListDraws
{
#ifdef RACE_TEST
	struct Held { gl4PipelineShader *shader; u32 cull; int first, count; };
	Held held[256];
	u32 heldCount = 0;
#endif

	~ListDraws() { Flush(); }

	void Draw(gl4PipelineShader *shader, u32 cull, int first, int count)
	{
#ifdef RACE_TEST
		if (heldCount == ARRAY_SIZE(held))
			Flush();
		held[heldCount++] = { shader, cull, first, count };
#else
		Record(shader, cull, first, count);
#endif
	}
	/* what the draws before did to the lists is done before the draws after start */
	void Barrier()
	{
		Flush();
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
	}
	void Flush()
	{
#ifdef RACE_TEST
		for (int v = 0, more = (int)heldCount; more != 0; v += 3)
		{
			more = 0;
			for (u32 i = heldCount; i-- > 0; )
				if (v < held[i].count)
				{
					Record(held[i].shader, held[i].cull, held[i].first + v, 3);
					more++;
				}
		}
		heldCount = 0;
#endif
	}

private:
	static void Record(gl4PipelineShader *shader, u32 cull, int first, int count)
	{
		glcache.UseProgram(shader->program);
		gl4ShaderUniforms.Set(shader);
		SetCull(cull);
		glDrawArrays(GL_TRIANGLES, first, count); glCheck();
	}
};
}

void DrawTranslucentModVols(int first, int count, bool useOpaqueGeom)
{
	// (of a frame decoded on the GPU, its triangles are the GPU's)
	const u32 triangles = pvrrc.gpu ? pvrrc.gpu_modtrig : (u32)pvrrc.modtrig.used();
	if (count == 0 || triangles == 0)
		return;
	glBindVertexArray(gl4.vbo.modvol_vao);

	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);

	glcache.Disable(GL_DEPTH_TEST);
	glcache.Disable(GL_STENCIL_TEST);

	glCheck();

	ModifierVolumeParam* params = useOpaqueGeom ? &pvrrc.global_param_mvo.head()[first] : &pvrrc.global_param_mvo_tr.head()[first];

	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

	int mod_base = -1;
	ListDraws listDraws;

	for (int cmv = 0; cmv < count; cmv++)
	{
		ModifierVolumeParam& param = params[cmv];

		if (param.count == 0)
			continue;

		u32 mv_mode = param.isp.DepthMode;

		verify(param.first >= 0 && param.first + param.count <= triangles);

		if (mod_base == -1)
			mod_base = param.first;

		gl4PipelineShader *shader;
		if (!param.isp.VolumeLast && mv_mode > 0)
			shader = &g_abuffer_tr_modvol_shaders[Or];	// OR'ing (open volume or quad)
		else
			shader = &g_abuffer_tr_modvol_shaders[Xor];	// XOR'ing (closed volume)
		glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
		listDraws.Draw(shader, param.isp.CullMode, param.first * 3, param.count * 3);

		if (mv_mode == 1 || mv_mode == 2)
		{
			//Sum the area
			shader = &g_abuffer_tr_modvol_shaders[mv_mode == 1 ? Inclusion : Exclusion];
			glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
			listDraws.Draw(shader, param.isp.CullMode, mod_base * 3, (param.first + param.count - mod_base) * 3);
			mod_base = -1;
		}
	}
	listDraws.Flush();
	glBindVertexArray(gl4.vbo.main_vao);
}

/* The bands this frame is drawn in, from the counts of the oldest frame
 * whose fence has passed: never this frame's, which nothing waits for.
 * More at once when that one had fragments that found no room; fewer when
 * a second of frames in a row would have fitted in fewer. Each band is
 * taken to need what the fullest one did, with a quarter more room. The
 * screen's frames and renders to a texture are counted apart. */
u32 gl4OitChooseBands(bool rtt)
{
	const u32 slot = (count_head + 1) % OIT_COUNT_SLOTS;	// the oldest in flight, when all are
	for (u32 k = 0; k < OIT_COUNT_SLOTS; k++)
	{
		const u32 i = (slot + k) % OIT_COUNT_SLOTS;
		if (count_fence[i] == nullptr)
			continue;
		const GLenum r = glClientWaitSync((GLsync)count_fence[i], 0, 0);
		if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED)
			break;
		glDeleteSync((GLsync)count_fence[i]);
		count_fence[i] = nullptr;

		GLuint counts[OIT_MAX_BANDS];
		const u32 drawnIn = std::max<u32>(std::min<u32>(count_bands[i], OIT_MAX_BANDS), 1);
		glBindBuffer(GL_COPY_READ_BUFFER, count_buffer);
		glGetBufferSubData(GL_COPY_READ_BUFFER, (GLintptr)(i * OIT_MAX_BANDS * sizeof(GLuint)), drawnIn * sizeof(GLuint), counts);
		glBindBuffer(GL_COPY_READ_BUFFER, 0);
		u32 fragments = 0;
		for (u32 b = 0; b < drawnIn; b++)
			fragments = std::max<u32>(fragments, counts[b]);

		const u64 room = std::max<u64>((u64)pixels_buffer_size / OIT_FRAGMENT_BYTES, 1);
		const u64 frame = (u64)fragments * drawnIn;
		const u32 wanted = (u32)std::max<u64>(std::min<u64>((frame + frame / 4 + room - 1) / room, OIT_MAX_BANDS), 1);
		u32& bands = oit_bands[count_rtt[i] ? 1 : 0];
		u32& roomy = oit_roomy_frames[count_rtt[i] ? 1 : 0];
		const u32 was = bands;
		if (fragments > room)
		{
			bands = std::min<u32>(std::max({ bands, wanted, drawnIn + 1 }), OIT_MAX_BANDS);
			roomy = 0;
		}
		else if (wanted < bands)
		{
			if (++roomy >= 60)
			{
				bands = wanted;
				roomy = 0;
			}
		}
		else
			roomy = 0;
		if (bands != was)
			NOTICE_LOG(RENDERER, "OpenGL per-pixel renderer: %u translucent fragments in a band of %u%s, room for %u: %u band%s",
					fragments, drawnIn, count_rtt[i] ? " (a render to a texture)" : "", (u32)room, bands, bands > 1 ? "s" : "");
		break;
	}
	return oit_bands[rtt ? 1 : 0];
}

void gl4OitBandBegin()
{
	/* The band before (or the frame before) may still be being put on the
	 * picture: its lists read from the first entries, which this band's
	 * polygons write again, and its pointers set back to nothing, which
	 * this band's polygons link from. Nothing of this band's starts before
	 * that is done. */
	glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_ATOMIC_COUNTER_BARRIER_BIT);
	// the counter back at nothing, on the GPU, after what the band before drew
	const GLuint zero = 0;
	glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, atomic_buffer);
	glClearBufferSubData(GL_ATOMIC_COUNTER_BUFFER, GL_R32UI, 0, sizeof(GLuint), GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
}

void gl4OitBandEnd(u32 band)
{
	if (band >= OIT_MAX_BANDS)
		return;
	glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
	glBindBuffer(GL_COPY_WRITE_BUFFER, count_buffer);
	glCopyBufferSubData(GL_ATOMIC_COUNTER_BUFFER, GL_COPY_WRITE_BUFFER, 0,
			(GLintptr)((count_head * OIT_MAX_BANDS + band) * sizeof(GLuint)), sizeof(GLuint));
	glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
}

void gl4OitFrameEnd(u32 bands, bool rtt)
{
	if (count_fence[count_head] != nullptr)
		// (a slot still in flight is used again: its count is lost, not waited for)
		glDeleteSync((GLsync)count_fence[count_head]);
	count_fence[count_head] = (void *)glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	count_bands[count_head] = bands;
	count_rtt[count_head] = rtt;
	count_head = (count_head + 1) % OIT_COUNT_SLOTS;
}

void renderABuffer()
{
	// Render to output FBO
	glcache.UseProgram(g_abuffer_final_shader.program);
	gl4ShaderUniforms.Set(&g_abuffer_final_shader);

	glcache.Disable(GL_DEPTH_TEST);
	glcache.Disable(GL_CULL_FACE);
	// (the band's pixels alone: the others' lists are empty, and their picture is done)
	gl4ScissorOff();
	// what the last polygon blended with is no business of the lists being put on the picture
	glcache.Disable(GL_BLEND);
	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

	abufferDrawQuad();

	glCheck();

	// Clear A-buffer pointers
	glcache.UseProgram(g_abuffer_clear_shader.program);
	gl4ShaderUniforms.Set(&g_abuffer_clear_shader);

	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	abufferDrawQuad();

	glActiveTexture(GL_TEXTURE0);

	glCheck();
}
