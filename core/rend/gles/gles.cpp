#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
#include <math.h>
#include "rend/rtt_watch.h"
#include <string.h>

#include <libretro.h>

#include "gles.h"
#include "ta_decoder_gl.h"
#include "glprogram_cache.h"

#ifndef GL_RED
#define GL_RED                            0x1903
#endif
#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION                  0x821B
#endif
#ifndef GL_MINOR_VERSION
#define GL_MINOR_VERSION                  0x821C
#endif

GLCache glcache;
gl_ctx gl;
GlTaDecoder glTaDecoder;

struct ShaderUniforms_t ShaderUniforms;

u32 gcflip;

float fb_scale_x, fb_scale_y;

//Fragment and vertex shaders code

static const char* VertexShaderSource = R"(%s
#define TARGET_GL %s
#define pp_Gouraud %d
#define DEPTH_UBO %d

#define GLES2 0
#define GLES3 1
#define GL2 2
#define GL3 3

// (uniform buffers: OpenGL 3.1's, an extension to GLSL 1.30; see depth_scale below)
#if DEPTH_UBO == 1 && TARGET_GL == GL3
#extension GL_ARB_uniform_buffer_object : enable
#endif
#if TARGET_GL == GL2
#define highp
#define lowp
#define mediump
#endif
#if TARGET_GL == GLES2 || TARGET_GL == GL2
#define in attribute
#define out varying
#endif


#if TARGET_GL == GL3 || TARGET_GL == GLES3
#if pp_Gouraud == 0
#define INTERPOLATION flat
#else
#define INTERPOLATION smooth
#endif
#else
#define INTERPOLATION
#endif

/* The depth is the position's (rend/depth_scale.h): the vertex goes out at
   (x*w, y*w, a*S + b*w, w) with w = 1/z, a depth of a*S*z + b, 1/w as the
   chip has it, linear in screen space. depth_scale is (a, b, -, S): S is
   1/fZ_max, which a frame decoded on the GPU has only on the GPU, so where
   the context has uniform buffers it is one the renderer copies into
   (DEPTH_UBO; gl_ctx::depth_ubo). The same text places the vertices of
   every program, and the position is invariant: a pass that tests EQUAL
   against another's depth is exact. */
invariant gl_Position;
#if DEPTH_UBO == 1
layout(std140) uniform DepthScaleBlock { highp vec4 depth_scale; };
#else
uniform highp vec4      depth_scale;
#endif
/* Vertex constants*/ 
uniform highp mat4 normal_matrix;
uniform highp float sp_FOG_DENSITY;

/* Vertex input */
in highp vec4    in_pos;
in mediump vec4  in_base;
in mediump vec4  in_offs;
in highp vec2    in_uv;
/* output */
INTERPOLATION out mediump vec4 vtx_base;
INTERPOLATION out mediump vec4 vtx_offs;
              out highp vec2 vtx_uv;
#if TARGET_GL == GLES2
              out highp float fog_depth;
#endif 

void main()
{
	vtx_base = in_base;
	vtx_offs = in_offs;
	vtx_uv = in_uv;
	highp vec4 vpos = normal_matrix * in_pos;
	vpos.w = 1.0 / vpos.z;
#if TARGET_GL == GLES2
   // Interpolated with perspective correction, 1/depth comes out as one over
   // the depth at the pixel, which is what fog goes by. depth itself does not:
   // it came out too large inside any triangle that spans a range of depths.
   fog_depth = vpos.w;
#endif
	vpos.xy *= vpos.w;
	// (a z beyond fZ_max is at the nearest depth, not clipped)
	vpos.z = min(depth_scale.x * depth_scale.w + depth_scale.y * vpos.w, vpos.w);
	gl_Position = vpos;
}
)";

const char* PixelPipelineShader =
R"(%s
#define TARGET_GL %s

#define cp_AlphaTest %d
#define pp_ClipInside %d
#define pp_UseAlpha %d
#define pp_Texture %d
#define pp_IgnoreTexA %d
#define pp_ShadInstr %d
#define pp_Offset %d
#define pp_FogCtrl %d
#define pp_Gouraud %d
#define pp_BumpMap %d
#define FogClamping %d
#define pp_TriLinear %d
#define pp_Palette %d
#define pp_Shadowed %d

#define PI 3.1415926

/* The general program (glprogram_cache.h): one for each pp_Gouraud, the
 * other bits of a polygon's key (GetProgram()) read from a uniform. The
 * code below is the same for both, so what one draws the other draws. */
#ifdef GENERAL_PROGRAM
uniform highp int key_bits;
#undef cp_AlphaTest
#undef pp_ClipInside
#undef pp_UseAlpha
#undef pp_Texture
#undef pp_IgnoreTexA
#undef pp_ShadInstr
#undef pp_Offset
#undef pp_FogCtrl
#undef pp_BumpMap
#undef FogClamping
#undef pp_TriLinear
#undef pp_Palette
#undef pp_Shadowed
#define pp_ClipInside ((key_bits >> 15) & 1)
#define cp_AlphaTest ((key_bits >> 14) & 1)
#define pp_Texture ((key_bits >> 13) & 1)
#define pp_UseAlpha ((key_bits >> 12) & 1)
#define pp_IgnoreTexA ((key_bits >> 11) & 1)
#define pp_ShadInstr ((key_bits >> 9) & 3)
#define pp_Offset ((key_bits >> 8) & 1)
#define pp_FogCtrl ((key_bits >> 6) & 3)
#define pp_BumpMap ((key_bits >> 4) & 1)
#define FogClamping ((key_bits >> 3) & 1)
#define pp_TriLinear ((key_bits >> 2) & 1)
#define pp_Palette ((key_bits >> 1) & 1)
#define pp_Shadowed (key_bits & 1)
#endif

#define GLES2 0
#define GLES3 1
#define GL2 2
#define GL3 3

#if TARGET_GL == GL2
#define highp
#define lowp
#define mediump
#endif
#if TARGET_GL == GLES3
out highp vec4 FragColor;
#define gl_FragColor FragColor
#define FOG_CHANNEL a
#elif TARGET_GL == GL3
out highp vec4 FragColor;
#define gl_FragColor FragColor
#define FOG_CHANNEL r
#else
#define in varying
#define texture texture2D
#define FOG_CHANNEL a
#endif

#if TARGET_GL == GL3 || TARGET_GL == GLES3
#if pp_Gouraud == 0
#define INTERPOLATION flat
#else
#define INTERPOLATION smooth
#endif
#else
#define INTERPOLATION
#endif

/* Shader program params*/
/* gles has no alpha test stage, so its emulated on the shader */
uniform highp float cp_AlphaTestValue;
uniform highp vec4 pp_ClipTest;
uniform mediump vec3 sp_FOG_COL_RAM,sp_FOG_COL_VERT;
uniform highp float sp_FOG_DENSITY;
uniform mediump sampler2D tex,fog_table;
uniform lowp float trilinear_alpha;
uniform mediump vec4 fog_clamp_min;
uniform mediump vec4 fog_clamp_max;
uniform mediump sampler2D palette;
uniform mediump int palette_index;
#if TARGET_GL != GLES2
// The depth prepass of sorted translucent polygons (rend/sorter.h): its least alpha, 0 for the colours
uniform highp float prepass_alpha;
#endif
uniform lowp float shade_scale_factor;

/* Precision. The colours that come in are eight bits each, so many 255ths.
 * lowp need not hold those: it may be a number of 256ths and nothing finer,
 * and no 255th but 0 and 1 is one of them - a colour, a texel, the fog
 * colour or a clamp limit kept in lowp can be half a step out before
 * anything is done with it, and comes out a step out about as often as
 * not. Nor need lowp reach past 2, and a texel times 255, which is how the
 * fog table and a palette index are read, is done in the precision of the
 * texel: a sampler is lowp unless it says otherwise. So colours, samplers
 * and what is worked out from them (texture times colour, plus offset, into
 * fog) are mediump, which has ten bits at the least. (Only what is a number
 * of quarters or of 256ths as it comes stays lowp.) Texture coordinates are
 * highp: in mediump, which may be a 16-bit float, a coordinate that repeats
 * a large texture a few times no longer says which texel. The clip
 * rectangle is in pixels and was lowp, which need not reach past 2. */
