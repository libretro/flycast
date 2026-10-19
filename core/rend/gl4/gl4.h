#pragma once
#include "rend/gles/gles.h"
#include <unordered_map>
#include <vector>

void gl4DrawStrips(GLuint output_fbo, int width, int height);

/* A frame drawn in horizontal bands (abuffer.cpp, gl4OitChooseBands()):
 * the band being drawn, in pixels of the framebuffer; no height when the
 * frame is drawn whole. Every scissor rectangle the frame's draws set is
 * cut to it: gl4Scissor() in place of glcache.Scissor(), gl4ScissorOff()
 * in place of turning the scissor test off. */
struct gl4Band
{
	int y, height;
};
extern gl4Band gl4band;
static inline void gl4Scissor(int x, int y, int width, int height)
{
	if (gl4band.height != 0)
	{
		const int top = y > gl4band.y ? y : gl4band.y;
		const int bottom = y + height < gl4band.y + gl4band.height ? y + height : gl4band.y + gl4band.height;
		y = top;
		height = bottom > top ? bottom - top : 0;
	}
	glcache.Enable(GL_SCISSOR_TEST);
	glcache.Scissor(x, y, width, height);
}
static inline void gl4ScissorOff()
{
	if (gl4band.height != 0)
	{
		glcache.Enable(GL_SCISSOR_TEST);
		glcache.Scissor(0, gl4band.y, 0x7FFF, gl4band.height);
	}
	else
		glcache.Disable(GL_SCISSOR_TEST);
}
/* The bands this frame (a render to a texture, or the screen's) is drawn
 * in: from the fragment counts of frames before, which nothing waits for */
u32 gl4OitChooseBands(bool rtt);
/* A band starts: its lists empty, its count at nothing; not before the
 * band before is on the picture */
void gl4OitBandBegin();
/* A band's translucent polygons are all drawn: its count copied out */
void gl4OitBandEnd(u32 band);
/* The frame's bands are drawn: a fence for their counts */
void gl4OitFrameEnd(u32 bands, bool rtt);

enum class Pass { Depth, Color, OIT };

struct gl4PipelineShader
{
	GLuint program;

	GLint pp_ClipTest;
	GLint cp_AlphaTestValue;
	GLint sp_FOG_COL_RAM;
	GLint sp_FOG_COL_VERT;
	GLint sp_FOG_DENSITY;
	GLint shade_scale_factor;
	GLint pp_Number;
	GLint blend_mode;
	GLint use_alpha;
	GLint ignore_tex_alpha;
	GLint shading_instr;
	GLint fog_control;
	GLint trilinear_alpha;
	GLint fog_clamp_min, fog_clamp_max;
	GLint normal_matrix;
	GLint palette_index;

	bool cp_AlphaTest;
	bool pp_InsideClipping;
	bool pp_Texture;
	bool pp_UseAlpha;
	bool pp_IgnoreTexA;
	u32 pp_ShadInstr;
	bool pp_Offset;
	u32 pp_FogCtrl;
	Pass pass;
	bool pp_TwoVolumes;
	bool pp_Gouraud;
	bool pp_BumpMap;
	bool fog_clamping;
	bool palette;

	/* (rend/gles/glprogram_cache.h) The polygon's key (gl4GetProgram());
	 * for one drawn with a general program, that program's record, whose
	 * program and uniforms these are, and the program of its own being
	 * made in the background (0: none); the frame it was last polled in;
	 * the uniform a general program reads the key from (-1: none) */
	u32 key;
	gl4PipelineShader *general;
	GLuint pending;
	u32 polled;
	GLint key_bits;
};


struct gl4_ctx
{
	struct
	{
		GLuint program;

		GLint normal_matrix;
	} modvol_shader;

	std::unordered_map<u32, gl4PipelineShader> shaders;
	/* The general programs (rend/gles/glprogram_cache.h), by pass and
	 * pp_Gouraud; and the frame, for polling the programs being made */
	gl4PipelineShader general[3][2];
	u32 frame;

