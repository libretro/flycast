#define PVR_REGS_FOR_RENDERER	// see hw/pvr/pvr_regs.h
#include <libretro.h>

#include "gl4.h"

GLuint gl4BindRTT(u32 addy, u32 fbw, u32 fbh, u32 channels, u32 fmt)
{
	/* (with the pool, the framebuffer is kept, and the texture is the pool's) */
	if (gl.rtt.fbo && !gl.rtt.kept)
		glDeleteFramebuffers(1,&gl.rtt.fbo);
	if (gl.rtt.pooled != nullptr)
		glTexPool.Give(gl.rtt.pooled);
	else if (gl.rtt.tex)
		glcache.DeleteTextures(1,&gl.rtt.tex);
	gl.rtt.pooled = nullptr;
	gl.rtt.tex = 0;

	gl.rtt.TexAddr=addy>>3;

	// Find the smallest power of two texture that fits the viewport
	u32 fbh2 = 8;
	while (fbh2 < fbh)
		fbh2 *= 2;
	u32 fbw2 = 8;
	while (fbw2 < fbw)
		fbw2 *= 2;

	if (settings.rend.RenderToTextureUpscale > 1)
	{
		fbw *= settings.rend.RenderToTextureUpscale;
		fbh *= settings.rend.RenderToTextureUpscale;
		fbw2 *= settings.rend.RenderToTextureUpscale;
		fbh2 *= settings.rend.RenderToTextureUpscale;
	}

	// A texture for rendering to: the pool's, made at the start, if it has one of that shape
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

	// Create the object that will allow us to render to the aforementioned texture
	if (!gl.rtt.kept)
		glGenFramebuffers(1, &gl.rtt.fbo);
	glBindFramebuffer(RARCH_GL_FRAMEBUFFER, gl.rtt.fbo);

	// Attach the texture to the FBO
	glFramebufferTexture2D(RARCH_GL_FRAMEBUFFER, RARCH_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl.rtt.tex, 0);

	// Check that our FBO creation was successful
	GLuint uStatus = glCheckFramebufferStatus(RARCH_GL_FRAMEBUFFER);

	verify(uStatus == RARCH_GL_FRAMEBUFFER_COMPLETE);

	glViewport(0, 0, fbw, fbh);		// TODO CLIP_X/Y min?

	return gl.rtt.fbo;
}