/* Vertex input*/
INTERPOLATION in mediump vec4 vtx_base;
INTERPOLATION in mediump vec4 vtx_offs;
in highp vec2 vtx_uv;
#if TARGET_GL == GLES2
in highp float fog_depth;
#endif

// The fog coefficient for a depth, worked out the way the PowerVR2 does it.
// The depth times the fog density, between 1 and just under 256, is taken
// apart as a float: its exponent and the top four bits of its mantissa
// pick one of the 128 table entries, and the next eight bits of mantissa
// blend that entry with the following one. The blend is done in whole
// numbers out of 256 and so is the result, which is returned over 256.
// Splitting the float is done by comparing and halving, which is exact
// and costs less than the log2() and pow() this used to take.
highp float fog_mode2(highp float w)
{
#if TARGET_GL == GLES2
	// fog_depth is 1/depth, the one thing a varying carries across a triangle exactly
	highp float z = sp_FOG_DENSITY / fog_depth;
#else
	highp float z = w * sp_FOG_DENSITY;
#endif
	// Not clamp(): where the depth is infinite and the density zero their
	// product is not a number, and that has to come out as 1 like anything
	// else below 1. The texture lookup this used to end in swallowed it.
	if (!(z >= 1.0))
		z = 1.0;
	if (z > 255.9999)
		z = 255.9999;
	highp float e = 0.0;
	if (z >= 16.0) { z *= 0.0625; e = 4.0; }
	if (z >= 4.0) { z *= 0.25; e += 2.0; }
	if (z >= 2.0) { z *= 0.5; e += 1.0; }
	highp float t = (z - 1.0) * 16.0;
	highp float i = floor(t);
	highp float blend = floor((t - i) * 256.0);
	highp float u = (e * 16.0 + i + 0.5) / 128.0;
	// The table is two rows: the entry's own value under 0.75, the next entry's under 0.25
	highp float next = floor(texture(fog_table, vec2(u, 0.25)).FOG_CHANNEL * 255.0 + 0.5);
	highp float cur = floor(texture(fog_table, vec2(u, 0.75)).FOG_CHANNEL * 255.0 + 0.5);
	return floor((next * blend + cur * (255.0 - blend)) / 256.0) / 256.0;
}

highp vec4 fog_clamp(highp vec4 col)
{
	if (FogClamping == 1)
		return clamp(col, fog_clamp_min, fog_clamp_max);
	return col;
}

// The palette is a row of 1024 colours, and a colour is read at the middle
// of its texel. (It used to be read at index/1023: the first few a
// thousandth of a texel from the one before, the last at the very end of
// the row.)
mediump vec4 palettePixel(highp vec2 coords)
{
	highp float index = texture(tex, coords).FOG_CHANNEL;
	highp vec2 c = vec2((index * 255.0 + float(palette_index) + 0.5) / 1024.0, 0.5);
	return texture(palette, c);
}

void main()
{
	// Clip inside the box
	if (pp_ClipInside==-1)
	{
		if (gl_FragCoord.x >= pp_ClipTest.x && gl_FragCoord.x <= pp_ClipTest.z
				&& gl_FragCoord.y >= pp_ClipTest.y && gl_FragCoord.y <= pp_ClipTest.w)
			discard;
	}
	
	mediump vec4 color=vtx_base;
	if (pp_UseAlpha==0)
		color.a=1.0;
	mediump vec4 offset = vtx_offs;
	if (pp_Shadowed == 1)
	{
		// A pixel inside a modifier volume: what the polygon is shaded with,
		// its base and offset colours, is scaled before the texture is
		// combined with them. The texture itself is not.
		color.rgb *= shade_scale_factor;
		offset = vec4(vtx_offs.rgb * shade_scale_factor, vtx_offs.a);
	}
	if (pp_Texture==1)
	{
		mediump vec4 texcol;
		if (pp_Palette == 0)
			texcol = texture(tex, vtx_uv);
		else
			texcol = palettePixel(vtx_uv);
		
		if (pp_BumpMap == 1)
		{
			highp float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;
			highp float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;
			texcol.a = clamp(offset.a + offset.r * sin(s) + offset.g * cos(s) * cos(r - 2.0 * PI * offset.b), 0.0, 1.0);
			texcol.rgb = vec3(1.0, 1.0, 1.0);	
		}
		else if (pp_IgnoreTexA==1)
			texcol.a=1.0;	
		if (pp_ShadInstr==0)
		{
			color=texcol;
		}
		if (pp_ShadInstr==1)
		{
			color.rgb*=texcol.rgb;
			color.a=texcol.a;
		}
		if (pp_ShadInstr==2)
		{
			color.rgb=mix(color.rgb,texcol.rgb,texcol.a);
		}
		if (pp_ShadInstr==3)
		{
			color*=texcol;
		}
		
		if (pp_Offset==1 && pp_BumpMap == 0)
		{
			color.rgb+=offset.rgb;
		}
	}
	
	color = fog_clamp(color);
	
	// Look-up table mode 2: the fog colour takes the place of the pixel's
	// colour and the fog coefficient the place of its alpha. This is a mode
	// of the fog unit, which comes after the texture has been combined in,
	// so it is the combined colour that is replaced: a textured polygon
	// comes out in plain fog colour. It used to be done to the base colour,
	// before texturing, and the texture was then applied on top.
	if (pp_FogCtrl == 3)
		color = vec4(sp_FOG_COL_RAM.rgb, fog_mode2(gl_FragCoord.w) * (256.0 / 255.0));
	if (pp_FogCtrl == 0)
	{
		highp float fog = fog_mode2(gl_FragCoord.w);
		color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_RAM.rgb * fog;
	}
	if (pp_FogCtrl == 1 && pp_Offset==1 && pp_BumpMap == 0)
	{
		highp float fog = offset.a * (255.0 / 256.0);
		color.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_VERT.rgb * fog;
	}
	
	if (pp_TriLinear == 1)
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
#if TARGET_GL != GLES2
	// (the sorted translucent depth prepass: rend/sorter.h; the depth itself is the position's)
	if (color.a < prepass_alpha)
		discard;
#endif
	gl_FragColor =color;
}
)";

static const char* ModifierVolumeShader = 
R"(%s
#define TARGET_GL %s

#define GLES2 0
#define GLES3 1
#define GL2 2
#define GL3 3

#if TARGET_GL == GL2
#define highp
#define lowp
#define mediump
#endif
#if TARGET_GL != GLES2 && TARGET_GL != GL2
out highp vec4 FragColor;
#define gl_FragColor FragColor
#endif

uniform lowp float sp_ShaderColor;
// (the depth is the position's; the colour is the final pass's tint)
void main()
{
   gl_FragColor=vec4(0.0, 0.0, 0.0, sp_ShaderColor);
}
)";


int screen_width  = 640;
int screen_height = 480;
GLuint fogTextureId;
GLuint paletteTextureId;

xform ViewportMatrix = { 1.f, 1.f, 1.f, 0.f, 0.f, 0.f };

/* (tests: every polygon drawn with the general programs, to see they draw
 * what the polygons' own ones do) */
#ifdef GL_PROGRAM_TEST_GENERAL
static const bool PipelineTestGeneral = true;
#else
static const bool PipelineTestGeneral = false;
#endif

/* A polygon's key: what its program is made for. (Bit 5 is pp_Gouraud,
 * the one the general programs do not read from it.) */
static u32 pipeline_key(bool cp_AlphaTest, bool pp_InsideClipping,
		bool pp_Texture, bool pp_UseAlpha, bool pp_IgnoreTexA, u32 pp_ShadInstr, bool pp_Offset,
		u32 pp_FogCtrl, bool pp_Gouraud, bool pp_BumpMap, bool fog_clamping, bool trilinear,
		bool palette, bool shadowed)
{
	u32 rv=0;

	rv |= pp_InsideClipping;
	rv<<=1; rv|=cp_AlphaTest;
	rv<<=1; rv|=pp_Texture;
	rv<<=1; rv|=pp_UseAlpha;
	rv<<=1; rv|=pp_IgnoreTexA;
	rv<<=2; rv|=pp_ShadInstr;
	rv<<=1; rv|=pp_Offset;
	rv<<=2; rv|=pp_FogCtrl;
	rv<<=1; rv|=pp_Gouraud;
	rv<<=1; rv|=pp_BumpMap;
	rv<<=1; rv|=fog_clamping;
	rv<<=1; rv|=trilinear;
	rv<<=1; rv|=palette;
	rv<<=1; rv|=shadowed;
	return rv;
}

