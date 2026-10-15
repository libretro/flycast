/* The PowerVR's textures made from their bytes by the Direct3D 9
 * renderer's pixel shader (shader model 3): what rend/texdecode.h's
 * texd_texel() makes, to the bit, for the renderer's dx9/dx9_texdecoder.h.
 *
 * Shader model 3 has no integer operations: its integers are floats, and
 * there is no shift and no and. So this is texd_texel() again, written in
 * float arithmetic that is exact: every value is an integer below 2^24,
 * which a 32-bit float holds as it is; a shift is a multiplication or a
 * floor() of one by a power of two, which is exact, and every power of two
 * is a constant here, never something the shader has to work out. 32-bit
 * texels are never put together into one number (it would not fit): they
 * are four bytes, red first. tools/rend/texdecode_test.sh holds this text
 * to the C reference on a Direct3D 9 device (tools/rend/texdecode_d3d9.c),
 * texture for texture.
 *
 * The bytes of a texture - the same as the other renderers copy, the plan
 * of rend/texdecode.h - are in a ring texture of A8R8G8B8 texels, 1024 a
 * row: a texel is four bytes of the ring, the lowest in blue. A level is
 * drawn into the level of the texture, which is in the format the texture
 * is sampled in: R5G6B5, A1R5G5B5, A4R4G4B4 (the colour is each field over
 * its largest value, which the target turns back into the field), or
 * A8R8G8B8, or A8 for palette indices.
 *
 * Compiled with d3dcompiler_47 (ps_3_0, vs_3_0) into rend/texdecode_dx9.inc
 * by tools/rend/kernels_dxbc.sh: TEXD_SM3_KERNEL is 1 for the vertex
 * shader, 2 for the pixel shader. */
#ifndef REND_TEXDECODE_SM3_H
#define REND_TEXDECODE_SM3_H

/* What a level is drawn with: four constants from c32, as the renderer sets
 * them (DX9TexDecoder) */
#define TEXD_SM3_CONSTANT 32
/* The ring's texels a row */
#define TEXD_SM3_RING_WIDTH 1024
/* What is drawn into */
#define TEXD_SM3_565   0
#define TEXD_SM3_5551  1
#define TEXD_SM3_4444  2
#define TEXD_SM3_8888  3
#define TEXD_SM3_8     4

#if defined(TEXD_SM3_KERNEL) && TEXD_SM3_KERNEL == 1

float4 main(float4 pos : POSITION) : POSITION
{
	return pos;
}

#elif defined(TEXD_SM3_KERNEL) && TEXD_SM3_KERNEL == 2

sampler2D texd_ring : register(s0);
float4 texd_c0 : register(c32);	// base (in words), data, pal, 1 / the ring's rows
float4 texd_c1 : register(c33);	// width, height, stride, rows (of a planar texture)
float4 texd_c2 : register(c34);	// format, layout, what it is made into, corner
float4 texd_c3 : register(c35);	// log2 of the width and height laid out by, target, 0

/* (v >> log2(div)) & (mod - 1), @div and @mod powers of two */
float texd_f(float v, float div, float mod)
{
	float t = floor(v * (1.0 / div));
	return t - floor(t * (1.0 / mod)) * mod;
}

/* Word @w of the texture's bytes: its four bytes, the lowest first */
float4 texd_word(float w)
{
	w += texd_c0.x;
	float y = floor(w * (1.0 / TEXD_SM3_RING_WIDTH));
	float x = w - y * TEXD_SM3_RING_WIDTH;
	float4 c = tex2Dlod(texd_ring, float4((x + 0.5) * (1.0 / TEXD_SM3_RING_WIDTH), (y + 0.5) * texd_c0.w, 0, 0));
	c = floor(c * 255.0 + 0.5);
	return float4(c.b, c.g, c.r, c.a);
}

float texd_byte(float at)
{
	float w = floor(at * 0.25);
	float k = at - w * 4;
	float4 b = texd_word(w);
	return k < 2 ? (k < 1 ? b.x : b.y) : (k < 3 ? b.z : b.w);
}

/* (@at even) */
float texd_half(float at)
{
	float w = floor(at * 0.25);
	float4 b = texd_word(w);
	return at - w * 4 < 1 ? b.x + b.y * 256 : b.z + b.w * 256;
}

/* texd_twiddle() */
float texd_twiddle(float x, float y, float lw, float lh)
{
	float n = min(lw, lh);
	float at = 0, m = 1;
	[unroll] for (int i = 0; i < 10; i++)
	{
		if ((float)i < n)
		{
			float hx = floor(x * 0.5), hy = floor(y * 0.5);
			at += (y - hy * 2) * m + (x - hx * 2) * m * 2;
			x = hx;
			y = hy;
			m *= 4;
		}
	}
	return at + (lw > lh ? x : y) * m;
}

/* texd_div0() by 8, 32 and 64 */
float texd_div0(float a, float inv)
{
	return a >= 0 ? floor(a * inv) : -floor(-a * inv);
}

/* YUV422(), as bytes */
float4 texd_yuv(float y, float u, float v)
{
	u -= 128;
	v -= 128;
	return float4(clamp(y + texd_div0(v * 11, 1.0 / 8), 0, 255),
			clamp(y - texd_div0(u * 11 + v * 22, 1.0 / 32), 0, 255),
			clamp(y + texd_div0(u * 110, 1.0 / 64), 0, 255), 255);
}

/* texd_rgb(): the 16 bits as the converters make them in x, or the four
 * bytes of the 32 */
