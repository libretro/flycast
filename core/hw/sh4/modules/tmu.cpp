/*
	Lovely timers, its amazing how many times this module was bugged
*/

#include "types.h"
#include "../sh4_sched.h"
#include "tmu.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_mmr.h"


#define tmu_underflow 0x0100
#define tmu_UNIE      0x0020
#define tmu_ICPF      0x0200

u32 tmu_shift[3];
u32 tmu_mask[3];
u64 tmu_mask64[3];

u32 old_mode[3] = {0xFFFF,0xFFFF,0xFFFF};

static const InterruptID tmu_intID[3]={sh4_TMU0_TUNI0,sh4_TMU1_TUNI1,sh4_TMU2_TUNI2};
int tmu_sched[3];

#if 0
const u32 tmu_ch_bit[3]={1,2,4};

u32 tmu_prescaler[3];
u32 tmu_prescaler_shift[3];
u32 tmu_prescaler_mask[3];

//Accurate counts for the channel ch
template<u32 ch>
void UpdateTMU_chan(u32 clc)
{
	//if channel is on
	//if ((TMU_TSTR & tmu_ch_bit[ch])!=0)
	//{
		//count :D
		tmu_prescaler[ch]+=clc;
		u32 steps=tmu_prescaler[ch]>>tmu_prescaler_shift[ch];
		
		//remove the full steps from the prescaler counter
		tmu_prescaler[ch]&=tmu_prescaler_mask[ch];

		if (unlikely(steps>TMU_TCNT(ch)))
		{
			//remove the 'extra' steps to overflow
			steps-=TMU_TCNT(ch);
			//refill the counter
			TMU_TCNT(ch) = TMU_TCOR(ch);
			//raise the interrupt
			TMU_TCR(ch) |= tmu_underflow;
			InterruptPend(tmu_intID[ch],1);
			
			//remove the full underflows (possible because we only check every 448 cycles)
			//this can be done with a div, but its very very very rare so this is probably faster
			//THIS can probably be replaced with a verify check on counter setup (haven't seen any game do this)
			while(steps>TMU_TCOR(ch))
				steps-=TMU_TCOR(ch);

			//steps now has the partial steps needed for update, guaranteed it won't cause an overflow
		}
		//count down
		TMU_TCNT(ch)-=steps;
	//}
}

template<u32 chans>
void UpdateTMU_i(u32 Cycles)
{
	if (chans & 1) UpdateTMU_chan<0>(Cycles);
	if (chans & 2) UpdateTMU_chan<1>(Cycles);
	if (chans & 4) UpdateTMU_chan<2>(Cycles);
}
#endif

u32 tmu_ch_base[3];
u64 tmu_ch_base64[3];

/* The count as it stands, in 64 bits: below 0 is a count that has gone
 * past 0 and has not been loaded again yet. */
static s64 read_TMU_TCNTch64(u32 ch)
{
	return tmu_ch_base64[ch] - ((sh4_sched_now64() >> tmu_shift[ch])&tmu_mask64[ch]);
}

static void sched_chan_tick(int ch)
{
	if (!tmu_mask64[ch])
	{
		// stopped: nothing is to come
		sh4_sched_request(tmu_sched[ch], -1);
		return;
	}

	/* The counter underflows on the tick after the one that brings it to
	 * 0, and ticks fall where the clock is a multiple of the divider. The
	 * scheduler calls back at the first time slice that ends after the
	 * time asked for: one cycle before that tick is asked for, so that it
	 * is the first slice to end on the tick or after it. In 64 bits: a
	 * count of thousands of millions times a divider of thousands does not
	 * fit 32, and what was left of it there was any time at all. More
	 * than the scheduler takes is cut down to that, and the call that
	 * comes then asks again. */
	const u64 now = sh4_sched_now64();
	const s64 togo = (s64)((((now >> tmu_shift[ch]) + read_TMU_TCNTch64(ch) + 1) << tmu_shift[ch]) - 1 - now);

	sh4_sched_request(tmu_sched[ch], togo <= 0 ? 0 : togo > SH4_MAIN_CLOCK ? SH4_MAIN_CLOCK : (int)togo);
}

static void write_TMU_TCNTch(u32 ch, u32 data)
{
	tmu_ch_base64[ch] = data + ((sh4_sched_now64() >> tmu_shift[ch])&tmu_mask64[ch]);
	tmu_ch_base[ch] = (u32)tmu_ch_base64[ch];

	sched_chan_tick(ch);
}

