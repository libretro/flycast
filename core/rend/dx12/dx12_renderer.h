/*
	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once
/* (Direct3D's headers before the emulator's: hw/pvr/pvr_regs.h has macros
 * with the names of some of their parameters) */
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d12.h>
#include "types.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta_ctx.h"
#include "rend/sorter.h"
#include "rend/tileclip.h"
#include "rend/transform_matrix.h"
#include "rend/shadows.h"
#include "dx12context_lr.h"
#include "dx12_overlay.h"
#include "dx12_texdecoder.h"
#include "dx12_texture.h"
#include "dx12_ta_decoder.h"
#include <vector>

/* The Direct3D 12 renderer: translucent polygons sorted by triangle or by
 * strip. What the game sees is what it sees with the Direct3D 11 renderer
 * (dx11/dx11_renderer.cpp), which this follows step for step - the same
 * passes, the same shadows (rend/shadows.h), the same renders to textures
 * that stay on the card until the game touches them (rend/rtt_watch.h),
 * the picture kept across a new context (rend/last_picture.h) - with
 * Direct3D 12's means:
 *  - one command list a render, executed on the frontend's queue when the
 *    render is done (DX12Context::Submit()), and never waited for;
 *  - every polygon drawn by the same shaders, its bits in ten root
 *    constants (dx12_shader_flags.h); the pipeline is the fixed-function
 *    state alone (dx12_pso.h), from a table made at the start;
 *  - the textures in one bindless table, the samplers in another: a draw
 *    binds nothing, it says which;
 *  - a render's vertices, indexes and constants in the upload ring, mapped
 *    once, read by the GPU where they were written;
 *  - draws that differ only in their root constants and index range -
 *    sorted translucent triangles above all - recorded as one
 *    ExecuteIndirect, with their arguments in the ring;
 *  - the frame a picture for each of the frontend's sync indexes, handed
 *    over with the fence that says when it is drawn (version 2) or copied
 *    by the frontend (version 1).
 *
 * SSAA (the core option; rend/ssaa_pattern.h), as the Vulkan renderers
 * have it (vulkan/ssaa.h): the screen's frames are drawn into a picture of
 * 2, 4 or 8 samples a pixel in the standard pattern and a depth of as many
 * (SsaaColor, the samples' depth), made when the renderer starts or
 * changes size - never in a frame - and resolved into the screen's picture
 * by a shader with the shared rule (rend/tile/pvr_resolve.h), not the
 * device's resolve. How many: the most of those asked for that the device
 * draws with, then that the video memory budget the context read at the
 * start has room for (DX12Context::SsaaSamples()). Polygons are shaded at
 * each sample, but for the ones drawn at a single rate
 * (rend/ssaa_rate.h: sprites, the background, flat strips), shaded once a
 * pixel as without SSAA; the per-pixel renderer's translucent polygons are
 * put in its lists once a pixel with the samples they cover. The VMUs'
 * screens and the crosshairs go on the picture after the resolve. Renders
 * to textures have one sample a pixel, as without SSAA (Direct3D 12 has no
 * memory that is only backed when used, which is where the Vulkan
 * renderers supersample them). */
class DX12Renderer : public Renderer
{
public:
	bool Init() override;
	void Resize(int w, int h) override;
	void Term() override;
	bool Process(TA_context* ctx) override;
	bool Render() override;
	void KeepPicture() override;
	u64 GetTexture(TSP tsp, TCW tcw, int area = 0) override;
	// (TA processing switched on or off)
	void OptionsChanged() override;

protected:
	struct VertexConstants
	{
		float scale[4];		// x, y, z, and the depth's scale (shaders/dx12_raster.hlsl)
		float offset[4];
	};
	struct FragmentConstants
	{
		float colorClampMin[4];
		float colorClampMax[4];
		float fogColRam[4];
		float fogColVert[4];
		float alphaTestValue;
		float fogDensity;
		float shadeScale;
		u32 pixelBufferCount;	// the per-pixel renderer's: how many pixels its lists have room for
		u32 ssaaMask;			// SSAA's: every sample of a pixel, a bit each
		u32 ssaaLog2;
	};
	/* The polygon's root constants, as shaders/dx12_raster.hlsl reads them */
	struct PolyConstants
	{
		float clipTest[4];
		float trilinearAlpha;
		float paletteIndex;
		float prepassAlpha;	// the depth prepass's least alpha (rend/sorter.h), 0 for the colour
		u32 flags;			// DX12_PF_
		u32 texture;		// in the bindless table
		u32 sampler;		// in the sampler table
		// the per-pixel renderer's
		u32 texture1;		// the second volume's
		u32 sampler1;
		u32 polyNumber;		// the translucent polygon's, in its list
		u32 oitFlags;		// DX12_OF_
	};
	/* One picture of the screen's */
	struct ScreenTarget
	{
		DX12Image image;
	};

