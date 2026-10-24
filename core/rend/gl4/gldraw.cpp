#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
#include "gl4.h"
#include "rend/gles/ta_decoder_gl.h"
#include "rend/gles/glprogram_cache.h"
#include "rend/modvol_clip.h"

/*

Drawing and related state management
Takes vertex, textures and renders to the currently set up target
*/

static gl4PipelineShader* CurrentShader;
extern u32 gcflip;
GLuint geom_fbo;
GLuint stencilTexId;
GLuint opaqueTexId;
GLuint depthTexId;
GLuint texSamplers[2];
GLuint depth_fbo;
GLuint depthSaveTexId;

/* (tests: every polygon drawn with the general programs, to see they draw
 * what the polygons' own ones do) */
#ifdef GL_PROGRAM_TEST_GENERAL
static const bool PipelineTestGeneral = true;
#else
static const bool PipelineTestGeneral = false;
#endif

static void pipeline_from_key(gl4PipelineShader *s, u32 key)
{
	s->key = key;
	s->pp_InsideClipping = (key >> 16) & 1;
	s->cp_AlphaTest = (key >> 15) & 1;
	s->pp_Texture = (key >> 14) & 1;
	s->pp_UseAlpha = (key >> 13) & 1;
	s->pp_IgnoreTexA = (key >> 12) & 1;
	s->pp_ShadInstr = (key >> 10) & 3;
	s->pp_Offset = (key >> 9) & 1;
	s->pp_FogCtrl = (key >> 7) & 3;
	s->pp_TwoVolumes = (key >> 6) & 1;
	s->pp_Gouraud = (key >> 5) & 1;
	s->pp_BumpMap = (key >> 4) & 1;
	s->fog_clamping = (key >> 3) & 1;
	s->palette = (key >> 2) & 1;
	s->pass = (Pass)(key & 3);
}

// A polygon drawn with the general program @g until its own is there
static void use_general(gl4PipelineShader *s, gl4PipelineShader *g)
{
	const u32 key = s->key;
	const GLuint pending = s->pending;
	gl4PipelineShader bits = *s;
	*s = *g;
	// (its own bits, the general one's program and uniforms)
	s->cp_AlphaTest = bits.cp_AlphaTest;
	s->pp_InsideClipping = bits.pp_InsideClipping;
	s->pp_Texture = bits.pp_Texture;
	s->pp_UseAlpha = bits.pp_UseAlpha;
	s->pp_IgnoreTexA = bits.pp_IgnoreTexA;
	s->pp_ShadInstr = bits.pp_ShadInstr;
	s->pp_Offset = bits.pp_Offset;
	s->pp_FogCtrl = bits.pp_FogCtrl;
	s->pass = bits.pass;
	s->pp_TwoVolumes = bits.pp_TwoVolumes;
	s->pp_Gouraud = bits.pp_Gouraud;
	s->pp_BumpMap = bits.pp_BumpMap;
	s->fog_clamping = bits.fog_clamping;
	s->palette = bits.palette;
	s->key = key;
	s->pending = pending;
	s->general = g;
}

/* A key first drawn (in a frame): drawn with the general program, its own
 * asked for in the background where the driver does that (else that is it
 * for this run: the key is kept, and its program made at the start of the
 * next); without a general program, made now as ever */
static void pipeline_first(gl4PipelineShader *s, u32 key)
{
	pipeline_from_key(s, key);
	glprog_note(GLPROG_GL4, key);
	gl4PipelineShader *g = &gl4.general[(int)s->pass][s->pp_Gouraud];
	if (g->program == 0)
	{
		gl4CompilePipelineShader(s);
		return;
	}
	use_general(s, g);
	glprog_general_used();
	if (glprog_background() && !PipelineTestGeneral)
	{
		s->pending = gl4MakePipelineProgram(s, false, true);
		s->polled = gl4.frame;
	}
}

// (once a frame) whether its own program is there now
static void pipeline_poll(gl4PipelineShader *s)
{
	s->polled = gl4.frame;
	switch (glprog_poll(s->pending))
	{
	case GLPROG_PENDING:
		break;
	case GLPROG_READY:
		s->program = s->pending;
		s->pending = 0;
		s->general = nullptr;
		gl4PipelineLocations(s, false);
		break;
	default:
		WARN_LOG(RENDERER, "OpenGL: the program of key %x did not link; the general one draws it", s->key);
		glDeleteProgram(s->pending);
		s->pending = 0;
		break;
	}
}

static gl4PipelineShader *gl4GetProgram(bool cp_AlphaTest, bool pp_InsideClipping,
							bool pp_Texture, bool pp_UseAlpha, bool pp_IgnoreTexA, u32 pp_ShadInstr, bool pp_Offset,
							u32 pp_FogCtrl, bool pp_TwoVolumes, bool pp_Gouraud, bool pp_BumpMap, bool fog_clamping,
							bool palette, Pass pass)
{
	u32 rv=0;

	rv |= pp_InsideClipping;
	rv <<= 1; rv |= cp_AlphaTest;
	rv <<= 1; rv |= pp_Texture;
	rv <<= 1; rv |= pp_UseAlpha;
	rv <<= 1; rv |= pp_IgnoreTexA;
	rv <<= 2; rv |= pp_ShadInstr;
	rv <<= 1; rv |= pp_Offset;
	rv <<= 2; rv |= pp_FogCtrl;
	rv <<= 1; rv |= pp_TwoVolumes;
	rv <<= 1; rv |= pp_Gouraud;
	rv <<= 1; rv |= pp_BumpMap;
	rv <<= 1; rv |= fog_clamping;
	rv <<= 1; rv |= palette;
	rv <<= 2; rv |= (int)pass;

	gl4PipelineShader *shader = &gl4.shaders[rv];
	if (shader->program == 0)
		pipeline_first(shader, rv);
	else if (shader->pending != 0 && shader->polled != gl4.frame)
		pipeline_poll(shader);

	return shader;
}

