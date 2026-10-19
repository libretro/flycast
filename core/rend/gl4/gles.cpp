#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
#include <math.h>
#include "rend/rtt_watch.h"

#include "gl4.h"
#include "rend/gles/ta_decoder_gl.h"
#include "rend/ta_gpu.h"
#include "rend/gles/glprogram_cache.h"

gl4_ctx gl4;

gl4ShaderUniforms_t gl4ShaderUniforms;

#define FOG_CHANNEL "r"
#define FOG_IMG_TYPE GL_RED

//Fragment and vertex shaders code

static const char* VertexShaderSource = R"(#version 430
#define pp_Gouraud %d

#if pp_Gouraud == 0
#define INTERPOLATION flat
#else
#define INTERPOLATION smooth
#endif

/* The depth is the position's (rend/depth_scale.h; gles/gles.cpp has the
   same): a vertex goes out at (x*w, y*w, a*S + b*w, w) with w = 1/z, a
   depth of a*S*z + b, 1/w as the chip has it. depth_scale is (a, b, -, S),
   S = 1/fZ_max, in the uniform buffer a frame decoded on the GPU writes.
   The same text places the vertices of every program, and the position is
   invariant: a pass that tests EQUAL against another's depth is exact. */
invariant gl_Position;
layout(std140, binding = 0) uniform DepthScaleBlock { highp vec4 depth_scale; };
/* Vertex constants*/ 
uniform highp mat4 normal_matrix;
/* Vertex input */
in vec4 in_pos;
in vec4 in_base;
in vec4 in_offs;
in vec2 in_uv;
in vec4 in_base1;
in vec4 in_offs1;
in vec2 in_uv1;
// a translucent strip's number, of a frame decoded on the GPU (TA_GPU_INSTANCE); 0 for the others
in uint in_pp;
/* output */
INTERPOLATION out vec4 vtx_base;
INTERPOLATION out vec4 vtx_offs;
			  out vec2 vtx_uv;
INTERPOLATION out vec4 vtx_base1;
INTERPOLATION out vec4 vtx_offs1;
			  out vec2 vtx_uv1;
flat out uint vtx_pp;
void main()
{
	vtx_pp = in_pp;
	vtx_base = in_base;
	vtx_offs = in_offs;
	vtx_uv = in_uv;
	vtx_base1 = in_base1;
	vtx_offs1 = in_offs1;
	vtx_uv1 = in_uv1;
	vec4 vpos = normal_matrix * in_pos;
	vpos.w = 1.0 / vpos.z;
	vpos.xy *= vpos.w;
	// (a z beyond fZ_max is at the nearest depth, not clipped)
	vpos.z = min(depth_scale.x * depth_scale.w + depth_scale.y * vpos.w, vpos.w);
	gl_Position = vpos;
}
)";

const char* gl4PixelPipelineShader = SHADER_HEADER
R"(
#define cp_AlphaTest %d
#define pp_ClipInside %d
#define pp_UseAlpha %d
#define pp_Texture %d
#define pp_IgnoreTexA %d
#define pp_ShadInstr %d
#define pp_Offset %d
#define pp_FogCtrl %d
#define pp_TwoVolumes %d
#define pp_Gouraud %d
#define pp_BumpMap %d
#define FogClamping %d
#define pp_Palette %d
#define PASS %d
#define GENERAL_PROGRAM %d
#define PI 3.1415926

/* The general programs (rend/gles/glprogram_cache.h): one for each PASS and
 * pp_Gouraud, the other bits of a polygon's key (gl4GetProgram()) read
 * from a uniform. The code below is the same for both, so what one draws
 * the other draws. */
#if GENERAL_PROGRAM == 1
uniform int key_bits;
#undef cp_AlphaTest
#undef pp_ClipInside
#undef pp_UseAlpha
#undef pp_Texture
#undef pp_IgnoreTexA
#undef pp_ShadInstr
#undef pp_Offset
#undef pp_FogCtrl
#undef pp_TwoVolumes
#undef pp_BumpMap
#undef FogClamping
#undef pp_Palette
#define pp_ClipInside ((key_bits >> 16) & 1)
#define cp_AlphaTest ((key_bits >> 15) & 1)
#define pp_Texture ((key_bits >> 14) & 1)
#define pp_UseAlpha ((key_bits >> 13) & 1)
#define pp_IgnoreTexA ((key_bits >> 12) & 1)
#define pp_ShadInstr ((key_bits >> 10) & 3)
#define pp_Offset ((key_bits >> 9) & 1)
#define pp_FogCtrl ((key_bits >> 7) & 3)
#define pp_TwoVolumes ((key_bits >> 6) & 1)
#define pp_BumpMap ((key_bits >> 4) & 1)
#define FogClamping ((key_bits >> 3) & 1)
#define pp_Palette ((key_bits >> 2) & 1)
#endif

#define PASS_DEPTH 0
#define PASS_COLOR 1
#define PASS_OIT 2

#if PASS == PASS_DEPTH || PASS == PASS_COLOR
	out vec4 FragColor;
#endif

#if pp_Gouraud == 0
#define INTERPOLATION flat
#else
#define INTERPOLATION smooth
#endif

/* Shader program params*/
uniform float cp_AlphaTestValue;
uniform vec4 pp_ClipTest;
uniform vec3 sp_FOG_COL_RAM,sp_FOG_COL_VERT;
uniform float sp_FOG_DENSITY;
uniform float shade_scale_factor;
uniform sampler2D tex0, tex1;
layout(binding = 5) uniform sampler2D fog_table;
uniform int pp_Number;
uniform usampler2D shadow_stencil;
uniform sampler2D DepthTex;
uniform float trilinear_alpha;
uniform vec4 fog_clamp_min;
uniform vec4 fog_clamp_max;
uniform sampler2D palette;
uniform float palette_index;

uniform ivec2 blend_mode[2];
uniform bool use_alpha[2];
uniform bool ignore_tex_alpha[2];
uniform int shading_instr[2];
uniform int fog_control[2];

