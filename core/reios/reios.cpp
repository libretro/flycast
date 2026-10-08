/*
	Extremely primitive bios replacement

	Many thanks to Lars Olsson (jlo@ludd.luth.se) for bios decompile work
		http://www.ludd.luth.se/~jlo/dc/bootROM.c
		http://www.ludd.luth.se/~jlo/dc/bootROM.h
		http://www.ludd.luth.se/~jlo/dc/security_stuff.c
	Bits and pieces from redream (https://github.com/inolen/redream)
*/

#include "reios.h"
#include <algorithm>
#include <streams/file_stream.h>

#include "reios_elf.h"

#include "gdrom_hle.h"
#include "descrambl.h"

#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/holly/sb_mem.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/aica/aica_if.h"
#include "iso9660.h"
#include "font.h"
#include "hw/aica/aica.h"

#include <map>

#define debugf(...) DEBUG_LOG(REIOS, __VA_ARGS__)

#define dc_bios_syscall_system				0x8C0000B0
#define dc_bios_syscall_font				0x8C0000B4
#define dc_bios_syscall_flashrom			0x8C0000B8
#define dc_bios_syscall_gd					0x8C0000BC
#define dc_bios_syscall_gd2					0x8C0000C0
#define dc_bios_syscall_misc				0x8c0000E0

//At least one game (ooga) uses this directly
#define dc_bios_entrypoint_gd2	0x8c0010F0

#define SYSINFO_ID_ADDR 0x8C001010
#define FONT_TABLE_ADDR 0xa0100020

static MemChip *flashrom;
static u32 base_fad = 45150;
static bool descrambl = false;

extern char game_dir_no_slash[1024];

static void reios_pre_init()
{
	if (libGDR_GetDiscType() == GdRom) {
		base_fad = 45150;
		descrambl = false;
	} else {
		u8 ses[6];
		libGDR_GetSessionInfo(ses, 0);
		libGDR_GetSessionInfo(ses, ses[2]);
		base_fad = (ses[3] << 16) | (ses[4] << 8) | (ses[5] << 0);
		descrambl = true;
	}
}

static u32 decode_iso733(iso733_t v)
{
	return ((v >> 56) & 0x000000FF)
			| ((v >> 40) & 0x0000FF00)
			| ((v >> 24) & 0x00FF0000)
			| ((v >> 8) & 0xFF000000);
}

static bool reios_locate_bootfile(const char* bootfile)
{
	reios_pre_init();

	// Load IP.BIN bootstrap
	u8 *ip_bin = GetMemPtr(0x8c008000, 0);
	memset(ip_bin, 0xFF, 16);
	libGDR_ReadSector(ip_bin, base_fad, 16, 2048);
	/* A read from a drive with nothing in it reads nothing, and the drive
	 * can say it has a disc when it has not: a state saved with one in,
	 * loaded with the lid open, says so. What was read then has to be a
	 * boot sector before anything is made of it. */
	if (memcmp(ip_bin, "SEGA SEGAKATANA ", 16) != 0)
		return false;

	/* What temp holds of a directory: 2 MB, which is a thousand sectors. */
	const u32 dir_room = 2048 * 1024;
	u32 data_len = dir_room;
	/* Zeroed: left as it came, it could be the block the last boot's
	 * directory was read into and freed, and a read that read nothing
	 * left that directory there to find the boot file in - which was then
	 * "loaded" by reading nothing again, and run from memory that had
	 * just been wiped. */
	u8* temp = new u8[data_len]();

	libGDR_ReadSector(temp, base_fad + 16, 1, 2048);
	iso9660_pvd_t *pvd = (iso9660_pvd_t *)temp;

	if (pvd->type == 1 && !memcmp(pvd->id, ISO_STANDARD_ID, strlen(ISO_STANDARD_ID)) && pvd->version == 1)
	{
		INFO_LOG(REIOS, "iso9660 PVD found");
		u32 lba = decode_iso733(pvd->root_directory_record.extent);
		u32 len = decode_iso733(pvd->root_directory_record.size);

		/* The directory is read into temp: no more of it than temp holds,
		 * whatever length the disc gives for it - a length above that
		 * was read past the end of temp, and one near 4 GB came out of
		 * the rounding below as nothing. */
		if (len > dir_room)
		{
			WARN_LOG(REIOS, "iso9660 root directory of %u bytes: the first %u are looked at", len, dir_room);
			len = dir_room;
		}
		data_len = ((len + 2047) / 2048) * 2048;

		INFO_LOG(REIOS, "iso9660 root_directory, FAD: %d, len: %d", 150 + lba, data_len);
		libGDR_ReadSector(temp, 150 + lba, data_len / 2048, 2048);
	}
	else {
		libGDR_ReadSector(temp, base_fad + 16, data_len / 2048, 2048);
	}

	int bootfile_len = strlen(bootfile);
	while (bootfile_len > 0 && isspace(bootfile[bootfile_len - 1]))
		bootfile_len--;
	/* A record is 33 bytes and then its name (the 33rd is the name's
	 * length). */
	const u32 name_at = 33;
	for (u32 i = 0; bootfile_len > 0 && i + name_at <= data_len; )
	{
		iso9660_dir_t *dir = (iso9660_dir_t *)&temp[i];
		const u32 record_len = dir->length;
		if (record_len == 0)
		{
			/* Records do not run from one sector into the next: the rest
			 * of this one is padding, and the directory goes on at the
			 * start of the next. */
			i = (i / 2048 + 1) * 2048;
			continue;
		}
		/* One that says it is shorter than a record is, or that runs past
		 * what was read, is not a record: the directory ends there. */
		if (record_len < name_at || record_len > data_len - i)
			break;

		if ((dir->file_flags & ISO_DIRECTORY) == 0
				&& (u32)bootfile_len <= record_len - name_at
				&& (u32)bootfile_len <= (u8)dir->filename.str[0]
				&& memcmp(dir->filename.str + 1, bootfile, bootfile_len) == 0)
		{
			INFO_LOG(REIOS, "Found %.*s at offset %X", bootfile_len, bootfile, i);

			u32 lba = decode_iso733(dir->extent);
			u32 len = decode_iso733(dir->size);

			if (!memcmp(bootfile, "0WINCEOS.BIN", 12))
			{
				/* (its first sector is not part of the program) */
				if (len < 2048)
				{
					ERROR_LOG(REIOS, "Boot file too small: %u", len);
					delete[] temp;
					return false;
				}
				lba++;
				len -= 2048;
			}

			INFO_LOG(REIOS, "file LBA: %d", lba);
			INFO_LOG(REIOS, "file LEN: %d", len);

			/* The program goes to 8c010000, and has to fit between there
			 * and the end of main memory: a length off a bad disc is not
			 * read past it. */
			/* (the length itself is compared: rounded up to sectors first,
			 * one near 4 GB came out as nothing and passed) */
			if (len == 0 || len > RAM_SIZE - 0x10000)
			{
				ERROR_LOG(REIOS, "Boot file too large: %u", len);
				delete[] temp;
				return false;
			}
			if (descrambl)
				descrambl_file(lba + 150, len, GetMemPtr(0x8c010000, 0));
			else
				libGDR_ReadSector(GetMemPtr(0x8c010000, 0), lba + 150, (len + 2047) / 2048, 2048);

			delete[] temp;

			u8 data[24] = {0};
			// system id
			for (u32 j = 0; j < 8; j++)
				data[j] = _vmem_ReadMem8(0x0021a056 + j);

			// system properties
			for (u32 j = 0; j < 5; j++)
				data[8 + j] = _vmem_ReadMem8(0x0021a000 + j);

			// system settings
			// if the flash has them: a damaged one is not a reason to put junk there
			flash_syscfg_block syscfg = {};
			if (static_cast<DCFlashChip*>(flashrom)->ReadBlock(FLASH_PT_USER, FLASH_USER_SYSCFG, &syscfg))
				memcpy(&data[16], &syscfg.time_lo, 8);
			else
				WARN_LOG(REIOS, "Can't read system settings from flash");

			memcpy(GetMemPtr(0x8c000068, sizeof(data)), data, sizeof(data));

			return true;
		}
		i += record_len;
	}

	delete[] temp;
	return false;
}

