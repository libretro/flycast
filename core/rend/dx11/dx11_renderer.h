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
#include <d3d11.h>
#include <d3dcompiler.h>
#include "types.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta_ctx.h"
#include "rend/sorter.h"
#include "rend/tileclip.h"
#include "rend/transform_matrix.h"
#include "rend/shadows.h"
#include "rend/depth_scale.h"
#include "dx11context_lr.h"
#include "dx11_overlay.h"
#include "dx11_texdecoder.h"
#include <vector>

/* The Direct3D 11 renderers. This one sorts translucent polygons by
 * triangle or by strip; the per-pixel one (oit/) is made from it.
 *
 * Everything the game sees is what it sees with the Vulkan renderers,
 * which this follows step for step: the TA context and its render passes,
 * the shadows of rend/shadows.h, renders to textures that stay on the card
 * until the game touches them in video memory (rend/rtt_watch.h), the
 * picture kept across a new context or a new size (rend/last_picture.h).
 * The frame is a picture the frontend is handed (DX11Context::SetFrame). */
class DX11Renderer : public Renderer
{
public:
	bool Init() override;
	void Resize(int w, int h) override;
	void Term() override;
	bool Process(TA_context* ctx) override;
	bool Render() override;
	void KeepPicture() override;
	u64 GetTexture(TSP tsp, TCW tcw, int area = 0) override;

protected:
	struct VertexConstants
	{
		float scale[4];
		float offset[4];
		float depthScale[4];	// 1/fZ_max, 1 / the scale's z (rend/depth_scale.h)
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
		float padding;
	};
	struct PolyConstants
	{
		float clipTest[4];
		float trilinearAlpha;
		float paletteIndex;
		float prepassAlpha;	// the depth prepass's least alpha (rend/sorter.h), 0 for the colour
		float padding;
	};
	struct ScreenTarget
	{
		ComPtr<ID3D11Texture2D> texture;
		ComPtr<ID3D11RenderTargetView> target;
		ComPtr<ID3D11ShaderResourceView> view;
	};

	/* The frame's polygons: into what Begin...() bound */
	virtual void DrawFrame();
	/* What is bound by the time DrawFrame() is called: the per-pixel
	 * renderer puts the frame together in pictures of its own, and binds
	 * the screen's only at the end */
	virtual bool BindsScreen() const { return true; }
	/* What the polygons' vertices are read with, and the polygon's constants are in */
	virtual ID3D11InputLayout *PolyInputLayout() const { return mainInputLayout; }
	virtual ID3D11Buffer *PolyConstantsBuffer() const { return polyConstants; }

	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *deviceContext = nullptr;
	DX11Context *context = nullptr;
	DX11Shaders *shaders = nullptr;
	DX11Samplers *samplers = nullptr;
	DX11BlendStates *blendStates = nullptr;
	DX11DepthStencilStates depthStencilStates;
	DX11TexDecoder texDecoder;		// (before the cache: textures going away tell it)
	DX11TexturePool texPool;		// (and give their textures back)
	DX11TextureCache texCache;
	DX11Overlay overlay;
	TransformMatrix<false> matrices;

	ComPtr<ID3D11InputLayout> mainInputLayout;
	ComPtr<ID3D11InputLayout> positionInputLayout;	// the main vertices' positions alone
	ComPtr<ID3D11InputLayout> modVolInputLayout;	// the modifier volumes', from vertex slot 1
	/* A render's vertices, indexes and modifier volumes, one after the
	 * other in one buffer, bound once (vertex slots 0 and 1, and the
	 * indexes), each render's after the last one's, and the buffer started
	 * again only when it is full: where this render's are in it */
	ComPtr<ID3D11Buffer> geometry;
	u32 geometrySize = 0;
	u32 geometryUsed = 0;
	INT vertexBase = 0;			// (in vertices)
	UINT indexBase = 0;			// (in indexes)
	UINT modVolBase = 0;		// (in modifier volume vertices)
	bool haveModVols = false;
	ComPtr<ID3D11Buffer> vtxConstants;
	ComPtr<ID3D11Buffer> fragConstants;
	ComPtr<ID3D11Buffer> polyConstants;
	ComPtr<ID3D11RasterizerState> rasterizers[3];	// no culling, front, back
	ComPtr<ID3D11Texture2D> fogTexture;
	ComPtr<ID3D11ShaderResourceView> fogView;
	u8 fogUploaded[256];
	bool fogUploadedValid = false;
	ComPtr<ID3D11Texture2D> paletteTexture;
	ComPtr<ID3D11ShaderResourceView> paletteView;