/* Vertex input*/
INTERPOLATION in vec4 vtx_base;
INTERPOLATION in vec4 vtx_offs;
			  in vec2 vtx_uv;
INTERPOLATION in vec4 vtx_base1;
INTERPOLATION in vec4 vtx_offs1;
			  in vec2 vtx_uv1;
flat in uint vtx_pp;

// The fog coefficient for a depth, worked out the way the PowerVR2 does it.
// The depth times the fog density, between 1 and just under 256, is taken
// apart as a float: its exponent and the top four bits of its mantissa
// pick one of the 128 table entries, and the next eight bits of mantissa
// blend that entry with the following one. The blend is done in whole
// numbers out of 256 and so is the result, which is returned over 256.
// Splitting the float is done by comparing and halving, which is exact
// and costs less than the log2() and pow() this used to take.
float fog_mode2(float w)
{
	float z = w * sp_FOG_DENSITY;
	// Not clamp(): where the depth is infinite and the density zero their
	// product is not a number, and that has to come out as 1 like anything
	// else below 1. The texture lookup this used to end in swallowed it.
	if (!(z >= 1.0))
		z = 1.0;
	if (z > 255.9999)
		z = 255.9999;
	float e = 0.0;
	if (z >= 16.0) { z *= 0.0625; e = 4.0; }
	if (z >= 4.0) { z *= 0.25; e += 2.0; }
	if (z >= 2.0) { z *= 0.5; e += 1.0; }
	float t = (z - 1.0) * 16.0;
	float i = floor(t);
	float blend = floor((t - i) * 256.0);
	float u = (e * 16.0 + i + 0.5) / 128.0;
	// The table is two rows: the entry's own value under 0.75, the next entry's under 0.25
	float next = floor(texture(fog_table, vec2(u, 0.25)).r * 255.0 + 0.5);
	float cur = floor(texture(fog_table, vec2(u, 0.75)).r * 255.0 + 0.5);
	return floor((next * blend + cur * (255.0 - blend)) / 256.0) / 256.0;
}

vec4 fog_clamp(vec4 col)
{
	if (FogClamping == 1)
		return clamp(col, fog_clamp_min, fog_clamp_max);
	return col;
}

vec4 palettePixel(sampler2D tex, vec2 coords)
{
	// palette_index is where the middle of the palette's first colour is in the row of 1024
	vec4 c = vec4(texture(tex, coords).r * (255.0 / 1024.0) + palette_index, 0.5, 0.0, 0.0);
	return texture(palette, c.xy);
}