ip_meta_t ip_meta;

void reios_disk_id()
{
	if (libGDR_GetDiscType() == Open || libGDR_GetDiscType() == NoDisk)
	{
		memset(&ip_meta, 0, sizeof(ip_meta));
		return;
	}
	reios_pre_init();

	u8 buf[2048];
	libGDR_ReadSector(buf, base_fad, 1, sizeof(buf));
	memcpy(&ip_meta, buf, sizeof(ip_meta));
	INFO_LOG(REIOS, "hardware %.16s maker %.16s ks %.5s type %.6s num %.5s area %.8s ctrl %.4s dev %c vga %c wince %c "
			"product %.10s version %.6s date %.8s boot %.16s softco %.16s name %.128s",
			ip_meta.hardware_id, ip_meta.maker_id, ip_meta.ks, ip_meta.disk_type, ip_meta.disk_num, ip_meta.area_symbols,
			ip_meta.ctrl, ip_meta.dev, ip_meta.vga, ip_meta.wince,
			ip_meta.product_number, ip_meta.product_version,
			ip_meta.release_date, ip_meta.boot_filename, ip_meta.software_company, ip_meta.software_name);
}

static void reios_sys_system() {
	u32 cmd = r[7];

	switch (cmd)
	{
	case 0:	//SYSINFO_INIT
		{
			debugf("reios_sys_system: SYSINFO_INIT");
			// 0x00-0x07: system_id
			// 0x08-0x0c: system_props
			// 0x0d-0x0f: padding (zeroed out)
			// 0x10-0x17: settings
			u8 data[24] = {0};

			// read system_id from 0x0001a056
			for (int i  = 0; i < 8; i++)
				data[i] = flashrom->Read8(0x1a056 + i);

			// read system_props from 0x0001a000
			for (int i  = 0; i < 5; i++)
				data[8 + i] = flashrom->Read8(0x1a000 + i);

			// 0x0d-0x17: padding (zeroed out)

			memcpy(GetMemPtr(0x8c000068, sizeof(data)), data, sizeof(data));

			r[0] = 0;
		}
		break;

	case 2: //SYSINFO_ICON
		debugf("reios_sys_system: SYSINFO_ICON");
		// r4 = icon number (0-9, but only 5-9 seems to really be icons)
		// r5 = destination buffer (704 bytes in size)
		r[0] = 704;
		break;

	case 3: //SYSINFO_ID
		debugf("reios_sys_system: SYSINFO_ID");
		r[0] = 0x8c000068;
		break;

	default:
		WARN_LOG(REIOS, "reios_sys_system: unhandled cmd %d", cmd);
		break;
	}
}

static void reios_sys_font() {
	u32 cmd = r[1];

	switch (cmd)
	{
	case 0:		// FONTROM_ADDRESS
		debugf("FONTROM_ADDRESS");
		r[0] = FONT_TABLE_ADDR;	// in ROM
		break;

	case 1:		// FONTROM_LOCK
		debugf("FONTROM_LOCK");
		r[0] = 0;
		break;

	case 2:		// FONTROM_UNLOCK
		debugf("FONTROM_UNLOCK");
		r[0] = 0;
		break;

	default:
		WARN_LOG(REIOS, "reios_sys_font cmd %x", cmd);
		break;
	}
}

static void reios_sys_flashrom() {
	u32 cmd = r[7];

	switch (cmd)
	{
		case 0: // FLASHROM_INFO
			{
				/*
					r4 = partition number(0 - 4)
					r5 = pointer to two 32 bit integers to receive the result.
						The first will be the offset of the partition start, in bytes from the start of the flashrom.
						The second will be the size of the partition, in bytes.
					Returns:
					r0 = 0 if successful, -1 if no such partition exists
				 */

				u32 part = r[4];
				u32 dest = r[5];
				debugf("reios_sys_flashrom: FLASHROM_INFO part %d dest %08x", part, dest);

				if (part < FLASH_PT_NUM)
				{
					int offset, size;
					static_cast<DCFlashChip*>(flashrom)->GetPartitionInfo(part, &offset, &size);
					WriteMem32(dest, offset);
					WriteMem32(dest + 4, size);

					r[0] = 0;
				}
				else {
					r[0] = -1;
				}
			}
			break;

		case 1:	//FLASHROM_READ
			{
				/*
					r4 = read start position, in bytes from the start of the flashrom
					r5 = pointer to destination buffer
					r6 = number of bytes to read
					Returns:
					r0 = 0 if successful, -1 if read failed
				*/
				u32 offset = r[4];
				u32 dest = r[5];
				u32 size = r[6];

				debugf("reios_sys_flashrom: FLASHROM_READ offs %x dest %08x size %x", offset, dest, size);
				for (int i = 0; i < size; i++)
					WriteMem8(dest++, flashrom->Read8(offset + i));

				// 0 for success, not the number of bytes: Slave Zero (PAL) goes by it
				r[0] = 0;
			}
			break;


		case 2:	//FLASHROM_WRITE
			{
				/*
					r4 = write start position, in bytes from the start of the flashrom
					r5 = pointer to source buffer
					r6 = number of bytes to write
					Returns:
					r0 = number of written bytes if successful, -1 if write failed
				*/

				u32 offs = r[4];
				u32 src = r[5];
				u32 size = r[6];

				debugf("reios_sys_flashrom: FLASHROM_WRITE offs %x src %08x size %x", offs, src, size);

				for (int i = 0; i < size; i++)
					flashrom->data[offs + i] &= ReadMem8(src + i);

				r[0] = size;
			}
			break;

		case 3:	//FLASHROM_DELETE
			{
				/*
				   r4 = offset of the start of the partition you want to delete, in bytes from the start of the flashrom
				   Returns:
				   r0 = zero if successful, -1 if delete failed
				*/
				u32 offset = r[4];

				debugf("reios_sys_flashrom: FLASHROM_DELETE offs %x", offset);

				bool found = false;

				for (int part = 0; part < FLASH_PT_NUM; part++)
				{
					int part_offset;
					int size;
					static_cast<DCFlashChip*>(flashrom)->GetPartitionInfo(part, &part_offset, &size);
					if (offset == part_offset)
					{
						found = true;
						memset(flashrom->data + offset, 0xFF, size);
					}
				}

				r[0] = found ? 0 : -1;
			}
			break;

		default:
			WARN_LOG(REIOS, "reios_sys_flashrom: not handled, %d", cmd);
			break;
	}
}

static void reios_sys_gd()
{
	gdrom_hle_op();
}

static void reios_sys_gd2()
{
	gdrom_hle_op();
}

static void reios_sys_misc()
{
	INFO_LOG(REIOS, "reios_sys_misc - r7: 0x%08X, r4 0x%08X, r5 0x%08X, r6 0x%08X", r[7], r[4], r[5], r[6]);
	switch (r[4])
	{
	case 2:	// check disk
		r[0] = 0;
		// Reload part of IP.BIN bootstrap
		libGDR_ReadSector(GetMemPtr(0x8c008100, 0), base_fad, 7, 2048);
		break;

	default:
		break;
	}
}

typedef void hook_fp();
static u32 hook_addr(hook_fp* fn);

static void setup_syscall(u32 hook_addr, u32 syscall_addr) {
	WriteMem32(syscall_addr, hook_addr);
	WriteMem16(hook_addr, REIOS_OPCODE);

	debugf("Patching syscall vector %08X, points to %08X", syscall_addr, hook_addr);
	debugf(" - address %08X: data %04X [%04X]", hook_addr, ReadMem16(hook_addr), REIOS_OPCODE);
}

