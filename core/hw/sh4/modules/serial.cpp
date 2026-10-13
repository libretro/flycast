/*
	Dreamcast serial port.
	This is missing most of the functionality, but works for KOS (And thats all that uses it)
*/
#include "types.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/naomi/naomi_periph.h"
#include "modules.h"
#include <algorithm>
#include <vector>

extern std::vector<sched_list> sch_list;

SCIF_SCFSR2_type SCIF_SCFSR2;
SCIF_SCSCR2_type SCIF_SCSCR2;

/*
//SCIF SCSMR2 0xFFE80000 0x1FE80000 16 0x0000 0x0000 Held Held Pclk
SCSMR2_type SCIF_SCSMR2;

//SCIF SCBRR2 0xFFE80004 0x1FE80004 8 0xFF 0xFF Held Held Pclk
u8 SCIF_SCBRR2;

//SCIF SCSCR2 0xFFE80008 0x1FE80008 16 0x0000 0x0000 Held Held Pclk
SCSCR2_type SCIF_SCSCR2;

//SCIF SCFTDR2 0xFFE8000C 0x1FE8000C 8 Undefined Undefined Held Held Pclk
u8 SCIF_SCFTDR2;

//SCIF SCFSR2 0xFFE80010 0x1FE80010 16 0x0060 0x0060 Held Held Pclk
SCSCR2_type SCIF_SCFSR2;

//SCIF SCFRDR2 0xFFE80014 0x1FE80014 8 Undefined Undefined Held Held Pclk
//Read OLNY
u8 SCIF_SCFRDR2;

//SCIF SCFCR2 0xFFE80018 0x1FE80018 16 0x0000 0x0000 Held Held Pclk
SCFCR2_type SCIF_SCFCR2;

//Read OLNY
//SCIF SCFDR2 0xFFE8001C 0x1FE8001C 16 0x0000 0x0000 Held Held Pclk
SCFDR2_type SCIF_SCFDR2;

//SCIF SCSPTR2 0xFFE80020 0x1FE80020 16 0x0000 0x0000 Held Held Pclk
SCSPTR2_type SCIF_SCSPTR2;

//SCIF SCLSR2 0xFFE80024 0x1FE80024 16 0x0000 0x0000 Held Held Pclk
SCLSR2_type SCIF_SCLSR2;
*/

static void Serial_UpdateInterrupts()
{
    InterruptPend(sh4_SCIF_TXI, SCIF_SCFSR2.TDFE);
    InterruptMask(sh4_SCIF_TXI, SCIF_SCSCR2.TIE);

    InterruptPend(sh4_SCIF_RXI, SCIF_SCFSR2.RDF);
    InterruptMask(sh4_SCIF_RXI, SCIF_SCSCR2.RIE);
}

/*
	With a device plugged in (an arcade game's card reader, touch panel or
	hopper: see SerialPort::Pipe) the port has its 16-byte FIFOs, sends and
	receives a character each character time, and raises its interrupts as
	the FIFOs fill and empty. With nothing plugged in it is what it has
	always been here: always ready to send, never anything received.
*/
static SerialPort::Pipe *scif_pipe;
static int scif_schid = -1;
static u8 scif_txf[16];
static u8 scif_rxf[16];
static u32 scif_txn;
static u32 scif_rxn;
static bool scif_transmitting;
// the status bits as the game last read them: only those can be cleared
static u16 scif_status_read;

enum
{
	SCIF_DR = 0x01,
	SCIF_RDF = 0x02,
	SCIF_PER = 0x04,
	SCIF_FER = 0x08,
	SCIF_BRK = 0x10,
	SCIF_TDFE = 0x20,
	SCIF_TEND = 0x40,
	SCIF_ER = 0x80,
};