void main()
{
   #if PASS == PASS_OIT
      // Manual depth testing
      float frontDepth = texture(DepthTex, gl_FragCoord.xy / textureSize(DepthTex, 0)).r;
      if (FRAG_DEPTH < frontDepth)
         discard;
   #endif
  
	// Clip inside the box
	if (pp_ClipInside == 1)
	{
		if (gl_FragCoord.x >= pp_ClipTest.x && gl_FragCoord.x <= pp_ClipTest.z
				&& gl_FragCoord.y >= pp_ClipTest.y && gl_FragCoord.y <= pp_ClipTest.w)
			discard;
	}
	
   vec4 color = vtx_base;
   vec4 offset = vtx_offs;
   vec2 uv = vtx_uv;
   bool area1 = false;
   ivec2 cur_blend_mode = blend_mode[0];
   // (with one volume, the polygon's own; with two, the first's unless the pixel is in the second)
   bool cur_use_alpha = pp_UseAlpha == 1;
   bool cur_ignore_tex_alpha = pp_IgnoreTexA == 1;
   int cur_shading_instr = pp_ShadInstr;
   int cur_fog_control = pp_FogCtrl;
	
	if (pp_TwoVolumes == 1)
	{
      cur_use_alpha = use_alpha[0];
      cur_ignore_tex_alpha = ignore_tex_alpha[0];
      cur_shading_instr = shading_instr[0];
      cur_fog_control = fog_control[0];
      #if PASS == PASS_COLOR
			uvec4 stencil = texture(shadow_stencil, gl_FragCoord.xy / textureSize(shadow_stencil, 0));
			if (stencil.r == 0x81u) {
				color = vtx_base1;
				offset = vtx_offs1;
				uv = vtx_uv1;
				area1 = true;
            cur_blend_mode = blend_mode[1];
            cur_use_alpha = use_alpha[1];
            cur_ignore_tex_alpha = ignore_tex_alpha[1];
            cur_shading_instr = shading_instr[1];
            cur_fog_control = fog_control[1];
			}
      #endif
	}
	
	#if PASS == PASS_COLOR
	else
	{
		// Inside a modifier volume the PowerVR2 scales what the polygon is shaded
		// with, its base and offset colours, before the texture is combined with
		// them. Scaling the combined colour instead is the same thing for a
		// modulated texture, but also darkens a decal texture, which the
		// hardware leaves alone.
		uvec4 stencil = texture(shadow_stencil, gl_FragCoord.xy / textureSize(shadow_stencil, 0));
		if (stencil.r == 0x81u) {
			color.rgb *= shade_scale_factor;
			offset.rgb *= shade_scale_factor;
		}
	}
	#endif
	if (!cur_use_alpha)
		color.a=1.0;
	if (pp_Texture==1)
	{
      vec4 texcol;
      if (pp_Palette == 0)
      {
         if (area1)
            texcol = texture(tex1, uv);
         else
            texcol = texture(tex0, uv);
      }
      else
      {
         // only the first volume's texture is ever one of palette indices
         if (area1)
            texcol = texture(tex1, uv);
         else
            texcol = palettePixel(tex0, uv);
      }

      if (pp_BumpMap == 1)
      {
			float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;
			float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;
			texcol.a = clamp(vtx_offs.a + vtx_offs.r * sin(s) + vtx_offs.g * cos(s) * cos(r - 2.0 * PI * vtx_offs.b), 0.0, 1.0);
			texcol.rgb = vec3(1.0, 1.0, 1.0);	
      }
      else if (cur_ignore_tex_alpha)
			texcol.a=1.0;	
      if (cur_shading_instr == 0) // DECAL
		{
         color=texcol;
		}
      if (cur_shading_instr == 1) // MODULATE
		{
			color.rgb*=texcol.rgb;
			color.a=texcol.a;
		}
      if (cur_shading_instr == 2) // DECAL ALPHA
		{
			color.rgb=mix(color.rgb,texcol.rgb,texcol.a);
		}
      if (cur_shading_instr == 3) // MODULATE ALPHA
		{
			color*=texcol;
		}
		
		if (pp_Offset==1 && pp_BumpMap == 0)
		{
         color.rgb += offset.rgb;
		}
	}
   
	color = fog_clamp(color);
	
	// Look-up table mode 2: the fog colour takes the place of the pixel's
	// colour and the fog coefficient the place of its alpha. This is a mode
	// of the fog unit, which comes after the texture has been combined in,
	// so it is the combined colour that is replaced: a textured polygon
	// comes out in plain fog colour. It used to be done to the base colour,
	// before texturing, and the texture was then applied on top.
	if (cur_fog_control == 3) // LUT Mode 2
		color = vec4(sp_FOG_COL_RAM.rgb, fog_mode2(gl_FragCoord.w) * (256.0 / 255.0));
	if (cur_fog_control == 0) // LUT
	{
		float fog = fog_mode2(gl_FragCoord.w);
		color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_RAM.rgb * fog;
	}
	if (pp_Offset==1 && pp_BumpMap == 0 && cur_fog_control == 1)  // Per vertex
	{
		float fog = offset.a * (255.0 / 256.0);
		color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_VERT.rgb * fog;
	}
   
   color *= trilinear_alpha;
	
	// Punch-through polygons are tested on the alpha of the finished colour,
	// after the texture and the vertex colours are combined and clamped.
	if (cp_AlphaTest == 1)
	{
      if (cp_AlphaTestValue > color.a)
         discard;
      color.a=1.0;
	}
  
   //color.rgb=vec3(gl_FragCoord.w * sp_FOG_DENSITY / 128.0);
	
	#if PASS == PASS_COLOR 
      FragColor = color;
	#elif PASS == PASS_OIT
      // Discard as many pixels as possible
      switch (cur_blend_mode.y) // DST
		{
		case ONE:
         switch (cur_blend_mode.x) // SRC
			{
				case ZERO:
               discard;
				case ONE:
				case OTHER_COLOR:
				case INVERSE_OTHER_COLOR:
               if (color == vec4(0.0))
                  discard;
					break;
				case SRC_ALPHA:
               if (color.a == 0.0 || color.rgb == vec3(0.0))
                  discard;
               break;
				case INVERSE_SRC_ALPHA:
               if (color.a == 1.0 || color.rgb == vec3(0.0))
                  discard;
					break;
			}
			break;
		// Destination times a white source is not left alone when the
		// fragment turns out to be in a shadow: its colour is scaled then.
		case INVERSE_OTHER_COLOR:
         if (cur_blend_mode.x <= SRC_ALPHA && color == vec4(0.0))
            discard;
			break;
		case SRC_ALPHA:
         if ((cur_blend_mode.x == ZERO || cur_blend_mode.x == INVERSE_SRC_ALPHA) && color.a == 1.0)
            discard;
			break;
		case INVERSE_SRC_ALPHA:
         switch (cur_blend_mode.x) // SRC
			{
				case ZERO:
				case SRC_ALPHA:
               if (color.a == 0.0)
                  discard;
					break;
				case ONE:
				case OTHER_COLOR:
				case INVERSE_OTHER_COLOR:
               if (color == vec4(0.0))
                  discard;
					break;
			}
			break;
		}
		
      ivec2 coords = ivec2(gl_FragCoord.xy);
      uint idx =  getNextPixelIndex();
      
      Pixel pixel;
      pixel.color = packUnorm4x8(color);
      pixel.depth = FRAG_DEPTH;
      pixel.seq_num = uint(pp_Number) + vtx_pp;
      pixel.next = imageAtomicExchange(abufferPointerImg, coords, idx);
      pixels[idx] = pixel;
     
      discard;
		
	#endif
}
)";

// (the depth is the position's; nothing is shaded)
static const char* ModifierVolumeShader = SHADER_HEADER
R"(
void main()
{
}
)";

int max_image_width;
int max_image_height;
extern GLuint fogTextureId;
extern GLuint paletteTextureId;

/* A program of @pixel_source (the polygons' by default) for @s: compiled
 * now (@background: asked of the driver, not waited for; @general: the
 * general one of its pass and pp_Gouraud) */
static GLuint make_program(const gl4PipelineShader* s, const char *pixel_source, const char *vertex_source, bool general, bool background)
{
	char vshader[16384];

	snprintf(vshader, sizeof(vshader), vertex_source == NULL ? VertexShaderSource : vertex_source, s->pp_Gouraud);

	char pshader[16384];

	// (the other shaders made with this have no GENERAL_PROGRAM: the last number is not used)
	snprintf(pshader, sizeof(pshader), pixel_source,
                s->cp_AlphaTest, s->pp_InsideClipping, s->pp_UseAlpha,
                s->pp_Texture, s->pp_IgnoreTexA, s->pp_ShadInstr, s->pp_Offset, s->pp_FogCtrl,
				s->pp_TwoVolumes, s->pp_Gouraud, s->pp_BumpMap, s->fog_clamping, s->palette,
				(int)s->pass, general ? 1 : 0);

	return glprog_make(vshader, pshader, background);
}

GLuint gl4MakePipelineProgram(const gl4PipelineShader* s, bool general, bool background)
{
	return make_program(s, gl4PixelPipelineShader, NULL, general, background);
}

