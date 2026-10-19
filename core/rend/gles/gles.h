#pragma once
#include <unordered_map>
#include <libretro.h>
#include <glsm/glsm.h>
#include <glsm/glsmsym.h>
#include "rend/rend.h"
#include "rend/TexCache.h"
#include "rend/transform_matrix.h"
#include "rend/sorter.h"
#include "rend/tileclip.h"
#include "rend/tex_pool.h"
#include "postprocess.h"
#include "glcache.h"
#include "rend/depth_scale.h"
#include "rend/ta_gpu.h"

#ifndef GL_DEPTH32F_STENCIL8
#define GL_DEPTH32F_STENCIL8 0x8CAD
#endif
#ifndef GL_LOWER_LEFT
#define GL_LOWER_LEFT 0x8CA1
#endif
#ifndef GL_ZERO_TO_ONE
#define GL_ZERO_TO_ONE 0x935F
#endif
#ifndef GL_NEGATIVE_ONE_TO_ONE
#define GL_NEGATIVE_ONE_TO_ONE 0x935E
#endif
#ifndef GL_UNIFORM_BUFFER
#define GL_UNIFORM_BUFFER 0x8A11
#endif
#ifndef GL_COPY_READ_BUFFER
#define GL_COPY_READ_BUFFER 0x8F36
#endif
// the uniform buffer binding of the depth scale (DepthScaleBlock in the vertex shaders)
#define DEPTH_UBO_BINDING 0
// (the OpenGL ES headers spell the calling convention this way)
#ifndef APIENTRYP
#ifdef GL_APIENTRYP
#define APIENTRYP GL_APIENTRYP
#else
#define APIENTRYP *
#endif
#endif

#ifndef TEXTURE_MAX_ANISOTROPY_EXT
#define TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
#ifndef GL_TEXTURE_BASE_LEVEL
#define GL_TEXTURE_BASE_LEVEL                   0x813C
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL                    0x813D
#endif


#define VERTEX_POS_ARRAY      0
#define VERTEX_COL_BASE_ARRAY 1
#define VERTEX_COL_OFFS_ARRAY 2
#define VERTEX_UV_ARRAY       3
// OIT only
#define VERTEX_COL_BASE1_ARRAY 4
#define VERTEX_COL_OFFS1_ARRAY 5
#define VERTEX_UV1_ARRAY 6
// (OIT only) a translucent strip's number: GPU-side TA processing's draws give it
#define VERTEX_PP_ARRAY 7

#define glCheck()

//vertex types
extern u32 gcflip;
extern xform ViewportMatrix;

void DrawStrips(void);

struct PipelineShader
{
	GLuint program;
	GLint depth_scale;
	GLint pp_ClipTest;
	GLint cp_AlphaTestValue;
	GLint sp_FOG_COL_RAM;
	GLint sp_FOG_COL_VERT;
	GLint sp_FOG_DENSITY;
	GLint trilinear_alpha;
	GLint fog_clamp_min, fog_clamp_max;
	GLint normal_matrix;
	GLint palette_index;
	GLint shade_scale_factor;
	GLint prepass_alpha;	// -1 where there is no depth prepass (GLES2): rend/sorter.h

	//
	bool cp_AlphaTest;
	bool pp_InsideClipping;
	bool pp_Texture;
	bool pp_UseAlpha;
	bool pp_IgnoreTexA;
	u32 pp_ShadInstr;
	bool pp_Offset;
	u32 pp_FogCtrl;
	bool pp_Gouraud;
	bool pp_BumpMap;
	bool fog_clamping;
	bool trilinear;
	bool palette;
	bool shadowed;

	/* (glprogram_cache.h) The polygon's key (GetProgram()); for one drawn
	 * with a general program, that program's record, whose program and
	 * uniforms these are, and the program of its own being made in the
	 * background (0: none); the frame it was last polled in */
	u32 key;
	PipelineShader *general;
	GLuint pending;
	u32 polled;
	// (a general program's record) the uniform it reads the key from, and the key it has
	GLint key_bits;
	u32 key_now;
	// the frame the frame's uniforms were last given to the program in (ShaderUniforms.Set())
	u32 uniforms_set;
};

/* A polygon's program bound (from GetProgram()), with this frame's uniforms
 * and, for a general one, the polygon's key */
void UsePipelineShader(PipelineShader *s);

#include "rend/vram_budget.h"

struct gl_ctx
{
	struct
	{
		GLuint program;

		GLint depth_scale;
		GLint sp_ShaderColor;
		GLint normal_matrix;

	} modvol_shader;

	std::unordered_map<u32, PipelineShader> shaders;
	/* The general programs (glprogram_cache.h), by pp_Gouraud: no program
	 * where there are none (OpenGL ES 2, OpenGL 2) */
	PipelineShader general[2];
	// the frame, for the uniforms given once a frame (UsePipelineShader())
	u32 uniforms_frame;

