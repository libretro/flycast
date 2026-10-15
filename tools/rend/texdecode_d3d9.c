/* The Direct3D 9 renderer's texture shaders against the texture cache's
 * CPU converters, byte for byte: texdecode_test.c's steps 2 and 3 on a
 * Direct3D 9 device.
 *
 * texdecode_test.c, run with "dump", writes every texture of its corpus -
 * the plan, the bytes the GPU is handed, what the CPU made - to its
 * standard output; this reads that from its standard input, as
 * texdecode_d3d11.c does. Each texture is then made as
 * dx9/dx9_texdecoder.cpp makes it, with the shaders the core has
 * (rend/texdecode_dx9.inc, from rend/texdecode_sm3.h): its bytes in a ring
 * texture of A8R8G8B8 texels, 1024 a row, and a draw for each level into
 * the level of a render target texture in the format the renderer samples
 * it in - R5G6B5, A1R5G5B5 or A4R4G4B4 for 16-bit texels, A8R8G8B8 for
 * 32-bit ones, A8 for palette indices. Where the device cannot draw into
 * that format (wined3d: some of the 16-bit ones, A8), it is drawn into an
 * A32B32G32R32F target, whose colours turned back into the fields (each
 * times its largest value, rounded, as the device's conversion to the
 * format does) have to be the texels all the same.
 *
 * The levels are read back and have to be what the CPU made, as the
 * renderer uploads it: 16-bit texels with their bits turned to Direct3D's
 * order (alpha at the top), 32-bit ones as the A8R8G8B8 texture has them
 * (blue in the lowest byte).
 *
 * And the planar 16-bit textures the renderer copies from video memory as
 * they are: their texels, as they are there, have to be what the CPU made
 * in Direct3D's order - which is what puts them in R5G6B5, A1R5G5B5 and
 * A4R4G4B4 textures with no conversion.
 *
 * Built with mingw-w64 and run under wine (wined3d) by texdecode_test.sh. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>

#include "texdecode.h"
#include "texdecode_case.h"
#define TEXD_SM3_KERNEL 0
#include "texdecode_sm3.h"

typedef unsigned char u8;
typedef unsigned int u32;
#include "texdecode_dx9.inc"

#define RING_ROWS 2048
#define RING_BYTES (RING_ROWS * TEXD_SM3_RING_WIDTH * 4)

static IDirect3D9 *d3d;
static IDirect3DDevice9 *dev;
static IDirect3DTexture9 *ring;
static IDirect3DVertexShader9 *vs;
static IDirect3DPixelShader9 *ps;
static IDirect3DVertexDeclaration9 *decl;
static int drawable[5];		/* by TextureType */
static unsigned failures, cases, through_kernel, drawn_float, natives, levels_made, by_target[5];

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("%s: %lx\n", #x, (unsigned long)hr_); exit(2); } } while (0)

static const D3DFORMAT formats[5] = { D3DFMT_R5G6B5, D3DFMT_A1R5G5B5, D3DFMT_A4R4G4B4, D3DFMT_A8R8G8B8, D3DFMT_A8 };

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
   static const D3DVERTEXELEMENT9 elements[] = {
      { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
      D3DDECL_END()
   };
   D3DPRESENT_PARAMETERS pp;
   D3DDISPLAYMODE mode;
   HWND wnd;
   int i;

   wnd = CreateWindowA("STATIC", "texdecode_d3d9", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
   d3d = Direct3DCreate9(D3D_SDK_VERSION);
   if (d3d == NULL)
   {
      printf("texdecode_d3d9: no Direct3D 9\n");
      exit(2);
   }
   CHECK(IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode));
   memset(&pp, 0, sizeof(pp));
   pp.Windowed = TRUE;
   pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
   pp.BackBufferFormat = D3DFMT_UNKNOWN;
   pp.BackBufferWidth = pp.BackBufferHeight = 64;
   pp.hDeviceWindow = wnd;
   CHECK(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
         D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &dev));
   CHECK(IDirect3DDevice9_CreateTexture(dev, TEXD_SM3_RING_WIDTH, RING_ROWS, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
         D3DPOOL_DEFAULT, &ring, NULL));
   CHECK(IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)texdecode_dx9[1], &vs));
   CHECK(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)texdecode_dx9[2], &ps));
   CHECK(IDirect3DDevice9_CreateVertexDeclaration(dev, elements, &decl));
   for (i = 0; i < 5; i++)
      drawable[i] = IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mode.Format, D3DUSAGE_RENDERTARGET,
            D3DRTYPE_TEXTURE, formats[i]) == D3D_OK;
   IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
   IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
   IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
   IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
   IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
   IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
   IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);
   IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
   IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
   IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
   IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
   IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
   IDirect3DDevice9_SetVertexShader(dev, vs);
   IDirect3DDevice9_SetPixelShader(dev, ps);
   IDirect3DDevice9_SetVertexDeclaration(dev, decl);
   IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)ring);
   printf("texdecode_d3d9: drawn into: 565 %d, 5551 %d, 4444 %d, 8888 %d, A8 %d\n",
         drawable[0], drawable[1], drawable[2], drawable[3], drawable[4]);
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