/* The keys more than one game draws with (glprogram_cache.h), of Jet Grind
 * Radio, Dead or Alive 2, Sega Rally 2 and Virtua Fighter 4: made at the
 * start with those of earlier runs. (The general programs draw the
 * others the first time.) */
static const u32 CommonKeys[] = {
	0x0121, 0x1521, 0x2100, 0x3100, 0x5101, 0x6f22,
};

void gl4MakePrograms()
{
	for (int p = 0; p < 3; p++)
		for (int g = 0; g < 2; g++)
		{
			gl4PipelineShader *s = &gl4.general[p][g];
			memset(s, 0, sizeof(*s));
			s->pp_Gouraud = g;
			s->pass = (Pass)p;
			s->program = gl4MakePipelineProgram(s, true, true);
		}
	u32 count = 0;
	const u32 *known = glprog_known(GLPROG_GL4, &count);
	std::vector<u32> keys;
	keys.reserve(count + ARRAY_SIZE(CommonKeys) + 2);
	for (u32 i = 0; i < count; i++)
		if (known[i] <= 0x1FFFF && (known[i] & 3) != 3)
			keys.push_back(known[i]);
	for (u32 k : CommonKeys)
		keys.push_back(k);
	if (keys.size() > 1024)
		keys.resize(1024);
	if (!PipelineTestGeneral)
		for (u32 key : keys)
		{
			gl4PipelineShader *s = &gl4.shaders[key];
			if (s->program != 0 || s->pending != 0)
				continue;
			pipeline_from_key(s, key);
			s->pending = gl4MakePipelineProgram(s, false, true);
		}
	// now waited for
	for (int p = 0; p < 3; p++)
		for (int g = 0; g < 2; g++)
		{
			gl4PipelineShader *s = &gl4.general[p][g];
			if (glprog_wait(s->program))
				gl4PipelineLocations(s, true);
			else
			{
				WARN_LOG(RENDERER, "OpenGL: no general program (pass %d, pp_Gouraud %d)", p, g);
				glDeleteProgram(s->program);
				s->program = 0;
			}
		}
	for (auto it = gl4.shaders.begin(); it != gl4.shaders.end(); )
	{
		gl4PipelineShader *s = &it->second;
		if (s->pending != 0 && s->program == 0)
		{
			const GLuint p = s->pending;
			s->pending = 0;
			if (glprog_wait(p))
			{
				s->program = p;
				gl4PipelineLocations(s, false);
			}
			else
			{
				WARN_LOG(RENDERER, "OpenGL: the program of key %x did not link", s->key);
				glDeleteProgram(p);
				it = gl4.shaders.erase(it);
				continue;
			}
		}
		++it;
	}
}

static void SetTextureRepeatMode(int index, GLuint dir, u32 clamp, u32 mirror)
{
	if (clamp)
		glSamplerParameteri(texSamplers[index], dir, GL_CLAMP_TO_EDGE);
	else
		glSamplerParameteri(texSamplers[index], dir, mirror ? GL_MIRRORED_REPEAT : GL_REPEAT);
}

static void SetBaseClipping()
{
	if (gl4ShaderUniforms.base_clipping.enabled)
		gl4Scissor(gl4ShaderUniforms.base_clipping.x, gl4ShaderUniforms.base_clipping.y, gl4ShaderUniforms.base_clipping.width, gl4ShaderUniforms.base_clipping.height);
	else
		gl4ScissorOff();
}

