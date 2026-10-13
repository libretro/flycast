/*
	This file is a mix of my code, Zezu's, and duno wtf-else (most likely ElSemi's ?)
*/
#include "types.h"
#include "hw/holly/sb.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/holly/holly_intc.h"
#include "hw/maple/maple_devs.h"

#include "naomi.h"
#include "naomi_cart.h"
#include "naomi_regs.h"
#include "naomi_m3comm.h"
#include "decrypt.h"
#include "naomi_periph.h"
#include "card_reader.h"
#include "touchscreen.h"
#include "hopper.h"
#include "printer.h"
#include "hw/sh4/modules/modules.h"
#include "savewriter.h"
#include <streams/file_stream.h>
#include <algorithm>
#include <vector>

//#define NAOMI_COMM

static NaomiM3Comm m3comm;

static const u32 BoardID=0x980055AA;
u32 GSerialBuffer=0,BSerialBuffer=0;
int GBufPos=0,BBufPos=0;
int GState=0,BState=0;
int GOldClk=0,BOldClk=0;
int BControl=0,BCmd=0,BLastCmd=0;
int GControl=0,GCmd=0,GLastCmd=0;
int SerStep=0,SerStep2=0;

static bool aw_ram_test_skipped = false;
static u8 aw_maple_devs;
static int coin_chute[4];
// how many times the DIMM board has answered: see naomi_process()
static int opcd;

/* A transfer from the cartridge takes time. It moves a piece at a time at
 * the rate of the board's bus, the registers that say how far it has got
 * move with it, and SB_GDST goes back to 0 and the interrupt is raised
 * after the last piece. Games count on it: one starts a sound whose data
 * is still coming, another looks at SB_GDST and SB_GDLEND while it waits. */
#define NAOMI_DMA_CHUNK 1024	// bytes moved at a time
static int naomi_dma_schid = -1;
// SH4 cycles for a byte: 10 is 20 MB a second, 27 is 7
static int naomi_dma_delay = 10;

extern std::vector<sched_list> sch_list;

#ifdef NAOMI_COMM
	u32 CommOffset;
	u32* CommSharedMem;
	HANDLE CommMapFile=INVALID_HANDLE_VALUE;
#endif

/*
El numero de serie solo puede contener:
0-9		(0x30-0x39)
A-H		(0x41-0x48)
J-N		(0x4A-0x4E)
P-Z		(0x50-0x5A)
*/
unsigned char BSerial[]="\xB7"/*CRC1*/"\x19"/*CRC2*/"0123234437897584372973927387463782196719782697849162342198671923649";
unsigned char GSerial[]="\xB7"/*CRC1*/"\x19"/*CRC2*/"0123234437897584372973927387463782196719782697849162342198671923649";

unsigned int ShiftCRC(unsigned int CRC,unsigned int rounds)
{
	const unsigned int Magic=0x10210000;
	unsigned int i;
	for(i=0;i<rounds;++i)
	{
		if(CRC&0x80000000)
			CRC=(CRC<<1)+Magic;
		else
			CRC=(CRC<<1);
	}
	return CRC;
}

unsigned short CRCSerial(const u8 *Serial,unsigned int len)
{
	unsigned int CRC=0xDEBDEB00;
	unsigned int i;

	for(i=0;i<len;++i)
	{
		unsigned char c=Serial[i];
		//CRC&=0xFFFFFF00;
		CRC|=c;
		CRC=ShiftCRC(CRC,8);
	}
	CRC=ShiftCRC(CRC,8);
	return (u16)(CRC>>16);
}


void NaomiInit()
{
	u16 CRC;
	CRC=CRCSerial(BSerial+2,0x2E);
	BSerial[0]=(u8)(CRC>>8);
	BSerial[1]=(u8)(CRC);

	CRC=CRCSerial(GSerial+2,0x2E);
	GSerial[0]=(u8)(CRC>>8);
	GSerial[1]=(u8)(CRC);
}




void NaomiBoardIDWrite(const u16 Data)
{
	int Dat=Data&8;
	int Clk=Data&4;
	int Rst=Data&0x20;
	int Sta=Data&0x10;
	

	if(Rst)
	{
		BState=0;
		BBufPos=0;
	}

	
	if(Clk!=BOldClk && !Clk)	//Falling Edge clock
	{
		//State change
		if(BState==0 && Sta) 
			BState=1;		
		if(BState==1 && !Sta)
			BState=2;

		if((BControl&0xfff)==0xFF0)	//Command mode
		{
			BCmd<<=1;
			if(Dat)
				BCmd|=1;
			else
				BCmd&=0xfffffffe;
		}

		//State processing
		if(BState==1)		//LoadBoardID
		{
			BSerialBuffer=BoardID;
			BBufPos=0;		//??
		}
		if(BState==2)		//ShiftBoardID
		{
			BBufPos++;
		}
	}
	BOldClk=Clk;
}

u16 NaomiBoardIDRead()
{
	if((BControl&0xff)==0xFE)
		return 0xffff;
	// (the count goes on for as long as the game clocks it: one of the 32 bits)
	return (BSerialBuffer&(1u<<((31-BBufPos)&31)))?8:0;
}

static u32 AdaptByte(u8 val)
{
	return val<<24;
}

