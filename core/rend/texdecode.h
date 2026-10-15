/* A texture of the PowerVR's, made from the bytes it has in video memory:
 * one texel at a time, the way the texture cache's converters make all of
 * them (TexCache.h), to the same bits. For the GPU to do the converting -
 * a compute kernel run over each level of the texture - from the bytes
 * themselves, copied as they are; and for the CPU, which has the same
 * text as a reference for the tests.
 *
 * The header is read three ways, like rend/tile/pvr_lane.h:
 * - by a C89 or C++ compiler (nothing defined);
 * - by a GLSL compiler, with TEXD_SHADER_GLSL defined;
 * - by an HLSL compiler, with TEXD_SHADER_HLSL defined.
 * So it is written in what the three have in common: functions of
 * unsigned and signed 32-bit integers, by value, no pointers, no
 * structures. Where the bytes are read from is the reader's business:
 * TEXD_WORD(i) is the i-th 32-bit word of the texture's bytes as they were
 * copied, little-endian - from the ring's buffer in the GLSL kernel below
 * (TEXD_KERNEL_FORMAT defined), from what a Direct3D kernel defines it as,
 * and on the C side from texd_c_bytes (texd_c_set()). The core has the
 * kernels as SPIR-V in texdecode_spv.inc, which tools/rend/kernels_spv.sh
 * makes and checks (tools/rend/texdecode_test.sh runs the check); and the
 * Direct3D 11 ones (TEXD_KERNEL_D3D defined) as DXBC in
 * texdecode_dxbc.inc, which tools/rend/kernels_dxbc.sh makes and checks.
 *
 * The bytes of a texture, as copied, start at its first byte in video
 * memory (the codebook, for a compressed one) and run to its last, every
 * mipmap level included. After them, on a multiple of four, comes the part
 * of the palette a paletted texture uses, already in the texels it gives
 * (palette16_ram or palette32_ram): 16 or 256 words.
 *
 * What a level is made with is six words, the same for every texel of it
 * (the push constants of a dispatch):
 *   data   where the level's texels, or its codebook indices, start
 *   pal    where the palette starts
 *   wh     the level's width | height << 16
 *   sr     a planar texture's row length (stride) | rows there are << 16
 *   mode   TEXD_MODE(): pixel format, layout, texel size, the corner rule,
 *          and the log2 of the width and height the layout is laid out by
 *          (and, for the Direct3D kernel that draws 16-bit texels, the
 *          format it draws them into: TEXD_D3D16())
 *   (base) where the bytes are in the shader's buffer, for TEXD_WORD()
 * all but base in bytes from the texture's first one. */
#ifndef REND_TEXDECODE_H
#define REND_TEXDECODE_H

#if defined(TEXD_SHADER_GLSL) || defined(TEXD_SHADER_HLSL)

#define TEXD_FN
#define texd_u32 uint
#define texd_s32 int
#define TEXD_U32(x) uint(x)
#define TEXD_S32(x) int(x)
#ifdef TEXD_SHADER_HLSL
#undef TEXD_U32
#undef TEXD_S32
#define TEXD_U32(x) ((uint)(x))
#define TEXD_S32(x) ((int)(x))
#endif

#else

#include <string.h>
#include <retro_inline.h>

#define TEXD_FN static INLINE
typedef unsigned int texd_u32;
typedef int texd_s32;
#define TEXD_U32(x) ((texd_u32)(x))
#define TEXD_S32(x) ((texd_s32)(x))

/* the bytes TEXD_WORD() reads on this side */
static const unsigned char *texd_c_bytes;
static INLINE void texd_c_set(const void *bytes)
{
   texd_c_bytes = (const unsigned char *)bytes;
}
static INLINE texd_u32 texd_c_word(texd_u32 i)
{
   const unsigned char *p = texd_c_bytes + (size_t)i * 4;
   return TEXD_U32(p[0]) | (TEXD_U32(p[1]) << 8) | (TEXD_U32(p[2]) << 16) | (TEXD_U32(p[3]) << 24);
}
#define TEXD_WORD(i) texd_c_word(i)

#endif

/* The kernel, in GLSL: TEXD_KERNEL_FORMAT is the storage format of the
 * level it writes (r8ui, r16ui or r32ui, by the texel's size: the image
 * is viewed as integers of that size) */