template <u32 Type, bool SortingEnabled, Pass pass>
static void SetGPState(const PolyParam* gp)
{
	// Trilinear filtering. Ignore if texture isn't mipmapped (shenmue snowflakes)
	if (gp->pcw.Texture && gp->tsp.FilterMode > 1 && Type != ListType_Punch_Through && gp->tcw.MipMapped == 1)
	{
		gl4ShaderUniforms.trilinear_alpha = 0.25 * (gp->tsp.MipMapD & 0x3);
		if (gp->tsp.FilterMode == 2)
			// Trilinear pass A
			gl4ShaderUniforms.trilinear_alpha = 1.0 - gl4ShaderUniforms.trilinear_alpha;
	}
	else
		gl4ShaderUniforms.trilinear_alpha = 1.0;

	int clip_rect[4] = {};
	TileClipping clipmode = GetTileClip(gp->tileclip, ViewportMatrix, clip_rect);
	bool palette = false;

	if (pass == Pass::Depth)
	{
		/* A punch-through polygon is cut out by its texture's alpha in this
		 * pass too. Where the texture is one of palette indices, that alpha
		 * is in the palette: without it the cut-out was made from the
		 * indices, and what the polygon left in the depth and stencil
		 * buffers - shadows on it, what shows through it - was wrong.
		 * (Upstream: shadows in Alone in the Dark.) */
		palette = Type == ListType_Punch_Through && gp->pcw.Texture
				&& BaseTextureCacheData::IsGpuHandledPaletted(gp->tsp, gp->tcw);
		CurrentShader = gl4GetProgram(Type == ListType_Punch_Through ? true : false,
				clipmode == TileClipping::Inside,
				Type == ListType_Punch_Through ? gp->pcw.Texture : false,
				true,
				gp->tsp.IgnoreTexA,
				0,
				false,
				2,
				false,	// TODO Can PT have two different textures for area 0 and 1 ??
				false,
				false,
				false,
				palette,
				pass);
	}
	else
	{
		// Two volumes mode only supported for OP and PT
		bool two_volumes_mode = (gp->tsp1.full != (u32)-1) && Type != ListType_Translucent;
		bool color_clamp = gp->tsp.ColorClamp && (pvrrc.fog_clamp_min != 0 || pvrrc.fog_clamp_max != 0xffffffff);

		int fog_ctrl = gp->tsp.FogCtrl;

		palette = BaseTextureCacheData::IsGpuHandledPaletted(gp->tsp, gp->tcw);

		CurrentShader = gl4GetProgram(Type == ListType_Punch_Through ? true : false,
				clipmode == TileClipping::Inside,
				gp->pcw.Texture,
				gp->tsp.UseAlpha,
				gp->tsp.IgnoreTexA,
				gp->tsp.ShadInstr,
				gp->pcw.Offset,
				fog_ctrl,
				two_volumes_mode,
				gp->pcw.Gouraud,
				gp->tcw.PixelFmt == PixelBumpMap,
				color_clamp,
				palette,
				pass);
	}
	glcache.UseProgram(CurrentShader->program);

	if (palette)
	{
		if (gp->tcw.PixelFmt == PixelPal4)
			gl4ShaderUniforms.palette_index = (float(gp->tcw.PalSelect << 4) + 0.5f) / 1024.f;
		else
			gl4ShaderUniforms.palette_index = (float((gp->tcw.PalSelect >> 4) << 8) + 0.5f) / 1024.f;
	}

	gl4ShaderUniforms.tsp0 = gp->tsp;
	gl4ShaderUniforms.tsp1 = gp->tsp1;
	gl4ShaderUniforms.tcw0 = gp->tcw;
	gl4ShaderUniforms.tcw1 = gp->tcw1;
	gl4ShaderUniforms.Set(CurrentShader);

	// An opaque polygon is blended by its instructions too when it is in a
	// list continuation; the ones of the first render pass have had theirs
	// set to source x 1 and destination x 0 when the list was read. That
	// pair is no blending at all, and is left as that so it costs nothing.
	if (pass == Pass::Color && (Type == ListType_Translucent || Type == ListType_Punch_Through
			|| gp->tsp.SrcInstr != 1 || gp->tsp.DstInstr != 0))
	{
		glcache.Enable(GL_BLEND);
		glcache.BlendFunc(SrcBlendGL[gp->tsp.SrcInstr], DstBlendGL[gp->tsp.DstInstr]);
	}
	else
		glcache.Disable(GL_BLEND);

	if (clipmode == TileClipping::Inside)
		glUniform4f(CurrentShader->pp_ClipTest, clip_rect[0], clip_rect[1], clip_rect[0] + clip_rect[2], clip_rect[1] + clip_rect[3]);
	if (clipmode == TileClipping::Outside)
	{
		if (gl4ShaderUniforms.base_clipping.enabled)
			IntersectClipRect(clip_rect, gl4ShaderUniforms.base_clipping.x, gl4ShaderUniforms.base_clipping.y,
					gl4ShaderUniforms.base_clipping.width, gl4ShaderUniforms.base_clipping.height);
		gl4Scissor(clip_rect[0], clip_rect[1], clip_rect[2], clip_rect[3]);
	}
	else
		SetBaseClipping();

	// This bit controls which pixels are affected by modvols
	const u32 stencil = gp->pcw.Shadow != 0 ? 0x80 : 0x0;

	glcache.StencilFunc(GL_ALWAYS, stencil, stencil);

	if (CurrentShader->pp_Texture)
	{
		for (int i = 0; i < 2; i++)
		{
			glActiveTexture(GL_TEXTURE0 + i);
			GLuint texid = (GLuint)(i == 0 ? gp->texid : gp->texid1);

			glBindTexture(GL_TEXTURE_2D, texid == (GLuint)-1 ? 0 : texid);

			if (texid != (GLuint)-1)
			{
				TSP tsp = i == 0 ? gp->tsp : gp->tsp1;

				glBindSampler(i, texSamplers[i]);
				SetTextureRepeatMode(i, GL_TEXTURE_WRAP_S, tsp.ClampU, tsp.FlipU);
				SetTextureRepeatMode(i, GL_TEXTURE_WRAP_T, tsp.ClampV, tsp.FlipV);

				//set texture filter mode
				/* Mipmapping does not depend on the filter: see the
				 * per-triangle renderer. This went by the first texture's
				 * word for the second texture too. */
				const TCW tcw = i == 0 ? gp->tcw : gp->tcw1;
				const bool index_texture = i == 0 && palette;
				const bool mipmapped = !index_texture && tcw.MipMapped != 0 && !settings.rend.ForceTextureLOD0
						&& (tcw.ScanOrder == 0 || tcw.PixelFmt == PixelPal4 || tcw.PixelFmt == PixelPal8);
				// the two samplers are shared by every texture: what one polygon set, the next has unless it is set again
				glSamplerParameterf(texSamplers[i], GL_TEXTURE_LOD_BIAS, mipmapped ? D_Adjust_LoD_Bias[tsp.MipMapD] : 0.f);
				if (tsp.FilterMode == 0)
				{
					/* No filtering - the level is still picked. And no
					 * anisotropy: left on from a filtered polygon, it had
					 * these filtered all the same, a texture of palette
					 * indices included. */
					glSamplerParameteri(texSamplers[i], GL_TEXTURE_MIN_FILTER, mipmapped ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
					glSamplerParameteri(texSamplers[i], GL_TEXTURE_MAG_FILTER, GL_NEAREST);
					if (gl.max_anisotropy > 1.f)
						glSamplerParameterf(texSamplers[i], GL_TEXTURE_MAX_ANISOTROPY_EXT, 1.f);
				}
				else
				{
					//bilinear filtering
					//PowerVR supports also trilinear via two passes, but we ignore that for now
					glSamplerParameteri(texSamplers[i], GL_TEXTURE_MIN_FILTER, mipmapped ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR);
					glSamplerParameteri(texSamplers[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
					if (gl.max_anisotropy > 1.f)
					{
						if (settings.rend.AnisotropicFiltering > 1)
						{
							glSamplerParameterf(texSamplers[i], GL_TEXTURE_MAX_ANISOTROPY_EXT,
									std::min((f32)settings.rend.AnisotropicFiltering, gl.max_anisotropy));
							// Set the recommended minification filter for best results - not for
							// a punch-through polygon, whose alpha two levels blended would spoil
							if (mipmapped && Type != ListType_Punch_Through)
								glSamplerParameteri(texSamplers[i], GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
						}
						else
							glSamplerParameterf(texSamplers[i], GL_TEXTURE_MAX_ANISOTROPY_EXT, 1.f);
					}

				}
			}
		}
		glActiveTexture(GL_TEXTURE0);
	}

	//set cull mode !
	//cflip is required when exploding triangles for triangle sorting
	//gcflip is global clip flip, needed for when rendering to texture due to mirrored Y direction
	SetCull(gp->isp.CullMode ^ gcflip);

	//set Z mode, only if required
	if (Type == ListType_Punch_Through || (pass == Pass::Depth && SortingEnabled))
	{
		glcache.DepthFunc(GL_GEQUAL);
	}
	else if (Type == ListType_Opaque || (Type == ListType_Translucent && !SortingEnabled))
	{
		glcache.DepthFunc(Zfunction[gp->isp.DepthMode]);
	}

	if (pass == Pass::Depth || pass == Pass::Color)
	{
		// Z Write Disable seems to be ignored for punch-through polys
		// Fixes Worms World Party, Bust-a-Move 4 and Re-Volt
		if (Type == ListType_Punch_Through)
			glcache.DepthMask(GL_TRUE);
		else
			glcache.DepthMask(!gp->isp.ZWriteDis);
	}
	else
		glcache.DepthMask(GL_FALSE);
}

template <u32 Type, bool SortingEnabled, Pass pass>
static void DrawList(const List<PolyParam>& gply, int first, int count)
{
	PolyParam* params = &gply.head()[first];

	if (count == 0)
		return;

	while (count-- > 0)
	{
		if (params->count > 2)
		{
			if ((Type == ListType_Opaque || (Type == ListType_Translucent && !SortingEnabled)) && params->isp.DepthMode == 0)
			{
				// depthFunc = never
				params++;
				continue;
			}
			if (pass == Pass::OIT && Type == ListType_Translucent && params->tsp.SrcInstr == 0 && params->tsp.DstInstr == 1)
			{
				// dst = dst
				params++;
				continue;
			}
			/* (of a frame decoded on the GPU, a draw group's translucent
			 * strips are numbered by their draws: TA_GPU_INSTANCE) */
			gl4ShaderUniforms.poly_number = pvrrc.gpu ? 0 : params - gply.head();
			SetGPState<Type, SortingEnabled, pass>(params);
			if (pvrrc.gpu)
				glTaDecoder.DrawGroup(Type, params->first, params->count);
			else
				glDrawElements(GL_TRIANGLE_STRIP, params->count, GL_UNSIGNED_INT, (GLvoid*)(sizeof(u32) * params->first));
			glCheck();
		}

		params++;
	}
}

void gl4SetupMainVBO()
{
	glBindVertexArray(gl4.vbo.main_vao);

	glBindBuffer(GL_ARRAY_BUFFER, gl4.vbo.geometry);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl4.vbo.idxs);

	//setup vertex buffers attrib pointers
	glEnableVertexAttribArray(VERTEX_POS_ARRAY);
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex,x));

	glEnableVertexAttribArray(VERTEX_COL_BASE_ARRAY);
	glVertexAttribPointer(VERTEX_COL_BASE_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex,col));

	glEnableVertexAttribArray(VERTEX_COL_OFFS_ARRAY);
	glVertexAttribPointer(VERTEX_COL_OFFS_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex,vtx_spc));

	glEnableVertexAttribArray(VERTEX_UV_ARRAY);
	glVertexAttribPointer(VERTEX_UV_ARRAY, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex,u));
  
	glEnableVertexAttribArray(VERTEX_COL_BASE1_ARRAY); glCheck();
	glVertexAttribPointer(VERTEX_COL_BASE1_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex, col1)); glCheck();

	glEnableVertexAttribArray(VERTEX_COL_OFFS1_ARRAY); glCheck();
	glVertexAttribPointer(VERTEX_COL_OFFS1_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex, spc1)); glCheck();

	glEnableVertexAttribArray(VERTEX_UV1_ARRAY); glCheck();
	glVertexAttribPointer(VERTEX_UV1_ARRAY, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, u1)); glCheck();
}

