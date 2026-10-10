/*
	gdrom, v3
	Overly complex implementation of a very ugly device
*/

#include "gdromv3.h"
#include "hw/mem/_vmem.h"

#include "types.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/modules/dmac.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_interpreter.h"

#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"

#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_sched.h"

int gdrom_sched;

//Sense: ASC - ASCQ - Key
signed int sns_asc=0;
signed int sns_ascq=0;
signed int sns_key=0;

read_params_t read_params ;
packet_cmd_t packet_cmd;

//Buffer for sector reads [dma]
read_buff_t read_buff ;

//pio buffer
pio_buff_t pio_buff ;

u32 set_mode_offset;
ata_cmd_t ata_cmd ;
cdda_t cdda ;

gd_states gd_state;
DiscType gd_disk_type;
/*
	GD rom reset -> GDS_WAITCMD

	GDS_WAITCMD -> ATA/SPI command [Command code is on ata_cmd]
	SPI Command -> GDS_WAITPACKET -> GDS_SPI_* , depending on input

	GDS_SPI_READSECTOR -> Depending on features , it can do quite a few things
*/
u32 data_write_mode=0;

//Registers
	u32 DriveSel;
	GD_ErrRegT Error;
	GD_InterruptReasonT IntReason;
	GD_FeaturesT Features;
	GD_SecCountT SecCount;
	GD_SecNumbT SecNumber;
	
	GD_StatusT GDStatus;

	
ByteCount_t ByteCount ;
//end
GD_HardwareInfo_t GD_HardwareInfo;

#define printf_rm(...)  DEBUG_LOG(GDROM, __VA_ARGS__)
#define printf_ata(...)  DEBUG_LOG(GDROM, __VA_ARGS__)
#define printf_spi(...)  DEBUG_LOG(GDROM, __VA_ARGS__)
#define printf_spicmd(...)  DEBUG_LOG(GDROM, __VA_ARGS__)
#define printf_subcode(...)  DEBUG_LOG(GDROM, __VA_ARGS__)

const s16* libCore_CDDA_Sector(s16* sector)
{
	const s16* out = sector;

	//silence ! :p
	if (cdda.status == cdda_t::Playing)
	{
		/* The end of the disc. Playing stops there, whatever end the game
		 * gave, and the drive stays on the last sector there was: a game
		 * that waits for the music to be over would otherwise wait while
		 * silence is played up to an end that is not on the disc. */
		const u32 lead_out = libGDR_LeadOutFAD();
		if (lead_out != 0 && cdda.CurrAddr.FAD >= lead_out)
		{
			if (cdda.CurrAddr.FAD != 0)
				cdda.CurrAddr.FAD--;
			cdda.status = cdda_t::Terminated;
			SecNumber.Status = GD_PAUSE;
			memset(sector, 0, 2352);
			return out;
		}
		const u8* lent = libGDR_LendRawSector(cdda.CurrAddr.FAD);
		if (lent)
			out = (const s16*)lent;
		else
			libGDR_ReadSector((u8*)sector,cdda.CurrAddr.FAD,1,2352);
		cdda.CurrAddr.FAD++;
		if (cdda.CurrAddr.FAD >= cdda.EndAddr.FAD)
		{
			if (cdda.repeats==0)
			{
				//stop
				cdda.status = cdda_t::Terminated;
				SecNumber.Status = GD_PAUSE;
			}
			else
			{
				//Repeat ;)
				if (cdda.repeats!=0xf)
					cdda.repeats--;

				cdda.CurrAddr.FAD=cdda.StartAddr.FAD;
			}
		}
	}
	else
	{
		memset(sector,0,2352);
	}
	return out;
}
void gd_spi_pio_end(const u8* buffer, u32 len, gd_states next_state = gds_pio_end);
void gd_process_spi_cmd();
void gd_process_ata_cmd();

/* The cache only ever holds the sector a transfer consumed part of:
 * whole sectors go from the image to RAM without it. */
static void FillReadBuffer(void)
{
	read_buff.cache_index=0;
	u32 count = read_params.remaining_sectors;

	if (count > 1)
		count = 1;

	read_buff.cache_size=count*read_params.sector_type;

	libGDR_ReadSector(read_buff.cache,read_params.start_sector,count,read_params.sector_type);
	read_params.start_sector+=count;
	read_params.remaining_sectors-=count;
}

void gd_set_state(gd_states state)
{
	gd_states prev=gd_state;
	gd_state=state;
	switch(state)
	{
		case gds_waitcmd:
			GDStatus.DRDY=1;   // Can accept ATA command :)
			GDStatus.BSY=0;    // Does not access command block
			break;

		case gds_procata:

			GDStatus.DRDY=0;   // Can't accept ATA command
			GDStatus.BSY=1;    // Accessing command block to process command
			gd_process_ata_cmd();
			break;

		case gds_waitpacket:

			// Prepare for packet command
			packet_cmd.index=0;

			// Set CoD, clear BSY and IO
			IntReason.CoD=1;
			GDStatus.BSY = 0;
			IntReason.IO=0;

			// Make DRQ valid
			GDStatus.DRQ = 1;

			// ATA can optionally raise the interrupt ...
			// RaiseInterrupt(holly_GDROM_CMD);
			break;

		case gds_procpacket:

			GDStatus.DRQ=0;     // Can't accept ATA command
			GDStatus.BSY=1;     // Accessing command block to process command
			gd_process_spi_cmd();
			break;
			//yep , get/set are the same !
		case gds_pio_get_data:
		case gds_pio_send_data:
			//  When preparations are complete, the following steps are carried out at the device.
			//(1)   Number of bytes to be read is set in "Byte Count" register. 
			ByteCount.full =(u16)(pio_buff.size<<1);
			//(2)   IO bit is set and CoD bit is cleared. 
			IntReason.IO=1;
			IntReason.CoD=0;
			//(3)   DRQ bit is set, BSY bit is cleared. 
			GDStatus.DRQ=1;
			GDStatus.BSY=0;
			//(4)   INTRQ is set, and a host interrupt is issued.
			asic_RaiseInterrupt(holly_GDROM_CMD);
			/*
			The number of bytes normally is the byte number in the register at the time of receiving 
			the command, but it may also be the total of several devices handled by the buffer at that point.
			*/
			break;

		case gds_readsector_pio:
			{
				/*
				If more data are to be sent, the device sets the BSY bit and repeats the above sequence 
				from step 7. 
				*/
				GDStatus.BSY=1;

				u32 sector_count = read_params.remaining_sectors;
				gd_states next_state=gds_pio_end;

				if (sector_count > 27)
				{
					sector_count = 27;
					next_state = gds_readsector_pio;
				}

				libGDR_ReadSector((u8*)&pio_buff.data[0],read_params.start_sector,sector_count, read_params.sector_type);
				read_params.start_sector+=sector_count;
				read_params.remaining_sectors-=sector_count;

				gd_spi_pio_end(0,sector_count*read_params.sector_type,next_state);
			}
			break;
			
		case gds_readsector_dma:
			break;

		case gds_pio_end:
			
			GDStatus.DRQ=0;//all data is sent !

			gd_set_state(gds_procpacketdone);
			break;

		case gds_procpacketdone:
			/*
			7.  When the device is ready to send the status, it writes the 
			final status (IO, CoD, DRDY set, BSY, DRQ cleared) to the "Status" register before making INTRQ valid. 
			After checking INTRQ, the host reads the "Status" register to check the completion status. 
			*/
			//Set IO, CoD, DRDY
			GDStatus.DRDY=1;
			IntReason.CoD=1;
			IntReason.IO=1;

			//Clear DRQ,BSY
			GDStatus.DRQ=0;
			GDStatus.BSY=0;
			//Make INTRQ valid
			asic_RaiseInterrupt(holly_GDROM_CMD);

			//command finished !
			gd_set_state(gds_waitcmd);
			break;

		case gds_process_set_mode:
			{
				/* As many bytes as the command said, which is one less
				 * than the words hold when that is an odd number, and no
				 * further than the ten that can be set. */
				u32 count = std::min((u32)packet_cmd.data_8[4], pio_buff.size << 1);
				if (set_mode_offset >= 10)
					count = 0;
				else if (count > 10 - set_mode_offset)
					count = 10 - set_mode_offset;
				if (count != 0)
					memcpy((u8 *)&GD_HardwareInfo + set_mode_offset, pio_buff.data, count);
			}
			//end pio transfer ;)
			gd_set_state(gds_pio_end);
			break;

		default :
			die("Unhandled GDROM state ...");
			break;
	}
}


