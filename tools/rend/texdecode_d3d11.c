/* The Direct3D 11 renderers' texture kernels against the texture cache's
 * CPU converters, byte for byte: texdecode_test.c's step 2 and 3 on a
 * Direct3D 11 device.
 *
 * texdecode_test.c, run with "dump", makes every texture of its corpus on
 * the CPU and writes, for each, what a renderer that converts on the GPU
 * is handed (the plan and the bytes) and what the CPU made, to its
 * standard output; this reads that from its standard input. Each texture
 * is then made as dx11/dx11_texdecoder.cpp makes it, with the kernels the
 * core has (rend/texdecode_dxbc.inc), into a texture made as the texture
 * pool makes them (dx11/dx11_texture.cpp):
 *   - 32-bit texels and palette indices by the compute kernels, through
 *     R8G8B8A8_UINT and R8_UINT views of typeless textures;
 *   - 16-bit texels drawn by the pixel shader into B5G6R5, B5G5R5A1 or
 *     B4G4R4A4 textures, through render target views, where the device can
 *     draw into that format (the renderer converts the others on the CPU);
 *     where it cannot, into an R32G32B32A32_FLOAT one, whose colours turned
 *     back into the fields (each times its largest value, rounded, as the
 *     device's conversion to the format does) have to be the texels all
 *     the same;
 *   - and the planar 16-bit textures the renderer copies as they are, with
 *     UpdateSubresource from their rows in video memory.
 * The levels are copied back and have to be what the CPU made, as the
 * renderer uploads it: the 16-bit texels with their bits turned to
 * Direct3D's order (alpha at the top).
 *
 * Built with mingw-w64 and run under wine (wined3d) by texdecode_test.sh. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>

#include "texdecode.h"
#include "texdecode_case.h"

typedef unsigned char u8;
typedef unsigned int u32;
#include "texdecode_dxbc.inc"

#define RING_BYTES (8u * 1024 * 1024)

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11Buffer *ring, *constants;
static ID3D11ShaderResourceView *ring_view;
static ID3D11ComputeShader *cs32, *cs8;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps;
static ID3D11RasterizerState *rast;
static ID3D11DepthStencilState *nodepth;
static int drawable[3];		/* by TextureType 565, 5551, 4444 */
static unsigned failures, cases, through_kernel, drawn_16, drawn_float, natives, levels_made;

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("%s: %lx\n", #x, (unsigned long)hr_); exit(2); } } while (0)

static void fail(const struct texd_case *c, const char *what)
{
   if (failures++ < 20)
      printf("FAIL %s: fmt %u scan %u vq %u mip %u stride %u (ctl %u) %ux%u filter %u force32 %u lod0 %u pal %u/%u at %06x\n",
            what, c->fmt, c->scan_order, c->vq, c->mip, c->stride_sel, c->text_control & 31,
            8u << c->tex_u, 8u << c->tex_v, c->filter, c->force32, c->lod0, c->pal_ctrl, c->pal_select, c->addr);
}

static int read_all(void *p, size_t n)
{
   return fread(p, 1, n, stdin) == n;
}

static void init(void)
{
   D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0, got;
   D3D11_BUFFER_DESC bd;
   D3D11_SHADER_RESOURCE_VIEW_DESC sv;
   D3D11_RASTERIZER_DESC rd;
   D3D11_DEPTH_STENCIL_DESC dd;
   static const DXGI_FORMAT f16[3] = { DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM };
   int i;

   CHECK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &want, 1, D3D11_SDK_VERSION, &dev, &got, &ctx));
   memset(&bd, 0, sizeof(bd));
   bd.ByteWidth = RING_BYTES;
   bd.Usage = D3D11_USAGE_DYNAMIC;
   bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
   bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
   bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
   CHECK(ID3D11Device_CreateBuffer(dev, &bd, NULL, &ring));
   memset(&sv, 0, sizeof(sv));
   sv.Format = DXGI_FORMAT_R32_TYPELESS;
   sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
   sv.BufferEx.NumElements = RING_BYTES / 4;
   sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
   CHECK(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)ring, &sv, &ring_view));
   memset(&bd, 0, sizeof(bd));
   bd.ByteWidth = 32;
   bd.Usage = D3D11_USAGE_DEFAULT;
   bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
   CHECK(ID3D11Device_CreateBuffer(dev, &bd, NULL, &constants));
   CHECK(ID3D11Device_CreateComputeShader(dev, texdecode_dxbc[1], texdecode_dxbc_size[1], NULL, &cs32));
   CHECK(ID3D11Device_CreateComputeShader(dev, texdecode_dxbc[2], texdecode_dxbc_size[2], NULL, &cs8));
   CHECK(ID3D11Device_CreateVertexShader(dev, texdecode_dxbc[3], texdecode_dxbc_size[3], NULL, &vs));
   CHECK(ID3D11Device_CreatePixelShader(dev, texdecode_dxbc[4], texdecode_dxbc_size[4], NULL, &ps));
   memset(&rd, 0, sizeof(rd));
   rd.FillMode = D3D11_FILL_SOLID;
   rd.CullMode = D3D11_CULL_NONE;
   rd.DepthClipEnable = TRUE;
   CHECK(ID3D11Device_CreateRasterizerState(dev, &rd, &rast));
   memset(&dd, 0, sizeof(dd));
   dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
   dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
   CHECK(ID3D11Device_CreateDepthStencilState(dev, &dd, &nodepth));
   for (i = 0; i < 3; i++)
   {
      UINT s = 0;
      ID3D11Device_CheckFormatSupport(dev, f16[i], &s);
      drawable[i] = (s & (D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE))
            == (D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE);
   }
   printf("texdecode_d3d11: feature level %x; 16-bit formats drawn into: 565 %d, 5551 %d, 4444 %d\n",
         (unsigned)got, drawable[0], drawable[1], drawable[2]);
}