/* What the vertex arrays and storage binding 1 have: -1 not known, 0 the
 * renderer's own buffers, 1 the GPU decode's */
static int frame_buffers = -1;

void gl4ForgetFrameBuffers()
{
	frame_buffers = -1;
}

void gl4BindFrameBuffers(bool gpu)
{
	if (frame_buffers == (gpu ? 1 : 0))
		return;
	frame_buffers = gpu ? 1 : 0;
	if (!gpu)
	{
		gl4SetupMainVBO();
		glDisableVertexAttribArray(VERTEX_PP_ARRAY);
		// (an attribute without its array: the number is 0)
		gl4.vertexAttribI4ui(VERTEX_PP_ARRAY, 0, 0, 0, 0);
		gl4SetupModvolVBO();
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gl4.vbo.tr_poly_params);
		glBindVertexArray(gl4.vbo.main_vao);
		return;
	}
	const GlTaDraw& d = glTaDecoder.Draw();
	glBindVertexArray(gl4.vbo.main_vao);
	glTaDecoder.BindGeometry();
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex,x));
	glVertexAttribPointer(VERTEX_COL_BASE_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex,col));
	glVertexAttribPointer(VERTEX_COL_OFFS_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex,vtx_spc));
	glVertexAttribPointer(VERTEX_UV_ARRAY, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex,u));
	glVertexAttribPointer(VERTEX_COL_BASE1_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex, col1));
	glVertexAttribPointer(VERTEX_COL_OFFS1_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex, spc1));
	glVertexAttribPointer(VERTEX_UV1_ARRAY, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, u1));
	// a strip's number: one a draw, the draw record's baseInstance picking it
	glBindBuffer(GL_ARRAY_BUFFER, d.instances);
	glVertexAttribIPointer(VERTEX_PP_ARRAY, 1, GL_UNSIGNED_INT, sizeof(u32), (void*)0);
	gl4.vertexAttribDivisor(VERTEX_PP_ARRAY, 1);
	glEnableVertexAttribArray(VERTEX_PP_ARRAY);
	glBindVertexArray(gl4.vbo.modvol_vao);
	glTaDecoder.BindModVols();
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, (void*)0);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, d.trParams);
	glBindVertexArray(gl4.vbo.main_vao);
}

