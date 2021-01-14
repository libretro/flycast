#include <math.h>
#include <string.h>

#include <libretro.h>

#include "vita.h"
#include "rend/rend.h"
#include "rend/TexCache.h"

#include "hw/pvr/Renderer_if.h"
#include "hw/mem/_vmem.h"

#ifndef GL_RED
#define GL_RED                            0x1903
#endif
#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION                  0x821B
#endif
#ifndef GL_MINOR_VERSION
#define GL_MINOR_VERSION                  0x821C
#endif

uint16_t *gIndices;
float *gVertexBuffer;
uint16_t *gIndicesPtr;
float *gVertexBufferPtr;

extern uint32_t idx_incr, vtx_incr;

GLCache glcache;
gl_ctx gl;

struct ShaderUniforms_t ShaderUniforms;

u32 gcflip;

float fb_scale_x = 0.0f;
float fb_scale_y = 0.0f;
float scale_x, scale_y;

//Fragment and vertex shaders code

static const char* VertexShaderSource = R"(void main(
	float4 in_pos,
	fixed4 in_base,
	fixed4 in_offs,
	half2 in_uv,
	uniform float4x4 normal_matrix,
	fixed4 out vtx_base : COLOR0,
	fixed4 out vtx_offs : COLOR1,
	half2 out vtx_uv : TEXCOORD0,
	float4 out vpos : POSITION)
{
	vtx_base=in_base;
	vtx_offs=in_offs;
	vtx_uv=in_uv;
	vpos = normal_matrix * in_pos;
	
	vpos.w = 1.0 / vpos.z;
	vpos.z = vpos.w; 

	vpos.xy *= vpos.w; 
}
)";

const char* PixelPipelineShader =
R"(#define cp_AlphaTest %d
#define pp_ClipInside %d
#define pp_UseAlpha %d
#define pp_Texture %d
#define pp_IgnoreTexA %d
#define pp_ShadInstr %d
#define pp_Offset %d
#define pp_FogCtrl %d
#define pp_BumpMap %d
#define FogClamping %d
#define pp_TriLinear %d
#define pp_Palette %d

#define PI 3.1415926

fixed fog_mode2(float w, float density, sampler2D table)
{
	float z = clamp(w * density, 1.0, 255.9999);
	half exp_v = floor(log2(z));
	float m = z * 16.0 / pow(2.0, exp_v) - 16.0;
	half idx = floor(m) + exp_v * 16.0 + 0.5;
	float4 fog_coef = tex2D(table, float2(idx / 128.0, 0.75 - (m - floor(m)) / 2.0));
	return fog_coef.a;
}

float4 fog_clamp(float4 col, float4 clamp_min, float4 clamp_max)
{
#if FogClamping == 1
	return clamp(col, clamp_min, clamp_max);
#else
	return col;
#endif
}

