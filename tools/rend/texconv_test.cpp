/* The texture converters, against upstream's.
 *
 * Every converter the texture cache has - each pixel format, twiddled,
 * compressed, planar, paletted, to 16 and to 32 bits - is given the same
 * bytes at five sizes, and a hash and the first four pixels of what comes
 * out are printed, one line each. texconv_expected.txt is what upstream
 * flycast's converters print for the same bytes; texconv_test.sh compares.
 *
 * Built with -DUPSTREAM in an upstream tree (and its texconv.cpp object)
 * this prints that file's contents, which is how it was made:
 *   (echo "# random"; ./a.out; echo "# ramp"; ./a.out ramp)
 *
 * The converters are pure functions of their input, so this is also what a
 * rewrite of them has to keep printing. */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#ifdef UPSTREAM
#include "rend/texconv.h"
#define T opengl::pvrTexInfo
#else
#include "rend/TexCache.h"
#endif

static u8 *buf;
static u32 rnd_state = 0x12345678;
static u32 rnd() { rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 17; rnd_state ^= rnd_state << 5; return rnd_state; }

template<typename P, typename F> static void run(const char *name, F fn, u32 w, u32 h)
{
	// upstream's VQ converters are handed the indices, this tree's the codebook in front of them
	u32 off = 0;
#ifdef UPSTREAM
	if (strstr(name, "VQ")) off = 2048;
#endif
	if (!fn) { printf("%-12s %4ux%-4u (none)\n", name, w, h); return; }
	PixelBuffer<P> pb;
	pb.init(w, h);
	memset(pb.data(), 0xEE, (size_t)w * h * sizeof(P));
	fn(&pb, buf + 2048 + off, w, h);
	const u8 *d = (const u8 *)pb.data();
	u32 hash = 2166136261u;
	for (size_t i = 0; i < (size_t)w * h * sizeof(P); i++) hash = (hash ^ d[i]) * 16777619u;
	printf("%-12s %4ux%-4u %08x  first", name, w, h, hash);
	for (int i = 0; i < 4; i++) { u32 v = 0; memcpy(&v, d + i * sizeof(P), sizeof(P)); printf(" %0*x", (int)sizeof(P) * 2, v); }
	printf("\n");
}