static void reios_setup_state(u32 boot_addr)
{
	/* San Francisco Rush adds up parts of the BIOS's area of memory as a
	 * protection and wants a certain sum. Words it adds up and nothing
	 * uses are set so that it gets it. (From upstream, as it is there.) */
	{
		short *p = (short *)GetMemPtr(0x8c0010f0, 2);
		int chksum = (int)0xFFF937D1;
		for (int i = 0; i < 10; i++)
			chksum -= *p++;
		p += 0xee - 1;
		for (int i = 0; i < 3; i++)
			chksum += *p++;
		p += 0x347 - 1;
		for (int i = 0; i < 11; i++)
			chksum -= *p++;
		p += 0xbf8 - 1;
		for (int i = 0; i < 98; i++)
		{
			short v = chksum < 0 ? std::min(-chksum, 32767) : std::max(-chksum, -32768);
			*p = v;
			chksum += *p++;
		}
	}

	// The DMA controller on, as the BIOS leaves it: KallistiOS expects that
	DMAC_DMAOR.full = 0x8201;

	/* The video cable and standard, on the SH4's port as the BIOS leaves
	 * them: Windows CE reads them there, and took a PAL console for an
	 * NTSC one (Caesars Palace 2000, The Next Tetris, KISS Psycho Circus). */
	if (settings.dreamcast.broadcast == 1)
		BSC_PDTRA.full = 4;
	BSC_PCTRA.full = 0x000A03F0;

	// Set up AICA interrupt masks
	libAICA_WriteReg(SCIEB_addr, 0x48, 2);
	libAICA_WriteReg(SCILV0_addr, 0x18, 1);
	libAICA_WriteReg(SCILV1_addr, 0x50, 1);
	libAICA_WriteReg(SCILV2_addr, 0x08, 1);

	/*
	Post Boot registers from actual bios boot
	r
	[0x00000000]	0xac0005d8
	[0x00000001]	0x00000009
	[0x00000002]	0xac00940c
	[0x00000003]	0x00000000
	[0x00000004]	0xac008300
	[0x00000005]	0xf4000000
	[0x00000006]	0xf4002000
	[0x00000007]	0x00000070
	[0x00000008]	0x00000000
	[0x00000009]	0x00000000
	[0x0000000a]	0x00000000
	[0x0000000b]	0x00000000
	[0x0000000c]	0x00000000
	[0x0000000d]	0x00000000
	[0x0000000e]	0x00000000
	[0x0000000f]	0x8d000000
	mac
	l	0x5bfcb024
	h	0x00000000
	r_bank
	[0x00000000]	0xdfffffff
	[0x00000001]	0x500000f1
	[0x00000002]	0x00000000
	[0x00000003]	0x00000000
	[0x00000004]	0x00000000
	[0x00000005]	0x00000000
	[0x00000006]	0x00000000
	[0x00000007]	0x00000000
	gbr	0x8c000000
	ssr	0x40000001
	spc	0x8c000776
	sgr	0x8d000000
	dbr	0x8c000010
	vbr	0x8c000000
	pr	0xac00043c
	fpul	0x00000000
	pc	0xac008300

	+		sr	{T=1 status = 0x400000f0}
	+		fpscr	{full=0x00040001}
	+		old_sr	{T=1 status=0x400000f0}
	+		old_fpscr	{full=0x00040001}

	*/

	//Setup registers to imitate a normal boot
	r[15] = 0x8d000000;

	gbr = 0x8c000000;
	ssr = 0x40000001;
	spc = 0x8c000776;
	sgr = 0x8d000000;
	dbr = 0x8c000010;
	vbr = 0x8c000000;
	pr = 0xac00043c;
	fpul = 0x00000000;
	next_pc = boot_addr;

	sr.status = 0x400000f0;
	sr.T = 1;

	old_sr.status = 0x400000f0;

	fpscr.full = 0x00040001;
	old_fpscr.full = 0x00040001;
}

static void reios_setup_naomi(u32 boot_addr, u32 load_end, u32 now) {
	/*
		SR 0x60000000 0x00000001
		FPSRC 0x00040001

		-		xffr	0x13e1fe40	float [32]
		[0x0]	1.00000000	float
		[0x1]	0.000000000	float
		[0x2]	0.000000000	float
		[0x3]	0.000000000	float
		[0x4]	0.000000000	float
		[0x5]	1.00000000	float
		[0x6]	0.000000000	float
		[0x7]	0.000000000	float
		[0x8]	0.000000000	float
		[0x9]	0.000000000	float
		[0xa]	1.00000000	float
		[0xb]	0.000000000	float
		[0xc]	0.000000000	float
		[0xd]	0.000000000	float
		[0xe]	0.000000000	float
		[0xf]	1.00000000	float
		[0x10]	1.00000000	float
		[0x11]	2.14748365e+009	float
		[0x12]	0.000000000	float
		[0x13]	480.000000	float
		[0x14]	9.99999975e-006	float
		[0x15]	0.000000000	float
		[0x16]	0.00208333321	float
		[0x17]	0.000000000	float
		[0x18]	0.000000000	float
		[0x19]	2.14748365e+009	float
		[0x1a]	1.00000000	float
		[0x1b]	-1.00000000	float
		[0x1c]	0.000000000	float
		[0x1d]	0.000000000	float
		[0x1e]	0.000000000	float
		[0x1f]	0.000000000	float
		
		-		r	0x13e1fec0	unsigned int [16]
		[0x0]	0x0c021000	unsigned int
		[0x1]	0x0c01f820	unsigned int
		[0x2]	0xa0710004	unsigned int
		[0x3]	0x0c01f130	unsigned int
		[0x4]	0x5bfccd08	unsigned int
		[0x5]	0xa05f7000	unsigned int
		[0x6]	0xa05f7008	unsigned int
		[0x7]	0x00000007	unsigned int
		[0x8]	0x00000000	unsigned int
		[0x9]	0x00002000	unsigned int
		[0xa]	0xffffffff	unsigned int
		[0xb]	0x0c0e0000	unsigned int
		[0xc]	0x00000000	unsigned int
		[0xd]	0x00000000	unsigned int
		[0xe]	0x00000000	unsigned int
		[0xf]	0x0cc00000	unsigned int

		-		mac	{full=0x0000000000002000 l=0x00002000 h=0x00000000 }	Sh4Context::<unnamed-tag>::<unnamed-tag>::<unnamed-type-mac>
		full	0x0000000000002000	unsigned __int64
		l	0x00002000	unsigned int
		h	0x00000000	unsigned int
		
		-		r_bank	0x13e1ff08	unsigned int [8]
		[0x0]	0x00000000	unsigned int
		[0x1]	0x00000000	unsigned int
		[0x2]	0x00000000	unsigned int
		[0x3]	0x00000000	unsigned int
		[0x4]	0x00000000	unsigned int
		[0x5]	0x00000000	unsigned int
		[0x6]	0x00000000	unsigned int
		[0x7]	0x00000000	unsigned int
		gbr	0x0c2abcc0	unsigned int
		ssr	0x60000000	unsigned int
		spc	0x0c041738	unsigned int
		sgr	0x0cbfffb0	unsigned int
		dbr	0x00000fff	unsigned int
		vbr	0x0c000000	unsigned int
		pr	0xac0195ee	unsigned int
		fpul	0x000001e0	unsigned int
		pc	0x0c021000	unsigned int
		jdyn	0x0c021000	unsigned int

	*/

	//Setup registers to imitate a normal boot
	r[0] = boot_addr;
	r[1] = 0x0c01f820;		// the header's copy, at its entry points
	r[2] = 0xa0710004;
	r[3] = 0x0c01f130;		// where the time is
	r[4] = now;
	r[5] = 0xa05f7000;
	r[6] = 0xa05f7008;
	r[7] = settings.System == DC_PLATFORM_NAOMI2 ? 0x00000007 : 0x00000006;
	r[8] = 0x00000000;
	r[9] = 0x00000000;
	r[10] = 0xffffffff;
	r[11] = load_end;
	r[12] = 0x00000000;
	r[13] = 0x00000000;
	r[14] = 0x00000000;
	r[15] = 0x0cc00000;

	gbr = 0x00000000;
	ssr = 0x60000000;
	spc = 0x0c020000;
	sgr = 0x0cbfffb4;
	dbr = 0x00000fff;
	vbr = 0x0c000000;
	pr = 0xac01965e;
	fpul = 0x000001e0;
	next_pc = boot_addr;

	sr.status = 0x60000000;
	sr.T = 1;

	old_sr.status = 0x60000000;

	fpscr.full = 0x00040001;
	old_fpscr.full = 0x00040001;
}

static u8 *reios_rom;       /* the boot ROM's memory, which reios fills */

