/* See rtt_watch.h. */
#include "rtt_watch.h"
#include "hw/mem/_vmem.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/sh4/modules/mmu.h"
#include <cstdlib>
#include <cstring>
#if !defined(TARGET_NO_THREADS)
#include <rthreads/rthreads.h>
#include "libretro/emu_baton.h"
extern EmuBaton emu_baton;
#endif

#define RTT_WATCH_SLOTS 16

static RttWatch slots[RTT_WATCH_SLOTS];
static RttWatchBackend backend;
static u8 *pixels;               /* kept: the fault handler does not allocate */
static size_t pixels_size;
static uintptr_t garbage[RTT_WATCH_SLOTS];
static unsigned garbage_count;
int rtt_watch_request;
static u32 request_offset, request_size;
#if !defined(TARGET_NO_THREADS)
static uintptr_t render_thread, emu_thread_id;
#endif

static inline u32 first_page(const RttWatch& s) { return s.addr / PAGE_SIZE; }
static inline u32 last_page(const RttWatch& s) { return (s.addr + s.bytes - 1) / PAGE_SIZE; }

static bool page_wanted(u32 page, const RttWatch *but)
{
	for (const RttWatch& s : slots)
		if (s.used && &s != but && page >= first_page(s) && page <= last_page(s))
			return true;
	return false;
}

/* The slot is done with: its pages go back to what they were, except
 * where another slot has them too. */
static void close_slot(RttWatch& s, bool opened)
{
	const u32 last = last_page(s);

	s.used = false;
	/* runs of pages that get the same, in one call each: a page costs a
	 * system call for every mapping of it */
	for (u32 page = first_page(s); page <= last; )
	{
		const bool wanted = page_wanted(page, &s);
		u32 end = page + 1;

		while (end <= last && page_wanted(end, &s) == wanted)
			end++;
		if (!wanted)
			_vmem_unwatch_vram(page * PAGE_SIZE, (end - page) * PAGE_SIZE);
		else if (opened)
			_vmem_watch_vram(page * PAGE_SIZE, (end - page) * PAGE_SIZE);
		page = end;
	}
}

static void release_garbage(void)
{
	while (garbage_count != 0)
		backend.release(garbage[--garbage_count]);
}

