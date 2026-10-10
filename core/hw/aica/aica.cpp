#include "aica.h"
#include "aica_if.h"
#include "aica_mem.h"
#include "sgc_if.h"
#include "hw/arm7/arm_mem.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "hw/sh4/sh4_sched.h"

#define SH4_IRQ_BIT (1 << (holly_SPU_IRQ & 31))

CommonData_struct* CommonData;
DSPData_struct* DSPData;
InterruptInfo* MCIEB;
InterruptInfo* MCIPD;
InterruptInfo* MCIRE;
InterruptInfo* SCIEB;
InterruptInfo* SCIPD;
InterruptInfo* SCIRE;

//Interrupts
//arm side
static u32 GetL(u32 which)
{
   if (which > 7)
      which = 7; //higher bits share bit 7

   u32 bit = 1 << which;
   u32 rv  = 0;

   if (CommonData->SCILV0 & bit)
      rv = 1;

   if (CommonData->SCILV1 & bit)
      rv |= 2;

   if (CommonData->SCILV2 & bit)
      rv |= 4;

   return rv;
}

static void update_arm_interrupts()
{
   u32 p_ints=SCIEB->full & SCIPD->full;

   /* Nothing pending and nothing raised: there is nothing to tell the ARM.
    * This is how it is for nearly every sample, and each of them asks. */
   if (p_ints == 0 && !aica_interr)
      return;

   u32 Lval=0;
   if (p_ints)
   {
      u32 bit_value=1;//first bit
      //scan all interrupts , lo to hi bit.I assume low bit ints have higher priority over others
      for (u32 i=0;i<11;i++)
      {
         if (p_ints & bit_value)
         {
            //for the first one , Set the L reg & exit
            Lval=GetL(i);
            break;
         }
         bit_value<<=1; //next bit
      }
   }

   libARM_InterruptChange(p_ints,Lval);
}

//sh4 side
static void UpdateSh4Ints()
{
   u32 p_ints = MCIEB->full & MCIPD->full;
   if (p_ints)
   {
      if ((SB_ISTEXT & SH4_IRQ_BIT) == 0)
         //if no interrupt is already pending then raise one :)
         asic_RaiseInterrupt(holly_SPU_IRQ);
   }
   else
   {
      if ((SB_ISTEXT & SH4_IRQ_BIT) != 0)
         asic_CancelInterrupt(holly_SPU_IRQ);
   }

}


AicaTimer timers[3];
int aica_schid = -1;
const int AICA_TICK = 145125;	// 44.1 KHz / 32

u32 sh4_sched_remaining(int id);

/* How many of the 32 samples of the tick in progress the hardware has put
 * out by now: the tick is emulated in one go when it ends, but the AICA
 * makes a sample every 1/44100 s. */
u32 libAICA_SamplesIntoTick()
{
	if (aica_schid == -1)
		return 0;

	u32 remaining = sh4_sched_remaining(aica_schid);
	if (remaining >= (u32)AICA_TICK)
		// not scheduled, or due this instant
		return 0;

	u32 samples = (u32)(((u64)(AICA_TICK - remaining) * 32) / AICA_TICK);
	return samples > 31 ? 31 : samples;
}

static int AicaUpdate(int tag, int c, int j)
{
   aicaarm::run(32);

	return AICA_TICK;
}

//Mainloop

void libAICA_TimeStep()
{
   for (int i=0;i<3;i++)
		timers[i].StepTimer(1);

	SCIPD->SAMPLE_DONE = 1;
	MCIPD->SAMPLE_DONE = 1;

	AICA_Sample();

	//Make sure sh4/arm interrupt system is up to date :)
	update_arm_interrupts();
	UpdateSh4Ints();	
}

static void AicaInternalDMA()
{
	/* A transfer to the registers can reach the register that starts one, and
	 * set its start bit again: the transfer is already running, and that is
	 * all the bit says. Starting another from inside this one, each of which
	 * did the same, never came back. The bit reads 0 once this is done. */
	static bool running;

	if (!CommonData->DEXE || running)
		return;
	running = true;

	// Start dma
	DEBUG_LOG(AICA, "AICA internal DMA: DGATE %d DDIR %d DLG %x", CommonData->DGATE, CommonData->DDIR, CommonData->DLG);
	if (CommonData->DGATE)
	{
		// Clear memory/registers
		if (CommonData->DDIR)
		{
			// to wave mem
			u32 addr = ((CommonData->DMEA_hi << 16) | (CommonData->DMEA_lo << 2)) & ARAM_MASK;
			// (DLG counts 32-bit words; what is left of memory is in bytes)
			u32 len = std::min(CommonData->DLG, (ARAM_SIZE - addr) / 4);
			memset(&aica_ram.data[addr], 0, len * 4);
		}
		else
		{
			// to regs
			// (the length is taken once: it is one of the registers
			// this may clear)
			u32 addr = CommonData->DRGA << 2;
			const u32 len = CommonData->DLG;
			for (u32 i = 0; i < len; i++, addr += 4)
				WriteMem_aica_reg(addr, 0, 4);
		}
	}
	else
	{
		// Data xfer
		u32 waddr = ((CommonData->DMEA_hi << 16) | (CommonData->DMEA_lo << 2)) & ARAM_MASK;
		u32 raddr = CommonData->DRGA << 2;
		u32 len = std::min(CommonData->DLG, (ARAM_SIZE - waddr) / 4);
		if (CommonData->DDIR)
		{
			// reg to wave mem
			for (u32 i = 0; i < len; i++, waddr += 4, raddr += 4)
				*(u32*)&aica_ram[waddr] = ReadMem_aica_reg(raddr, 4);
		}
		else
		{
			// wave mem to regs
			for (u32 i = 0; i < len; i++, waddr += 4, raddr += 4)
				WriteMem_aica_reg(raddr, *(u32*)&aica_ram[waddr], 4);
		}
	}
	running = false;
	CommonData->DEXE = 0;
	MCIPD->DMA_END = 1;
	UpdateSh4Ints();
	SCIPD->DMA_END = 1;
	update_arm_interrupts();
}