/* A byte of a serial number, as AdaptByte() gives it. The block is six bits
 * of a command the game shifts in (or whatever a save state has), and the
 * numbers end long before the last of those: nothing past the end. */
static u32 SerialByte(const unsigned char *serial, int block, unsigned int byte)
{
	const unsigned int index = 8u * (unsigned int)block + byte;

	return index < sizeof(GSerial) ? AdaptByte(serial[index]) : 0;
}

void NaomiBoardIDWriteControl(const u16 Data)
{
	if((Data&0xfff)==0xF30 && BCmd!=BLastCmd)
	{
		if((BCmd&0x81)==0x81)
		{
			SerStep2=(BCmd>>1)&0x3f;

			BSerialBuffer=0x00000000;	//First block contains CRC
			BBufPos=0;
		}
		if((BCmd&0xff)==0x55)	//Load Offset 0
		{
			BState=2;
			BBufPos=0;
			BSerialBuffer=SerialByte(BSerial, SerStep2, 0)>>1;
		}
		if((BCmd&0xff)==0xAA)	//Load Offset 1
		{
			BState=2;
			BBufPos=0;
			BSerialBuffer=SerialByte(BSerial, SerStep2, 1);
		}
		if((BCmd&0xff)==0x54)
		{
			BState=2;
			BBufPos=0;
			BSerialBuffer=SerialByte(BSerial, SerStep2, 2);
		}
		if((BCmd&0xff)==0xA8)
		{
			BState=2;
			BBufPos=0;
			BSerialBuffer=SerialByte(BSerial, SerStep2, 3);
		}
		if((BCmd&0xff)==0x50)
		{
			BState=2;
			BBufPos=0;
			BSerialBuffer=SerialByte(BSerial, SerStep2, 4);
		}
		if((BCmd&0xff)==0xA0)
		{
			BState=2;
			BBufPos=0;
			BSerialBuffer=SerialByte(BSerial, SerStep2, 5);
		}
		if((BCmd&0xff)==0x40)
		{
			BState=2;
			BBufPos=0;
			BSerialBuffer=SerialByte(BSerial, SerStep2, 6);
		}
		if((BCmd&0xff)==0x80)
		{
			BState=2;
			BBufPos=0;
			BSerialBuffer=SerialByte(BSerial, SerStep2, 7);
		}
		BLastCmd=BCmd;
	}
	BControl=Data;
}

void NaomiGameIDProcessCmd()
{
	if(GCmd!=GLastCmd)
	{
		if((GCmd&0x81)==0x81)
		{
			SerStep=(GCmd>>1)&0x3f;

			GSerialBuffer=0x00000000;	//First block contains CRC
			GBufPos=0;
		}
		if((GCmd&0xff)==0x55)	//Load Offset 0
		{
			GState=2;
			GBufPos=0;
			GSerialBuffer=SerialByte(GSerial, SerStep, 0)>>0;
		}
		if((GCmd&0xff)==0xAA)	//Load Offset 1
		{
			GState=2;
			GBufPos=0;
			GSerialBuffer=SerialByte(GSerial, SerStep, 1);
		}
		if((GCmd&0xff)==0x54)
		{
			GState=2;
			GBufPos=0;
			GSerialBuffer=SerialByte(GSerial, SerStep, 2);
		}
		if((GCmd&0xff)==0xA8)
		{
			GState=2;
			GBufPos=0;
			GSerialBuffer=SerialByte(GSerial, SerStep, 3);
		}
		if((GCmd&0xff)==0x50)
		{
			GState=2;
			GBufPos=0;
			GSerialBuffer=SerialByte(GSerial, SerStep, 4);
		}
		if((GCmd&0xff)==0xA0)
		{
			GState=2;
			GBufPos=0;
			GSerialBuffer=SerialByte(GSerial, SerStep, 5);
		}
		if((GCmd&0xff)==0x40)
		{
			GState=2;
			GBufPos=0;
			GSerialBuffer=SerialByte(GSerial, SerStep, 6);
		}
		if((GCmd&0xff)==0x80)
		{
			GState=2;
			GBufPos=0;
			GSerialBuffer=SerialByte(GSerial, SerStep, 7);
		}
		GLastCmd=GCmd;
	}
}


void NaomiGameIDWrite(const u16 Data)
{
	int Dat=Data&0x01;
	int Clk=Data&0x02;
	int Rst=Data&0x04;
	int Sta=Data&0x08;
	int Cmd=Data&0x10;
	

	if(Rst)
	{
		GState=0;
		GBufPos=0;
	}

	
	if(Clk!=GOldClk && !Clk)	//Falling Edge clock
	{
		//State change
		if(GState==0 && Sta) 
			GState=1;		
		if(GState==1 && !Sta)
			GState=2;
		
		
		
		

		//State processing
		if(GState==1)		//LoadBoardID
		{
			GSerialBuffer=BoardID;
			GBufPos=0;		//??
		}
		if(GState==2)		//ShiftBoardID
			GBufPos++;

		if(GControl!=Cmd && !Cmd)
		{
			NaomiGameIDProcessCmd();
		}
		GControl=Cmd;
	}
	if(Clk!=GOldClk && Clk)	//Rising Edge clock
	{
		if(Cmd)	//Command mode
		{
			GCmd<<=1;
			if(Dat)
				GCmd|=1;
			else
				GCmd&=0xfffffffe;
			GControl=Cmd;
		}
		
	}

	GOldClk=Clk;

}

