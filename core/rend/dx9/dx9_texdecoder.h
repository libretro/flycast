/* Textures made on the GPU from their bytes in video memory, for the
 * Direct3D 9 renderer: what dx11/dx11_texdecoder.h is for the Direct3D 11
 * ones, with the pixel shader of rend/texdecode_sm3.h - Direct3D 9 has no
 * compute shaders, and shader model 3 no integers.
 *
 * The texture's bytes - the texels, the codebook of a compressed texture,
 * the part of the palette a paletted one uses - are copied as they are
 * into a ring: one dynamic A8R8G8B8 texture, made when the renderer
 * starts, 1024 texels (4 KB) a row, locked once a frame with
 * D3DLOCK_DISCARD at the first texture the frame makes and unlocked when
 * the frame's are drawn. Each level is then one draw, of a triangle that
 * covers it, into the level of the texture - a render target texture of
 * the pool's, in the format the texture is sampled in: R5G6B5, A1R5G5B5,
 * A4R4G4B4, A8R8G8B8 or A8. All of a frame's are drawn before its scene
 * (Flush()). A frame that asks for more than the ring holds has the rest
 * converted on the CPU, as before; so has a format the card cannot draw
 * into.
 *
 * Nothing is compiled while a game runs: the shaders are bytecode made
 * when the core was built (rend/texdecode_dx9.inc). And not on by
 * default: the GPU makes them only with the core option's "Enabled,
 * Direct3D 9 too" (settings.rend.GpuTextureDecodeD3D9). A planar 16-bit
 * texture needs none of this: its texels go to the texture as they are
 * (DX9Texture::GpuDecode()). */
#pragma once
#include <windows.h>
#include <d3d9.h>
#include "rend/dx11/comptr.h"
#include "types.h"
#include "rend/texdecode.h"
#include "rend/TexCache.h"

class DX9Context;

class DX9TexDecoder
{
public:
	/* Everything made, if the textures are to be made on the GPU; nothing
	 * otherwise */
	void Init(DX9Context *context);
	void Term();
	bool Ready() const { return ready; }

	/* Whether a texture of @type can be drawn into (the pool has render
	 * targets of its format) */
	bool Takes(TextureType type) const { return ready && (unsigned)type < 5 && takes[(int)type]; }

	/* @bytes of the ring for a texture of @levels levels, and where they
	 * are in words; null if there is no room */
	u8 *Begin(u32 bytes, u32 levels, u32 *base);
	/* A level of it, drawn into @surface (@type's format), with the six
	 * words of rend/texdecode.h */
	void Add(IDirect3DSurface9 *surface, TextureType type, const u32 pc[6]);
	u32 Next() const { return jobCount; }
	u32 Serial() const { return serial; }
	/* The @count levels from @job on, added under @serial, are not made
	 * after all */
	void Cancel(u32 job, u32 count, u32 serial);

	/* What the frame asked for, drawn: before its scene */
	void Flush();

	// what has been done so far, for measuring
	u64 statBytes = 0;
	u64 statLevels = 0;
	u64 statFull = 0;

private:
	enum { MAX_JOBS = 4096 };
	struct Job
	{
		IDirect3DSurface9 *surface;
		float k[16];		// the shader's four constants
		u32 width, height;
	};

	bool ready = false;
	bool takes[5] = {};
	DX9Context *context = nullptr;
	IDirect3DDevice9 *device = nullptr;
	ComPtr<IDirect3DTexture9> ring;
	u32 ringRows = 0;
	u8 *mapped = nullptr;
	u32 head = 0;			// where the next bytes go
	ComPtr<IDirect3DVertexShader9> vertexShader;
	ComPtr<IDirect3DPixelShader9> pixelShader;
	ComPtr<IDirect3DVertexDeclaration9> decl;

	Job jobs[MAX_JOBS];
	u32 jobCount = 0;
	u32 serial = 1;
};