/* As the console packs a pixel into the framebuffer it renders to. */
static void pack(const RttWatch& s, const u8 *p, u16 *dst)
{
	const u32 skip = (s.stride - s.w * 2) / 2;

	for (u32 l = 0; l < s.h; l++, dst += skip)
	{
		switch (s.packmode)
		{
		case 0:
			for (u32 c = 0; c < s.w; c++, p += 4)
				*dst++ = (u16)(((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3) | s.kval_bit);
			break;
		case 1:
			for (u32 c = 0; c < s.w; c++, p += 4)
				*dst++ = (u16)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
			break;
		case 2:
			for (u32 c = 0; c < s.w; c++, p += 4)
				*dst++ = (u16)(((p[0] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[2] >> 4) | ((p[3] >> 4) << 12));
			break;
		default:
			for (u32 c = 0; c < s.w; c++, p += 4)
				*dst++ = (u16)(((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3)
						| (p[3] >= s.alpha_threshold ? 0x8000 : 0));
			break;
		}
	}
}

/* Render thread, and the machine is not running: the picture goes into
 * video memory. */
static void fill(RttWatch& s)
{
	backend.read(&s, pixels);
	_vmem_open_watched_vram(first_page(s) * PAGE_SIZE, (last_page(s) - first_page(s) + 1) * PAGE_SIZE);
	pack(s, pixels, (u16 *)&vram[s.addr]);
	close_slot(s, true);
	if (s.owned)
		backend.release(s.tex);
}

static void drop(RttWatch& s)
{
	close_slot(s, false);
	if (s.owned)
		garbage[garbage_count++] = s.tex;
}

static void fill_range(u32 offset, u32 size)
{
	const u32 first = offset / PAGE_SIZE, last = (offset + size - 1) / PAGE_SIZE;

	for (RttWatch& s : slots)
		if (s.used && first_page(s) <= last && last_page(s) >= first)
			fill(s);
	/* never leave a page that faults with nothing behind it */
	for (u32 page = first; page <= last; page++)
		if (_vmem_vram_page_watched(page) && !page_wanted(page, NULL))
			_vmem_unwatch_vram(page * PAGE_SIZE, PAGE_SIZE);
}

static void drop_range(u32 offset, u32 size)
{
	const u32 first = offset / PAGE_SIZE, last = (offset + size - 1) / PAGE_SIZE;

	for (RttWatch& s : slots)
		if (s.used && first_page(s) <= last && last_page(s) >= first)
			drop(s);
	for (u32 page = first; page <= last; page++)
		if (_vmem_vram_page_watched(page) && !page_wanted(page, NULL))
			_vmem_unwatch_vram(page * PAGE_SIZE, PAGE_SIZE);
}

void rtt_watch_supersede(u32 addr, u32 bytes)
{
	for (RttWatch& s : slots)
	{
		if (!s.used || s.addr >= addr + bytes || s.addr + s.bytes <= addr)
			continue;
		if (s.addr >= addr && s.addr + s.bytes <= addr + bytes)
		{
			/* Its pages stay as they are: the render that covers it is
			 * about to have them watched, and a game that renders to the
			 * same texture every frame pays for no system call here. (Were
			 * that not to follow, the first touch finds nothing waiting
			 * and opens them.) */
			s.used = false;
			if (s.owned)
				backend.release(s.tex);
		}
		else
			fill(s);
	}
}

void rtt_watch_add(const RttWatch *watch, const RttWatchBackend *b)
{
	RttWatch *slot = NULL;
	const size_t need = (size_t)watch->w * watch->h * 4;

	backend = *b;
#if !defined(TARGET_NO_THREADS)
	render_thread = sthread_get_current_thread_id();
#endif
	release_garbage();
	if (need > pixels_size)
	{
		free(pixels);
		pixels = (u8 *)malloc(need);
		pixels_size = need;
	}

#if defined(TARGET_NO_EXCEPTIONS)
	const bool can_watch = false;
#else
	/* With the MMU on, the game's own mappings of video memory come and go
	 * outside the ones closed here. */
	const bool can_watch = !mmu_enabled();
#endif
	if (!can_watch || watch->bytes == 0 || watch->addr + watch->bytes > VRAM_SIZE)
	{
		/* into video memory now, as the console has it */
		if (watch->bytes != 0 && watch->addr + watch->bytes <= VRAM_SIZE)
		{
			drop_range(watch->addr, watch->bytes);
			backend.read(watch, pixels);
			pack(*watch, pixels, (u16 *)&vram[watch->addr]);
		}
		if (watch->owned)
			backend.release(watch->tex);
		return;
	}

	for (RttWatch& s : slots)
		if (!s.used)
		{
			slot = &s;
			break;
		}
	if (slot == NULL)
	{
		slot = &slots[0];
		fill(*slot);
	}
	*slot = *watch;
	slot->used = true;
	slot->age = 0;
	_vmem_watch_vram(first_page(*slot) * PAGE_SIZE, (last_page(*slot) - first_page(*slot) + 1) * PAGE_SIZE);
}

bool rtt_watch_take(uintptr_t tex)
{
	for (RttWatch& s : slots)
		if (s.used && s.tex == tex && !s.owned)
		{
			s.owned = true;
			return true;
		}
	return false;
}

void rtt_watch_touch(u32 offset, u32 size)
{
	bool any = false;

	if (offset >= VRAM_SIZE)
		return;
	for (u32 page = offset / PAGE_SIZE; page <= (offset + size - 1) / PAGE_SIZE && !any; page++)
		any = _vmem_vram_page_watched(page);
	if (!any)
		return;

#if !defined(TARGET_NO_THREADS)
	const uintptr_t self = sthread_get_current_thread_id();
	if (self != render_thread)
	{
		if (self == emu_thread_id && emu_baton.FrameRunning())
		{
			/* The thread that renders is in retro_run, taking what this one
			 * hands over: it fetches the picture while this one sleeps. */
			request_offset = offset;
			request_size = size;
			emu_baton.WaitWorkDone();
			emu_baton.HandOver(&rtt_watch_request);
			return;
		}
		/* Some other thread, with the machine standing still and nobody
		 * rendering (the frontend saving a state from a thread of its own):
		 * the graphics card cannot be asked from here. The picture stays
		 * where it is drawn from; video memory does not get it. */
		drop_range(offset, size);
		return;
	}
#endif
	fill_range(offset, size);
}

bool rtt_watch_fault(void *address)
{
	const u32 offset = _vmem_get_vram_offset(address);

	if (offset == (u32)-1 || offset >= VRAM_SIZE || !_vmem_vram_page_watched(offset / PAGE_SIZE))
		return false;
	rtt_watch_touch(offset & ~(u32)PAGE_MASK, PAGE_SIZE);
	return true;
}

void rtt_watch_serve(void)
{
	release_garbage();
	fill_range(request_offset, request_size);
}

void rtt_watch_flush(void)
{
	if (backend.read == NULL)
		return;
	for (RttWatch& s : slots)
		if (s.used)
			fill(s);
}

#define RTT_WATCH_SETTLED 3

void rtt_watch_frame(void)
{
	if (backend.read == NULL)
		return;
	for (RttWatch& s : slots)
		if (s.used && ++s.age >= RTT_WATCH_SETTLED)
			fill(s);
}

void rtt_watch_forget(void)
{
	for (RttWatch& s : slots)
		if (s.used)
			drop(s);
}

void rtt_watch_term(void)
{
	for (RttWatch& s : slots)
		if (s.used)
			drop(s);
	if (backend.release != NULL)
		release_garbage();
	garbage_count = 0;
}

void rtt_watch_emu_thread(void)
{
#if !defined(TARGET_NO_THREADS)
	emu_thread_id = sthread_get_current_thread_id();
#endif
}
