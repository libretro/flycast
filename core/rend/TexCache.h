#pragma once
#include "oslib/oslib.h"
#include "hw/pvr/Renderer_if.h"

#include <algorithm>
#include <array>
#include <retro_atomic.h>
#include <memory>
#include <unordered_map>

extern u8* vq_codebook;
extern u32 palette_index;
extern u32 palette16_ram[1024];
extern u32 palette32_ram[1024];
extern u32 pal_hash_256[4];
extern u32 pal_hash_16[64];
extern bool KillTex;
extern bool palette_updated;

extern u32 detwiddle[2][11][1024];

template<class pixel_type>
class PixelBuffer
{
	pixel_type* p_buffer_start = nullptr;
	pixel_type* p_current_mipmap = nullptr;
	pixel_type* p_current_line = nullptr;
	pixel_type* p_current_pixel = nullptr;

	u32 pixels_per_line = 0;
	bool borrowed = false;	// the memory is the caller's: not freed here

public:
	~PixelBuffer()
   {
		deinit();
   }

	// How much memory a picture this size takes, with every smaller level if @mipmapped
	static size_t bytes(u32 width, u32 height, bool mipmapped)
	{
		size_t size = (size_t)width * height * sizeof(pixel_type);
		if (mipmapped)
		{
			do
			{
				width /= 2;
				height /= 2;
				size += (size_t)width * height * sizeof(pixel_type);
			}
			while (width != 0 && height != 0);
		}
		return size;
	}

	void init(u32 width, u32 height, bool mipmapped)
	{
		deinit();
		p_buffer_start = p_current_line = p_current_pixel = p_current_mipmap = (pixel_type *)malloc(bytes(width, height, mipmapped));
		this->pixels_per_line = 1;
	}

	/* The same on memory the caller has, bytes() of it or more: nothing is
	 * allocated and nothing freed. For what is converted and handed on at
	 * once, texture after texture (texcache_scratch()). */
	void init(u32 width, u32 height, bool mipmapped, void *memory)
	{
		deinit();
		p_buffer_start = p_current_line = p_current_pixel = p_current_mipmap = (pixel_type *)memory;
		this->pixels_per_line = mipmapped ? 1 : width;
		borrowed = true;
	}

   void init(u32 width, u32 height)
   {
      deinit();
		p_buffer_start = p_current_line = p_current_pixel = p_current_mipmap = (pixel_type *)malloc(width * height * sizeof(pixel_type));
		this->pixels_per_line = width;
   }

   void deinit()
	{
		if (p_buffer_start != NULL)
		{
			if (!borrowed)
				free(p_buffer_start);
			p_buffer_start = p_current_mipmap = p_current_line = p_current_pixel = NULL;
		}
		borrowed = false;
	}

	void steal_data(PixelBuffer &buffer)
	{
		deinit();
		p_buffer_start = p_current_mipmap = p_current_line = p_current_pixel = buffer.p_buffer_start;
		pixels_per_line = buffer.pixels_per_line;
		borrowed = buffer.borrowed;
		buffer.borrowed = false;
		buffer.p_buffer_start = buffer.p_current_mipmap = buffer.p_current_line = buffer.p_current_pixel = NULL;
	}

	void set_mipmap(int level)
	{
		size_t offset = 0;
		for (int i = 0; i < level; i++)
			offset += (1 << (2 * i));
		p_current_mipmap = p_current_line = p_current_pixel = p_buffer_start + offset;
		pixels_per_line = 1 << level;
	}

	__forceinline pixel_type *data(u32 x = 0, u32 y = 0)
	{
		return p_current_mipmap + pixels_per_line * y + x;
	}

	// pixels from one line to the next
	__forceinline u32 pitch() const
	{
		return pixels_per_line;
	}

   __forceinline void prel(u32 x,pixel_type value)
 	{
 		p_current_pixel[x]=value;
 	}

   __forceinline void prel(u32 x,u32 y,pixel_type value)
 	{
 		p_current_pixel[y*pixels_per_line+x]=value;
 	}

   __forceinline void rmovex(u32 value)
	{
		p_current_pixel+=value;
	}
	__forceinline void rmovey(u32 value)
	{
		p_current_line+=pixels_per_line*value;
		p_current_pixel=p_current_line;
	}
	__forceinline void amove(u32 x_m,u32 y_m)
	{
		//p_current_pixel=p_buffer_start;
		p_current_line = p_current_mipmap + pixels_per_line * y_m;
		p_current_pixel=p_current_line + x_m;
	}
};

/* Where a converter is writing: the block it is on and how far it is to the
 * line below. The converters used to write through the PixelBuffer itself,
 * whose own fields a store of a 32-bit pixel might - for all the compiler
 * can tell - have changed: they were read again from memory after every
 * pixel, and so was everything else the loop used. A cursor is a local of
 * the loop that nothing else can reach, and stays in registers. */
template<class pixel_type>
struct PixelCursor
{
	pixel_type *p;
	u32 pitch;
	const u32 *pal;	// the part of the palette the texture uses, for the converters that look colours up

	__forceinline void prel(u32 x, pixel_type value)
	{
		p[x] = value;
	}
	__forceinline void prel(u32 x, u32 y, pixel_type value)
	{
		p[y * pitch + x] = value;
	}
};

void palette_update();
void texcache_scratch_free();

#define clamp(minv, maxv, x) (x < minv ? minv : x > maxv ? maxv : x)

// Unpack to 16-bit word

#define ARGB1555( word )	( ((word>>15)&1) | (((word>>10) & 0x1F)<<11)  | (((word>>5) & 0x1F)<<6)  | (((word>>0) & 0x1F)<<1) )

#define ARGB565( word )	( (((word>>0)&0x1F)<<0) | (((word>>5)&0x3F)<<5) | (((word>>11)&0x1F)<<11) )
	
#define ARGB4444( word ) ( (((word>>0)&0xF)<<4) | (((word>>4)&0xF)<<8) | (((word>>8)&0xF)<<12) | (((word>>12)&0xF)<<0) )

#define ARGB8888( word ) ( (((word>>4)&0xF)<<4) | (((word>>12)&0xF)<<8) | (((word>>20)&0xF)<<12) | (((word>>28)&0xF)<<0) )

// Unpack to 32-bit word