void gl4PipelineLocations(gl4PipelineShader* s, bool general)
{
	glcache.UseProgram(s->program);

	//setup texture 0 as the input for the shader
	GLint gu = glGetUniformLocation(s->program, "tex0");
	if ((s->pp_Texture == 1 || general) && gu != -1)
		glUniform1i(gu, 0);
	// Setup texture 1 as the input for area 1 in two volume mode
	gu = glGetUniformLocation(s->program, "tex1");
	if ((s->pp_Texture == 1 || general) && gu != -1)
		glUniform1i(gu, 1);

	//get the uniform locations
	s->pp_ClipTest      = glGetUniformLocation(s->program, "pp_ClipTest");

	s->sp_FOG_DENSITY   = glGetUniformLocation(s->program, "sp_FOG_DENSITY");

	s->cp_AlphaTestValue= glGetUniformLocation(s->program, "cp_AlphaTestValue");

	//FOG_COL_RAM,FOG_COL_VERT,FOG_DENSITY;
	if ((s->pp_FogCtrl==1 && s->pp_Texture==1) || general)
		s->sp_FOG_COL_VERT = glGetUniformLocation(s->program, "sp_FOG_COL_VERT");
	else
		s->sp_FOG_COL_VERT = -1;
	if (s->pp_FogCtrl==0 || s->pp_FogCtrl==3 || general)
	{
		s->sp_FOG_COL_RAM=glGetUniformLocation(s->program, "sp_FOG_COL_RAM");
	}
	else
	{
		s->sp_FOG_COL_RAM=-1;
	}
	s->shade_scale_factor = glGetUniformLocation(s->program, "shade_scale_factor");

	// Use texture 1 for depth texture
	gu = glGetUniformLocation(s->program, "DepthTex");
	if (gu != -1)
		glUniform1i(gu, 2);		// GL_TEXTURE2

	s->trilinear_alpha = glGetUniformLocation(s->program, "trilinear_alpha");
	
	if (s->fog_clamping || general)
	{
		s->fog_clamp_min = glGetUniformLocation(s->program, "fog_clamp_min");
		s->fog_clamp_max = glGetUniformLocation(s->program, "fog_clamp_max");
	}
	else
	{
		s->fog_clamp_min = -1;
		s->fog_clamp_max = -1;
	}
	s->normal_matrix = glGetUniformLocation(s->program, "normal_matrix");

	// Shadow stencil for OP/PT rendering pass
	gu = glGetUniformLocation(s->program, "shadow_stencil");
	if (gu != -1)
		glUniform1i(gu, 3);		// GL_TEXTURE3

	s->pp_Number = glGetUniformLocation(s->program, "pp_Number");

	s->blend_mode = glGetUniformLocation(s->program, "blend_mode");
	s->use_alpha = glGetUniformLocation(s->program, "use_alpha");
	s->ignore_tex_alpha = glGetUniformLocation(s->program, "ignore_tex_alpha");
	s->shading_instr = glGetUniformLocation(s->program, "shading_instr");
	s->fog_control = glGetUniformLocation(s->program, "fog_control");

	gu = glGetUniformLocation(s->program, "palette");
	if (gu != -1)
		glUniform1i(gu, 6);		// GL_TEXTURE6
	s->palette_index = glGetUniformLocation(s->program, "palette_index");

	s->key_bits = general ? glGetUniformLocation(s->program, "key_bits") : -1;
}

bool gl4CompilePipelineShader(	gl4PipelineShader* s, const char *pixel_source /* = PixelPipelineShader */, const char *vertex_source /* = NULL */)
{
	s->program = make_program(s, pixel_source, vertex_source, false, false);
	if (glprog_failed())
		die("shader compile fail\n");
	gl4PipelineLocations(s, false);

	return glIsProgram(s->program)==GL_TRUE;
}

static void gl_term(void)
{
	glDeleteProgram(gl4.modvol_shader.program);
	glDeleteVertexArrays(1, &gl4.vbo.main_vao);
	glDeleteVertexArrays(1, &gl4.vbo.modvol_vao);
	glDeleteBuffers(1, &gl4.vbo.geometry);
	glDeleteBuffers(1, &gl4.vbo.modvols);
	glDeleteBuffers(1, &gl4.vbo.idxs);
	glDeleteBuffers(1, &gl4.vbo.idxs2);
	glDeleteBuffers(1, &gl4.vbo.tr_poly_params);
	if (gl.depth_ubo != 0)
		glDeleteBuffers(1, &gl.depth_ubo);
	gl.depth_ubo = 0;
	for (auto it = gl4.shaders.begin(); it != gl4.shaders.end(); it++)
	{
		// (one drawn with a general program has none of its own, but may have one coming)
		if (it->second.program != 0 && it->second.general == nullptr)
			glDeleteProgram(it->second.program);
		if (it->second.pending != 0)
			glDeleteProgram(it->second.pending);
	}
	gl4.shaders.clear();
	for (auto& pass : gl4.general)
		for (gl4PipelineShader& g : pass)
		{
			if (g.program != 0)
				glDeleteProgram(g.program);
			g.program = 0;
		}
	glDeleteTextures(1, &fbTextureId);
	fbTextureId = 0;
	glDeleteTextures(1, &fogTextureId);
	fogTextureId = 0;
	glcache.DeleteTextures(1, &paletteTextureId);
	paletteTextureId = 0;
}