float4 texd_rgb(float fmt, float otype, float c)
{
	if (otype == 1)
	{
		if (fmt == 1)
			return float4(c, 0, 0, 0);
		if (fmt == 0)
			return float4(texd_f(c, 32768, 2) + texd_f(c, 1024, 32) * 2048 + texd_f(c, 32, 32) * 64 + texd_f(c, 1, 32) * 2, 0, 0, 0);
		return float4(texd_f(c, 1, 16) * 16 + texd_f(c, 16, 16) * 256 + texd_f(c, 256, 16) * 4096 + texd_f(c, 4096, 16), 0, 0, 0);
	}
	if (fmt == 1)
		return float4(texd_f(c, 2048, 32) * 8 + texd_f(c, 8192, 8),
				texd_f(c, 32, 64) * 4 + texd_f(c, 512, 4),
				texd_f(c, 1, 32) * 8 + texd_f(c, 4, 8), 255);
	if (fmt == 0)
		return float4(texd_f(c, 1024, 32) * 8 + texd_f(c, 4096, 8),
				texd_f(c, 32, 32) * 8 + texd_f(c, 128, 8),
				texd_f(c, 1, 32) * 8 + texd_f(c, 4, 8), texd_f(c, 32768, 2) * 255);
	return float4(texd_f(c, 256, 16) * 17, texd_f(c, 16, 16) * 17, texd_f(c, 1, 16) * 17, texd_f(c, 4096, 16) * 17);
}

/* texd_pal() */
float4 texd_pal(float otype, float pal, float index)
{
	if (otype == 0)
		return float4(index, 0, 0, 0);
	if (otype == 1)
		return float4(texd_half(pal + index * 4), 0, 0, 0);
	return texd_word(pal * 0.25 + index);
}

/* texd_texel() */
float4 texd_texel(float x, float y)
{
	float data = texd_c0.y, pal = texd_c0.z;
	float stride = texd_c1.z, rows = texd_c1.w;
	float fmt = texd_c2.x, scan = texd_c2.y, otype = texd_c2.z;
	float e, k;

	if (fmt == 4)
		fmt = 2;
	if (texd_c2.w != 0)
	{
		x = 1;
		y = 1;
	}
	if (scan >= 2)
	{
		if (x >= stride || y >= rows)
			return float4(0, 0, 0, 0);
		if (scan == 2)
		{
			if (fmt == 3)
			{
				float hx = floor(x * 0.5);
				e = data + 2 * (y * stride) + 4 * hx;
				return texd_yuv(texd_byte(e + 1 + 2 * (x - hx * 2)), texd_byte(e), texd_byte(e + 2));
			}
			return texd_rgb(fmt, otype, texd_half(data + 2 * (y * stride + x)));
		}
		e = texd_byte(data + y * floor(stride * 0.25) + floor(x * 0.25)) * 8;
		k = x - floor(x * 0.25) * 4;
		if (fmt == 3)
		{
			float hk = floor(k * 0.5);
			e += 4 * hk;
			return texd_yuv(floor(texd_half(e + 2 * (k - hk * 2)) * (1.0 / 256)), texd_f(texd_half(e), 1, 256), texd_f(texd_half(e + 2), 1, 256));
		}
		return texd_rgb(fmt, otype, texd_half(e + 2 * k));
	}

	float t = texd_twiddle(x, y, texd_c3.x, texd_c3.y);
	if (scan == 1)
	{
		/* an index for every 16, 8 or 4 texels */
		float div = fmt == 5 ? 16 : fmt == 6 ? 8 : 4;
		float q = floor(t * (fmt == 5 ? 1.0 / 16 : fmt == 6 ? 1.0 / 8 : 1.0 / 4));
		e = texd_byte(data + q) * 8;	// the codebook is at the start
		k = t - q * div;
	}
	else
	{
		e = data;
		k = t;
	}
	if (fmt == 5)
	{
		float hk = floor(k * 0.5);
		float b = texd_byte(e + hk);
		return texd_pal(otype, pal, k - hk * 2 != 0 ? floor(b * (1.0 / 16)) : texd_f(b, 1, 16));
	}
	if (fmt == 6)
		return texd_pal(otype, pal, texd_byte(e + k));
	if (fmt == 3)
	{
		float by = texd_f(k, 1, 2);
		e += 8 * floor(k * 0.25);
		k = texd_f(k, 1, 4);
		return texd_yuv(floor(texd_half(e + 2 * k) * (1.0 / 256)), texd_f(texd_half(e + 2 * by), 1, 256), texd_f(texd_half(e + 4 + 2 * by), 1, 256));
	}
	return texd_rgb(fmt, otype, texd_half(e + 2 * k));
}

/* The texel at the pixel, in what is drawn into */
float4 main(float2 vpos : VPOS) : COLOR
{
	float4 v = texd_texel(floor(vpos.x), floor(vpos.y));
	float target = texd_c3.z;
	if (target == TEXD_SM3_8888)
		return v * (1.0 / 255);
	if (target == TEXD_SM3_8)
		return float4(0, 0, 0, v.x * (1.0 / 255));
	float c = v.x;
	if (target == TEXD_SM3_565)
		return float4(floor(c * (1.0 / 2048)) * (1.0 / 31), texd_f(c, 32, 64) * (1.0 / 63), texd_f(c, 1, 32) * (1.0 / 31), 1);
	if (target == TEXD_SM3_5551)
		return float4(floor(c * (1.0 / 2048)) * (1.0 / 31), texd_f(c, 64, 32) * (1.0 / 31), texd_f(c, 2, 32) * (1.0 / 31), texd_f(c, 1, 2));
	return float4(floor(c * (1.0 / 4096)) * (1.0 / 15), texd_f(c, 256, 16) * (1.0 / 15), texd_f(c, 16, 16) * (1.0 / 15), texd_f(c, 1, 16) * (1.0 / 15));
}

#endif

#endif