void gl4SetupModvolVBO()
{
	glBindVertexArray(gl4.vbo.modvol_vao);

	glBindBuffer(GL_ARRAY_BUFFER, gl4.vbo.modvols);

	//setup vertex buffers attrib pointers
	glEnableVertexAttribArray(VERTEX_POS_ARRAY); glCheck();
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(float)*3, (void*)0); glCheck();
}

static void DrawModVols(int first, int count)
{
	/* A bit of explanation:
	* In theory it works like this: generate a 1-bit stencil for each polygon
	* volume, and then AND or OR it against the overall 1-bit tile stencil at 
	* the end of the volume. */

	if (count == 0)
		return;

	glBindVertexArray(gl4.vbo.modvol_vao);

	glcache.Disable(GL_BLEND);
	SetBaseClipping();

	glcache.UseProgram(gl4.modvol_shader.program);

	glcache.Enable(GL_DEPTH_TEST);
	glcache.DepthMask(GL_FALSE);
	glcache.DepthFunc(GL_GREATER);

	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

	ModifierVolumeParam* params = &pvrrc.global_param_mvo.head()[first];

	int mod_base = -1;
	u32 clip = 0;	// what the scissor is set for: so far nothing, the whole picture

	for (int cmv = 0; cmv < count; cmv++)
	{
		ModifierVolumeParam& param = params[cmv];
		u32 mvFirst, mvCount;
		mv_clip_range_of(pvrrc, pvrrc.global_param_mvo.head(), param, mvFirst, mvCount);

		if (mvCount == 0)
			continue;

		u32 mv_mode = param.isp.DepthMode;

		/* A volume is clipped like a polygon, to the rectangle it was sent
		 * under if it asked to be: its shadow fell outside it. (Kept in:
		 * the scissor. Kept out is not done, as it is not for upstream.) */
		if (param.tileclip != clip)
		{
			int clip_rect[4];
			clip = param.tileclip;
			if (GetTileClip(clip, ViewportMatrix, clip_rect) == TileClipping::Outside)
				gl4Scissor(clip_rect[0], clip_rect[1], clip_rect[2], clip_rect[3]);
			else
				SetBaseClipping();
		}

		if (mod_base == -1)
			mod_base = mvFirst;

		if (!param.isp.VolumeLast && mv_mode > 0)
			SetMVS_Mode(Or, param.isp);		// OR'ing (open volume or quad)
		else
			SetMVS_Mode(Xor, param.isp);	// XOR'ing (closed volume)

		glDrawArrays(GL_TRIANGLES, mvFirst * 3, mvCount * 3);

		if (mv_mode == 1 || mv_mode == 2)
		{
			// Sum the area
			SetMVS_Mode(mv_mode == 1 ? Inclusion : Exclusion, param.isp);
			glDrawArrays(GL_TRIANGLES, mod_base * 3, (mvFirst + mvCount - mod_base) * 3);
			mod_base = -1;
		}
	}
	SetBaseClipping();
	//restore states
	glBindVertexArray(gl4.vbo.main_vao);
	glcache.Enable(GL_DEPTH_TEST);
	glcache.DepthMask(GL_TRUE);
}

void renderABuffer();
void DrawTranslucentModVols(int first, int count, bool useOpaqueGeom);

static GLuint CreateColorFBOTexture(int width, int height)
{
	GLuint texId = glcache.GenTexture();
	glcache.BindTexture(GL_TEXTURE_2D, texId);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texId, 0);
	glCheck();

	return texId;
}

/* The working buffer's colour is the screen's last picture: see gl4DrawStrips(). */
static bool working_holds_screen;

void gl4CreateTextures(int width, int height)
{
	working_holds_screen = false;
	if (geom_fbo == 0)
	{
		glGenFramebuffers(1, &geom_fbo);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, geom_fbo);

	stencilTexId = glcache.GenTexture();
	glBindTexture(GL_TEXTURE_2D, stencilTexId); glCheck();
	glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);		// OpenGL >= 4.3
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	// Using glTexStorage2D instead of glTexImage2D to satisfy requirement GL_TEXTURE_IMMUTABLE_FORMAT=true, needed for glTextureView below
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH32F_STENCIL8, width, height);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, stencilTexId, 0); glCheck();

	opaqueTexId = CreateColorFBOTexture(width, height);

	depthTexId = glcache.GenTexture();
	glTextureView(depthTexId, GL_TEXTURE_2D, stencilTexId, GL_DEPTH32F_STENCIL8, 0, 1, 0, 1);
	glCheck();
	glBindTexture(GL_TEXTURE_2D, depthTexId);
	glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glCheck();

	GLuint uStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	verify(uStatus == GL_FRAMEBUFFER_COMPLETE);
}

