#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
#include <math.h>
#include "rend/rtt_watch.h"
#include "rend/last_picture.h"
#include <algorithm>

#include <libretro.h>

#include "gles.h"
#include "gl_texdecoder.h"
#include "hw/pvr/pvr_mem.h"

#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
/* (the pool's formats: desktop OpenGL's, which the OpenGL ES 2 headers have not) */
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_RGB8
#define GL_RGB8 0x8051
#endif
#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_UNSIGNED_SHORT_1_5_5_5_REV
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4_REV
#define GL_UNSIGNED_SHORT_4_4_4_4_REV 0x8365
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

#ifndef GL_IMPLEMENTATION_COLOR_READ_TYPE
#define GL_IMPLEMENTATION_COLOR_READ_TYPE 0x8B9A
#endif

#ifndef GL_IMPLEMENTATION_COLOR_READ_FORMAT
#define GL_IMPLEMENTATION_COLOR_READ_FORMAT 0x8B9B
#endif

/*
Textures

Textures are converted to native OpenGL textures
The mapping is done with tcw:tsp -> GL texture. That includes stuff like
filtering/ texture repeat

To save space native formats are used for 1555/565/4444 (only bit shuffling is done)
YUV is converted to 8888
PALs are decoded to their unpaletted format (5551/565/4444/8888 depending on palette type)

Compression
	look into it, but afaik PVRC is not realtime doable
*/

#if FEAT_HAS_SOFTREND
	#include <xmmintrin.h>
#endif

extern u32 decoded_colors[3][65536];
GlTextureCache TexCache;
GLTexturePool glTexPool;

extern "C" struct retro_hw_render_callback hw_render;

/* ---- The texture pool ---- */

/* By TextureType, and the pictures rendered to: the internal format, and
 * the format and type of what is uploaded */
static const GLenum poolInternal[GLTexturePool::FORMATS] = { GL_RGB565, GL_RGB5_A1, GL_RGBA4, GL_RGBA8, GL_R8, GL_RGBA8, GL_RGB8 };
static const GLenum poolFormat[GLTexturePool::FORMATS] = { GL_RGB, GL_RGBA, GL_RGBA, GL_RGBA, GL_RED, GL_RGBA, GL_RGB };
static const GLenum poolType[GLTexturePool::FORMATS] = {
	GL_UNSIGNED_SHORT_5_6_5, GL_UNSIGNED_SHORT_5_5_5_1, GL_UNSIGNED_SHORT_4_4_4_4, GL_UNSIGNED_BYTE, GL_UNSIGNED_BYTE, GL_UNSIGNED_BYTE, GL_UNSIGNED_BYTE
};
static const u32 poolTexel[GLTexturePool::FORMATS] = { 2, 2, 2, 4, 1, 4, 4 };
static const char * const poolNames[GLTexturePool::FORMATS] = { "565", "5551", "4444", "8888", "8-bit", "RGBA8 rendered to", "RGB8 rendered to" };

void GLTexturePool::Init()
{
	if (ready)
		return;
	if (gl.is_gles || gl.gl_major < 3)
		return;
	storage = gl.gl_major > 4 || (gl.gl_major == 4 && gl.gl_minor >= 2);
	book.Reset(1);
	// how much games use them, in eighths of what they use 565
	static const u32 use[FORMATS] = { 8, 4, 4, 4, 2, 0, 0 };
	static const u8 kind[FORMATS] = { 0, 0, 0, 0, 0, TexPoolBook::TARGETS, TexPoolBook::TARGETS };
	u32 counts[TexPoolBook::CLASSES];
	const u64 bytes = TexPoolBook::Plan(gl.vram_budget, poolTexel, use, kind, FORMATS, counts);
	ready = true;
	while (glGetError() != GL_NO_ERROR)
		;
	for (int c = 0; c < TexPoolBook::CLASSES; c++)
		for (u32 i = 0; i < counts[c]; i++)
		{
			GLPooledTexture *t = Make(c, false);
			if (t == nullptr)
				break;
			book.Give(t);
		}
	NOTICE_LOG(RENDERER, "OpenGL: texture pool of %u textures, %u MB (planned %u MB)", book.made, (u32)(book.bytesMade >> 20), (u32)(bytes >> 20));
}

void GLTexturePool::Term()
{
	if (!ready)
		return;
	book.Summary("OpenGL");
	while (all != nullptr)
	{
		GLPooledTexture *next = all->all;
		glcache.DeleteTextures(1, &all->name);
		delete all;
		all = next;
	}
	book.Reset(1);
	ready = false;
}

