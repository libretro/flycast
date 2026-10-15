/* Textures' images kept and handed out again, so that none is made while
 * a game runs.
 *
 * A texture used to make its image the first time it was used, and again
 * whenever its size or format changed - in the middle of a game, a scene's
 * worth of them in one frame when it loads, which is what the time spent
 * updating textures went on. Images are made when the renderer starts
 * instead, in the shapes the PowerVR's textures come in, and a texture
 * takes one of its shape from here and gives it back when it is done
 * with it.
 *
 * The PowerVR's textures are 8 to 1024 texels wide and high, in powers of
 * two, with all their mipmap levels or only the largest: with the
 * renderer's few formats, the shapes are few. An image's class is its
 * format, its width, its height and whether it has the levels; each class
 * has a list of the free images of that shape. How many of each are made
 * at the start is a share of the video memory the renderer may use
 * (rend/vram_budget.h), spread over the classes the way games' textures
 * are: mostly 16-bit, mipmapped and square, 64 to 256 texels; a quarter of
 * it to the rectangles, by fixed shares - the smaller the shape and the
 * nearer to square, the more. Pictures rendered to (a format of the kind
 * TARGETS) are a fixed few of the shapes renders come in, from 64x64 to
 * 1024x1024 (half as many where the share is the least). A class that has
 * none left when one is asked for has one made then - which means the
 * share was too small for the game - and it stays in the pool after: it is
 * counted, and said once for each class.
 *
 * The lists are threaded through the images' own entries, and nothing is
 * allocated to take an image or to give one back. Only the thread that
 * renders touches a pool. */
#ifndef REND_TEX_POOL_H
#define REND_TEX_POOL_H

#include "types.h"
#include "log/Log.h"
#include "rend/vram_budget.h"
#include <string.h>

/* What the pool knows of an image: the renderer's entry starts with this */
struct TexPoolEntry
{
	TexPoolEntry *next;
	u32 cls;
	u32 bytes;
};

class TexPoolBook
{
public:
	enum { SIZES = 8, MIN_LOG2 = 3, MAX_FORMATS = 12, MAX_FRAMES = 8 };
	/* What a format's images are for: textures; or pictures rendered to */
	enum { TEXTURES = 0, TARGETS = 1 };
	enum { CLASSES = MAX_FORMATS * SIZES * SIZES * 2 };

	/* The class of an image of @format (the renderer's own number for it,
	 * below MAX_FORMATS), @width by @height, with its mipmap levels or
	 * not; -1 if it is not a shape the pool has */
	static int Class(u32 format, u32 width, u32 height, bool mips)
	{
		const int lw = log2_exact(width), lh = log2_exact(height);
		if (format >= MAX_FORMATS || lw < 0 || lh < 0)
			return -1;
		// (a mipmapped texture of the PowerVR's is square)
		if (mips && lw != lh)
			return -1;
		return (int)(((format * SIZES + (u32)lw) * SIZES + (u32)lh) * 2 + (mips ? 1 : 0));
	}
	static u32 Format(int cls) { return (u32)cls / (SIZES * SIZES * 2); }
	static u32 Width(int cls) { return 8u << (((u32)cls / (SIZES * 2)) % SIZES); }
	static u32 Height(int cls) { return 8u << (((u32)cls / 2) % SIZES); }
	static bool Mips(int cls) { return (cls & 1) != 0; }
	/* The image's texels, all levels, for @texel bytes a texel */
	static u32 Bytes(int cls, u32 texel)
	{
		u32 w = Width(cls), h = Height(cls), bytes = w * h * texel;
		if (Mips(cls))
			for (w /= 2; w != 0; w /= 2)
				bytes += w * w * texel;
		return bytes;
	}