static void scif_interrupts()
{
	InterruptPend(sh4_SCIF_TXI, SCIF_SCFSR2.TDFE);
	InterruptMask(sh4_SCIF_TXI, SCIF_SCSCR2.TIE);

	InterruptPend(sh4_SCIF_RXI, SCIF_SCFSR2.RDF || SCIF_SCFSR2.DR);
	InterruptMask(sh4_SCIF_RXI, SCIF_SCSCR2.RIE);

	InterruptPend(sh4_SCIF_BRI, SCIF_SCFSR2.BRK);
	InterruptMask(sh4_SCIF_BRI, SCIF_SCSCR2.RIE || SCIF_SCSCR2.REIE);

	InterruptPend(sh4_SCIF_ERI, SCIF_SCFSR2.ER || SCIF_SCFSR2.FER || SCIF_SCFSR2.PER);
	InterruptMask(sh4_SCIF_ERI, SCIF_SCSCR2.RIE || SCIF_SCSCR2.REIE);
}

// The time a character takes on the line, in SH4 cycles
static int scif_char_cycles()
{
	const u16 smr = SCIF_SCSMR2.full;
	// start bit, 7 or 8 data bits, parity bit if any, 1 or 2 stop bits
	const int frame = 1 + 8 - ((smr >> 6) & 1) + ((smr >> 5) & 1) + 1 + ((smr >> 3) & 1);
	// 95 bauds at the slowest
	const int bauds = SH4_MAIN_CLOCK / 4 / (SCIF_SCBRR2 + 1) / 32 / (1 << ((smr & 3) * 2));
	return frame * (SH4_MAIN_CLOCK / bauds);
}

static bool scif_tdfe()
{
	return scif_txn <= (1u << (3 - ((SCIF_SCFCR2.full >> 4) & 3)));
}

static bool scif_rdf()
{
	static const u32 levels[] = { 1, 4, 8, 14 };
	return scif_rxn >= levels[(SCIF_SCFCR2.full >> 6) & 3];
}

static void scif_set_status(u16 bit)
{
	scif_status_read &= ~bit;
	SCIF_SCFSR2.full |= bit;
}

// The character being sent is out: the next one from the FIFO goes
static void scif_tx_done()
{
	if (!scif_transmitting || SCIF_SCFCR2.TFRST)
		return;
	if (scif_txn == 0)
	{
		scif_set_status(SCIF_TEND);
		scif_transmitting = false;
		return;
	}
	const u8 v = scif_txf[0];
	scif_txn--;
	memmove(scif_txf, scif_txf + 1, scif_txn);
	scif_pipe->write(v);
	if (scif_tdfe())
	{
		scif_set_status(SCIF_TDFE);
		scif_interrupts();
	}
}

// A character from the device, if it has one
static void scif_rx()
{
	if (scif_pipe->available() > 0)
	{
		const u8 v = scif_pipe->read();
		if (!SCIF_SCSCR2.RE || SCIF_SCFCR2.RFRST)
			return;
		if (scif_rxn == sizeof(scif_rxf))
		{
			SCIF_SCLSR2.ORER = 1;
			scif_interrupts();
			INFO_LOG(SH4, "scif: Receive overrun");
		}
		else
		{
			scif_rxf[scif_rxn++] = v;
			if (scif_rdf())
			{
				scif_set_status(SCIF_RDF);
				scif_interrupts();
			}
		}
	}
	else if (scif_rxn != 0)
	{
		// nothing more has come for a character's time
		scif_set_status(SCIF_DR);
		scif_interrupts();
	}
}

/* Whether there is anything for the next character time: a character
 * going out, one the device has for the game, or the wait after the last
 * one that came. The port is not looked at otherwise: a device that has
 * something new to send says so (scif_kick()). */
static bool scif_busy()
{
	return scif_transmitting || scif_pipe->available() > 0
		|| (scif_rxn != 0 && !SCIF_SCFSR2.DR);
}

static int scif_sched(int tag, int cycles, int jitter)
{
	if (scif_pipe == NULL)
		return 0;
	scif_tx_done();
	scif_rx();
	return scif_busy() ? scif_char_cycles() : 0;
}