u16 NaomiGameIDRead()
{
	return (GSerialBuffer&(1u<<((31-GBufPos)&31)))?1:0;
}

//DIMM board
//Uses interrupt ext#3  (holly_EXT_PCI)

//status/flags ? 0x1 is some completion/init flag(?), 0x100 is the interrupt disable flag (?)
//n1 bios rev g (n2/epr-23605b has similar behavior of not same):
//3c=0x1E03
//40=0
//44=0
//48=0
//read 4c
//wait for 4c not 0
//4c=[4c]-1

//Naomi 2 bios epr-23609
//read 3c
//wait 4c to be non 0
//

//SO the writes to 3c/stuff are not relaced with 4c '1'
//If the dimm board has some internal cpu/pic logic 
//4c '1' seems to be the init done bit (?)
//n1/n2 clears it after getting a non 0 value
//n1 bios writes the value -1, meaning it expects the bit 0 to be set
//.//

u32 reg_dimm_command;		// command, written, 0x1E03 some flag ?
u32 reg_dimm_offsetl;
u32 reg_dimm_parameterl;
u32 reg_dimm_parameterh;
u32 reg_dimm_status = 0x11;

void naomi_process(u32 command, u32 offsetl, u32 parameterl, u32 parameterh)
{
	DEBUG_LOG(NAOMI, "Naomi process 0x%04X 0x%04X 0x%04X 0x%04X", command, offsetl, parameterl, parameterh);
	DEBUG_LOG(NAOMI, "Possible format 0 %d 0x%02X 0x%04X",command >> 15,(command & 0x7e00) >> 9, command & 0x1FF);
	DEBUG_LOG(NAOMI, "Possible format 1 0x%02X 0x%02X", (command & 0xFF00) >> 8,command & 0xFF);

	u32 param=(command&0xFF);
	if (param==0xFF)
	{
		DEBUG_LOG(NAOMI, "invalid opcode or smth ?");
	}
	//else if (param!=3)
	if (opcd<255)
	{
		reg_dimm_command=0x8000 | (opcd%12<<9) | (0x0);
		DEBUG_LOG(NAOMI, "new reg is 0x%X", reg_dimm_command);
		asic_RaiseInterrupt(holly_EXP_PCI);
		DEBUG_LOG(NAOMI, "Interrupt raised");
		opcd++;
	}
}

u32 ReadMem_naomi(u32 address, u32 size)
{
	if (unlikely(CurrentCartridge == NULL))
	{
		INFO_LOG(NAOMI, "called without cartridge");
		return 0xFFFF;
	}
	// (a NAOMI's: an Atomiswave has no such board, and its cartridge answers there - as for a write)
	if (address >= NAOMI_COMM2_CTRL_addr && address <= NAOMI_COMM2_STATUS1_addr && SYSTEM_IS_NAOMI())
		return m3comm.ReadMem(address, size);
	else
		return CurrentCartridge->ReadMem(address, size);
}

void WriteMem_naomi(u32 address, u32 data, u32 size)
{
	if (unlikely(CurrentCartridge == NULL))
	{
		INFO_LOG(NAOMI, "called without cartridge");
		return;
	}
	if (address >= NAOMI_COMM2_CTRL_addr && address <= NAOMI_COMM2_STATUS1_addr && SYSTEM_IS_NAOMI())
		m3comm.WriteMem(address, data, size);
	else
		CurrentCartridge->WriteMem(address, data, size);
}

/* The next piece of the transfer that is under way. */
static int naomi_dma_sched(int tag, int sch_cycl, int jitter)
{
	static u32 zeroes[NAOMI_DMA_CHUNK / 4];
	const u32 total = (SB_GDLEN + 31) & ~31;
	u32 start = SB_GDSTARD;
	u32 len = 0;

	// (nothing under way: a save state's doing, or the cartridge is gone)
	if (SB_GDST != 1 || CurrentCartridge == NULL)
		return 0;

	if (SB_GDLEND < total)
		len = std::min(total - SB_GDLEND, (u32)NAOMI_DMA_CHUNK);
	SB_GDLEND += len;
	while (len > 0)
	{
		u32 block_len = len;
		void* ptr = CurrentCartridge->GetDmaPtr(block_len);
		if (block_len == 0 || ptr == NULL)
		{
			// Nothing there to read: the rest of the piece is zeroes, and
			// the transfer goes on to where it was asked to end.
			INFO_LOG(NAOMI, "DMA transfer past the end of the cartridge");
			WriteMemBlock_nommu_ptr(start, zeroes, len);
			start += len;
			break;
		}
		WriteMemBlock_nommu_ptr(start, (u32*)ptr, block_len);
		CurrentCartridge->AdvancePtr(block_len);
		len -= block_len;
		start += block_len;
	}
	SB_GDSTARD = start;
	// (stopped by what it wrote: a piece can land on the register that does that)
	if (SB_GDST != 1)
		return 0;
	if (SB_GDLEND >= total)
	{
		SB_GDST = 0;
		asic_RaiseInterrupt(holly_GDROM_DMA);
		return 0;
	}
	// (the time the next piece takes: never none, with a piece still to come)
	const u32 left = SB_GDLEN - SB_GDLEND;
	return (left == 0 || left > NAOMI_DMA_CHUNK ? NAOMI_DMA_CHUNK : left) * naomi_dma_delay;
}