	/* A rectangle of the PowerVR's, 2^(@lw + 3) by 2^(@lh + 3) texels: its
	 * share of the rectangles of a format used as much as 565. Strips and
	 * large ones are few. */
	static u32 RectShare(u32 lw, u32 lh)
	{
		// by the shorter side, for one twice as long; half as many for each doubling more
		static const u32 shorter[SIZES] = { 2, 4, 6, 6, 4, 2, 1, 0 };
		const u32 d = lw > lh ? lw - lh : lh - lw;
		return d == 0 ? 0 : shorter[lw < lh ? lw : lh] >> (d - 1);
	}
	/* How many of each shape of picture rendered to there are: the square
	 * ones from 64 to 512, two each, and 1024x1024; and what renders of a
	 * screen are, 1024x512 (640x480 and the like) and 512x256 */
	static u32 TargetCount(u32 lw, u32 lh)
	{
		if (lw == lh)
			return lw >= 3 && lw <= 6 ? 2 : lw == 7 ? 1 : 0;
		if (lw == 7 && lh == 6)
			return 4;
		if (lw == 6 && lh == 5)
			return 2;
		return lw >= 3 && lh >= 3 && (lw == lh + 1 || lh == lw + 1) ? 1 : 0;
	}

	/* How many images of each class to make at the start (into @counts,
	 * CLASSES of them), for formats of @texel bytes a texel (0 for a
	 * format there is none of), of the kind @kind[f] (TEXTURES, TARGETS;
	 * all TEXTURES if null) and the given relative use (@use, in eighths:
	 * 8 for a format games use as much as 565). What the textures take is
	 * a sixteenth of what the budget has left, but no less than 16 MB and
	 * no more than 128 MB: three quarters of it the square ones, a quarter
	 * the rectangles. Returns the bytes, the targets' too. */
	static u64 Plan(const VramBudget& budget, const u32 *texel, const u32 *use, const u8 *kind, u32 formats, u32 *counts)
	{
		/* For a format used as much as 565: square, mipmapped, then not;
		 * from 8 texels to 1024. */
		static const u32 mipped[SIZES] = { 1, 4, 6, 12, 16, 16, 4, 0 };
		static const u32 plain[SIZES]  = { 0, 0, 1,  2,  4,  4, 2, 0 };
		u64 unit = 0, rectUnit = 0, want, bytes = 0;
		u32 f, s, t;

		memset(counts, 0, sizeof(u32) * CLASSES);
		for (f = 0; f < formats && f < MAX_FORMATS; f++)
		{
			if (texel[f] == 0 || (kind != nullptr && kind[f] != TEXTURES))
				continue;
			for (s = 0; s < SIZES; s++)
			{
				unit += (u64)mipped[s] * use[f] * Bytes(Class(f, 8u << s, 8u << s, true), texel[f]);
				unit += (u64)plain[s] * use[f] * Bytes(Class(f, 8u << s, 8u << s, false), texel[f]);
				for (t = 0; t < SIZES; t++)
					rectUnit += (u64)RectShare(s, t) * use[f] * Bytes(Class(f, 8u << s, 8u << t, false), texel[f]);
			}
		}
		want = (u64)budget.available_kib() * 1024 / 16;
		if (want < 16ull << 20)
			want = 16ull << 20;
		if (want > 128ull << 20)
			want = 128ull << 20;
		// the tables above times their part of want / unit, rounded up (the units are in eighths)
		const u64 squares = want * 8 * 3 / 4, rects = want * 8 / 4;
		for (f = 0; f < formats && f < MAX_FORMATS; f++)
		{
			if (texel[f] == 0)
				continue;
			if (kind != nullptr && kind[f] == TARGETS)
			{
				for (s = 0; s < SIZES; s++)
					for (t = 0; t < SIZES; t++)
					{
						const int c = Class(f, 8u << s, 8u << t, false);
						// (half as many on a card with little memory)
						counts[c] = want < 64ull << 20 ? (TargetCount(s, t) + 1) / 2 : TargetCount(s, t);
						bytes += (u64)counts[c] * Bytes(c, texel[f]);
					}
				continue;
			}
			for (s = 0; s < SIZES && unit != 0; s++)
			{
				const int m = Class(f, 8u << s, 8u << s, true), p = Class(f, 8u << s, 8u << s, false);
				counts[m] = (u32)(((u64)mipped[s] * use[f] * squares + unit * 8 - 1) / (unit * 8));
				counts[p] = (u32)(((u64)plain[s] * use[f] * squares + unit * 8 - 1) / (unit * 8));
				bytes += (u64)counts[m] * Bytes(m, texel[f]) + (u64)counts[p] * Bytes(p, texel[f]);
				for (t = 0; t < SIZES && rectUnit != 0; t++)
				{
					if (t == s)
						continue;
					const int r = Class(f, 8u << s, 8u << t, false);
					// (to the nearest: there are many of them, and few of each are used)
					counts[r] = (u32)(((u64)RectShare(s, t) * use[f] * rects + rectUnit * 4) / (rectUnit * 8));
					bytes += (u64)counts[r] * Bytes(r, texel[f]);
				}
			}
		}
		return bytes;
	}