void main(
	fixed4 vtx_base : COLOR0,
	fixed4 vtx_offs : COLOR1,
	half2 vtx_uv : TEXCOORD0,
	float4 coords : WPOS,
	uniform half cp_AlphaTestValue,
	uniform half4 pp_ClipTest,
	uniform half3 sp_FOG_COL_RAM,
	uniform half3 sp_FOG_COL_VERT,
	uniform float sp_FOG_DENSITY,
	uniform sampler2D tex : TEXUNIT0,
	uniform sampler2D fog_table : TEXUNIT1,
	uniform sampler2D palette : TEXUNIT2,
	uniform half trilinear_alpha,
	uniform half4 fog_clamp_min,
	uniform half4 fog_clamp_max,
	uniform int palette_index,
	float4 out frag_clr : COLOR,
	float out frag_depth : DEPTH
	)
{
	// Clip inside the box
	#if pp_ClipInside==-1
	if (coords.x >= pp_ClipTest.x && coords.x <= pp_ClipTest.z
			&& coords.y >= pp_ClipTest.y && coords.y <= pp_ClipTest.w)
		discard;
	#endif
	
	fixed4 color = vtx_base;
	#if pp_UseAlpha == 0
	color.a = 1.0f;
	#endif
	#if pp_FogCtrl == 3
	color = fixed4(sp_FOG_COL_RAM.rgb , fog_mode2(coords.w, sp_FOG_DENSITY, fog_table));
	#endif
	#if pp_Texture==1
	
		
		#if pp_Palette == 0
	fixed4 texcol = tex2D(tex, vtx_uv);
		#else
	int color_idx = int(floor(tex2D(tex, vtx_uv).a * 255.0 + 0.5)) + palette_index;
	float2 c = float2(mod(float(color_idx), 32.0) / 31.0, float(color_idx / 32) / 31.0);
	fixed4 texcol = tex2D(palette, c);
		#endif
		
		#if pp_BumpMap == 1
	float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;
	float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;
	texcol.a = clamp(vtx_offs.a + vtx_offs.r * sin(s) + vtx_offs.g * cos(s) * cos(r - 2.0 * PI * vtx_offs.b), 0.0, 1.0);
	texcol.rgb = fixed3(1.0, 1.0, 1.0);	
		#else
			#if pp_IgnoreTexA==1
	texcol.a=1.0;	
			#endif
			
			#if cp_AlphaTest == 1
	if (cp_AlphaTestValue > texcol.a)
		discard;
			#endif 
		#endif
		#if pp_ShadInstr==0
	color = texcol;
		#endif
		#if pp_ShadInstr==1
	color.rgb *= texcol.rgb;
	color.a = texcol.a;
		#endif
		#if pp_ShadInstr==2
	color.rgb = lerp(color.rgb,texcol.rgb,texcol.a);
		#endif
		#if  pp_ShadInstr==3
	color *= texcol;
		#endif
		
		#if pp_Offset==1 && pp_BumpMap == 0
	color.rgb += vtx_offs.rgb;
		#endif
	#endif
	
	color = fog_clamp(color, fog_clamp_min, fog_clamp_max);
	
	#if pp_FogCtrl == 0
	color.rgb = lerp(color.rgb, sp_FOG_COL_RAM.rgb, fog_mode2(coords.w, sp_FOG_DENSITY, fog_table)); 
	#endif
	#if pp_FogCtrl == 1 && pp_Offset == 1 && pp_BumpMap == 0
	color.rgb = lerp(color.rgb, sp_FOG_COL_VERT.rgb, vtx_offs.a);
	#endif
	
	#if pp_TriLinear == 1
	color *= trilinear_alpha;
	#endif
	
	#if cp_AlphaTest == 1
	color.a = 1.0;
	#endif 
	
	float w = coords.w * 100000.0;
	frag_depth = log2(1.0 + w) / 34.0;
	
	frag_clr = color;
}
)";

int screen_width  = 640;
int screen_height = 480;
GLuint fogTextureId;
GLuint paletteTextureId;

glm::mat4 ViewportMatrix;

PipelineShader *GetProgram(
      u32 cp_AlphaTest,
      u32 pp_InsideClipping,
      u32 pp_Texture,
      u32 pp_UseAlpha,
      u32 pp_IgnoreTexA,
      u32 pp_ShadInstr,
      u32 pp_Offset,
      u32 pp_FogCtrl,
      bool pp_Gouraud,
      bool pp_BumpMap,
      bool fog_clamping,
      bool trilinear,
	  bool palette)
{
	u32 rv=0;

	rv|=pp_InsideClipping;
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

	PipelineShader *shader = &gl.shaders[rv];
	if (shader->program == 0)
	{
		shader->cp_AlphaTest = cp_AlphaTest;
		shader->pp_InsideClipping = pp_InsideClipping;
		shader->pp_Texture = pp_Texture;
		shader->pp_UseAlpha = pp_UseAlpha;
		shader->pp_IgnoreTexA = pp_IgnoreTexA;
		shader->pp_ShadInstr = pp_ShadInstr;
		shader->pp_Offset = pp_Offset;
		shader->pp_FogCtrl = pp_FogCtrl;
		shader->pp_Gouraud = pp_Gouraud;
		shader->pp_BumpMap = pp_BumpMap;
		shader->fog_clamping = fog_clamping;
		shader->trilinear = trilinear;
		shader->palette = palette;
		CompilePipelineShader(shader);
	}
    glcache.UseProgram(shader->program);
    ShaderUniforms.Set(shader);

   return shader;
}

