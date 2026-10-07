#pragma once
/* What a game renders to a texture is in video memory afterwards, on the
 * console. Here it is on the graphics card, where drawing with it costs
 * nothing and it can be finer than the console's; getting it back into
 * video memory means waiting for the card. Most games only ever draw with
 * it, so it is not got back until the game does something that needs it
 * there.
 *
 * The pages of video memory it would be in are closed to every access.
 * Whatever touches them first - the game reading or writing, the texture
 * cache making a texture of another shape from that memory, the framebuffer
 * being read for the screen, a state being saved - faults, and the fault is
 * answered by fetching the picture, writing it into video memory as the
 * console would have, and opening the pages again. From then on it is
 * ordinary video memory.
 *
 * Nothing depends on when the card is done: the game sees the same bytes at
 * the same point whichever way it goes, and the machine never runs while
 * the pages are being filled. The table below belongs to the thread that
 * renders; the emulation thread only looks at the page marks, and hands
 * what it touched over. */
#include "types.h"

struct RttWatch
{
	u32 addr;            /* where in video memory, in bytes */
	u32 bytes;           /* from its first pixel to its last */
	u32 w, h;            /* pixels, the console's */
	u32 stride;          /* bytes from one line to the next */
	u32 packmode;        /* FB_W_CTRL: 0 0555, 1 565, 2 4444, 3 1555 */
	u32 kval_bit;        /* the top bit of a 0555 pixel */
	u32 alpha_threshold; /* what makes the top bit of a 1555 one */
	u32 tex;             /* the renderer's name for the picture */
	u32 scale;           /* how many times finer than w by h it is */
	bool owned;          /* nothing else will delete tex */
	bool used;
};

struct RttWatchBackend
{
	/* The picture as w by h pixels of R, G, B, A bytes, first line first. */
	void (*read)(const RttWatch *watch, u8 *rgba);
	void (*release)(u32 tex);
};

/* Render thread, the machine standing still. A new render covers this
 * memory: what was waiting there is not wanted (or, where it is only
 * partly covered, goes to video memory first). */
void rtt_watch_supersede(u32 addr, u32 bytes);
/* Render thread, the machine standing still: the render is finished. */
void rtt_watch_add(const RttWatch *watch, const RttWatchBackend *backend);
/* The texture cache is deleting @tex: true if it is wanted here still,
 * and then it is this module's to delete. */
bool rtt_watch_take(u32 tex);
/* Fault handler, any thread: true if the fault was one of these pages. */
bool rtt_watch_fault(void *address);
/* The same for code that knows it is about to touch video memory. */
void rtt_watch_touch(u32 offset, u32 size);
/* Render thread: do what the emulation thread handed over. */
void rtt_watch_serve(void);
/* What the emulation thread hands over: its address. */
extern int rtt_watch_request;
/* The machine standing still, any thread: video memory is about to be
 * replaced whole (a state is loaded), nothing waiting is wanted. */
void rtt_watch_forget(void);
/* Render thread: the renderer is going, and its pictures with it. */
void rtt_watch_term(void);
/* The emulation thread says which one it is. */
void rtt_watch_emu_thread(void);
