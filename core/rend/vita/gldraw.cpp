#include "vita.h"
#include "rend/rend.h"
#include "rend/sorter.h"

extern uint16_t *gIndices;
extern float *gVertexBuffer;
extern uint16_t *gIndicesPtr;
extern float *gVertexBufferPtr;
uint32_t idx_incr = 0, vtx_incr = 0;

/*

Drawing and related state management
Takes vertex, textures and renders to the currently set up target
*/

const static u32 CullMode[]= 
{

	GL_NONE, //0    No culling          No culling
	GL_NONE, //1    Cull if Small       Cull if ( |det| < fpu_cull_val )

	GL_FRONT, //2   Cull if Negative    Cull if ( |det| < 0 ) or ( |det| < fpu_cull_val )
	GL_BACK,  //3   Cull if Positive    Cull if ( |det| > 0 ) or ( |det| < fpu_cull_val )
};
const u32 Zfunction[] =
{
	GL_NEVER,       //0 Never
	GL_LESS,        //1 Less
	GL_EQUAL,       //2 Equal
	GL_LEQUAL,      //3 Less Or Equal
	GL_GREATER,     //4 Greater
	GL_NOTEQUAL,    //5 Not Equal
	GL_GEQUAL,      //6 Greater Or Equal
	GL_ALWAYS,      //7 Always
};

/*
0   Zero                  (0, 0, 0, 0)
1   One                   (1, 1, 1, 1)
2   Dither Color          (OR, OG, OB, OA) 
3   Inverse Dither Color  (1-OR, 1-OG, 1-OB, 1-OA)
4   SRC Alpha             (SA, SA, SA, SA)
5   Inverse SRC Alpha     (1-SA, 1-SA, 1-SA, 1-SA)
6   DST Alpha             (DA, DA, DA, DA)
7   Inverse DST Alpha     (1-DA, 1-DA, 1-DA, 1-DA)
*/

const u32 DstBlendGL[] =
{
	GL_ZERO,
	GL_ONE,
	GL_SRC_COLOR,
	GL_ONE_MINUS_SRC_COLOR,
	GL_SRC_ALPHA,
	GL_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA,
	GL_ONE_MINUS_DST_ALPHA
};

const u32 SrcBlendGL[] =
{
	GL_ZERO,
	GL_ONE,
	GL_DST_COLOR,
	GL_ONE_MINUS_DST_COLOR,
	GL_SRC_ALPHA,
	GL_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA,
	GL_ONE_MINUS_DST_ALPHA
};

extern int screen_width;
extern int screen_height;

PipelineShader* CurrentShader;
extern u32 gcflip;
GLuint vmuTextureId[4]={0,0,0,0};
GLuint lightgunTextureId[4]={0,0,0,0};

void SetCull(u32 CulliMode)
{
	if (CullMode[CulliMode] == GL_NONE)
		glcache.Disable(GL_CULL_FACE);
	else
	{
		glcache.Enable(GL_CULL_FACE);
		glcache.CullFace(CullMode[CulliMode]); //GL_FRONT/GL_BACK, ...
	}
}

static void SetTextureRepeatMode(GLuint dir, u32 clamp, u32 mirror)
{
	if (clamp)
		glcache.TexParameteri(GL_TEXTURE_2D, dir, GL_CLAMP_TO_EDGE);
	else
		glcache.TexParameteri(GL_TEXTURE_2D, dir, mirror ? GL_MIRRORED_REPEAT : GL_REPEAT);
}

static void SetBaseClipping()
{
	if (ShaderUniforms.base_clipping.enabled)
	{
		glcache.Enable(GL_SCISSOR_TEST);
		glcache.Scissor(ShaderUniforms.base_clipping.x, ShaderUniforms.base_clipping.y, ShaderUniforms.base_clipping.width, ShaderUniforms.base_clipping.height);
	}
	else
		glcache.Disable(GL_SCISSOR_TEST);
}

