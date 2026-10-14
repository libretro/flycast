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
#include "dx11context_lr.h"
#include "dx11_overlay.h"
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
		float padding[2];
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

	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *deviceContext = nullptr;
	DX11Shaders *shaders = nullptr;
	DX11Samplers *samplers = nullptr;
	DX11BlendStates *blendStates = nullptr;
	DX11DepthStencilStates depthStencilStates;
	DX11TextureCache texCache;
	DX11Overlay overlay;
	TransformMatrix<false> matrices;

	ComPtr<ID3D11InputLayout> mainInputLayout;
	ComPtr<ID3D11InputLayout> positionInputLayout;	// the modifier volumes', and the main vertices' positions alone
	ComPtr<ID3D11Buffer> vertexBuffer;
	ComPtr<ID3D11Buffer> indexBuffer;
	ComPtr<ID3D11Buffer> modVolBuffer;
	u32 vertexBufferSize = 0;
	u32 indexBufferSize = 0;
	u32 modVolBufferSize = 0;
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
		return havePicture ? screenTargets[lastScreen].view.get() : restoredPicture.get();
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
	D3D11_RECT currentScissor{};
	void SetScissor(const D3D11_RECT& rect);
	TileClipping SetTileClip(u32 val, D3D11_RECT& rect);
	void SetBaseScissor();
	void SetProvokingVertices();
	void SetCullMode(int mode);
	void SetPolyConstants(const PolyConstants& constants);
	template<typename T>
	void SetConstants(ID3D11Buffer *buffer, const T& constants)
	{
		D3D11_MAPPED_SUBRESOURCE mapped;
		if (SUCCEEDED(deviceContext->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		{
			memcpy(mapped.pData, &constants, sizeof(constants));
			deviceContext->Unmap(buffer, 0);
		}
	}
	void MakeFragmentConstants(FragmentConstants& constants);
	/* The polygons, the volumes and the indexes, and the sorted triangles'
	 * indexes after those (sortedIndexes) */
	bool UploadGeometry();
	/* What every polygon of the frame is drawn with */
	void SetupFrame();
	void ResetState();

	/* What was last set, so that nothing is set twice */
	struct
	{
		ID3D11VertexShader *vs;
		ID3D11PixelShader *ps;
		ID3D11BlendState *blend;
		ID3D11DepthStencilState *depth;
		UINT stencilRef;
		ID3D11RasterizerState *raster;
		ID3D11ShaderResourceView *srv[4];
		ID3D11SamplerState *sampler[4];
		D3D11_PRIMITIVE_TOPOLOGY topology;
		PolyConstants poly;
		bool polyValid;
	} state;
	u32 stateChanges = 0;
	void SetVS(ID3D11VertexShader *vs);
	void SetPS(ID3D11PixelShader *ps);
	void SetBlend(ID3D11BlendState *blend);
	void SetDepthStencil(ID3D11DepthStencilState *depth, UINT ref = 0);
	void SetTexture(int slot, ID3D11ShaderResourceView *view, ID3D11SamplerState *sampler);
	void SetTopology(D3D11_PRIMITIVE_TOPOLOGY topology);
	void BindMainGeometry();

	// Per-triangle sort results
	std::vector<std::vector<SortTrigDrawParam>> sortedPolys;
	std::vector<std::vector<u32>> sortedIndexes;
	u32 sortedIndexCount = 0;
	void SortTriangles();

private:
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
	void DrawList(u32 listType, bool sortTriangles, const List<PolyParam>& polys, u32 first, u32 last);
	void DrawModVols(int first, int count, const RenderPass& previous_pass, const RenderPass& current_pass);

	/* The framebuffer the game wrote itself, for a frame that is that */
	ComPtr<ID3D11Texture2D> fbTexture;
	ComPtr<ID3D11ShaderResourceView> fbView;
	int fbWidth = 0;
	int fbHeight = 0;
};