	struct
	{
		GLuint geometry,modvols,idxs,idxs2;
	} vbo;

	struct
	{
		u32 TexAddr;
		GLuint depthb;
		GLuint tex;
		GLuint fbo;
		struct GLPooledTexture *pooled;	// what tex is of the pool's (rend/tex_pool.h)
		/* the framebuffer and the depth renderbuffer kept from one render
		 * to the next (with the pool), and how large that is */
		bool kept;
		u32 depthWidth, depthHeight;
	} rtt;

	const char *gl_version;
	const char *glsl_version_header;
	int gl_major;
	int gl_minor;
	bool is_gles;
	/* The depth (rend/depth_scale.h; findGLVersion()): the format of the
	 * depth buffers this renderer makes, the uniform buffer the depth scale
	 * is in (0: a plain uniform), and glClipControl where the context has
	 * it (null where not) */
	GLenum depth_format;
	GLuint depth_ubo;
	void (APIENTRYP clipControl)(GLenum origin, GLenum depth);
	void (APIENTRYP copyBufferSubData)(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size);
	GLuint single_channel_format;
	GLenum index_type;
	bool stencil_present;
	f32 max_anisotropy;
	VramBudget vram_budget;	// what video memory there was to use, at findGLVersion()

	size_t get_index_size() { return index_type == GL_UNSIGNED_INT ? sizeof(u32) : sizeof(u16); }
};

extern gl_ctx gl;
extern GLuint fbTextureId;
extern float fb_scale_x, fb_scale_y;

u64 gl_GetTexture(TSP tsp, TCW tcw, int area = 0);
struct text_info {
	u16* pdata;
	u32 width;
	u32 height;
	u32 textype; // 0 565, 1 1555, 2 4444
};

enum ModifierVolumeMode { Xor, Or, Inclusion, Exclusion, ModeCount };

/* @taFlags: how the renderer draws a frame GPU-side TA processing decodes
 * (ta_gpu_prepare()'s TA_GPU_*; gles/ta_decoder_gl.h); 0, never */
bool ProcessFrame(TA_context* ctx, u32 taFlags = 0);
void UpdateFogTexture(u8 *fog_table, GLenum texture_slot, GLint fog_image_format);
void GetFramebufferScaling(float& scale_x, float& scale_y, float& scissoring_scale_x, float& scissoring_scale_y);
void GetFramebufferSize(float& dc_width, float& dc_height);

void UpdatePaletteTexture(GLenum texture_slot);
void DoCleanup();
void SortPParams(int first, int count);
void SetCull(u32 CullMode);
s32 SetTileClip(u32 val, GLint uniform);
void SetMVS_Mode(ModifierVolumeMode mv_mode, ISP_Modvol ispc);

void BindRTT(u32 addy, u32 fbw, u32 fbh, u32 channels, u32 fmt);
void ReadRTTBuffer();
void gl_keep_picture(void);
void gl_restore_picture(void);
void gl_carry_picture(int from_w, int from_h, int to_w, int to_h);
void RenderFramebuffer();
void DrawFramebuffer();

PipelineShader *GetProgram(bool cp_AlphaTest, bool pp_InsideClipping,
		bool pp_Texture, bool pp_UseAlpha, bool pp_IgnoreTexA, u32 pp_ShadInstr, bool pp_Offset,
		u32 pp_FogCtrl, bool pp_Gouraud, bool pp_BumpMap, bool fog_clamping, bool trilinear,
		bool palette, bool shadowed = false);
void vertex_buffer_unmap(void);

void findGLVersion();
GLuint gl_CompileShader(const char* shader, GLuint type);
GLuint gl_CompileAndLink(const char* VertexShader, const char* FragmentShader);
void gl_bind_depth_ubo(GLuint program);
bool CompilePipelineShader(PipelineShader* s);
void vertex_buffer_unmap();

extern GLuint vmuTextureId[4];
extern GLuint lightgunTextureId[4];
void UpdateVmuTexture(int vmu_screen_number);