static void pipeline_from_key(PipelineShader *s, u32 key)
{
	s->key = key;
	s->pp_InsideClipping = (key >> 15) & 1;
	s->cp_AlphaTest = (key >> 14) & 1;
	s->pp_Texture = (key >> 13) & 1;
	s->pp_UseAlpha = (key >> 12) & 1;
	s->pp_IgnoreTexA = (key >> 11) & 1;
	s->pp_ShadInstr = (key >> 9) & 3;
	s->pp_Offset = (key >> 8) & 1;
	s->pp_FogCtrl = (key >> 6) & 3;
	s->pp_Gouraud = (key >> 5) & 1;
	s->pp_BumpMap = (key >> 4) & 1;
	s->fog_clamping = (key >> 3) & 1;
	s->trilinear = (key >> 2) & 1;
	s->palette = (key >> 1) & 1;
	s->shadowed = key & 1;
}

// A program's depth scale block (DEPTH_UBO in the vertex shader) given the renderer's uniform buffer
void gl_bind_depth_ubo(GLuint program)
{
#ifndef HAVE_OPENGLES2
	if (gl.depth_ubo == 0)
		return;
	const GLuint block = glGetUniformBlockIndex(program, "DepthScaleBlock");
	if (block != GL_INVALID_INDEX)
		glUniformBlockBinding(program, block, DEPTH_UBO_BINDING);
#endif
}

/* A polygon's program asked of the driver (glprogram_cache.h): its own, or
 * (@general) the general one of its pp_Gouraud */
static GLuint make_pipeline_program(const PipelineShader *s, bool general, bool background)
{
	char vshader[8192];
	int rc = snprintf(vshader, sizeof(vshader), VertexShaderSource, gl.glsl_version_header, gl.gl_version, s->pp_Gouraud, gl.depth_ubo != 0);
	verify(rc + 1 <= (int)sizeof(vshader));

	char header[128];
	snprintf(header, sizeof(header), "%s\n#define GENERAL_PROGRAM", gl.glsl_version_header);
	char pshader[16384];
	rc = snprintf(pshader, sizeof(pshader), PixelPipelineShader, general ? header : gl.glsl_version_header, gl.gl_version,
                s->cp_AlphaTest,s->pp_InsideClipping,s->pp_UseAlpha,
                s->pp_Texture,s->pp_IgnoreTexA,s->pp_ShadInstr,s->pp_Offset,s->pp_FogCtrl, s->pp_Gouraud, s->pp_BumpMap,
				s->fog_clamping, s->trilinear, s->palette, s->shadowed);
	verify(rc + 1 <= (int)sizeof(pshader));

	return glprog_make(vshader, pshader, background);
}

/* Its uniforms found, the samplers and this frame's uniforms given (the
 * program bound). A general program has all of them. */
static void pipeline_locations(PipelineShader *s, bool general)
{
	glcache.UseProgram(s->program);

	//setup texture 0 as the input for the shader
	GLint gu = glGetUniformLocation(s->program, "tex");
	if (s->pp_Texture==1 || general)
		glUniform1i(gu,0);

	//get the uniform locations
	s->depth_scale      = glGetUniformLocation(s->program, "depth_scale");
	gl_bind_depth_ubo(s->program);

	s->pp_ClipTest      = glGetUniformLocation(s->program, "pp_ClipTest");

	s->sp_FOG_DENSITY   = glGetUniformLocation(s->program, "sp_FOG_DENSITY");

	s->cp_AlphaTestValue= glGetUniformLocation(s->program, "cp_AlphaTestValue");

	//FOG_COL_RAM,FOG_COL_VERT,FOG_DENSITY;
	if ((s->pp_FogCtrl==1 && s->pp_Texture==1) || general)
		s->sp_FOG_COL_VERT=glGetUniformLocation(s->program, "sp_FOG_COL_VERT");
	else
		s->sp_FOG_COL_VERT=-1;
	if (s->pp_FogCtrl==0 || s->pp_FogCtrl==3 || general)
	{
		s->sp_FOG_COL_RAM=glGetUniformLocation(s->program, "sp_FOG_COL_RAM");
	}
	else
	{
		s->sp_FOG_COL_RAM=-1;
	}
	// Setup texture 1 as the fog table
	gu = glGetUniformLocation(s->program, "fog_table");
	if (gu != -1)
		glUniform1i(gu, 1);
	// And texture 2 as palette
	gu = glGetUniformLocation(s->program, "palette");
	if (gu != -1)
		glUniform1i(gu, 2);
	s->palette_index = glGetUniformLocation(s->program, "palette_index");

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
	s->shade_scale_factor = glGetUniformLocation(s->program, "shade_scale_factor");
	s->prepass_alpha = glGetUniformLocation(s->program, "prepass_alpha");
	s->key_bits = general ? glGetUniformLocation(s->program, "key_bits") : -1;
	s->key_now = ~0u;

	ShaderUniforms.Set(s);
	s->uniforms_set = gl.uniforms_frame;
}

bool CompilePipelineShader(	PipelineShader* s)
{
	s->program = make_pipeline_program(s, false, false);
	if (glprog_failed())
		die("shader compile fail\n");
	pipeline_locations(s, false);

	return glIsProgram(s->program)==GL_TRUE;
}

// A polygon drawn with the general program @g until its own is there
static void use_general(PipelineShader *s, PipelineShader *g)
{
	s->program = g->program;
	s->depth_scale = g->depth_scale;
	s->pp_ClipTest = g->pp_ClipTest;
	s->cp_AlphaTestValue = g->cp_AlphaTestValue;
	s->sp_FOG_COL_RAM = g->sp_FOG_COL_RAM;
	s->sp_FOG_COL_VERT = g->sp_FOG_COL_VERT;
	s->sp_FOG_DENSITY = g->sp_FOG_DENSITY;
	s->trilinear_alpha = g->trilinear_alpha;
	s->fog_clamp_min = g->fog_clamp_min;
	s->fog_clamp_max = g->fog_clamp_max;
	s->normal_matrix = g->normal_matrix;
	s->palette_index = g->palette_index;
	s->shade_scale_factor = g->shade_scale_factor;
	s->prepass_alpha = g->prepass_alpha;
	s->key_bits = g->key_bits;
	s->general = g;
}

/* A key first drawn (in a frame): drawn with the general program, its own
 * asked for in the background where the driver does that (else that is it
 * for this run: the key is kept, and its program made at the start of the
 * next); without a general program, made now as ever */
static void pipeline_first(PipelineShader *s, u32 key)
{
	pipeline_from_key(s, key);
	glprog_note(GLPROG_GLES, key);
	PipelineShader *g = &gl.general[s->pp_Gouraud];
	if (g->program == 0)
	{
		CompilePipelineShader(s);
		return;
	}
	use_general(s, g);
	glprog_general_used();
	if (glprog_background() && !PipelineTestGeneral)
	{
		s->pending = make_pipeline_program(s, false, true);
		s->polled = gl.uniforms_frame;
	}
}

// (once a frame) whether its own program is there now
static void pipeline_poll(PipelineShader *s)
{
	s->polled = gl.uniforms_frame;
	switch (glprog_poll(s->pending))
	{
	case GLPROG_PENDING:
		break;
	case GLPROG_READY:
		s->program = s->pending;
		s->pending = 0;
		s->general = nullptr;
		pipeline_locations(s, false);
		break;
	default:
		WARN_LOG(RENDERER, "OpenGL: the program of key %x did not link; the general one draws it", s->key);
		glDeleteProgram(s->pending);
		s->pending = 0;
		break;
	}
}