/* ---- the NAOMI BIOS's system routines ----
 *
 * The BIOS is more than a loader. It leaves in memory, at 0c018000, a
 * table of routines that games call for the things every cabinet does
 * the same way: what the operator set in the test menu, how many coins
 * make a credit, the counting of coins and credits. (A game finds the
 * same block in the BIOS ROM at a0060000 and copies it over.) Without
 * them a game calls into empty memory and starts again, for ever.
 *
 * These are those routines, each doing what the BIOS's does - which was
 * found by reading epr-21576h's and watching what Dead or Alive 2 and
 * Virtua Fighter 4 call and with what. None of the BIOS's code is here:
 * a routine's place in the table holds the trap that reios answers.
 *
 *   0  a constant                1  where things are in the BIOS ROM
 *   2  read, 3 write a word the BIOS keeps for the game
 *   4  check the operator's settings, and put back the defaults if they
 *      cannot be
 *   5  the coin setting of a number: coins and credits for each chute
 *   6  start the coin handling for a game
 *   7  hold the players' credits to a limit
 *   8  what each player can do with the credits there are
 *   9  once a frame: coin and service switches in, credits out
 *  10  the board: region, the game's serial
 *  11  the operator's settings
 *  13  whether a service credit is for one player
 *  20  whether a cartridge answers
 *
 * 12, 16 to 19 and 21 to 25 are for the GD-ROM's DIMM board and the
 * network board; no cartridge game seen calls them, and they answer 0.
 *
 * The settings (17 words at 0c01f000, which routine 11 hands out):
 *   0 the most credits there can be   1 players, less one
 *   2 a coin chute to each player     3 free play
 *   4 which coin setting              5-12 credits to start, to continue...
 *   13, 14 what a coin of chute 1, 2 counts   15 coins for a bonus one
 *   16 how many of those make a credit
 * The real BIOS reads them from the EEPROM the test menu writes; these
 * are its defaults (one coin, one credit).
 *
 * The coin state a game keeps and passes in ("C" below), in words:
 *   0-3 which of settings 5-12 each player is at   4-7 credits
 *   8-11 coins towards the next credit             12-15 towards a bonus
 *   16-19 coins waiting to be counted              20 service credits
 *   21-24 how long each service switch is held     25-28 coin meters
 *   29-44 a queue of sounds to make, 45 and 46 its ends
 *   47-50 coins counted by the I/O board           51-54 switches: now,
 *   before, pressed, let go (bits 0-3 coin, 4-7 service)
 */
#define NB_BLOCK        0x0c018000
#define NB_ROUTINES     0x0c018400      /* the traps: 8 bytes apart */
#define NB_CONSTANT     0x0c018200
#define NB_ROM_AREAS    0x0c018204
#define NB_COIN_TABLE   0x0c018304
#define NB_MAX_CREDITS  0x0c019e34      /* by region */
#define NB_ONE_PLAYER   0x0c019e54
#define NB_SERVICE      0x0c019e58      /* service credits given, by player */
#define NB_SETTINGS     0x0c01f000
#define NB_BOARD        0x0c01f100
#define NB_GAME_WORD    0x0c01f120
#define NB_CREDITS_AT   0x0c01ff04
#define NB_BLOCK_SIZE   0x7000

static u32 nb_rd(u32 a)            { return ReadMem32(a); }
static void nb_wr(u32 a, u32 v)    { WriteMem32(a, v); }
static void nb_inc(u32 a)          { WriteMem32(a, ReadMem32(a) + 1); }

/* A sound for the game to make: 1 a coin, 2 a credit. */
static void nb_queue_event(u32 C, u32 event)
{
	u32 head = nb_rd(C + 0xb4);
	u32 next = (head + 1) & 15;
	if (next == nb_rd(C + 0xb8))
		return;
	nb_wr(C + 116 + head * 4, event);
	nb_wr(C + 0xb4, next);
}

/* A coin of @chute for @player. The credits it made. */
static u32 nb_add_coin(u32 S, u32 C, u32 player, u32 chute)
{
	u32 added = 0;
	u32 credits = C + 16 + player * 4;
	u32 coins = C + 32 + player * 4;
	u32 bonus = C + 48 + player * 4;

	if (nb_rd(S + 8) == 0)
		bonus = C + 48;
	else if (nb_rd(NB_ONE_PLAYER))
		nb_wr(NB_SERVICE + player * 4, 0);
	nb_wr(C + 80, 0);

	if (nb_rd(credits) < nb_rd(S) && nb_rd(S + 12) != 1)
	{
		nb_wr(coins, nb_rd(coins) + nb_rd(S + 52 + chute * 4));
		if (nb_rd(S + 60) != 0)
		{
			nb_wr(bonus, nb_rd(bonus) + nb_rd(S + 52 + chute * 4));
			while (nb_rd(bonus) >= nb_rd(S + 60))
			{
				nb_wr(bonus, nb_rd(bonus) - nb_rd(S + 60));
				nb_inc(coins);
			}
		}
		while (nb_rd(coins) >= nb_rd(S + 64))
		{
			nb_inc(credits);
			added++;
			if (nb_rd(credits) == nb_rd(S))
			{
				/* full: what is over is lost */
				nb_wr(coins, 0);
				nb_wr(bonus, 0);
				break;
			}
			nb_wr(coins, nb_rd(coins) - nb_rd(S + 64));
		}
		nb_queue_event(C, added ? 2 : 1);
	}

	/* the meters */
	if (nb_rd(S + 8) == 0 && nb_rd(S + 52) != nb_rd(S + 56))
		nb_inc(C + 100 + chute * 4);
	else
		nb_inc(C + 100);
	return added;
}

/* A service credit for @player, if there is room. True if one was given. */
static bool nb_service_credit(u32 S, u32 C, u32 player)
{
	u32 credits = C + 16 + player * 4;
	if (nb_rd(credits) >= nb_rd(S) || nb_rd(S + 12) == 1)
		return false;
	nb_inc(credits);
	if (nb_rd(credits) == nb_rd(S))
		nb_wr(C + 32 + player * 4, 0);
	nb_wr(C + 48 + player * 4, 0);
	nb_queue_event(C, 2);
	return true;
}

/* The coins waiting and the service switch, with one chute for all (the
 * four coin inputs are two chutes, twice). @B: the bookkeeping. */
static u32 nb_process_common(u32 S, u32 C, u32 service, u32 B)
{
	u32 from_coins = 0, from_service = 0, did = 0;

	for (u32 i = 0; i < 4; i++)
	{
		if (nb_rd(C + 64 + i * 4) == 0)
			continue;
		nb_wr(C + 64 + i * 4, nb_rd(C + 64 + i * 4) - 1);
		nb_wr(NB_SERVICE + i * 4, 0);
		from_coins += nb_add_coin(S, C, 0, i & 1);
		did = 1;
		nb_inc(B + 48);
		nb_inc(B + 32 + i * 4);
	}
	if (service != 0 && nb_rd(C + 80) < 9)
	{
		if (nb_rd(C + 16) < nb_rd(S))
			nb_inc(C + 80);
		if (nb_service_credit(S, C, 0))
			from_service++;
		did = 1;
	}
	nb_wr(B + 56, nb_rd(B + 56) + from_service);
	nb_wr(B + 52, nb_rd(B + 52) + from_coins);
	nb_wr(B + 60, nb_rd(B + 60) + from_coins + from_service);
	return did;
}