static DXGI_FORMAT typed_format(int type)
{
   switch (type)
   {
   case 0: return DXGI_FORMAT_B5G6R5_UNORM;
   case 1: return DXGI_FORMAT_B5G5R5A1_UNORM;
   case 2: return DXGI_FORMAT_B4G4R4A4_UNORM;
   case 3: return DXGI_FORMAT_R8G8B8A8_UNORM;
   default: return DXGI_FORMAT_R8_UNORM;
   }
}

static unsigned texel_bytes(unsigned out)
{
   return out == TEXD_OUT32 ? 4 : out == TEXD_OUT16 ? 2 : 1;
}

/* where level @i of the plan's (0 the smallest) is in what the CPU made */
static size_t level_at(const struct texd_plan *plan, unsigned i)
{
   size_t at = 0;
   unsigned j;
   if (!plan->mipmapped)
      return 0;
   for (j = 0; j < i; j++)
      at += (size_t)1 << (2 * j);
   return at * texel_bytes(plan->out);
}

/* What the CPU made, as the renderer uploads it: 16-bit texels with their
 * bits turned to Direct3D's order (dx11_texture.cpp's turnBits) */
static void d3d_order(unsigned char *p, size_t texels, int type)
{
   const unsigned by = type == 1 ? 1 : type == 2 ? 4 : 0;
   size_t i;
   if (by == 0)
      return;
   for (i = 0; i < texels; i++)
   {
      unsigned v = p[2 * i] | (p[2 * i + 1] << 8);
      v = ((v >> by) | (v << (16 - by))) & 0xFFFF;
      p[2 * i] = (unsigned char)v;
      p[2 * i + 1] = (unsigned char)(v >> 8);
   }
}