extern struct ShaderUniforms_t
{
	float PT_ALPHA;
	float depth_coefs[4];
	float fog_den_float;
	float ps_FOG_COL_RAM[3];
	float ps_FOG_COL_VERT[3];
	float trilinear_alpha;
	float fog_clamp_min[4];
	float fog_clamp_max[4];
	float normal_mat[16];
	struct {
		bool enabled;
		int x;
		int y;
		int width;
		int height;
	} base_clipping;
	int palette_index;
	float shade_scale_factor;

	void Set(const PipelineShader* s)
	{
		if (s->shade_scale_factor != -1)
			glUniform1f(s->shade_scale_factor, shade_scale_factor);

		if (s->cp_AlphaTestValue!=-1)
			glUniform1f(s->cp_AlphaTestValue,PT_ALPHA);

		if (s->depth_scale!=-1)
			glUniform4fv( s->depth_scale, 1, depth_coefs);

		if (s->sp_FOG_DENSITY!=-1)
			glUniform1f(s->sp_FOG_DENSITY, fog_den_float);

		if (s->sp_FOG_COL_RAM!=-1)
			glUniform3fv( s->sp_FOG_COL_RAM, 1, ps_FOG_COL_RAM);

		if (s->sp_FOG_COL_VERT!=-1)
			glUniform3fv( s->sp_FOG_COL_VERT, 1, ps_FOG_COL_VERT);

		if (s->fog_clamp_min != -1)
			glUniform4fv(s->fog_clamp_min, 1, fog_clamp_min);
		if (s->fog_clamp_max != -1)
			glUniform4fv(s->fog_clamp_max, 1, fog_clamp_max);

		if (s->normal_matrix != -1)
			glUniformMatrix4fv(s->normal_matrix, 1, GL_FALSE, normal_mat);

		if (s->palette_index != -1)
			glUniform1i(s->palette_index, palette_index);
	}

} ShaderUniforms;

/* A texture of the texture pool's (rend/tex_pool.h) */
struct GLPooledTexture : TexPoolEntry
{
	GLuint name = 0;
	u32 levels = 1;
	GLPooledTexture *all = nullptr;		// every one there is, for Term()
};

/* The cache's textures on desktop OpenGL 3 and later (not OpenGL ES, nor
 * OpenGL 2, which make theirs as before): made when the renderer starts,
 * with their storage (glTexStorage2D() where there is one), and handed out
 * to the textures that need one of their shape. The formats are the texture
 * types, by their numbers, and the pictures rendered to: RGBA8 and RGB8.
 * A texture given back can be handed out again at once: OpenGL sees to it
 * that what the GPU still has to do with it is done first. */
class GLTexturePool
{
public:
	void Init();
	void Term();
	bool Ready() const { return ready; }
	/* A texture of @type, @width by @height, with its levels or not: a
	 * free one of that shape, or one made now if there is none. Null if the
	 * shape is not one of the pool's. */
	GLPooledTexture *Take(TextureType type, u32 width, u32 height, bool mips);
	/* A picture to render to, RGB8 or RGBA8 */
	GLPooledTexture *TakeTarget(bool rgb, u32 width, u32 height);
	void Give(GLPooledTexture *texture) { book.Give(texture); }
	/* The pool's texture of that name, or null */
	GLPooledTexture *Find(GLuint name) const;
	const TexPoolBook& Book() const { return book; }
	enum { FORMATS = 7, TARGET_RGBA = 5, TARGET_RGB = 6 };

private:
	GLPooledTexture *Make(int cls, bool late);
	GLPooledTexture *TakeClass(int cls);

	TexPoolBook book;
	bool ready = false;
	bool storage = false;		// glTexStorage2D()
	GLPooledTexture *all = nullptr;
};
extern GLTexturePool glTexPool;

/* The textures' pool and the decoder (gl_texdecoder.h): made when a renderer
 * starts (@depth: with the depth buffer renders to textures are made with),
 * and what a frame's textures asked of the decoder, recorded before its
 * draws */
void gl_tex_init(bool depth);
void gl_tex_term();
void gl_tex_flush();

class TextureCacheData : public BaseTextureCacheData
{
public:
	GLuint texID;   //gl texture: its own, or the pool's
	virtual std::string GetId() override { return std::to_string(texID); }
	virtual void UploadToGPU(int width, int height, u8 *temp_tex_buffer, bool mipmapped, bool mipmapsIncluded = false) override;
	virtual bool GpuDecode(const texd_plan& plan) override;
	virtual bool Delete() override;
	/* The picture rendered to is the texture now: @tex, the pool's @pooled
	 * or one of its own */
	void SetRendered(GLuint tex, GLPooledTexture *pooled);
	/* A texture of its own to be made anew: a custom texture's */
	void Renew();

private:
	/* The pool's texture for this shape, the one it has if it is that;
	 * false if the pool has none of this shape */
	bool TakePooled(u32 width, u32 height, bool mips);
	void ReleasePooled();
	void CancelDecode();

	GLPooledTexture *pooled = nullptr;
	GLuint ownID = 0;		// its own texture, kept while it has the pool's
	u32 decodeJob = 0, decodeSerial = 0;
};

class GlTextureCache : public BaseTextureCache<TextureCacheData>
{
};
extern GlTextureCache TexCache;

extern const u32 Zfunction[8];
extern const u32 SrcBlendGL[], DstBlendGL[];

extern "C" struct retro_hw_render_callback hw_render;