static void scif_kick()
{
	if (scif_pipe != NULL && !sh4_sched_is_scheduled(scif_schid))
		sh4_sched_request(scif_schid, scif_char_cycles());
}

static void scif_clear()
{
	scif_txn = 0;
	scif_rxn = 0;
	scif_transmitting = false;
	scif_status_read = 0;
	if (scif_schid >= 0)
		sh4_sched_request(scif_schid, -1);
}

void scif_set_pipe(SerialPort::Pipe *pipe)
{
	if (pipe == scif_pipe)
		return;
	// (registered once a game has a device: the scheduler has one event less to look at for every other)
	if (pipe != NULL && scif_schid < 0)
		scif_schid = sh4_sched_register(0, &scif_sched);
	scif_clear();
	scif_pipe = pipe;
}

SerialPort::Pipe *scif_get_pipe()
{
	return scif_pipe;
}

void scif_update_status()
{
	scif_kick();
}

void SCIFSerialPort::setPipe(Pipe *pipe)
{
	scif_set_pipe(pipe);
}

void SCIFSerialPort::updateStatus()
{
	scif_kick();
}

SCIFSerialPort& SCIFSerialPort::Instance()
{
	static SCIFSerialPort instance;
	return instance;
}

static void scif_pipe_write(u8 data)
{
	if (SCIF_SCFCR2.TFRST)
		return;
	if (SCIF_SCSMR2.CHR)
		data &= 0x7f;
	if (scif_txn == 0 && !scif_transmitting && SCIF_SCSCR2.TE)
	{
		scif_pipe->write(data);
		scif_transmitting = true;
		sh4_sched_request(scif_schid, scif_char_cycles());
		// straight into the shift register: the FIFO is still empty
		scif_set_status(SCIF_TDFE);
		scif_interrupts();
	}
	else if (scif_txn < sizeof(scif_txf))
		scif_txf[scif_txn++] = data;
}

static void scif_pipe_write_status(u32 value)
{
	u16 data = (u16)(value | ~0x00f3u | ~(u32)scif_status_read);
	// RDF and TDFE stay set as long as the FIFO is past its trigger level
	if (scif_rdf())
		data |= SCIF_RDF;
	if (scif_tdfe())
		data |= SCIF_TDFE;
	if (scif_rxn != 0)
		data |= SCIF_DR;
	SCIF_SCFSR2.full &= data;
	scif_status_read &= data;
	scif_interrupts();
}

static u8 scif_pipe_read_data()
{
	if (scif_rxn == 0)
		return 0;
	const u8 data = scif_rxf[0];
	scif_rxn--;
	memmove(scif_rxf, scif_rxf + 1, scif_rxn);
	return data;
}

/* What a device's port has that the registers do not (V24): the two
 * FIFOs, whether a character is going out, the status bits the game has
 * read, and when the next character time is. Written only for a game that
 * has a device on the port. 45 bytes. */
void scif_serialize_v24(void **data, unsigned int *total_size)
{
	u8 counts[3] = { (u8)scif_txn, (u8)scif_rxn, (u8)scif_transmitting };
	s32 start = -1, end = -1;

	if (scif_schid >= 0)
	{
		start = sch_list[scif_schid].start;
		end = sch_list[scif_schid].end;
	}
	LIBRETRO_SA(scif_txf, sizeof(scif_txf));
	LIBRETRO_SA(scif_rxf, sizeof(scif_rxf));
	LIBRETRO_SA(counts, 3);
	LIBRETRO_S(scif_status_read);
	LIBRETRO_S(start);
	LIBRETRO_S(end);
}

bool scif_unserialize_v24(void **data, unsigned int *total_size)
{
	u8 counts[3] = { 0, 0, 0 };
	s32 start = -1, end = -1;
	u16 status_read = 0;

	LIBRETRO_USA(scif_txf, sizeof(scif_txf));
	LIBRETRO_USA(scif_rxf, sizeof(scif_rxf));
	LIBRETRO_USA(counts, 3);
	LIBRETRO_US(status_read);
	LIBRETRO_US(start);
	if (!LIBRETRO_US(end))
		return false;
	scif_txn = std::min<u32>(counts[0], sizeof(scif_txf));
	scif_rxn = std::min<u32>(counts[1], sizeof(scif_rxf));
	scif_transmitting = counts[2] != 0;
	scif_status_read = status_read & 0xff;
	periph_sched_restore(scif_schid, start, end);
	return true;
}