#define ARGB1555_32( word )    ( ((word & 0x8000) ? 0xFF000000 : 0) | \
								(((word >> 10) & 0x1F) << 3) | (((word >> 12) & 0x7) << 0) | \
								(((word >> 5) & 0x1F) << 11) | (((word >> 7) & 0x7) << 8) | \
								(((word >> 0) & 0x1F) << 19) | (((word >> 2) & 0x7) << 16) )

#define ARGB565_32( word )     ( (((word >> 11) & 0x1F) << 3) | (((word >> 13) & 0x7) << 0) | \
								(((word >> 5) & 0x3F) << 10) | (((word >> 9) & 0x3) << 8) | \
								(((word >> 0) & 0x1F) << 19) | (((word >> 2) & 0x7) << 16) | \
								0xFF000000 )

#define ARGB4444_32( word ) ( (((word >> 12) & 0xF) << 28) | (((word >> 12) & 0xF) << 24) | \
								(((word >> 8) & 0xF) << 4) | (((word >> 8) & 0xF) << 0) | \
								(((word >> 4) & 0xF) << 12) | (((word >> 4) & 0xF) << 8) | \
								(((word >> 0) & 0xF) << 20) | (((word >> 0) & 0xF) << 16) )

#define ARGB8888_32( word ) ( ((word >> 0) & 0xFF000000) | (((word >> 16) & 0xFF) << 0) | (((word >> 8) & 0xFF) << 8) | ((word & 0xFF) << 16) )

inline static u32 YUV422(s32 Y,s32 Yu,s32 Yv)
{
	Yu-=128;
	Yv-=128;

	s32 R = Y + Yv*11/8;            // Y + (Yv-128) * (11/8) ?
	s32 G = Y - (Yu*11 + Yv*22)/32; // Y - (Yu-128) * (11/8) * 0.25 - (Yv-128) * (11/8) * 0.5 ?
	s32 B = Y + Yu*110/64;          // Y + (Yu-128) * (11/8) * 1.25 ?

	// each held to 0..255 without a branch: what a pixel's colour is cannot be predicted
	R = R < 0 ? 0 : R;
	G = G < 0 ? 0 : G;
	B = B < 0 ? 0 : B;
	R = R > 255 ? 255 : R;
	G = G > 255 ? 255 : G;
	B = B > 255 ? 255 : B;
	return R | (G << 8) | (B << 16) | 0xFF000000;
}

#define twop(x,y,bcx,bcy) (detwiddle[0][bcy][x]+detwiddle[1][bcx][y])

/* The four 16-bit pixels of a twiddled 2x2 block to 32 bits, four at a time
 * where the processor can, and into their two rows: the first and third on
 * the upper, the second and fourth on the lower. Every channel is widened
 * by repeating its top bits, exactly as ARGB565_32() and the others do one
 * pixel at a time - the same bits come out. Planar textures get this from
 * the compiler; in twiddled order it has to be asked for. */
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
typedef __m128i tw4;
#define TW4_LOAD(src)      _mm_unpacklo_epi16(_mm_loadl_epi64((const __m128i *)(src)), _mm_setzero_si128())
#define TW4_SET(k)         _mm_set1_epi32((int)(k))
#define TW4_SHR(v, n)      _mm_srli_epi32(v, n)
#define TW4_SHL(v, n)      _mm_slli_epi32(v, n)
#define TW4_AND(a, b)      _mm_and_si128(a, b)
#define TW4_OR(a, b)       _mm_or_si128(a, b)
#define TW4_SIGN16(v)      _mm_srai_epi32(_mm_slli_epi32(v, 16), 31)   /* all ones where bit 15 is set */
static __forceinline void tw4_store(u32 *row0, u32 *row1, tw4 px)
{
	px = _mm_shuffle_epi32(px, _MM_SHUFFLE(3, 1, 2, 0));
	_mm_storel_epi64((__m128i *)row0, px);
	_mm_storel_epi64((__m128i *)row1, _mm_unpackhi_epi64(px, px));
}
#define TW4_SIMD
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#include <arm_neon.h>
typedef uint32x4_t tw4;
#define TW4_LOAD(src)      vmovl_u16(vld1_u16((const uint16_t *)(src)))
#define TW4_SET(k)         vdupq_n_u32(k)
#define TW4_SHR(v, n)      vshrq_n_u32(v, n)
#define TW4_SHL(v, n)      vshlq_n_u32(v, n)
#define TW4_AND(a, b)      vandq_u32(a, b)
#define TW4_OR(a, b)       vorrq_u32(a, b)
#define TW4_SIGN16(v)      vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_u32(vshlq_n_u32(v, 16)), 31))
static __forceinline void tw4_store(u32 *row0, u32 *row1, tw4 px)
{
	const uint32x2x2_t rows = vuzp_u32(vget_low_u32(px), vget_high_u32(px));
	vst1_u32(row0, rows.val[0]);
	vst1_u32(row1, rows.val[1]);
}
#define TW4_SIMD
#endif

#ifdef TW4_SIMD
// a channel of five bits to eight, of six, of four
#define TW4_5TO8(c)        TW4_OR(TW4_SHL(c, 3), TW4_SHR(c, 2))
#define TW4_6TO8(c)        TW4_OR(TW4_SHL(c, 2), TW4_SHR(c, 4))
#define TW4_4TO8(c)        TW4_OR(TW4_SHL(c, 4), c)

static __forceinline void tw_block_565(u32 *row0, u32 *row1, const u8 *src)
{
	const tw4 v = TW4_LOAD(src);
	const tw4 r = TW4_5TO8(TW4_SHR(v, 11));
	const tw4 g = TW4_6TO8(TW4_AND(TW4_SHR(v, 5), TW4_SET(0x3F)));
	const tw4 b = TW4_5TO8(TW4_AND(v, TW4_SET(0x1F)));
	tw4_store(row0, row1, TW4_OR(TW4_OR(r, TW4_SHL(g, 8)), TW4_OR(TW4_SHL(b, 16), TW4_SET(0xFF000000))));
}

static __forceinline void tw_block_1555(u32 *row0, u32 *row1, const u8 *src)
{
	const tw4 v = TW4_LOAD(src);
	const tw4 r = TW4_5TO8(TW4_AND(TW4_SHR(v, 10), TW4_SET(0x1F)));
	const tw4 g = TW4_5TO8(TW4_AND(TW4_SHR(v, 5), TW4_SET(0x1F)));
	const tw4 b = TW4_5TO8(TW4_AND(v, TW4_SET(0x1F)));
	const tw4 a = TW4_AND(TW4_SIGN16(v), TW4_SET(0xFF000000));
	tw4_store(row0, row1, TW4_OR(TW4_OR(r, TW4_SHL(g, 8)), TW4_OR(TW4_SHL(b, 16), a)));
}