void gd_setdisc()
{
	cdda.status = cdda_t::NoInfo;

	DiscType newd = (DiscType)libGDR_GetDiscType();
	
	switch(newd)
	{
	case NoDisk:
		SecNumber.Status = GD_NODISC;
		//GDStatus.BSY=0;
		//GDStatus.DRDY=1;
		break;

	case Open:
		SecNumber.Status = GD_OPEN;
		//GDStatus.BSY=0;
		GDStatus.DRDY=1;
		break;

	case Busy:
		SecNumber.Status = GD_BUSY;
		GDStatus.BSY=1;
		GDStatus.DRDY=0;
		break;

	default :
		if (SecNumber.Status==GD_BUSY)
			SecNumber.Status = GD_PAUSE;
		else
			SecNumber.Status = GD_STANDBY;
		//GDStatus.BSY=0;
		//GDStatus.DRDY=1;
		break;
	}

	if (gd_disk_type==Busy && newd!=Busy)
	{
		GDStatus.BSY=0;
		GDStatus.DRDY=1;
	}

	gd_disk_type=newd;

	SecNumber.DiscFormat=gd_disk_type>>4;
}

/* A disc going in.
 *
 * The drive does not know what it has been given the moment the lid shuts:
 * it spins the disc up and reads its table of contents, and for that long
 * it says it is busy, becoming ready. Then it says, once, that the medium
 * may have changed, and from then on it has a disc of whatever kind it
 * found. Games that have the player change discs wait for exactly this,
 * and some of them do not notice a disc that is simply there at once:
 * upstream names Skies of Arcadia, Shenmue II, Alone in the Dark and
 * Dancing Blade among those its one-second delay fixed.
 *
 * For that second libGDR_GetDiscType() goes on answering as it did before
 * the disc went in. */
int gd_swap_schid = -1;

static int gd_swap_sched(int tag, int cycl, int jitter)
{
	if (libGDR_DiscPresent())
		sns_asc = 0x28;		// not ready to ready: the medium may have changed
	else
		sns_asc = 0x29;		// nothing there to read
	sns_ascq = 0;
	sns_key = 6;				// unit attention
	gd_setdisc();
	return 0;
}

bool gd_swap_pending()
{
	return gd_swap_schid >= 0 && sh4_sched_is_scheduled(gd_swap_schid);
}

// The lid is opened again, or the disc is taken away, before the drive is done looking
void gd_swap_cancel()
{
	if (gd_swap_pending())
		sh4_sched_request(gd_swap_schid, -1);
}

void gd_disc_inserted()
{
	read_params = { 0 };
	set_mode_offset = 0;
	packet_cmd = { 0 };
	memset(&read_buff, 0, sizeof(read_buff));
	pio_buff = { gds_waitcmd, 0 };
	ata_cmd = { 0 };
	cdda = { cdda_t::NoInfo, 0 };

	sns_asc = 4;				// in the process of becoming ready
	sns_ascq = 1;
	sns_key = 2;				// not ready
	SecNumber.Status = GD_BUSY;
	sh4_sched_request(gd_swap_schid, SH4_MAIN_CLOCK);	// 1 s
}

void gd_reset()
{
	//Reset the drive
	gd_setdisc();
	gd_set_state(gds_waitcmd);
}

static u32 GetFAD(u8* data, bool msf)
{
	if(msf)
		return data[0] * 60 * 75 + data[1] * 75 + data[2];
	else
		return (data[0] << 16) | (data[1] << 8) | data[2];
}

//disk changes etc
void libCore_gdrom_disc_change()
{
	gd_setdisc();
	read_params = { 0 };
	set_mode_offset = 0;
	packet_cmd = { 0 };
	memset(&read_buff, 0, sizeof(read_buff));
	pio_buff = { gds_waitcmd, 0 };
	ata_cmd = { 0 };
	cdda = { cdda_t::NoInfo, 0 };
}

//This handles the work of setting up the pio regs/state :)
void gd_spi_pio_end(const u8* buffer, u32 len, gd_states next_state)
{
	pio_buff.index=0;
	/* The data register is 16 bits wide: a reply of an odd number of bytes
	 * goes out in one word more, whose high byte is padding. (The odd byte
	 * was dropped, and a reply of one byte was no words at all: the drive
	 * offered data and had none to give, so the command never ended.) */
	pio_buff.size=(len+1)>>1;
	pio_buff.next_state=next_state;

	if (buffer!=0)
		memcpy(pio_buff.data,buffer,len);
	if (len&1)
		((u8 *)pio_buff.data)[len]=0;

	if (len==0)
		gd_set_state(next_state);
	else
		gd_set_state(gds_pio_send_data);
}
/* A reply of which the program says how much it wants and, for some, from
 * how far into it. It gets as much as it asks for: what there is of the
 * reply from there, and zeroes after that. (The numbers are the program's
 * own, a byte or two of its command, and went straight into a copy out of
 * ten bytes on the stack, or thirty-two of a variable - whatever came
 * after those in memory was sent with them.) */
static void gd_spi_pio_reply(const u8 *reply, u32 size, u32 offset, u32 len)
{
	u32 have = offset < size ? size - offset : 0;

	if (len > sizeof(pio_buff.data))
		len = sizeof(pio_buff.data);
	if (have > len)
		have = len;
	if (have != 0)
		memcpy(pio_buff.data, reply + offset, have);
	memset((u8 *)pio_buff.data + have, 0, len - have);
	gd_spi_pio_end(NULL, len);
}