GLPooledTexture *GLTexturePool::Make(int cls, bool late)
{
	const u32 f = TexPoolBook::Format(cls);
	const u32 width = TexPoolBook::Width(cls), height = TexPoolBook::Height(cls);
	GLPooledTexture *t = new GLPooledTexture();
	t->levels = TexPoolBook::Mips(cls) ? 1 + bitscanrev(std::max(width, height)) : 1;
	glGenTextures(1, &t->name);
	glcache.BindTexture(GL_TEXTURE_2D, t->name);
#ifndef HAVE_OPENGLES2
	if (storage)
		glTexStorage2D(GL_TEXTURE_2D, t->levels, poolInternal[f], width, height);
	else
#endif
	{
		for (u32 l = 0; l < t->levels; l++)
			glTexImage2D(GL_TEXTURE_2D, l, poolInternal[f], std::max(width >> l, 1u), std::max(height >> l, 1u), 0, poolFormat[f], poolType[f], nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, t->levels - 1);
	}
	if (glGetError() != GL_NO_ERROR)
	{
		WARN_LOG(RENDERER, "OpenGL: texture pool: a %ux%u %s texture could not be made", width, height, poolNames[f]);
		glcache.DeleteTextures(1, &t->name);
		delete t;
		return nullptr;
	}
	book.Made(t, cls, TexPoolBook::Bytes(cls, poolTexel[f]), late, poolNames[f]);
	t->all = all;
	all = t;
	return t;
}

GLPooledTexture *GLTexturePool::TakeClass(int cls)
{
	if (!ready || cls < 0)
		return nullptr;
	TexPoolEntry *e = book.Take(cls);
	return e != nullptr ? static_cast<GLPooledTexture *>(e) : Make(cls, true);
}

GLPooledTexture *GLTexturePool::Take(TextureType type, u32 width, u32 height, bool mips)
{
	if ((unsigned)type >= 5)
		return nullptr;
	return TakeClass(TexPoolBook::Class((u32)type, width, height, mips));
}

GLPooledTexture *GLTexturePool::TakeTarget(bool rgb, u32 width, u32 height)
{
	return TakeClass(TexPoolBook::Class(rgb ? TARGET_RGB : TARGET_RGBA, width, height, false));
}

GLPooledTexture *GLTexturePool::Find(GLuint name) const
{
	for (GLPooledTexture *t = all; t != nullptr; t = t->all)
		if (t->name == name)
			return t;
	return nullptr;
}

void gl_tex_init(bool depth)
{
	glTexDecoder.Init();
	glTexPool.Init();
	gl.rtt.kept = false;
	gl.rtt.pooled = nullptr;
	if (!glTexPool.Ready())
		return;
	/* The framebuffer renders to textures are drawn through, and their
	 * depth buffer, now and for as long as the renderer is: as large as
	 * the largest texture of the pool's, at the scale they are made at */
	glGenFramebuffers(1, &gl.rtt.fbo);
	gl.rtt.depthWidth = gl.rtt.depthHeight = 0;
	if (depth)
	{
		GLint most = 0;
		glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &most);
		const u32 scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
		const u32 side = std::min<u32>(1024 * scale, (u32)std::max(most, 1024));
		glGenRenderbuffers(1, &gl.rtt.depthb);
		glBindRenderbuffer(RARCH_GL_RENDERBUFFER, gl.rtt.depthb);
		glRenderbufferStorage(RARCH_GL_RENDERBUFFER, RARCH_GL_DEPTH24_STENCIL8, side, side);
		gl.rtt.depthWidth = gl.rtt.depthHeight = side;
	}
	gl.rtt.kept = true;
}

void gl_tex_term()
{
	if (gl.rtt.kept)
	{
		if (gl.rtt.fbo)
			glDeleteFramebuffers(1, &gl.rtt.fbo);
		if (gl.rtt.depthb)
			glDeleteRenderbuffers(1, &gl.rtt.depthb);
		gl.rtt.fbo = gl.rtt.depthb = 0;
		gl.rtt.kept = false;
	}
	if (gl.rtt.pooled != nullptr)
	{
		glTexPool.Give(gl.rtt.pooled);
		gl.rtt.pooled = nullptr;
		gl.rtt.tex = 0;
	}
	glTexPool.Term();
	glTexDecoder.Term();
}

void gl_tex_flush()
{
	glTexDecoder.Flush();
}

bool TextureCacheData::TakePooled(u32 width, u32 height, bool mips)
{
	if (!glTexPool.Ready() || texID == 0)
		return false;
	if (pooled != nullptr && (int)pooled->cls == TexPoolBook::Class((u32)tex_type, width, height, mips))
		return true;
	GLPooledTexture *t = glTexPool.Take(tex_type, width, height, mips);
	if (t == nullptr)
		return false;
	if (pooled != nullptr)
		glTexPool.Give(pooled);
	else
		ownID = texID;
	pooled = t;
	texID = t->name;
	return true;
}

void TextureCacheData::ReleasePooled()
{
	if (pooled == nullptr)
		return;
	glTexPool.Give(pooled);
	pooled = nullptr;
	texID = ownID != 0 ? ownID : glcache.GenTexture();
	ownID = 0;
}

void TextureCacheData::CancelDecode()
{
	if (decodeSerial != 0)
		glTexDecoder.Cancel(decodeJob, decodeSerial);
	decodeSerial = 0;
}

/* Made on the GPU (gl_texdecoder.h), or copied as it is. Everything that
 * can say no does before anything is changed but the texture it is made
 * into, which a no leaves for the CPU's upload to take. */