	DX12Context *context = nullptr;
	ID3D12Device *device = nullptr;
	DX12TexDecoder texDecoder;		// (before the cache: textures going away tell it)
	DX12TexturePool texPool;		// (and give their textures back)
	DX12TextureCache texCache;
	DX12Overlay overlay;
	TransformMatrix<false> matrices;

	/* GPU-side TA processing (dx12_ta_decoder.h): a frame it decodes is
	 * drawn from its buffers, its draws' arguments the GPU's */
	DX12TaDecoder taDecoder;
	/* How a frame decoded on the GPU is drawn (ta_gpu_prepare()'s
	 * TA_GPU_*); 0: none is - every frame is parsed */
	virtual u32 TaFlags() const
	{
		/* sorted translucent triangles sorted from the frame's depths, as
		 * GenSorted() sorts them (TA_GPU_SORT); sorted by strip, the polygons
		 * are sorted from their vertices, which a frame decoded on the GPU
		 * has there alone: such a frame is parsed */
		return TA_GPU_DRAW | TA_GPU_PROVOKE | TA_GPU_SHADOWS
				| (!settings.pvr.Emulation.AlphaSortMode && taDecoder.CanSort() ? TA_GPU_SORT : TA_GPU_NO_SORT);
	}
	/* Whether the decoder is to have room for sorted translucent triangles (TA_GPU_SORT) */
	virtual bool TaSorts() const { return true; }
	/* ...its vertex constants, with the GPU's 1/fZ_max copied in: in a
	 * buffer of the default heap, which the ring's words are copied to */
	ComPtr<ID3D12Resource> gpuVtxConstants;
	D3D12_RESOURCE_STATES gpuVtxConstantsState = D3D12_RESOURCE_STATE_COPY_DEST;

	/* A render's vertices, indexes and modifier volumes, in the upload
	 * ring, one after the other: views of them */
	D3D12_VERTEX_BUFFER_VIEW vertexView{};		// stride sizeof(Vertex)
	D3D12_INDEX_BUFFER_VIEW indexView{};
	D3D12_VERTEX_BUFFER_VIEW modVolView{};		// stride three floats
	bool haveModVols = false;
	/* ...and the constants */
	D3D12_GPU_VIRTUAL_ADDRESS vtxConstantsGpu = 0;
	D3D12_GPU_VIRTUAL_ADDRESS fragConstantsGpu = 0;

	DX12Image fogTexture;		// 128 by 2, at DX12_SRV_FOG
	u8 fogUploaded[256];
	bool fogUploadedValid = false;
	DX12Image paletteTexture;	// 1024 by 1, at DX12_SRV_PALETTE

	/* The depth and stencil, for the screen and renders to textures: as
	 * large as the largest of them so far. Its views in the depth heap: to
	 * draw with (depth.rtv), and for the per-pixel renderer, which reads
	 * the stencil or both from the shaders, ones that leave the stencil or
	 * both as they are. Its two planes are transitioned apart there:
	 * depth.state is the depth's, stencilState the stencil's. */
	DX12Image depth;
	D3D12_RESOURCE_STATES stencilState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	u32 dsvReadStencil = ~0u;
	u32 dsvReadOnly = ~0u;
	/* (while the screen is drawn with SSAA, the samples' depth is the one
	 * there, and the depth of one sample is kept here: SsaaDepth()) */
	struct
	{
		DX12Image image;
		D3D12_RESOURCE_STATES stencilState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		u32 dsvReadStencil = ~0u;
		u32 dsvReadOnly = ~0u;
	} otherDepth;
	bool EnsureDepth(u32 width, u32 height);
	/* The depth's planes into these states */
	void DepthStates(D3D12_RESOURCE_STATES depthPlane, D3D12_RESOURCE_STATES stencilPlane);