#if defined(TEXD_SHADER_GLSL) && defined(TEXD_KERNEL_FORMAT)
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, set = 0, binding = 0) readonly buffer Ring { uint words[]; } ring;
layout(set = 1, binding = 0, TEXD_KERNEL_FORMAT) uniform writeonly uimage2D dst;
layout(push_constant) uniform Level { uint base, data, pal, wh, sr, mode; } lv;
#define TEXD_WORD(i) ring.words[lv.base + (i)]
#endif

/* The kernels in HLSL, for the Direct3D 11 renderers (dx11/dx11_texdecoder.h).
 * TEXD_KERNEL_D3D is which:
 *   1  cs_5_0, a level of 32-bit texels, into an R8G8B8A8_UINT view of it
 *   2  cs_5_0, a level of palette indices, into an R8_UINT view
 *   3  vs_5_0, the triangle that covers a level, for 4
 *   4  ps_5_0, a level of 16-bit texels drawn into it, through a render
 *      target view: Direct3D 11 has no integer view of the 16-bit formats,
 *      and few devices write them through an unordered access view. The
 *      colour is each field over its largest value, which the target
 *      turns back into the field, exactly. */
#if defined(TEXD_SHADER_HLSL) && defined(TEXD_KERNEL_D3D)
ByteAddressBuffer texd_ring : register(t0);
cbuffer TexdLevel : register(b0) { uint lv_base, lv_data, lv_pal, lv_wh, lv_sr, lv_mode, lv_pad0, lv_pad1; };
#define TEXD_WORD(i) texd_ring.Load((lv_base + (i)) * 4u)
#if TEXD_KERNEL_D3D == 1
RWTexture2D<uint4> texd_dst : register(u0);
#elif TEXD_KERNEL_D3D == 2
RWTexture2D<uint> texd_dst : register(u0);
#endif
#endif

/* What kernel 4 draws: the 16-bit texels' format, in the mode's bits 20
 * and 21 */
#define TEXD_D3D_565   0
#define TEXD_D3D_5551  1
#define TEXD_D3D_4444  2
#define TEXD_D3D16(f)  (TEXD_U32(f) << 20)

/* The pixel formats, as the texture control word has them */
#define TEXD_1555    0
#define TEXD_565     1
#define TEXD_4444    2
#define TEXD_YUV     3
#define TEXD_BUMP    4   /* converted as 4444 */
#define TEXD_PAL4    5
#define TEXD_PAL8    6

/* How the texels are laid out */
#define TEXD_TW      0   /* twiddled */
#define TEXD_VQ      1   /* twiddled and compressed: a codebook index for every 4 (8, 16 paletted) texels */
#define TEXD_PL      2   /* planar, rows of 'stride' texels */
#define TEXD_PLVQ    3   /* planar and compressed: an index for every 4 texels of a row */

/* What a texel is made into: a palette index; 16 bits, as the 16-bit
 * converters make them (5551, 565, 4444 with the alpha at the bottom);
 * 32 bits, RGBA from the lowest byte */
#define TEXD_OUT8    0
#define TEXD_OUT16   1
#define TEXD_OUT32   2

/* @corner: the level is the 1x1 one of a compressed texture, which is the
 * lower right texel of the 2x2 block its index names */
#define TEXD_MODE(fmt, scan, otype, corner, log2w, log2h) \
   (TEXD_U32(fmt) | (TEXD_U32(scan) << 4) | (TEXD_U32(otype) << 6) | (TEXD_U32(corner) << 8) \
    | (TEXD_U32(log2w) << 12) | (TEXD_U32(log2h) << 16))

TEXD_FN texd_u32 texd_byte(texd_u32 at)
{
   return (TEXD_WORD(at >> 2) >> ((at & 3u) * 8u)) & 0xFFu;
}

/* (@at even) */
TEXD_FN texd_u32 texd_half(texd_u32 at)
{
   return (TEXD_WORD(at >> 2) >> ((at & 2u) * 8u)) & 0xFFFFu;
}