void gd_spi_pio_read_end(u32 len, gd_states next_state)
{
	pio_buff.index=0;
	// (an odd number of bytes comes in one word more: see gd_spi_pio_end)
	pio_buff.size=(len+1)>>1;
	pio_buff.next_state=next_state;

	/* Counted in words: with none of them nothing is waited for. (It was
	 * waited for: the count of words taken never came to equal the none
	 * there were, and the game could go on sending until they were written
	 * past the end of the buffer.) */
	if (pio_buff.size==0)
		gd_set_state(next_state);
	else
		gd_set_state(gds_pio_get_data);
}
void gd_process_ata_cmd()
{
	//Any ATA command clears these bits, unless aborted/error :p
	Error.ABRT=0;

	/* (Unit attention - a disc was changed - is for the packet command that
	 * comes next to report, which it does: see gd_process_spi_cmd. An ATA
	 * command is not failed by it.) */
	if (sns_key == 0x0 			// No sense
			|| sns_key == 0xB	// Aborted
			|| sns_key == 6)	// Unit attention
		GDStatus.CHECK=0;
	else
		GDStatus.CHECK=1;

	switch(ata_cmd.command)
	{
	case ATA_NOP:
		printf_ata("ATA_NOP");
		/*
			Setting "abort" in the error register 
			Setting an error in the status register 
			Clearing "busy" in the status register 
			Asserting the INTRQ signal
		*/
		Error.ABRT = 1;
		Error.Sense = sns_key;
		GDStatus.BSY = 0;
		GDStatus.CHECK = 1;
		// no data is on offer after this: Sentimental Graffiti 2 waits on it when the disc is changed
		GDStatus.DRQ = 0;

		asic_RaiseInterrupt(holly_GDROM_CMD);
		gd_set_state(gds_waitcmd);
		break;

	case ATA_SOFT_RESET:
		{
			printf_ata("ATA_SOFT_RESET");
			gd_reset();
			GDStatus.full = 0;
			Error.full = 1;
			sns_key = 0;
			SecNumber.Status = GD_PAUSE;
			IntReason.full = 1;
			// DC Checker expects these values
			ByteCount.low = 0x14;
			ByteCount.hi = 0xEB;
		}
		break;

	case ATA_EXEC_DIAG:
		printf_ata("ATA_EXEC_DIAG");
		Error.full = 1;	// No error
		sns_key = 0;
		GDStatus.BSY = 0;
		GDStatus.CHECK = 1;

		asic_RaiseInterrupt(holly_GDROM_CMD);
		gd_set_state(gds_waitcmd);
		break;

	case ATA_SPI_PACKET:
		printf_ata("ATA_SPI_PACKET");
		gd_set_state(gds_waitpacket);
		break;

	case ATA_IDENTIFY_DEV:
		printf_ata("ATA_IDENTIFY_DEV: offset %d len %d", packet_cmd.data_8[2], packet_cmd.data_8[4]);
		GDStatus.BSY = 0;
		gd_spi_pio_end((const u8*)&reply_a1[0], 0x50);
		break;

	case ATA_SET_FEATURES:
		printf_ata("ATA_SET_FEATURES");

		//Set features sets :
		//Error : ABRT
		Error.ABRT=0;  // Command was not aborted ;) [hopefully ...]

		//status : DRDY , DSC , DF , CHECK
		//DRDY is set on state change
		GDStatus.DSC=0;
		GDStatus.DF=0;
		GDStatus.DRQ = 0;
		asic_RaiseInterrupt(holly_GDROM_CMD);
		gd_set_state(gds_waitcmd);
		break;

	case ATA_IDENTIFY:
		printf_ata("ATA_IDENTIFY\n");

		// Set Signature
		DriveSel = 0xa0;

		SecCount.full = 1;
		SecNumber.full = 1;
		ByteCount.low = 0x14;
		ByteCount.hi = 0xeb;

		// ABORT command
		Error.full = 0x4;

		GDStatus.full = 0;
		GDStatus.DRDY = 1;
		GDStatus.CHECK = 1;

		asic_RaiseInterrupt(holly_GDROM_CMD);
		gd_set_state(gds_waitcmd);
		break;

	default:
		// not a command the drive has: it says so, as it does for NOP
		WARN_LOG(GDROM, "Unknown ATA command %x", ata_cmd.command);
		Error.ABRT = 1;
		Error.Sense = 5;	// illegal request
		GDStatus.BSY = 0;
		GDStatus.CHECK = 1;
		asic_RaiseInterrupt(holly_GDROM_CMD);
		gd_set_state(gds_waitcmd);
		break;
	};
}

/* The Q channel's own checksum: CRC-16 with the CCITT polynomial, sent
 * inverted. */
static u16 subq_crc(const u8 *data, int len)
{
	u16 crc = 0;

	while (len-- > 0)
	{
		crc ^= (u16)(*data++ << 8);
		for (int bit = 0; bit < 8; bit++)
			crc = (crc & 0x8000) ? (u16)((crc << 1) ^ 0x1021) : (u16)(crc << 1);
	}
	return (u16)~crc;
}

static u8 bin2bcd(u32 v)
{
	return (u8)((v % 10) | ((v / 10) << 4));
}

/* Where the drive is: the sector being played, while it plays or is paused
 * in the middle of playing, and otherwise where it last read. */
static u32 gd_current_fad(u32 read_fad)
{
	return cdda.status == cdda_t::Playing || cdda.status == cdda_t::Paused ? cdda.CurrAddr.FAD : read_fad;
}

u32 gd_get_subcode(u32 format, u32 fad, u8 *subc_info)
{
	subc_info[0] = 0;
	switch (cdda.status)
	{
	case cdda_t::NoInfo:
	default:
		subc_info[1] = 0x15;	// No audio status info
		break;
	case cdda_t::Playing:
		subc_info[1] = 0x11;	// Audio playback in progress
		break;
	case cdda_t::Paused:
		subc_info[1] = 0x12;	// Audio playback paused
		break;
	case cdda_t::Terminated:
		subc_info[1] = 0x13;	// Audio playback ended normally
		break;
	}

	switch (format)
	{
	case 0:	// Raw subcode
		subc_info[2] = 0;
		subc_info[3] = 100;
		/* Images seldom carry the subcode, and what was read here was that
		 * of the last sector read, not of where the drive is. It is made
		 * instead: the twelve bytes of a Q frame - kind of track, track,
		 * index, time into the track, time into the disc, checksum - one
		 * bit to a byte, in the Q channel's bit, as the drive gives them. */
		{
			const u32 cur_fad = gd_current_fad(fad);
			u32 elapsed;
			const u32 tracknum = libGDR_GetTrackNumber(cur_fad, elapsed);
			u8 adr, ctrl, q[12];

			libGDR_GetTrackAdrAndControl(tracknum, adr, ctrl);
			q[0] = (u8)((ctrl << 4) | adr);
			q[1] = bin2bcd(tracknum);
			q[2] = bin2bcd(1);						// index
			q[3] = bin2bcd(elapsed / 60 / 75);		// minutes,
			q[4] = bin2bcd((elapsed / 75) % 60);	// seconds
			q[5] = bin2bcd(elapsed % 75);			// and frames into the track
			q[6] = 0;
			q[7] = bin2bcd(cur_fad / 60 / 75);		// and into the disc
			q[8] = bin2bcd((cur_fad / 75) % 60);
			q[9] = bin2bcd(cur_fad % 75);
			const u16 crc = subq_crc(q, 10);
			q[10] = (u8)(crc >> 8);
			q[11] = (u8)crc;
			for (int i = 0; i < 12; i++)
				for (int bit = 0; bit < 8; bit++)
					subc_info[4 + i * 8 + bit] = (q[i] & (0x80 >> bit)) ? 0x40 : 0;
		}
		break;

	case 1:	// Q data only
	default:
		{
			// where the drive is, not where it last read, while music plays
			fad = gd_current_fad(fad);
			u32 elapsed;
			u32 tracknum = libGDR_GetTrackNumber(fad, elapsed);

			//2 DATA Length MSB (0 = 0h)
			subc_info[2] = 0;
			//3 DATA Length LSB (14 = Eh)
			subc_info[3] = 0xE;
			//4 Control ADR: of the track the drive is on, not of the disc
			u8 adr, ctrl;
			libGDR_GetTrackAdrAndControl(tracknum, adr, ctrl);
			subc_info[4] = (u8)((ctrl << 4) | adr);
			//5-13	DATA-Q
			u8* data_q = &subc_info[5 - 1];
			//-When ADR = 1
			//1 TNO - track number
			data_q[1] = tracknum;
			//2 X - index within track
			data_q[2] = 1;
			//3-5   Elapsed FAD within track
			data_q[3] = elapsed >> 16;
			data_q[4] = elapsed >> 8;
			data_q[5] = elapsed;
			//6 ZERO
			data_q[6] = 0;
			//7-9 FAD
			data_q[7] = fad >> 16;
			data_q[8] = fad >> 8;
			data_q[9] = fad;
			DEBUG_LOG(GDROM, "gd_get_subcode: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
					 subc_info[0], subc_info[1], subc_info[2], subc_info[3],
					 subc_info[4], subc_info[5], subc_info[6], subc_info[7],
					 subc_info[8], subc_info[9], subc_info[10], subc_info[11],
					 subc_info[12], subc_info[13]);
		}
		break;

	case 2:	// Media catalog number (UPC/bar code)
		{
			//2 DATA Length MSB (0 = 0h)
			subc_info[2] = 0;
			//3 DATA Length LSB (24 = 18h)
			subc_info[3] = 0x18;
			//4 Format Code
			subc_info[4] = 2;
			//5-7 reserved
			subc_info[5] = 0;
			subc_info[6] = 0;
			subc_info[7] = 0;
			//8 MCVal (bit 7)
			subc_info[8] = 0;	// not valid
			//9-21 Media catalog number
			memcpy(&subc_info[9], "0000000000000", 13);
			//22-23 reserved
			subc_info[22] = 0;
			subc_info[23] = 0;
			DEBUG_LOG(GDROM, "gd_get_subcode: format 2 (Media catalog number). audio %x", subc_info[1]);
		}
		break;
	}
	return subc_info[3];
}

