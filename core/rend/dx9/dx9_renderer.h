/*
	Copyright 2021 flyinghead

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
#include <windows.h>
#include <d3d9.h>
#include "types.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta_ctx.h"
#include "rend/sorter.h"
#include "rend/tileclip.h"
#include "rend/transform_matrix.h"
#include "rend/shadows.h"
#include "rend/depth_scale.h"
#include "dx9context_lr.h"
#include "dx9_texture.h"
#include "dx9_texdecoder.h"
#include "dx9_overlay.h"
#include <vector>

/* The Direct3D 9 renderer: translucent polygons sorted by triangle or by
 * strip (Direct3D 9 has nothing a per-pixel renderer could be made of).
 *
 * It is the Direct3D 11 renderer (dx11/dx11_renderer.h) step for step,
 * which is the Vulkan renderer's: the TA context and its render passes,
 * the shadows of rend/shadows.h, renders to textures that stay on the
 * card until the game touches them in video memory (rend/rtt_watch.h),
 * the picture kept across a new context or a new size
 * (rend/last_picture.h). The frame is a picture the frontend is handed as
 * it is (DX9Context::SetFrame).
 *
 * Everything a frame needs is made when the renderer is set up or the
 * screen changes size: the shaders, all of them; the geometry buffers, as
 * large as the tile accelerator's lists can be; the depth buffer and the
 * screen's pictures. Textures come from the texture cache, and go back to
 * the context to be used again when the cache lets them go. */
class DX9Renderer : public Renderer
{
public:
	bool Init() override;
	void Resize(int w, int h) override;
	void Term() override;
	bool Process(TA_context* ctx) override;
	bool Render() override;
	void KeepPicture() override;
	u64 GetTexture(TSP tsp, TCW tcw, int area = 0) override;

private:
	IDirect3DDevice9 *device = nullptr;
	DX9Context *context = nullptr;
	DX9Shaders *shaders = nullptr;
	DX9TexDecoder texDecoder;		// (before the cache: textures going away tell it)
	DX9TexturePool texPool;			// (and give their textures back)
	DX9TextureCache texCache;
	DX9Overlay overlay;
	TransformMatrix<false> matrices;

	ComPtr<IDirect3DVertexDeclaration9> mainDecl;
	ComPtr<IDirect3DVertexDeclaration9> positionDecl;	// the positions alone, of either kind of vertex
	/* A render's vertices and modifier volumes in one buffer, and its
	 * indexes in another, each render's after the last one's, and the
	 * buffers started again (discarded) only when they are full: where
	 * this render's are in them */
	ComPtr<IDirect3DVertexBuffer9> vertexBuffer;
	ComPtr<IDirect3DIndexBuffer9> indexBuffer;
	u32 vertexBufferSize = 0;
	u32 indexBufferSize = 0;
	u32 vertexBufferUsed = 0;
	u32 indexBufferUsed = 0;
	INT vertexBase = 0;			// (in vertices)
	UINT vertexCount = 0;
	UINT indexBase = 0;			// (in indexes)
	UINT modVolBase = 0;		// (in modifier volume vertices)
	bool haveModVols = false;
	bool MakeBuffers(u32 vertexBytes, u32 indexBytes);
	ComPtr<IDirect3DTexture9> fogTexture;
	u8 fogUploaded[256];
	bool fogUploadedValid = false;
	ComPtr<IDirect3DTexture9> paletteTexture;

	/* The depth and stencil, for the screen and renders to textures: as
	 * large as the largest of them so far */
	ComPtr<IDirect3DSurface9> depthSurface;
	u32 depthWidth = 0;
	u32 depthHeight = 0;
	bool EnsureDepth(u32 width, u32 height);