void findGLVersion()
{
   gl.stencil_present = true;
   gl.index_type = GL_UNSIGNED_SHORT;
   gl.gl_major = 2;
   gl.is_gles = true;
   gl.fog_image_format = GL_ALPHA;
   gl.max_anisotropy = 1.f;
}

uint32_t shader_idx = 0;
GLuint gl_CompileShader(const char* shader,GLuint type)
{
	GLint result;
	GLuint rv=glCreateShader(type);
	glShaderSource(rv, 1,&shader, NULL);
	glCompileShader(rv);

	//lets see if it compiled ...
	glGetShaderiv(rv, GL_COMPILE_STATUS, &result);
	
	if (!result)
		WARN_LOG(RENDERER, "Shader: failed to compile");

	return rv;
}

GLuint gl_CompileAndLink(const char* VertexShader, const char* FragmentShader)
{
	/* Create vertex/fragment shaders */
	GLuint vs      = gl_CompileShader(VertexShader ,GL_VERTEX_SHADER);
	GLuint ps      = gl_CompileShader(FragmentShader ,GL_FRAGMENT_SHADER);
	GLuint program = glCreateProgram();

	glAttachShader(program, vs);
	glAttachShader(program, ps);
	
	/* Bind vertex attribute to VBO inputs */
	vglBindPackedAttribLocation(program, "in_pos" ,           3, GL_FLOAT        ,                 0, sizeof(float) * 11);
	vglBindPackedAttribLocation(program, "in_base",           4, GL_UNSIGNED_BYTE, sizeof(float) * 3, sizeof(float) * 11);
	vglBindPackedAttribLocation(program, "in_offs",           4, GL_UNSIGNED_BYTE, sizeof(float) * 4, sizeof(float) * 11);
	vglBindPackedAttribLocation(program, "in_uv"  ,           2, GL_FLOAT        , sizeof(float) * 5, sizeof(float) * 11);
	
	glLinkProgram(program);
	
	glcache.UseProgram(program);

	return program;
}


bool CompilePipelineShader(PipelineShader *s)
{
   char vshader[8192];

   sprintf(vshader, VertexShaderSource);

	char pshader[8192];

   sprintf(pshader,PixelPipelineShader,
                s->cp_AlphaTest,s->pp_InsideClipping,s->pp_UseAlpha,
                s->pp_Texture,s->pp_IgnoreTexA,s->pp_ShadInstr,
				s->pp_Offset,s->pp_FogCtrl, s->pp_BumpMap, s->fog_clamping, s->trilinear, s->palette);

	s->program            = gl_CompileAndLink(vshader, pshader);

	//get the uniform locations
	
	s->pp_ClipTest = glGetUniformLocation(s->program, "pp_ClipTest");
	
	s->sp_FOG_DENSITY   = glGetUniformLocation(s->program, "sp_FOG_DENSITY");
	
	s->cp_AlphaTestValue= glGetUniformLocation(s->program, "cp_AlphaTestValue");

	//FOG_COL_RAM,FOG_COL_VERT,FOG_DENSITY;
	if (s->pp_FogCtrl==1 && s->pp_Texture==1)
		s->sp_FOG_COL_VERT = glGetUniformLocation(s->program, "sp_FOG_COL_VERT");
	else
		s->sp_FOG_COL_VERT = -1;
	
	if (s->pp_FogCtrl==0 || s->pp_FogCtrl==3) {
		s->sp_FOG_COL_RAM = glGetUniformLocation(s->program, "sp_FOG_COL_RAM");
		s->sp_FOG_DENSITY = glGetUniformLocation(s->program, "sp_FOG_DENSITY");
	} else {
		s->sp_FOG_COL_RAM = -1;
		s->sp_FOG_DENSITY = -1;
	}
	
	s->palette_index = glGetUniformLocation(s->program, "palette_index");
	
	if (s->trilinear)
		s->trilinear_alpha = glGetUniformLocation(s->program, "trilinear_alpha");
	else
		s->trilinear_alpha = -1;
   
	if (s->fog_clamping)
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

	ShaderUniforms.Set(s);

	return GL_TRUE;
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
		if (it.second.program != 0)
			glDeleteProgram(it.second.program);
	}
	gl.shaders.clear();
}