void UsePipelineShader(PipelineShader *s)
{
	glcache.UseProgram(s->program);
	PipelineShader *owner = s->general != nullptr ? s->general : s;
	if (owner->uniforms_set != gl.uniforms_frame)
	{
		owner->uniforms_set = gl.uniforms_frame;
		ShaderUniforms.Set(owner);
	}
	if (s->general != nullptr && owner->key_now != s->key)
	{
		owner->key_now = s->key;
		glUniform1i(owner->key_bits, (GLint)s->key);
	}
}

/* The keys more than one game draws with (glprogram_cache.h), of Jet Grind
 * Radio, Dead or Alive 2, Sega Rally 2 and Virtua Fighter 4: made at the
 * start with those of earlier runs. (The general program draws the
 * others the first time.) */
static const u16 CommonKeys[] = {
	0x00a0,	// Gouraud, untextured, no alpha, no fog
	0x0aa0,	// ...the same, with texture bits a polygon without a texture leaves
	0x37a0,	// Gouraud, textured, alpha, modulated alpha, offset, no fog
};

/* At the renderer's start: the general programs, and the programs of the
 * keys of earlier runs and the common ones, all asked of the driver before
 * any is waited for */
static void gl_make_programs()
{
	for (int g = 0; g < 2; g++)
	{
		PipelineShader *s = &gl.general[g];
		memset(s, 0, sizeof(*s));
		s->pp_Gouraud = g;
		// (OpenGL ES 2 and OpenGL 2: none; the shaders' integers are 3's)
		if (gl.gl_major >= 3)
			s->program = make_pipeline_program(s, true, true);
	}
	u32 count = 0;
	const u32 *known = glprog_known(GLPROG_GLES, &count);
	const u32 most = count + (u32)ARRAY_SIZE(CommonKeys) + 3;
	std::vector<u32> keys;
	keys.reserve(most);
	for (u32 i = 0; i < count; i++)
		if (known[i] <= 0xFFFF)
			keys.push_back(known[i]);
	for (u16 k : CommonKeys)
		keys.push_back(k);
	// (the framebuffer's, the VMU screens' and the crosshairs': drawn with DrawQuad())
	keys.push_back(pipeline_key(false, false, true, false, true, 0, false, 2, false, false, false, false, false, false));
	keys.push_back(pipeline_key(false, false, true, true, false, 0, false, 2, false, false, false, false, false, false));
	if (keys.size() > 1024)
		keys.resize(1024);
	if (!PipelineTestGeneral)
		for (u32 key : keys)
		{
			PipelineShader *s = &gl.shaders[key];
			if (s->program != 0 || s->pending != 0)
				continue;
			pipeline_from_key(s, key);
			s->pending = make_pipeline_program(s, false, true);
		}
	// now waited for
	for (int g = 0; g < 2; g++)
	{
		PipelineShader *s = &gl.general[g];
		if (s->program == 0)
			continue;
		if (glprog_wait(s->program))
			pipeline_locations(s, true);
		else
		{
			WARN_LOG(RENDERER, "OpenGL: no general program (pp_Gouraud %d)", g);
			glDeleteProgram(s->program);
			s->program = 0;
		}
	}
	for (auto it = gl.shaders.begin(); it != gl.shaders.end(); )
	{
		PipelineShader *s = &it->second;
		if (s->pending != 0 && s->program == 0)
		{
			const GLuint p = s->pending;
			s->pending = 0;
			if (glprog_wait(p))
			{
				s->program = p;
				pipeline_locations(s, false);
			}
			else
			{
				WARN_LOG(RENDERER, "OpenGL: the program of key %x did not link", s->key);
				glDeleteProgram(p);
				it = gl.shaders.erase(it);
				continue;
			}
		}
		++it;
	}
}

PipelineShader *GetProgram(bool cp_AlphaTest, bool pp_InsideClipping,
		bool pp_Texture, bool pp_UseAlpha, bool pp_IgnoreTexA, u32 pp_ShadInstr, bool pp_Offset,
		u32 pp_FogCtrl, bool pp_Gouraud, bool pp_BumpMap, bool fog_clamping, bool trilinear,
		bool palette, bool shadowed)
{
	const u32 rv = pipeline_key(cp_AlphaTest, pp_InsideClipping, pp_Texture, pp_UseAlpha, pp_IgnoreTexA,
			pp_ShadInstr, pp_Offset, pp_FogCtrl, pp_Gouraud, pp_BumpMap, fog_clamping, trilinear, palette, shadowed);

	PipelineShader *shader = &gl.shaders[rv];
	if (shader->program == 0)
		pipeline_first(shader, rv);
	else if (shader->pending != 0 && shader->polled != gl.uniforms_frame)
		pipeline_poll(shader);

#ifdef RPI4_SET_UNIFORM_ATTRIBUTES_BUG
	// rpi 4 has a bug where it does not save uniform and attribute state with
	// the program, so they have to be reinit each time you reuse the program
	glcache.UseProgram(shader->program);
	if (shader->general != nullptr)
	{
		// (and the key, and this frame's uniforms, given again: UsePipelineShader())
		shader->general->key_now = ~0u;
		shader->general->uniforms_set = gl.uniforms_frame - 1;
	}
	//setup texture 0 as the input for the shader
	GLuint gu=glGetUniformLocation(shader->program, "tex");
	if (shader->pp_Texture==1)
		glUniform1i(gu,0);
	// Setup texture 1 as the fog table
	gu = glGetUniformLocation(shader->program, "fog_table");
	if (gu != -1)
		glUniform1i(gu, 1);
	ShaderUniforms.Set(shader);
#endif

	return shader;
}

/* What video memory there is to use (rend/vram_budget.h): what
 * GL_NVX_gpu_memory_info says is available (all of it counts as the
 * budget, none as used), or GL_ATI_meminfo's free texture memory; and
 * without either, a fixed figure small enough for any device that runs the
 * renderer. */
static void gl_query_vram_budget(bool nvx_meminfo, bool ati_meminfo)
{
	const GLenum nvx_total_available = 0x9048;	// GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX
	const GLenum nvx_current_available = 0x9049;	// GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX
	const GLenum ati_texture_free = 0x87FC;		// GL_TEXTURE_FREE_MEMORY_ATI
	GLint kib[4] = { 0, 0, 0, 0 };

	gl.vram_budget = VramBudget();
	if (nvx_meminfo)
	{
		GLint total = 0;
		glGetIntegerv(nvx_total_available, &total);
		glGetIntegerv(nvx_current_available, &kib[0]);
		if (glGetError() == GL_NO_ERROR && total > 0 && kib[0] > 0)
		{
			gl.vram_budget.budget_kib = (u32)total;
			gl.vram_budget.usage_kib = total > kib[0] ? (u32)(total - kib[0]) : 0;
			gl.vram_budget.exact = true;
		}
	}
	if (!gl.vram_budget.exact && ati_meminfo)
	{
		glGetIntegerv(ati_texture_free, kib);
		if (glGetError() == GL_NO_ERROR && kib[0] > 0)
		{
			gl.vram_budget.budget_kib = (u32)kib[0];
			gl.vram_budget.exact = true;
		}
	}
	if (!gl.vram_budget.exact)
		gl.vram_budget.budget_kib = (gl.is_gles ? 256u : 512u) << 10;
	vram_budget_log("OpenGL", gl.vram_budget);
}