/* An older state, of a game with a device on the port: the FIFOs are
 * empty and nothing is going out, which is what the port that had none
 * looked like. */
void scif_state_before_v24(void)
{
	scif_clear();
}

static void SerialWrite(u32 addr, u32 data)
{
	if (scif_pipe != NULL)
	{
		scif_pipe_write((u8)data);
		return;
	}
	SCIF_SCFSR2.TDFE = 1;
	SCIF_SCFSR2.TEND = 1;

	Serial_UpdateInterrupts();
}

//SCIF_SCFSR2 read
static u32 ReadSerialStatus(u32 addr)
{
	if (scif_pipe != NULL)
	{
		scif_status_read = SCIF_SCFSR2.full;
		return SCIF_SCFSR2.full;
	}
	if (false /*PendingSerialData()*/)
	{
		return SCIF_SCFSR2.full | 2;
	}
	else
	{
		return SCIF_SCFSR2.full | 0;
	}
}

static void WriteSerialStatus(u32 addr,u32 data)
{
	if (scif_pipe != NULL)
	{
		scif_pipe_write_status(data);
		return;
	}
	if (!SCIF_SCFSR2.BRK)
		data &= ~0x10;

	SCIF_SCFSR2.full = data & ~3;

	SCIF_SCFSR2.TDFE = 1;
	SCIF_SCFSR2.TEND = 1;

	Serial_UpdateInterrupts();
}

//SCIF_SCFDR2 - 16b
static u32 Read_SCFDR2(u32 addr)
{
	if (scif_pipe != NULL)
		return scif_rxn | (scif_txn << 8);
	return 0;
}
//SCIF_SCFRDR2
static u32 ReadSerialData(u32 addr)
{
	if (scif_pipe != NULL)
		return scif_pipe_read_data();
	s32 rd=0;//ReadSerial();
	return (u8)rd ;
}

//SCSCR2

static u32 SCSCR2_read(u32 addr)
{
	return SCIF_SCSCR2.full;
}

static void SCSCR2_write(u32 addr, u32 data)
{
	if (scif_pipe != NULL)
	{
		SCIF_SCSCR2.full = data & 0x00fa;
		if (!SCIF_SCSCR2.TE)
			scif_set_status(SCIF_TEND);
		scif_interrupts();
		return;
	}
	SCIF_SCSCR2.full = data;

	Serial_UpdateInterrupts();
}

//SCFCR2
static void SCFCR2_write(u32 addr, u32 data)
{
	if (scif_pipe == NULL)
	{
		SCIF_SCFCR2.full = (u16)data;
		return;
	}
	if (SCIF_SCFCR2.TFRST && !(data & 4))
	{
		// the transmit FIFO out of its reset
		scif_set_status(SCIF_TEND);
		scif_set_status(SCIF_TDFE);
		scif_interrupts();
	}
	SCIF_SCFCR2.full = data & 0x00ff;
	if (SCIF_SCFCR2.TFRST)
	{
		scif_txn = 0;
		scif_transmitting = false;
	}
	if (SCIF_SCFCR2.RFRST)
		scif_rxn = 0;
}