/* What the CPU made, as the Direct3D 9 texture has it: 16-bit texels with
 * their bits turned (dx9_texture.cpp's copyRow), 32-bit ones blue first */
static void d3d_order(unsigned char *p, size_t texels, int type)
{
   size_t i;
   if (type == 3)
   {
      for (i = 0; i < texels; i++)
      {
         const unsigned char r = p[4 * i];
         p[4 * i] = p[4 * i + 2];
         p[4 * i + 2] = r;
      }
      return;
   }
   if (type == 1 || type == 2)
   {
      const unsigned by = type == 1 ? 1 : 4;
      for (i = 0; i < texels; i++)
      {
         unsigned v = p[2 * i] | (p[2 * i + 1] << 8);
         v = ((v >> by) | (v << (16 - by))) & 0xFFFF;
         p[2 * i] = (unsigned char)v;
         p[2 * i + 1] = (unsigned char)(v >> 8);
      }
   }
}

/* A texel drawn as floats (R G B A), as the texel of TextureType @type */
static void from_floats(const float *f, int type, unsigned char *to)
{
#define FIELD(v, max) ((unsigned)((v) * (max) + 0.5f))
   unsigned v;
   if (type == 4)
   {
      to[0] = (unsigned char)FIELD(f[3], 255);
      return;
   }
   if (type == 3)
   {
      to[0] = (unsigned char)FIELD(f[2], 255);
      to[1] = (unsigned char)FIELD(f[1], 255);
      to[2] = (unsigned char)FIELD(f[0], 255);
      to[3] = (unsigned char)FIELD(f[3], 255);
      return;
   }
   if (type == 1)
      v = (FIELD(f[3], 1) << 15) | (FIELD(f[0], 31) << 10) | (FIELD(f[1], 31) << 5) | FIELD(f[2], 31);
   else if (type == 2)
      v = (FIELD(f[3], 15) << 12) | (FIELD(f[0], 15) << 8) | (FIELD(f[1], 15) << 4) | FIELD(f[2], 15);
   else
      v = (FIELD(f[0], 31) << 11) | (FIELD(f[1], 63) << 5) | FIELD(f[2], 31);
   to[0] = (unsigned char)v;
   to[1] = (unsigned char)(v >> 8);
#undef FIELD
}