/* The same with a chute to each player; @service has a bit for each. */
static u32 nb_process_individual(u32 S, u32 C, u32 service, u32 B)
{
	s32 last = (s32)nb_rd(S + 4);
	u32 from_coins = 0, from_service = 0, did = 0;

	for (s32 p = 0; p < 4 && (p == 0 || p <= last); p++)
	{
		if (nb_rd(C + 64 + p * 4) == 0)
			continue;
		nb_wr(C + 64 + p * 4, nb_rd(C + 64 + p * 4) - 1);
		from_coins += nb_add_coin(S, C, p, 0);
		did = 1;
		nb_inc(B + 48);
		nb_inc(B + 32 + p * 4);
	}
	if (service != 0)
	{
		if (nb_rd(NB_ONE_PLAYER) != 0)
		{
			/* to the player whose switch it was */
			for (s32 p = 0; p <= last; p++)
			{
				if (!(service & (1u << p)) || (s32)nb_rd(NB_SERVICE + p * 4) >= 9)
					continue;
				did = 1;
				if (nb_rd(C + 16 + p * 4) < nb_rd(S))
					nb_inc(NB_SERVICE + p * 4);
				if (nb_service_credit(S, C, p))
					from_service++;
			}
		}
		else if (nb_rd(C + 80) < 9)
		{
			/* to every player */
			did = 1;
			for (s32 p = 0; p <= last; p++)
				if (nb_rd(C + 16 + p * 4) < nb_rd(S))
				{
					nb_inc(C + 80);
					break;
				}
			for (s32 p = 0; p <= last; p++)
				if (nb_service_credit(S, C, p))
					from_service++;
		}
	}
	nb_wr(B + 56, nb_rd(B + 56) + from_service);
	nb_wr(B + 52, nb_rd(B + 52) + from_coins);
	nb_wr(B + 60, nb_rd(B + 60) + from_coins + from_service);
	return did;
}

/* For each player four bytes at @O: credits, coins towards the next,
 * coins to a credit, and whether there is enough to play (0 nothing,
 * 1 not yet, 2 yes). With one chute the first three are once, for all. */
static void nb_status(u32 S, u32 C, u32 O)
{
	s32 last = (s32)nb_rd(S + 4);
	bool individual = nb_rd(S + 8) == 1;

	if (!individual)
	{
		WriteMem8(O, (u8)nb_rd(C + 16));
		WriteMem8(O + 1, (u8)nb_rd(C + 32));
		WriteMem8(O + 2, ReadMem8(S + 64));
	}
	for (s32 p = 0; p <= last; p++)
	{
		u32 from = individual ? (u32)p : 0;
		u32 credits = nb_rd(C + 16 + from * 4) & 0xff;
		u32 coins = nb_rd(C + 32 + from * 4) & 0xff;
		u8 can;

		if (individual)
		{
			WriteMem8(O + p * 4, (u8)credits);
			WriteMem8(O + p * 4 + 1, (u8)coins);
			WriteMem8(O + p * 4 + 2, ReadMem8(S + 64));
		}
		if (nb_rd(S + 12) == 1)
			can = 2;
		else if ((credits | coins) == 0)
			can = 0;
		else
			can = (s32)credits >= (s32)nb_rd(S + 20 + nb_rd(C + p * 4) * 4) ? 2 : 1;
		WriteMem8(O + p * 4 + 3, can);
	}
}

static void nb_sys_constant()   { r[0] = nb_rd(NB_CONSTANT); }
static void nb_sys_rom_area()
{
	nb_wr(r[5], nb_rd(NB_ROM_AREAS + r[4] * 8));
	nb_wr(r[5] + 4, nb_rd(NB_ROM_AREAS + r[4] * 8 + 4));
}
static void nb_sys_get_word()   { r[0] = nb_rd(NB_GAME_WORD); }
static void nb_sys_set_word()   { nb_wr(NB_GAME_WORD, r[4]); }

static void nb_sys_check_settings()
{
	u32 S = r[4];
	u32 bad = 0, bit = 0;
	u32 region = nb_rd(NB_BOARD);

	if (nb_rd(S + 16) >= 28)
		bad |= 1u << bit;
	nb_wr(S + 12, nb_rd(S + 16) == 26);		/* setting 27 is free play */
	bit++;
	if (region < 8)
		nb_wr(S, nb_rd(NB_MAX_CREDITS + region * 4));
	if (nb_rd(S) < 9 || nb_rd(S) > 24)
		bad |= 1u << bit;
	bit++;
	if ((s32)nb_rd(S + 4) >= 4)
		bad |= 1u << bit;
	bit++;
	if ((s32)nb_rd(S + 8) >= 2)
		bad |= 1u << bit;
	bit++;
	if ((s32)nb_rd(S + 12) >= 2)
		bad |= 1u << bit;
	bit++;
	if (nb_rd(S + 52) < 1 || nb_rd(S + 52) > 9)
		bad |= 1u << bit;
	bit++;
	if (nb_rd(S + 56) < 1 || nb_rd(S + 56) > 9)
		bad |= 1u << bit;
	bit++;
	if (nb_rd(S + 60) > 9)
		bad |= 1u << bit;
	bit++;
	if (nb_rd(S + 64) < 1 || nb_rd(S + 64) > 9)
		bad |= 1u << bit;
	bit++;
	for (u32 i = 0; i < 8; i++, bit++)
		if (nb_rd(S + 20 + i * 4) < 1 || nb_rd(S + 20 + i * 4) > 5)
			bad |= 1u << bit;

	if (bad)
	{
		nb_wr(S, 9);
		nb_wr(S + 4, 1);
		nb_wr(S + 8, 0);
		nb_wr(S + 12, 0);
		nb_wr(S + 16, 0);
		for (u32 i = 0; i < 8; i++)
			nb_wr(S + 20 + i * 4, 1);
		nb_wr(S + 52, 1);
		nb_wr(S + 56, 1);
		nb_wr(S + 60, 0);
		nb_wr(S + 64, 1);
	}
	r[0] = bad;
}

static void nb_sys_coin_setting()
{
	r[0] = NB_COIN_TABLE + ((s32)r[4] > 28 ? 0 : r[4]) * 4;
}

static void nb_sys_start()
{
	u32 S = r[4], C = r[5], K = r[6], O = r[7];

	nb_wr(NB_CREDITS_AT, K);
	/* (the backup memory says whether service credits go by player) */
	nb_wr(NB_ONE_PLAYER, ReadMem32(0xa020000c) == 1 && ReadMem32(0xa0200104) == 1);
	nb_wr(C + 80, 0);
	nb_wr(C + 0xb4, 0);
	nb_wr(C + 0xb8, 0);
	nb_wr(C + 0xcc, 0);
	nb_wr(C + 0xd0, 0);
	nb_wr(C + 0xd4, 0);
	nb_wr(C + 0xd8, 0);
	for (u32 i = 0; i < 4; i++)
	{
		nb_wr(C + 16 + i * 4, nb_rd(K + i * 4));
		nb_wr(C + i * 4, 0);
		nb_wr(C + 32 + i * 4, 0);
		nb_wr(C + 48 + i * 4, 0);
		nb_wr(C + 64 + i * 4, 0);
		nb_wr(C + 100 + i * 4, 0);
		nb_wr(C + 84 + i * 4, 0);
		nb_wr(C + 0xbc + i * 4, 0);
		nb_wr(NB_SERVICE + i * 4, 0);
	}
	WriteMem8(O + 16, (u8)nb_rd(S + 4) | (nb_rd(S + 8) != 0 ? 0x80 : 0));
	WriteMem8(O + 17, ReadMem8(S + 12));
	WriteMem8(O + 18, 2);
	WriteMem8(O + 19, 1);
}

static void nb_sys_limit_credits()
{
	u32 count = 0;
	for (u32 i = 0; i < 4; i++)
		if (nb_rd(r[5] + i * 4) > r[4])
		{
			nb_wr(r[5] + i * 4, r[4]);
			count++;
		}
	r[0] = count;
}

static void nb_sys_status()     { nb_status(r[4], r[5], r[6]); }

