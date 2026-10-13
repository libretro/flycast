/* Telling when a game is only waiting.
 *
 * Under the accurate SH4 timing a game that waits for the next frame in a
 * loop goes round it thousands of times a frame, and a loop of several
 * blocks - one that calls a function on every pass, as Soul Calibur's does -
 * is not one the decoder can see for what it is. This sees it by watching.
 *
 * Where a block ends by going back, to a lower address or its own, there
 * is a "site". Now and then a site takes a copy of all the SH4's registers.
 * If the next time round, within the same time slice, they are all the same
 * again, and no block that can change anything but registers has run in
 * between, then the processor is exactly where it was: it will do the same
 * again, and again, until something from outside changes what it reads. In
 * this emulator that only happens between time slices - interrupts are
 * taken there, devices are run there, and the clock and timers a program
 * can read stand still within one. So the passes that would still have
 * been made in the slice are taken off its cycles at once, all but the
 * last, which is run: every pass costs what the one between the two looks
 * did, and the slice ends where, and with what left over, it would have
 * ended had the loop gone round in full. The game cannot tell, and neither
 * can a save state: what a site has seen is the host's, and is lost on a
 * load, and whether it skips or not, the machine comes out the same.
 *
 * "Can change anything but registers" is a block with a store, a store
 * queue flush or an instruction the interpreter runs in it: such a block
 * moves sh4_write_gen on as it starts.
 *
 * A loop that is getting somewhere fails the comparison, or moves the
 * generation, and its site then looks less and less often, up to once in
 * 1024 passes, so that an ordinary loop pays two instructions a pass.
 *
 * The recompilers emit the code: a count kept down at each site, a call to
 * sh4_wait_check() when it runs out, and the move of sh4_write_gen at the
 * top of a block that stores. Only under the accurate SH4 timing, and
 * only with the MMU off. */
#pragma once
#include "types.h"
#include "hw/sh4/sh4_if.h"
#include "blockmanager.h"

struct WaitSite
{
	s32 skip;			// passes left before the next look; counted down by the block
	u32 backoff;		// how many to skip after a look that found the loop busy
	u32 gen;			// sh4_write_gen when the copy was taken
	bool valid;
	u64 slice;			// the time slice the copy was taken in
	s32 cycles;			// the cycle counter when the copy was taken
	u8 regs[offsetof(Sh4Context, CpuRunning)];	// everything from the first register to FPSCR and its shadow
};

// Moved on by every block that can change something other than a register
extern u32 sh4_write_gen;

// A site for a block being compiled. NULL when there are no more, and for
// a block whose site could never find the processor waiting: the block then
// goes unwatched, and pays nothing.
WaitSite *sh4_wait_site(const RuntimeBlockInfo *block);

// Every block is gone, and with them whatever had a site's address
void sh4_wait_sites_reset();

// Whether a block can change anything but registers
bool sh4_block_writes(const RuntimeBlockInfo *block);

extern "C" {
// Called by a block going back, when its site's count has run out, with
// the cycle counter as the block has it there. Returns the counter to go on
// with: less by whole passes if the processor is going round in a loop
// that cannot end before the time slice does, else as it was.
s32 sh4_wait_check(WaitSite *site, s32 cycles);
}