/* A field of the command packet that the drive can make nothing of. The
 * command ends in a check condition, as one the drive does not know does:
 * illegal request, with "invalid field in the command packet" for a reason. */
static void gd_spi_invalid_field()
{
	GDStatus.CHECK = 1;
	sns_key = 5;	// Illegal request
	sns_asc = 0x24;	// Invalid field in command packet
	sns_ascq = 0;
}

void gd_process_spi_cmd()
{

	printf_spi("Sense: %02x %02x %02x", sns_asc, sns_ascq, sns_key);

	printf_spi("SPI command %02x;",packet_cmd.data_8[0]);
	printf_spi("Params: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
		packet_cmd.data_8[0], packet_cmd.data_8[1], packet_cmd.data_8[2], packet_cmd.data_8[3], packet_cmd.data_8[4], packet_cmd.data_8[5],
		packet_cmd.data_8[6], packet_cmd.data_8[7], packet_cmd.data_8[8], packet_cmd.data_8[9], packet_cmd.data_8[10], packet_cmd.data_8[11] );

	if (sns_key == 0x0 			// No sense
			|| sns_key == 0xB)	// Aborted
		GDStatus.CHECK=0;
	else
		GDStatus.CHECK=1;

	switch(packet_cmd.data_8[0])
	{
	case SPI_TEST_UNIT:
		printf_spicmd("SPI_TEST_UNIT");

		GDStatus.CHECK=SecNumber.Status==GD_BUSY; // Drive is ready ;)

		gd_set_state(gds_procpacketdone);
		break;

	case SPI_REQ_MODE:
		GD_HardwareInfo.speed = 0;	// doesn't seem to be settable, or perhaps not for GD-Roms
		GD_HardwareInfo._res0[0] = 0;
		GD_HardwareInfo._res0[1] = 0;
		GD_HardwareInfo._res1 = 0;
		GD_HardwareInfo._res2[0] = 0;
		GD_HardwareInfo._res2[1] = 0;
		GD_HardwareInfo.read_flags &= 0x39;
		printf_spicmd("SPI_REQ_MODE cd-rom speed %d flags %x retry %x", GD_HardwareInfo.speed, GD_HardwareInfo.read_flags, GD_HardwareInfo.read_retry);
		gd_spi_pio_reply((const u8 *)&GD_HardwareInfo, sizeof(GD_HardwareInfo), packet_cmd.data_8[2], packet_cmd.data_8[4]);
		break;

		/////////////////////////////////////////////////
		// *FIXME* CHECK FOR DMA, Diff Settings !?!@$#!@%
	case SPI_CD_READ:
	case SPI_CD_READ2:
		{
#define readcmd packet_cmd.GDReadBlock

			cdda.status = cdda_t::NoInfo;
			u32 sector_type=2048;
			if (readcmd.head ==1 && readcmd.subh==1 && readcmd.data==1 && readcmd.expdtype==3 && readcmd.other==0)
				sector_type=2340;
			else if (readcmd.other == 1 || readcmd.expdtype == 1)
				sector_type = 2352;		// an audio sector, whole: NBA Hoopz reads its voices this way
			else if(readcmd.head ||readcmd.subh || readcmd.other || (!readcmd.data)) // assert
				WARN_LOG(GDROM, "GDROM: *FIXME* ADD MORE CD READ SETTINGS %d %d %d %d 0x%01X",readcmd.head,readcmd.subh,readcmd.other,readcmd.data,readcmd.expdtype);

			read_params.start_sector = GetFAD(&packet_cmd.data_8[2],packet_cmd.GDReadBlock.prmtype);
			// *FIXME LIBRETRO* : packet_cmd.data_8 should probably be rebased to readcmd.b
			if (packet_cmd.data_8[0] == SPI_CD_READ)
				read_params.remaining_sectors = (packet_cmd.data_8[8]<<16) | (packet_cmd.data_8[9]<<8) | (packet_cmd.data_8[10]);
			else
				read_params.remaining_sectors = (packet_cmd.data_8[6] << 8) | packet_cmd.data_8[7];
			read_params.sector_type = sector_type;//yeah i know , not really many types supported...
			/* What is left of a sector of the read before, if that one
			 * was given up halfway, is not the start of this one. (It was
			 * sent first, since the cache stopped being filled afresh
			 * for every read.) */
			read_buff.cache_size = 0;
			libGDR_Prefetch(read_params.start_sector, read_params.remaining_sectors);

			printf_spicmd("SPI_CD_READ - Sector=%d Size=%d/%d DMA=%d",read_params.start_sector,read_params.remaining_sectors,read_params.sector_type,Features.CDRead.DMA);
			if (Features.CDRead.DMA == 1)
			{
				gd_set_state(gds_readsector_dma);
			}
			else
			{
				gd_set_state(gds_readsector_pio);
			}
		}
		break;

	case SPI_GET_TOC:
		{
			printf_spicmd("SPI_GET_TOC");
			//printf("SPI_GET_TOC - %d\n",(packet_cmd.data_8[4]) | (packet_cmd.data_8[3]<<8) );
			u32 toc_gd[102];
			
			//toc - dd/sd
			libGDR_GetToc(&toc_gd[0],packet_cmd.data_8[1]&0x1);
			 
			gd_spi_pio_reply((const u8 *)&toc_gd[0], sizeof(toc_gd), 0, (packet_cmd.data_8[4]) | (packet_cmd.data_8[3]<<8));
		}
		break;

		//mount/map drive ? some kind of reset/unlock ??
		//seems like a non data command :)
	case 0x70:
		printf_spicmd("SPI : unknown ? [0x70]");
		//printf("SPI : unknown ? [0x70]\n");
		/*GDStatus.full=0x50; //FIXME
		RaiseInterrupt(holly_GDROM_CMD);*/

		gd_set_state(gds_procpacketdone);
		break;


	// Command 71 seems to trigger some sort of authentication check(?).
	// Update Sept 1st 2010: It looks like after a sequence of events the drive ends up having a specific state.
	// If the drive is fed with a "bootable" disc it ends up in "PAUSE" state. On all other cases it ends up in "STANDBY".
	// Cmd 70 and Error Handling / Sense also seem to take part in the above mentioned sequence of events.
	// This is more or less a hack until more info about this command becomes available. ~Psy
	case 0x71:
		{
			printf_spicmd("SPI : unknown ? [0x71]");
			//printf("SPI : unknown ? [0x71]\n");
			extern const u32 reply_71_sz;

			gd_spi_pio_end((const u8*)&reply_71[0], reply_71_sz);//uCount


			if (libGDR_GetDiscType()==GdRom || libGDR_GetDiscType()==CdRom_XA)
				SecNumber.Status=GD_PAUSE;
			else
				SecNumber.Status=GD_STANDBY;
		}
		break;
	case SPI_SET_MODE:
		{
			printf_spicmd("SPI_SET_MODE");
			u32 Offset = packet_cmd.data_8[2];
			// limit to writable area (the first ten bytes; from past them, nothing)
			u32 Count = Offset >= 10 ? 0 : std::min((u32)packet_cmd.data_8[4], 10 - Offset);
			set_mode_offset=Offset;
			gd_spi_pio_read_end(Count,gds_process_set_mode);
		}

		break;

	case SPI_REQ_STAT:
		{
			printf_spicmd("SPI_REQ_STAT");
			// where the drive is: see gd_current_fad()
			const u32 cur_fad = gd_current_fad(read_params.start_sector - 1);
			u32 elapsed;
			u32 tracknum = libGDR_GetTrackNumber(cur_fad, elapsed);
			u8 stat[10];

			//0  0   0   0   0   STATUS
			stat[0]=SecNumber.Status;   //low nibble 
			//1 Disc Format Repeat Count
			stat[1]=(u8)(SecNumber.DiscFormat<<4) | (cdda.repeats);
			//2 Address Control
			{
				// of the track the drive is on: an audio track on a disc with data is still audio
				u8 adr, ctrl;
				libGDR_GetTrackAdrAndControl(tracknum, adr, ctrl);
				stat[2] = (u8)((ctrl << 4) | adr);
			}
			//3 TNO
			stat[3] = tracknum;
			//4 X
			stat[4] = 1;
			//5 FAD
			stat[5] = (u8)(cur_fad >> 16);
			//6 FAD
			stat[6] = (u8)(cur_fad >> 8);
			//7 FAD
			stat[7] = (u8)cur_fad;
			//8 Max Read Error Retry Times
			stat[8]=0;
			//9 0   0   0   0   0   0   0   0
			stat[9]=0;

			gd_spi_pio_reply(stat, sizeof(stat), packet_cmd.data_8[2], packet_cmd.data_8[4]);
		}
		break;

	case SPI_REQ_ERROR:
		{
			printf_spicmd("SPI_REQ_ERROR");
			u8 resp[10];
			resp[0]=0xF0;
			resp[1]=0;
			resp[2]=sns_key;//sense
			resp[3]=0;
			resp[4]=resp[5]=resp[6]=resp[7]=0; //Command Specific Information
			resp[8]=sns_asc;//Additional Sense Code
			resp[9]=sns_ascq;//Additional Sense Code Qualifier

			gd_spi_pio_reply(resp, sizeof(resp), 0, packet_cmd.data_8[4]);
			sns_key = 0;
			sns_asc = 0;
			sns_ascq = 0;
			GDStatus.CHECK = 0;
		}
		break;

	case SPI_REQ_SES:
		{
			printf_spicmd("SPI_REQ_SES");

			u8 ses_inf[6];
			libGDR_GetSessionInfo(ses_inf,packet_cmd.data_8[2]);
			ses_inf[0]=SecNumber.Status;
			gd_spi_pio_reply(ses_inf, sizeof(ses_inf), 0, packet_cmd.data_8[4]);
		}
		break;

	case SPI_CD_OPEN:
		printf_spicmd("SPI_CD_OPEN Unhandled");
		
		gd_set_state(gds_procpacketdone);
		break;

	case SPI_CD_PLAY:
		{
			const u32 param_type = packet_cmd.data_8[1] & 7;
			printf_spicmd("SPI_CD_PLAY param_type=%d", param_type);

			if (param_type == 1 || param_type == 2)
			{
				cdda.status = cdda_t::Playing;
				SecNumber.Status = GD_PLAY;

				bool min_sec_frame = param_type == 2;
				cdda.StartAddr.FAD = cdda.CurrAddr.FAD = GetFAD(&packet_cmd.data_8[2], min_sec_frame);
				cdda.EndAddr.FAD = GetFAD(&packet_cmd.data_8[8], min_sec_frame);
				if (cdda.EndAddr.FAD == 0)
				{
					// Get the last sector of the disk
					u8 ses_inf[6] = {};
					libGDR_GetSessionInfo(ses_inf, 0);

					cdda.EndAddr.FAD = ses_inf[3] << 16 | ses_inf[4] << 8 | ses_inf[5];
				}
				cdda.repeats = packet_cmd.data_8[6] & 0xF;
				GDStatus.DSC = 1;
			}
			else if (param_type == 7)
			{
				if (cdda.status == cdda_t::Paused)
				{
					// Resume from previous pos unless we're at the end
					if (cdda.CurrAddr.FAD > cdda.EndAddr.FAD)
					{
						cdda.status = cdda_t::Terminated;
						SecNumber.Status = GD_STANDBY;
					}
					else
					{
						cdda.status = cdda_t::Playing;
						SecNumber.Status = GD_PLAY;
					}
				}
			}
			else
			{
				/* No way of saying where to play that the drive has: it
				 * goes on with what it was doing and reports the error.
				 * (This stopped the emulator.) */
				WARN_LOG(GDROM, "SPI_CD_PLAY: unknown parameter type %d", param_type);
				gd_spi_invalid_field();
			}

			DEBUG_LOG(GDROM, "CDDA StartAddr=%d EndAddr=%d repeats=%d status=%d CurrAddr=%d",cdda.StartAddr.FAD,
					cdda.EndAddr.FAD, cdda.repeats, cdda.status, cdda.CurrAddr.FAD);

			gd_set_state(gds_procpacketdone);
		}
		break;

	case SPI_CD_SEEK:
		{
			const u32 param_type = packet_cmd.data_8[1] & 7;
			printf_spicmd("SPI_CD_SEEK param_type=%d", param_type);

			if (param_type < 1 || param_type > 4)
			{
				/* Likewise, and looked at first: music that is playing is
				 * not paused for a command the drive does not carry out. */
				WARN_LOG(GDROM, "SPI_CD_SEEK: unknown parameter type %d", param_type);
				gd_spi_invalid_field();
				gd_set_state(gds_procpacketdone);
				break;
			}
			SecNumber.Status = GD_PAUSE;
			if (cdda.status == cdda_t::Playing)
				cdda.status = cdda_t::Paused;

			if (param_type == 1 || param_type == 2)
			{
				bool min_sec_frame = param_type == 2;
				cdda.StartAddr.FAD = cdda.CurrAddr.FAD = GetFAD(&packet_cmd.data_8[2], min_sec_frame);
#ifdef STRICT_MODE
				SecNumber.Status = GD_SEEK;
				GDStatus.DSC = 0;
				sh4_sched_request(gdrom_schid, SH4_MAIN_CLOCK / 50);	// 20 ms
#else
				GDStatus.DSC = 1;
#endif
			}
			else if (param_type == 3)
			{
				//stop audio , goto home
				cdda.StartAddr.FAD = cdda.CurrAddr.FAD = 150;
				cdda.status = cdda_t::NoInfo;
#ifdef STRICT_MODE
				SecNumber.Status = GD_BUSY;
				GDStatus.DSC = 0;
				sh4_sched_request(gdrom_schid, SH4_MAIN_CLOCK / 50);	// 20 ms
#else
				SecNumber.Status = GD_STANDBY;
				GDStatus.DSC = 1;
#endif
			}
			else if (param_type == 4)
			{
				//pause audio -- nothing more
			}

			DEBUG_LOG(GDROM, "CDDA StartAddr=%d EndAddr=%d repeats=%d status=%d CurrAddr=%d",cdda.StartAddr.FAD,
					cdda.EndAddr.FAD, cdda.repeats, cdda.status, cdda.CurrAddr.FAD);

			gd_set_state(gds_procpacketdone);
		}
		break;

	case SPI_CD_SCAN:
		printf_spicmd("SPI_CD_SCAN Unhandled");
		

		gd_set_state(gds_procpacketdone);
		break;

	case SPI_GET_SCD:
		{
			printf_spicmd("SPI_GET_SCD");

			const u32 format = packet_cmd.data_8[1] & 0xF;
			const u32 alloc_len = (packet_cmd.data_8[3] << 8) | packet_cmd.data_8[4];
			u8 subc_info[100];
			u32 size = gd_get_subcode(format, read_params.start_sector - 1, subc_info);
			gd_spi_pio_end(subc_info, std::min(size, alloc_len));
		}
		break;

	default:
		INFO_LOG(GDROM, "GDROM: Unhandled Sega SPI frame: %X", packet_cmd.data_8[0]);
		GDStatus.CHECK = 1;
		sns_key = 5;	// Illegal request
		sns_asc = 0x20;	// Unsupported command was received
		sns_ascq = 0;
		/* And the command is over (upstream): without this the drive
		 * stayed busy with it, no interrupt came, and it took nothing
		 * more. */
		gd_set_state(gds_procpacketdone);
		break;
	}
}
//Read handler
u32 ReadMem_gdrom(u32 Addr, u32 sz)
{	
	switch (Addr)
	{
		//cancel interrupt
	case GD_STATUS_Read :
		asic_CancelInterrupt(holly_GDROM_CMD);	//Clear INTRQ signal
		if (DriveSel & 0x10)
			return 0;		// the second drive: there is none
		printf_rm("GDROM: STATUS [cancel int](v=%X)",GDStatus.full);
		return GDStatus.full;

	case GD_ALTSTAT_Read:
		//printf_rm("GDROM: AltStatus (v=%X)",GDStatus.full);
		return GDStatus.full;

	case GD_BYCTLLO	:
		printf_rm("GDROM: Read From GD_BYCTLLO");
		return ByteCount.low;

	case GD_BYCTLHI	:
		printf_rm("GDROM: Read From GD_BYCTLHI");
		return ByteCount.hi;

	case GD_DATA:
		if(2!=sz)
			INFO_LOG(GDROM, "GDROM: Bad size on DATA REG Read");

		//if (gd_state == gds_pio_send_data)
		//{
			if (pio_buff.index == pio_buff.size)
			{
				INFO_LOG(GDROM, "GDROM: Illegal Read From DATA (underflow)");
			}
			else
			{
				u32 rv= pio_buff.data[pio_buff.index];
				pio_buff.index+=1;
				ByteCount.full-=2;

            //end of pio transfer !
				if (pio_buff.index==pio_buff.size)
					gd_set_state(pio_buff.next_state);
				return rv;
			}

		//}
		//else
		//	printf("GDROM: Illegal Read From DATA (wrong mode)\n");

		return 0;

	case GD_DRVSEL:
		printf_rm("GDROM: Read From DriveSel");
		return DriveSel;

	case GD_ERROR_Read:
		printf_rm("GDROM: Read from ERROR Register");
		Error.Sense=sns_key;
		return Error.full;

	case GD_IREASON_Read:
		printf_rm("GDROM: Read from INTREASON Register");
		return IntReason.full;

	case GD_SECTNUM:
		printf_rm("GDROM: Read from SecNumber Register (v=%X)", SecNumber.full);
		return SecNumber.full;

	default:
		INFO_LOG(GDROM, "GDROM: Unhandled read from address %X, Size:%X",Addr,sz);
		return 0;
	}
}