template <u32 Type, bool SortingEnabled>
__forceinline static void SetGPState(const PolyParam* gp,u32 cflip=0)
{
if (gp->pcw.Texture && gp->tsp.FilterMode > 1 && Type != ListType_Punch_Through && gp->tcw.MipMapped == 1)
	{
		ShaderUniforms.trilinear_alpha = 0.25 * (gp->tsp.MipMapD & 0x3);
		if (gp->tsp.FilterMode == 2)
			// Trilinear pass A
			ShaderUniforms.trilinear_alpha = 1.0 - ShaderUniforms.trilinear_alpha;
	}
	else
		ShaderUniforms.trilinear_alpha = 1.f;

	bool color_clamp = gp->tsp.ColorClamp && (pvrrc.fog_clamp_min != 0 || pvrrc.fog_clamp_max != 0xffffffff);
	int fog_ctrl = settings.rend.Fog ? gp->tsp.FogCtrl : 2;

	int clip_rect[4] = {};
	TileClipping clipmode = GetTileClip(gp->tileclip, ViewportMatrix, clip_rect);
	bool palette = BaseTextureCacheData::IsGpuHandledPaletted(gp->tsp, gp->tcw);

	CurrentShader = GetProgram(Type == ListType_Punch_Through ? 1 : 0,
								  clipmode == TileClipping::Inside,
								  gp->pcw.Texture,
								  gp->tsp.UseAlpha,
								  gp->tsp.IgnoreTexA,
								  gp->tsp.ShadInstr,
								  gp->pcw.Offset,
								  fog_ctrl,
								  gp->pcw.Gouraud,
								  gp->tcw.PixelFmt == PixelBumpMap,
								  color_clamp,
								  ShaderUniforms.trilinear_alpha != 1.f,
								  palette);

	glcache.UseProgram(CurrentShader->program);
	if (CurrentShader->trilinear_alpha != -1)
		glUniform1f(CurrentShader->trilinear_alpha, ShaderUniforms.trilinear_alpha);
	if (palette)
	{
		if (gp->tcw.PixelFmt == PixelPal4)
			ShaderUniforms.palette_index = gp->tcw.PalSelect << 4;
		else
			ShaderUniforms.palette_index = (gp->tcw.PalSelect >> 4) << 8;
		glUniform1i(CurrentShader->palette_index, ShaderUniforms.palette_index);
	}

	if (clipmode == TileClipping::Inside)
		glUniform4f(CurrentShader->pp_ClipTest, clip_rect[0], clip_rect[1], clip_rect[0] + clip_rect[2], clip_rect[1] + clip_rect[3]);
	if (clipmode == TileClipping::Outside)
	{
		glcache.Enable(GL_SCISSOR_TEST);
		glcache.Scissor(clip_rect[0], clip_rect[1], clip_rect[2], clip_rect[3]);
	}
	else
		SetBaseClipping();

	//This bit control which pixels are affected
	//by modvols
	const u32 stencil=(gp->pcw.Shadow!=0)?0x80:0x0;

	glcache.StencilFunc(GL_ALWAYS,stencil,stencil);

	glcache.BindTexture(GL_TEXTURE_2D, gp->texid == (u64)-1 ? 0 : (GLuint)gp->texid);

	SetTextureRepeatMode(GL_TEXTURE_WRAP_S, gp->tsp.ClampU, gp->tsp.FlipU);
	SetTextureRepeatMode(GL_TEXTURE_WRAP_T, gp->tsp.ClampV, gp->tsp.FlipV);

	//set texture filter mode
	if (gp->tsp.FilterMode == 0 || palette)
	{
		//disable filtering, mipmaps
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	else
	{
		//bilinear filtering
		//PowerVR supports also trilinear via two passes, but we ignore that for now
		bool mipmapped = gp->tcw.MipMapped != 0 && gp->tcw.ScanOrder == 0 && settings.rend.UseMipmaps;
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmapped ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}

	// Apparently punch-through polys support blending, or at least some combinations
	if (Type == ListType_Translucent || Type == ListType_Punch_Through)
	{
		glcache.Enable(GL_BLEND);
		glcache.BlendFunc(SrcBlendGL[gp->tsp.SrcInstr], DstBlendGL[gp->tsp.DstInstr]);
	}
	else
		glcache.Disable(GL_BLEND);

	//set cull mode !
	//cflip is required when exploding triangles for triangle sorting
	//gcflip is global clip flip, needed for when rendering to texture due to mirrored Y direction
	SetCull(gp->isp.CullMode ^ cflip ^ gcflip);

	//set Z mode, only if required
	if (Type == ListType_Punch_Through || (Type == ListType_Translucent && SortingEnabled))
	{
		glcache.DepthFunc(GL_GEQUAL);
	}
	else
	{
		glcache.DepthFunc(Zfunction[gp->isp.DepthMode]);
	}

	if (SortingEnabled && settings.pvr.Emulation.AlphaSortMode == 0)
		glcache.DepthMask(GL_FALSE);
	else
	{
		// Z Write Disable seems to be ignored for punch-through.
		// Fixes Worms World Party, Bust-a-Move 4 and Re-Volt
		if (Type == ListType_Punch_Through)
			glcache.DepthMask(GL_TRUE);
		else
			glcache.DepthMask(!gp->isp.ZWriteDis);
	}
}

template <u32 Type, bool SortingEnabled>
static void DrawList(const List<PolyParam>& gply, int first, int count)
{
   PolyParam* params= &gply.head()[first];

   /* We want at least 1 PParam */
   if (count==0)
      return;

   /* set some 'global' modes for all primitives */
   glcache.Enable(GL_STENCIL_TEST);
   glcache.StencilFunc(GL_ALWAYS,0,0);
   glcache.StencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);

   while(count-->0)
   {
      if (params->count>2) /* this actually happens for some games. No idea why .. */
      {
         SetGPState<Type,SortingEnabled>(params);
		 vglIndexPointerMapped(gIndices + params->first);
		 vglDrawObjects(GL_TRIANGLE_STRIP, params->count, GL_FALSE);
      }

      params++;
   }
}