void gl4DrawStrips(GLuint output_fbo, int width, int height)
{
	glBindFramebuffer(GL_FRAMEBUFFER, geom_fbo);
	if (texSamplers[0] == 0)
		glGenSamplers(2, texSamplers);

	if (!pvrrc.isRTT && pvrrc.clearFramebuffer)
	{
		// a framebuffer the game has not drawn to lately: nothing of the last picture belongs in it
		glcache.Disable(GL_SCISSOR_TEST);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glcache.ClearColor(VO_BORDER_COL.Red / 255.f, VO_BORDER_COL.Green / 255.f, VO_BORDER_COL.Blue / 255.f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT);
		if (gl4ShaderUniforms.base_clipping.enabled)
			glcache.Enable(GL_SCISSOR_TEST);
	}
	else if (!pvrrc.isRTT && !working_holds_screen)
	{
		/* A render that covers only part of the screen leaves the rest as
		 * it was - which is as it is in this working buffer, and that is
		 * the screen's last picture only if nothing else has been drawn
		 * through it since. A render to a texture has, or the buffer is
		 * new: the screen's picture is put back first. (Every frame would
		 * do, as upstream has it; it is only needed then.) */
		glcache.Disable(GL_SCISSOR_TEST);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, geom_fbo);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, output_fbo);
		glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_FRAMEBUFFER, geom_fbo);
		if (gl4ShaderUniforms.base_clipping.enabled)
			glcache.Enable(GL_SCISSOR_TEST);
	}
	working_holds_screen = !pvrrc.isRTT;
	glcache.DepthMask(GL_TRUE);
	glClearDepth(0.0);
	glcache.StencilMask(0xFF);
	glClear(GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glCheck();

	//Draw the strips !

	//We use sampler 0
	glActiveTexture(GL_TEXTURE0);
	glProvokingVertex(GL_LAST_VERTEX_CONVENTION);

	/* In bands, when the frames before had more translucent fragments than
	 * the lists have room for (abuffer.cpp): all of the frame's render
	 * passes once a band, every draw and clear cut to the band by the
	 * scissor, each band's lists starting empty and put on the picture
	 * before the next band's. Nothing is dropped and nothing is allocated;
	 * the opaque polygons are drawn once a band. */
	const u32 bands = std::max<u32>(std::min<u32>(gl4OitChooseBands(pvrrc.isRTT), (u32)std::max(height, 1)), 1);
	const int bandHeight = (height + (int)bands - 1) / (int)bands;
	const u32 bandCount = (u32)((height + bandHeight - 1) / bandHeight);
	int render_pass_count = pvrrc.render_passes.used();

	for (u32 band = 0; band < bandCount; band++)
	{
	gl4band.y = (int)band * bandHeight;
	gl4band.height = bandCount > 1 ? std::min(bandHeight, height - gl4band.y) : 0;
	gl4OitBandBegin();
	glBindFramebuffer(GL_FRAMEBUFFER, geom_fbo);
	// (the modifier volumes of the band before left the stencil mask at their bits)
	glcache.StencilMask(0xFF);
	RenderPass previous_pass = {};

	for (int render_pass = 0; render_pass < render_pass_count; render_pass++)
	{
		const RenderPass& current_pass = pvrrc.render_passes.head()[render_pass];

		// Check if we can skip this pass, in part or completely, in case nothing is drawn (Cosmic Smash)
		bool skip_op_pt = true;
		bool skip_tr = true;
		for (u32 j = previous_pass.op_count; skip_op_pt && j < current_pass.op_count; j++)
		{
			if (pvrrc.global_param_op.head()[j].count > 2)
				skip_op_pt = false;
		}
		for (u32 j = previous_pass.pt_count; skip_op_pt && j < current_pass.pt_count; j++)
		{
			if (pvrrc.global_param_pt.head()[j].count > 2)
				skip_op_pt = false;
		}
		for (u32 j = previous_pass.tr_count; skip_tr && j < current_pass.tr_count; j++)
		{
			if (pvrrc.global_param_tr.head()[j].count > 2)
				skip_tr = false;
		}
		if (skip_op_pt && skip_tr)
		{
			previous_pass = current_pass;
			continue;
		}
		DEBUG_LOG(RENDERER, "Render pass %d/%d OP %d PT %d TR %d autosort %d", render_pass + 1, render_pass_count,
				current_pass.op_count - previous_pass.op_count,
				current_pass.pt_count - previous_pass.pt_count,
				current_pass.tr_count - previous_pass.tr_count,
				current_pass.autosort);

		glBindVertexArray(gl4.vbo.main_vao);

		if (!skip_op_pt)
		{
			//
			// PASS 1: Geometry pass to update depth and stencil
			//
			if (render_pass > 0)
			{
				// Make a copy of the depth buffer that will be reused in pass 2
				if (depth_fbo == 0)
					glGenFramebuffers(1, &depth_fbo);
				glBindFramebuffer(GL_FRAMEBUFFER, depth_fbo);
				if (depthSaveTexId == 0)
				{
					depthSaveTexId = glcache.GenTexture();
					glBindTexture(GL_TEXTURE_2D, depthSaveTexId);
					glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
					glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH32F_STENCIL8, max_image_width, max_image_height, 0, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, NULL); glCheck();
					glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depthSaveTexId, 0); glCheck();
				}
				GLuint uStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
				verify(uStatus == GL_FRAMEBUFFER_COMPLETE);

				glBindFramebuffer(GL_READ_FRAMEBUFFER, geom_fbo);
				glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
				glCheck();

				glBindFramebuffer(GL_FRAMEBUFFER, geom_fbo);
			}
			glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
			glcache.Enable(GL_DEPTH_TEST);
			glcache.DepthMask(GL_TRUE);
			glcache.Enable(GL_STENCIL_TEST);
			glcache.StencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

			DrawList<ListType_Opaque, false, Pass::Depth>(pvrrc.global_param_op, previous_pass.op_count, current_pass.op_count - previous_pass.op_count);
			DrawList<ListType_Punch_Through, false, Pass::Depth>(pvrrc.global_param_pt, previous_pass.pt_count, current_pass.pt_count - previous_pass.pt_count);

			// Modifier volumes
			DrawModVols(previous_pass.mvo_count, current_pass.mvo_count - previous_pass.mvo_count);

			//
			// PASS 2: Render OP and PT to fbo
			//
			if (render_pass == 0)
			{
				glcache.DepthMask(GL_TRUE);
				glClearDepth(0.0);
				glClear(GL_DEPTH_BUFFER_BIT);
			}
			else
			{
				// Restore the depth buffer from the last render pass
				// FIXME This is pretty slow apparently (CS)
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, geom_fbo);
				glBindFramebuffer(GL_READ_FRAMEBUFFER, depth_fbo);
				glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
				glCheck();
				glBindFramebuffer(GL_FRAMEBUFFER, geom_fbo);
			}

			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			glcache.Disable(GL_STENCIL_TEST);

			// Bind stencil buffer for the fragment shader (shadowing)
			glActiveTexture(GL_TEXTURE3);
			glBindTexture(GL_TEXTURE_2D, stencilTexId);
			glActiveTexture(GL_TEXTURE0);
			glCheck();

			//Opaque
			DrawList<ListType_Opaque, false, Pass::Color>(pvrrc.global_param_op, previous_pass.op_count, current_pass.op_count - previous_pass.op_count);

			//Alpha tested
			DrawList<ListType_Punch_Through, false, Pass::Color>(pvrrc.global_param_pt, previous_pass.pt_count, current_pass.pt_count - previous_pass.pt_count);

			// Unbind stencil
			glActiveTexture(GL_TEXTURE3);
			glBindTexture(GL_TEXTURE_2D, 0);
			glActiveTexture(GL_TEXTURE0);
		}

		if (!skip_tr)
		{
			//
			// PASS 3: Render TR to a-buffers
			//
			if (current_pass.autosort)
			{
				glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
				glcache.Disable(GL_DEPTH_TEST);

				// Although the depth test is disabled and thus writes to the depth buffer are also disabled,
				// AMD cards have serious issues when the depth/stencil texture is still bound to the framebuffer
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
				glActiveTexture(GL_TEXTURE2);
				glBindTexture(GL_TEXTURE_2D, depthTexId);
				glActiveTexture(GL_TEXTURE0);
				DrawList<ListType_Translucent, true, Pass::OIT>(pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count - previous_pass.tr_count);

				// Translucent modifier volumes
				SetBaseClipping();
				if (current_pass.mv_op_tr_shared)
					DrawTranslucentModVols(previous_pass.mvo_count, current_pass.mvo_count - previous_pass.mvo_count, true);
				else
					DrawTranslucentModVols(previous_pass.mvo_tr_count, current_pass.mvo_tr_count - previous_pass.mvo_tr_count, false);

				// Rebind the depth/stencil texture to the framebuffer
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, stencilTexId, 0);

				if (render_pass < render_pass_count - 1)
				{
					//
					// PASS 3b: Geometry pass with TR to update the depth for the next TA render pass
					//
					// Unbind depth texture
					glActiveTexture(GL_TEXTURE2);
					glBindTexture(GL_TEXTURE_2D, 0);
					glActiveTexture(GL_TEXTURE0);

					glcache.Enable(GL_DEPTH_TEST);
					DrawList<ListType_Translucent, true, Pass::Depth>(pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count - previous_pass.tr_count);
				}
			}
			else
			{
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				glcache.Enable(GL_DEPTH_TEST);
				DrawList<ListType_Translucent, false, Pass::Color>(pvrrc.global_param_tr, previous_pass.tr_count, current_pass.tr_count - previous_pass.tr_count);
				glcache.Disable(GL_BLEND);
			}
			glCheck();

			if (render_pass < render_pass_count - 1)
			{
				//
				// PASS 3c: Render a-buffer to temporary texture
				//
				GLuint texId = CreateColorFBOTexture(max_image_width, max_image_height);

				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

				glActiveTexture(GL_TEXTURE0);
				glBindSampler(0, 0);
				glBindTexture(GL_TEXTURE_2D, opaqueTexId);

				renderABuffer();

				glcache.DeleteTextures(1, &opaqueTexId);
				opaqueTexId = texId;

				glCheck();
			}
		}

		if (!skip_op_pt && render_pass < render_pass_count - 1)
		{
			// Clear the stencil from this pass
			glcache.StencilMask(0xFF);
			glClear(GL_STENCIL_BUFFER_BIT);
		}

		previous_pass = current_pass;
	}

	//
	// PASS 4: Render a-buffers to screen
	//
	glBindFramebuffer(GL_FRAMEBUFFER, output_fbo); glCheck();
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	glActiveTexture(GL_TEXTURE0);
	glBindSampler(0, 0);
	glBindTexture(GL_TEXTURE_2D, opaqueTexId);
	renderABuffer();
	gl4OitBandEnd(band);
	}
	gl4band.height = 0;
	gl4OitFrameEnd(bandCount, pvrrc.isRTT);
}