//Init term res
void serial_init()
{
	//SCIF SCSMR2 0xFFE80000 0x1FE80000 16 0x0000 0x0000 Held Held Pclk
	sh4_rio_reg(SCIF,SCIF_SCSMR2_addr,RIO_DATA,16);

	//SCIF SCBRR2 0xFFE80004 0x1FE80004 8 0xFF 0xFF Held Held Pclk
	sh4_rio_reg(SCIF,SCIF_SCBRR2_addr,RIO_DATA,8);

	//SCIF SCSCR2 0xFFE80008 0x1FE80008 16 0x0000 0x0000 Held Held Pclk
	sh4_rio_reg(SCIF, SCIF_SCSCR2_addr, RIO_FUNC, 16, &SCSCR2_read, &SCSCR2_write);

	//Write only 
	//SCIF SCFTDR2 0xFFE8000C 0x1FE8000C 8 Undefined Undefined Held Held Pclk
	sh4_rio_reg(SCIF,SCIF_SCFTDR2_addr,RIO_WF,8,0,&SerialWrite);

	//SCIF SCFSR2 0xFFE80010 0x1FE80010 16 0x0060 0x0060 Held Held Pclk
	sh4_rio_reg(SCIF,SCIF_SCFSR2_addr,RIO_FUNC,16,&ReadSerialStatus,&WriteSerialStatus);

	//READ only
	//SCIF SCFRDR2 0xFFE80014 0x1FE80014 8 Undefined Undefined Held Held Pclk
	sh4_rio_reg(SCIF,SCIF_SCFRDR2_addr,RIO_RO_FUNC,8,&ReadSerialData);

	//SCIF SCFCR2 0xFFE80018 0x1FE80018 16 0x0000 0x0000 Held Held Pclk
	sh4_rio_reg(SCIF,SCIF_SCFCR2_addr,RIO_WF,16,0,&SCFCR2_write);

	//Read only
	//SCIF SCFDR2 0xFFE8001C 0x1FE8001C 16 0x0000 0x0000 Held Held Pclk
	sh4_rio_reg(SCIF,SCIF_SCFDR2_addr,RIO_RO_FUNC,16,&Read_SCFDR2);

	//SCIF SCSPTR2 0xFFE80020 0x1FE80020 16 0x0000 0x0000 Held Held Pclk
	sh4_rio_reg(SCIF,SCIF_SCSPTR2_addr,RIO_DATA,16);

	//SCIF SCLSR2 0xFFE80024 0x1FE80024 16 0x0000 0x0000 Held Held Pclk
	sh4_rio_reg(SCIF,SCIF_SCLSR2_addr,RIO_DATA,16);
}
void serial_reset()
{
	/*
	SCIF SCSMR2 H'FFE8 0000 H'1FE8 0000 16 H'0000 H'0000 Held Held Pclk
	SCIF SCBRR2 H'FFE8 0004 H'1FE8 0004 8 H'FF H'FF Held Held Pclk
	SCIF SCSCR2 H'FFE8 0008 H'1FE8 0008 16 H'0000 H'0000 Held Held Pclk
	SCIF SCFTDR2 H'FFE8 000C H'1FE8 000C 8 Undefined Undefined Held Held Pclk
	SCIF SCFSR2 H'FFE8 0010 H'1FE8 0010 16 H'0060 H'0060 Held Held Pclk
	SCIF SCFRDR2 H'FFE8 0014 H'1FE8 0014 8 Undefined Undefined Held Held Pclk
	SCIF SCFCR2 H'FFE8 0018 H'1FE8 0018 16 H'0000 H'0000 Held Held Pclk
	SCIF SCFDR2 H'FFE8 001C H'1FE8 001C 16 H'0000 H'0000 Held Held Pclk
	SCIF SCSPTR2 H'FFE8 0020 H'1FE8 0020 16 H'0000*2 H'0000*2 Held Held Pclk
	SCIF SCLSR2 H'FFE8 0024 H'1FE8 0024 16 H'0000 H'0000 Held Held Pclk
	*/
	SCIF_SCSMR2.full=0x0000;
	SCIF_SCBRR2=0xFF;
	SCIF_SCFSR2.full=0x060;
	SCIF_SCFCR2.full=0x000;
	SCIF_SCSPTR2.full=0x000;
	SCIF_SCLSR2.full=0x000;
	SCIF_SCSCR2.full = 0;
	// (a device stays plugged in)
	scif_clear();
}

void serial_term()
{
}