static vector<SortTrigDrawParam>	pidx_sort;

//All pixels are in area 0 by default.
//If inside an 'in' volume, they are in area 1
//if inside an 'out' volume, they are in area 0
/*
	Stencil bits:
		bit 7: mv affected (must be preserved)
		bit 1: current volume state
		but 0: summary result (starts off as 0)

	Lower 2 bits:

	IN volume (logical OR):
	00 -> 00
	01 -> 01
	10 -> 01
	11 -> 01

	Out volume (logical AND):
	00 -> 00
	01 -> 00
	10 -> 00
	11 -> 01
*/
void SetMVS_Mode(ModifierVolumeMode mv_mode, ISP_Modvol ispc)
{
	if (mv_mode == Xor)
	{
		// set states
		glcache.Enable(GL_DEPTH_TEST);
		// write only bit 1
      glcache.StencilMask(2);
      // no stencil testing
      glcache.StencilFunc(GL_ALWAYS, 0, 2);
		// count the number of pixels in front of the Z buffer (xor zpass)
      glcache.StencilOp(GL_KEEP, GL_KEEP, GL_INVERT);

		//Cull mode needs to be set
		SetCull(ispc.CullMode);
	}
	else if (mv_mode == Or)
   {
      // set states
      glcache.Enable(GL_DEPTH_TEST);
		// write only bit 1
      glcache.StencilMask(2);
      // no stencil testing
      glcache.StencilFunc(GL_ALWAYS, 2, 2);
      // Or'ing of all triangles
      glcache.StencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

      // Cull mode needs to be set
      SetCull(ispc.CullMode);
   }
   else
	{
      // Inclusion or Exclusion volume

		// no depth test
		glcache.Disable(GL_DEPTH_TEST);
      // write bits 1:0
      glcache.StencilMask(3);

		if (mv_mode == Inclusion)
		{
         // Inclusion volume
			//res : old : final 
			//0   : 0      : 00
			//0   : 1      : 01
			//1   : 0      : 01
			//1   : 1      : 01
			
			// if (1<=st) st=1; else st=0;
         glcache.StencilFunc(GL_LEQUAL, 1, 3);
         glcache.StencilOp(GL_ZERO, GL_ZERO, GL_REPLACE);
		}
		else
		{
         // Exclusion volume
			/*
				I've only seen a single game use it, so i guess it doesn't matter ? (Zombie revenge)
				(actually, i think there was also another, racing game)
			*/

         // The initial value for exclusion volumes is 1 so we need to invert the result before and'ing.
			//res : old : final 
			//0   : 0   : 00
			//0   : 1   : 01
			//1   : 0   : 00
			//1   : 1   : 00

			// if (1 == st) st = 1; else st = 0;
         glcache.StencilFunc(GL_EQUAL, 1, 3);
         glcache.StencilOp(GL_ZERO, GL_ZERO, GL_KEEP);
		}
	}
}