void findGLVersion()
{
	gl.stencil_present = true;
	gl.index_type = GL_UNSIGNED_INT;

	while (true)
		if (glGetError() == GL_NO_ERROR)
			break;
	glGetIntegerv(GL_MAJOR_VERSION, &gl.gl_major);
	if (glGetError() == GL_INVALID_ENUM)
		gl.gl_major = 2;
	else
		glGetIntegerv(GL_MINOR_VERSION, &gl.gl_minor);

	const char *version = (const char *)glGetString(GL_VERSION);
	NOTICE_LOG(RENDERER, "OpenGL version: %s", version);
	if (!strncmp(version, "OpenGL ES", 9))
	{
		gl.is_gles = true;
		if (gl.gl_major >= 3)
		{
			gl.gl_version = "GLES3";
			gl.glsl_version_header = "#version 300 es";
		}
		else
		{
			gl.gl_version = "GLES2";
			gl.glsl_version_header = "";
			gl.index_type = GL_UNSIGNED_SHORT;
		}
		gl.single_channel_format = GL_ALPHA;

		GLint stencilBits = 0;
		glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
		if (stencilBits == 0)
			gl.stencil_present = false;
	}
	else
	{
		gl.is_gles = false;
		if (gl.gl_major >= 3)
		{
			gl.gl_version = "GL3";
			gl.glsl_version_header = "#version 130";
			gl.single_channel_format = GL_RED;
		}
		else
		{
			gl.gl_version = "GL2";
			gl.glsl_version_header = "#version 120";
			gl.single_channel_format = GL_ALPHA;
		}
	}
	gl.max_anisotropy = 1.f;
	bool nvx_meminfo = false, ati_meminfo = false;
	/* The depth (rend/depth_scale.h): a 32-bit float depth buffer where
	 * this renderer makes its own (OpenGL 3.0, OpenGL ES 3.0; the frame
	 * buffer the frontend gives has what it gives), the depth scale in a
	 * uniform buffer where there are any (OpenGL 3.1 or its extension,
	 * OpenGL ES 3.0: so that a frame decoded on the GPU can write it), and
	 * clip space z in [0,1] where the context has ARB_clip_control
	 * (OpenGL 4.5's), which keeps a float depth buffer's precision */
	bool ubo = false, clip_control = false;
	gl.depth_format = gl.gl_major >= 3 ? GL_DEPTH32F_STENCIL8 : RARCH_GL_DEPTH24_STENCIL8;
#ifndef HAVE_OPENGLES2
	// (a build with the OpenGL ES 2 headers has no uniform buffer functions: the uniform, on any context)
	ubo = gl.is_gles ? gl.gl_major >= 3 : gl.gl_major * 10 + gl.gl_minor >= 31;
	// (by the extension, which every driver with it lists, 4.5's or not: the other path can then be tried by turning the extension off)
	if (gl.gl_major >= 3)
	{
		for (u32 i = 0; ; i++)
		{
			const char* extension = (const char *)glGetStringi(GL_EXTENSIONS, i);
			if (extension == nullptr)
				break;
			if (!strcmp(extension, "GL_EXT_texture_filter_anisotropic"))
				glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &gl.max_anisotropy);
			else if (!strcmp(extension, "GL_NVX_gpu_memory_info"))
				nvx_meminfo = true;
			else if (!strcmp(extension, "GL_ATI_meminfo"))
				ati_meminfo = true;
			else if (!strcmp(extension, "GL_ARB_uniform_buffer_object"))
				ubo = true;
			else if (!strcmp(extension, "GL_ARB_clip_control") && !gl.is_gles)
				clip_control = true;
		}
	}
	else
#endif
	{
		const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
		if (extensions != nullptr)
		{
			nvx_meminfo = strstr(extensions, "GL_NVX_gpu_memory_info") != nullptr;
			ati_meminfo = strstr(extensions, "GL_ATI_meminfo") != nullptr;
		}
	}
	gl.clipControl = nullptr;
	gl.copyBufferSubData = nullptr;
	{
		glsm_ctx_proc_address_t proc = { nullptr };
		if (glsm_ctl(GLSM_CTL_PROC_ADDRESS_GET, &proc) && proc.addr != nullptr)
		{
			if (clip_control)
				gl.clipControl = (void (APIENTRYP)(GLenum, GLenum))proc.addr("glClipControl");
			if (ubo)
				gl.copyBufferSubData = (void (APIENTRYP)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr))proc.addr("glCopyBufferSubData");
		}
	}
	if (gl.depth_ubo != 0)
		glDeleteBuffers(1, &gl.depth_ubo);
	gl.depth_ubo = 0;
	// (the copy is OpenGL 3.1's too; without it the uniform, and no frame decoded on the GPU: GlTaDecoder needs 4.3)
	if (ubo && gl.copyBufferSubData != nullptr)
	{
		glGenBuffers(1, &gl.depth_ubo);
		glBindBuffer(GL_UNIFORM_BUFFER, gl.depth_ubo);
		glBufferData(GL_UNIFORM_BUFFER, 16, nullptr, GL_DYNAMIC_DRAW);
		glBindBuffer(GL_UNIFORM_BUFFER, 0);
	}
	(void)clip_control;
	NOTICE_LOG(RENDERER, "OpenGL depth: %s depth buffers, the scale in a %s, clip space z in %s",
			gl.depth_format == GL_DEPTH32F_STENCIL8 ? "32-bit float" : "24-bit",
			gl.depth_ubo != 0 ? "uniform buffer" : "uniform", gl.clipControl != nullptr ? "[0,1]" : "[-1,1]");
	while (glGetError() != GL_NO_ERROR)
		;
	gl_query_vram_budget(nvx_meminfo, ati_meminfo);
}

GLuint gl_CompileShader(const char* shader,GLuint type)
{
	GLint result;
	GLint compile_log_len;
	GLuint rv=glCreateShader(type);
	glShaderSource(rv, 1,&shader, NULL);
	glCompileShader(rv);

	//lets see if it compiled ...
	glGetShaderiv(rv, GL_COMPILE_STATUS, &result);
	glGetShaderiv(rv, GL_INFO_LOG_LENGTH, &compile_log_len);

	if (!result && compile_log_len>0)
	{
		if (compile_log_len==0)
			compile_log_len=1;
		char* compile_log=(char*)malloc(compile_log_len);
		*compile_log=0;

		glGetShaderInfoLog(rv, compile_log_len, &compile_log_len, compile_log);
		WARN_LOG(RENDERER, "Shader: %s \n%s\n", result ? "compiled!" : "failed to compile", compile_log);

		free(compile_log);
	}

	return rv;
}

// (through the programs kept: glprogram_cache.h)
GLuint gl_CompileAndLink(const char* VertexShader, const char* FragmentShader)
{
	const GLuint program = glprog_make(VertexShader, FragmentShader);
	if (glprog_failed())
		die("shader compile fail\n");

	glcache.UseProgram(program);

	verify(glIsProgram(program));

	return program;
}

/*
GL|ES 2
Slower, smaller subset of gl2

*Optimisation notes*
Keep stuff in packed ints
Keep data as small as possible
Keep vertex programs as small as possible
The drivers more or less suck. Don't depend on dynamic allocation, or any 'complex' feature
as it is likely to be problematic/slow
Do we really want to enable striping joins?

*Design notes*
Follow same architecture as the d3d renderer for now
Render to texture, keep track of textures in GL memory
Direct flip to screen (no vlbank/fb emulation)
Do we really need a combining shader? it is needlessly expensive for openGL | ES
Render contexts
Free over time? we actually care about ram usage here?
Limit max resource size? for psp 48k verts worked just fine

FB:
Pixel clip, mapping

SPG/VO:
mapping

TA:
Tile clip

*/

static void gl_delete_shaders()
{
	for (const auto& it : gl.shaders)
	{
		// (one drawn with a general program has none of its own, but may have one coming)
		if (it.second.program != 0 && it.second.general == nullptr)
			glDeleteProgram(it.second.program);
		if (it.second.pending != 0)
			glDeleteProgram(it.second.pending);
	}
	gl.shaders.clear();
	for (PipelineShader& g : gl.general)
	{
		if (g.program != 0)
			glDeleteProgram(g.program);
		g.program = 0;
	}
	glDeleteProgram(gl.modvol_shader.program);
	gl.modvol_shader.program = 0;
}

static void own_target_term(void);	// (RenderFrame()'s framebuffer)

static void gl_term(void)
{
	glcache.DeleteTextures(ARRAY_SIZE(vmuTextureId), vmuTextureId);
	memset(vmuTextureId, 0, sizeof(vmuTextureId));
	glcache.DeleteTextures(ARRAY_SIZE(lightgunTextureId), lightgunTextureId);
	memset(lightgunTextureId, 0, sizeof(lightgunTextureId));

	glDeleteBuffers(1, &gl.vbo.geometry);
	glDeleteBuffers(1, &gl.vbo.modvols);
	glDeleteBuffers(1, &gl.vbo.idxs);
	glDeleteBuffers(1, &gl.vbo.idxs2);
	if (gl.depth_ubo != 0)
		glDeleteBuffers(1, &gl.depth_ubo);
	gl.depth_ubo = 0;
	glDeleteTextures(1, &fbTextureId);
	fbTextureId = 0;
	glDeleteTextures(1, &fogTextureId);
	fogTextureId = 0;
	glcache.DeleteTextures(1, &paletteTextureId);
	paletteTextureId = 0;
	own_target_term();

	gl_delete_shaders();
}