//Write Handler
void WriteMem_gdrom(u32 Addr, u32 data, u32 sz)
{
	switch(Addr)
	{
	case GD_BYCTLLO:
		printf_rm("GDROM: Write to GD_BYCTLLO = %X, Size:%X",data,sz);
		ByteCount.low =(u8) data;
		break;

	case GD_BYCTLHI: 
		printf_rm("GDROM: Write to GD_BYCTLHI = %X, Size:%X",data,sz);
		ByteCount.hi =(u8) data;
		break;

	case GD_DATA: 
		{
			if(2!=sz)
				INFO_LOG(GDROM, "GDROM: Bad size on DATA REG");
			if (gd_state == gds_waitpacket)
			{
				packet_cmd.data_16[packet_cmd.index]=(u16)data;
				packet_cmd.index+=1;
				if (packet_cmd.index==6)
					gd_set_state(gds_procpacket);
			}
			else if (gd_state == gds_pio_get_data)
			{
				pio_buff.data[pio_buff.index]=(u16)data;
				pio_buff.index+=1;
				if (pio_buff.size==pio_buff.index)
					gd_set_state(pio_buff.next_state);
			}
			else
			{
				INFO_LOG(GDROM, "GDROM: Illegal Write to DATA");
			}
			return;
		}

	case GD_DEVCTRL_Write:
		INFO_LOG(GDROM, "GDROM: Write GD_DEVCTRL (Not implemented on Dreamcast)");
		break;

	case GD_DRVSEL: 
		// the top three bits are not the game's to change
		DriveSel = (DriveSel & 0xe0) | (data & 0x1f);
		if (DriveSel & 0x10)
			INFO_LOG(GDROM, "GDROM: the second drive, which there is not, selected: %02X", data);
		break;

		// By writing "3" as Feature Number and issuing the Set Feature command,
		// the PIO or DMA transfer mode set in the Sector Count register can be selected.
		// The actual transfer mode is specified by the Sector Counter Register. 

	case GD_FEATURES_Write:
		printf_rm("GDROM: Write to GD_FEATURES");
		Features.full =(u8) data;
		break;

	case GD_SECTCNT_Write:
		DEBUG_LOG(GDROM, "GDROM: Write to SecCount = %X", data);
		SecCount.full =(u8) data;
		break;

	case GD_SECTNUM:
		INFO_LOG(GDROM, "GDROM: Write to SecNum; not possible = %X", data);
		break;

	case GD_COMMAND_Write:
		//printf("\nGDROM:\tCOMMAND: %X !\n", data);
		if (DriveSel & 0x10)
		{
			// for the second drive: nobody takes it
			DEBUG_LOG(GDROM, "ATA command to slave drive ignored: %x", data);
			break;
		}
		ata_cmd.command=(u8)data;
		gd_set_state(gds_procata);
		break;

	default:
		INFO_LOG(GDROM, "GDROM: Unhandled write to address %X <= %X, Size:%X",Addr,data,sz);
		break;
	}
}