//Dma Start
void Naomi_DmaStart(u32 addr, u32 data)
{
	// (a transfer that is under way goes on to its end: it is not started again)
	if ((data & 1) == 0 || SB_GDST == 1)
		return;

	if (SB_GDEN==0)
	{
		INFO_LOG(NAOMI, "Invalid (NAOMI)GD-DMA start, SB_GDEN=0. Ignoring it.");
		return;
	}

	if (!m3comm.DmaStart(addr, data) && CurrentCartridge != NULL)
	{
		DEBUG_LOG(NAOMI, "NAOMI-DMA start addr %08X len %d", SB_GDSTAR, SB_GDLEN);
		// (a cartridge is only read: the other direction is taken as this one)
		if (SB_GDDIR != 1)
			DEBUG_LOG(NAOMI, "NAOMI-DMA to the cartridge (SB_GDDIR=0): done as a read");
		SB_GDST = 1;
		SB_GDSTARD = SB_GDSTAR & 0x1FFFFFE0;
		SB_GDLEND = 0;
		sh4_sched_request(naomi_dma_schid, std::min(SB_GDLEN, (u32)NAOMI_DMA_CHUNK) * naomi_dma_delay);
		return;
	}
	SB_GDSTARD = SB_GDSTAR + SB_GDLEN;
	SB_GDLEND = SB_GDLEN;
	asic_RaiseInterrupt(holly_GDROM_DMA);
}

/* No transfer is under way any more, and its next piece is not to come. */
static void naomi_dma_cancel()
{
	if (naomi_dma_schid >= 0 && sh4_sched_is_scheduled(naomi_dma_schid))
		sh4_sched_request(naomi_dma_schid, -1);
}

void Naomi_DmaEnable(u32 addr, u32 data)
{
	SB_GDEN=data&1;
	if (SB_GDEN==0 && SB_GDST==1)
	{
		INFO_LOG(NAOMI, "(NAOMI)GD-DMA aborted");
		SB_GDST=0;
		naomi_dma_cancel();
	}
}
void naomi_reg_Init()
{
	#ifdef NAOMI_COMM
	CommMapFile = CreateFileMapping(
		INVALID_HANDLE_VALUE,    // use paging file
		NULL,                    // default security 
		PAGE_READWRITE,          // read/write access
		0,                       // max. object size 
		0x1000*4,                // buffer size  
		L"Global\\nullDC_103_naomi_comm");                 // name of mapping object

	if (CommMapFile == NULL || CommMapFile==INVALID_HANDLE_VALUE) 
	{ 
		_tprintf(TEXT("Could not create file mapping object (%d).\nTrying to open existing one\n"), 	GetLastError());
		
		CommMapFile=OpenFileMapping(
                   FILE_MAP_ALL_ACCESS,   // read/write access
                   FALSE,                 // do not inherit the name
                   L"Global\\nullDC_103_naomi_comm");               // name of mapping object 
	}
	
	if (CommMapFile == NULL || CommMapFile==INVALID_HANDLE_VALUE) 
	{ 
		_tprintf(TEXT("Could not open existing file either\n"), 	GetLastError());
		CommMapFile=INVALID_HANDLE_VALUE;
	}
	else
	{
		printf("NAOMI: Created \"Global\\nullDC_103_naomi_comm\"\n");
		CommSharedMem = (u32*) MapViewOfFile(CommMapFile,   // handle to map object
			FILE_MAP_ALL_ACCESS, // read/write permission
			0,                   
			0,                   
			0x1000*4);           

		if (CommSharedMem == NULL) 
		{ 
			_tprintf(TEXT("Could not map view of file (%d).\n"), 
				GetLastError()); 

			CloseHandle(CommMapFile);
			CommMapFile=INVALID_HANDLE_VALUE;
		}
		else
			printf("NAOMI: Mapped CommSharedMem\n");
	}
	#endif
	NaomiInit();

	naomi_dma_schid = sh4_sched_register(0, &naomi_dma_sched);

	sb_rio_register(SB_GDST_addr, RIO_WF, 0, &Naomi_DmaStart);

	sb_rio_register(SB_GDEN_addr, RIO_WF, 0, &Naomi_DmaEnable);
}

void naomi_reg_Term()
{
#ifdef NAOMI_COMM
	if (CommSharedMem)
	{
		UnmapViewOfFile(CommSharedMem);
	}
	if (CommMapFile!=INVALID_HANDLE_VALUE)
	{
		CloseHandle(CommMapFile);
	}
#endif
	// (the machine set up next may be a Dreamcast, which does not register this again)
	naomi_dma_cancel();
	m3comm.closeNetwork();
	naomi_periph_term();
}