//Memory i/o
template<u32 sz>
void WriteAicaReg(u32 reg,u32 data)
{
	switch (reg)
	{
	/* These four are written a byte at a time as well as whole. The low
	 * byte of a register is at its own address and is handled as the whole
	 * register is: a byte has nothing above bit 7. The high byte is at the
	 * address after, which only a byte write gets to. */
	case SCIPD_addr:
		if (data & (1<<5))
		{
			SCIPD->SCPU=1;
			update_arm_interrupts();
		}
		//Read only
		return;

	case SCIPD_addr + 1:
	case MCIPD_addr + 1:
		//Read only, and bit 5 is in the other byte
		return;

	case SCIRE_addr:
		{
			SCIPD->full&=~(data /*& SCIEB->full*/ );	//is the & SCIEB->full needed ? doesn't seem like it
			data=0;//Write only
			update_arm_interrupts();
		}
		break;

	case SCIRE_addr + 1:
		SCIPD->full &= ~(data << 8);
		update_arm_interrupts();
		break;

	case MCIPD_addr:
		if (data & (1<<5))
		{
			MCIPD->SCPU=1;
			UpdateSh4Ints();
		}
		//Read only
		return;

	case MCIRE_addr:
		{
			MCIPD->full&=~data;
			UpdateSh4Ints();
			//Write only
		}
		break;

	case MCIRE_addr + 1:
		MCIPD->full &= ~(data << 8);
		UpdateSh4Ints();
		break;

	// (the prescaler is in a timer's high byte)
	case TIMER_A:
	case TIMER_A + 1:
		WriteMemArr<sz>(aica_reg, reg, data);
		timers[0].RegisterWrite();
		break;

	case TIMER_B:
	case TIMER_B + 1:
		WriteMemArr<sz>(aica_reg, reg, data);
		timers[1].RegisterWrite();
		break;

	case TIMER_C:
	case TIMER_C + 1:
		WriteMemArr<sz>(aica_reg, reg, data);
		timers[2].RegisterWrite();
		break;

	// DEXE, DDIR, DLG
	case 0x288C:
		WriteMemArr<sz>(aica_reg, reg, data);
		AicaInternalDMA();
		break;

	default:
		WriteMemArr<sz>(aica_reg, reg, data);
		break;
	}
}



template void WriteAicaReg<1>(u32 reg,u32 data);
template void WriteAicaReg<2>(u32 reg,u32 data);

//misc :p
s32 libAICA_Init()
{
	init_mem();
	aica_Init();

	static_assert(sizeof(*CommonData) == 0x508, "Invalid CommonData size");
	static_assert(sizeof(*DSPData) == 0x15C8, "Invalid DSPData size");

	CommonData=(CommonData_struct*)&aica_reg[0x2800];
	DSPData=(DSPData_struct*)&aica_reg[0x3000];
	//slave cpu (arm7)

	SCIEB=(InterruptInfo*)&aica_reg[0x289C];
	SCIPD=(InterruptInfo*)&aica_reg[0x289C+4];
	SCIRE=(InterruptInfo*)&aica_reg[0x289C+8];
	//Main cpu (sh4)
	MCIEB=(InterruptInfo*)&aica_reg[0x28B4];
	MCIPD=(InterruptInfo*)&aica_reg[0x28B4+4];
	MCIRE=(InterruptInfo*)&aica_reg[0x28B4+8];

	sgc_Init();
	// (asked for by the reset that follows)
	aica_schid = sh4_sched_register(0, &AicaUpdate);

	return 0;
}

void libAICA_Reset(bool hard)
{
	if (hard)
	{
		init_mem();
		sgc_Init();
	}
	for (u32 i = 0; i < 3; i++)
		timers[i].Init(aica_reg, i);
	aica_Reset(hard);
	/* The sample clock does not stop for a reset: a tick that is on its
	 * way comes when it was going to. It is asked for here only when there
	 * is none, which is the first reset of a machine. */
	if (!sh4_sched_is_scheduled(aica_schid))
		sh4_sched_request(aica_schid, AICA_TICK);
}

void libAICA_Term()
{
	sgc_Term();
	term_mem();
}