	/* The depth and stencil, for the screen and renders to textures: as
	 * large as the largest of them so far. A view to draw with and, for
	 * the per-pixel renderer, views that leave the stencil or both to be
	 * read by the shaders as well. */
	ComPtr<ID3D11Texture2D> depthTex;
	ComPtr<ID3D11DepthStencilView> depthView;
	ComPtr<ID3D11DepthStencilView> depthViewReadStencil;
	ComPtr<ID3D11DepthStencilView> depthViewReadOnly;
	ComPtr<ID3D11ShaderResourceView> depthSRV;
	ComPtr<ID3D11ShaderResourceView> stencilSRV;
	u32 depthWidth = 0;
	u32 depthHeight = 0;
	bool EnsureDepth(u32 width, u32 height);

	/* The screen: a picture for each of the frontend's sync indexes (one
	 * with version 1), the size rendered at */
	std::vector<ScreenTarget> screenTargets;
	/* ...and one to draw a second render to the screen in, in a frame whose
	 * first one has been handed over (version 2) already: it takes the
	 * place of the one handed over, which stays the frontend's */
	ScreenTarget extraTarget;
	bool lastInExtra = false;		// the last picture is in it
	u32 screenWidth = 0;
	u32 screenHeight = 0;
	int screenIndex = 0;			// the one being drawn
	int lastScreen = -1;			// the one the last picture is in
	bool havePicture = false;
	/* ...or the picture kept from the context before (rend/last_picture.h),
	 * or from the size before (Resize()) */
	ComPtr<ID3D11ShaderResourceView> restoredPicture;
	/* Whether the render starts from the last picture: it covers only part
	 * of the screen, and the game has drawn to this framebuffer lately */
	bool KeepsLastPicture() const
	{
		return !pvrrc.clearFramebuffer && matrices.IsClipped() && (havePicture || restoredPicture);
	}
	/* What it is drawn from */
	ID3D11ShaderResourceView *LastPictureView() const
	{
		return havePicture ? (lastInExtra ? extraTarget.view.get() : screenTargets[lastScreen].view.get()) : restoredPicture.get();
	}
	ID3D11Texture2D *LastPictureTexture() const
	{
		return lastInExtra ? extraTarget.texture.get() : screenTargets[lastScreen].texture.get();
	}
	/* The last picture drawn whole onto what is bound */
	void DrawLastPicture(u32 width, u32 height);

	/* A render to a texture: the texture, where in video memory */
	DX11Texture *rttTexture = nullptr;
	u32 rttAddr = 0;
	/* What the frame is drawn into, and the size of what is drawn of it */
	ID3D11RenderTargetView *targetView = nullptr;
	u32 targetWidth = 0;
	u32 targetHeight = 0;