void naomi_reg_Reset(bool hard)
{
	SB_GDST = 0;
	SB_GDEN = 0;
	naomi_dma_cancel();
	// An Atomiswave's bus is the slower one, and two games made for it and
	// moved to a NAOMI cartridge still expect its pace.
	if (settings.System == DC_PLATFORM_ATOMISWAVE
			|| !strcmp(naomi_game_id, "FORCE FIVE") || !strcmp(naomi_game_id, "KENJU"))
		naomi_dma_delay = 27;
	else
		naomi_dma_delay = 10;

	aw_ram_test_skipped = false;
	// (the kinds of device the last game had on ports 3 and 4: the next one
	// may leave the register alone and want plain controllers)
	aw_maple_devs = 0;
	GSerialBuffer = 0;
	BSerialBuffer = 0;
	GBufPos = 0;
	BBufPos = 0;
	GState = 0;
	BState = 0;
	GOldClk = 0;
	BOldClk = 0;
	BControl = 0;
	BCmd = 0;
	BLastCmd = 0;
	GControl = 0;
	GCmd = 0;
	GLastCmd = 0;
	SerStep = 0;
	SerStep2 = 0;
	reg_dimm_command = 0;
	reg_dimm_offsetl = 0;
	reg_dimm_parameterl = 0;
	reg_dimm_parameterh = 0;
	reg_dimm_status = 0x11;
	opcd = 0;
	memset(coin_chute, 0, sizeof(coin_chute));
	cryptoReset();
	m3comm.reset();
	if (hard && CurrentCartridge != NULL)
		CurrentCartridge->Reset();
	// (a hard reset is a power cycle: the devices start again, from their files)
	if (hard)
		naomi_periph_init();
}

/* The cartridge transfer's event as a save state has it. With no transfer
 * under way nothing is due, whatever the state says; with one, the next
 * piece is due within the time a piece takes, and at once if the state has
 * it otherwise. */
static void naomi_dma_set_state(s32 start, s32 end)
{
	if (naomi_dma_schid < 0)
		return;
	if (SB_GDST != 1 || CurrentCartridge == NULL)
		end = -1;
	else
	{
		const u32 now = sh4_sched_now();

		if (end == -1 || (u32)end - now > (u32)(NAOMI_DMA_CHUNK * naomi_dma_delay))
		{
			start = (s32)now;
			end = start == -1 ? 0 : start;
		}
	}
	sch_list[naomi_dma_schid].start = start;
	sch_list[naomi_dma_schid].end = end;
}

/* What the save states before V21 did not hold, for an arcade board (a
 * Dreamcast has none of it, and nothing is written for one):
 *   - when the next piece of a cartridge transfer is due (how far it has
 *     got is in the registers);
 *   - which devices an Atomiswave game has asked for on ports 3 and 4,
 *     whether its BIOS has been let past the RAM test, and how long each
 *     coin switch has been read as closed;
 *   - how many times the DIMM board has answered;
 *   - what the cartridge has to add (see Cartridge::SerializeV21()). */
void naomi_serialize_v21(void **data, unsigned int *total_size)
{
	s32 dma_start = -1, dma_end = -1;
	u8 board[7];

	if (settings.System == DC_PLATFORM_DREAMCAST)
		return;

	if (naomi_dma_schid >= 0)
	{
		dma_start = sch_list[naomi_dma_schid].start;
		dma_end = sch_list[naomi_dma_schid].end;
	}
	board[0] = aw_maple_devs;
	board[1] = aw_ram_test_skipped;
	board[2] = (u8)opcd;
	for (int i = 0; i < 4; i++)
		board[3 + i] = (u8)coin_chute[i];

	LIBRETRO_S(dma_start);
	LIBRETRO_S(dma_end);
	LIBRETRO_SA(board, 7);

	if (CurrentCartridge != NULL)
		CurrentCartridge->SerializeV21(data, total_size);
}

bool naomi_unserialize_v21(void **data, unsigned int *total_size)
{
	s32 dma_start = -1, dma_end = -1;
	u8 board[7] = { 0 };

	if (settings.System == DC_PLATFORM_DREAMCAST)
		return true;

	LIBRETRO_US(dma_start);
	LIBRETRO_US(dma_end);
	LIBRETRO_USA(board, 7);

	naomi_dma_set_state(dma_start, dma_end);
	aw_maple_devs = board[0] & 0xF0;
	aw_ram_test_skipped = board[1] != 0;
	opcd = board[2];
	for (int i = 0; i < 4; i++)
		coin_chute[i] = std::min<int>(board[3 + i], 5);

	if (CurrentCartridge != NULL)
		CurrentCartridge->UnserializeV21(data, total_size);
	return true;
}

/* An older state: no transfer was ever under way in one, since a transfer
 * was over as soon as it was asked for. The rest stays as the running
 * machine has it, which is what loading such a state has always done. */
void naomi_state_before_v21(void)
{
	if (settings.System == DC_PLATFORM_DREAMCAST)
		return;
	naomi_dma_set_state(-1, -1);
}

void naomi_jvs_serialize_v23(void **data, unsigned int *total_size);
bool naomi_jvs_unserialize_v23(void **data, unsigned int *total_size);

/* What the save states before V23 did not hold, on a NAOMI and NAOMI 2
 * (nothing is written for a Dreamcast or an Atomiswave, which have neither):
 *   - the communication board's four registers and its two 128 KB
 *     memories (NaomiM3Comm::serialize());
 *   - the 64 KB program on its way to the I/O board's processor, all
 *     zeroes when none is (maple_devs.cpp). */