static void nb_sys_frame()
{
	u32 S = r[4], C = r[5], O = r[6], sw = r[7];
	u32 counted = nb_rd(r[15]);
	u32 B = nb_rd(r[15] + 4);
	u32 before[4], service = 0, did;

	for (u32 i = 0; i < 4; i++)
		before[i] = nb_rd(C + 16 + i * 4);

	u32 last_sw = nb_rd(C + 0xcc);
	nb_wr(C + 0xd0, last_sw);
	nb_wr(C + 0xcc, sw);
	nb_wr(C + 0xd4, sw & ~last_sw);
	nb_wr(C + 0xd8, ~sw & last_sw);

	for (u32 i = 0; i < 4; i++)
	{
		u32 held = C + 84 + i * 4;
		u32 bit = 0x10u << i;

		nb_wr(C + 64 + i * 4, 0);
		nb_wr(C + 0xbc + i * 4, nb_rd(counted + i * 4));
		/* a service switch counts once it has been down for seven frames */
		if (nb_rd(held) == 0)
		{
			if (nb_rd(C + 0xd4) & bit)
				nb_wr(held, 1);
		}
		else if (sw & bit)
			nb_inc(held);
		else
			nb_wr(held, 0);
		if (nb_rd(held) >= 7)
		{
			nb_wr(held, 0);
			service |= 1u << i;
		}
	}

	/* a coin switch let go is a coin; so is each the I/O board counted */
	bool individual = nb_rd(S + 8) != 0;
	s32 last = individual ? (s32)nb_rd(S + 4) : 3;
	for (s32 i = 0; i <= last; i++)
	{
		if (nb_rd(C + 0xd8) & (1u << i))
			nb_inc(C + 64 + i * 4);
		nb_wr(C + 64 + i * 4, nb_rd(C + 64 + i * 4) + nb_rd(C + 0xbc + i * 4));
	}
	did = individual ? nb_process_individual(S, C, service, B)
	                 : nb_process_common(S, C, service, B);

	if (did == 1)
		for (u32 i = 0; i < 4; i++)
		{
			u32 now = nb_rd(C + 16 + i * 4);
			nb_wr(B + i * 4, now);
			nb_wr(B + 16 + i * 4, nb_rd(B + 16 + i * 4) + now - before[i]);
		}
	nb_status(S, C, O);
	for (u32 i = 0; i < 4; i++)
		nb_wr(C + 0xbc + i * 4, 0);
	r[0] = did;
}

/* (both are words in main memory, handed over a word at a time) */
static void nb_sys_board()
{
	for (u32 i = 0; i < 20; i += 4)
		WriteMem32(r[4] + i, ReadMem32(NB_BOARD + i));
}

static void nb_sys_settings()
{
	for (u32 i = 0; i < 68; i += 4)
		WriteMem32(r[4] + i, ReadMem32(NB_SETTINGS + i));
}

static void nb_sys_one_player() { nb_wr(NB_ONE_PLAYER, r[4] == 1); }

static void nb_sys_cartridge()
{
	if (r[4] != 0 && ReadMem32(0xa05f7418) != 0)
		r[0] = 0;
	else
		r[0] = ReadMem16(0xa05f703c) == 0xffff ? (u32)-1 : 1;
}

static void nb_sys_none()
{
	static bool said;
	if (!said)
		WARN_LOG(REIOS, "NAOMI: a BIOS routine for the DIMM or network board was called (from %08x): answering 0", pr);
	said = true;
	r[0] = 0;
}

static hook_fp* const nb_routines[26] = {
	nb_sys_constant, nb_sys_rom_area, nb_sys_get_word, nb_sys_set_word,
	nb_sys_check_settings, nb_sys_coin_setting, nb_sys_start, nb_sys_limit_credits,
	nb_sys_status, nb_sys_frame, nb_sys_board, nb_sys_settings,
	nb_sys_none, nb_sys_one_player, NULL, NULL,
	nb_sys_none, nb_sys_none, nb_sys_none, nb_sys_none,
	nb_sys_cartridge, nb_sys_none, nb_sys_none, nb_sys_none,
	nb_sys_none, nb_sys_none,
};

/* The block as the BIOS has it at 60000 in its ROM, to @block: the table,
 * a trap for each routine, and the tables of numbers they hand out. */
static void nb_build_block(u8 *block)
{
	/* coins and credits, as the test menu's 28 coin settings have them */
	static const u32 coin_table[29] = {
		0x00010101, 0x00010201, 0x00010301, 0x00010401, 0x00010501, 0x00020201, 0x00020501, 0x00030301,
		0x00040401, 0x00050501, 0x00060601, 0x00010102, 0x00010202, 0x00010402, 0x02010101, 0x02010201,
		0x00010103, 0x00010104, 0x04010101, 0x04010401, 0x00010105, 0x05010503, 0x05010102, 0x05010502,
		0x05010101, 0x05010501, 0x00010101, 0x00010101, 0x000bd306,
	};
	static const u32 rom_areas[3][2] = {
		{ 0xa0080000, 0x000200 }, { 0xa0080200, 0x0a7000 }, { 0xa0127200, 0x07a000 },
	};
	static const u32 max_credits[8] = { 9, 24, 24, 9, 9, 9, 9, 9 };
	u32 *w = (u32 *)block;

	memset(block, 0, NB_BLOCK_SIZE);
	for (u32 i = 0; i < 26; i++)
		if (nb_routines[i])
		{
			w[i] = NB_ROUTINES + i * 8;
			*(u16 *)(block + (NB_ROUTINES - NB_BLOCK) + i * 8) = REIOS_OPCODE;
		}
	w[(NB_CONSTANT - NB_BLOCK) / 4] = 0x3f800000;
	memcpy(block + (NB_ROM_AREAS - NB_BLOCK), rom_areas, sizeof(rom_areas));
	memcpy(block + (NB_COIN_TABLE - NB_BLOCK), coin_table, sizeof(coin_table));
	memcpy(block + (NB_MAX_CREDITS - NB_BLOCK), max_credits, sizeof(max_credits));
}

/* ---- the NAOMI BIOS's interrupt service ----
 *
 * A game does not take the processor's exception vectors for itself: the
 * vector base stays where the BIOS put it, at 0c000000, and the game
 * writes the address of its handler for an event into the BIOS's table
 * at 0c0001c0 - a word for each event code, every 0x20 - which starts out
 * full of the address of a handler that does nothing. The BIOS's code at
 * the vectors saves the interrupted program's registers, calls the
 * handler as a C function, and puts everything back.
 *
 * Here that is done from two traps: one at each vector, which puts the
 * registers by and sends the processor to the handler
 * with a return address that is the other trap, where they are put back
 * and the interrupted program goes on. (The BIOS can also switch between
 * saved programs there, and takes a handler's answer of "not mine" to its
 * error screen. No game seen asks for either.) */
#define NB_VECTORS         0x0c000000
#define NB_HANDLERS        0x0c0001c0
#define NB_HANDLER_COUNT   66            /* events 000 to 820 */
#define NB_NO_HANDLER      0x0c001000    /* rts; mov #1,r0 */
#define NB_HANDLER_RETURN  0x0c000680

#define NB_SAVED_COUNT      0x0c000124    /* how many interrupted programs are kept */
#define NB_SAVED           0x0c004000    /* each in 0x200 bytes, where the BIOS keeps its own */
#define NB_SAVED_MAX       8

/* (The registers are kept in the machine's memory, not here: a saved
 * state taken while a handler runs has them.) */
static void nb_save_registers(u32 to)
{
	u32 i;
	for (i = 0; i < 16; i++, to += 4)
		WriteMem32(to, r[i]);
	for (i = 0; i < 8; i++, to += 4)
		WriteMem32(to, r_bank[i]);
	for (i = 0; i < 32; i++, to += 4)
		WriteMem32(to, *(u32 *)&xf[i]);
	WriteMem32(to, pr);
	WriteMem32(to + 4, gbr);
	WriteMem32(to + 8, ssr);
	WriteMem32(to + 12, spc);
	WriteMem32(to + 16, sgr);
	WriteMem32(to + 20, fpul);
	WriteMem32(to + 24, fpscr.full);
	WriteMem32(to + 28, mac.l);
	WriteMem32(to + 32, mac.h);
}

static void nb_restore_registers(u32 from)
{
	u32 i;
	for (i = 0; i < 16; i++, from += 4)
		r[i] = ReadMem32(from);
	for (i = 0; i < 8; i++, from += 4)
		r_bank[i] = ReadMem32(from);
	for (i = 0; i < 32; i++, from += 4)
		*(u32 *)&xf[i] = ReadMem32(from);
	pr = ReadMem32(from);
	gbr = ReadMem32(from + 4);
	ssr = ReadMem32(from + 8);
	spc = ReadMem32(from + 12);
	sgr = ReadMem32(from + 16);
	fpul = ReadMem32(from + 20);
	fpscr.full = ReadMem32(from + 24);
	UpdateFPSCR();
	mac.l = ReadMem32(from + 28);
	mac.h = ReadMem32(from + 32);
}