bool TextureCacheData::GpuDecode(const texd_plan& plan)
{
	if (texID == 0)
		return false;
	CancelDecode();
	if (plan.native)
	{
		const u32 stride = plan.level[0].sr & 0xFFFF;
		if ((unsigned)tex_type >= 3)
			return false;
		if (glTexDecoder.Native(tex_type))
		{
			/* Through the ring, into the pool's texture: the PowerVR's
			 * texels are OpenGL's 5-6-5, 1-5-5-5 reversed and 4-4-4-4
			 * reversed as they are */
			u32 offset;
			if (!TakePooled(plan.width, plan.height, false))
				return false;
			u8 * const to = glTexDecoder.Begin(plan.bytes, 0, &offset);
			if (to == nullptr)
				return false;
			memcpy(to, &vram[plan.src], plan.bytes);
			decodeSerial = glTexDecoder.Serial();
			decodeJob = glTexDecoder.AddCopy(texID, tex_type, offset + plan.level[0].data, stride, plan.width, plan.height);
			return true;
		}
		if (glTexPool.Ready())
			return false;
		/* Straight from video memory: OpenGL ES has 5-6-5 alone, and no row
		 * length but the texture's */
		if (gl.is_gles && (tex_type != TextureType::_565 || stride != plan.width))
			return false;
		GLenum format = GL_RGB, type = GL_UNSIGNED_SHORT_5_6_5, internal = GL_RGB;
		if (tex_type == TextureType::_5551)
		{
			format = GL_BGRA;
			type = GL_UNSIGNED_SHORT_1_5_5_5_REV;
			internal = GL_RGBA;
		}
		else if (tex_type == TextureType::_4444)
		{
			format = GL_BGRA;
			type = GL_UNSIGNED_SHORT_4_4_4_4_REV;
			internal = GL_RGBA;
		}
		glcache.BindTexture(GL_TEXTURE_2D, texID);
		if (stride != plan.width)
			glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
		glTexImage2D(GL_TEXTURE_2D, 0, internal, plan.width, plan.height, 0, format, type, &vram[plan.src + plan.level[0].data]);
		if (stride != plan.width)
			glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		return true;
	}
	if (!glTexDecoder.Takes(tex_type, plan.out) || !TakePooled(plan.width, plan.height, plan.mipmapped != 0)
			|| pooled->levels < plan.levels)
		return false;
	u32 offset;
	u8 * const to = glTexDecoder.Begin(plan.pal + plan.pal_entries * 4, plan.levels, &offset);
	if (to == nullptr)
		return false;
	memcpy(to, &vram[plan.src], plan.bytes);
	if (plan.pal_entries != 0)
		memcpy(to + plan.pal, (plan.out == TEXD_OUT16 ? palette16_ram : palette32_ram) + plan.pal_index, plan.pal_entries * 4);
	decodeSerial = glTexDecoder.Serial();
	decodeJob = glTexDecoder.AddDecode(texID, tex_type, plan, offset);
	return true;
}

void TextureCacheData::Renew()
{
	CancelDecode();
	if (pooled != nullptr)
		ReleasePooled();
	else
	{
		glcache.DeleteTextures(1, &texID);
		texID = glcache.GenTexture();
	}
}

void TextureCacheData::SetRendered(GLuint tex, GLPooledTexture *pooled)
{
	CancelDecode();
	if (this->pooled != nullptr)
	{
		glTexPool.Give(this->pooled);
		this->pooled = nullptr;
		if (ownID != 0)
			glcache.DeleteTextures(1, &ownID);
		ownID = 0;
	}
	else if (texID != 0)
		glcache.DeleteTextures(1, &texID);
	this->pooled = pooled;
	texID = tex;
}

