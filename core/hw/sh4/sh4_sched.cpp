
#include "types.h"
#include "sh4_interrupts.h"
#include "sh4_core.h"
#include "sh4_sched.h"

#include <compat/intrinsics.h>


//sh4 scheduler

/*

	register handler
	request callback at time

	single fire events only

	sh4_sched_register(id)
	sh4_sched_request(id, in_cycles)

	sh4_sched_now()

*/
u64 sh4_sched_ffb;


std::vector<sched_list> sch_list;

int sh4_sched_next_id=-1;

/* The clock, as sh4_sched_now() reads it: inline here, where it is read for
 * every event of every tick. (The functions the other files call can be
 * replaced from outside a shared library, and are not made inline.) */
static inline u32 sh4_sched_clock()
{
	return sh4_sched_ffb-Sh4cntx.sh4_sched_next;
}

static inline u32 sh4_sched_remaining(int id, u32 reference)
{
	if (sch_list[id].end != -1)
      return sch_list[id].end - reference;
   return -1;
}

// Whether there is a request waiting for this one
bool sh4_sched_is_scheduled(int id)
{
	// (an id that was never given out - a device this machine does not have - is waiting for nothing)
	return (size_t)id < sch_list.size() && sch_list[id].end != -1;
}

u32 sh4_sched_remaining(int id)
{
	return sh4_sched_remaining(id, sh4_sched_clock());
}

/* What sh4_sched_ffb and the time to the next event were set to last. */
static u64 sh4_sched_set_ffb;
static u32 sh4_sched_set_diff;

/* Set the next event to be slot, due in diff cycles. The clock reads the
 * same after as before: sh4_sched_ffb takes the change in sh4_sched_next. */
static void sh4_sched_set_next(int slot, u32 diff)
{
	sh4_sched_ffb-=Sh4cntx.sh4_sched_next;

	sh4_sched_next_id=slot;
   if (slot!=-1)
      Sh4cntx.sh4_sched_next=diff;
   else
      Sh4cntx.sh4_sched_next=SH4_MAIN_CLOCK;

	sh4_sched_ffb+=Sh4cntx.sh4_sched_next;
	sh4_sched_set_ffb=sh4_sched_ffb;
	sh4_sched_set_diff=diff;
}

/* Whether sh4_sched_next_id and sh4_sched_next may be out of date with
 * the list: an event was registered (its time cleared) since the last
 * look over all of them. */
static bool sh4_sched_stale = true;

/* The event that comes after the next one - of those waiting, the one
 * sh4_sched_ffts() would find if the next one were not there - and how
 * many cycles after the next one it comes, when that is known. And whether
 * the last look over the list saw an event whose time had gone by, or was
 * so far off it looks as if it had: then only a look over all of them can
 * tell what comes when. */
static bool sh4_sched_second_ok;
static int sh4_sched_second_id = -1;
static u32 sh4_sched_second_gap;
static bool sh4_sched_far = true;

static void sh4_sched_set_second(int id, u32 remaining, u32 diff)
{
	sh4_sched_second_ok = true;
	sh4_sched_second_id = id;
	sh4_sched_second_gap = id == -1 ? 0 : remaining - diff;
}

/* Of the first 32 events, the ones that are waiting (end is not -1), one
 * bit each: the tick goes through these alone. */
static u32 sh4_sched_waiting;

static inline void sh4_sched_mark(size_t id)
{
	if (id >= 32)
		return;
	if (sch_list[id].end != -1)
		sh4_sched_waiting |= 1u << id;
	else
		sh4_sched_waiting &= ~(1u << id);
}

/* The first id from @from on that may be waiting: with more than 32 events
 * there is no knowing, and it is @from. */
static inline size_t sh4_sched_next_waiting(size_t from)
{
	if (sch_list.size() > 32)
		return from;
	const u32 left = from < 32 ? sh4_sched_waiting >> from : 0;
	return left ? from + compat_ctz(left) : sch_list.size();
}

/* While sh4_sched_tick() goes through the list: the first id it has not
 * finished with yet, and whether an event before that was asked for again,
 * which the tick's own look for the next event has passed by. */
static bool sh4_sched_ticking;
static size_t sh4_sched_tick_pos;
static bool sh4_sched_tick_passed;
// (and whether an event other than the one being called was asked for)
static bool sh4_sched_tick_other;