/* A texel drawn as floats, as the 16 bits of TextureType @type (Direct3D's order) */
static unsigned from_floats(const float *f, int type)
{
#define FIELD(v, max) ((unsigned)((v) * (max) + 0.5f))
   if (type == 1)
      return (FIELD(f[3], 1) << 15) | (FIELD(f[0], 31) << 10) | (FIELD(f[1], 31) << 5) | FIELD(f[2], 31);
   if (type == 2)
      return (FIELD(f[3], 15) << 12) | (FIELD(f[0], 15) << 8) | (FIELD(f[1], 15) << 4) | FIELD(f[2], 15);
   return (FIELD(f[0], 31) << 11) | (FIELD(f[1], 63) << 5) | FIELD(f[2], 31);
#undef FIELD
}

/* Level @mip of @tex, @w by @h, against @want; drawn as floats if @type >= 0 */
static int same_level_as(ID3D11Texture2D *tex, DXGI_FORMAT format, unsigned mip, unsigned w, unsigned h, unsigned px, const unsigned char *want, int type);
static int same_level(ID3D11Texture2D *tex, DXGI_FORMAT format, unsigned mip, unsigned w, unsigned h, unsigned px, const unsigned char *want)
{
   return same_level_as(tex, format, mip, w, h, px, want, -1);
}

static int same_level_as(ID3D11Texture2D *tex, DXGI_FORMAT format, unsigned mip, unsigned w, unsigned h, unsigned px, const unsigned char *want, int type)
{
   unsigned char conv[16384 * 2];
   D3D11_TEXTURE2D_DESC d;
   ID3D11Texture2D *st;
   D3D11_MAPPED_SUBRESOURCE m;
   unsigned y;
   int same = 1;
   memset(&d, 0, sizeof(d));
   d.Width = w;
   d.Height = h;
   d.MipLevels = 1;
   d.ArraySize = 1;
   d.Format = format;
   d.SampleDesc.Count = 1;
   d.Usage = D3D11_USAGE_STAGING;
   d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   CHECK(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st));
   ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)st, 0, 0, 0, 0, (ID3D11Resource *)tex, mip, NULL);
   CHECK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m));
   for (y = 0; y < h && same; y++)
   {
      const unsigned char *got = (const unsigned char *)m.pData + (size_t)y * m.RowPitch, *row = want + (size_t)y * w * px;
      unsigned x, k;
      if (type >= 0)
      {
         for (x = 0; x < w; x++)
         {
            const unsigned v = from_floats((const float *)got + 4 * x, type);
            conv[2 * x] = (unsigned char)v;
            conv[2 * x + 1] = (unsigned char)(v >> 8);
         }
         got = conv;
      }
      same = memcmp(got, row, (size_t)w * px) == 0;
      for (x = 0; x < w && !same && failures < 5; x++)
         if (memcmp(got + x * px, row + x * px, px))
         {
            printf("  level %u (%ux%u): texel %u,%u is", mip, w, h, x, y);
            for (k = 0; k < px; k++)
               printf(" %02x", got[x * px + k]);
            printf(", the CPU's");
            for (k = 0; k < px; k++)
               printf(" %02x", row[x * px + k]);
            printf("\n");
            break;
         }
   }
   ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
   ID3D11Texture2D_Release(st);
   return same;
}