/* Back to the interrupted program: what the processor's rte does. */
static void nb_return_from_event()
{
	u32 to = spc;
	sh4_sr_SetFull(ssr);
	next_pc = to;
	if (UpdateSR())
		UpdateINTC();
}

static void nb_event(u32 event)
{
	u32 handler = NB_NO_HANDLER;
	u32 depth = ReadMem32(NB_SAVED_COUNT);

	if ((event >> 5) < NB_HANDLER_COUNT)
		handler = ReadMem32(NB_HANDLERS + (event >> 3));
	if (depth >= NB_SAVED_MAX || (handler & 0x1fffffff) == (NB_NO_HANDLER & 0x1fffffff) || handler == 0)
	{
		/* nobody's */
		nb_return_from_event();
		return;
	}
	nb_save_registers(NB_SAVED + depth * 0x200);
	WriteMem32(NB_SAVED_COUNT, depth + 1);
	pr = NB_HANDLER_RETURN;
	next_pc = handler;
}

static void nb_interrupt()   { nb_event(CCN_INTEVT); }
static void nb_exception()   { nb_event(CCN_EXPEVT); }

static void nb_handler_return()
{
	u32 depth = ReadMem32(NB_SAVED_COUNT);
	if (depth == 0 || depth > NB_SAVED_MAX)
		return;
	/* (the handler ran as the exception left the processor, on its other
	 * set of r0 to r7: both sets go back as they were, then the status
	 * register chooses) */
	WriteMem32(NB_SAVED_COUNT, depth - 1);
	nb_restore_registers(NB_SAVED + (depth - 1) * 0x200);
	nb_return_from_event();
}

/* The vectors, the table and the handler that does nothing, in memory. */
static void nb_install_interrupts()
{
	for (u32 i = 0; i < NB_HANDLER_COUNT; i++)
		WriteMem32(NB_HANDLERS + i * 4, NB_NO_HANDLER);
	WriteMem16(NB_NO_HANDLER, 0x000b);       /* rts */
	WriteMem16(NB_NO_HANDLER + 2, 0xe001);   /* mov #1,r0 */
	WriteMem16(NB_VECTORS + 0x100, REIOS_OPCODE);
	WriteMem16(NB_VECTORS + 0x400, REIOS_OPCODE);
	WriteMem16(NB_VECTORS + 0x600, REIOS_OPCODE);
	WriteMem16(NB_HANDLER_RETURN, REIOS_OPCODE);
	WriteMem32(NB_SAVED_COUNT, 0);
}

/* What the NAOMI's BIOS does to start a game, done for it when there is no
 * BIOS: a flat image's header says what is to be loaded and where the game
 * begins.
 *
 *   0x360  up to eight pieces to load, each three words - where in the
 *          cartridge, where in memory, how long - ended by a first word
 *          of ffffffff
 *   0x420  the address the game starts at
 *
 * The BIOS keeps a copy of the header at 0c01f400 and the time it read
 * from the clock at 0c01f130, and hands over with registers pointing at
 * both; the display is in the 640x480 mode it showed its logo in. All as
 * found by letting the real BIOS (epr-21576h, and the NAOMI 2's
 * epr-23605c) run up to the game's first instruction. What it is not: the
 * BIOS's test menu, its check of the game's region, or the routines it
 * leaves in memory below 0c006000 - no game seen so far calls them. */
static void reios_boot_naomi()
{
	static const u32 video[][2] = {
		{ 0x030, 0x00000101 }, { 0x044, 0x00800005 }, { 0x048, 0x00000009 }, { 0x04c, 0x000000a0 },
		{ 0x050, 0x00c00000 }, { 0x054, 0x00c00500 }, { 0x05c, 0x00177d3f }, { 0x060, 0x00800000 },
		{ 0x064, 0x00c00000 }, { 0x068, 0x027f0000 }, { 0x06c, 0x01df0000 }, { 0x074, 0x00000001 },
		{ 0x078, 0x3e4ccccd }, { 0x07c, 0x0027df77 }, { 0x080, 0x00000007 }, { 0x088, 0x3727c5a0 },
		{ 0x098, 0x00800408 }, { 0x0a0, 0x00000020 }, { 0x0a4, 0x0000001f }, { 0x0a8, 0x15d1c955 },
		{ 0x0b0, 0x00808080 }, { 0x0b4, 0x007f7f7f }, { 0x0b8, 0x00008009 }, { 0x0bc, 0xf000f0f0 },
		{ 0x0c8, 0x03430000 }, { 0x0cc, 0x00150208 }, { 0x0d4, 0x00880343 }, { 0x0d8, 0x02110353 },
		{ 0x0dc, 0x00240208 }, { 0x0e0, 0x03f19351 }, { 0x0e8, 0x00160000 }, { 0x0ec, 0x000000a5 },
		{ 0x0f0, 0x00240024 }, { 0x0f4, 0x00000400 }, { 0x110, 0x00093f39 }, { 0x118, 0x00008040 },
		{ 0x11c, 0x000000ff },
	};
	u8 header[0x500];
	u32 entry, load_end = 0, now;

	if (CurrentCartridge == NULL || !CurrentCartridge->Read(0, sizeof(header), header))
	{
		WARN_LOG(REIOS, "No cartridge loaded");
		return;
	}

	for (u32 i = 0; i < 8; i++)
	{
		const u8 *e = header + 0x360 + i * 12;
		u32 offset = e[0] | e[1] << 8 | e[2] << 16 | (u32)e[3] << 24;
		u32 addr   = e[4] | e[5] << 8 | e[6] << 16 | (u32)e[7] << 24;
		u32 size   = e[8] | e[9] << 8 | e[10] << 16 | (u32)e[11] << 24;
		u32 ram    = (addr & 0x1fffffff) - 0x0c000000;

		if (offset == 0xffffffff)
			break;
		/* (into main memory, whichever of its addresses is given) */
		if (ram >= RAM_SIZE || size > RAM_SIZE - ram
				|| !CurrentCartridge->Read(offset, size, GetMemPtr(0x8c000000 + ram, size)))
		{
			WARN_LOG(REIOS, "NAOMI boot: cannot load %x bytes at %08x from %x", size, addr, offset);
			continue;
		}
		NOTICE_LOG(REIOS, "NAOMI boot: %x bytes at %08x, from %x", size, addr, offset);
		load_end = (addr & 0x1fffffff) + size;
	}
	entry = header[0x420] | header[0x421] << 8 | header[0x422] << 16 | (u32)header[0x423] << 24;

	/* the header's copy, and the time */
	memcpy(GetMemPtr(0x8c01f400, sizeof(header)), header, sizeof(header));
	now = GetRTC_now();
	WriteMem32(0x8c01f130, now);

	/* the system routines, in the ROM where a game looks for them and in
	 * memory where it calls them; what they hand out */
	{
		static const u32 defaults[17] = { 9, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1 };
		u8 *rom_block = (reios_rom && BIOS_SIZE >= 0x60000 + NB_BLOCK_SIZE) ? reios_rom + 0x60000 : NULL;
		u32 region = settings.dreamcast.region;

		/* (the region the game is given: the one asked for if it runs there) */
		if (region > 3 || !(header[0x428] & (1 << region)))
			for (region = 0; region < 3 && !(header[0x428] & (1 << region)); region++)
				;
		if (rom_block)
			nb_build_block(rom_block);
		if (reios_rom)
		{
			/* Dead or Alive 2's start-up code waits for a word to be other
			 * than 0 that it reads through a pointer it has not set yet -
			 * which makes it this place in the boot ROM. */
			*(u32 *)&reios_rom[0xfc0] = 0x0009a016;
		}
		if (reios_rom && BIOS_SIZE >= 0x1ffd70)
		{
			/* What a game looks for in the boot ROM before it will use the
			 * routines above: it compares these 112 bytes with a copy it
			 * carries, and goes another way - one that never ends without
			 * the BIOS's own start-up - if they are not there. */
			static const char id1[] = "COPYRIGHT (C)SEGA ENTERPRISES,LTD.";
			static const char id2[] = "1998 All rights reserved by SEGA ENTERPRISES,LTD.";
			static const char id3[] = "NAOMI BOOT ROM";
			u8 *id = reios_rom + 0x1ffd00;
			memset(id, 0, 112);
			memcpy(id, id1, sizeof(id1));
			memcpy(id + 35, id2, sizeof(id2));
			memset(id + 85, 0xff, 11);
			memcpy(id + 96, id3, sizeof(id3));
		}
		nb_build_block(GetMemPtr(0x8c018000, NB_BLOCK_SIZE));
		nb_install_interrupts();
		memcpy(GetMemPtr(0x8c01f000, sizeof(defaults)), defaults, sizeof(defaults));
		WriteMem32(0x8c01f100, region);
		WriteMem32(0x8c01f104, 1);
		WriteMem32(0x8c01f108, 0);
		WriteMem32(0x8c01f10c, 1);
		memcpy(GetMemPtr(0x8c01f110, 4), header + 0x134, 4);
	}

	for (u32 i = 0; i < sizeof(video) / sizeof(video[0]); i++)
		pvr_WriteReg(0x005f8000 + video[i][0], video[i][1]);

	NOTICE_LOG(REIOS, "NAOMI boot: starting at %08x", entry);
	reios_setup_naomi(entry, load_end, now);
}