static void SetupMainVBO(void)
{
	vglVertexAttribPointerMapped(0, gVertexBuffer);
	vglIndexPointerMapped(gIndices);
}

void DrawStrips(void)
{
   SetupMainVBO();
   //Draw the strips !

   //We use sampler 0
   glActiveTexture(GL_TEXTURE0);

   RenderPass previous_pass = {};
   for (int render_pass = 0; render_pass < pvrrc.render_passes.used(); render_pass++)
   {
      const RenderPass& current_pass = pvrrc.render_passes.head()[render_pass];

      DEBUG_LOG(RENDERER, "Render pass %d OP %d PT %d TR %d MV %d", render_pass + 1,
      		current_pass.op_count - previous_pass.op_count,
			current_pass.pt_count - previous_pass.pt_count,
			current_pass.tr_count - previous_pass.tr_count,
			current_pass.mvo_count - previous_pass.mvo_count);

      //initial state
      glcache.Enable(GL_DEPTH_TEST);
      glcache.DepthMask(GL_TRUE);

      //Opaque
      DrawList<ListType_Opaque, false>(pvrrc.global_param_op, 
            previous_pass.op_count, current_pass.op_count - previous_pass.op_count);

      //Alpha tested
      DrawList<ListType_Punch_Through, false>(pvrrc.global_param_pt,
            previous_pass.pt_count, current_pass.pt_count - previous_pass.pt_count);

      //Alpha blended
      {
         if (current_pass.autosort)
         {
			SortPParams(previous_pass.tr_count, current_pass.tr_count - previous_pass.tr_count);
			DrawList<ListType_Translucent, true>(pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count - previous_pass.tr_count );
         }
         else
             DrawList<ListType_Translucent, false>(pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count - previous_pass.tr_count);
      }

      previous_pass = current_pass;
   }

   vertex_buffer_unmap();
}

static void DrawQuad(GLuint texId, float x, float y, float w, float h, float u0, float v0, float u1, float v1)
{
	struct Vertex vertices[] = {
		{ x,     y + h, 0.1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, u0, v1 },
		{ x,     y,     0.1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, u0, v0 },
		{ x + w, y + h, 0.1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, u1, v1 },
		{ x + w, y,     0.1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, u1, v0 },
	};
	GLushort indices[] = { 0, 1, 2, 1, 3 };

	glcache.Disable(GL_SCISSOR_TEST);
	glcache.Disable(GL_DEPTH_TEST);
	glcache.Disable(GL_STENCIL_TEST);
	glcache.Disable(GL_CULL_FACE);
	glcache.Disable(GL_BLEND);

	ShaderUniforms.trilinear_alpha = 1.0;

	PipelineShader *shader = GetProgram(0, false, 1, 0, 1, 0, 0, 2, false, false, false, false, false);
	glcache.UseProgram(shader->program);

	glActiveTexture(GL_TEXTURE0);
	glcache.BindTexture(GL_TEXTURE_2D, texId);

	gVertexBuffer += vtx_incr;
	gIndices += idx_incr;
	memcpy_neon(gVertexBuffer, vertices, sizeof(vertices));
	memcpy_neon(gIndices, indices, sizeof(indices));
	vtx_incr = sizeof(vertices) / sizeof(float);
	idx_incr = sizeof(indices) / sizeof(uint16_t);
	SetupMainVBO();
	vglDrawObjects(GL_TRIANGLE_STRIP, 5, GL_FALSE);
}

void DrawFramebuffer()
{
	DrawQuad(fbTextureId, 0, 0, 640.f, 480.f, 0, 0, 1, 1);
	glcache.DeleteTextures(1, &fbTextureId);
	fbTextureId = 0;
}