/* The counter has gone below 0, to @past: the flag is set, the interrupt
 * asked for, and the counter is what it was loaded with again less what it
 * has counted since. It is loaded on the tick after it reads 0, so a count
 * of -1 is that tick and the counter is TCOR: its period is TCOR + 1. (It
 * used to be loaded with TCOR on reaching 0, a period of TCOR.) */
static u32 tmu_underflowed(u32 ch, s64 past)
{
	TMU_TCR(ch) |= tmu_underflow;
	InterruptPend(tmu_intID[ch], 1);

	s64 next = (s64)TMU_TCOR(ch) + 1 + past;

	if (next < 0)
	{
		// TCOR is so small that it has underflowed more than once in the time
		next = (s64)TMU_TCOR(ch) - (s64)((u64)(-1 - past) % ((u64)TMU_TCOR(ch) + 1));
	}
	write_TMU_TCNTch(ch, (u32)next);
	return (u32)next;
}

/* What the counter reads now. The scheduler's call comes before anything
 * can read a count that has underflowed, the clock moving only between
 * time slices; were one read all the same, it is put right here. */
static u32 read_TMU_TCNTch(u32 ch)
{
	const s64 count = read_TMU_TCNTch64(ch);

	if (unlikely(count < 0))
		return tmu_underflowed(ch, count);
	return (u32)count;
}

template<u32 ch>
u32 read_TMU_TCNT(u32 addr)
{
	return read_TMU_TCNTch(ch);
}

template<u32 ch>
void write_TMU_TCNT(u32 addr, u32 data)
{
	write_TMU_TCNTch(ch,data);
}

static void turn_on_off_ch(u32 ch, bool on)
{
	// nothing changes for this channel: its count and the call asked for stay
	if ((tmu_mask64[ch] != 0) == on)
		return;

	u32 TCNT=read_TMU_TCNTch(ch);
	tmu_mask[ch]=on?0xFFFFFFFF:0x00000000;
	tmu_mask64[ch] = on ? 0xFFFFFFFFFFFFFFFF : 0x0000000000000000;
	write_TMU_TCNTch(ch,TCNT);
}

//Update internal counter registers
static void UpdateTMUCounts(u32 reg)
{
	InterruptPend(tmu_intID[reg],TMU_TCR(reg) & tmu_underflow);
	InterruptMask(tmu_intID[reg],TMU_TCR(reg) & tmu_UNIE);

	if (old_mode[reg]==(TMU_TCR(reg) & 0x7))
		return;
	// (0xFFFF after a hard reset, when there is no divider yet)
	const u32 mode_before=old_mode[reg];
	old_mode[reg]=(TMU_TCR(reg) & 0x7);

	u32 TCNT=read_TMU_TCNTch(reg);
	switch(TMU_TCR(reg) & 0x7)
	{
		case 0: //4
			tmu_shift[reg]=2;
			break;

		case 1: //16
			tmu_shift[reg]=4;
			break;

		case 2: //64
			tmu_shift[reg]=6;
			break;

		case 3: //256
			tmu_shift[reg]=8;
			break;

		case 4: //1024
			tmu_shift[reg]=10;
			break;

		case 5: //reserved
			INFO_LOG(SH4, "TMU ch%d - TCR%d mode is reserved (5)",reg,reg);
			break;

		case 6: //RTC
			INFO_LOG(SH4, "TMU ch%d - TCR%d mode is RTC (6), can't be used on Dreamcast",reg,reg);
			break;

		case 7: //external
			INFO_LOG(SH4, "TMU ch%d - TCR%d mode is External (7), can't be used on Dreamcast",reg,reg);
			break;
	}
	/* The last three are clocks this machine does not have. The divider is
	 * left as it was and the 2 below is added to it: once, on going to one
	 * of them. It was added again on going from one of them to another, and
	 * a game that kept doing that had the shift count past the width of
	 * what it shifts. */
	if (old_mode[reg]>=5 && mode_before>=5 && mode_before<=7)
		tmu_shift[reg]-=2;
	tmu_shift[reg]+=2;
	write_TMU_TCNTch(reg,TCNT);
}

//Write to status registers
template<int ch>
void TMU_TCR_write(u32 addr, u32 data)
{
	// (only channel 2 has the input capture bits)
	data &= ch == 2 ? 0x03ff : 0x013f;
	/* The underflow and input capture flags are cleared by writing 0 to
	 * them. Writing 1 leaves a flag as it is: it does not set it, and the
	 * interrupt that used to follow is not one the timer gives. */
	data &= TMU_TCR(ch) | ~(u32)(tmu_underflow | tmu_ICPF);
	TMU_TCR(ch)=(u16)data;
	UpdateTMUCounts(ch);
}