	/* The screen: a picture for each of the frontend's sync indexes (one
	 * with version 1), the size rendered at */
	std::vector<ScreenTarget> screenTargets;
	/* ...and one to draw a second render to the screen in, in a frame whose
	 * first one has been handed over (version 2) already: it takes the
	 * place of the one handed over, which stays the frontend's */
	ScreenTarget extraTarget;
	/* The screen's pictures can be drawn into by the compute renderer
	 * (unordered access views of them): set before they are made */
	bool storagePictures = false;
	bool lastInExtra = false;		// the last picture is in it
	u32 screenWidth = 0;
	u32 screenHeight = 0;
	int screenIndex = 0;			// the one being drawn
	int lastScreen = -1;			// the one the last picture is in
	bool havePicture = false;
	/* ...or the picture kept from the context before (rend/last_picture.h),
	 * or from the size before (Resize()) */
	DX12Image restoredPicture;
	void DropRestored();
	void DropImage(DX12Image& image);
	/* Whether the render starts from the last picture: it covers only part
	 * of the screen, and the game has drawn to this framebuffer lately */
	bool KeepsLastPicture() const
	{
		return !pvrrc.clearFramebuffer && matrices.IsClipped() && (havePicture || restoredPicture.resource);
	}
	/* Where it is */
	DX12Image *LastPictureImage()
	{
		return havePicture ? (lastInExtra ? &extraTarget.image : &screenTargets[lastScreen].image)
				: restoredPicture.resource ? &restoredPicture : nullptr;
	}
	/* The last picture drawn whole onto what is bound */
	void DrawLastPicture(u32 width, u32 height);

	/* A render to a texture: the texture, where in video memory */
	DX12Texture *rttTexture = nullptr;
	u32 rttAddr = 0;
	/* What the frame is drawn into, and the size of what is drawn of it */
	u32 targetRtv = ~0u;
	u32 targetWidth = 0;
	u32 targetHeight = 0;

	D3D12_RECT baseScissor{};
	TileClipping SetTileClip(u32 val, D3D12_RECT& rect);
	void SetBaseScissor();
	void SetProvokingVertices();
	void MakeFragmentConstants(FragmentConstants& constants);
	u32 pixelBufferRoom = 0;	// the per-pixel renderer's: how many pixels its lists have room for
	/* The polygons, the volumes and the indexes, and the sorted triangles'
	 * indexes after those (sortedIndexes), and the constants, in the ring */
	bool UploadGeometry();
	/* ...and the constants alone (UploadGeometry()'s, and a frame decoded
	 * on the GPU's: its 1/fZ_max copied in from the decode's words) */
	bool UploadConstants();
	/* What every polygon of the frame is drawn with */
	void SetupFrame();

	/* The frontend was waited for in Process(), for the Render() after it */
	bool heldFromProcess = false;

	/* ---- SSAA ---- */
	u32 ssaaSamples = 1;		// the screen's samples a pixel (1: off)
	u32 ssaaLog2 = 0;
	u32 ssaaWidth = 0, ssaaHeight = 0;	// the size its pictures are made for
	/* The pictures of samples the screen's frames are drawn into: one; the
	 * per-pixel renderer's two (SsaaColorImages()) */
	DX12Image ssaaColor[2];
	bool ssaaDepthIn = false;	// the samples' depth is in depth (SsaaDepth())
	/* SSAA of what the option asks, for a screen of @width x @height: its
	 * pictures made, its pipelines too; or off. At the start and when the
	 * size changes. */
	void SetupSsaa(u32 width, u32 height);
	void DropSsaa();
	/* The samples' depth in depth (and in the views of the per-pixel
	 * renderer's table for them), or the depth of one sample */
	void SsaaDepth(bool in);
	/* Whether the frame being drawn is drawn into the samples */
	bool SsaaActive() const { return ssaaDepthIn; }
	/* What a renderer has of SSAA's: how many pictures of samples, and how
	 * much video memory it takes for itself first (KiB) */
	virtual u32 SsaaColorImages() const { return 1; }
	virtual u32 SsaaFixedKib(u32 width, u32 height) const { return 0; }
	/* The pipelines a renderer wants made before a game draws (at Init(),
	 * before SSAA's are made from them) */
	virtual void PreparePipelines() {}
	/* The picture of samples at @srv of the table resolved into what is bound */
	void SsaaResolve(u32 srv);

	/* The frame's polygons: into what Begin...() bound */
	virtual void DrawFrame();
	/* What is bound by the time DrawFrame() is called: the per-pixel
	 * renderer puts the frame together in pictures of its own, and binds
	 * the screen's only at the end */
	virtual bool BindsScreen() const { return true; }
	/* A modifier volume's pipeline */
	ID3D12PipelineState *ModVolPipeline(bool depthTest, bool depthWrite, u32 depthFunc, DX12Stencil stencil, u32 cull, bool writeColor, u32 special);
	void ClearTarget(const DX12Image& image, const float color[4]);
	/* The frame's scissors, cut to the band being drawn by the per-pixel renderer */
	virtual D3D12_RECT CutScissor(const D3D12_RECT& rect) const { return rect; }