static bool gl_create_resources(void)
{
	// Create VAOs
	glGenVertexArrays(1, &gl4.vbo.main_vao);
	glGenVertexArrays(1, &gl4.vbo.modvol_vao);

	// Create VBOs
	glGenBuffers(1, &gl4.vbo.geometry);
	glGenBuffers(1, &gl4.vbo.modvols);
	glGenBuffers(1, &gl4.vbo.idxs);
	glGenBuffers(1, &gl4.vbo.idxs2);

	gl4SetupMainVBO();
	gl4SetupModvolVBO();

	glsm_ctx_proc_address_t proc = { nullptr };
	if (!glsm_ctl(GLSM_CTL_PROC_ADDRESS_GET, &proc) || proc.addr == nullptr)
		return false;
	gl4.vertexAttribDivisor = (void (APIENTRYP)(GLuint, GLuint))proc.addr("glVertexAttribDivisor");
	gl4.vertexAttribI4ui = (void (APIENTRYP)(GLuint, GLuint, GLuint, GLuint, GLuint))proc.addr("glVertexAttribI4ui");
	if (gl4.vertexAttribDivisor == nullptr || gl4.vertexAttribI4ui == nullptr)
		return false;
	{
		TaGpuSizes sz;
		ta_gpu_sizes(&sz);
		// (the most translucent polygons a frame can have: two words each, kept from frame to frame)
		gl4.trPolyWords.assign((size_t)sz.list[2] * 2, 0);
	}

	char vshader[16384];
	sprintf(vshader, VertexShaderSource, 1);

	gl4.modvol_shader.program=gl_CompileAndLink(vshader, ModifierVolumeShader);
	gl4.modvol_shader.normal_matrix  = glGetUniformLocation(gl4.modvol_shader.program, "normal_matrix");

	// Create the buffer for Translucent poly params
	glGenBuffers(1, &gl4.vbo.tr_poly_params);
	// Bind it
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, gl4.vbo.tr_poly_params);
	// Declare storage
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gl4.vbo.tr_poly_params);
	glCheck();

	return true;
}

extern void initABuffer();
void reshapeABuffer(int width, int height);
extern void gl4CreateTextures(int width, int height);

static void resize(int w, int h)
{
	if (w > max_image_width || h > max_image_height || stencilTexId == 0)
	{
		if (w > max_image_width)
			max_image_width = w;
		if (h > max_image_height)
			max_image_height = h;

		if (stencilTexId != 0)
		{
			glcache.DeleteTextures(1, &stencilTexId);
			stencilTexId = 0;
		}
		if (depthTexId != 0)
		{
			glcache.DeleteTextures(1, &depthTexId);
			depthTexId = 0;
		}
		if (opaqueTexId != 0)
		{
			glcache.DeleteTextures(1, &opaqueTexId);
			opaqueTexId = 0;
		}
		if (depthSaveTexId != 0)
		{
			glcache.DeleteTextures(1, &depthSaveTexId);
			depthSaveTexId = 0;
		}
		gl4CreateTextures(max_image_width, max_image_height);
		reshapeABuffer(max_image_width, max_image_height);
	}
}