void TextureCacheData::UploadToGPU(int width, int height, u8 *temp_tex_buffer, bool mipmapped, bool mipmapsIncluded)
{
	CancelDecode();
	if (texID != 0 && TakePooled(width, height, mipmapped || mipmapsIncluded))
	{
		/* Into the pool's texture, made before the game with every level
		 * it can have */
		glcache.BindTexture(GL_TEXTURE_2D, texID);
		const u32 f = (u32)tex_type;
		const u32 bpp = poolTexel[f];
		if (mipmapsIncluded)
			for (int i = 0; (1 << i) <= width; i++)
			{
				glTexSubImage2D(GL_TEXTURE_2D, pooled->levels - i - 1, 0, 0, 1 << i, 1 << i, poolFormat[f], poolType[f], temp_tex_buffer);
				temp_tex_buffer += (1 << (2 * i)) * bpp;
			}
		else
		{
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, poolFormat[f], poolType[f], temp_tex_buffer);
			if (mipmapped)
				glGenerateMipmap(GL_TEXTURE_2D);
		}
		return;
	}
	ReleasePooled();
	if (texID != 0)
	{
		//upload to OpenGL !
		glcache.BindTexture(GL_TEXTURE_2D, texID);
		GLuint comps = tex_type == TextureType::_8 ? gl.single_channel_format : GL_RGBA;
		GLuint gltype;
		u32 bytes_per_pixel = 2;
		switch (tex_type)
		{
		case TextureType::_5551:
			gltype = GL_UNSIGNED_SHORT_5_5_5_1;
			break;
		case TextureType::_565:
			gltype = GL_UNSIGNED_SHORT_5_6_5;
			comps = GL_RGB;
			break;
		case TextureType::_4444:
			gltype = GL_UNSIGNED_SHORT_4_4_4_4;
			break;
		case TextureType::_8888:
			bytes_per_pixel = 4;
			gltype = GL_UNSIGNED_BYTE;
			break;
		case TextureType::_8:
			bytes_per_pixel = 1;
			gltype = GL_UNSIGNED_BYTE;
			break;
		default:
			die("Unsupported texture type");
			gltype = 0;
			break;
		}
		if (mipmapsIncluded)
		{
			int mipmapLevels = 0;
			int dim = width;
			while (dim != 0)
			{
				mipmapLevels++;
				dim >>= 1;
			}
#if !defined(HAVE_OPENGLES2) && !defined(__APPLE__)
			// Open GL 4.2 or GLES 3.0 min
			if (gl.gl_major > 4 || (gl.gl_major == 4 && gl.gl_minor >= 2)
					|| (gl.is_gles && gl.gl_major >= 3))
			{
				GLuint internalFormat;
				switch (tex_type)
				{
				case TextureType::_5551:
					internalFormat = GL_RGB5_A1;
					break;
				case TextureType::_565:
					internalFormat = GL_RGB565;
					break;
				case TextureType::_4444:
					internalFormat = GL_RGBA4;
					break;
				case TextureType::_8888:
					internalFormat = GL_RGBA8;
					break;
				case TextureType::_8:
					internalFormat = comps;
					break;
				default:
					die("Unsupported texture format");
					internalFormat = 0;
					break;
				}
				if (Updates == 1)
				{
					glTexStorage2D(GL_TEXTURE_2D, mipmapLevels, internalFormat, width, height);
					glCheck();
				}
				for (int i = 0; i < mipmapLevels; i++)
				{
					glTexSubImage2D(GL_TEXTURE_2D, mipmapLevels - i - 1, 0, 0, 1 << i, 1 << i, comps, gltype, temp_tex_buffer);
					temp_tex_buffer += (1 << (2 * i)) * bytes_per_pixel;
				}
			}
			else
#endif
			{
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mipmapLevels - 1);
				for (int i = 0; i < mipmapLevels; i++)
				{
					glTexImage2D(GL_TEXTURE_2D, mipmapLevels - i - 1, comps, 1 << i, 1 << i, 0, comps, gltype, temp_tex_buffer);
					temp_tex_buffer += (1 << (2 * i)) * bytes_per_pixel;
				}
			}
		}
		else
		{
			glTexImage2D(GL_TEXTURE_2D, 0,comps, width, height, 0, comps, gltype, temp_tex_buffer);
			if (mipmapped)
				glGenerateMipmap(GL_TEXTURE_2D);
	}
		glCheck();
	}
	else {
		#if FEAT_HAS_SOFTREND
			/*
			if (tex_type == TextureType::_565)
				tex_type = 0;
			else if (tex_type == TextureType::_5551)
				tex_type = 1;
			else if (tex_type == TextureType::_4444)
				tex_type = 2;
			*/
			u16 *tex_data = (u16 *)temp_tex_buffer;
			if (pData) {
				_mm_free(pData);
			}

			pData = (u16*)_mm_malloc(w * h * 16, 16);
			for (int y = 0; y < h; y++) {
				for (int x = 0; x < w; x++) {
					u32* data = (u32*)&pData[(x + y*w) * 8];

					data[0] = decoded_colors[tex_type][tex_data[(x + 1) % w + (y + 1) % h * w]];
					data[1] = decoded_colors[tex_type][tex_data[(x + 0) % w + (y + 1) % h * w]];
					data[2] = decoded_colors[tex_type][tex_data[(x + 1) % w + (y + 0) % h * w]];
					data[3] = decoded_colors[tex_type][tex_data[(x + 0) % w + (y + 0) % h * w]];
				}
			}
		#else
			die("Soft rend disabled, invalid code path");
		#endif
	}
}
	
bool TextureCacheData::Delete()
{
	if (!BaseTextureCacheData::Delete())
		return false;
	CancelDecode();

	/* (a render to a texture that video memory has not got yet is kept: see rend/rtt_watch.h) */
	if (texID && !rtt_watch_take(texID)) {
		if (pooled != nullptr)
			glTexPool.Give(pooled);
		else
			glcache.DeleteTextures(1, &texID);
	}
	pooled = nullptr;
	if (ownID != 0 && ownID != texID)
		glcache.DeleteTextures(1, &ownID);
	ownID = 0;
	
	return true;
}

/* rend/rtt_watch.h asks for a picture it left on the graphics card: w by h
 * pixels, whatever the card has it at. Called wherever the game first
 * touched the memory, the middle of a frame's set-up included, so what it
 * changes it puts back. */