static void reios_boot()
{
	NOTICE_LOG(REIOS, "-----------------");
	NOTICE_LOG(REIOS, "REIOS: Booting up");
	NOTICE_LOG(REIOS, "-----------------");
	//setup syscalls
	//find boot file
	//boot it

	memset(GetMemPtr(0x8C000000, 0), 0xFF, 64 * 1024);

	setup_syscall(hook_addr(&reios_sys_system), dc_bios_syscall_system);
	setup_syscall(hook_addr(&reios_sys_font), dc_bios_syscall_font);
	setup_syscall(hook_addr(&reios_sys_flashrom), dc_bios_syscall_flashrom);
	setup_syscall(hook_addr(&reios_sys_gd), dc_bios_syscall_gd);
	setup_syscall(hook_addr(&reios_sys_gd2), dc_bios_syscall_gd2);
	setup_syscall(hook_addr(&reios_sys_misc), dc_bios_syscall_misc);

	//Infinite loop for arm !
	WriteMem32(0x80800000, 0xEAFFFFFE);

	if (!settings.reios.ElfFile.empty()) {
		/* Where a Dreamcast program is linked to start, unless the file
		 * says otherwise and means main memory by it */
		u32 entry = 0x8C010000;
		if (!reios_loadElf(settings.reios.ElfFile, &entry)) {
			/* Nothing to run: the CPU idles where the program would have
			 * started, as it does when there is no disc to boot, and
			 * does not go off into whatever memory holds. */
			u16* idle = (u16*)GetMemPtr(0x8c010000, 4);
			msgboxf("Failed to open %s", MBX_ICONERROR, settings.reios.ElfFile.c_str());
			idle[0] = 0xAFFE;   /* bra to itself */
			idle[1] = 0x0009;   /* nop in the delay slot */
			entry = 0x8c010000;
		}
		reios_setup_state(entry);
	}
	else {
		if (settings.System == DC_PLATFORM_DREAMCAST) {
			char bootfile[sizeof(ip_meta.boot_filename) + 1] = {0};
			memcpy(bootfile, ip_meta.boot_filename, sizeof(ip_meta.boot_filename));
			bool no_disc = libGDR_GetDiscType() == Open || libGDR_GetDiscType() == NoDisk;
			if (no_disc || bootfile[0] == '\0' || !reios_locate_bootfile(bootfile))
			{
				/* Nothing to run - no disc in, or no boot file on it: the
				 * CPU idles at the boot address until a reset with a disc
				 * in, the way the BIOS waits at its menu. */
				u16* idle = (u16*)GetMemPtr(0x8c008300, 4);
				msgboxf("Failed to locate bootfile %s", MBX_ICONERROR, bootfile);
				idle[0] = 0xAFFE;   /* bra to itself */
				idle[1] = 0x0009;   /* nop in the delay slot */
			}
			reios_setup_state(0xac008300);
		}
		else {
			verify(SYSTEM_IS_NAOMI());
			reios_boot_naomi();
		}
	}
}

static std::map<u32, hook_fp*> hooks;
static std::map<hook_fp*, u32> hooks_rev;

#define SYSCALL_ADDR_MAP(addr) (((addr) & 0x1FFFFFFF) | 0x80000000)

static void register_hook(u32 pc, hook_fp* fn) {
	hooks[SYSCALL_ADDR_MAP(pc)] = fn;
	hooks_rev[fn] = pc;
}

void DYNACALL reios_trap(u32 op) {
	verify(op == REIOS_OPCODE);
	u32 pc = next_pc - 2;

	u32 mapd = SYSCALL_ADDR_MAP(pc);

	//debugf("dispatch %08X -> %08X", pc, mapd);

	// (looking one up with [] would make an empty one, and then call it)
	auto hook = hooks.find(mapd);
	if (hook == hooks.end() || !hook->second)
	{
		ERROR_LOG(REIOS, "Unknown trap vector %08x pc %08x", mapd, pc);
		return;
	}
	hook->second();

	// Return from syscall, except if pc was modified
	if (pc == next_pc - 2)
		next_pc = pr;
}

static u32 hook_addr(hook_fp* fn) {
	if (hooks_rev.count(fn))
		return hooks_rev[fn];
	else {
		ERROR_LOG(REIOS, "hook_addr: Failed to reverse lookup %p", fn);
		verify(false);
		return 0;
	}
}

bool reios_init()
{
	INFO_LOG(REIOS, "reios: Init");

	register_hook(0xA0000000, reios_boot);
	for (u32 i = 0; i < 26; i++)
		if (nb_routines[i])
			register_hook(NB_ROUTINES + i * 8, nb_routines[i]);
	register_hook(NB_VECTORS + 0x100, nb_exception);
	register_hook(NB_VECTORS + 0x400, nb_exception);
	register_hook(NB_VECTORS + 0x600, nb_interrupt);
	register_hook(NB_HANDLER_RETURN, nb_handler_return);

	register_hook(0x8C001000, reios_sys_system);
	register_hook(0x8C001002, reios_sys_font);
	register_hook(0x8C001004, reios_sys_flashrom);
	register_hook(0x8C001006, reios_sys_gd);
	register_hook(0x8C001008, reios_sys_misc);

	register_hook(dc_bios_entrypoint_gd2, reios_sys_gd2);

	return true;
}

void reios_reset(u8* rom, MemChip* flash)
{
	flashrom = flash;
	reios_rom = rom;

	memset(rom, 0x00, BIOS_SIZE);
	memset(GetMemPtr(0x8C000000, 0), 0, RAM_SIZE);

	u16* rom16 = (u16*)rom;

	rom16[0] = REIOS_OPCODE;

	/* Three games read the BIOS's ROM through a pointer they never set,
	 * and only work because of what the real one has at these places.
	 * (From upstream.) */
	*(u32 *)&rom[0x44c] = 0xe303d463;	// The Grinch
	*(u32 *)&rom[0x1c] = 0x71294118;	// Jeremy McGrath Supercross 2000
	*(u32 *)&rom[0x8] = 0x44094409;		// Rent a Hero No. 1

	u8 *pFont = rom + (FONT_TABLE_ADDR % BIOS_SIZE);

	// 288 12 × 24 pixels (36 bytes) characters
	// 7078 24 × 24 pixels (72 bytes) characters
	// 129 32 × 32 pixels (128 bytes) characters
	memset(pFont, 0, 536496);
	std::string font_file(game_dir_no_slash);
	font_file += "/font.bin";
	RFILE *font = filestream_open(font_file.c_str(), RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (font == NULL)
	{
		INFO_LOG(REIOS, "font.bin not found. Using built-in font");
		memcpy(pFont, builtin_font, sizeof(builtin_font));
	}
	else
	{
		size_t size = (size_t)filestream_get_size(font);
		size_t nread = (size_t)filestream_read(font, pFont, size);
		filestream_close(font);
		if (nread != size)
			WARN_LOG(REIOS, "font.bin: read truncated");
		else
			INFO_LOG(REIOS, "font.bin: loaded %zd bytes", size);
	}
}