int main(int argc, char **argv)
{
	static const u32 sizes[][2] = { {8, 8}, {32, 32}, {64, 16}, {16, 64}, {256, 256} };
	buf = (u8 *)malloc(4 * 1024 * 1024);
	if (argc > 1 && !strcmp(argv[1], "ramp"))
		for (u32 i = 0; i < 4 * 1024 * 1024; i += 2) { buf[i] = (i / 2) & 0xFF; buf[i + 1] = ((i / 2) >> 8) & 0xFF; }
	else
		for (u32 i = 0; i < 4 * 1024 * 1024; i++) buf[i] = (u8)rnd();
	vq_codebook = buf;
	palette_index = 64;
	for (u32 i = 0; i < 1024; i++) { palette16_ram[i] = (i * 2654435761u) >> 16; palette32_ram[i] = i * 2246822519u; }
	for (auto& s : sizes)
	{
		u32 w = s[0], h = s[1];
#ifdef UPSTREAM
		run<u16>("1555_TW", T[0].TW, w, h);   run<u16>("1555_VQ", T[0].VQ, w, h);
		run<u16>("565_TW", T[1].TW, w, h);    run<u16>("565_VQ", T[1].VQ, w, h);
		run<u16>("4444_TW", T[2].TW, w, h);   run<u16>("4444_VQ", T[2].VQ, w, h);
		run<u16>("BMP_TW", T[4].TW, w, h);    run<u16>("BMP_VQ", T[4].VQ, w, h);
		run<u16>("PAL4_TW", T[5].TW, w, h);   run<u16>("PAL4_VQ", T[5].VQ, w, h);
		run<u16>("PAL8_TW", T[6].TW, w, h);   run<u16>("PAL8_VQ", T[6].VQ, w, h);
		run<u32>("1555_PL32", T[0].PL32, w, h); run<u32>("1555_TW32", T[0].TW32, w, h); run<u32>("1555_VQ32", T[0].VQ32, w, h);
		run<u32>("565_PL32", T[1].PL32, w, h);  run<u32>("565_TW32", T[1].TW32, w, h);  run<u32>("565_VQ32", T[1].VQ32, w, h);
		run<u32>("4444_PL32", T[2].PL32, w, h); run<u32>("4444_TW32", T[2].TW32, w, h); run<u32>("4444_VQ32", T[2].VQ32, w, h);
		run<u32>("YUV_PL", T[3].PL32, w, h);    run<u32>("YUV_TW", T[3].TW32, w, h);    run<u32>("YUV_VQ", T[3].VQ32, w, h);
		run<u32>("PAL4_TW32", T[5].TW32, w, h); run<u32>("PAL4_VQ32", T[5].VQ32, w, h);
		run<u32>("PAL8_TW32", T[6].TW32, w, h); run<u32>("PAL8_VQ32", T[6].VQ32, w, h);
		run<u8>("PAL4PT_TW", T[5].TW8, w, h);   run<u8>("PAL8PT_TW", T[6].TW8, w, h);
		run<u32>("1555_PLVQ32", T[0].PLVQ32, w, h); run<u32>("565_PLVQ32", T[1].PLVQ32, w, h);
		run<u32>("4444_PLVQ32", T[2].PLVQ32, w, h); run<u32>("YUV_PLVQ", T[3].PLVQ32, w, h);
#else
		run<u16>("1555_TW", &tex1555_TW, w, h);   run<u16>("1555_VQ", &tex1555_VQ, w, h);
		run<u16>("565_TW", &tex565_TW, w, h);     run<u16>("565_VQ", &tex565_VQ, w, h);
		run<u16>("4444_TW", &tex4444_TW, w, h);   run<u16>("4444_VQ", &tex4444_VQ, w, h);
		run<u16>("BMP_TW", &texBMP_TW, w, h);     run<u16>("BMP_VQ", &texBMP_VQ, w, h);
		run<u16>("PAL4_TW", &texPAL4_TW, w, h);   run<u16>("PAL4_VQ", &texPAL4_VQ, w, h);
		run<u16>("PAL8_TW", &texPAL8_TW, w, h);   run<u16>("PAL8_VQ", &texPAL8_VQ, w, h);
		run<u32>("1555_PL32", &tex1555_PL32, w, h); run<u32>("1555_TW32", &tex1555_TW32, w, h); run<u32>("1555_VQ32", &tex1555_VQ32, w, h);
		run<u32>("565_PL32", &tex565_PL32, w, h);   run<u32>("565_TW32", &tex565_TW32, w, h);   run<u32>("565_VQ32", &tex565_VQ32, w, h);
		run<u32>("4444_PL32", &tex4444_PL32, w, h); run<u32>("4444_TW32", &tex4444_TW32, w, h); run<u32>("4444_VQ32", &tex4444_VQ32, w, h);
		run<u32>("YUV_PL", &texYUV422_PL, w, h);    run<u32>("YUV_TW", &texYUV422_TW, w, h);    run<u32>("YUV_VQ", &texYUV422_VQ, w, h);
		run<u32>("PAL4_TW32", &texPAL4_TW32, w, h); run<u32>("PAL4_VQ32", &texPAL4_VQ32, w, h);
		run<u32>("PAL8_TW32", &texPAL8_TW32, w, h); run<u32>("PAL8_VQ32", &texPAL8_VQ32, w, h);
		run<u8>("PAL4PT_TW", &texPAL4PT_TW, w, h);  run<u8>("PAL8PT_TW", &texPAL8PT_TW, w, h);
		run<u32>("1555_PLVQ32", &tex1555_PLVQ32, w, h); run<u32>("565_PLVQ32", &tex565_PLVQ32, w, h);
		run<u32>("4444_PLVQ32", &tex4444_PLVQ32, w, h); run<u32>("YUV_PLVQ", &texYUV422_PLVQ, w, h);
#endif
	}
	return 0;
}