int GDROM_TICK=1500000;

/* How much the game asked to have transferred; 0 stands for all 32 MB. It
 * goes 32 bytes at a time, so a length that is not so many of those is
 * carried on to the end of the 32 bytes it stops in. (Such a length stopped
 * the emulator.) */
static inline u32 gd_dma_len()
{
	return (SB_GDLEN + 31) & 0x01ffffe0;
}

static int getGDROMTicks()
{
   if (SB_GDST & 1)
   {
	  if (GDROM_TICK < 1500000)
		 return GDROM_TICK;
     const u32 gdlen = gd_dma_len();
     u32 len = gdlen == 0 ? 0x02000000 : gdlen;
     /* A large transfer comes off the disc as it turns: 10240 bytes at the
      * drive's 1.8 MB/s. This was a round 1000000 cycles, which is 2 MB/s,
      * faster than the drive goes; upstream found Sakura Taisen 3's music
      * breaking at that rate.
      *
      * A small one is already in the drive's buffer and goes at the speed
      * of the bus. That is 100 MB/s on paper; upstream counts 25 MB/s,
      * the rate at which World Series Baseball 2K2 stops freezing, and so
      * does this. */
     if (len - SB_GDLEND > 10240)
		 return (int)((u64)SH4_MAIN_CLOCK * 10240 / 1800000);
	  else
        return (int)((u64)SH4_MAIN_CLOCK * std::min((u32)10240, len - SB_GDLEND) / 25000000);
   }
   else
	  return 0;
}