static bool gl_create_resources(void)
{
	/* create VBOs */
	glGenBuffers(1, &gl.vbo.geometry);
	glGenBuffers(1, &gl.vbo.modvols);
	glGenBuffers(1, &gl.vbo.idxs);
	glGenBuffers(1, &gl.vbo.idxs2);

   findGLVersion();
   glprog_open();

   char vshader[8192];
   sprintf(vshader, VertexShaderSource, gl.glsl_version_header, gl.gl_version, 1, gl.depth_ubo != 0);
   char fshader[8192];
	sprintf(fshader, ModifierVolumeShader, gl.glsl_version_header, gl.gl_version);

	gl.modvol_shader.program = gl_CompileAndLink(vshader, fshader);
	gl_bind_depth_ubo(gl.modvol_shader.program);
	gl.modvol_shader.normal_matrix  = glGetUniformLocation(gl.modvol_shader.program, "normal_matrix");
	gl.modvol_shader.depth_scale = glGetUniformLocation(gl.modvol_shader.program, "depth_scale");
	gl.modvol_shader.sp_ShaderColor = glGetUniformLocation(gl.modvol_shader.program, "sp_ShaderColor");

	return true;
}

void UpdateFogTexture(u8 *fog_table, GLenum texture_slot, GLint fog_image_format)
{
	/* The table is 256 bytes, so whether it needs uploading is decided by
	 * looking at it: build the texture's contents from the registers this
	 * frame is rendered with and upload them if they are not what the GPU
	 * already has.
	 *
	 * This used to go by a flag the register write set and the renderer
	 * cleared. With Threaded Rendering the two are different threads, and
	 * the write could come after this frame's copy of the registers was
	 * taken: the renderer then uploaded the old table, cleared the flag,
	 * and the new table was never uploaded. A game that rewrites the same
	 * table every frame also no longer uploads it every frame. */
	static u8 uploaded[256];
	u8 temp_tex_buffer[256];
	MakeFogTexture(temp_tex_buffer);

	if (fogTextureId != 0 && !memcmp(uploaded, temp_tex_buffer, sizeof(uploaded)))
		return;

	glActiveTexture(texture_slot);
	if (fogTextureId == 0)
	{
		fogTextureId = glcache.GenTexture();
		glcache.BindTexture(GL_TEXTURE_2D, fogTextureId);
		/* The shader reads single entries, at the middle of their texels,
		 * and blends them itself: nothing for the card to filter, and
		 * filtering, a card whose weights are not exact there lets the
		 * entry next to it in. */
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	else
		glcache.BindTexture(GL_TEXTURE_2D, fogTextureId);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, fog_image_format, 128, 2, 0, fog_image_format, GL_UNSIGNED_BYTE, temp_tex_buffer);
	glCheck();
	memcpy(uploaded, temp_tex_buffer, sizeof(uploaded));

	glActiveTexture(GL_TEXTURE0);
}

void UpdatePaletteTexture(GLenum texture_slot)
{
	glActiveTexture(texture_slot);
	if (paletteTextureId == 0)
	{
		paletteTextureId = glcache.GenTexture();
		glcache.BindTexture(GL_TEXTURE_2D, paletteTextureId);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	else
		glcache.BindTexture(GL_TEXTURE_2D, paletteTextureId);

   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1024, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, palette32_ram);
	glCheck();

	glActiveTexture(GL_TEXTURE0);
}

void DrawVmuTexture(u8 vmu_screen_number);
void DrawGunCrosshair(u8 port);

void vertex_buffer_unmap(void)
{
   glBindBuffer(GL_ARRAY_BUFFER, 0);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void DoCleanup() {
}

static void upload_vertex_indices()
{
	if (gl.index_type == GL_UNSIGNED_SHORT)
	{
		static bool overrun;
		static List<u16> short_idx;
		if (short_idx.daty != NULL)
			short_idx.Free();
		short_idx.Init(pvrrc.idx.used(), &overrun, NULL);
		for (u32 *p = pvrrc.idx.head(); p < pvrrc.idx.LastPtr(0); p++)
			*(short_idx.Append()) = *p;
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, short_idx.bytes(), short_idx.head(), GL_STREAM_DRAW);
	}
	else
		glBufferData(GL_ELEMENT_ARRAY_BUFFER,pvrrc.idx.bytes(),pvrrc.idx.head(),GL_STREAM_DRAW);
}

/* The picture of a frame for the screen, drawn into a framebuffer of the
 * renderer's own and copied to the frontend's at the end.
 *
 * The depth is 1/w, scaled by 1/fZ_max (rend/depth_scale.h), and needs a
 * float depth buffer: a frame whose fZ_max is 100000 (Virtua Fighter 4's,
 * which has a vertex that far) puts its sky, at a 1/w of 0.003, at a depth
 * of 3e-8 - below the step of a 24-bit depth buffer, where it is the same
 * depth as the background plane behind it, and its depth test, greater,
 * fails: the sky was not drawn, and a wedge of the background plane's
 * colour showed through it, on one frame of the fade to white. The
 * frontend's framebuffer has the depth buffer it has (RetroArch's: 24
 * bits), so this renderer draws into one with gl.depth_format's - 32-bit
 * float, as the per-pixel renderer and Vulkan's have - wherever there is
 * one (OpenGL 3, OpenGL ES 3). It is made when the size of the picture
 * changes, not in a frame, and starts out as what the frontend's has (the
 * picture so far: a frame need not clear it). */
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058			/* (OpenGL ES 3's, under OpenGL ES 2's headers) */
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
static GLuint own_fbo, own_color, own_depth;
static int own_w, own_h;

static void own_target_term(void)
{
	if (own_fbo != 0)
		glDeleteFramebuffers(1, &own_fbo);
	if (own_color != 0)
		glDeleteRenderbuffers(1, &own_color);
	if (own_depth != 0)
		glDeleteRenderbuffers(1, &own_depth);
	own_fbo = own_color = own_depth = 0;
	own_w = own_h = 0;
}

/* The framebuffer the screen's picture is drawn in bound, w x h: true if
 * it is the renderer's own (own_target_present() copies it to the
 * frontend's), false if it is the frontend's */