/* The low 16 bits of @v, a zero between every two */
TEXD_FN texd_u32 texd_spread(texd_u32 v)
{
   v &= 0xFFFFu;
   v = (v | (v << 8)) & 0x00FF00FFu;
   v = (v | (v << 4)) & 0x0F0F0F0Fu;
   v = (v | (v << 2)) & 0x33333333u;
   v = (v | (v << 1)) & 0x55555555u;
   return v;
}

/* Where texel (@x, @y) of a twiddled picture 2^@lw by 2^@lh wide is, in
 * texels: the bits of y and x taken in turn, y's first, for as long as
 * both have any, then the rest of the larger one's - detwiddle[][][] in
 * TexCache.cpp */
TEXD_FN texd_u32 texd_twiddle(texd_u32 x, texd_u32 y, texd_u32 lw, texd_u32 lh)
{
   texd_u32 n = lw < lh ? lw : lh;
   texd_u32 mask = (1u << n) - 1u;
   texd_u32 at = texd_spread(y & mask) | (texd_spread(x & mask) << 1);
   if (lw > lh)
      at |= (x >> n) << (2u * n);
   else
      at |= (y >> n) << (2u * n);
   return at;
}

/* a / d for a positive @d, rounded towards zero as C's division is */
TEXD_FN texd_s32 texd_div0(texd_s32 a, texd_s32 d)
{
   return a >= 0 ? a / d : -((-a) / d);
}

