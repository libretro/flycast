/*
	Highly inefficient and boring interpreter. Nothing special here
*/

#include "types.h"
#include "hw/sh4/sh4_cycles.h"
#include "hw/sh4/modules/mmu.h"

#include "../sh4_interpreter.h"
#include "../sh4_opcode_list.h"
#include "../sh4_core.h"
#include "../sh4_interrupts.h"
#include "hw/sh4/sh4_mem.h"
#include "../sh4_sched.h"
#include "hw/holly/sb.h"
#include "../sh4_cache.h"

#define CPU_RATIO      (8)

sh4_icache icache;
sh4_ocache ocache;

static s32 l;

// The cycles of what is being run, for the accurate setting
static Sh4Cycles timing;

static void ExecuteOpcode(u16 op)
{
	if (sr.FD == 1 && OpDesc[op]->IsFloatingPoint())
		RaiseFPUDisableException();
	OpPtr[op](op);
	if (settings.dynarec.AccurateTiming)
		l -= timing.count(op, mmu_enabled() ? 5 : 2);
	else
		l -= CPU_RATIO;
}

static u16 ReadNexOp()
{
	u32 addr = next_pc;
	next_pc += 2;

	return IReadMem16(addr);
}

void Sh4_int_Run()
{
   sh4_int_bCpuRun = true;
	RestoreHostRoundingMode();

	l = SH4_TIMESLICE;

   do
   {
#if !defined(NO_MMU)
      try {
#endif
         do
         {
			u32 op = ReadNexOp();

            ExecuteOpcode(op);
         } while (l > 0);
         l += SH4_TIMESLICE;
         timing.reset();
         UpdateSystem_INTC();
#if !defined(NO_MMU)
      }
      catch (SH4ThrownException& ex) {
         Do_Exception(ex.epc, ex.expEvn, ex.callVect);
         l -= CPU_RATIO * 5;	// an exception requires the instruction pipeline to drain, so approx 5 cycles
      }
#endif
   } while(sh4_int_bCpuRun);
   
   sh4_int_bCpuRun=false;
}

void Sh4_int_Stop()
{
	if (sh4_int_bCpuRun)
	{
		sh4_int_bCpuRun=false;
		// and the time slice ends with the instruction being executed
		l = 0;
	}
}

void Sh4_int_Start()
{
	if (!sh4_int_bCpuRun)
		sh4_int_bCpuRun=true;
}


void Sh4_int_Step()
{
	if (sh4_int_bCpuRun)
	{
		WARN_LOG(INTERPRETER, "Sh4 Is running , can't step");
	}
	else
	{
		u32 op = ReadNexOp();
		ExecuteOpcode(op);
	}
}

void Sh4_int_Skip()
{
	if (!sh4_int_bCpuRun)
		next_pc+=2;
}

void Sh4_int_Reset(bool hard)
{
	if (sh4_int_bCpuRun)
		return;

	if (hard)
	{
		/* All but how long it is to the scheduler's next event: the
		 * scheduler's clock is counted from that, and what is queued -
		 * some of it by the devices, reset just before - is timed by the
		 * clock. Cleared, the clock jumped ahead by as much and all of it
		 * came early. */
		int sched_next = Sh4cntx.sh4_sched_next;

		memset(&p_sh4rcb->cntx, 0, sizeof(p_sh4rcb->cntx));
		Sh4cntx.sh4_sched_next = sched_next;
	}
	next_pc = 0xA0000000;

	memset(r,0,sizeof(r));
	memset(r_bank,0,sizeof(r_bank));

	gbr=ssr=spc=sgr=dbr=vbr=0;
	mac.full=pr=fpul=0;

	sh4_sr_SetFull(0x700000F0);
	old_sr.status=sr.status;
	UpdateSR();

	fpscr.full = 0x00040001;
	old_fpscr=fpscr;
	UpdateFPSCR();
	icache.Reset(hard);
	ocache.Reset(hard);

	//Any more registers have default value ?
	INFO_LOG(INTERPRETER, "Sh4 Reset");
}

bool Sh4_int_IsCpuRunning()
{
	return sh4_int_bCpuRun;
}

//TODO : Check for valid delayslot instruction
void ExecuteDelayslot()
{
#if !defined(NO_MMU)
   try {
#endif
      u32 op = ReadNexOp();

      ExecuteOpcode(op);
#if !defined(NO_MMU)
   }
   catch (SH4ThrownException& ex) {
		AdjustDelaySlotException(ex);
      //printf("Delay slot exception\n");
      throw ex;
   }
#endif
}

void ExecuteDelayslot_RTE()
{
	// In an RTE delay slot, status register (SR) bits are referenced as follows.
	// In instruction access, the MD bit is used before modification, and in data access,
	// the MD bit is accessed after modification.
	// The other bits - S, T, M, Q, FD, BL, and RB - after modification are used for delay slot
	// instruction execution. The STC and STC.L SR instructions access all SR bits after modification.
	bool fetched = false;
#if !defined(NO_MMU)
   try {
#endif
      u32 op = ReadNexOp();

      // Fetched as RTE was, in privileged mode. Now SR is as it was saved.
      fetched = true;
      sh4_sr_SetFull(ssr);
      ExecuteOpcode(op);
#if !defined(NO_MMU)
   }
   catch (SH4ThrownException& ex) {
      ERROR_LOG(INTERPRETER, "Exception in RTE delay slot");
      // the return is made all the same: with SR as it was saved
      if (!fetched)
         sh4_sr_SetFull(ssr);
   }
#endif
}

// every SH4_TIMESLICE cycles
int UpdateSystem()
{
	Sh4cntx.sh4_sched_next -= SH4_TIMESLICE;
	if (Sh4cntx.sh4_sched_next<0)
		sh4_sched_tick(SH4_TIMESLICE);

	return Sh4cntx.interrupt_pend;
}

int UpdateSystem_INTC()
{
	if (UpdateSystem())
		return UpdateINTC();
	else
		return 0;
}

static void sh4_int_resetcache()
{
}

//Get an interface to sh4 interpreter
void Get_Sh4Interpreter(sh4_if *cpu)
{
	cpu->Run = Sh4_int_Run;
	cpu->Stop = Sh4_int_Stop;
	cpu->Start = Sh4_int_Start;
	cpu->Step = Sh4_int_Step;
	cpu->Skip = Sh4_int_Skip;
	cpu->Reset = Sh4_int_Reset;
	cpu->Init = Sh4_int_Init;
	cpu->Term = Sh4_int_Term;
	cpu->IsCpuRunning = Sh4_int_IsCpuRunning;

	cpu->ResetCache = sh4_int_resetcache;
}

void Sh4_int_Init()
{
	static_assert(sizeof(Sh4cntx) == 448, "Invalid Sh4Cntx size");

	memset(&p_sh4rcb->cntx, 0, sizeof(p_sh4rcb->cntx));
}

void Sh4_int_Term()
{
	Sh4_int_Stop();
	INFO_LOG(INTERPRETER, "Sh4 Term");
}