static void rtt_gl_read(const RttWatch *watch, u8 *rgba)
{
	GLint was_fbo = 0, was_pack = 4;
	GLuint fbo = 0;
	const u32 w = watch->w, h = watch->h, scale = watch->scale;
	const u32 x0 = watch->x0 * (scale > 1 ? scale : 1), y0 = watch->y0 * (scale > 1 ? scale : 1);

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &was_fbo);
	glGetIntegerv(GL_PACK_ALIGNMENT, &was_pack);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(RARCH_GL_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, watch->tex, 0);

	if (scale <= 1)
		glReadPixels(x0, y0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
#if defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER) && defined(GL_RGBA8)
	else if (gl.gl_major >= 3)
	{
		/* made the console's size on the card, filtered, and read at that */
		GLuint small_fbo = 0, small = 0;
		const GLboolean was_scissor = glIsEnabled(GL_SCISSOR_TEST);

		glGenRenderbuffers(1, &small);
		glBindRenderbuffer(RARCH_GL_RENDERBUFFER, small);
		glRenderbufferStorage(RARCH_GL_RENDERBUFFER, GL_RGBA8, w, h);
		glGenFramebuffers(1, &small_fbo);
		glBindFramebuffer(RARCH_GL_FRAMEBUFFER, small_fbo);
		glFramebufferRenderbuffer(RARCH_GL_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, RARCH_GL_RENDERBUFFER, small);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, small_fbo);
		if (was_scissor)
			glDisable(GL_SCISSOR_TEST);
		glBlitFramebuffer(x0, y0, x0 + w * scale, y0 + h * scale, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		if (was_scissor)
			glEnable(GL_SCISSOR_TEST);
		glBindFramebuffer(RARCH_GL_FRAMEBUFFER, small_fbo);
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
		glDeleteFramebuffers(1, &small_fbo);
		glDeleteRenderbuffers(1, &small);
	}
#endif
	else
	{
		/* no way to make it smaller on the card: the middle pixel of each */
		u8 line[1024 * 8 * 4];

		for (u32 y = 0; y < h; y++)
		{
			glReadPixels(x0, y0 + y * scale + scale / 2, w * scale, 1, GL_RGBA, GL_UNSIGNED_BYTE, line);
			for (u32 x = 0; x < w; x++)
				memcpy(rgba + (y * w + x) * 4, line + (x * scale + scale / 2) * 4, 4);
		}
	}

	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, was_fbo);
	glDeleteFramebuffers(1, &fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, was_pack);
}

static void rtt_gl_release(uintptr_t tex)
{
	GLuint id = (GLuint)tex;
	// (the pool's goes back to it)
	GLPooledTexture *pooled = glTexPool.Find(id);
	if (pooled != nullptr)
		glTexPool.Give(pooled);
	else
		glcache.DeleteTextures(1, &id);
}

static const RttWatchBackend rtt_gl_backend = { rtt_gl_read, rtt_gl_release };

/* rend/last_picture.h: the context is going, and the screen's last picture
 * is in the framebuffer the frontend gave to draw in. */
void gl_keep_picture(void)
{
	const int w = screen_width, h = screen_height;
#if defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER)
	/* (what cannot be put back is not read: see gl_restore_picture()) */
	if (gl.gl_major < 3)
		return;
#else
	return;
#endif
	u8 *pixels = last_picture_keep(w, h);
	GLint was_fbo = 0, was_pack = 4;

	if (pixels == NULL)
		return;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &was_fbo);
	glGetIntegerv(GL_PACK_ALIGNMENT, &was_pack);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, hw_render.get_current_framebuffer());
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, was_fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, was_pack);

	/* read bottom line first; kept top line first */
	u8 *line = (u8 *)malloc((size_t)w * 4);
	if (line != NULL)
	{
		for (int y = 0; y < h / 2; y++)
		{
			u8 *a = pixels + (size_t)y * w * 4, *b = pixels + (size_t)(h - 1 - y) * w * 4;
			memcpy(line, a, (size_t)w * 4);
			memcpy(a, b, (size_t)w * 4);
			memcpy(b, line, (size_t)w * 4);
		}
		free(line);
	}
}

/* The first render to the screen in a new context starts from the picture
 * kept from the old one: it goes into the framebuffer that is bound, the
 * one the render is about to draw in, at that one's size. */
void gl_restore_picture(void)
{
	int w, h;
	const u8 *pixels = last_picture(&w, &h);

	if (pixels == NULL)
		return;
#if defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER)
	if (gl.gl_major < 3)
		return;

	GLint target = 0;
	GLuint fbo = 0;
	const GLboolean was_scissor = glIsEnabled(GL_SCISSOR_TEST);

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &target);
	GLuint tex = glcache.GenTexture();
	glcache.BindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
	if (was_scissor)
		glDisable(GL_SCISSOR_TEST);
	/* the texture's first line is the picture's top one: upside down on the way */
	glBlitFramebuffer(0, 0, w, h, 0, screen_height, screen_width, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	if (was_scissor)
		glEnable(GL_SCISSOR_TEST);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, target);
	glDeleteFramebuffers(1, &fbo);
	glcache.DeleteTextures(1, &tex);
#endif
}

/* The size rendered at changes and the context stays (Renderer::Resize()).
 * The picture is in the frontend's buffer, in the corner of it that was the
 * old size; a game that draws part of the screen needs it in the corner
 * that is the new size. It is scaled from the one to the other on the
 * graphics card - by way of a texture, the two overlap - and never leaves
 * it. (Not with OpenGL ES 2, which cannot do it or put a kept picture
 * back either.) */