void naomi_serialize_v23(void **data, unsigned int *total_size)
{
	if (!SYSTEM_IS_NAOMI())
		return;
	m3comm.serialize(data, total_size);
	naomi_jvs_serialize_v23(data, total_size);
}

bool naomi_unserialize_v23(void **data, unsigned int *total_size)
{
	if (!SYSTEM_IS_NAOMI())
		return true;
	return m3comm.unserialize(data, total_size)
		&& naomi_jvs_unserialize_v23(data, total_size);
}

/* An older state: the board and the program on its way stay as the running
 * machine has them, which is what loading such a state has always done. */
void naomi_state_before_v23(void)
{
}

extern u32 kcode[4];

u32 libExtDevice_ReadMem_A0_006(u32 addr,u32 size) {
	addr &= 0x7ff;
	//printf("libExtDevice_ReadMem_A0_006 %d@%08x: %x\n", size, addr, mem600[addr]);
	switch (addr)
	{
//	case 0:
//		return 0;
//	case 4:
//		return 1;
	case 0x280:
	   {
		  // 0x00600280 r  0000dcba
		  //	a/b - 1P/2P coin inputs (JAMMA), active low
		  //	c/d - 3P/4P coin inputs (EX. IO board), active low
		  //
		  //	(ab == 0) -> BIOS skip RAM test
		  if (!aw_ram_test_skipped)
		  {
			 // Skip the RAM test. Also fixes Dolphin Blue in-game freeze
			  aw_ram_test_skipped = true;
			 return 0;
		  }
		  u8 coins = 0xF;
		  for (int i = 0; i < 4; i++)
		  {
			 if (!(kcode[i] & AWAVE_COIN_KEY))
			 {
				// ggx15 needs 4 or 5 reads to register the coin but it needs to be limited to avoid coin errors
				if (coin_chute[i] < 5)
				{
				   coin_chute[i]++;
				   coins &= ~(1 << i);
				}
			 }
			 else
				coin_chute[i] = 0;
		  }

		  return coins;
	   }
	case 0x284:		// Atomiswave maple devices
		// ddcc0000 where cc/dd are the types of devices on maple bus 2 and 3:
		// 0: regular AtomisWave controller
		// 1: light gun
		// 2,3: mouse/trackball
		//printf("NAOMI 600284 read %x\n", aw_maple_devs);
		return aw_maple_devs;
	}
	INFO_LOG(NAOMI, "Unhandled read @ %x sz %d", addr, size);
	return 0xFF;
}

void libExtDevice_WriteMem_A0_006(u32 addr,u32 data,u32 size) {
	addr &= 0x7ff;
	//printf("libExtDevice_WriteMem_A0_006 %d@%08x: %x\n", size, addr, data);
	switch (addr)
	{
	case 0x284:		// Atomiswave maple devices
		DEBUG_LOG(NAOMI, "NAOMI 600284 write %x", data);
		aw_maple_devs = data & 0xF0;
		return;
	//case 0x28C:		// Wheel force feedback?
	default:
		break;
	}
	INFO_LOG(NAOMI, "Unhandled write @ %x (%d): %x", addr, size, data);
}

/*
	The game's devices besides its I/O board (naomi_periph.h)
*/
char naomi_set_name[32];
bool naomi_card_readers_enabled;
// L is the "Insert Card" button (libretro.cpp)
bool naomi_card_button;

static bool periph_rfid;
// what is read once each time the I/O board bridge is asked
static bool periph_polled;
// the "Insert Card" button of each player, as it was the last time
static u8 periph_card_button[2];

extern char eeprom_file[PATH_MAX];
extern std::vector<sched_list> sch_list;

static void periph_path(char *path, size_t size, const char *suffix)
{
	// the EEPROM's name, without its extension
	size_t len = strlen(eeprom_file);
	static const char ext[] = ".eeprom";
	if (len >= sizeof(ext) - 1 && !strcmp(eeprom_file + len - (sizeof(ext) - 1), ext))
		len -= sizeof(ext) - 1;
	snprintf(path, size, "%.*s%s", (int)len, eeprom_file, suffix);
}

size_t periph_load_file(const char *suffix, void *buf, size_t size)
{
	char path[PATH_MAX + 16];
	memset(buf, 0, size);
	if (eeprom_file[0] == '\0')
		return 0;
	periph_path(path, sizeof(path), suffix);
	RFILE *f = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (f == NULL)
		return 0;
	const int64_t n = filestream_read(f, buf, size);
	filestream_close(f);
	INFO_LOG(NAOMI, "Loaded %s", path);
	return n > 0 ? (size_t)n : 0;
}

void periph_save_file(const char *suffix, const void *buf, size_t size, retro_atomic_int_t *failed)
{
	char path[PATH_MAX + 16];
	if (eeprom_file[0] == '\0')
		return;
	if (retro_atomic_load_acquire_int(failed) && retro_atomic_exchange_int(failed, 0))
		WARN_LOG(NAOMI, "A file of the game's devices could not be saved");
	periph_path(path, sizeof(path), suffix);
	if (!save_writer_put_file(path, buf, size, failed))
		WARN_LOG(NAOMI, "Cannot save %s", path);
}