void sh4_sched_ffts(void)
{
	u32 diff=-1;
	int slot=-1;
	u32 diff2=-1;
	int slot2=-1;
	bool overdue=false;
	const u32 now=sh4_sched_clock();

	// (the times may have been put in place from a state: the bits are made again)
	sh4_sched_waiting=0;
	for (size_t i=0;i<sch_list.size();i++)
	{
		sh4_sched_mark(i);
		const u32 remaining=sh4_sched_remaining(i, now);
		if (sch_list[i].end!=-1 && remaining>=0x80000000u)
			overdue=true;
		if (remaining<diff)
		{
			slot2=slot;
			diff2=diff;
			slot=i;
			diff=remaining;
		}
		else if (remaining<diff2)
		{
			slot2=i;
			diff2=remaining;
		}
	}

	sh4_sched_stale=false;
	sh4_sched_far=overdue;
	sh4_sched_set_next(slot, diff);
	sh4_sched_set_second(slot2, diff2, diff);
}

int sh4_sched_register(int tag, sh4_sched_callback* ssc)
{
	sched_list t={ssc,tag,-1,-1};

	/* The list is the process's and outlives the machine: every game
	 * loaded after the first has its devices register again. A callback
	 * that is already in the list keeps the place it has, and so the id
	 * its owner and the save state know it by, and whatever it was waiting
	 * for in the machine before is forgotten. Appended again each time,
	 * the list grew by up to ten a load, and the last machine's events -
	 * its scanline event, which asks for itself again for ever - went on
	 * being called beside the new one's. */
	sh4_sched_stale=true;
	for (size_t i=0;i<sch_list.size();i++)
	{
		if (sch_list[i].cb==ssc && sch_list[i].tag==tag)
		{
			sch_list[i]=t;
			sh4_sched_mark(i);
			return i;
		}
	}

	sch_list.push_back(t);
	sh4_sched_mark(sch_list.size()-1);

	return sch_list.size()-1;
}

/*
	Return current cycle count, in 32 bits (wraps after 21 dreamcast seconds)
*/
u32 sh4_sched_now(void)
{
	return sh4_sched_ffb-Sh4cntx.sh4_sched_next;
}

/*
	Return current cycle count, in 64 bits (effectively never wraps)
*/
u64 sh4_sched_now64(void)
{
	return sh4_sched_ffb-Sh4cntx.sh4_sched_next;
}
void sh4_sched_request(int id, int cycles)
{
	verify(cycles== -1 || (cycles >= 0 && cycles <= SH4_MAIN_CLOCK));

	sch_list[id].start = sh4_sched_clock();
   sch_list[id].end   = -1;

	if (cycles != -1)
	{
		// (the time is 32 bits and wraps: added as unsigned, a signed sum past the top is not defined)
		sch_list[id].end = (int)((u32)sch_list[id].start + (u32)cycles);
		if (sch_list[id].end == -1)
			sch_list[id].end++;
	}
	sh4_sched_mark(id);

	if (sh4_sched_ticking)
	{
		/* The tick looks for the next event as it goes, and once at the end
		 * if it has gone past this one already. */
		if ((size_t)id < sh4_sched_tick_pos)
			sh4_sched_tick_passed = true;
		if ((size_t)id != sh4_sched_tick_pos)
			sh4_sched_tick_other = true;
		return;
	}

	/* Since the last look over the list every event's time has come nearer
	 * by the same, and while the next one has not come (sh4_sched_next is
	 * not below 0) none has come round past it: the next is the one that
	 * was, or this one if it comes first - or as soon, with a smaller id -
	 * unless this one was the next and now comes later. That holds while
	 * the clock has only gone on since: a state loaded or a CPU reset
	 * leaves it to sh4_sched_ffts(). */
	const int next=Sh4cntx.sh4_sched_next;
	const int slot=sh4_sched_next_id;
	if (!sh4_sched_stale && slot!=-1 && next>=0
			&& sh4_sched_ffb==sh4_sched_set_ffb && (u32)next<=sh4_sched_set_diff)
	{
		const u32 diff=(u32)next;
		const u32 remaining=sh4_sched_remaining(id, sch_list[id].start);
		// (when the one after the next is known: how far off it is now)
		const u32 second=diff+sh4_sched_second_gap;
		const int slot2=sh4_sched_second_id;
		if (id==slot)
		{
			if (remaining<=diff)
			{
				sh4_sched_set_next(slot, remaining);
				if (sh4_sched_second_ok)
					sh4_sched_set_second(slot2, second, remaining);
				return;
			}
		}
		else
		{
			if (remaining<diff || (remaining==diff && id<slot))
			{
				// the next one is the one after this now
				sh4_sched_set_next(id, remaining);
				sh4_sched_set_second(slot, diff, remaining);
			}
			else if (sh4_sched_second_ok)
			{
				if (id==slot2)
				{
					if (remaining<=second)
						sh4_sched_second_gap=remaining-diff;
					else
						sh4_sched_second_ok=false;
				}
				else if (sch_list[id].end!=-1
						&& (slot2==-1 || remaining<second || (remaining==second && id<slot2)))
					sh4_sched_set_second(id, remaining, diff);
			}
			return;
		}
	}

	sh4_sched_ffts();
}