static bool RenderFrame()
{
	int vmu_screen_number = 0 ;
	int lightgun_port = 0 ;

	DoCleanup();

	const bool is_rtt = pvrrc.isRTT;
	/* A pixel clip that ends before it starts holds no pixel: the chip
	 * writes nothing. (Its size would be negative here, and a texture or
	 * buffer that size is none at all.) */
	if (is_rtt && (pvrrc.fb_X_CLIP.max < pvrrc.fb_X_CLIP.min || pvrrc.fb_Y_CLIP.max < pvrrc.fb_Y_CLIP.min))
	{
		WARN_LOG(RENDERER, "Render to texture with an empty pixel clip: %d-%d, %d-%d",
				pvrrc.fb_X_CLIP.min, pvrrc.fb_X_CLIP.max, pvrrc.fb_Y_CLIP.min, pvrrc.fb_Y_CLIP.max);
		return false;
	}

	TransformMatrix<true> matrices(pvrrc);
	xform_to_mat4(&matrices.GetNormalMatrix(), gl4ShaderUniforms.normal_mat);
	const xform& scissor_mat = matrices.GetScissorMatrix();
	ViewportMatrix = matrices.GetViewportMatrix();

	/* The depth is the position's (rend/depth_scale.h, and the vertex
	 * shader): a*S*z + b, with S = 1/fZ_max; a and b take the depth to the
	 * device's clip space - [0,1] with clip control, else [-1,1] - and undo
	 * what the matrix scales z by. For a frame decoded on the GPU, S is the
	 * decode's word, copied over the host's (the copy is ordered after the
	 * decode's kernels: GlTaDecoder). */
	{
		const float mul = 1.f / matrices.GetNormalMatrix().sz;
		const float coefs[4] = { gl.clipControl != nullptr ? mul : 2.f * mul, gl.clipControl != nullptr ? 0.f : -1.f,
				0.f, pvr_depth_scale(pvrrc.fZ_max) };
		glBindBuffer(GL_UNIFORM_BUFFER, gl.depth_ubo);
		glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(coefs), coefs);
		if (pvrrc.gpu)
		{
			glBindBuffer(GL_COPY_READ_BUFFER, glTaDecoder.Draw().frame);
			gl.copyBufferSubData(GL_COPY_READ_BUFFER, GL_UNIFORM_BUFFER, TAG_O_ZSCALE * 4, 12, 4);
			glBindBuffer(GL_COPY_READ_BUFFER, 0);
		}
		glBindBuffer(GL_UNIFORM_BUFFER, 0);
		glBindBufferBase(GL_UNIFORM_BUFFER, DEPTH_UBO_BINDING, gl.depth_ubo);
		if (gl.clipControl != nullptr)
			gl.clipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
	}

	if (!is_rtt)
		gcflip=0;
	else
		gcflip=1;

	/*
		Handle Dc to screen scaling
	*/

	int rendering_width;
	int rendering_height;
	if (is_rtt)
	{
		int scaling = settings.rend.RenderToTextureUpscale;
		rendering_width = matrices.GetDreamcastViewport().x * scaling;
		rendering_height = matrices.GetDreamcastViewport().y * scaling;
	}
	else
	{
		rendering_width = screen_width;
		rendering_height = screen_height;
	}
	resize(rendering_width, rendering_height);

	//VERT and RAM fog color constants
	u8* fog_colvert_bgra=(u8*)&FOG_COL_VERT;
	u8* fog_colram_bgra=(u8*)&FOG_COL_RAM;
	gl4ShaderUniforms.ps_FOG_COL_VERT[0]=fog_colvert_bgra[2]/255.0f;
	gl4ShaderUniforms.ps_FOG_COL_VERT[1]=fog_colvert_bgra[1]/255.0f;
	gl4ShaderUniforms.ps_FOG_COL_VERT[2]=fog_colvert_bgra[0]/255.0f;

	gl4ShaderUniforms.ps_FOG_COL_RAM[0]=fog_colram_bgra [2]/255.0f;
	gl4ShaderUniforms.ps_FOG_COL_RAM[1]=fog_colram_bgra [1]/255.0f;
	gl4ShaderUniforms.ps_FOG_COL_RAM[2]=fog_colram_bgra [0]/255.0f;

	//Fog density constant
	u8* fog_density=(u8*)&FOG_DENSITY;
	float fog_den_mant=fog_density[1]/128.0f;  //bit 7 -> x. bit, so [6:0] -> fraction -> /128
	s32 fog_den_exp=(s8)fog_density[0];
	gl4ShaderUniforms.fog_den_float = fog_den_mant * powf(2.0f,fog_den_exp) * settings.rend.ExtraDepthScale;

	gl4ShaderUniforms.fog_clamp_min[0] = ((pvrrc.fog_clamp_min >> 16) & 0xFF) / 255.0f;
	gl4ShaderUniforms.fog_clamp_min[1] = ((pvrrc.fog_clamp_min >> 8) & 0xFF) / 255.0f;
	gl4ShaderUniforms.fog_clamp_min[2] = ((pvrrc.fog_clamp_min >> 0) & 0xFF) / 255.0f;
	gl4ShaderUniforms.fog_clamp_min[3] = ((pvrrc.fog_clamp_min >> 24) & 0xFF) / 255.0f;

	gl4ShaderUniforms.fog_clamp_max[0] = ((pvrrc.fog_clamp_max >> 16) & 0xFF) / 255.0f;
	gl4ShaderUniforms.fog_clamp_max[1] = ((pvrrc.fog_clamp_max >> 8) & 0xFF) / 255.0f;
	gl4ShaderUniforms.fog_clamp_max[2] = ((pvrrc.fog_clamp_max >> 0) & 0xFF) / 255.0f;
	gl4ShaderUniforms.fog_clamp_max[3] = ((pvrrc.fog_clamp_max >> 24) & 0xFF) / 255.0f;

	UpdateFogTexture((u8 *)FOG_TABLE, GL_TEXTURE5, GL_RED);
	if (palette_updated)
	{
		UpdatePaletteTexture(GL_TEXTURE6);
		palette_updated = false;
	}

	// (the programs being made in the background polled once a frame: gl4GetProgram())
	gl4.frame++;

	glcache.UseProgram(gl4.modvol_shader.program);

	glUniformMatrix4fv(gl4.modvol_shader.normal_matrix, 1, GL_FALSE, gl4ShaderUniforms.normal_mat);

	// half a step under the reference: see the same in gles.cpp
	gl4ShaderUniforms.PT_ALPHA = ((PT_ALPHA_REF & 0xFF) - 0.5f) / 255.0f;

	GLuint output_fbo;

	//setup render target first
	if (is_rtt)
	{
		GLuint channels,format;
		switch(FB_W_CTRL.fb_packmode)
		{
		case 0: //0x0   0555 KRGB 16 bit  (default)	Bit 15 is the value of fb_kval[7].
			channels=GL_RGBA;
			format=GL_UNSIGNED_BYTE;
			break;

		case 1: //0x1   565 RGB 16 bit
			channels=GL_RGB;
			/* Eight bits a channel, as the other renderers have it: the
			 * console blends at eight bits and cuts the result down to five
			 * and six when it writes it to video memory, which is done when
			 * the picture is read back. Asked for as 5-6-5, a driver may
			 * give just that, and every blend then rounds to five bits. */
			format=GL_UNSIGNED_BYTE;
			break;

		case 2: //0x2   4444 ARGB 16 bit
			channels=GL_RGBA;
			format=GL_UNSIGNED_BYTE;
			break;

		case 3://0x3    1555 ARGB 16 bit    The alpha value is determined by comparison with the value of fb_alpha_threshold.
			channels=GL_RGBA;
			format=GL_UNSIGNED_BYTE;
			break;

		case 4: //0x4   888 RGB 24 bit packed
		case 5: //0x5   0888 KRGB 32 bit    K is the value of fk_kval.
		case 6: //0x6   8888 ARGB 32 bit
			WARN_LOG(RENDERER, "Unsupported render to texture format: %d", FB_W_CTRL.fb_packmode);
			return false;
		case 7: //7     invalid
			// (a mode the chip does not have, which a game can write all the same: nothing is drawn)
			WARN_LOG(RENDERER, "Invalid render to texture format: 7");
			return false;
		}
		DEBUG_LOG(RENDERER, "RTT packmode=%d stride=%d - %d,%d -> %d,%d", FB_W_CTRL.fb_packmode, FB_W_LINESTRIDE.stride * 8,
 				FB_X_CLIP.min, FB_Y_CLIP.min, FB_X_CLIP.max, FB_Y_CLIP.max);
		output_fbo = gl4BindRTT(FB_W_SOF1 & VRAM_MASK, (u32)lroundf(matrices.GetDreamcastViewport().x),
				(u32)lroundf(matrices.GetDreamcastViewport().y), channels, format);
	}
	else
	{
		glViewport(0, 0, screen_width, screen_height);
		output_fbo = hw_render.get_current_framebuffer();
	}

	glcache.Disable(GL_SCISSOR_TEST);

	//move vertex to gpu
	if (!pvrrc.isRenderFramebuffer && pvrrc.gpu)
		// (decoded on the GPU: there already)
		gl4BindFrameBuffers(true);
	else if (!pvrrc.isRenderFramebuffer)
	{
		gl4BindFrameBuffers(false);
		//Main VBO
		glBindVertexArray(gl4.vbo.main_vao);
		glBindBuffer(GL_ARRAY_BUFFER, gl4.vbo.geometry); glCheck();
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl4.vbo.idxs); glCheck();

		glBufferData(GL_ARRAY_BUFFER,pvrrc.verts.bytes(),pvrrc.verts.head(),GL_STREAM_DRAW); glCheck();

		glBufferData(GL_ELEMENT_ARRAY_BUFFER,pvrrc.idx.bytes(),pvrrc.idx.head(),GL_STREAM_DRAW); glCheck();

		//Modvol VBO
		if (pvrrc.modtrig.used())
		{
			glBindVertexArray(gl4.vbo.modvol_vao);
			glBindBuffer(GL_ARRAY_BUFFER, gl4.vbo.modvols); glCheck();
			glBufferData(GL_ARRAY_BUFFER,pvrrc.modtrig.bytes(),pvrrc.modtrig.head(),GL_STREAM_DRAW); glCheck();
		}

		/* The translucent polygons' words: two each (see PolyParam in
		 * SHADER_HEADER), made into memory kept for them */
		const u32 trCount = std::min<u32>(pvrrc.global_param_tr.used(), (u32)gl4.trPolyWords.size() / 2);
		const PolyParam *pp = pvrrc.global_param_tr.head();
		u32 *words = gl4.trPolyWords.data();
		for (u32 i = 0; i < trCount; i++, pp++, words += 2)
		{
			words[0] = (pp->tsp.full & 0xffff00c0) | ((pp->isp.full >> 16) & 0xe400) | ((pp->pcw.full >> 7) & 1)
					| (pp->tsp1.full != (u32)-1 || pp->tcw1.full != (u32)-1 ? TAG_P_TWOVOL : 0u);
			words[1] = pp->tsp1.full;
		}
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, gl4.vbo.tr_poly_params);
		glBufferData(GL_SHADER_STORAGE_BUFFER, trCount * 8, gl4.trPolyWords.data(), GL_STATIC_DRAW);
		glCheck();
	}
	if (!pvrrc.isRenderFramebuffer)
	{
		if (is_rtt || !settings.rend.WideScreen || matrices.IsClipped())
		{
			float width;
			float height;
			float min_x;
			float min_y;
			if (!is_rtt)
			{
				min_x = xform_x(&scissor_mat, pvrrc.fb_X_CLIP.min);
				min_y = xform_y(&scissor_mat, pvrrc.fb_Y_CLIP.min);
				width = xform_w(&scissor_mat, pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1);
				height = xform_h(&scissor_mat, pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1);
				if (width < 0)
				{
					min_x += width;
					width = -width;
				}
				if (height < 0)
				{
					min_y += height;
					height = -height;
				}
				if (matrices.GetSidebarWidth() > 0)
				{
					float rounded_offs_x = matrices.GetSidebarWidth() + 0.5f;

					glcache.ClearColor(0.f, 0.f, 0.f, 0.f);
					glcache.Enable(GL_SCISSOR_TEST);
					glcache.Scissor(0, 0, rounded_offs_x, screen_height);
					glClear(GL_COLOR_BUFFER_BIT);
					glcache.Scissor(screen_width - rounded_offs_x, 0, rounded_offs_x, screen_height);
					glClear(GL_COLOR_BUFFER_BIT);
				}
			}
			else
			{
				width = pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1;
				height = pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1;
				min_x = pvrrc.fb_X_CLIP.min;
				min_y = pvrrc.fb_Y_CLIP.min;
				if (settings.rend.RenderToTextureUpscale > 1)
				{
					min_x *= settings.rend.RenderToTextureUpscale;
					min_y *= settings.rend.RenderToTextureUpscale;
					width *= settings.rend.RenderToTextureUpscale;
					height *= settings.rend.RenderToTextureUpscale;
				}
			}

			gl4ShaderUniforms.base_clipping.enabled = true;
			gl4ShaderUniforms.base_clipping.x = (int)lroundf(min_x);
			gl4ShaderUniforms.base_clipping.y = (int)lroundf(min_y);
			gl4ShaderUniforms.base_clipping.width = (int)lroundf(width);
			gl4ShaderUniforms.base_clipping.height = (int)lroundf(height);
			glcache.Scissor(gl4ShaderUniforms.base_clipping.x, gl4ShaderUniforms.base_clipping.y,
					gl4ShaderUniforms.base_clipping.width, gl4ShaderUniforms.base_clipping.height);
			glcache.Enable(GL_SCISSOR_TEST);
		}
		else
		{
			gl4ShaderUniforms.base_clipping.enabled = false;
		}

		if (!is_rtt)
		{
			/* in a new context, the picture kept from the old one (rend/last_picture.h) */
			glBindFramebuffer(GL_FRAMEBUFFER, settings.rend.PowerVR2Filter ? postProcessor.GetFramebuffer() : output_fbo);
			gl_restore_picture();
		}
		if (settings.rend.PowerVR2Filter && !is_rtt)
		{
			gl4DrawStrips(postProcessor.GetFramebuffer(), rendering_width, rendering_height);
			postProcessor.Render(output_fbo);
		}
		else
			gl4DrawStrips(output_fbo, rendering_width, rendering_height);
	}
	else
	{
		glBindFramebuffer(GL_FRAMEBUFFER, output_fbo);

		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glcache.ClearColor(0.f, 0.f, 0.f, 0.f);
		glClear(GL_COLOR_BUFFER_BIT);

		gl4DrawFramebuffer(640.f, 480.f);
	}


	/* VMU/Crosshair code - libretro-specific */
	if (!is_rtt)
	{
		if (settings.System == DC_PLATFORM_DREAMCAST)
		{
			for ( vmu_screen_number = 0 ; vmu_screen_number < 4 ; vmu_screen_number++)
				if ( vmu_screen_params[vmu_screen_number].vmu_screen_display )
					gl4DrawVmuTexture(vmu_screen_number);
		}

		for ( lightgun_port = 0 ; lightgun_port < 4 ; lightgun_port++)
			gl4DrawGunCrosshair(lightgun_port);
	}

	KillTex = false;

	if (is_rtt)
		ReadRTTBuffer();

	glBindVertexArray(0);
	// (the frontend's own drawing has the context as it was)
	if (gl.clipControl != nullptr)
		gl.clipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);

	return !is_rtt;
}