void periph_sched_restore(int id, s32 start, s32 end)
{
	if (id < 0)
		return;
	if (end != -1)
	{
		const u32 now = sh4_sched_now();
		const u32 asked = (u32)end - (u32)start;
		const u32 gone = now - (u32)start;
		if (asked > SH4_MAIN_CLOCK || gone > asked)
		{
			start = (s32)now;
			end = start == -1 ? 0 : start;
		}
	}
	sch_list[id].start = start;
	sch_list[id].end = end;
	sh4_sched_ffts();
}

static bool id_starts(const char *prefix)
{
	return !strncmp(naomi_game_id, prefix, strlen(prefix));
}

static bool set_starts(const char *prefix)
{
	return !strncmp(naomi_set_name, prefix, strlen(prefix));
}

void naomi_periph_term(void)
{
	card_reader::term();
	card_reader::barcodeTerm();
	touchscreen::term();
	hopper::term();
	printer::term();
	rfid_init(false, false);
	periph_rfid = false;
	periph_polled = false;
	naomi_card_button = false;
	memset(periph_card_button, 0, sizeof(periph_card_button));
}

/* The devices are the game's, by the name it gives itself (and, for two
 * whose versions differ, the set's). The card readers only with the core
 * option: a game that has none of its own plays without. */
void naomi_periph_init(void)
{
	naomi_periph_term();
	if (!SYSTEM_IS_NAOMI())
		return;
	// (what was saved last is in the files first)
	save_writer_drain();

	if (naomi_card_readers_enabled)
	{
		if (!strcmp(naomi_game_id, "INITIAL D")
				|| !strcmp(naomi_game_id, "INITIAL D Ver.2")
				|| !strcmp(naomi_game_id, "INITIAL D Ver.3")
				|| !strcmp(naomi_game_id, "INITIAL D CYCRAFT"))
			card_reader::initdInit();
		else if (!strcmp(naomi_game_id, "CLUB KART IN JAPAN")
				&& (!strcmp(naomi_set_name, "clubkrt") || !strcmp(naomi_set_name, "clubkrto")
					|| !strcmp(naomi_set_name, "clubkrta") || !strcmp(naomi_set_name, "clubkrtc")))
			card_reader::clubkInit();
		else if (!strcmp(naomi_game_id, " DERBY OWNERS CLUB WE ---------")
				|| !strcmp(naomi_game_id, " DERBY OWNERS CLUB ------------")
				|| !strcmp(naomi_game_id, " DERBY OWNERS CLUB II-----------"))
			card_reader::derbyInit();
		else if (id_starts("MKG TKOB")
				|| id_starts("MUSHIKING")
				|| !strcmp(naomi_game_id, "MUSHIUSA '04 1ST VER0.900-")
				|| id_starts("DINOSAUR KING")
				|| !strcmp(naomi_game_id, "INW PUPPY 2008 VER1.001")
				|| id_starts("LOVE AND BERRY")
				|| !strcmp(naomi_game_id, "BTR 2K9 VER 1.004"))
			card_reader::barcodeInit();

		if (id_starts("MKG TKOB")
				|| !strcmp(naomi_game_id, "VIRTUA FIGHTER 4 JAPAN")
				|| !strcmp(naomi_game_id, "VF4 EVOLUTION JAPAN")
				|| !strcmp(naomi_game_id, "VF4 FINAL TUNED JAPAN"))
		{
			periph_rfid = true;
			// Mushiking's cards are in from the start
			rfid_init(true, id_starts("MKG TKOB"));
		}
	}
	if (!strcmp(naomi_game_id, "POKASUKA GHOST (JAPANESE)")	// Manic Panic Ghosts
			|| !strcmp(naomi_game_id, "TOUCH DE ZUNO (JAPAN)"))
		touchscreen::init();
	if (!strcmp(naomi_game_id, " TOUCH DE UNOH -------------")
			|| !strcmp(naomi_game_id, " TOUCH DE UNOH 2 -----------")
			|| !strcmp(naomi_game_id, "MIRAI YOSOU STUDIO"))
		printer::init();
	if (!strcmp(naomi_set_name, "clubkprz") || set_starts("clubkpz")
			|| set_starts("shootpl")
			|| !strcmp(naomi_game_id, "KICK '4' CASH"))
		hopper::init();

	naomi_card_button = card_reader::readerAvailable();
	periph_polled = naomi_card_button || hopper::active();
}

bool naomi_periph_has_card_reader(void)
{
	return naomi_card_button;
}

bool naomi_periph_rfid_game(void)
{
	return periph_rfid;
}

void naomi_periph_poll(void)
{
	if (!periph_polled)
		return;
	if (naomi_card_button)
	{
		for (int i = 0; i < 2; i++)
		{
			const u8 down = (mcfg_ArcadeKcode(i) & NAOMI_INSERT_CARD_KEY) == 0;
			if (down && !periph_card_button[i])
				card_reader::insertCard(i);
			periph_card_button[i] = down;
		}
	}
	hopper::poll();
}

/* What the game's devices have, in a save state since V24, on a NAOMI: a
 * byte that says which there are (0 for most games, and then nothing
 * more), the players' "Insert Card" buttons, and then each one's:
 *   - the SCIF's FIFOs, for a device on it (scif_serialize_v24(), 45 bytes);
 *   - the card readers' (card_reader::serialize_v24(), 2 bytes and up);
 *   - the touch panel's (touchscreen::serialize_v24(), 60 bytes);
 *   - the hopper board's (hopper::serialize_v24(), 2404 bytes);
 *   - the IC cards' (rfid_serialize_v24(), 262 bytes).
 * One that the state has and the game has not is read past, and one the
 * game has and the state has not is as an older state has it. */