static void gl_term(void)
{
	glcache.DeleteTextures(ARRAY_SIZE(vmuTextureId), vmuTextureId);
	memset(vmuTextureId, 0, sizeof(vmuTextureId));
	glcache.DeleteTextures(ARRAY_SIZE(lightgunTextureId), lightgunTextureId);
	memset(lightgunTextureId, 0, sizeof(lightgunTextureId));

	free(gVertexBufferPtr);
	free(gIndicesPtr);
	glDeleteTextures(1, &fbTextureId);
	fbTextureId = 0;
	glDeleteTextures(1, &fogTextureId);
	fogTextureId = 0;

	gl_delete_shaders();
}

static bool gl_create_resources(void)
{
	/* create VBOs */
	gVertexBufferPtr = (float*)malloc(0x1800000);
	gIndicesPtr = (uint16_t*)malloc(0x600000);
	gVertexBuffer = gVertexBufferPtr;
	gIndices = gIndicesPtr;

	findGLVersion();

	return true;
}

void UpdateFogTexture(u8 *fog_table, GLenum texture_slot, GLint fog_image_format)
{
	glActiveTexture(texture_slot);
	if (fogTextureId == 0)
	{
		fogTextureId = glcache.GenTexture();
		glcache.BindTexture(GL_TEXTURE_2D, fogTextureId);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	else
		glcache.BindTexture(GL_TEXTURE_2D, fogTextureId);

	u8 temp_tex_buffer[256];
	MakeFogTexture(temp_tex_buffer);

	glTexImage2D(GL_TEXTURE_2D, 0, fog_image_format, 128, 2, 0, fog_image_format, GL_UNSIGNED_BYTE, temp_tex_buffer);

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

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, palette32_ram);

	glActiveTexture(GL_TEXTURE0);
}

void DrawVmuTexture(u8 vmu_screen_number, bool draw_additional_primitives);
void DrawGunCrosshair(u8 port, bool draw_additional_primitives);

void vertex_buffer_unmap(void)
{
}

void DoCleanup() {
}

static void upload_vertex_indices()
{
	gIndices += idx_incr;
	static bool overrun;
	static List<u16> short_idx;
	if (short_idx.daty != NULL)
		short_idx.Free();
	short_idx.Init(pvrrc.idx.used(), &overrun, NULL);
	for (u32 *p = pvrrc.idx.head(); p < pvrrc.idx.LastPtr(0); p++)
		*(short_idx.Append()) = *p;
	memcpy_neon(gIndices, short_idx.head(), short_idx.bytes());
	idx_incr = short_idx.bytes() / sizeof(uint16_t);
}