void gl_carry_picture(int from_w, int from_h, int to_w, int to_h)
{
	if (from_w <= 0 || from_h <= 0 || (from_w == to_w && from_h == to_h))
		return;
	/* (with the PowerVR2 filter the picture drawn on is the filter's own,
	 * which takes it over itself: PostProcessor::Resize()) */
	if (settings.rend.PowerVR2Filter)
		return;
#if defined(GL_READ_FRAMEBUFFER) && defined(GL_DRAW_FRAMEBUFFER)
	if (gl.gl_major >= 3)
	{
		GLint was_fbo = 0;
		GLuint fbo = 0;
		const GLuint target = hw_render.get_current_framebuffer();
		const GLboolean was_scissor = glIsEnabled(GL_SCISSOR_TEST);

		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &was_fbo);
		GLuint tex = glcache.GenTexture();
		glcache.BindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, to_w, to_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glGenFramebuffers(1, &fbo);
		if (was_scissor)
			glDisable(GL_SCISSOR_TEST);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
		glBlitFramebuffer(0, 0, from_w, from_h, 0, 0, to_w, to_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
		glBlitFramebuffer(0, 0, to_w, to_h, 0, 0, to_w, to_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		if (was_scissor)
			glEnable(GL_SCISSOR_TEST);
		glBindFramebuffer(RARCH_GL_FRAMEBUFFER, was_fbo);
		glDeleteFramebuffers(1, &fbo);
		glcache.DeleteTextures(1, &tex);
	}
#endif
}

/* OpenGL ES 2 promises a render to a 5-6-5 texture and to nothing finer:
 * set once a driver has refused one of eight bits a channel. */
static bool rtt_rgb_bytes_refused;

void BindRTT(u32 addy, u32 fbw, u32 fbh, u32 channels, u32 fmt)
{
	if (rtt_rgb_bytes_refused && channels == GL_RGB)
		fmt = GL_UNSIGNED_SHORT_5_6_5;

	/* (with the pool, the framebuffer and the depth buffer are kept, and
	 * the texture is the pool's) */
	if (gl.rtt.fbo && !gl.rtt.kept)
      glDeleteFramebuffers(1,&gl.rtt.fbo);
	if (gl.rtt.pooled != nullptr)
		glTexPool.Give(gl.rtt.pooled);
	else if (gl.rtt.tex)
      glcache.DeleteTextures(1,&gl.rtt.tex);
	gl.rtt.pooled = nullptr;
	gl.rtt.tex = 0;
	if (gl.rtt.depthb && !gl.rtt.kept)
      glDeleteRenderbuffers(1,&gl.rtt.depthb);

	gl.rtt.TexAddr=addy>>3;

	// Find the smallest power of two texture that fits the viewport
   u32 fbh2 = 2;
   while (fbh2 < fbh)
      fbh2 *= 2;
   u32 fbw2 = 2;
   while (fbw2 < fbw)
      fbw2 *= 2;

   if (settings.rend.RenderToTextureUpscale > 1)
	{
		fbw *= settings.rend.RenderToTextureUpscale;
		fbh *= settings.rend.RenderToTextureUpscale;
		fbw2 *= settings.rend.RenderToTextureUpscale;
		fbh2 *= settings.rend.RenderToTextureUpscale;
	}

	/* Get the currently bound frame buffer object. On most platforms this just gives 0. */

	if (gl.rtt.kept && (fbw2 > gl.rtt.depthWidth || fbh2 > gl.rtt.depthHeight))
	{
		/* (a render larger than the kept depth buffer: one of that size,
		 * and kept from then on) */
		WARN_LOG(RENDERER, "Render to a texture of %u x %u: its depth buffer made larger", fbw2, fbh2);
		glDeleteRenderbuffers(1, &gl.rtt.depthb);
		gl.rtt.depthb = 0;
	}
	if (!gl.rtt.kept || gl.rtt.depthb == 0)
	{
		/* Generate and bind a render buffer which will become a depth buffer shared between our two FBOs */
		glGenRenderbuffers(1, &gl.rtt.depthb);
		glBindRenderbuffer(RARCH_GL_RENDERBUFFER, gl.rtt.depthb);

		/*
			Currently it is unknown to GL that we want our new render buffer to be a depth buffer.
			glRenderbufferStorage will fix this and in this case will allocate a depth buffer
			m_i32TexSize by m_i32TexSize.
		*/

		glRenderbufferStorage(RARCH_GL_RENDERBUFFER, RARCH_GL_DEPTH24_STENCIL8, fbw2, fbh2);
		gl.rtt.depthWidth = fbw2;
		gl.rtt.depthHeight = fbh2;
	}

	/* A texture for rendering to: the pool's, made at the start, if it has one of that shape */
	if (fmt == GL_UNSIGNED_BYTE)
		gl.rtt.pooled = glTexPool.TakeTarget(channels == GL_RGB, fbw2, fbh2);
	if (gl.rtt.pooled != nullptr)
		gl.rtt.tex = gl.rtt.pooled->name;
	else
	{
		gl.rtt.tex = glcache.GenTexture();
		glcache.BindTexture(GL_TEXTURE_2D, gl.rtt.tex);

		glTexImage2D(GL_TEXTURE_2D, 0, channels, fbw2, fbh2, 0, channels, fmt, 0);
	}

	/* Create the object that will allow us to render to the aforementioned texture */
	if (!gl.rtt.kept)
		glGenFramebuffers(1, &gl.rtt.fbo);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, gl.rtt.fbo);

	/* Attach the texture to the FBO */
	glFramebufferTexture2D(RARCH_GL_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl.rtt.tex, 0);

	// Attach the depth buffer we created earlier to our FBO.
#if defined(HAVE_OPENGLES2) || defined(HAVE_OPENGLES1) || defined(OSX_PPC)
	glFramebufferRenderbuffer(RARCH_GL_FRAMEBUFFER, RARCH_GL_DEPTH_ATTACHMENT,
         RARCH_GL_RENDERBUFFER, gl.rtt.depthb);
   glFramebufferRenderbuffer(RARCH_GL_FRAMEBUFFER, RARCH_GL_STENCIL_ATTACHMENT,
         RARCH_GL_RENDERBUFFER, gl.rtt.depthb);
#else
   glFramebufferRenderbuffer(RARCH_GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
         RARCH_GL_RENDERBUFFER, gl.rtt.depthb);
#endif

	/* Check that our FBO creation was successful */
	GLuint uStatus = glCheckFramebufferStatus(RARCH_GL_FRAMEBUFFER);

	if (uStatus != RARCH_GL_FRAMEBUFFER_COMPLETE && channels == GL_RGB && fmt == GL_UNSIGNED_BYTE)
	{
		rtt_rgb_bytes_refused = true;
		if (gl.rtt.pooled != nullptr)
		{
			// (the pool's goes back: one of the cache's own, made as below)
			glTexPool.Give(gl.rtt.pooled);
			gl.rtt.pooled = nullptr;
			gl.rtt.tex = glcache.GenTexture();
			glFramebufferTexture2D(RARCH_GL_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl.rtt.tex, 0);
		}
		/* (the texture is still the one bound, and stays attached) */
		glcache.BindTexture(GL_TEXTURE_2D, gl.rtt.tex);
		glTexImage2D(GL_TEXTURE_2D, 0, channels, fbw2, fbh2, 0, channels, GL_UNSIGNED_SHORT_5_6_5, 0);
		uStatus = glCheckFramebufferStatus(RARCH_GL_FRAMEBUFFER);
	}

	verify(uStatus == RARCH_GL_FRAMEBUFFER_COMPLETE);

   glViewport(0, 0, fbw, fbh);		// TODO CLIP_X/Y min?
}