static bool own_target_bind(int w, int h)
{
#if defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER)
	if (gl.depth_format != GL_DEPTH32F_STENCIL8 || w <= 0 || h <= 0)
		return false;
	const GLuint target = hw_render.get_current_framebuffer();
	if (own_fbo == 0 || own_w != w || own_h != h)
	{
		own_target_term();
		glGenRenderbuffers(1, &own_color);
		glBindRenderbuffer(RARCH_GL_RENDERBUFFER, own_color);
		glRenderbufferStorage(RARCH_GL_RENDERBUFFER, GL_RGBA8, w, h);
		glGenRenderbuffers(1, &own_depth);
		glBindRenderbuffer(RARCH_GL_RENDERBUFFER, own_depth);
		glRenderbufferStorage(RARCH_GL_RENDERBUFFER, gl.depth_format, w, h);
		glBindRenderbuffer(RARCH_GL_RENDERBUFFER, 0);
		glGenFramebuffers(1, &own_fbo);
		glBindFramebuffer(RARCH_GL_FRAMEBUFFER, own_fbo);
		glFramebufferRenderbuffer(RARCH_GL_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, RARCH_GL_RENDERBUFFER, own_color);
		glFramebufferRenderbuffer(RARCH_GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, RARCH_GL_RENDERBUFFER, own_depth);
		if (glCheckFramebufferStatus(RARCH_GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		{
			WARN_LOG(RENDERER, "OpenGL: no framebuffer with a float depth buffer: drawing into the frontend's");
			own_target_term();
			glBindFramebuffer(RARCH_GL_FRAMEBUFFER, target);
			return false;
		}
		own_w = w;
		own_h = h;
		glcache.Disable(GL_SCISSOR_TEST);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, own_fbo);
		glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, own_fbo);
	return true;
#else
	(void)w; (void)h;
	return false;
#endif
}

// The picture drawn in the renderer's own framebuffer to the frontend's, which is left bound
static void own_target_present(void)
{
#if defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER)
	const GLuint target = hw_render.get_current_framebuffer();
	glcache.Disable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, own_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
	glBlitFramebuffer(0, 0, own_w, own_h, 0, 0, own_w, own_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, target);
#endif
}

static bool RenderFrame(void)
{
	int vmu_screen_number = 0 ;
	bool own_target = false;
	int lightgun_port = 0 ;

	DoCleanup();

	bool is_rtt = pvrrc.isRTT;

	TransformMatrix<true> matrices(pvrrc);
	xform_to_mat4(&matrices.GetNormalMatrix(), ShaderUniforms.normal_mat);
	const xform& scissor_mat = matrices.GetScissorMatrix();
	ViewportMatrix = matrices.GetViewportMatrix();

	if (!is_rtt)
		gcflip = 0;
	else
		gcflip = 1;

	/* The depth is the position's (rend/depth_scale.h, and the vertex
	 * shader): a*S*z + b, with S = 1/fZ_max; a and b take the depth to the
	 * device's clip space - [0,1] with clip control, else [-1,1] - and undo
	 * what the matrix scales z by */
	{
		const float mul = 1.f / matrices.GetNormalMatrix().sz;
		ShaderUniforms.depth_coefs[0] = gl.clipControl != nullptr ? mul : 2.f * mul;
		ShaderUniforms.depth_coefs[1] = gl.clipControl != nullptr ? 0.f : -1.f;
		ShaderUniforms.depth_coefs[2] = 0;
		ShaderUniforms.depth_coefs[3] = pvr_depth_scale(pvrrc.fZ_max);
#ifndef HAVE_OPENGLES2
		if (gl.depth_ubo != 0)
		{
			/* ...in the uniform buffer, and for a frame decoded on the GPU,
			 * S from the decode's word of it, copied over the host's (the
			 * copy is ordered after the decode's kernels: GlTaDecoder) */
			glBindBuffer(GL_UNIFORM_BUFFER, gl.depth_ubo);
			glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(ShaderUniforms.depth_coefs), ShaderUniforms.depth_coefs);
			if (pvrrc.gpu)
			{
				glBindBuffer(GL_COPY_READ_BUFFER, glTaDecoder.Draw().frame);
				gl.copyBufferSubData(GL_COPY_READ_BUFFER, GL_UNIFORM_BUFFER, TAG_O_ZSCALE * 4, 12, 4);
				glBindBuffer(GL_COPY_READ_BUFFER, 0);
			}
			glBindBuffer(GL_UNIFORM_BUFFER, 0);
			glBindBufferBase(GL_UNIFORM_BUFFER, DEPTH_UBO_BINDING, gl.depth_ubo);
		}
#endif
	}
	if (gl.clipControl != nullptr)
		gl.clipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);

	//VERT and RAM fog color constants
	u8* fog_colvert_bgra = (u8*)&FOG_COL_VERT;
	u8* fog_colram_bgra = (u8*)&FOG_COL_RAM;
	ShaderUniforms.ps_FOG_COL_VERT[0] = fog_colvert_bgra[2] / 255.0f;
	ShaderUniforms.ps_FOG_COL_VERT[1] = fog_colvert_bgra[1] / 255.0f;
	ShaderUniforms.ps_FOG_COL_VERT[2] = fog_colvert_bgra[0] / 255.0f;

	ShaderUniforms.ps_FOG_COL_RAM[0] = fog_colram_bgra[2] / 255.0f;
	ShaderUniforms.ps_FOG_COL_RAM[1] = fog_colram_bgra[1] / 255.0f;
	ShaderUniforms.ps_FOG_COL_RAM[2] = fog_colram_bgra[0] / 255.0f;

	//Fog density constant
	u8* fog_density = (u8*)&FOG_DENSITY;
	float fog_den_mant = fog_density[1] / 128.0f;  //bit 7 -> x. bit, so [6:0] -> fraction -> /128
	s32 fog_den_exp = (s8)fog_density[0];
	ShaderUniforms.fog_den_float = fog_den_mant * powf(2.0f, fog_den_exp);

	ShaderUniforms.fog_clamp_min[0] = ((pvrrc.fog_clamp_min >> 16) & 0xFF) / 255.0f;
	ShaderUniforms.fog_clamp_min[1] = ((pvrrc.fog_clamp_min >> 8) & 0xFF) / 255.0f;
	ShaderUniforms.fog_clamp_min[2] = ((pvrrc.fog_clamp_min >> 0) & 0xFF) / 255.0f;
	ShaderUniforms.fog_clamp_min[3] = ((pvrrc.fog_clamp_min >> 24) & 0xFF) / 255.0f;

	ShaderUniforms.fog_clamp_max[0] = ((pvrrc.fog_clamp_max >> 16) & 0xFF) / 255.0f;
	ShaderUniforms.fog_clamp_max[1] = ((pvrrc.fog_clamp_max >> 8) & 0xFF) / 255.0f;
	ShaderUniforms.fog_clamp_max[2] = ((pvrrc.fog_clamp_max >> 0) & 0xFF) / 255.0f;
	ShaderUniforms.fog_clamp_max[3] = ((pvrrc.fog_clamp_max >> 24) & 0xFF) / 255.0f;


	UpdateFogTexture((u8 *)FOG_TABLE, GL_TEXTURE1, gl.single_channel_format);
	if (palette_updated)
	{
		UpdatePaletteTexture(GL_TEXTURE2);
		palette_updated = false;
	}

	glcache.UseProgram(gl.modvol_shader.program);

	glUniform4fv(gl.modvol_shader.depth_scale, 1, ShaderUniforms.depth_coefs);
	glUniformMatrix4fv(gl.modvol_shader.normal_matrix, 1, GL_FALSE, ShaderUniforms.normal_mat);

	/* A punch-through texel is kept when its alpha, eight bits, is at least
	 * the reference. The shader compares floats, and the two come to it by
	 * different roads - a texel's alpha by the graphics card's own division -
	 * so that one equal to the reference could come out a hair under it and
	 * be dropped. The reference is given half a step low: then any alpha
	 * that is a whole number of steps is on the right side by half a one. */
	ShaderUniforms.PT_ALPHA = ((PT_ALPHA_REF & 0xFF) - 0.5f) / 255.0f;
	ShaderUniforms.shade_scale_factor = FPU_SHAD_SCALE.scale_factor / 256.f;

	// (given to each program the first time it is bound in the frame: UsePipelineShader())
	gl.uniforms_frame++;

	//setup render target first
	if (is_rtt)
	{
		/* A pixel clip that ends before it starts holds no pixel: the chip
		 * writes nothing. (Its size would be negative here, and a texture
		 * that size is no texture at all.) */
		if (pvrrc.fb_X_CLIP.max < pvrrc.fb_X_CLIP.min || pvrrc.fb_Y_CLIP.max < pvrrc.fb_Y_CLIP.min
				|| FB_X_CLIP.max < FB_X_CLIP.min || FB_Y_CLIP.max < FB_Y_CLIP.min)
		{
			WARN_LOG(RENDERER, "Render to texture with an empty pixel clip: %d-%d, %d-%d",
					pvrrc.fb_X_CLIP.min, pvrrc.fb_X_CLIP.max, pvrrc.fb_Y_CLIP.min, pvrrc.fb_Y_CLIP.max);
			return false;
		}
		GLuint channels,format;
		switch(FB_W_CTRL.fb_packmode)
		{
			case 0: //0x0   0555 KRGB 16 bit  (default)	Bit 15 is the value of fb_kval[7].
				channels=GL_RGBA;
				format=GL_UNSIGNED_BYTE;
				break;

			case 1: //0x1   565 RGB 16 bit
				channels=GL_RGB;
				/* Five or six bits a channel is what the game asked for, and
				 * what the picture is cut down to when it goes to video
				 * memory. As a texture it does not have to lose them: eight
				 * bits a channel, as the Vulkan renderer keeps it. (Where a
				 * driver will not render to that, BindRTT() goes back to
				 * 5-6-5.) */
				format = GL_UNSIGNED_BYTE;
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
		DEBUG_LOG(RENDERER, "RTT packmode=%d stride=%d - %d,%d -> %d,%d\n", FB_W_CTRL.fb_packmode, FB_W_LINESTRIDE.stride * 8,
 				FB_X_CLIP.min, FB_Y_CLIP.min, FB_X_CLIP.max, FB_Y_CLIP.max);
		// (from the frame's first pixel: see TransformMatrix)
		BindRTT(FB_W_SOF1 & VRAM_MASK, FB_X_CLIP.max + 1, FB_Y_CLIP.max + 1, channels,format);
	}
	else
	{
		if (settings.rend.PowerVR2Filter && !pvrrc.isRenderFramebuffer)
			postProcessor.SelectFramebuffer();
		else
			own_target = own_target_bind(screen_width, screen_height);
		glViewport(0, 0, screen_width, screen_height);
		/* in a new context, the picture kept from the old one (rend/last_picture.h) */
		gl_restore_picture();
	}

	bool wide_screen_on = !is_rtt && settings.rend.WideScreen && !matrices.IsClipped();

	//Color is cleared by the background plane

	glcache.Disable(GL_SCISSOR_TEST);

	glcache.DepthMask(GL_TRUE);
	glClearDepth(0.0);
	glcache.StencilMask(0xFF); glCheck();
	glClearStencil(0);
	if (!is_rtt && pvrrc.clearFramebuffer)
	{
		// a framebuffer the game has not drawn to lately: nothing of the last picture belongs in it
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glcache.ClearColor(VO_BORDER_COL.Red / 255.f, VO_BORDER_COL.Green / 255.f, VO_BORDER_COL.Blue / 255.f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glCheck();
	}
	else
	{
		glClear(GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glCheck();
	}

	//move vertex to gpu

	if (!pvrrc.isRenderFramebuffer)
	{
		// (a frame decoded on the GPU is there already: DrawStrips() draws from the decode's buffers)
		if (!pvrrc.gpu)
		{
			//Main VBO
			glBindBuffer(GL_ARRAY_BUFFER, gl.vbo.geometry); glCheck();
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl.vbo.idxs); glCheck();

			glBufferData(GL_ARRAY_BUFFER,pvrrc.verts.bytes(),pvrrc.verts.head(),GL_STREAM_DRAW); glCheck();

			upload_vertex_indices();

			//Modvol VBO
			if (pvrrc.modtrig.used())
			{
				glBindBuffer(GL_ARRAY_BUFFER, gl.vbo.modvols); glCheck();
				glBufferData(GL_ARRAY_BUFFER,pvrrc.modtrig.bytes(),pvrrc.modtrig.head(),GL_STREAM_DRAW); glCheck();
			}
		}

		if (!wide_screen_on)
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

			ShaderUniforms.base_clipping.enabled = true;
			ShaderUniforms.base_clipping.x = (int)lroundf(min_x);
			ShaderUniforms.base_clipping.y = (int)lroundf(min_y);
			ShaderUniforms.base_clipping.width = (int)lroundf(width);
			ShaderUniforms.base_clipping.height = (int)lroundf(height);
			glcache.Scissor(ShaderUniforms.base_clipping.x, ShaderUniforms.base_clipping.y, ShaderUniforms.base_clipping.width, ShaderUniforms.base_clipping.height);
			glcache.Enable(GL_SCISSOR_TEST);
		}
		else
		{
			ShaderUniforms.base_clipping.enabled = false;
		}

		DrawStrips();
		if (settings.rend.PowerVR2Filter && !is_rtt)
			postProcessor.Render(hw_render.get_current_framebuffer());
	}
	else
	{
		glcache.ClearColor(0.f, 0.f, 0.f, 0.f);
		glClear(GL_COLOR_BUFFER_BIT);
		glBindBuffer(GL_ARRAY_BUFFER, gl.vbo.geometry); glCheck();
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl.vbo.idxs); glCheck();
		DrawFramebuffer();
	}

	/* VMU/Crosshair code - libretro-specific */
	if (!is_rtt)
	{
		if (settings.System == DC_PLATFORM_DREAMCAST)
		{
			for ( vmu_screen_number = 0 ; vmu_screen_number < 4 ; vmu_screen_number++)
				if ( vmu_screen_params[vmu_screen_number].vmu_screen_display )
					DrawVmuTexture(vmu_screen_number);
		}

		for ( lightgun_port = 0 ; lightgun_port < 4 ; lightgun_port++)
			DrawGunCrosshair(lightgun_port);
	}

	KillTex = false;

	if (own_target)
		own_target_present();
	if (is_rtt)
		ReadRTTBuffer();
	// (the frontend's own drawing has the context as it was)
	if (gl.clipControl != nullptr)
		gl.clipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);

	return !is_rtt;
}