//Chan 2 not used functions
static u32 TMU_TCPR2_read(u32 addr)
{
	INFO_LOG(SH4, "Read from TMU_TCPR2 - this register should be not used on Dreamcast according to docs");
	return 0;
}

static void TMU_TCPR2_write(u32 addr, u32 data)
{
	INFO_LOG(SH4, "Write to TMU_TCPR2 - this register should be not used on Dreamcast according to docs, data=%d", data);
}

static void write_TMU_TSTR(u32 addr, u32 data)
{
	TMU_TSTR=data & 7;
	//?

	for (int i=0;i<3;i++)
		turn_on_off_ch(i,data&(1<<i));
}

static int sched_tmu_cb(int ch, int sch_cycl, int jitter)
{
	if (tmu_mask64[ch])
	{
		const s64 count = read_TMU_TCNTch64(ch);

		/* Below 0 it has underflowed. Not below 0, the call has come
		 * before time - as much time as the scheduler takes was asked for,
		 * and it was not enough: the rest is asked for. (This used to
		 * take a count no greater than the number of cycles the call was
		 * late by for an underflow: ticks compared with cycles, and the
		 * flag could be raised some hundreds of ticks early.) */
		if (count < 0)
			tmu_underflowed(ch, count);
		else
			sched_chan_tick(ch);
	}
	return 0;	// asked for again already, where there is something to come
}

u32 sh4_sched_remaining(int id);

/* To be called at the end of a state load, when the registers, the
 * scheduler's clock and the variables above are those of the state:
 * everything here that follows from something else is worked out again, and
 * what does not is brought within what the code above can take. A state
 * that is as this file makes it is left as it is. */
void tmu_state_loaded(void)
{
	for (u32 ch = 0; ch < 3; ch++)
	{
		const u32 mode = TMU_TCR(ch) & 7;

		/* The divider: 4, 16, 64, 256 or 1024 of a clock that is a
		 * quarter of the scheduler's. With a clock source the machine does
		 * not have (5 to 7) it is four times the divider there was before,
		 * which only the state knows: any of those. */
		if (mode <= 4)
			tmu_shift[ch] = 4 + 2 * mode;
		else if (tmu_shift[ch] > 14)
			tmu_shift[ch] = 14;
		else if (tmu_shift[ch] < 6 || (tmu_shift[ch] & 1))
			tmu_shift[ch] = 6;
		old_mode[ch] = mode;

		tmu_mask[ch] = (TMU_TSTR & (1 << ch)) ? 0xFFFFFFFF : 0x00000000;
		tmu_mask64[ch] = (TMU_TSTR & (1 << ch)) ? 0xFFFFFFFFFFFFFFFF : 0x0000000000000000;

		// a count is 32 bits, or a little below 0 when the state was made between an underflow and its call
		s64 count = read_TMU_TCNTch64(ch);
		if (count > 0xFFFFFFFFll || count < -0xFFFFFFFFll)
			count = (u32)count;
		tmu_ch_base64[ch] = count + ((sh4_sched_now64() >> tmu_shift[ch])&tmu_mask64[ch]);
		tmu_ch_base[ch] = (u32)tmu_ch_base64[ch];

		InterruptPend(tmu_intID[ch],TMU_TCR(ch) & tmu_underflow);
		InterruptMask(tmu_intID[ch],TMU_TCR(ch) & tmu_UNIE);

		if (count < 0)
			tmu_underflowed(ch, count);
		else if (!tmu_mask64[ch])
		{
			// stopped: nothing is to come
			if (sh4_sched_is_scheduled(tmu_sched[ch]))
				sh4_sched_request(tmu_sched[ch], -1);
		}
		else
		{
			/* The call the state has asked for is left as it is when it
			 * comes no later than the underflow: one that comes before it
			 * asks again for the rest (sched_tmu_cb). Asked for anew from
			 * here, it would be the same call with other numbers in the
			 * scheduler, and two machines in the same state would save
			 * states that are not the same bytes. */
			const u64 now = sh4_sched_now64();
			const s64 togo = (s64)((((now >> tmu_shift[ch]) + count + 1) << tmu_shift[ch]) - 1 - now);

			if (!sh4_sched_is_scheduled(tmu_sched[ch])
					|| (s64)(s32)sh4_sched_remaining(tmu_sched[ch]) > togo)
				sched_chan_tick(ch);
		}
	}
}