static __forceinline void tw_block_4444(u32 *row0, u32 *row1, const u8 *src)
{
	const tw4 v = TW4_LOAD(src);
	const tw4 a = TW4_4TO8(TW4_SHR(v, 12));
	const tw4 r = TW4_4TO8(TW4_AND(TW4_SHR(v, 8), TW4_SET(0xF)));
	const tw4 g = TW4_4TO8(TW4_AND(TW4_SHR(v, 4), TW4_SET(0xF)));
	const tw4 b = TW4_4TO8(TW4_AND(v, TW4_SET(0xF)));
	tw4_store(row0, row1, TW4_OR(TW4_OR(r, TW4_SHL(g, 8)), TW4_OR(TW4_SHL(b, 16), TW4_SHL(a, 24))));
}
#else
// one at a time, where there is nothing better
#define TW_BLOCK_SCALAR(name, unpack) \
static __forceinline void name(u32 *row0, u32 *row1, const u8 *src) \
{ \
	const u16 *p_in = (const u16 *)src; \
	row0[0] = unpack(p_in[0]); \
	row1[0] = unpack(p_in[1]); \
	row0[1] = unpack(p_in[2]); \
	row1[1] = unpack(p_in[3]); \
}
TW_BLOCK_SCALAR(tw_block_565, ARGB565_32)
TW_BLOCK_SCALAR(tw_block_1555, ARGB1555_32)
TW_BLOCK_SCALAR(tw_block_4444, ARGB4444_32)
#endif

/* And to 16 bits. The three conversions there are a 16-bit turn to the left
 * - by nothing for 565, by one bit for 1555 (the alpha bit goes from the top
 * to the bottom), by four for 4444 (the alpha nibble likewise) - so a block's
 * four pixels are turned at once as the 64-bit number they are in memory,
 * and written as two 32-bit halves, one a row. Where bytes are the other
 * way round they are done one by one. */
#ifndef MSB_FIRST
static __forceinline void tw16_block(u16 *row0, u16 *row1, const u8 *src, const u32 turn)
{
	u64 v;
	u32 lo, hi, upper, lower;

	memcpy(&v, src, sizeof(v));
	if (turn != 0)
	{
		const u64 low_bits = 0x0001000100010001ull * ((1u << turn) - 1);
		v = ((v << turn) & ~low_bits) | ((v >> (16 - turn)) & low_bits);
	}
	lo = (u32)v;
	hi = (u32)(v >> 32);
	upper = (lo & 0xFFFF) | (hi << 16);          // the first and the third
	lower = (lo >> 16) | (hi & 0xFFFF0000);      // the second and the fourth
	memcpy(row0, &upper, sizeof(upper));
	memcpy(row1, &lower, sizeof(lower));
}
#else
static __forceinline void tw16_block(u16 *row0, u16 *row1, const u8 *src, const u32 turn)
{
	const u16 *p_in = (const u16 *)src;
	row0[0] = (u16)((p_in[0] << turn) | (p_in[0] >> ((16 - turn) & 15)));
	row1[0] = (u16)((p_in[1] << turn) | (p_in[1] >> ((16 - turn) & 15)));
	row0[1] = (u16)((p_in[2] << turn) | (p_in[2] >> ((16 - turn) & 15)));
	row1[1] = (u16)((p_in[3] << turn) | (p_in[3] >> ((16 - turn) & 15)));
}
#endif

/* Two or four pixels next to each other, written as one number where that
 * is a store less (or three). For the palette converters, whose pixels come
 * out of a table one by one. */
#ifndef MSB_FIRST
static __forceinline void tw_put2(u16 *row, u32 a, u32 b)
{
	const u32 v = (a & 0xFFFF) | (b << 16);
	memcpy(row, &v, sizeof(v));
}
static __forceinline void tw_put2(u32 *row, u32 a, u32 b)
{
	const u64 v = a | ((u64)b << 32);
	memcpy(row, &v, sizeof(v));
}
static __forceinline void tw_put4(u16 *row, u32 a, u32 b, u32 c, u32 d)
{
	const u64 v = (a & 0xFFFF) | ((u64)(b & 0xFFFF) << 16) | ((u64)(c & 0xFFFF) << 32) | ((u64)d << 48);
	memcpy(row, &v, sizeof(v));
}
#else
static __forceinline void tw_put2(u16 *row, u32 a, u32 b) { row[0] = (u16)a; row[1] = (u16)b; }
static __forceinline void tw_put2(u32 *row, u32 a, u32 b) { row[0] = a; row[1] = b; }
static __forceinline void tw_put4(u16 *row, u32 a, u32 b, u32 c, u32 d) { row[0] = (u16)a; row[1] = (u16)b; row[2] = (u16)c; row[3] = (u16)d; }
#endif
static __forceinline void tw_put4(u32 *row, u32 a, u32 b, u32 c, u32 d)
{
	tw_put2(row, a, b);
	tw_put2(row + 2, c, d);
}

/* Four YUV pixels at a time. They come as U Y V Y U Y V Y, two pixels
 * sharing a U and a V, and go through the same arithmetic as YUV422()
 * above - the divisions rounding towards zero as C's do, the results held
 * to 0..255 by the saturating pack - sixteen bits a lane. The four pixels
 * come out in order. */
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define YUV4_SSE2
// a lane divided by 8, 32 or 64 (@shift 3, 5, 6), towards zero
#define YUV4_DIV(t, shift) _mm_srai_epi16(_mm_add_epi16(t, _mm_and_si128(_mm_srai_epi16(t, 15), _mm_set1_epi16((1 << (shift)) - 1))), shift)
static __forceinline void yuv_convert(const __m128i in, __m128i *rg, __m128i *ba)
{
	const __m128i y = _mm_srli_epi16(in, 8);
	const __m128i uv = _mm_and_si128(in, _mm_set1_epi16(0x00FF));
	__m128i u = _mm_and_si128(uv, _mm_set1_epi32(0x0000FFFF));
	__m128i v = _mm_srli_epi32(uv, 16);
	__m128i t, r, g, b, rb, ga;

	// each U and V under both of its pixels, less 128
	u = _mm_sub_epi16(_mm_or_si128(u, _mm_slli_epi32(u, 16)), _mm_set1_epi16(128));
	v = _mm_sub_epi16(_mm_or_si128(v, _mm_slli_epi32(v, 16)), _mm_set1_epi16(128));
	t = _mm_mullo_epi16(v, _mm_set1_epi16(11));
	r = _mm_add_epi16(y, YUV4_DIV(t, 3));
	t = _mm_add_epi16(_mm_mullo_epi16(u, _mm_set1_epi16(11)), _mm_slli_epi16(t, 1));
	g = _mm_sub_epi16(y, YUV4_DIV(t, 5));
	t = _mm_mullo_epi16(u, _mm_set1_epi16(110));
	b = _mm_add_epi16(y, YUV4_DIV(t, 6));
	rb = _mm_packus_epi16(r, b);
	ga = _mm_packus_epi16(g, _mm_set1_epi16(255));
	*rg = _mm_unpacklo_epi8(rb, ga);
	*ba = _mm_unpackhi_epi8(rb, ga);
}