	struct
	{
		GLuint geometry,modvols,idxs,idxs2;
		GLuint main_vao;
		GLuint modvol_vao;
		GLuint tr_poly_params;
	} vbo;
	/* (made with the resources) the translucent polygons' words of a frame
	 * the CPU parsed, made here (see PolyParam in SHADER_HEADER) */
	std::vector<u32> trPolyWords;
	// what the state manager (glsm) has no function for
	void (APIENTRYP vertexAttribDivisor)(GLuint index, GLuint divisor);
	void (APIENTRYP vertexAttribI4ui)(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w);
};

extern gl4_ctx gl4;

extern int screen_width;
extern int screen_height;
extern int max_image_width;
extern int max_image_height;

GLuint gl4BindRTT(u32 addy, u32 fbw, u32 fbh, u32 channels, u32 fmt);
void gl4DrawFramebuffer(float w, float h);
void abufferDrawQuad();

extern const char *gl4PixelPipelineShader;
bool gl4CompilePipelineShader(gl4PipelineShader* s, const char *pixel_source = gl4PixelPipelineShader, const char *vertex_source = NULL);
/* A polygon's program (glprogram_cache.h) asked of the driver: its own, or
 * (@general) the general one of its pass and pp_Gouraud; and its uniforms
 * found, once it is there (the program bound) */
GLuint gl4MakePipelineProgram(const gl4PipelineShader* s, bool general, bool background);
void gl4PipelineLocations(gl4PipelineShader* s, bool general);
// At the renderer's start: the general programs and those of the keys of earlier runs
void gl4MakePrograms();

extern GLuint stencilTexId;
extern GLuint depthTexId;
extern GLuint opaqueTexId;
extern GLuint depthSaveTexId;
extern GLuint geom_fbo;
extern GLuint texSamplers[2];
extern GLuint depth_fbo;

void gl4DrawVmuTexture(u8 vmu_screen_number);
void gl4DrawGunCrosshair(u8 port);

#define SHADER_HEADER "#version 430 \n\
\n\
layout(r32ui, binding = 4) uniform coherent restrict uimage2D abufferPointerImg; \n\
struct Pixel { \n\
	uint color;		// RGBA8, as the PowerVR keeps a fragment's colour \n\
	float depth; \n\
	uint seq_num; \n\
	uint next; \n\
}; \n\
#define EOL 0xFFFFFFFFu \n\
layout (binding = 0, std430) coherent restrict buffer PixelBuffer { \n\
	Pixel pixels[]; \n\
}; \n\
layout(binding = 0, offset = 0) uniform atomic_uint buffer_index; \n\
\n\
#define ZERO				0 \n\
#define ONE					1 \n\
#define OTHER_COLOR			2 \n\
#define INVERSE_OTHER_COLOR	3 \n\
#define SRC_ALPHA			4 \n\
#define INVERSE_SRC_ALPHA	5 \n\
#define DST_ALPHA			6 \n\
#define INVERSE_DST_ALPHA	7 \n\
 \n\
uint getNextPixelIndex() \n\
{ \n\
	uint index = atomicCounterIncrement(buffer_index); \n\
	if (index >= pixels.length()) \n\
		// Buffer overflow \n\
		discard; \n\
	 \n\
	return index; \n\
} \n\
\n\
/* A fragment's depth: the position's, which the depth test took (rend/depth_scale.h) */ \n\
#define FRAG_DEPTH gl_FragCoord.z \n\
/* A translucent polygon's words, as the Vulkan per-pixel renderer and \n\
 * GPU-side TA processing have them (rend/ta_gpu.h, TAG_P_WORDS): its TSP \n\
 * word with the ISP word's depth function and depth write bits in 13-15 \n\
 * and 10, whether it has two volumes (its second TSP or TCW word not all \n\
 * ones: TAG_P_TWOVOL) in 1 and whether it takes shadows in 0; and its \n\
 * second volume's TSP word */ \n\