TEXD_FN texd_s32 texd_clamp255(texd_s32 v)
{
   return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* YUV422() */
TEXD_FN texd_u32 texd_yuv(texd_u32 y8, texd_u32 u8, texd_u32 v8)
{
   texd_s32 y = TEXD_S32(y8);
   texd_s32 u = TEXD_S32(u8) - 128;
   texd_s32 v = TEXD_S32(v8) - 128;
   texd_s32 r = texd_clamp255(y + texd_div0(v * 11, 8));
   texd_s32 g = texd_clamp255(y - texd_div0(u * 11 + v * 22, 32));
   texd_s32 b = texd_clamp255(y + texd_div0(u * 110, 64));
   return TEXD_U32(r) | (TEXD_U32(g) << 8) | (TEXD_U32(b) << 16) | 0xFF000000u;
}

/* A 16-bit texel of format @fmt to what @out wants: ARGB1555() and the
 * others, ARGB1555_32() and the others */
TEXD_FN texd_u32 texd_rgb(texd_u32 fmt, texd_u32 otype, texd_u32 c)
{
   if (otype == TEXD_OUT16)
   {
      if (fmt == TEXD_565)
         return c;
      if (fmt == TEXD_1555)
         return ((c >> 15) & 1u) | (((c >> 10) & 0x1Fu) << 11) | (((c >> 5) & 0x1Fu) << 6) | ((c & 0x1Fu) << 1);
      return ((c & 0xFu) << 4) | (((c >> 4) & 0xFu) << 8) | (((c >> 8) & 0xFu) << 12) | ((c >> 12) & 0xFu);
   }
   if (fmt == TEXD_565)
      return (((c >> 11) & 0x1Fu) << 3) | ((c >> 13) & 0x7u)
            | (((c >> 5) & 0x3Fu) << 10) | (((c >> 9) & 0x3u) << 8)
            | ((c & 0x1Fu) << 19) | (((c >> 2) & 0x7u) << 16)
            | 0xFF000000u;
   if (fmt == TEXD_1555)
      return ((c & 0x8000u) != 0u ? 0xFF000000u : 0u)
            | (((c >> 10) & 0x1Fu) << 3) | ((c >> 12) & 0x7u)
            | (((c >> 5) & 0x1Fu) << 11) | (((c >> 7) & 0x7u) << 8)
            | ((c & 0x1Fu) << 19) | (((c >> 2) & 0x7u) << 16);
   return (((c >> 12) & 0xFu) << 28) | (((c >> 12) & 0xFu) << 24)
         | (((c >> 8) & 0xFu) << 4) | ((c >> 8) & 0xFu)
         | (((c >> 4) & 0xFu) << 12) | (((c >> 4) & 0xFu) << 8)
         | ((c & 0xFu) << 20) | ((c & 0xFu) << 16);
}

/* A palette index to what @out wants */
TEXD_FN texd_u32 texd_pal(texd_u32 otype, texd_u32 pal, texd_u32 index)
{
   texd_u32 c;
   if (otype == TEXD_OUT8)
      return index;
   c = TEXD_WORD((pal >> 2) + index);
   return otype == TEXD_OUT16 ? (c & 0xFFFFu) : c;
}

/* Texel (@x, @y) of the level: its bits as the converters write them */
TEXD_FN texd_u32 texd_texel(texd_u32 data, texd_u32 pal, texd_u32 sr, texd_u32 mode, texd_u32 x, texd_u32 y)
{
   texd_u32 fmt = mode & 15u;
   texd_u32 scan = (mode >> 4) & 3u;
   texd_u32 otype = (mode >> 6) & 3u;
   texd_u32 lw = (mode >> 12) & 15u;
   texd_u32 lh = (mode >> 16) & 15u;
   texd_u32 t, k, e, div;

   if (fmt == TEXD_BUMP)
      fmt = TEXD_4444;
   if (((mode >> 8) & 1u) != 0u)
   {
      x = 1u;
      y = 1u;
   }
   if (scan >= TEXD_PL)
   {
      texd_u32 stride = sr & 0xFFFFu;
      texd_u32 rows = sr >> 16;
      if (x >= stride || y >= rows)
         return 0u;
      if (scan == TEXD_PL)
      {
         if (fmt == TEXD_YUV)
         {
            e = data + 2u * (y * stride) + 4u * (x >> 1);
            return texd_yuv(texd_byte(e + 1u + 2u * (x & 1u)), texd_byte(e), texd_byte(e + 2u));
         }
         return texd_rgb(fmt, otype, texd_half(data + 2u * (y * stride + x)));
      }
      e = texd_byte(data + y * (stride >> 2) + (x >> 2)) * 8u;
      k = x & 3u;
      if (fmt == TEXD_YUV)
      {
         /* (the same bytes as the planar texture's, read as halves: the
          * Direct3D compiler, fxc, makes the Y of an odd texel the even
          * one's of texd_byte(e + 1u + 2u * (k & 1u)) here) */
         e += 4u * (k >> 1);
         return texd_yuv(texd_half(e + 2u * (k & 1u)) >> 8, texd_half(e) & 0xFFu, texd_half(e + 2u) & 0xFFu);
      }
      return texd_rgb(fmt, otype, texd_half(e + 2u * k));
   }

   t = texd_twiddle(x, y, lw, lh);
   if (scan == TEXD_VQ)
   {
      div = fmt == TEXD_PAL4 ? 16u : fmt == TEXD_PAL8 ? 8u : 4u;
      e = texd_byte(data + t / div) * 8u;	/* the codebook is at the start */
      k = t & (div - 1u);
   }
   else
   {
      e = data;
      k = t;
   }
   if (fmt == TEXD_PAL4)
      return texd_pal(otype, pal, (texd_byte(e + (k >> 1)) >> ((k & 1u) * 4u)) & 15u);
   if (fmt == TEXD_PAL8)
      return texd_pal(otype, pal, texd_byte(e + k));
   if (fmt == TEXD_YUV)
   {
      /* a 2x2 block of four words, twiddled: the left column's upper
       * and lower texel, then the right column's. Y is in each word's
       * high byte; a row's U is in the low byte of its left word, its V
       * in that of its right word */
      texd_u32 by = k & 1u;
      e += 8u * (k >> 2);
      k &= 3u;
      return texd_yuv(texd_half(e + 2u * k) >> 8, texd_half(e + 2u * by) & 0xFFu, texd_half(e + 4u + 2u * by) & 0xFFu);
   }
   return texd_rgb(fmt, otype, texd_half(e + 2u * k));
}

#if defined(TEXD_SHADER_GLSL) && defined(TEXD_KERNEL_FORMAT)
/* a texel of the level for each invocation, in groups of 8x8 */
void main()
{
   uint x = gl_GlobalInvocationID.x;
   uint y = gl_GlobalInvocationID.y;
   if (x >= (lv.wh & 0xFFFFu) || y >= (lv.wh >> 16))
      return;
   imageStore(dst, ivec2(x, y), uvec4(texd_texel(lv.data, lv.pal, lv.sr, lv.mode, x, y), 0u, 0u, 0u));
}
#endif

#if defined(TEXD_SHADER_HLSL) && defined(TEXD_KERNEL_D3D)
#if TEXD_KERNEL_D3D <= 2
/* a texel of the level for each invocation, in groups of 8x8 */
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
   uint v;
   if (id.x >= (lv_wh & 0xFFFFu) || id.y >= (lv_wh >> 16))
      return;
   v = texd_texel(lv_data, lv_pal, lv_sr, lv_mode, id.x, id.y);
#if TEXD_KERNEL_D3D == 1
   texd_dst[id.xy] = uint4(v & 0xFFu, (v >> 8) & 0xFFu, (v >> 16) & 0xFFu, v >> 24);
#else
   texd_dst[id.xy] = v;
#endif
}
#elif TEXD_KERNEL_D3D == 3
float4 main(uint id : SV_VertexID) : SV_Position
{
   return float4(float(id & 1u) * 4.0 - 1.0, float(id >> 1) * 4.0 - 1.0, 0.0, 1.0);
}
#else
/* the texel at the pixel's centre, from the 16 bits the converters make
 * (red at the top, alpha at the bottom) */