//Init/Res/Term
void tmu_init()
{
	//TMU TOCR 0xFFD80000 0x1FD80000 8 0x00 0x00 Held Held Pclk
	sh4_rio_reg(TMU,TMU_TOCR_addr,RIO_DATA,8);
	sh4_rio_wmask(TMU,TMU_TOCR_addr,0x01);

	//TMU TSTR 0xFFD80004 0x1FD80004 8 0x00 0x00 Held 0x00 Pclk
	sh4_rio_reg(TMU,TMU_TSTR_addr,RIO_WF,8,0,&write_TMU_TSTR);

	//TMU TCOR0 0xFFD80008 0x1FD80008 32 0xFFFFFFFF 0xFFFFFFFF Held Held Pclk
	sh4_rio_reg(TMU,TMU_TCOR0_addr,RIO_DATA,32);

	//TMU TCNT0 0xFFD8000C 0x1FD8000C 32 0xFFFFFFFF 0xFFFFFFFF Held Held Pclk
	sh4_rio_reg(TMU,TMU_TCNT0_addr,RIO_FUNC,32,&read_TMU_TCNT<0>,&write_TMU_TCNT<0>);

	//TMU TCR0 0xFFD80010 0x1FD80010 16 0x0000 0x0000 Held Held Pclk
	sh4_rio_reg(TMU,TMU_TCR0_addr,RIO_WF,16,0,&TMU_TCR_write<0>);

	//TMU TCOR1 0xFFD80014 0x1FD80014 32 0xFFFFFFFF 0xFFFFFFFF Held Held Pclk
	sh4_rio_reg(TMU,TMU_TCOR1_addr,RIO_DATA,32);

	//TMU TCNT1 0xFFD80018 0x1FD80018 32 0xFFFFFFFF 0xFFFFFFFF Held Held Pclk
	sh4_rio_reg(TMU,TMU_TCNT1_addr,RIO_FUNC,32,&read_TMU_TCNT<1>,&write_TMU_TCNT<1>);

	//TMU TCR1 0xFFD8001C 0x1FD8001C 16 0x0000 0x0000 Held Held Pclk
	sh4_rio_reg(TMU,TMU_TCR1_addr,RIO_WF,16,0,&TMU_TCR_write<1>);

	//TMU TCOR2 0xFFD80020 0x1FD80020 32 0xFFFFFFFF 0xFFFFFFFF Held Held Pclk
	sh4_rio_reg(TMU,TMU_TCOR2_addr,RIO_DATA,32);

	//TMU TCNT2 0xFFD80024 0x1FD80024 32 0xFFFFFFFF 0xFFFFFFFF Held Held Pclk
	sh4_rio_reg(TMU,TMU_TCNT2_addr,RIO_FUNC,32,&read_TMU_TCNT<2>,&write_TMU_TCNT<2>);
	
	//TMU TCR2 0xFFD80028 0x1FD80028 16 0x0000 0x0000 Held Held Pclk
	sh4_rio_reg(TMU,TMU_TCR2_addr,RIO_WF,16,0,&TMU_TCR_write<2>);

	//TMU TCPR2 0xFFD8002C 0x1FD8002C 32 Held Held Held Held Pclk
	sh4_rio_reg(TMU,TMU_TCPR2_addr,RIO_FUNC,32,&TMU_TCPR2_read,&TMU_TCPR2_write);

	for (int i = 0; i < 3; i++) {
		tmu_sched[i] = sh4_sched_register(i, &sched_tmu_cb);
		sh4_sched_request(tmu_sched[i], -1);
	}
}


void tmu_reset(bool hard)
{
	if (hard)
	{
		memset(tmu_shift, 0, sizeof(tmu_shift));
		memset(tmu_mask, 0, sizeof(tmu_mask));
		memset(tmu_mask64, 0, sizeof(tmu_mask64));
		memset(old_mode, 0xFF, sizeof(old_mode));
		memset(tmu_ch_base, 0, sizeof(tmu_ch_base));
		memset(tmu_ch_base64, 0, sizeof(tmu_ch_base64));
	}
	TMU_TOCR=TMU_TSTR=0;
	TMU_TCOR(0) = TMU_TCOR(1) = TMU_TCOR(2) = 0xffffffff;
//	TMU_TCNT(0) = TMU_TCNT(1) = TMU_TCNT(2) = 0xffffffff;
	TMU_TCR(0) = TMU_TCR(1) = TMU_TCR(2) = 0;

	UpdateTMUCounts(0);
	UpdateTMUCounts(1);
	UpdateTMUCounts(2);

	write_TMU_TSTR(0,0);

	for (int i=0;i<3;i++)
		write_TMU_TCNTch(i,0xffffffff);
}

void tmu_term()
{
}