struct PolyParam { \n\
	int tsp_isp_pcw; \n\
	int tsp1; \n\
}; \n\
layout (binding = 1, std430) readonly buffer TrPolyParamBuffer { \n\
	PolyParam tr_poly_params[]; \n\
}; \n\
 \n\
#define GET_TSP_FOR_AREA int tsp; if (area1) tsp = pp.tsp1; else tsp = pp.tsp_isp_pcw; \n\
 \n\
int getSrcBlendFunc(const PolyParam pp, bool area1) \n\
{ \n\
	GET_TSP_FOR_AREA \n\
	return (tsp >> 29) & 7; \n\
} \n\
\n\
int getDstBlendFunc(const PolyParam pp, bool area1) \n\
{ \n\
	GET_TSP_FOR_AREA \n\
	return (tsp >> 26) & 7; \n\
} \n\
\n\
bool getSrcSelect(const PolyParam pp, bool area1) \n\
{ \n\
	GET_TSP_FOR_AREA \n\
	return ((tsp >> 25) & 1) != 0; \n\
} \n\
\n\
bool getDstSelect(const PolyParam pp, bool area1) \n\
{ \n\
	GET_TSP_FOR_AREA \n\
	return ((tsp >> 24) & 1) != 0; \n\
} \n\
\n\
int getFogControl(const PolyParam pp, bool area1) \n\
{ \n\
	GET_TSP_FOR_AREA \n\
	return (tsp >> 22) & 3; \n\
} \n\
\n\
bool getUseAlpha(const PolyParam pp, bool area1) \n\
{ \n\
	GET_TSP_FOR_AREA \n\
	return ((tsp >> 20) & 1) != 0; \n\
} \n\
\n\
bool getIgnoreTexAlpha(const PolyParam pp, bool area1) \n\
{ \n\
	GET_TSP_FOR_AREA \n\
	return ((tsp >> 19) & 1) != 0; \n\
} \n\
\n\
int getShadingInstruction(const PolyParam pp, bool area1) \n\
{ \n\
	GET_TSP_FOR_AREA \n\
	return (tsp >> 6) & 3; \n\
} \n\
\n\
int getDepthFunc(const PolyParam pp) \n\
{ \n\
	return (pp.tsp_isp_pcw >> 13) & 7; \n\
} \n\
\n\
bool getDepthMask(const PolyParam pp) \n\
{ \n\
	return ((pp.tsp_isp_pcw >> 10) & 1) != 1; \n\
} \n\
\n\
bool getShadowEnable(const PolyParam pp) \n\
{ \n\
	return (pp.tsp_isp_pcw & 1) != 0; \n\
} \n\
\n\
uint getPolyNumber(const Pixel pixel) \n\
{ \n\
	return pixel.seq_num & 0x3FFFFFFFu; \n\
} \n\
\n\
#define SHADOW_STENCIL 0x40000000u \n\
#define SHADOW_ACC	   0x80000000u \n\
\n\
bool isShadowed(const Pixel pixel) \n\
{ \n\
	return (pixel.seq_num & SHADOW_ACC) == SHADOW_ACC; \n\
} \n\
\n\
bool isTwoVolumes(const PolyParam pp) \n\
{ \n\
	return (pp.tsp_isp_pcw & 2) != 0; \n\
} \n\
 \n\
"

void gl4SetupMainVBO();
void gl4SetupModvolVBO();
/* A frame decoded on the GPU (pvrrc.gpu, rend/gles/ta_decoder_gl.h): its
 * vertices, indices, modifier volume triangles and translucent polygons'
 * words bound in their places (the vertex arrays' and binding 1), and each
 * translucent strip's number given its fragments by the draw
 * (TA_GPU_INSTANCE); or, @gpu false, the renderer's own buffers - for a
 * frame the CPU parsed, and what is drawn from them after (the VMU
 * screens, the crosshairs, the framebuffer). Nothing is done when they are
 * bound already; gl4ForgetFrameBuffers(): the decoder's buffers are made
 * again, and are bound the next time. */
