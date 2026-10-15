/* The CPU's side of texdecode_test.c: the texture cache itself
 * (core/rend/TexCache.o) making a texture, with what it would hand a
 * renderer that converts on the GPU caught on the way.
 *
 * A texture is made with BaseTextureCacheData::Create() and Update(), as
 * the renderers make them. Update() first offers the texture to
 * GpuDecode(), which takes the plan and says no; then converts it on the
 * CPU and hands the texels to UploadToGPU(), which takes those. So one
 * Update() gives both, from the same state. */
// the registers the frame is rendered with, as the texture cache reads them
#define PVR_REGS_FOR_RENDERER
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "types.h"
#include "rend/TexCache.h"
#include "rend/texdecode.h"
#include "hw/pvr/pvr_regs.h"
#include "texdecode_case.h"

// what the texture cache takes from the rest of the core
settings_t settings;
u32 FrameCount = 1;
unsigned VRAM_SIZE = 8 * 1024 * 1024;
unsigned VRAM_MASK = 8 * 1024 * 1024 - 1;
static u8 pvr_regs_mem[0x10000];
u8 *rend_pvr_regs = pvr_regs_mem;
void _vmem_protect_vram(u32, u32) {}
void _vmem_unprotect_vram(u32, u32) {}
u32 _vmem_get_vram_offset(void *) { return (u32)-1; }
bool _vmem_vram_page_protected(u32) { return false; }
void os_DebugBreak() { abort(); }
void GenericLog(LogTypes::LOG_LEVELS, LogTypes::LOG_TYPE, const char *, int, const char *, ...) {}
extern VArray2 vram;

namespace {
struct TestTexture : public BaseTextureCacheData
{
	bool force32 = false;
	bool planned = false;
	texd_plan plan;
	u8 *out = nullptr;
	size_t cap = 0, bytes = 0;

	std::string GetId() override { return "test"; }
	bool Force32BitTexture(TextureType type) const override { return force32; }
	bool GpuDecode(const texd_plan& p) override
	{
		plan = p;
		planned = true;
		return false;
	}
	void UploadToGPU(int width, int height, u8 *data, bool mipmapped, bool mipmapsIncluded) override
	{
		const size_t pixel = tex_type == TextureType::_8888 ? 4 : tex_type == TextureType::_8 ? 1 : 2;
		size_t n = (size_t)width * height * pixel;
		if (mipmapsIncluded)
			for (int w = width / 2; w != 0; w /= 2)
				n += (size_t)w * w * pixel;
		bytes = n <= cap ? n : 0;
		if (bytes != 0)
			memcpy(out, data, n);
	}
};
}

extern "C" void texd_cpu_init(void *video_memory)
{
	vram.data = (u8 *)video_memory;
	vram.size = VRAM_SIZE;
	settings.rend.TextureUpscale = 1;
	settings.rend.MaxFilteredTextureSize = 256;
	settings.rend.GpuTextureDecode = true;
}

extern "C" int texd_cpu_run(const struct texd_case *c, struct texd_plan *plan, void *out, size_t cap, size_t *bytes, int *type)
{
	TestTexture t;
	t.tcw.full = 0;
	t.tcw.TexAddr = c->addr >> 3;
	t.tcw.PixelFmt = c->fmt;
	t.tcw.ScanOrder = c->scan_order;
	t.tcw.VQ_Comp = c->vq;
	t.tcw.MipMapped = c->mip;
	t.tcw.StrideSel = c->stride_sel;
	if (c->fmt == PixelPal4 || c->fmt == PixelPal8)
		t.tcw.PalSelect = c->pal_select;	// (over the bits above it: as the hardware has it)
	t.tsp.full = 0;
	t.tsp.TexU = c->tex_u;
	t.tsp.TexV = c->tex_v;
	t.tsp.FilterMode = c->filter;
	t.area = 0;
	t.force32 = c->force32 != 0;
	settings.rend.ForceTextureLOD0 = c->lod0 != 0;
	TEXT_CONTROL = c->text_control;
	PAL_RAM_CTRL = c->pal_ctrl;
	palette_update();
	t.out = (u8 *)out;
	t.cap = cap;

	t.Create();
	t.Update();
	t.Delete();
	*bytes = t.bytes;
	*type = (int)t.tex_type;
	if (t.planned)
		*plan = t.plan;
	return t.planned;
}

/* For the palette: the registers' memory */
extern "C" void *texd_cpu_palette_ram(void)
{
	return PALETTE_RAM;
}