/* Returns how much time has passed for this callback */
static int sh4_sched_elapsed(int id)
{
   if (sch_list[id].end == -1)
      return -1;

   int rv=sh4_sched_clock()-sch_list[id].start;
   sch_list[id].start=sh4_sched_clock();
   return rv;
}

static void handle_cb(int id)
{
	int remain=(int)((u32)sch_list[id].end-(u32)sch_list[id].start);
	int elapsd=sh4_sched_elapsed(id);
	int jitter=elapsd-remain;

	sch_list[id].end=-1;
	sh4_sched_mark(id);
	int re_sch=sch_list[id].cb(sch_list[id].tag,remain,jitter);

	if (re_sch > 0)
		sh4_sched_request(id, std::max(0, re_sch - jitter));
}

void sh4_sched_tick(int cycles)
{
	/*
	Sh4cntx.sh4_sched_time+=cycles;
	Sh4cntx.sh4_sched_next-=cycles;
	*/

	if (Sh4cntx.sh4_sched_next<0)
	{
		const u32 now=sh4_sched_clock();
		u32 fztime=now-cycles;
		if (sh4_sched_next_id!=-1)
		{
			size_t first = sh4_sched_next_waiting(0);

			sh4_sched_ticking = true;
			sh4_sched_tick_passed = false;
			sh4_sched_tick_other = false;

			/* Most ticks, the next event alone has come: the one after it
			 * comes after now, and the next is called alone. That is known
			 * when the clock has only gone on since the next and the one
			 * after it were found, by no more than this tick (the next one
			 * is in it), and no time had gone by. */
			const int next = Sh4cntx.sh4_sched_next;
			const int slot0 = sh4_sched_next_id;
			const int slot2 = sh4_sched_second_id;
			if (sh4_sched_second_ok && !sh4_sched_stale && !sh4_sched_far
					&& sh4_sched_ffb == sh4_sched_set_ffb && next >= -cycles
					&& (slot2 == -1 || (s64)next + sh4_sched_second_gap > 0))
			{
				sh4_sched_tick_pos = slot0;
				handle_cb(slot0);
				if (!sh4_sched_tick_other && !sh4_sched_stale)
				{
					sh4_sched_ticking = false;
					const u32 remaining = sh4_sched_remaining(slot0, now);
					const u32 second = (u32)next + sh4_sched_second_gap;
					if (slot2 == -1 || remaining < second || (remaining == second && slot0 < slot2))
					{
						const int slot = remaining != (u32)-1 ? slot0 : -1;
						sh4_sched_set_next(slot, remaining);
						if (slot != -1)
							sh4_sched_set_second(slot2, second, remaining);
						else
							sh4_sched_set_second(-1, 0, 0);
					}
					else
					{
						sh4_sched_set_next(slot2, second);
						sh4_sched_second_ok = false;
					}
					return;
				}
				// the others asked for are looked at as ever, and then all of them again
				first = sh4_sched_next_waiting(slot0 + 1);
				sh4_sched_tick_passed = true;
			}

			/* What sh4_sched_ffts() would find after: each event is looked
			 * at once it is done with, and again only if it is asked for
			 * after that. (The clock does not move during a tick.) The ones
			 * not waiting are passed over: they are neither called nor
			 * the next. */
			u32 diff=-1;
			int slot=-1;
			u32 diff2=-1;
			int slot2b=-1;
			bool overdue=false;

			for (size_t i = first; i < sch_list.size(); i = sh4_sched_next_waiting(i + 1))
			{
				sh4_sched_tick_pos = i;
				int remaining = sh4_sched_remaining(i, fztime);
				verify(remaining >= 0 || remaining == -1);
				if (remaining >= 0 && remaining <= (u32)cycles)
					handle_cb(i);
				const u32 next = sh4_sched_remaining(i, now);
				if (sch_list[i].end != -1 && next >= 0x80000000u)
					overdue = true;
				if (next < diff)
				{
					slot2b = slot;
					diff2 = diff;
					slot = i;
					diff = next;
				}
				else if (next < diff2)
				{
					slot2b = i;
					diff2 = next;
				}
			}
			sh4_sched_ticking = false;
			if (!sh4_sched_tick_passed && !sh4_sched_stale)
			{
				sh4_sched_far = overdue;
				sh4_sched_set_next(slot, diff);
				sh4_sched_set_second(slot2b, diff2, diff);
				return;
			}
		}
		sh4_sched_ffts();
	}
}