void rend_set_fb_scale(float x, float y)
{
	fb_scale_x = x;
	fb_scale_y = y;
}

bool ProcessFrame(TA_context* ctx, u32 taFlags)
{
   if (KillTex)
   {
      TexCache.Clear();
      INFO_LOG(RENDERER, "Texture cache cleared");
   }

   glTaDecoder.BeginFrame();
   if (ctx->rend.isRenderFramebuffer)
		RenderFramebuffer();
	else
	{
		/* GPU-side TA processing: the header pass makes the frame ready to
		 * be drawn from the GPU's decode, in the full parse's place, and the
		 * decode is dispatched; or it is parsed as ever */
		bool rv;
		if (taFlags == 0 || !glTaDecoder.Ready() || !glTaDecoder.Prepare(ctx, taFlags, &rv))
			rv = ta_parse_vdrc(ctx);
		else if (rv)
			glTaDecoder.Record(ctx);
		if (!rv)
			return false;
		// the textures the frame's polygons asked for that are made on the GPU, before the draws
		gl_tex_flush();
	}
   TexCache.CollectCleanup();

   return !ctx->rend.Overrun;
}

struct glesrend : Renderer
{
   bool Init() override
   {
      if (!gl_create_resources())
         return false;

      glcache.EnableCache();
      /* A new context has dithering on, and a driver may then dither what
       * is drawn (to a target of less than eight bits a channel, as a rule).
       * Nothing here wants it: what the console dithers is dithered when
       * the picture is packed for video memory. */
      glDisable(GL_DITHER);

#ifdef HAVE_OPENGLES
      glHint(GL_GENERATE_MIPMAP_HINT, GL_FASTEST);
#endif

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
      // the textures' pool and the decoder, all of them now (gl_texdecoder.h)
      gl_tex_init(true);

      if (settings.rend.PowerVR2Filter)
      	postProcessor.Init();

      /* GPU-side TA processing, if it is on and the context has OpenGL 4.3:
       * here, when the renderer is set up, not in a frame */
      glTaDecoder.WantSort(true);
      glTaDecoder.Setup(ta_gpu_wanted());

      // the polygons' programs, so that none is made in a frame (glprogram_cache.h)
      gl_make_programs();
      glprog_save();
      glprog_started();

      return true;
   }
	void Resize(int w, int h) override
	{
		gl_carry_picture(screen_width, screen_height, w, h);
		screen_width=w;
		screen_height=h;
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
		glprog_log("per triangle");
		glprog_close();
		glTaDecoder.Term();
		postProcessor.Term();
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
      /* GPU-side TA processing: a sorted pass's translucent triangles
       * sorted from the frame's depths (TA_GPU_SORT); sorted by strip, the
       * polygons are sorted from the CPU's vertices, and a frame that may
       * have a sorted pass is parsed. The polygons drawn again for shadows
       * are those the GPU says reach into the volumes. */
      return ProcessFrame(ctx, TA_GPU_DRAW | TA_GPU_SHADOWS
            | (!settings.pvr.Emulation.AlphaSortMode && glTaDecoder.CanSort() ? TA_GPU_SORT : TA_GPU_NO_SORT));
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

	virtual u64 GetTexture(TSP tsp, TCW tcw, int area = 0) override {
		return gl_GetTexture(tsp, tcw, area);
	}

	// (TA processing switched on or off: between frames, the state bound)
	void OptionsChanged() override
	{
		glTaDecoder.Setup(ta_gpu_wanted());
	}
};

Renderer* rend_GLES2() { return new glesrend(); }