// four pixels, from eight bytes in the low half of @in
static __forceinline __m128i yuv4_convert(const __m128i in)
{
	__m128i rg, ba;
	yuv_convert(in, &rg, &ba);
	return _mm_unpacklo_epi16(rg, ba);
}
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#define YUV4_NEON
#define YUV4_DIV(t, shift) vshr_n_s16(vadd_s16(t, vand_s16(vshr_n_s16(t, 15), vdup_n_s16((1 << (shift)) - 1))), shift)
static __forceinline void yuv4_convert(const uint8x8_t in, uint8x8_t *first, uint8x8_t *second)
{
	const uint16x4_t w = vreinterpret_u16_u8(in);
	const int16x4_t y = vreinterpret_s16_u16(vshr_n_u16(w, 8));
	const uint32x2_t uv = vreinterpret_u32_u16(vand_u16(w, vdup_n_u16(0x00FF)));
	uint32x2_t u32v = vand_u32(uv, vdup_n_u32(0x0000FFFF));
	uint32x2_t v32v = vshr_n_u32(uv, 16);
	int16x4_t u, v, t, r, g, b;
	uint8x8_t rb, ga;
	uint8x8x2_t bytes;
	uint16x4x2_t words;

	u = vsub_s16(vreinterpret_s16_u32(vorr_u32(u32v, vshl_n_u32(u32v, 16))), vdup_n_s16(128));
	v = vsub_s16(vreinterpret_s16_u32(vorr_u32(v32v, vshl_n_u32(v32v, 16))), vdup_n_s16(128));
	t = vmul_n_s16(v, 11);
	r = vadd_s16(y, YUV4_DIV(t, 3));
	t = vadd_s16(vmul_n_s16(u, 11), vshl_n_s16(t, 1));
	g = vsub_s16(y, YUV4_DIV(t, 5));
	t = vmul_n_s16(u, 110);
	b = vadd_s16(y, YUV4_DIV(t, 6));
	rb = vqmovun_s16(vcombine_s16(r, b));
	ga = vqmovun_s16(vcombine_s16(g, vdup_n_s16(255)));
	bytes = vzip_u8(rb, ga);
	words = vzip_u16(vreinterpret_u16_u8(bytes.val[0]), vreinterpret_u16_u8(bytes.val[1]));
	*first = vreinterpret_u8_u16(words.val[0]);
	*second = vreinterpret_u8_u16(words.val[1]);
}
#endif