/* Level @mip of @tex, @w by @h, in @format, against @want; drawn as floats if @type >= 0 */
static int same_level(IDirect3DTexture9 *tex, D3DFORMAT format, unsigned mip, unsigned w, unsigned h, unsigned px,
      const unsigned char *want, int type)
{
   unsigned char conv[1024 * 4];
   IDirect3DSurface9 *level, *sys;
   D3DLOCKED_RECT r;
   unsigned y;
   int same = 1;
   CHECK(IDirect3DTexture9_GetSurfaceLevel(tex, mip, &level));
   CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, w, h, format, D3DPOOL_SYSTEMMEM, &sys, NULL));
   CHECK(IDirect3DDevice9_GetRenderTargetData(dev, level, sys));
   CHECK(IDirect3DSurface9_LockRect(sys, &r, NULL, D3DLOCK_READONLY));
   for (y = 0; y < h && same; y++)
   {
      const unsigned char *got = (const unsigned char *)r.pBits + (size_t)y * r.Pitch, *row = want + (size_t)y * w * px;
      unsigned x, k;
      if (type >= 0)
      {
         for (x = 0; x < w; x++)
            from_floats((const float *)got + 4 * x, type, conv + x * px);
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
   IDirect3DSurface9_UnlockRect(sys);
   IDirect3DSurface9_Release(sys);
   IDirect3DSurface9_Release(level);
   return same;
}

/* The bytes into the ring from word @base, as the renderer writes them:
 * rows of the ring's width */
static void fill_ring(const unsigned char *raw, unsigned nbytes, unsigned base)
{
   D3DLOCKED_RECT r;
   const unsigned row = TEXD_SM3_RING_WIDTH * 4;
   unsigned at = base * 4, done = 0;
   CHECK(IDirect3DTexture9_LockRect(ring, 0, &r, NULL, D3DLOCK_DISCARD));
   while (done < nbytes)
   {
      const unsigned y = at / row, x = at % row;
      unsigned n = row - x;
      if (n > nbytes - done)
         n = nbytes - done;
      memcpy((unsigned char *)r.pBits + (size_t)y * r.Pitch + x, raw + done, n);
      done += n;
      at += n;
   }
   IDirect3DTexture9_UnlockRect(ring, 0);
}

static void run_kernel(const struct texd_case *c, const struct texd_plan *plan, int type, const unsigned char *raw, unsigned nbytes,
      const unsigned char *cpu)
{
   const unsigned px = texel_bytes(plan->out);
   const unsigned nlevels = plan->mipmapped ? plan->levels : 1;
   const unsigned base = 64;   /* somewhere in the ring that is not its start */
   D3DFORMAT target;
   int as_floats = -1, drawn_as = type;
   IDirect3DTexture9 *tex;
   unsigned i;
   size_t texels = 0;
   unsigned char *want;

   if (plan->out == TEXD_OUT16 ? type > 2 : plan->out == TEXD_OUT32 ? type != 3 : type != 4)
   {
      fail(c, "a texel size of another format");
      return;
   }
   target = formats[type];
   if (!drawable[type])
   {
      target = D3DFMT_A32B32G32R32F;
      as_floats = type;
      drawn_float++;
   }
   through_kernel++;
   by_target[drawn_as]++;
   CHECK(IDirect3DDevice9_CreateTexture(dev, plan->width, plan->height, nlevels, D3DUSAGE_RENDERTARGET, target, D3DPOOL_DEFAULT, &tex, NULL));
   fill_ring(raw, nbytes, base);

   CHECK(IDirect3DDevice9_BeginScene(dev));
   for (i = 0; i < plan->levels; i++)
   {
      static const float tri[3][4] = { { -1.f, -1.f, 0.f, 1.f }, { -1.f, 3.f, 0.f, 1.f }, { 3.f, -1.f, 0.f, 1.f } };
      const struct texd_level *lv = &plan->level[i];
      const unsigned w = lv->wh & 0xFFFF, h = lv->wh >> 16, m = lv->mode;
      const float k[16] = {
         (float)base, (float)lv->data, (float)plan->pal, 1.f / RING_ROWS,
         (float)w, (float)h, (float)(lv->sr & 0xFFFF), (float)(lv->sr >> 16),
         (float)(m & 15), (float)((m >> 4) & 3), (float)((m >> 6) & 3), (float)((m >> 8) & 1),
         (float)((m >> 12) & 15), (float)((m >> 16) & 15), (float)drawn_as, 0.f,
      };
      D3DVIEWPORT9 vp = { 0, 0, w, h, 0.f, 1.f };
      IDirect3DSurface9 *s;
      CHECK(IDirect3DTexture9_GetSurfaceLevel(tex, lv->mip, &s));
      CHECK(IDirect3DDevice9_SetRenderTarget(dev, 0, s));
      CHECK(IDirect3DDevice9_SetViewport(dev, &vp));
      CHECK(IDirect3DDevice9_SetPixelShaderConstantF(dev, TEXD_SM3_CONSTANT, k, 4));
      CHECK(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0])));
      IDirect3DSurface9_Release(s);
   }
   CHECK(IDirect3DDevice9_EndScene(dev));

   for (i = 0; i < plan->levels; i++)
      texels += (size_t)(plan->level[i].wh & 0xFFFF) * (plan->level[i].wh >> 16);
   want = (unsigned char *)malloc(texels * px);
   memcpy(want, cpu, texels * px);
   d3d_order(want, texels, type);
   for (i = 0; i < plan->levels; i++)
   {
      const struct texd_level *lv = &plan->level[i];
      levels_made++;
      if (!same_level(tex, target, lv->mip, lv->wh & 0xFFFF, lv->wh >> 16, px, want + level_at(plan, i), as_floats))
      {
         fail(c, "drawn");
         break;
      }
   }
   free(want);
   IDirect3DTexture9_Release(tex);
}

/* A planar 16-bit texture the renderer copies as it is: its texels as they
 * are in video memory have to be the CPU's in Direct3D's order */
static void run_native(const struct texd_case *c, const struct texd_plan *plan, int type, const unsigned char *raw, const unsigned char *cpu)
{
   const unsigned w = plan->width, h = plan->height, stride = plan->level[0].sr & 0xFFFF;
   unsigned char *want;
   unsigned y;

   natives++;
   want = (unsigned char *)malloc((size_t)w * h * 2);
   memcpy(want, cpu, (size_t)w * h * 2);
   d3d_order(want, (size_t)w * h, type);
   for (y = 0; y < h; y++)
      if (memcmp(raw + plan->level[0].data + (size_t)y * stride * 2, want + (size_t)y * w * 2, (size_t)w * 2) != 0)
      {
         fail(c, "copied as it is");
         break;
      }
   free(want);
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
   printf("texdecode_d3d9: %u textures, %u drawn by the pixel shader (into 565 %u, 5551 %u, 4444 %u, 8888 %u, A8 %u; %u of them as floats, the device not drawing into their format), %u copied as they are; %u levels\n",
         cases, through_kernel, by_target[0], by_target[1], by_target[2], by_target[3], by_target[4], drawn_float, natives, levels_made);
   if (cases == 0 || failures)
   {
      printf("FAIL: %u textures differ%s\n", failures, cases == 0 ? " (none read)" : "");
      return 1;
   }
   printf("texdecode_d3d9: all the same, byte for byte\n");
   return 0;
}