void UpdateVmuTexture(int vmu_screen_number)
{
	s32 x,y ;
	u8 temp_tex_buffer[VMU_SCREEN_HEIGHT*VMU_SCREEN_WIDTH*4];
	u8 *dst = temp_tex_buffer;
	u8 *src = NULL ;
	u8 *origsrc = NULL ;
	u8 vmu_pixel_on_R = vmu_screen_params[vmu_screen_number].vmu_pixel_on_R ;
	u8 vmu_pixel_on_G = vmu_screen_params[vmu_screen_number].vmu_pixel_on_G ;
	u8 vmu_pixel_on_B = vmu_screen_params[vmu_screen_number].vmu_pixel_on_B ;
	u8 vmu_pixel_off_R = vmu_screen_params[vmu_screen_number].vmu_pixel_off_R ;
	u8 vmu_pixel_off_G = vmu_screen_params[vmu_screen_number].vmu_pixel_off_G ;
	u8 vmu_pixel_off_B = vmu_screen_params[vmu_screen_number].vmu_pixel_off_B ;
	u8 vmu_screen_opacity = vmu_screen_params[vmu_screen_number].vmu_screen_opacity ;

	if (vmuTextureId[vmu_screen_number] == 0)
	{
		vmuTextureId[vmu_screen_number] = glcache.GenTexture();
		glcache.BindTexture(GL_TEXTURE_2D, vmuTextureId[vmu_screen_number]);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	else
		glcache.BindTexture(GL_TEXTURE_2D, vmuTextureId[vmu_screen_number]);


	origsrc = vmu_screen_params[vmu_screen_number].vmu_lcd_screen ;

	if ( origsrc == NULL )
		return ;


	for ( y = VMU_SCREEN_HEIGHT-1 ; y >= 0 ; y--)
	{
		src = origsrc + (y*VMU_SCREEN_WIDTH) ;

		for ( x = 0 ; x < VMU_SCREEN_WIDTH ; x++)
		{
			if ( *src++ > 0 )
			{
				*dst++ = vmu_pixel_on_R ;
				*dst++ = vmu_pixel_on_G ;
				*dst++ = vmu_pixel_on_B ;
				*dst++ = vmu_screen_opacity ;
			}
			else
			{
				*dst++ = vmu_pixel_off_R ;
				*dst++ = vmu_pixel_off_G ;
				*dst++ = vmu_pixel_off_B ;
				*dst++ = vmu_screen_opacity ;
			}
		}
	}

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, VMU_SCREEN_WIDTH, VMU_SCREEN_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, temp_tex_buffer);

	vmu_screen_params[vmu_screen_number].vmu_screen_needs_update = false ;

}

void DrawVmuTexture(u8 vmu_screen_number, bool draw_additional_primitives)
{
	glActiveTexture(GL_TEXTURE0);

	float x=0 ;
	float y=0 ;
	float w=VMU_SCREEN_WIDTH*vmu_screen_params[vmu_screen_number].vmu_screen_size_mult ;
	float h=VMU_SCREEN_HEIGHT*vmu_screen_params[vmu_screen_number].vmu_screen_size_mult ;

	if (vmu_screen_params[vmu_screen_number].vmu_screen_needs_update || vmuTextureId[vmu_screen_number] == 0)
		UpdateVmuTexture(vmu_screen_number) ;

	switch ( vmu_screen_params[vmu_screen_number].vmu_screen_position )
	{
		case UPPER_LEFT :
		{
			x = 0 ;
			y = 0 ;
			break ;
		}
		case UPPER_RIGHT :
		{
			x = 640-w ;
			y = 0 ;
			break ;
		}
		case LOWER_LEFT :
		{
			x = 0 ;
			y = 480-h ;
			break ;
		}
		case LOWER_RIGHT :
		{
			x = 640-w ;
			y = 480-h ;
			break ;
		}
	}

	glcache.BindTexture(GL_TEXTURE_2D, vmuTextureId[vmu_screen_number]);

	glcache.Disable(GL_SCISSOR_TEST);
	glcache.Disable(GL_DEPTH_TEST);
	glcache.Disable(GL_STENCIL_TEST);
	glcache.Disable(GL_CULL_FACE);
	glcache.Enable(GL_BLEND);
	glcache.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	PipelineShader *shader = GetProgram(0, 1, 1, 1, 0, 0, 0, 2, false, false, false, false, false);
	glcache.UseProgram(shader->program);

	{
		struct Vertex vertices[] = {
				{ x,   y+h, 1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 0, 1 },
				{ x,   y,   1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 0, 0 },
				{ x+w, y+h, 1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 1, 1 },
				{ x+w, y,   1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 1, 0 },
		};
		GLushort indices[] = { 0, 1, 2, 1, 3 };

		gVertexBuffer += vtx_incr;
		gIndices += idx_incr;
		memcpy_neon(gVertexBuffer, vertices, sizeof(vertices));
		memcpy_neon(gIndices, indices, sizeof(indices));
		vtx_incr = sizeof(vertices) / sizeof(float);
		idx_incr = sizeof(indices) / sizeof(uint16_t);
		SetupMainVBO();
	}

	vglDrawObjects(GL_TRIANGLE_STRIP, 5, GL_FALSE);
}