	D3D11_RECT baseScissor{};
	void SetScissor(const D3D11_RECT& rect) { context->SetScissor(rect); }
	TileClipping SetTileClip(u32 val, D3D11_RECT& rect);
	void SetBaseScissor();
	void SetProvokingVertices();
	void SetCullMode(int mode);
	void SetPolyConstants(const PolyConstants& constants);
	template<typename T>
	void SetConstants(ID3D11Buffer *buffer, const T& constants)
	{
		// (the whole buffer: the constants are a whole number of 16-byte registers)
		static_assert(sizeof(T) % 16 == 0, "constant buffer size");
		deviceContext->UpdateSubresource(buffer, 0, nullptr, &constants, 0, 0);
	}
	/* ...when they are not what the buffer has already */
	template<typename T>
	void SetConstants(ID3D11Buffer *buffer, const T& constants, T& uploaded, bool& valid)
	{
		if (valid && !memcmp(&constants, &uploaded, sizeof(constants)))
			return;
		SetConstants(buffer, constants);
		uploaded = constants;
		valid = true;
	}
	VertexConstants vtxUploaded;
	FragmentConstants fragUploaded;
	PolyConstants polyUploaded;
	bool vtxUploadedValid = false;
	bool fragUploadedValid = false;
	bool polyUploadedValid = false;
	void MakeFragmentConstants(FragmentConstants& constants);
	/* The polygons, the volumes and the indexes, and the sorted triangles'
	 * indexes after those (sortedIndexes) */
	bool UploadGeometry();
	/* What every polygon of the frame is drawn with */
	void SetupFrame();
	/* The geometry buffer, bound */
	void BindGeometry() { context->SetGeometry(geometry, sizeof(Vertex), sizeof(float) * 3, geometry); }

	/* (the context's, which sets only what differs from what is bound) */
	void SetVS(ID3D11VertexShader *vs) { context->SetVS(vs); }
	void SetPS(ID3D11PixelShader *ps) { context->SetPS(ps); }
	void SetBlend(ID3D11BlendState *blend) { context->SetBlend(blend); }
	void SetDepthStencil(ID3D11DepthStencilState *depth, UINT ref = 0) { context->SetDepthStencil(depth, ref); }
	void SetTexture(int slot, ID3D11ShaderResourceView *view, ID3D11SamplerState *sampler)
	{
		context->SetPSResource(slot, view);
		context->SetPSSampler(slot, sampler);
	}
	void SetTopology(D3D11_PRIMITIVE_TOPOLOGY topology) { context->SetTopology(topology); }

	/* The context taken by Process() and kept for the Render() after it */
	bool heldFromProcess = false;

	// Per-triangle sort results
	std::vector<std::vector<SortTrigDrawParam>> sortedPolys;
	std::vector<std::vector<u32>> sortedIndexes;
	u32 sortedIndexCount = 0;
	// ...and the depth prepasses of the sorted lists (rend/sorter.h), their indexes after each pass's sorted ones
	std::vector<std::vector<SortTrigDrawParam>> prepassPolys;
	void SortTriangles();
	/* Whether the sorted lists are drawn as sorted (and have a prepass):
	 * not by the per-pixel renderer */
	virtual bool DrawsSorted() const { return true; }

private:
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

	enum class ShadowPass { None, LaterWins, EarlierWins };
	void DrawPoly(u32 listType, bool sortTriangles, const PolyParam& poly, u32 first, u32 count,
			ShadowPass shadowPass = ShadowPass::None, const D3D11_RECT *within = nullptr);
	void DrawShadowed(u32 listType, const List<PolyParam>& polys, u32 first, u32 last,
			const ScreenBounds& area, const D3D11_RECT& scissor);
	void DrawSorted(const std::vector<SortTrigDrawParam>& polys, bool multipass);
	void DrawPrepass(const std::vector<SortTrigDrawParam>& polys);
	bool prepassDraw = false;	// DrawPoly() draws for the depth prepass
	void DrawList(u32 listType, bool sortTriangles, const List<PolyParam>& polys, u32 first, u32 last);
	void DrawModVols(int first, int count, const RenderPass& previous_pass, const RenderPass& current_pass);

	/* The framebuffer the game wrote itself, for a frame that is that */
	ComPtr<ID3D11Texture2D> fbTexture;
	ComPtr<ID3D11ShaderResourceView> fbView;
	int fbWidth = 0;
	int fbHeight = 0;
};