//is this needed ?
static int GDRomschd(int i, int c, int j)
{
	if (SecNumber.Status == GD_SEEK)
	{
		SecNumber.Status = GD_PAUSE;
		GDStatus.DSC = 1;
	}
	else if (SecNumber.Status == GD_BUSY)
	{
		SecNumber.Status = GD_STANDBY;
		GDStatus.DSC = 1;
	}
	if(!(SB_GDST&1) || !(SB_GDEN &1) || (read_buff.cache_size==0 && read_params.remaining_sectors==0))
		return 0;

   u32 src = SB_GDSTARD;
   const u32 gdlen = gd_dma_len();
   u32 len = (gdlen == 0 ? 0x02000000 : gdlen) - SB_GDLEND;

	/* No more than the drive has to give: what is left in the cache and
	 * the sectors still to be read. This used to be checked only when no
	 * sectors were left on the way in. A transfer longer than the read it
	 * belongs to then ran the sectors out halfway through the loop below,
	 * which went round for ever with nothing to move. The transfer now
	 * stops with what there was and stays unfinished, as it would on the
	 * hardware, until it is given more or called off. */
	{
		const u64 have = (u64)read_buff.cache_size
				+ (u64)read_params.remaining_sectors * read_params.sector_type;
		if (len > have)
			len = (u32)have;
	}

	len = std::min(len, (u32)10240);

#if 0
	// do we need to do this for GDROM DMA?
   if (0x8201 != (DMAC_DMAOR.full & DMAOR_MASK))
	{
      INFO_LOG(GDROM, "GDROM: DMAOR has invalid settings (%X)", DMAC_DMAOR.full);
		//return;
	}

	if(len == 0)
	{
		INFO_LOG(GDROM, "GDROM: Len: %X, Abnormal Termination !", len);
	}
#endif

	u32 len_backup = len;
	if(1 == SB_GDDIR) 
	{
		while(len)
		{
			u32 buff_size =read_buff.cache_size;

			/* Whole sectors bound for RAM go straight from the image into
			 * it; only a partial sector, or a destination that is not
			 * plain memory, passes through the cache. */
			if (buff_size == 0 && len >= read_params.sector_type
					&& read_params.remaining_sectors)
			{
				bool  dst_ismem;
				void* dst_ptr = _vmem_write_const(src, dst_ismem, 4);
				/* (no further than the memory goes on from there: as far
				 * as the end of it or of its 16 MB of the address space.
				 * The sector that would cross that goes through the
				 * cache, which is copied out a piece at a time.) */
				u32 n = 0;
				if (dst_ismem)
				{
					bool end_ismem;
					const u32 run = (u32)((u8*)_vmem_write_const(src | 0x00FFFFFF, end_ismem, 4) - (u8*)dst_ptr) + 1;
					n = std::min(len, run) / read_params.sector_type;
				}
				if (n != 0)
				{
					if (n > read_params.remaining_sectors)
						n = read_params.remaining_sectors;
					libGDR_ReadSector((u8*)dst_ptr, read_params.start_sector, n, read_params.sector_type);
					read_params.start_sector     += n;
					read_params.remaining_sectors -= n;
					src += n * read_params.sector_type;
					len -= n * read_params.sector_type;
					continue;
				}
			}
         //buffer is empty , fill it
			if (buff_size==0)
				FillReadBuffer();

			//transfer up to len bytes
			if (buff_size>len)
				buff_size=len;
			WriteMemBlock_nommu_ptr(src,(u32*)&read_buff.cache[read_buff.cache_index], buff_size);
			read_buff.cache_index+=buff_size;
			read_buff.cache_size-=buff_size;
			src+=buff_size;
			len-=buff_size;
		}
	}
	else
	{
		WARN_LOG(GDROM, "GDROM: SB_GDDIR %X (TO AICA WAVE MEM?)", src);
	}

   SB_GDLEND = (SB_GDLEND + len_backup) & 0x01ffffe0;
   SB_GDSTARD += len_backup;


	if (SB_GDLEND == gdlen)
	{
      SB_GDST = 0;
		asic_RaiseInterrupt(holly_GDROM_DMA);
	}
	//Read ALL sectors
	if (read_params.remaining_sectors==0)
	{
		//And all buffer :p
		if (read_buff.cache_size==0)
			gd_set_state(gds_procpacketdone);
	}

   return getGDROMTicks();
}

//DMA Start
void GDROM_DmaStart(u32 addr, u32 data)
{
	if (SB_GDEN==0)
	{
		INFO_LOG(GDROM, "Invalid GD-DMA start, SB_GDEN=0.Ignoring it.");
		return;
	}
	SB_GDST|=data&1;

	if (SB_GDST==1)
	{
		SB_GDSTARD=SB_GDSTAR;
		SB_GDLEND=0;
		DEBUG_LOG(GDROM, "GDROM-DMA start addr %08X len %d", SB_GDSTAR, SB_GDLEN);
		if (SB_GDLEN & 0x1F)
			WARN_LOG(GDROM, "GDROM: SB_GDLEN %X is not a multiple of 32: %X transferred", SB_GDLEN, gd_dma_len());

      int ticks = getGDROMTicks();
		if (ticks < SH4_TIMESLICE)
		{
			ticks = GDRomschd(0,0,0);
		}
 		if (ticks)
			sh4_sched_request(gdrom_sched, ticks);
	}
}

void GDROM_DmaEnable(u32 addr, u32 data)
{
	SB_GDEN = (data & 1);
	if (SB_GDEN == 0 && SB_GDST == 1)
	{
		printf_spi("GD-DMA aborted");
		SB_GDST = 0;
	}
}

/* The drive's state as a save state gave it. How much of a buffer is in
 * use, where in it the next word goes and how long a sector is are the
 * state's word for it, and were taken at that: a state that is damaged, or
 * was made to be, had the drive read and write outside its buffers. What
 * does not fit is made to, and a transfer that cannot be is called off.
 * Nothing changes for a state this drive wrote. */