	/* The screen: a picture for each of the frontend's sync indexes, the
	 * size rendered at - and, for each, one to draw a second render to the
	 * screen in, in a frame whose first one has been handed over already:
	 * the two change places, and the one handed over stays the frontend's
	 * until that sync index comes round again */
	struct ScreenTarget
	{
		ComPtr<IDirect3DTexture9> texture;
		ComPtr<IDirect3DSurface9> surface;
		u32 width = 0;
		u32 height = 0;
	};
	struct ScreenSlot
	{
		ScreenTarget target;
		ScreenTarget extra;
	};
	std::vector<ScreenSlot> screenSlots;
	u32 screenWidth = 0;
	u32 screenHeight = 0;
	int screenIndex = 0;			// the one being drawn
	/* The last picture drawn to the screen, or the one kept from the
	 * context before (rend/last_picture.h) or the size before (Resize()),
	 * whose bytes are R G B A when it was kept */
	ComPtr<IDirect3DTexture9> lastPicture;
	ComPtr<IDirect3DTexture9> restoredPicture;
	bool restoredSwap = false;
	/* Whether the render starts from the last picture: it covers only part
	 * of the screen, and the game has drawn to this framebuffer lately */
	bool KeepsLastPicture() const
	{
		return !pvrrc.clearFramebuffer && matrices.IsClipped() && (lastPicture || restoredPicture);
	}
	/* The last picture drawn whole onto what is bound */
	void DrawLastPicture();
	bool MakeScreenTarget(ScreenTarget& target, u32 width, u32 height);

	/* A render to a texture: the texture, where in video memory */
	DX9Texture *rttTexture = nullptr;
	u32 rttAddr = 0;

	RECT baseScissor{};
	TileClipping SetTileClip(u32 val, RECT& rect);
	void SetBaseScissor();
	void SetProvokingVertices();

	/* The constants, uploaded when they differ from what the device has */
	struct PolyConstants
	{
		float clipTest[4];
		float params[4];		// trilinear alpha, palette index, the depth prepass's least alpha (rend/sorter.h; 0 for the colour)
	};
	PolyConstants polyUploaded;
	u32 polyEpoch = 0;			// the context's epoch they were uploaded in, or not (0)
	void SetPolyConstants(const PolyConstants& constants);
	u32 frameEpoch = 0;
	/* What every polygon of the frame is drawn with */
	void SetupFrame();
	/* The polygons, the volumes and the indexes, and the sorted triangles'
	 * indexes after those (sortedIndexes) */
	bool UploadGeometry();

	/* The device taken by Process() and kept for the Render() after it */
	bool heldFromProcess = false;

	// Per-triangle sort results
	std::vector<std::vector<SortTrigDrawParam>> sortedPolys;
	std::vector<std::vector<u32>> sortedIndexes;
	u32 sortedIndexCount = 0;
	// ...and the depth prepasses of the sorted lists (rend/sorter.h), their indexes after each pass's sorted ones
	std::vector<std::vector<SortTrigDrawParam>> prepassPolys;
	void SortTriangles();

	bool ProcessLocked(TA_context* ctx);
	bool BeginScreen();
	void EndScreen();
	bool BeginRTT();
	void EndRTT();
	bool EnsureScreen(u32 width, u32 height);
	void CheckKeptPicture();
	void CheckFogTexture();
	void CheckPaletteTexture();
	bool UploadFramebuffer();
	void DrawFramebuffer();
	void DrawFrame();

	enum class ShadowPass { None, LaterWins, EarlierWins };
	void DrawPoly(u32 listType, bool sortTriangles, const PolyParam& poly, u32 first, u32 count,
			ShadowPass shadowPass = ShadowPass::None, const RECT *within = nullptr);
	void DrawShadowed(u32 listType, const List<PolyParam>& polys, u32 first, u32 last,
			const ScreenBounds& area, const RECT& scissor);
	void DrawSorted(const std::vector<SortTrigDrawParam>& polys, bool multipass);
	void DrawPrepass(const std::vector<SortTrigDrawParam>& polys);
	bool prepassDraw = false;	// DrawPoly() draws for the depth prepass
	void DrawList(u32 listType, bool sortTriangles, const List<PolyParam>& polys, u32 first, u32 last);
	void DrawModVols(int first, int count, const RenderPass& previous_pass, const RenderPass& current_pass);
	void SetCullMode(int mode);
	void Draw(D3DPRIMITIVETYPE type, UINT first, UINT count)
	{
		device->DrawIndexedPrimitive(type, vertexBase, 0, vertexCount, indexBase + first,
				type == D3DPT_TRIANGLESTRIP ? count - 2 : count / 3);
	}

	/* The framebuffer the game wrote itself, for a frame that is that */
	ComPtr<IDirect3DTexture9> fbTexture;
	int fbWidth = 0;
	int fbHeight = 0;
};