void ReadRTTBuffer() {
	/* The texture is the frame from its first pixel (see TransformMatrix);
	 * what goes to video memory is the pixel clip's part of it, at its
	 * place in the frame. */
	const u32 x0 = pvrrc.fb_X_CLIP.min;
	const u32 y0 = pvrrc.fb_Y_CLIP.min;
	const u32 tex_w = pvrrc.fb_X_CLIP.max + 1;
	const u32 tex_h = pvrrc.fb_Y_CLIP.max + 1;
	u32 w = tex_w - x0;
	u32 h = tex_h - y0;

	u32 stride = FB_W_LINESTRIDE.stride * 8;
	if (stride == 0)
		stride = tex_w * 2;
	else if (tex_w * 2 > stride) {
    	// Happens for Virtua Tennis
	w = x0 * 2 >= stride ? 0 : stride / 2 - x0;
    }

	const u8 fb_packmode = FB_W_CTRL.fb_packmode;

    //dumpRtTexture(fb_rtt.TexAddr, w, h);
    
    /* The picture stays on the graphics card, and video memory gets it when
     * something first touches it there: see rend/rtt_watch.h. */
    RttWatch watch = {};
    watch.addr = (gl.rtt.TexAddr << 3) + y0 * stride + x0 * 2;
    watch.w = w;
    watch.h = h;
    watch.x0 = x0;
    watch.y0 = y0;
    watch.stride = stride;
    watch.bytes = h == 0 || w == 0 ? 0 : stride * (h - 1) + w * 2;
    watch.packmode = fb_packmode;
    watch.kval_bit = (FB_W_CTRL.fb_kval & 0x80) << 8;
    watch.alpha_threshold = FB_W_CTRL.fb_alpha_threshold;
    watch.dither = FB_W_CTRL.fb_dither;
    watch.tex = gl.rtt.tex;
    watch.scale = settings.rend.RenderToTextureUpscale > 1 ? settings.rend.RenderToTextureUpscale : 1;
    if (watch.bytes != 0)
       rtt_watch_supersede(watch.addr, watch.bytes);

    if (tex_w > 1024 || tex_h > 1024) {
    	/* too large to be drawn with from here: kept for video memory alone */
    	watch.owned = true;
    	rtt_watch_add(&watch, &rtt_gl_backend);
    }
    else
    {
    	// TexAddr : gl.rtt.TexAddr, Reserved : 0, StrideSel : 0, ScanOrder : 1
    	TCW tcw = { { gl.rtt.TexAddr, 0, 0, 1 } };
    	switch (fb_packmode) {
    	case 0:
    	case 3:
    		tcw.PixelFmt = Pixel1555;
    		break;
    	case 1:
    		tcw.PixelFmt = Pixel565;
    		break;
    	case 2:
    		tcw.PixelFmt = Pixel4444;
    		break;
    	}
    	TSP tsp = { 0 };
	for (tsp.TexU = 0; tsp.TexU <= 7 && (8u << tsp.TexU) < tex_w; tsp.TexU++);
	for (tsp.TexV = 0; tsp.TexV <= 7 && (8u << tsp.TexV) < tex_h; tsp.TexV++);

    	TextureCacheData *texture_data = TexCache.getTextureCacheData(tsp, tcw);
    	if (texture_data->texID == 0)
    		texture_data->Create();
    	texture_data->SetRendered(gl.rtt.tex, gl.rtt.pooled);
    	texture_data->dirty = 0;
	texture_data->stride_read = 0;	// (rendered, not read)
      libCore_vramlock_Lock(texture_data->sa_tex, texture_data->sa + texture_data->size - 1, texture_data);
      rtt_watch_add(&watch, &rtt_gl_backend);
    }
    gl.rtt.tex = 0;
    gl.rtt.pooled = nullptr;

	if (gl.rtt.fbo && !gl.rtt.kept) { glDeleteFramebuffers(1,&gl.rtt.fbo); gl.rtt.fbo = 0; }
	if (gl.rtt.depthb && !gl.rtt.kept) { glDeleteRenderbuffers(1,&gl.rtt.depthb); gl.rtt.depthb = 0; }

   glBindFramebuffer(RARCH_GL_FRAMEBUFFER, hw_render.get_current_framebuffer());
}