static void run_kernel(const struct texd_case *c, const struct texd_plan *plan, int type, const unsigned char *raw, unsigned nbytes,
      const unsigned char *cpu)
{
   const unsigned px = texel_bytes(plan->out);
   const unsigned nlevels = plan->mipmapped ? plan->levels : 1;
   const unsigned base = 64;   /* somewhere in the ring that is not its start */
   const int wide = plan->out != TEXD_OUT16;
   const DXGI_FORMAT typed = typed_format(type);
   D3D11_TEXTURE2D_DESC d;
   D3D11_MAPPED_SUBRESOURCE m;
   ID3D11Texture2D *tex;
   ID3D11UnorderedAccessView *uav[TEXD_MAX_LEVELS];
   ID3D11RenderTargetView *rtv[TEXD_MAX_LEVELS];
   unsigned i, d3d16 = 0;
   size_t texels = 0;
   unsigned char *want;

   DXGI_FORMAT target = typed;
   int as_floats = -1;
   if (!wide)
   {
      if (type > 2)
      {
         fail(c, "16 bits of no 16-bit format");
         return;
      }
      if (!drawable[type])
      {
         target = DXGI_FORMAT_R32G32B32A32_FLOAT;
         as_floats = type;
         drawn_float++;
      }
      d3d16 = TEXD_D3D16(type == 1 ? TEXD_D3D_5551 : type == 2 ? TEXD_D3D_4444 : TEXD_D3D_565);
      drawn_16++;
   }
   through_kernel++;
   memset(&d, 0, sizeof(d));
   d.Width = plan->width;
   d.Height = plan->height;
   d.MipLevels = nlevels;
   d.ArraySize = 1;
   d.Format = !wide ? target : plan->out == TEXD_OUT8 ? DXGI_FORMAT_R8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
   d.SampleDesc.Count = 1;
   d.Usage = D3D11_USAGE_DEFAULT;
   d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (wide ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_RENDER_TARGET);
   CHECK(ID3D11Device_CreateTexture2D(dev, &d, NULL, &tex));
   for (i = 0; i < nlevels; i++)
   {
      if (wide)
      {
         D3D11_UNORDERED_ACCESS_VIEW_DESC u;
         memset(&u, 0, sizeof(u));
         u.Format = plan->out == TEXD_OUT8 ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_R8G8B8A8_UINT;
         u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
         u.Texture2D.MipSlice = i;
         CHECK(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)tex, &u, &uav[i]));
      }
      else
      {
         D3D11_RENDER_TARGET_VIEW_DESC r;
         memset(&r, 0, sizeof(r));
         r.Format = target;
         r.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
         r.Texture2D.MipSlice = i;
         CHECK(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, &r, &rtv[i]));
      }
   }

   CHECK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)ring, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
   memcpy((unsigned char *)m.pData + base * 4, raw, nbytes);
   ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)ring, 0);

   if (wide)
   {
      ID3D11UnorderedAccessView *none = NULL;
      ID3D11DeviceContext_CSSetShader(ctx, plan->out == TEXD_OUT32 ? cs32 : cs8, NULL, 0);
      ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &ring_view);
      ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &constants);
      for (i = 0; i < plan->levels; i++)
      {
         const struct texd_level *lv = &plan->level[i];
         u32 pc[8] = { base, lv->data, plan->pal, lv->wh, lv->sr, lv->mode, 0, 0 };
         ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)constants, 0, NULL, pc, 0, 0);
         ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &uav[lv->mip], NULL);
         ID3D11DeviceContext_Dispatch(ctx, ((lv->wh & 0xFFFF) + 7) / 8, ((lv->wh >> 16) + 7) / 8, 1);
      }
      ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &none, NULL);
   }
   else
   {
      ID3D11RenderTargetView *none = NULL;
      ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
      ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
      ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
      ID3D11DeviceContext_RSSetState(ctx, rast);
      ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xffffffff);
      ID3D11DeviceContext_OMSetDepthStencilState(ctx, nodepth, 0);
      ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &ring_view);
      ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &constants);
      for (i = 0; i < plan->levels; i++)
      {
         const struct texd_level *lv = &plan->level[i];
         u32 pc[8] = { base, lv->data, plan->pal, lv->wh, lv->sr, lv->mode | d3d16, 0, 0 };
         D3D11_VIEWPORT vp = { 0.f, 0.f, (float)(lv->wh & 0xFFFF), (float)(lv->wh >> 16), 0.f, 1.f };
         ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)constants, 0, NULL, pc, 0, 0);
         ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv[lv->mip], NULL);
         ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
         ID3D11DeviceContext_Draw(ctx, 3, 0);
      }
      ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &none, NULL);
   }

   for (i = 0; i < plan->levels; i++)
      texels += (size_t)(plan->level[i].wh & 0xFFFF) * (plan->level[i].wh >> 16);
   want = (unsigned char *)malloc(texels * px);
   memcpy(want, cpu, texels * px);
   if (!wide)
      d3d_order(want, texels, type);
   for (i = 0; i < plan->levels; i++)
   {
      const struct texd_level *lv = &plan->level[i];
      levels_made++;
      if ((lv->wh & 0xFFFF) > 16384
            || !same_level_as(tex, target, lv->mip, lv->wh & 0xFFFF, lv->wh >> 16, px, want + level_at(plan, i), as_floats))
      {
         fail(c, wide ? "compute kernel" : "drawn");
         break;
      }
   }
   free(want);
   for (i = 0; i < nlevels; i++)
   {
      if (wide)
         ID3D11UnorderedAccessView_Release(uav[i]);
      else
         ID3D11RenderTargetView_Release(rtv[i]);
   }
   ID3D11Texture2D_Release(tex);
}