	void Reset(u32 frames)
	{
		memset(free, 0, sizeof(free));
		memset(retired, 0, sizeof(retired));
		memset(said, 0, sizeof(said));
		this->frames = frames == 0 ? 1 : frames > MAX_FRAMES ? MAX_FRAMES : frames;
		current = 0;
		made = madeLate = lateClasses = 0;
		bytesMade = bytesLate = 0;
	}

	/* A free image of class @cls, or null: the class has none left, and the
	 * renderer makes one (Made() with @late) */
	TexPoolEntry *Take(int cls)
	{
		TexPoolEntry *e = free[cls];
		if (e != nullptr)
			free[cls] = e->next;
		return e;
	}
	/* An image made: at the start, or because its class had none left */
	void Made(TexPoolEntry *e, int cls, u32 bytes, bool late, const char *format)
	{
		e->next = nullptr;
		e->cls = (u32)cls;
		e->bytes = bytes;
		made++;
		bytesMade += bytes;
		if (!late)
			return;
		madeLate++;
		bytesLate += bytes;
		if (!said[cls])
		{
			said[cls] = true;
			lateClasses++;
			WARN_LOG(RENDERER, "Texture pool: no %ux%u %s%s image left, one made (the pool is too small for this game)",
					Width(cls), Height(cls), format, Mips(cls) ? " mipmapped" : "");
		}
	}
	/* Free again now: nothing the GPU has still to do uses it */
	void Give(TexPoolEntry *e)
	{
		e->next = free[e->cls];
		free[e->cls] = e;
	}
	/* Free again once the frames that may still use it are done: when this
	 * frame's place comes round again (Frame()) */
	void Retire(TexPoolEntry *e)
	{
		e->next = retired[current];
		retired[current] = e;
	}
	/* @index is the current frame's from here on: what was retired under
	 * it the last time is free */
	void Frame(u32 index)
	{
		current = index % frames;
		TexPoolEntry *e = retired[current];
		retired[current] = nullptr;
		while (e != nullptr)
		{
			TexPoolEntry *next = e->next;
			Give(e);
			e = next;
		}
	}
	/* Everything there is, free, retired or not, for the renderer to
	 * destroy: what is free and retired is handed to @fn here; what
	 * textures hold they give back first */
	template<typename Fn>
	void Drain(Fn fn)
	{
		for (u32 i = 0; i < MAX_FRAMES; i++)
			Frame(i);
		for (u32 c = 0; c < CLASSES; c++)
			while (free[c] != nullptr)
			{
				TexPoolEntry *e = free[c];
				free[c] = e->next;
				fn(e);
			}
	}

	void Summary(const char *api) const
	{
		NOTICE_LOG(RENDERER, "%s: texture pool, %u images made (%u MB), %u of them while the game ran (%u MB, %u shapes)",
				api, made, (u32)(bytesMade >> 20), madeLate, (u32)(bytesLate >> 20), lateClasses);
	}

	// what has been done so far, for measuring
	u32 made = 0, madeLate = 0, lateClasses = 0;
	u64 bytesMade = 0, bytesLate = 0;

private:
	static int log2_exact(u32 v)
	{
		int l;
		if (v < (8u << 0) || v > (8u << (SIZES - 1)) || (v & (v - 1)) != 0)
			return -1;
		for (l = 0; (8u << l) != v; l++)
			;
		return l;
	}

	TexPoolEntry *free[CLASSES];
	TexPoolEntry *retired[MAX_FRAMES];
	bool said[CLASSES];
	u32 frames = 1;
	u32 current = 0;
};

#endif