static bool RenderFrame(void)
{
	int vmu_screen_number = 0 ;
	int lightgun_port = 0 ;

	DoCleanup();

	bool is_rtt=pvrrc.isRTT;

	float vtx_min_fZ=0.f;   //pvrrc.fZ_min;
	float vtx_max_fZ=pvrrc.fZ_max;

	//sanitise the values, now with NaN detection (for omap)
	//0x49800000 is 1024*1024. Using integer math to avoid issues w/ infs and nans
	if ((s32&)vtx_max_fZ<0 || (u32&)vtx_max_fZ>0x49800000)
		vtx_max_fZ=10*1024;


	//add some extra range to avoid clipping border cases
	vtx_min_fZ*=0.98f;
	vtx_max_fZ*=1.001f;

	TransformMatrix<true> matrices(pvrrc);
	ShaderUniforms.normal_mat = matrices.GetNormalMatrix();
	const glm::mat4& scissor_mat = matrices.GetScissorMatrix();
	ViewportMatrix = matrices.GetViewportMatrix();

	if (!is_rtt)
		gcflip=0;
	else
		gcflip=1;

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


	if (fog_needs_update && settings.rend.Fog)
	{
		fog_needs_update=false;
		UpdateFogTexture((u8 *)FOG_TABLE, GL_TEXTURE1, gl.fog_image_format);
	}
	if (palette_updated)
	{
		UpdatePaletteTexture(GL_TEXTURE2);
		palette_updated = false;
	}

	ShaderUniforms.PT_ALPHA=(PT_ALPHA_REF&0xFF)/255.0f;
	
	for (const auto& it : gl.shaders)
	{
		glcache.UseProgram(it.second.program);
		ShaderUniforms.Set(&it.second);
	}

	bool wide_screen_on = settings.rend.WideScreen && !matrices.IsClipped();

   // Color is cleared by the background plane

   glcache.Disable(GL_SCISSOR_TEST);
   glClearDepth(0.0);
   glcache.DepthMask(GL_TRUE);
   glStencilMask(0xFF);
   glClearStencil(0);
   glClear((GLbitfield)(GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));

	//move vertex to gpu

   if (!pvrrc.isRenderFramebuffer)
   {
	  gVertexBuffer += vtx_incr;
	  memcpy_neon(gVertexBuffer, pvrrc.verts.head(), pvrrc.verts.bytes());
	  vtx_incr = pvrrc.verts.bytes() / sizeof(float);
	  
      upload_vertex_indices();

      if (!wide_screen_on)
      {
         float width;
         float height;
         float min_x;
         float min_y;
         if (!is_rtt)
         {
			glm::vec4 clip_min(pvrrc.fb_X_CLIP.min, pvrrc.fb_Y_CLIP.min, 0, 1);
			glm::vec4 clip_dim(pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1,
								   pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1, 0, 0);
			clip_min = scissor_mat * clip_min;
			clip_dim = scissor_mat * clip_dim;

			min_x = clip_min[0];
			min_y = clip_min[1];
			width = clip_dim[0];
			height = clip_dim[1];
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
			if (settings.rend.RenderToTextureUpscale > 1 && !settings.rend.RenderToTextureBuffer)
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
   }
   else
   {
      glcache.ClearColor(0.f, 0.f, 0.f, 0.f);
      glClear(GL_COLOR_BUFFER_BIT);
      DrawFramebuffer();
   }

   if (!is_rtt)
   {
   	if (settings.System == DC_PLATFORM_DREAMCAST)
   	{
			for ( vmu_screen_number = 0 ; vmu_screen_number < 4 ; vmu_screen_number++)
				if ( vmu_screen_params[vmu_screen_number].vmu_screen_display )
					DrawVmuTexture(vmu_screen_number, true) ;
   	}

		for ( lightgun_port = 0 ; lightgun_port < 4 ; lightgun_port++)
				DrawGunCrosshair(lightgun_port, true) ;
   }

	KillTex = false;

	return !is_rtt;
}

void rend_set_fb_scale(float x,float y)
{
	fb_scale_x=x;
	fb_scale_y=y;
}

void co_dc_yield(void);

bool ProcessFrame(TA_context* ctx)
{
   ctx->rend_inuse.Lock();

   if (KillTex)
   {
      TexCache.Clear();
      INFO_LOG(RENDERER, "Texture cache cleared");
   }

   if (ctx->rend.isRenderFramebuffer)
	{
		RenderFramebuffer();
		ctx->rend_inuse.Unlock();
	}
	else
	{
		if (!ta_parse_vdrc(ctx))
			return false;
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

#ifdef HAVE_TEXUPSCALE
      if (settings.rend.TextureUpscale > 1)
      {
         // Trick to preload the tables used by xBRZ
         u32 src[] { 0x11111111, 0x22222222, 0x33333333, 0x44444444 };
         u32 dst[16];
         UpscalexBRZ(2, src, dst, 2, 2, false);
      }
#endif
      fog_needs_update = true;
      TexCache.Clear();

      return true;
   }
	void Resize(int w, int h) override { screen_width=w; screen_height=h; }
	void Term() override
   {
		TexCache.Clear();

	   gl_term();
   }

	bool Process(TA_context* ctx) override
   {
      return ProcessFrame(ctx);
   }
	bool Render() override
   {
      bool ret = RenderFrame();
      return ret;
   }

	void Present() override
   {
      co_dc_yield();
   }

	virtual u64 GetTexture(TSP tsp, TCW tcw) override {
		return gl_GetTexture(tsp, tcw);
	}
};

Renderer* rend_GLES2() { return new glesrend(); }