static int TexCacheLookups;
static int TexCacheHits;
static float LastTexCacheStats;


u64 gl_GetTexture(TSP tsp, TCW tcw, int area)
{
   TexCacheLookups++;

	//lookup texture
   TextureCacheData* tf = TexCache.getTextureCacheData(tsp, tcw, area);

   if (tf->texID == 0)
   {
		tf->Create();
		tf->texID = glcache.GenTexture();
	}

	//update if needed
	if (tf->NeedsUpdate())
		tf->Update();
   else
   {
      if (tf->IsCustomTextureAvailable())
      {
      	tf->Renew();
      	tf->CheckCustomTexture();
      }
      TexCacheHits++;
   }

	// Return gl texture
	return tf->texID;
}


GLuint fbTextureId;

void RenderFramebuffer()
{
	if (FB_R_SIZE.fb_x_size == 0 || FB_R_SIZE.fb_y_size == 0)
		return;

	PixelBuffer<u32> pb;
	int width;
	int height;
	ReadFramebuffer(pb, width, height);
	
	if (fbTextureId == 0)
		fbTextureId = glcache.GenTexture();
	
	glcache.BindTexture(GL_TEXTURE_2D, fbTextureId);
	
	//set texture repeat mode
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pb.data());
}

#if 0
/* currently not needed, but perhaps for soft rendering */
void render_vmu_screen(u8* screen_data, u32 width, u32 height, u8 vmu_screen_to_display)
{
	u8 *dst = screen_data;
	u8 *src = NULL ;
	u32 line_size = width*4 ;
	u32 start_offset ;
	u32 x,y ;

	src = vmu_screen_params[vmu_screen_to_display].vmu_lcd_screen ;

	if ( src == NULL )
		return ;

	switch ( vmu_screen_params[vmu_screen_to_display].vmu_screen_position )
	{
		case UPPER_LEFT :
		{
			start_offset = 0 ;
			break ;
		}
		case UPPER_RIGHT :
		{
			start_offset = line_size - (VMU_SCREEN_WIDTH*vmu_screen_params[vmu_screen_to_display].vmu_screen_size_mult*4) ;
			break ;
		}
		case LOWER_LEFT :
		{
			start_offset = line_size*(height - VMU_SCREEN_HEIGHT) ;
			break ;
		}
		case LOWER_RIGHT :
		{
			start_offset = line_size*(height - VMU_SCREEN_HEIGHT) + (line_size - (VMU_SCREEN_WIDTH*vmu_screen_params[vmu_screen_to_display].vmu_screen_size_mult*4));
			break ;
		}
	}


	for ( y = 0 ; y < VMU_SCREEN_HEIGHT ; y++)
	{
		dst = screen_data + start_offset + (y*line_size);
		for ( x = 0 ; x < VMU_SCREEN_WIDTH ; x++)
		{
			if ( *src++ > 0 )
			{
				*dst++ = vmu_screen_params[vmu_screen_to_display].vmu_pixel_on_R ;
				*dst++ = vmu_screen_params[vmu_screen_to_display].vmu_pixel_on_G ;
				*dst++ = vmu_screen_params[vmu_screen_to_display].vmu_pixel_on_B ;
				*dst++ = vmu_screen_params[vmu_screen_to_display].vmu_screen_opacity ;
			}
			else
			{
				*dst++ = vmu_screen_params[vmu_screen_to_display].vmu_pixel_off_R ;
				*dst++ = vmu_screen_params[vmu_screen_to_display].vmu_pixel_off_G ;
				*dst++ = vmu_screen_params[vmu_screen_to_display].vmu_pixel_off_B ;
				*dst++ = vmu_screen_params[vmu_screen_to_display].vmu_screen_opacity ;
			}
		}
	}
}
#endif