void termABuffer();

void reshapeABuffer(int w, int h);

struct gl4rend : Renderer
{
	bool Init() override
	{
		findGLVersion();
		if (gl.gl_major < 4 || (gl.gl_major == 4 && gl.gl_minor < 3))
		{
			WARN_LOG(RENDERER, "Warning: OpenGL %d.%d doesn't support per-pixel sorting. 4.3 required", gl.gl_major, gl.gl_minor);
			return false;
		}
		// (the vertex shaders take the depth scale from a uniform buffer: findGLVersion())
		if (gl.depth_ubo == 0)
		{
			WARN_LOG(RENDERER, "OpenGL: no uniform buffer for the depth scale");
			return false;
		}
		glprog_open();

		if (!gl_create_resources())
			return false;

		glcache.DisableCache();
		/* (a new context has dithering on: see the other OpenGL renderer) */
		glDisable(GL_DITHER);

		//    glEnable(GL_DEBUG_OUTPUT);
		//    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		//    glDebugMessageCallback(gl_DebugOutput, NULL);
		//    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);

		initABuffer();

		// GPU-side TA processing, if it is on: here, when the renderer is set up, not in a frame
		glTaDecoder.WantSort(false);
		glTaDecoder.Setup(ta_gpu_wanted());
		gl4ForgetFrameBuffers();

#ifdef HAVE_TEXUPSCALE
		if (settings.rend.TextureUpscale > 1)
		{
			// Trick to preload the tables used by xBRZ
			u32 src[] { 0x11111111, 0x22222222, 0x33333333, 0x44444444 };
			u32 dst[16];
			UpscalexBRZ(2, src, dst, 2, 2, false);
		}
#endif
		palette_updated = true;
		TexCache.Clear();
		// the textures' pool and the decoder, all of them now (gles/gl_texdecoder.h)
		gl_tex_init(false);

		if (settings.rend.PowerVR2Filter)
			postProcessor.Init();

		// the polygons' programs, so that none is made in a frame (glprogram_cache.h)
		gl4MakePrograms();
		glprog_save();
		glprog_started();

		return true;
	}
	void Resize(int w, int h) override
	{
		gl_carry_picture(screen_width, screen_height, w, h);
		screen_width=w;
		screen_height=h;
		resize(w, h);
		// the filter's own picture is of the size there was: it goes to the new one
		if (settings.rend.PowerVR2Filter)
			postProcessor.Resize();
		else
			postProcessor.Term();
	}
	void KeepPicture() override
	{
		gl_keep_picture();
	}