float4 main(float4 pos : SV_Position) : SV_Target
{
   uint v = texd_texel(lv_data, lv_pal, lv_sr, lv_mode, uint(pos.x), uint(pos.y));
   uint f = (lv_mode >> 20) & 3u;
   if (f == TEXD_D3D_565)
      return float4(float(v >> 11) / 31.0, float((v >> 5) & 63u) / 63.0, float(v & 31u) / 31.0, 1.0);
   if (f == TEXD_D3D_5551)
      return float4(float(v >> 11) / 31.0, float((v >> 6) & 31u) / 31.0, float((v >> 1) & 31u) / 31.0, float(v & 1u));
   return float4(float(v >> 12) / 15.0, float((v >> 8) & 15u) / 15.0, float((v >> 4) & 15u) / 15.0, float(v & 15u) / 15.0);
}
#endif
#endif

#if !defined(TEXD_SHADER_GLSL) && !defined(TEXD_SHADER_HLSL)

/* What the texture cache hands a renderer that can convert on the GPU:
 * the bytes to copy, and a dispatch for each level. */
#define TEXD_MAX_LEVELS 11

struct texd_level
{
   texd_u32 data, wh, sr, mode;   /* pal is the plan's */
   texd_u32 mip;                  /* the image's level: 0 is the largest */
};

struct texd_plan
{
   texd_u32 src;           /* the first byte in video memory */
   texd_u32 bytes;         /* how many */
   texd_u32 pal_entries;   /* 0, 16 or 256: palette words after them */
   texd_u32 pal_index;     /* the first of them in the palette */
   texd_u32 pal;           /* where they go: the bytes, rounded up to four */
   texd_u32 out;           /* TEXD_OUT8, 16 or 32 */
   texd_u32 width, height; /* of the largest level */
   texd_u32 mipmapped;     /* every level is made, not only the largest */
   texd_u32 levels;
   /* nonzero: a planar 16-bit texture whose texels go to the image as they
    * are, in a format that has them in the PowerVR's order - no kernel,
    * only a copy of rows 'stride' (level[0].sr) texels long */
   texd_u32 native;
   struct texd_level level[TEXD_MAX_LEVELS];
};

/* What the converters make of the level, texel by texel, into @dst at
 * @pitch texels a row: the reference the GPU's kernel is held to. @bytes
 * are the plan's, palette and all. */
static INLINE void texd_c_level(const void *bytes, const struct texd_plan *plan, const struct texd_level *lv, void *dst, texd_u32 pitch)
{
   texd_u32 w = lv->wh & 0xFFFFu, h = lv->wh >> 16, x, y;
   texd_c_set(bytes);
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
      {
         texd_u32 v = texd_texel(lv->data, plan->pal, lv->sr, lv->mode, x, y);
         size_t at = (size_t)y * pitch + x;
         if (plan->out == TEXD_OUT32)
            memcpy((unsigned char *)dst + at * 4, &v, 4);
         else if (plan->out == TEXD_OUT16)
         {
            unsigned short s = (unsigned short)v;
            memcpy((unsigned char *)dst + at * 2, &s, 2);
         }
         else
            ((unsigned char *)dst)[at] = (unsigned char)v;
      }
}

#endif

#endif