void gdrom_state_loaded()
{
	const u32 pio_words = sizeof(pio_buff.data) / sizeof(pio_buff.data[0]);

	if (pio_buff.size > pio_words)
		pio_buff.size = pio_words;
	if (pio_buff.index > pio_buff.size)
		pio_buff.index = pio_buff.size;
	/* What follows a transfer is the end of the command, more sectors, or
	 * SET_MODE taking what it was sent. (Anything else is not a state to
	 * go to from there: taking data again, for one, with the buffer full.) */
	if (pio_buff.next_state != gds_waitcmd && pio_buff.next_state != gds_pio_end
			&& pio_buff.next_state != gds_readsector_pio && pio_buff.next_state != gds_process_set_mode)
		pio_buff.next_state = gds_pio_end;
	// SET_MODE's data goes into the first ten bytes of the drive's settings
	// (and, for an odd number of them, one byte of padding in the last word)
	if (pio_buff.next_state == gds_process_set_mode
			&& (set_mode_offset > 10 || (pio_buff.size << 1) > 11 - set_mode_offset))
		pio_buff.index = pio_buff.size = 0;
	// data is taken until the buffer is full, and it is full already
	if (gd_state == gds_pio_get_data && pio_buff.index >= pio_buff.size)
		gd_state = gds_waitcmd;
	// a packet is six words
	if (gd_state == gds_waitpacket && packet_cmd.index >= 6)
		packet_cmd.index = 0;
	// sectors are read as 2048, 2340 or 2352 bytes: of any other size, none
	if (read_params.sector_type != 2048 && read_params.sector_type != 2340
			&& read_params.sector_type != 2352)
		read_params.remaining_sectors = 0;
	// one of the states there are
	if ((u32)gd_state > (u32)gds_process_set_mode)
		gd_state = gds_waitcmd;
	/* How long a piece of a DMA transfer takes when disc loading is set to
	 * fast: a count of cycles, at least one, or 1500000 for "not set". Zero
	 * or less would have the transfer never go on. */
	if (GDROM_TICK < 1 || GDROM_TICK > 1500000)
		GDROM_TICK = 1500000;
}

/* The part of the drive's state that was not in a save state before V21:
 * the sector cache of a DMA read. It holds what is left of the one sector
 * a transfer took part of, so that is all that is saved: how many bytes,
 * and the bytes, in room for a whole sector (2352), the rest of it zero. */
#define GD_CACHE_SAVED 2352

void gdrom_serialize_v21(void **data, unsigned int *total_size)
{
	static const u8 blank[GD_CACHE_SAVED] = { 0 };
	const u8 *from = read_buff.cache;
	u32 left = read_buff.cache_size;

	if (left > GD_CACHE_SAVED || read_buff.cache_index > sizeof(read_buff.cache) - left)
		left = 0;
	else
		from += read_buff.cache_index;
	LIBRETRO_S(left);
	LIBRETRO_SA(from, left);
	LIBRETRO_SA(blank, GD_CACHE_SAVED - left);
}

bool gdrom_unserialize_v21(void **data, unsigned int *total_size)
{
	u32 left = 0;

	LIBRETRO_US(left);
	LIBRETRO_USA(read_buff.cache, GD_CACHE_SAVED);
	// (no more than the room that was saved; a count that says more is of nothing)
	if (left > GD_CACHE_SAVED)
		left = 0;
	read_buff.cache_index = 0;
	read_buff.cache_size = left;
	return true;
}

/* A state from before V21 has no cache in it, and was loaded as if the
 * cache had been empty: what was in it was never sent, and the read went on
 * from further into the disc than the game had got.
 *
 * What was in it can be worked out when one DMA transfer carries the whole
 * of the read, which the state shows: the read's command is still there,
 * with its first sector and its count, and the transfer under way is as
 * long as the read. Then the bytes sent so far are the transfer's count of
 * them, the sectors taken from the disc beyond those were in the cache (up
 * to 32 of them: the drive used to read that far ahead), and they are on
 * the disc still. The sectors that had not been touched are given back to
 * be read again, and the one that was partly sent is read into the cache.
 *
 * In any other case - a read sent in several transfers, or none under way -
 * the cache is empty, as it was for every such state. */
void gdrom_state_before_v21(void)
{
	read_buff.cache_index = 0;
	read_buff.cache_size = 0;

	const u32 cmd = packet_cmd.data_8[0];
	const u32 sector_type = read_params.sector_type;
	if (gd_state != gds_readsector_dma || !(SB_GDST & 1) || SB_GDDIR != 1
			|| (cmd != SPI_CD_READ && cmd != SPI_CD_READ2)
			|| (sector_type != 2048 && sector_type != 2340 && sector_type != 2352))
		return;

	const u32 first = GetFAD(&packet_cmd.data_8[2], packet_cmd.GDReadBlock.prmtype);
	const u32 count = cmd == SPI_CD_READ
			? (u32)((packet_cmd.data_8[8] << 16) | (packet_cmd.data_8[9] << 8) | packet_cmd.data_8[10])
			: (u32)((packet_cmd.data_8[6] << 8) | packet_cmd.data_8[7]);
	const u32 taken = read_params.start_sector - first;		// sectors read from the disc so far
	// (a transfer's length is in 32s of bytes, and not every number of sectors is)
	if (taken > count || read_params.remaining_sectors != count - taken
			|| (((u64)count * sector_type + 31) & ~(u64)31) != gd_dma_len())
		return;

	const u64 sent = SB_GDLEND;
	if (sent >= (u64)taken * sector_type)
		return;
	const u32 whole = (u32)(((u64)taken * sector_type - sent) / sector_type);
	const u32 part = (u32)(((u64)taken * sector_type - sent) % sector_type);

	read_params.start_sector -= whole;
	read_params.remaining_sectors += whole;
	if (part != 0)
	{
		libGDR_ReadSector(read_buff.cache, read_params.start_sector - 1, 1, sector_type);
		read_buff.cache_index = sector_type - part;
		read_buff.cache_size = part;
	}
}

//Init/Term/Res
void gdrom_reg_Init()
{
	sb_rio_register(SB_GDST_addr, RIO_WF, 0, &GDROM_DmaStart);
	sb_rio_register(SB_GDEN_addr, RIO_WF, 0, &GDROM_DmaEnable);

	gdrom_sched = sh4_sched_register(0, &GDRomschd);
	gd_swap_schid = sh4_sched_register(0, &gd_swap_sched);
}

void gdrom_reg_Term(void)
{
	/* The machine set up next may be one with no drive, which does not
	 * register these again: nothing of this one's is left waiting in them. */
	gd_swap_cancel();
	if (sh4_sched_is_scheduled(gdrom_sched))
		sh4_sched_request(gdrom_sched, -1);
}

void gdrom_reg_Reset(bool hard)
{
	SB_GDST = 0;
	SB_GDEN = 0;
	// (and the next piece of the transfer that was under way is not to come)
	if (sh4_sched_is_scheduled(gdrom_sched))
		sh4_sched_request(gdrom_sched, -1);

	/* The drive starts over as well: whatever command, transfer or music it
	 * was in the middle of is not carried into the game that starts next. */
	gd_state = gds_waitcmd;
	sns_asc = 0;
	sns_ascq = 0;
	sns_key = 0;
	set_mode_offset = 0;
	read_params = {};
	packet_cmd = {};
	read_buff = {};
	pio_buff = {};
	ata_cmd = {};
	cdda = {};
	gd_disk_type = NoDisk;
	data_write_mode = 0;
	DriveSel = 0xa0;
	Error = {};
	IntReason = {};
	Features = {};
	SecCount = {};
	SecNumber = {};
	GDStatus = {};
	ByteCount = {};
	// and looks at what disc is in it
	gd_swap_cancel();
	gd_setdisc();

	// set default hardware information
	memset(&GD_HardwareInfo, 0, sizeof(GD_HardwareInfo));
	GD_HardwareInfo.speed = 0x0;
	GD_HardwareInfo.standby_hi = 0x00;
	GD_HardwareInfo.standby_lo = 0xb4;
	GD_HardwareInfo.read_flags = 0x19;
	GD_HardwareInfo.read_retry = 0x08;
	memcpy(GD_HardwareInfo.drive_info, "SE      ", sizeof(GD_HardwareInfo.drive_info));
	memcpy(GD_HardwareInfo.system_version, "Rev 6.43", sizeof(GD_HardwareInfo.system_version));
	memcpy(GD_HardwareInfo.system_date, "990408", sizeof(GD_HardwareInfo.system_date));
}