	void Term() override
	{
		// (what is new kept, while the programs are there: glprogram_cache.h)
		glprog_log("per pixel");
		glprog_close();
		postProcessor.Term();
		glcache.DeleteTextures(ARRAY_SIZE(vmuTextureId), vmuTextureId);
		memset(vmuTextureId, 0, sizeof(vmuTextureId));
		glcache.DeleteTextures(ARRAY_SIZE(lightgunTextureId), lightgunTextureId);
		memset(lightgunTextureId, 0, sizeof(lightgunTextureId));

		glTaDecoder.Term();
		termABuffer();
		if (stencilTexId != 0)
		{
			glcache.DeleteTextures(1, &stencilTexId);
			stencilTexId = 0;
		}
		if (depthTexId != 0)
		{
			glcache.DeleteTextures(1, &depthTexId);
			depthTexId = 0;
		}
		if (opaqueTexId != 0)
		{
			glcache.DeleteTextures(1, &opaqueTexId);
			opaqueTexId = 0;
		}
		if (depthSaveTexId != 0)
		{
			glcache.DeleteTextures(1, &depthSaveTexId);
			depthSaveTexId = 0;
		}
		if (geom_fbo != 0)
		{
			glDeleteFramebuffers(1, &geom_fbo);
			geom_fbo = 0;
		}
		if (texSamplers[0] != 0)
		{
			glDeleteSamplers(2, texSamplers);
			texSamplers[0] = texSamplers[1] = 0;
		}
		if (depth_fbo != 0)
		{
			glDeleteFramebuffers(1, &depth_fbo);
			depth_fbo = 0;
		}
		TexCache.Clear();
		rtt_watch_term();
		gl_tex_term();

		gl_term();
	}

	bool Process(TA_context* ctx) override
	{
#if !defined(TARGET_NO_THREADS)
		if (!settings.rend.ThreadedRendering)
#endif
			glsm_ctl(GLSM_CTL_STATE_BIND, NULL);
		/* GPU-side TA processing: the frame drawn from the decode, each
		 * translucent strip numbered by its draw (TA_GPU_INSTANCE) */
		return ProcessFrame(ctx, TA_GPU_DRAW | TA_GPU_INSTANCE);
	}
	bool Render() override
	{
		bool ret = RenderFrame();
		glTaDecoder.EndFrame();
#if !defined(TARGET_NO_THREADS)
		if (!settings.rend.ThreadedRendering)
#endif
			glsm_ctl(GLSM_CTL_STATE_UNBIND, NULL);
		return ret;
	}

	virtual u64 GetTexture(TSP tsp, TCW tcw, int area = 0) override
	{
		return gl_GetTexture(tsp, tcw, area);
	}

	// (TA processing switched on or off: between frames, the state bound)
	void OptionsChanged() override
	{
		glTaDecoder.Setup(ta_gpu_wanted());
		gl4ForgetFrameBuffers();
	}
};

Renderer* rend_GL4() { return new gl4rend(); }