//pixel convertors !
#define pixelcvt_start_base(name,x,y,type) \
		struct name \
		{ \
			static const u32 xpp=x;\
			static const u32 ypp=y;	\
			template<class Out> __forceinline static void Convert(Out* pb,u8* data) \
		{

#define pixelcvt_start(name,x,y) pixelcvt_start_base(name, x, y, u16)
#define pixelcvt32_start(name,x,y) pixelcvt_start_base(name, x, y, u32)

#define pixelcvt_size_start(name, x, y) template<class pixel_size> \
struct name \
{ \
	static const u32 xpp=x;\
	static const u32 ypp=y;	\
   template<class Out> __forceinline static void Convert(Out* pb,u8* data) \
{

#define pixelcvt_end } }
#define pixelcvt_next(name,x,y) pixelcvt_end;  pixelcvt_start(name,x,y)
//
//Non twiddled
//
// 16-bit pixel buffer
pixelcvt_start(conv565_PL,4,1)
{
   //convert 4x1
	u16* p_in=(u16*)data;
	//0,0
	pb->prel(0,ARGB565(p_in[0]));
	//1,0
	pb->prel(1,ARGB565(p_in[1]));
	//2,0
	pb->prel(2,ARGB565(p_in[2]));
	//3,0
	pb->prel(3,ARGB565(p_in[3]));
}

pixelcvt_next(conv1555_PL,4,1)
{
   //convert 4x1
	u16* p_in=(u16*)data;
	//0,0
	pb->prel(0,ARGB1555(p_in[0]));
	//1,0
	pb->prel(1,ARGB1555(p_in[1]));
	//2,0
	pb->prel(2,ARGB1555(p_in[2]));
	//3,0
	pb->prel(3,ARGB1555(p_in[3]));
}

pixelcvt_next(conv4444_PL,4,1)
{
   //convert 4x1
	u16* p_in=(u16*)data;
	//0,0
	pb->prel(0,ARGB4444(p_in[0]));
	//1,0
	pb->prel(1,ARGB4444(p_in[1]));
	//2,0
	pb->prel(2,ARGB4444(p_in[2]));
	//3,0
	pb->prel(3,ARGB4444(p_in[3]));
}
pixelcvt_end;

// 32-bit pixel buffer
pixelcvt32_start(conv565_PL32,4,1)
{
	//convert 4x1
	u16* p_in=(u16*)data;
	//0,0
	pb->prel(0,ARGB565_32(p_in[0]));
	//1,0
	pb->prel(1,ARGB565_32(p_in[1]));
	//2,0
	pb->prel(2,ARGB565_32(p_in[2]));
	//3,0
	pb->prel(3,ARGB565_32(p_in[3]));
}
pixelcvt_end;
pixelcvt32_start(conv1555_PL32,4,1)
{
	//convert 4x1
	u16* p_in=(u16*)data;
	//0,0
	pb->prel(0,ARGB1555_32(p_in[0]));
	//1,0
	pb->prel(1,ARGB1555_32(p_in[1]));
	//2,0
	pb->prel(2,ARGB1555_32(p_in[2]));
	//3,0
	pb->prel(3,ARGB1555_32(p_in[3]));
}
pixelcvt_end;
pixelcvt32_start(conv4444_PL32,4,1)
{
	//convert 4x1
	u16* p_in=(u16*)data;
	//0,0
	pb->prel(0,ARGB4444_32(p_in[0]));
	//1,0
	pb->prel(1,ARGB4444_32(p_in[1]));
	//2,0
	pb->prel(2,ARGB4444_32(p_in[2]));
	//3,0
	pb->prel(3,ARGB4444_32(p_in[3]));
}
pixelcvt_end;

pixelcvt32_start(convYUV_PL,4,1)
{
#if defined(YUV4_SSE2)
	_mm_storeu_si128((__m128i *)pb->p, yuv4_convert(_mm_loadl_epi64((const __m128i *)data)));
#elif defined(YUV4_NEON)
	uint8x8_t first, second;
	yuv4_convert(vld1_u8(data), &first, &second);
	vst1_u8((u8 *)pb->p, first);
	vst1_u8((u8 *)(pb->p + 2), second);
#else
   //convert 4x1 4444 to 4x1 8888
	u32* p_in=(u32*)data;


	s32 Y0 = (p_in[0]>>8) &255; //
	s32 Yu = (p_in[0]>>0) &255; //p_in[0]
	s32 Y1 = (p_in[0]>>24) &255; //p_in[3]
	s32 Yv = (p_in[0]>>16) &255; //p_in[2]

	//0,0
	pb->prel(0,YUV422(Y0,Yu,Yv));
	//1,0
	pb->prel(1,YUV422(Y1,Yu,Yv));

	//next 4 bytes
	p_in+=1;

	Y0 = (p_in[0]>>8) &255; //
	Yu = (p_in[0]>>0) &255; //p_in[0]
	Y1 = (p_in[0]>>24) &255; //p_in[3]
	Yv = (p_in[0]>>16) &255; //p_in[2]

	//0,0
	pb->prel(2,YUV422(Y0,Yu,Yv));
	//1,0
	pb->prel(3,YUV422(Y1,Yu,Yv));
#endif
}
pixelcvt_end;

/* The same, eight pixels at a time, for a planar texture's rows - a video's,
 * usually. (The four-pixel one stays for compressed planar textures, whose
 * codebook entries are four pixels.) */
pixelcvt32_start(convYUV_PL8,8,1)
{
#if defined(YUV4_SSE2)
	__m128i rg, ba;
	yuv_convert(_mm_loadu_si128((const __m128i *)data), &rg, &ba);
	_mm_storeu_si128((__m128i *)pb->p, _mm_unpacklo_epi16(rg, ba));
	_mm_storeu_si128((__m128i *)(pb->p + 4), _mm_unpackhi_epi16(rg, ba));
#else
	Out next = *pb;
	next.p += 4;
	convYUV_PL::Convert(pb, data);
	convYUV_PL::Convert(&next, data + 8);
#endif
}
pixelcvt_end;

//
//twiddled 
//
// 16-bit pixel buffer
pixelcvt_start(conv565_TW,2,2)
{
	tw16_block(pb->p, pb->p + pb->pitch, data, 0);
}
pixelcvt_next(conv1555_TW,2,2)
{
	tw16_block(pb->p, pb->p + pb->pitch, data, 1);
}
pixelcvt_next(conv4444_TW,2,2)
{
	tw16_block(pb->p, pb->p + pb->pitch, data, 4);
}
pixelcvt_end;

// 32-bit pixel buffer
pixelcvt32_start(conv565_TW32,2,2)
{
	tw_block_565(pb->p, pb->p + pb->pitch, data);
}
pixelcvt_end;
pixelcvt32_start(conv1555_TW32,2,2)
{
	tw_block_1555(pb->p, pb->p + pb->pitch, data);
}
pixelcvt_end;
pixelcvt32_start(conv4444_TW32,2,2)
{
	tw_block_4444(pb->p, pb->p + pb->pitch, data);
}
pixelcvt_end;


pixelcvt32_start(convYUV_TW,2,2)
{
#if defined(YUV4_SSE2)
	// the block's second and third words change places: then it is two pixels of the upper row and two of the lower
	const __m128i px = yuv4_convert(_mm_shufflelo_epi16(_mm_loadl_epi64((const __m128i *)data), _MM_SHUFFLE(3, 1, 2, 0)));
	_mm_storel_epi64((__m128i *)pb->p, px);
	_mm_storel_epi64((__m128i *)(pb->p + pb->pitch), _mm_unpackhi_epi64(px, px));
#elif defined(YUV4_NEON)
	static const u8 order[8] = { 0, 1, 4, 5, 2, 3, 6, 7 };
	uint8x8_t upper, lower;
	yuv4_convert(vtbl1_u8(vld1_u8(data), vld1_u8(order)), &upper, &lower);
	vst1_u8((u8 *)pb->p, upper);
	vst1_u8((u8 *)(pb->p + pb->pitch), lower);
#else
   //convert 4x1 4444 to 4x1 8888
	u16* p_in=(u16*)data;


	s32 Y0 = (p_in[0]>>8) &255; //
	s32 Yu = (p_in[0]>>0) &255; //p_in[0]
	s32 Y1 = (p_in[2]>>8) &255; //p_in[3]
	s32 Yv = (p_in[2]>>0) &255; //p_in[2]

	//0,0
	pb->prel(0,0,YUV422(Y0,Yu,Yv));
	//1,0
	pb->prel(1,0,YUV422(Y1,Yu,Yv));

	//next 4 bytes
	//p_in+=2;

	Y0 = (p_in[1]>>8) &255; //
	Yu = (p_in[1]>>0) &255; //p_in[0]
	Y1 = (p_in[3]>>8) &255; //p_in[3]
	Yv = (p_in[3]>>0) &255; //p_in[2]

	//0,1
	pb->prel(0,1,YUV422(Y0,Yu,Yv));
	//1,1
	pb->prel(1,1,YUV422(Y1,Yu,Yv));
#endif
}
pixelcvt_end;

// 16-bit && 32-bit pixel buffers
pixelcvt_size_start(convPAL4_TW,4,4)
{
	/* Sixteen pixels, a nibble each, in twiddled order: the first eight are
	 * the left half of the block and the last eight the right, each half
	 * going down its two columns two rows at a time. A row is written at
	 * once. */
	const u8 * const p_in = data;
	const u32 * const pal = pb->pal;
	pixel_size * const r0 = pb->p;
	pixel_size * const r1 = r0 + pb->pitch;
	pixel_size * const r2 = r1 + pb->pitch;
	pixel_size * const r3 = r2 + pb->pitch;
#define PAL4_N(i) pal[(p_in[(i) >> 1] >> (((i) & 1) * 4)) & 0xF]
	tw_put4(r0, PAL4_N(0), PAL4_N(2), PAL4_N(8), PAL4_N(10));
	tw_put4(r1, PAL4_N(1), PAL4_N(3), PAL4_N(9), PAL4_N(11));
	tw_put4(r2, PAL4_N(4), PAL4_N(6), PAL4_N(12), PAL4_N(14));
	tw_put4(r3, PAL4_N(5), PAL4_N(7), PAL4_N(13), PAL4_N(15));
#undef PAL4_N
}
pixelcvt_end;

// Palette 4bpp -> 8bpp
pixelcvt_size_start(convPAL4PT_TW, 4, 4)
{
	u8* p_in = (u8 *)data;

	pb->prel(0, 0, p_in[0] & 0xF);
	pb->prel(0, 1, (p_in[0] >> 4) & 0xF); p_in++;
	pb->prel(1, 0, p_in[0] & 0xF);
	pb->prel(1, 1, (p_in[0] >> 4) & 0xF); p_in++;

	pb->prel(0, 2, p_in[0] & 0xF);
	pb->prel(0, 3, (p_in[0] >> 4) & 0xF); p_in++;
	pb->prel(1, 2, p_in[0] & 0xF);
	pb->prel(1, 3, (p_in[0] >> 4) & 0xF); p_in++;

	pb->prel(2, 0, p_in[0] & 0xF);
	pb->prel(2, 1, (p_in[0] >> 4) & 0xF); p_in++;
	pb->prel(3, 0, p_in[0] & 0xF);
	pb->prel(3, 1, (p_in[0] >> 4) & 0xF); p_in++;

	pb->prel(2, 2, p_in[0] & 0xF);
	pb->prel(2, 3, (p_in[0] >> 4) & 0xF); p_in++;
	pb->prel(3, 2, p_in[0] & 0xF);
	pb->prel(3, 3, (p_in[0] >> 4) & 0xF); p_in++;
}
pixelcvt_end;

pixelcvt_size_start(convPAL8_TW,2,4)
{
	// eight pixels, a byte each: two columns, going down two rows at a time
	const u8 * const p_in = data;
	const u32 * const pal = pb->pal;
	pixel_size * const r0 = pb->p;
	pixel_size * const r1 = r0 + pb->pitch;
	pixel_size * const r2 = r1 + pb->pitch;
	pixel_size * const r3 = r2 + pb->pitch;
	tw_put2(r0, pal[p_in[0]], pal[p_in[2]]);
	tw_put2(r1, pal[p_in[1]], pal[p_in[3]]);
	tw_put2(r2, pal[p_in[4]], pal[p_in[6]]);
	tw_put2(r3, pal[p_in[5]], pal[p_in[7]]);
}
pixelcvt_end;

// Palette 8bpp -> 8bpp (untwiddle only)
pixelcvt_size_start(convPAL8PT_TW, 2, 4)
{
	u8* p_in = (u8 *)data;

	pb->prel(0, 0, p_in[0]); p_in++;
	pb->prel(0, 1, p_in[0]); p_in++;
	pb->prel(1, 0, p_in[0]); p_in++;
	pb->prel(1, 1, p_in[0]); p_in++;

	pb->prel(0, 2, p_in[0]); p_in++;
	pb->prel(0, 3, p_in[0]); p_in++;
	pb->prel(1, 2, p_in[0]); p_in++;
	pb->prel(1, 3, p_in[0]); p_in++;
}
pixelcvt_end;


//handler functions

/* Compressed and not twiddled: each byte names a codebook entry, as for
 * any compressed texture, and an entry is four pixels in a row, left to
 * right. */
template<class PixelConvertor, class pixel_type>
void texture_PLVQ(PixelBuffer<pixel_type>* pb,u8* p_in,u32 Width,u32 Height)
{
	u8 * const codebook = vq_codebook;
	pixel_type *line = pb->data();
	const u32 pitch = pb->pitch();
	const u32 * const pal = (sizeof(pixel_type) == 2 ? palette16_ram : palette32_ram) + palette_index;

	p_in += 256 * 4 * 2;	// Skip VQ codebook
	Height /= PixelConvertor::ypp;
	Width /= PixelConvertor::xpp;

	for (u32 y = 0; y < Height; y++)
	{
		for (u32 x = 0; x < Width; x++)
		{
			PixelCursor<pixel_type> out = { line + x * PixelConvertor::xpp, pitch, pal };
			PixelConvertor::Convert(&out, &codebook[*p_in++ * 8]);
		}
		line += pitch * PixelConvertor::ypp;
	}
}

template<class PixelConvertor, class pixel_type>
void texture_PL(PixelBuffer<pixel_type>* pb,u8* p_in,u32 Width,u32 Height)
{
	pixel_type *line = pb->data();
	const u32 pitch = pb->pitch();
	const u32 * const pal = (sizeof(pixel_type) == 2 ? palette16_ram : palette32_ram) + palette_index;

	Height/=PixelConvertor::ypp;
	Width/=PixelConvertor::xpp;

	for (u32 y=0;y<Height;y++)
	{
		for (u32 x=0;x<Width;x++)
		{
			PixelCursor<pixel_type> out = { line + x * PixelConvertor::xpp, pitch, pal };
			PixelConvertor::Convert(&out, p_in);
			p_in += 2 * PixelConvertor::xpp;	// two bytes a pixel
		}
		line += pitch * PixelConvertor::ypp;
	}
}

/* Twiddled: the block for (x, y) is at the sum of two table entries, one for
 * each. The one for y is the same along a row. */
template<class PixelConvertor, class pixel_type>
void texture_TW(PixelBuffer<pixel_type>* pb,u8* p_in,u32 Width,u32 Height)
{
	const u32 divider = PixelConvertor::xpp * PixelConvertor::ypp;
	const u32 * const of_x = detwiddle[0][bitscanrev(Height)];
	const u32 * const of_y = detwiddle[1][bitscanrev(Width)];
	pixel_type *line = pb->data();
	const u32 pitch = pb->pitch();
	const u32 * const pal = (sizeof(pixel_type) == 2 ? palette16_ram : palette32_ram) + palette_index;

	for (u32 y = 0; y < Height; y += PixelConvertor::ypp)
	{
		const u32 row = of_y[y];
		for (u32 x = 0; x < Width; x += PixelConvertor::xpp)
		{
			PixelCursor<pixel_type> out = { line + x, pitch, pal };
			PixelConvertor::Convert(&out, &p_in[((of_x[x] + row) / divider) << 3]);
		}
		line += pitch * PixelConvertor::ypp;
	}
}

template<class PixelConvertor, class pixel_type>
void texture_VQ(PixelBuffer<pixel_type>* pb,u8* p_in,u32 Width,u32 Height)
{
	const u32 divider = PixelConvertor::xpp * PixelConvertor::ypp;
	const u32 * const of_x = detwiddle[0][bitscanrev(Height)];
	const u32 * const of_y = detwiddle[1][bitscanrev(Width)];
	u8 * const codebook = vq_codebook;
	pixel_type *line = pb->data();
	const u32 pitch = pb->pitch();
	const u32 * const pal = (sizeof(pixel_type) == 2 ? palette16_ram : palette32_ram) + palette_index;

	p_in += 256 * 4 * 2;	// Skip VQ codebook

	for (u32 y = 0; y < Height; y += PixelConvertor::ypp)
	{
		const u32 row = of_y[y];
		for (u32 x = 0; x < Width; x += PixelConvertor::xpp)
		{
			PixelCursor<pixel_type> out = { line + x, pitch, pal };
			PixelConvertor::Convert(&out, &codebook[p_in[(of_x[x] + row) / divider] * 8]);
		}
		line += pitch * PixelConvertor::ypp;
	}
}

//Planar
#define tex565_PL texture_PL<conv565_PL, u16>
#define tex1555_PL texture_PL<conv1555_PL, u16>
#define tex4444_PL texture_PL<conv4444_PL, u16>
#define texYUV422_PL texture_PL<convYUV_PL8, u32>
#define texBMP_PL tex4444_PL

#define tex565_PL32 texture_PL<conv565_PL32, u32>
#define tex1555_PL32 texture_PL<conv1555_PL32, u32>
#define tex4444_PL32 texture_PL<conv4444_PL32, u32>

//Planar and compressed
#define tex565_PLVQ32 texture_PLVQ<conv565_PL32, u32>
#define tex1555_PLVQ32 texture_PLVQ<conv1555_PL32, u32>
#define tex4444_PLVQ32 texture_PLVQ<conv4444_PL32, u32>
#define texYUV422_PLVQ texture_PLVQ<convYUV_PL, u32>

//Twiddle
#define tex565_TW texture_TW<conv565_TW, u16>
#define tex1555_TW texture_TW<conv1555_TW, u16>
#define tex4444_TW texture_TW<conv4444_TW, u16>
#define texYUV422_TW texture_TW<convYUV_TW, u32>
#define texBMP_TW tex4444_TW
#define texPAL4_TW texture_TW<convPAL4_TW<u16>, u16>
#define texPAL8_TW texture_TW<convPAL8_TW<u16>, u16>
#define texPAL4_TW32 texture_TW<convPAL4_TW<u32>, u32>
#define texPAL8_TW32  texture_TW<convPAL8_TW<u32>, u32>
#define texPAL4PT_TW texture_TW<convPAL4PT_TW<u8>, u8>
#define texPAL8PT_TW texture_TW<convPAL8PT_TW<u8>, u8>

#define tex565_TW32 texture_TW<conv565_TW32, u32>
#define tex1555_TW32 texture_TW<conv1555_TW32, u32>
#define tex4444_TW32 texture_TW<conv4444_TW32, u32>

//VQ
#define tex565_VQ texture_VQ<conv565_TW, u16>
#define tex1555_VQ texture_VQ<conv1555_TW, u16>
#define tex4444_VQ texture_VQ<conv4444_TW, u16>
#define texYUV422_VQ texture_VQ<convYUV_TW, u32>
#define texBMP_VQ tex4444_VQ
// According to the documentation, a texture cannot be compressed and use
// a palette at the same time. However the hardware displays them
// just fine.
#define texPAL4_VQ texture_VQ<convPAL4_TW<u16>, u16>
#define texPAL8_VQ texture_VQ<convPAL8_TW<u16>, u16>

#define tex565_VQ32 texture_VQ<conv565_TW32, u32>
#define tex1555_VQ32 texture_VQ<conv1555_TW32, u32>
#define tex4444_VQ32 texture_VQ<conv4444_TW32, u32>
#define texPAL4_VQ32 texture_VQ<convPAL4_TW<u32>, u32>
#define texPAL8_VQ32 texture_VQ<convPAL8_TW<u32>, u32>

class BaseTextureCacheData;

bool VramLockedWriteOffset(size_t offset);
void libCore_vramlock_Lock(u32 start_offset, u32 end_offset, BaseTextureCacheData *texture);

#ifdef HAVE_TEXUPSCALE
void UpscalexBRZ(int factor, u32* source, u32* dest, int width, int height, bool has_alpha);
#endif

struct PvrTexInfo;
template <class pixel_type> class PixelBuffer;
typedef void TexConvFP(PixelBuffer<u16>* pb,u8* p_in,u32 Width,u32 Height);
typedef void TexConvFP8(PixelBuffer<u8>* pb, u8* p_in, u32 Width, u32 Height);
typedef void TexConvFP32(PixelBuffer<u32>* pb,u8* p_in,u32 Width,u32 Height);
enum class TextureType { _565, _5551, _4444, _8888, _8 };

/* After this many updates a texture is treated as frequently updated: its vram
 * is left writable and changes are detected by content hash instead of a write
 * fault (see BaseTextureCacheData::Update / NeedsUpdate). */
#define FREQUENT_UPDATE_COUNT 8

class BaseTextureCacheData
{
public:
	TSP tsp;        //dreamcast texture parameters
	TCW tcw;

	// Decoded/filtered texture format
	TextureType tex_type;

	u32 sa;         //pixel data start address in vram (might be offset for mipmaps/etc)
	u32 sa_tex;		//texture data start address in vram
	u32 w,h;        //width & height of the texture
	u32 size;       //size, in bytes, in vram

	const PvrTexInfo* tex;
	TexConvFP*  texconv;
	TexConvFP32*  texconv32;
	TexConvFP8 *texconv8;

	u32 dirty;
	vram_block* lock_block;

	u32 Updates;
	u32 vram_hash;				// content hash of the locked vram range, used instead of
								// write-protection for frequently-updated textures

	u32 palette_index;
	u8 area = 0;				// 1 if it was first asked for as a second volume's texture: see IsGpuHandledPaletted()
	//used for palette updates
	u32 palette_hash;			// Palette hash at time of last update
	u32 vq_codebook;            // VQ quantizers table for compressed textures
	u32 texture_hash;			// xxhash of texture data, used for custom textures
	u32 old_texture_hash;		// legacy hash
	u32 old_vqtexture_hash;		// the name a compressed texture had while only part of it was hashed; 0 if not compressed
	u8* custom_image_data;		// loaded custom image data
	u32 custom_width;
	u32 custom_height;
	retro_atomic_int_t custom_load_in_progress;
	BaseTextureCacheData *custom_load_next;	// link in the custom texture loader's work list

	void PrintTextureName();
	virtual std::string GetId() = 0;

	bool IsPaletted()
	{
		return tcw.PixelFmt == PixelPal4 || tcw.PixelFmt == PixelPal8;
	}

	bool IsMipmapped()
	{
		// for a paletted texture that bit is not the scan order but part of
		// the palette selection
		return tcw.MipMapped != 0 && (IsPaletted() || tcw.ScanOrder == 0) && !settings.rend.ForceTextureLOD0;
	}

	const char* GetPixelFormatName()
	{
		switch (tcw.PixelFmt)
		{
		case Pixel1555: return "1555";
		case Pixel565: return "565";
		case Pixel4444: return "4444";
		case PixelYUV: return "yuv";
		case PixelBumpMap: return "bumpmap";
		case PixelPal4: return "pal4";
		case PixelPal8: return "pal8";
		default: return "unknown";
		}
	}

	bool IsCustomTextureAvailable()
	{
		return retro_atomic_load_acquire_int(&custom_load_in_progress) == 0 && custom_image_data != NULL;
	}

	void Create();
	void ComputeHash();
	u32 ComputeVramHash();
	void Update();
	virtual void UploadToGPU(int width, int height, u8 *temp_tex_buffer, bool mipmapped, bool mipmapsIncluded = false) = 0;
	virtual bool Force32BitTexture(TextureType type) const { return false; }
	void CheckCustomTexture();
	//true if : dirty or paletted texture and hashes don't match
	bool NeedsUpdate();
	virtual bool Delete();
	virtual ~BaseTextureCacheData() {}
	static bool IsGpuHandledPaletted(TSP tsp, TCW tcw, int area = 0)
	{
		// Some palette textures are handled on the GPU
		// This is currently limited to textures using nearest filtering and not mipmapped.
		// Enabling texture upscaling or dumping also disables this mode.
		/* And to a polygon's first texture. The shaders have one palette
		 * position to look colours up from, the first texture's: a second
		 * volume's texture left as indices was looked up with that, or, with
		 * a first texture that has no palette, shown as its indices. It is
		 * made colours here, like any texture. */
		return (tcw.PixelFmt == PixelPal4 || tcw.PixelFmt == PixelPal8)
				&& settings.rend.TextureUpscale == 1
				&& !settings.rend.DumpTextures
				&& tsp.FilterMode == 0
				&& !tcw.MipMapped
				&& !tcw.VQ_Comp
				&& area == 0;
	}
};

template<typename Texture>
class BaseTextureCache
{
	using TexCacheIter = typename std::unordered_map<u64, Texture>::iterator;
public:
	Texture *getTextureCacheData(TSP tsp, TCW tcw, int area = 0)
	{
		u64 key = tsp.full & TSPTextureCacheMask.full;
		if ((tcw.PixelFmt == PixelPal4 || tcw.PixelFmt == PixelPal8)
				&& !BaseTextureCacheData::IsGpuHandledPaletted(tsp, tcw, area))
			// Paletted textures have a palette selection that must be part of the key
			// We also add the palette type to the key to avoid thrashing the cache
			// when the palette type is changed. If the palette type is changed back in the future,
			// this texture will stil be available.
			// (bit 9: made colours here, which a texture of the same address left as indices is not)
			key |= ((u64)tcw.full << 32) | ((PAL_RAM_CTRL & 3) << 6) | ((tsp.FilterMode != 0) << 8) | (1 << 9);
		else
			key |= (u64)(tcw.full & TCWTextureCacheMask.full) << 32;

		TexCacheIter it = cache.find(key);

		Texture* texture;
		if (it != cache.end())
		{
			texture = &it->second;
			// Needed if the texture is updated
			texture->tcw.StrideSel = tcw.StrideSel;
		}
		else //create if not existing
		{
			texture = &cache[key];

			texture->tsp = tsp;
			texture->tcw = tcw;
			texture->area = (u8)area;
		}

		return texture;
	}

   virtual ~BaseTextureCache() {}

	void CollectCleanup()
	{
		std::vector<u64> list;

		u32 TargetFrame = std::max((u32)120, FrameCount) - 120;

		for (const auto& pair : cache)
		{
			if (pair.second.dirty && pair.second.dirty < TargetFrame)
				list.push_back(pair.first);

			if (list.size() > 5)
				break;
		}

		for (u64 id : list)
		{
			if (cache[id].Delete())
				cache.erase(id);
		}
	}

	void Clear()
	{
		for (auto& pair : cache)
			pair.second.Delete();

		cache.clear();
		KillTex = false;
		INFO_LOG(RENDERER, "Texture cache cleared");
	}

protected:
	std::unordered_map<u64, Texture> cache;
	// Only use TexU and TexV from TSP in the cache key
	//     TexV : 7, TexU : 7
	const TSP TSPTextureCacheMask = { { 7, 7 } };
	//     TexAddr : 0x1FFFFF, Reserved : 0, StrideSel : 0, ScanOrder : 1, PixelFmt : 7, VQ_Comp : 1, MipMapped : 1
	const TCW TCWTextureCacheMask = { { 0x1FFFFF, 0, 0, 1, 7, 1, 1 } };
};

void ReadFramebuffer(PixelBuffer<u32>& pb, int& width, int& height);
void WriteTextureToVRam(u32 width, u32 height, u8 *data, u16 *dst);

static inline void MakeFogTexture(u8 *tex_data)
{
	u8 *fog_table = (u8 *)FOG_TABLE;
	for (int i = 0; i < 128; i++)
	{
		tex_data[i] = fog_table[i * 4];
		tex_data[i + 128] = fog_table[i * 4 + 1];
	}
}
extern const std::array<f32, 16> D_Adjust_LoD_Bias;
#undef clamp