void UpdateLightGunTexture(int port)
{
	s32 x,y ;
	u8 temp_tex_buffer[LIGHTGUN_CROSSHAIR_SIZE*LIGHTGUN_CROSSHAIR_SIZE*4];
	u8 *dst = temp_tex_buffer;
	u8 *src = NULL ;

	if (lightgunTextureId[port] == 0)
	{
		lightgunTextureId[port] = glcache.GenTexture();
		glcache.BindTexture(GL_TEXTURE_2D, lightgunTextureId[port]);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	else
		glcache.BindTexture(GL_TEXTURE_2D, lightgunTextureId[port]);

	u8* colour = &( lightgun_palette[ lightgun_params[port].colour * 3 ] );

	for ( y = LIGHTGUN_CROSSHAIR_SIZE-1 ; y >= 0 ; y--)
	{
	   src = lightgun_img_crosshair + (y*LIGHTGUN_CROSSHAIR_SIZE) ;

	   for ( x = 0 ; x < LIGHTGUN_CROSSHAIR_SIZE ; x++)
	   {
		   if ( src[x] )
		   {
			  *dst++ = colour[0] ;
			  *dst++ = colour[1] ;
			  *dst++ = colour[2] ;
			  *dst++ = 0xFF ;
		   }
		   else
		   {			   
			  *dst++ = 0 ;
			  *dst++ = 0 ;
			  *dst++ = 0 ;
			  *dst++ = 0 ;
		   }
	   }
	}

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LIGHTGUN_CROSSHAIR_SIZE, LIGHTGUN_CROSSHAIR_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, temp_tex_buffer);

	lightgun_params[port].dirty = false;
}

void DrawGunCrosshair(u8 port, bool draw_additional_primitives)
{
	if ( lightgun_params[port].offscreen || (lightgun_params[port].colour==0) )
		return;
	
	glActiveTexture(GL_TEXTURE0);

	float x=0;
	float y=0;
	float w=LIGHTGUN_CROSSHAIR_SIZE;
	float h=LIGHTGUN_CROSSHAIR_SIZE;

	x = lightgun_params[port].x - ( LIGHTGUN_CROSSHAIR_SIZE / 2 );
	y = lightgun_params[port].y - ( LIGHTGUN_CROSSHAIR_SIZE / 2 );

	if ( lightgun_params[port].dirty || lightgunTextureId[port] == 0)
		UpdateLightGunTexture(port);

	glcache.BindTexture(GL_TEXTURE_2D, lightgunTextureId[port]);

	glcache.Disable(GL_SCISSOR_TEST);
	glcache.Disable(GL_DEPTH_TEST);
	glcache.Disable(GL_STENCIL_TEST);
	glcache.Disable(GL_CULL_FACE);
	glcache.Enable(GL_BLEND);
	glcache.BlendFunc(GL_SRC_ALPHA, GL_ONE);

	PipelineShader *shader = GetProgram(0, 1, 1, 1, 0, 0, 0, 2, false, false, false, false, false);
	glcache.UseProgram(shader->program);

	{
		struct Vertex vertices[] = {
				{ x,   y+h, 1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 0, 1 },
				{ x,   y,   1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 0, 0 },
				{ x+w, y+h, 1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 1, 1 },
				{ x+w, y,   1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 1, 0 },
		};
		GLushort indices[] = { 0, 1, 2, 1, 3 };
		
		gVertexBuffer += vtx_incr;
		gIndices += idx_incr;
		memcpy_neon(gVertexBuffer, vertices, sizeof(vertices));
		memcpy_neon(gIndices, indices, sizeof(indices));
		vtx_incr = sizeof(vertices) / sizeof(float);
		idx_incr = sizeof(indices) / sizeof(uint16_t);
		SetupMainVBO();
	}

	
	vglDrawObjects(GL_TRIANGLE_STRIP, 5, GL_FALSE);

	glcache.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