/* A planar 16-bit texture the renderer copies as it is: its rows from video
 * memory straight into the texture */
static void run_native(const struct texd_case *c, const struct texd_plan *plan, int type, const unsigned char *raw, const unsigned char *cpu)
{
   const unsigned w = plan->width, h = plan->height, stride = plan->level[0].sr & 0xFFFF;
   D3D11_TEXTURE2D_DESC d;
   ID3D11Texture2D *tex;
   unsigned char *want;

   natives++;
   memset(&d, 0, sizeof(d));
   d.Width = w;
   d.Height = h;
   d.MipLevels = 1;
   d.ArraySize = 1;
   d.Format = typed_format(type);
   d.SampleDesc.Count = 1;
   d.Usage = D3D11_USAGE_DEFAULT;
   d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
   CHECK(ID3D11Device_CreateTexture2D(dev, &d, NULL, &tex));
   ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)tex, 0, NULL, raw + plan->level[0].data, stride * 2, 0);
   want = (unsigned char *)malloc((size_t)w * h * 2);
   memcpy(want, cpu, (size_t)w * h * 2);
   d3d_order(want, (size_t)w * h, type);
   if (!same_level(tex, d.Format, 0, w, h, 2, want))
      fail(c, "copied as it is");
   free(want);
   ID3D11Texture2D_Release(tex);
}

int main(void)
{
   unsigned char *raw = NULL, *cpu = NULL;
   size_t raw_cap = 0, cpu_cap = 0;

   _setmode(_fileno(stdin), _O_BINARY);
   init();
   for (;;)
   {
      u32 head[4];
      struct texd_case c;
      struct texd_plan plan;
      u32 type, nbytes, cpubytes;
      if (!read_all(head, sizeof(head)) || head[0] != 0x43445854u)	/* 'TXDC' */
         break;
      type = head[1];
      nbytes = head[2];
      cpubytes = head[3];
      if (!read_all(&c, sizeof(c)) || !read_all(&plan, sizeof(plan)))
         break;
      if (nbytes + 4 > raw_cap)
         raw = (unsigned char *)realloc(raw, raw_cap = nbytes + 4);
      if (cpubytes > cpu_cap)
         cpu = (unsigned char *)realloc(cpu, cpu_cap = cpubytes);
      if (!read_all(raw, nbytes) || !read_all(cpu, cpubytes))
         break;
      cases++;
      if (nbytes + 64 * 4 > RING_BYTES)
         continue;
      run_kernel(&c, &plan, (int)type, raw, nbytes, cpu);
      if (plan.native)
         run_native(&c, &plan, (int)type, raw, cpu);
   }
   printf("texdecode_d3d11: %u textures, %u through the kernels (%u of them 16-bit, drawn; %u of those as floats, the device not drawing into their format), %u copied as they are; %u levels\n",
         cases, through_kernel, drawn_16, drawn_float, natives, levels_made);
   if (cases == 0 || failures)
   {
      printf("FAIL: %u textures differ%s\n", failures, cases == 0 ? " (none read)" : "");
      return 1;
   }
   printf("texdecode_d3d11: all the same, byte for byte\n");
   return 0;
}