enum
{
	PERIPH_SCIF = 1,
	PERIPH_CARD = 2,
	PERIPH_TOUCH = 4,
	PERIPH_HOPPER = 8,
	PERIPH_RFID = 16,
};
static const u32 PERIPH_SCIF_SIZE = 45;
static const u32 PERIPH_TOUCH_SIZE = 60;
static const u32 PERIPH_HOPPER_SIZE = 2404;
static const u32 PERIPH_RFID_SIZE = 262;

static u8 periph_present(void)
{
	u8 mask = 0;
	if (scif_get_pipe() != NULL)
		mask |= PERIPH_SCIF;
	if (card_reader::readerAvailable() && !periph_rfid)
		mask |= PERIPH_CARD;
	if (card_reader::barcodeAvailable())
		mask |= PERIPH_CARD;
	if (touchscreen::active())
		mask |= PERIPH_TOUCH;
	if (hopper::active())
		mask |= PERIPH_HOPPER;
	if (periph_rfid)
		mask |= PERIPH_RFID;
	return mask;
}

static bool periph_skip(void **data, unsigned int *total_size, u32 size)
{
	u8 scratch[256];
	while (size > 0)
	{
		const u32 n = std::min<u32>(size, sizeof(scratch));
		if (!ra_unserialize(scratch, n, data, total_size))
			return false;
		size -= n;
	}
	return true;
}

void naomi_periph_serialize_v24(void **data, unsigned int *total_size)
{
	if (!SYSTEM_IS_NAOMI())
		return;
	const u8 mask = periph_present();
	LIBRETRO_S(mask);
	if (mask == 0)
		return;
	LIBRETRO_SA(periph_card_button, 2);
	if (mask & PERIPH_SCIF)
		scif_serialize_v24(data, total_size);
	if (mask & PERIPH_CARD)
		card_reader::serialize_v24(data, total_size);
	if (mask & PERIPH_TOUCH)
		touchscreen::serialize_v24(data, total_size);
	if (mask & PERIPH_HOPPER)
		hopper::serialize_v24(data, total_size);
	if (mask & PERIPH_RFID)
		rfid_serialize_v24(data, total_size);
}

bool naomi_periph_unserialize_v24(void **data, unsigned int *total_size)
{
	if (!SYSTEM_IS_NAOMI())
		return true;
	const u8 have = periph_present();
	u8 mask = 0;
	if (!LIBRETRO_US(mask))
		return false;
	if (mask & ~(PERIPH_SCIF | PERIPH_CARD | PERIPH_TOUCH | PERIPH_HOPPER | PERIPH_RFID))
		return false;
	if (mask == 0)
	{
		naomi_periph_state_before_v24();
		return true;
	}
	LIBRETRO_USA(periph_card_button, 2);
	periph_card_button[0] = periph_card_button[0] != 0;
	periph_card_button[1] = periph_card_button[1] != 0;

	bool ok = true;
	if (mask & PERIPH_SCIF)
		ok = (have & PERIPH_SCIF) ? scif_unserialize_v24(data, total_size) : periph_skip(data, total_size, PERIPH_SCIF_SIZE);
	else if (have & PERIPH_SCIF)
		scif_state_before_v24();
	// (the readers' part says which it is, and reads past a reader the game has not got)
	if (ok && (mask & PERIPH_CARD))
		ok = card_reader::unserialize_v24(data, total_size);
	else if (have & PERIPH_CARD)
		card_reader::state_before_v24();
	if (ok && (mask & PERIPH_TOUCH))
		ok = (have & PERIPH_TOUCH) ? touchscreen::unserialize_v24(data, total_size) : periph_skip(data, total_size, PERIPH_TOUCH_SIZE);
	else if (have & PERIPH_TOUCH)
		touchscreen::state_before_v24();
	if (ok && (mask & PERIPH_HOPPER))
		ok = (have & PERIPH_HOPPER) ? hopper::unserialize_v24(data, total_size) : periph_skip(data, total_size, PERIPH_HOPPER_SIZE);
	else if (have & PERIPH_HOPPER)
		hopper::state_before_v24();
	if (ok && (mask & PERIPH_RFID))
		ok = (have & PERIPH_RFID) ? rfid_unserialize_v24(data, total_size) : periph_skip(data, total_size, PERIPH_RFID_SIZE);
	else if (have & PERIPH_RFID)
		rfid_state_before_v24();
	return ok;
}

/* An older state: none of the devices had anything on its way or a card
 * in it, and no button was held, which is what the game saw when it had
 * none of them. */
void naomi_periph_state_before_v24(void)
{
	if (!SYSTEM_IS_NAOMI())
		return;
	memset(periph_card_button, 0, sizeof(periph_card_button));
	if (scif_get_pipe() != NULL)
		scif_state_before_v24();
	card_reader::state_before_v24();
	touchscreen::state_before_v24();
	hopper::state_before_v24();
	if (periph_rfid)
		rfid_state_before_v24();
}