static void gl4_draw_quad_texture(GLuint texture, float w, float h)
{
 	glcache.Disable(GL_SCISSOR_TEST);
	glcache.Disable(GL_DEPTH_TEST);
	glcache.Disable(GL_STENCIL_TEST);
	glcache.Disable(GL_CULL_FACE);
	glcache.Disable(GL_BLEND);

	gl4ShaderUniforms.trilinear_alpha = 1.0;

 	CurrentShader = gl4GetProgram(false,
				false,
				true,
				false,
				true,
				0,
				false,
				2,
				false,
				false,
				false,
				false,
				false,
				Pass::Color);
	glcache.UseProgram(CurrentShader->program);
	gl4ShaderUniforms.Set(CurrentShader);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	gl4BindFrameBuffers(false);

	struct Vertex vertices[] = {
		{ 0,     0 + h, 1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 0, 1 },
		{ 0,     0,     1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 0, 0 },
		{ 0 + w, 0 + h, 1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 1, 1 },
		{ 0 + w, 0,     1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 1, 0 },
	};
	GLushort indices[] = { 0, 1, 2, 1, 3 };

	glBindVertexArray(gl4.vbo.main_vao);
	glBindBuffer(GL_ARRAY_BUFFER, gl4.vbo.geometry);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl4.vbo.idxs);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STREAM_DRAW);

	glDrawElements(GL_TRIANGLE_STRIP, 5, GL_UNSIGNED_SHORT, (void *)0);
}