void gl4BindFrameBuffers(bool gpu);
void gl4ForgetFrameBuffers();

extern struct gl4ShaderUniforms_t
{
	float PT_ALPHA;
	float fog_den_float;
	float ps_FOG_COL_RAM[3];
	float ps_FOG_COL_VERT[3];
	int poly_number;
	float trilinear_alpha;
	TSP tsp0;
	TSP tsp1;
	TCW tcw0;
	TCW tcw1;
	float fog_clamp_min[4];
	float fog_clamp_max[4];
	float normal_mat[16];
	float palette_index;

	struct {
		bool enabled;
		int x;
		int y;
		int width;
		int height;
	} base_clipping;

	void setUniformArray(GLint location, int v0, int v1)
	{
		int array[] = { v0, v1 };
		glUniform1iv(location, 2, array);
	}

	void Set(gl4PipelineShader* s)
	{
		if (s->cp_AlphaTestValue!=-1)
			glUniform1f(s->cp_AlphaTestValue,PT_ALPHA);

		if (s->sp_FOG_DENSITY!=-1)
			glUniform1f( s->sp_FOG_DENSITY,fog_den_float);

		if (s->sp_FOG_COL_RAM!=-1)
			glUniform3fv( s->sp_FOG_COL_RAM, 1, ps_FOG_COL_RAM);

		if (s->sp_FOG_COL_VERT!=-1)
			glUniform3fv( s->sp_FOG_COL_VERT, 1, ps_FOG_COL_VERT);

		if (s->shade_scale_factor != -1)
			glUniform1f(s->shade_scale_factor, FPU_SHAD_SCALE.scale_factor / 256.f);

		if (s->blend_mode != -1) {
			// The per-pixel pass drops a fragment whose blend would leave the
			// pixel as it is. It cannot tell that when the source is the second
			// accumulation buffer, or when the polygon has a second volume and
			// which blend applies is known only when the lists are put
			// together: those get a blend (8) that none of its cases take.
			bool keep = tsp0.SrcSelect || tsp1.full != (u32)-1 || tcw1.full != (u32)-1;
			u32 blend_mode[] = { keep ? 8u : (u32)tsp0.SrcInstr, keep ? 8u : (u32)tsp0.DstInstr, tsp1.SrcInstr, tsp1.DstInstr };
			glUniform2iv(s->blend_mode, 2, (GLint *)blend_mode);
		}

		if (s->use_alpha != -1)
			setUniformArray(s->use_alpha, tsp0.UseAlpha, tsp1.UseAlpha);

		if (s->ignore_tex_alpha != -1)
			setUniformArray(s->ignore_tex_alpha, tsp0.IgnoreTexA, tsp1.IgnoreTexA);

		if (s->shading_instr != -1)
			setUniformArray(s->shading_instr, tsp0.ShadInstr, tsp1.ShadInstr);

		if (s->fog_control != -1)
			setUniformArray(s->fog_control, tsp0.FogCtrl, tsp1.FogCtrl);

		if (s->pp_Number != -1)
			glUniform1i(s->pp_Number, poly_number);

		if (s->trilinear_alpha != -1)
			glUniform1f(s->trilinear_alpha, trilinear_alpha);
		
		if (s->fog_clamp_min != -1)
			glUniform4fv(s->fog_clamp_min, 1, fog_clamp_min);
		if (s->fog_clamp_max != -1)
			glUniform4fv(s->fog_clamp_max, 1, fog_clamp_max);

		if (s->normal_matrix != -1)
			glUniformMatrix4fv(s->normal_matrix, 1, GL_FALSE, normal_mat);

		if (s->palette_index != -1)
			glUniform1f(s->palette_index, palette_index);

		// (a polygon drawn with a general program: its key)
		if (s->key_bits != -1)
			glUniform1i(s->key_bits, (GLint)s->key);
	}

} gl4ShaderUniforms;

