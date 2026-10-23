/* Textures made on the GPU from their bytes in video memory, for the
 * Direct3D 11 renderers: what vulkan/tex_decoder.h is for the Vulkan ones.
 *
 * The texture's bytes - the texels, the codebook of a compressed texture,
 * the part of the palette a paletted one uses - are copied as they are
 * into a ring, and a kernel of rend/texdecode.h makes the texels from
 * them, straight into the texture: the same text the CPU's converters are
 * tested against, so the texels are theirs to the bit.
 *  - The ring is one dynamic buffer, made when the renderer starts. It is
 *    written through a mapping that stays open while the frame's textures
 *    are made (MAP_WRITE_NO_OVERWRITE after the bytes the GPU may still be
 *    reading, MAP_WRITE_DISCARD when it starts again from its beginning),
 *    and read by the kernels through a raw view.
 *  - 32-bit texels and palette indices are written by a compute kernel
 *    through an unsigned integer view of the texture's level
 *    (R8G8B8A8_UINT, R8_UINT, over a typeless texture).
 *  - 16-bit texels are drawn into the level by a pixel shader, through a
 *    render target view: Direct3D 11 has no integer view of the 16-bit
 *    formats, and writing them through an unordered access view is
 *    something few devices can. A format the device cannot draw into is
 *    converted on the CPU, as before.
 *  - A planar 16-bit texture needs no kernel: the PowerVR's texels are
 *    Direct3D's B5G6R5, B5G5R5A1 and B4G4R4A4 as they are, and its rows go
 *    from video memory to the texture with UpdateSubresource.
 * What a frame asks for is recorded at the end of Process(), before the
 * frame's draws (or before, when the ring starts again from its
 * beginning or the list of levels is full): nothing waits for the GPU,
 * and nothing is allocated or compiled while a game runs - the kernels are
 * DXBC made when the core was built (rend/texdecode_dxbc.inc). The
 * textures are the texture pool's (DX11TexturePool), which made the views
 * the kernels write through. */
#pragma once
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d11.h>
#include "comptr.h"
#include "types.h"
#include "rend/texdecode.h"
#include "rend/TexCache.h"

class DX11Context;

class DX11TexDecoder
{
public:
	/* Everything made; nothing is done here when the device cannot */
	void Init(DX11Context *context);
	void Term();
	bool Ready() const { return ready; }

	/* Whether a texture of @type can be made by the kernels: by the
	 * compute kernels (8888, 8), drawn (the 16-bit ones the device
	 * draws into) */
	bool Takes(TextureType type) const { return ready && (unsigned)type < 5 && takes[(int)type]; }

	/* A texture of @levels levels to be made from @bytes: where they go in
	 * the ring (a multiple of 16 of them), and where that is in words.
	 * What was asked for before is recorded first if there is no room for
	 * this. Null if a texture that large never fits. */
	u8 *Begin(u32 bytes, u32 levels, u32 *base);
	/* A level of it, made through @uav (32 or 8 bits) or drawn through
	 * @target (16 bits), with the six words of rend/texdecode.h */
	void Add(ID3D11UnorderedAccessView *uav, ID3D11RenderTargetView *target, const u32 pc[6]);
	/* Where the next level added goes, and the serial of what is not
	 * recorded yet: for Cancel() */
	u32 Next() const { return jobCount; }
	u32 Serial() const { return serial; }
	/* The @count levels from @job on, added under @serial, are not made
	 * after all: the texture is made another way, or goes */
	void Cancel(u32 job, u32 count, u32 serial);

	/* What was asked for, recorded: the ring unmapped, the kernels run */
	void Flush();

	// what has been done so far, for measuring
	u64 statBytes = 0;
	u64 statLevels = 0;

private:
	enum { MAX_JOBS = 2048 };
	struct Job
	{
		ID3D11UnorderedAccessView *uav;
		ID3D11RenderTargetView *target;
		u32 pc[8];		// (a whole constant buffer)
	};

	bool ready = false;
	bool takes[5] = {};
	bool noOverwrite = false;	// the device maps a buffer it reads from as a view without discarding it
	bool overlap = false;		// NVAPI's UAV overlap: a flush's kernels run without waits between them
	DX11Context *context = nullptr;
	ID3D11DeviceContext *deviceContext = nullptr;

	ComPtr<ID3D11Buffer> ring;
	ComPtr<ID3D11ShaderResourceView> ringView;
	u8 *mapped = nullptr;
	u32 ringSize = 0;
	u32 head = 0;			// where the next bytes go
	ComPtr<ID3D11Buffer> constants;
	ComPtr<ID3D11ComputeShader> kernel32, kernel8;
	ComPtr<ID3D11VertexShader> vertexShader;
	ComPtr<ID3D11PixelShader> pixelShader;
	ComPtr<ID3D11RasterizerState> rasterizer;
	ComPtr<ID3D11DepthStencilState> depthStencil;

	Job jobs[MAX_JOBS];
	u32 jobCount = 0;
	u32 serial = 0;
};