void gl4DrawFramebuffer(float w, float h)
{
	gl4_draw_quad_texture(fbTextureId, w, h);
	glcache.DeleteTextures(1, &fbTextureId);
	fbTextureId = 0;
}

void gl4DrawVmuTexture(u8 vmu_screen_number)
{
	glActiveTexture(GL_TEXTURE0);

	float x=0 ;
	float y=0 ;
	float w=VMU_SCREEN_WIDTH*vmu_screen_params[vmu_screen_number].vmu_screen_size_mult ;
	float h=VMU_SCREEN_HEIGHT*vmu_screen_params[vmu_screen_number].vmu_screen_size_mult ;

	// A new picture from the game, new colours from the options, or no texture yet
	const bool vmu_lcd_changed = vmu_lcd_refresh(vmu_screen_number);
	if (vmu_lcd_changed || vmu_screen_params[vmu_screen_number].vmu_screen_needs_update || vmuTextureId[vmu_screen_number] == 0)
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

   gl4BindFrameBuffers(false);
   glBindVertexArray(gl4.vbo.main_vao);

	gl4ShaderUniforms.trilinear_alpha = 1.0;

	CurrentShader = gl4GetProgram(false,
				0,
				true,
				true,
				false,
				0,
				false,
				2,
				false,
				false,
				false,
				false,
				false,
				Pass::Color);
	glcache.UseProgram(CurrentShader->program);
	gl4ShaderUniforms.Set(CurrentShader);

	{
		struct Vertex vertices[] = {
				{ x,   y+h, 1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 0, 1 },
				{ x,   y,   1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 0, 0 },
				{ x+w, y+h, 1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 1, 1 },
				{ x+w, y,   1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 1, 0 },
		};
		GLushort indices[] = { 0, 1, 2, 1, 3 };

		glBindBuffer(GL_ARRAY_BUFFER, gl4.vbo.geometry);
		glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl4.vbo.idxs);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STREAM_DRAW);
	}

	glDrawElements(GL_TRIANGLE_STRIP, 5, GL_UNSIGNED_SHORT, (void *)0);
}

void gl4UpdateLightGunTexture(int port)
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

void gl4DrawGunCrosshair(u8 port)
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

	if (lightgun_params[port].dirty || lightgunTextureId[port] == 0)
		gl4UpdateLightGunTexture(port);

	glcache.BindTexture(GL_TEXTURE_2D, lightgunTextureId[port]);

	glcache.Disable(GL_SCISSOR_TEST);
	glcache.Disable(GL_DEPTH_TEST);
	glcache.Disable(GL_STENCIL_TEST);
	glcache.Disable(GL_CULL_FACE);
   glcache.Enable(GL_BLEND);
	glcache.BlendFunc(GL_SRC_ALPHA, GL_ONE);

	gl4BindFrameBuffers(false);
	glBindVertexArray(gl4.vbo.main_vao);

	gl4ShaderUniforms.trilinear_alpha = 1.0;
	CurrentShader = gl4GetProgram(false,
				0,
				true,
				true,
				false,
				0,
				false,
				2,
				false,
				false,
				false,
				false,
				false,
				Pass::Color);
	glcache.UseProgram(CurrentShader->program);
	gl4ShaderUniforms.Set(CurrentShader);

	{
		struct Vertex vertices[] = {
				{ x,   y+h, 1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 0, 1 },
				{ x,   y,   1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 0, 0 },
				{ x+w, y+h, 1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 1, 1 },
				{ x+w, y,   1, { 255, 255, 255, 255 }, { 0, 0, 0, 0 }, 1, 0 },
		};
		GLushort indices[] = { 0, 1, 2, 1, 3 };

		glBindBuffer(GL_ARRAY_BUFFER, gl4.vbo.geometry);
		glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl4.vbo.idxs);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STREAM_DRAW);
	}

	glDrawElements(GL_TRIANGLE_STRIP, 5, GL_UNSIGNED_SHORT, (void *)0);

	glcache.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