	// Per-triangle sort results
	std::vector<std::vector<SortTrigDrawParam>> sortedPolys;
	std::vector<std::vector<u32>> sortedIndexes;
	u32 sortedIndexCount = 0;
	// ...and the depth prepasses of the sorted lists (rend/sorter.h), their indexes after each pass's sorted ones
	std::vector<std::vector<SortTrigDrawParam>> prepassPolys;
	void SortTriangles();

	/* ---- The draws, batched ----
	 *
	 * A polygon's draw is its pipeline, scissor, stencil reference and
	 * topology, and then its root constants and index range. Draws that
	 * share the first four, one after the other, are recorded as one
	 * ExecuteIndirect of the context's command signature, their constants
	 * and ranges written to the ring; a run of one is recorded as itself.
	 * The run is closed by anything that is not such a draw. */
	struct DrawState
	{
		ID3D12PipelineState *pso;
		D3D12_RECT scissor;
		UINT stencilRef;
		D3D12_PRIMITIVE_TOPOLOGY topology;
	};
	struct DrawArgs
	{
		u32 constants[DX12Context::PolyConstantCount];
		D3D12_DRAW_INDEXED_ARGUMENTS draw;
	};
	enum { BATCH_MAX = 4096 };
	DrawState batchState{};
	DrawArgs *batch = nullptr;
	u32 batchCount = 0;
	void Draw(const DrawState& state, const u32 *constants, u32 indexCount, u32 firstIndex);
	void FlushBatch();
	/* A draw group of a frame decoded on the GPU (pvrrc.gpu), drawn by its
	 * arguments there: an opaque or punch-through one by its command (or,
	 * @shadow, its second one, for shadows), a translucent one by its
	 * records, with their count. Recorded as itself. */
	void DrawGpu(const DrawState& state, const u32 *constants, u32 listType, bool shadow, u32 first, u32 count);
	u32 statBatches = 0, statBatched = 0;

	/* The screen's picture made ready (EnsureScreen()) and handed over once
	 * the frame is drawn into it (EndScreen()); a render to a texture's
	 * texture taken (BeginRTT()) and given to the texture once it is drawn
	 * (EndRTT()): Render()'s, and the compute renderer's (dx12_compute.h) */
	bool EnsureScreen(u32 width, u32 height);
	void EndScreen();
	bool BeginRTT();
	void EndRTT();
	/* The size of the texture a render to a texture draws into: from the
	 * frame's first pixel to the pixel clip's last, in powers of two, times
	 * Render To Texture Upscaling */
	static void RttSize(u32& widthPow2, u32& heightPow2);

private:
	bool ProcessLocked(TA_context* ctx);
	bool BeginScreen();
	void BeginScreenSsaa(ScreenTarget& target);
	bool MakeScreenTarget(ScreenTarget& target, u32 width, u32 height);
	void CheckKeptPicture();
	void CheckFogTexture();
	void CheckPaletteTexture();
	bool UploadFramebuffer();
	void DrawFramebuffer();

	enum class ShadowPass { None, LaterWins, EarlierWins };
	void DrawPoly(u32 listType, bool sortTriangles, const PolyParam& poly, u32 first, u32 count,
			ShadowPass shadowPass = ShadowPass::None, const D3D12_RECT *within = nullptr);
	void DrawShadowed(u32 listType, const List<PolyParam>& polys, u32 first, u32 last,
			const ScreenBounds& area, const D3D12_RECT& scissor);
	void DrawSorted(const SortTrigDrawParam *polys, u32 count, bool multipass);
	void DrawPrepass(const SortTrigDrawParam *polys, u32 count);
	/* (a frame decoded on the GPU) the draws are of the sorted indices,
	 * whose counts are the CPU's */
	bool gpuSorted = false;
	bool prepassDraw = false;	// DrawPoly() draws for the depth prepass
	void DrawList(u32 listType, bool sortTriangles, const List<PolyParam>& polys, u32 first, u32 last);
	void DrawModVols(int first, int count, const RenderPass& previous_pass, const RenderPass& current_pass, int render_pass);

	/* The framebuffer the game wrote itself, for a frame that is that */
	DX12Image fbTexture;
};
