/* Test for the NAOMI M3 link board's sharing with its network thread. See
 * run.sh.
 *
 * Builds the real core/hw/naomi/naomi_m3comm.cpp against a stand-in for the
 * network: packets that are one value repeated, so a packet seen half
 * written shows. One thread plays the game, writing its slot and reading
 * what came in through the board's registers and DMA; the board's own
 * thread sends and receives. ThreadSanitizer reports any memory the two
 * touch together. */
#include <stdio.h>
#include <string.h>
#include <retro_timers.h>

#include "types.h"
#include "hw/naomi/naomi_m3comm.h"
#include "hw/naomi/naomi_regs.h"
#include "hw/holly/sb.h"
#include "hw/mem/_vmem.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} } while (0)

#define SLOT_SIZE   0x180
#define SLOT_COUNT  3
#define PACKET_SIZE (SLOT_SIZE * SLOT_COUNT)
#define ROUNDS      3000

/* ---- what the board needs from the rest of the core ------------------- */

Array<RegisterStruct> sb_regs(0x540);

static u8 guest_ram[0x10000];	/* the game's memory, for DMA */

u8 DYNACALL _vmem_ReadMem8(u32 address) { return guest_ram[address & 0xFFFF]; }
void DYNACALL _vmem_WriteMem8(u32 address, u8 data) { guest_ram[address & 0xFFFF] = data; }

void GenericLog(LogTypes::LOG_LEVELS, LogTypes::LOG_TYPE, const char *, int, const char *, ...) {}

/* ---- the network: a ring that hands back packets of one value --------- */

static retro_atomic_int_t net_sent, net_received, net_torn;
static u8 net_last_rx[PACKET_SIZE];	/* network thread only */

bool NaomiNetwork::init() { return true; }

bool NaomiNetwork::startNetwork()
{
	slot_count = SLOT_COUNT;
	slot_id = 1;
	packet_number = 0;
	got_token = false;
	return true;
}

void NaomiNetwork::pipeSlaves() {}

bool NaomiNetwork::receive(u8 *data, u32 size)
{
	if (got_token || size != PACKET_SIZE)
		return false;
	packet_number++;
	memset(data, (u8)packet_number, size);
	memcpy(net_last_rx, data, size);
	got_token = true;
	retro_atomic_fetch_add_int(&net_received, 1);
	return true;
}

void NaomiNetwork::send(u8 *data, u32 size)
{
	if (!got_token)
		return;
	/* The board's own slot, whole as the game wrote it... */
	for (u32 i = 1; i < SLOT_SIZE; i++)
		if (data[i] != data[0])
		{
			retro_atomic_fetch_add_int(&net_torn, 1);
			break;
		}
	/* ...followed by what came in from the others. */
	if (size != PACKET_SIZE || memcmp(data + SLOT_SIZE, net_last_rx, PACKET_SIZE - SLOT_SIZE))
		retro_atomic_fetch_add_int(&net_torn, 1);
	packet_number++;
	got_token = false;
	retro_atomic_fetch_add_int(&net_sent, 1);
}

void NaomiNetwork::shutdown() { retro_atomic_store_release_int(&network_stopping, 1); }
bool NaomiNetwork::armWake() { retro_atomic_store_release_int(&network_stopping, 0); return true; }
void NaomiNetwork::waitForData() {}
void NaomiNetwork::waitStop(int64_t) {}
void NaomiNetwork::closeSockets() {}
void NaomiNetwork::terminate() { shutdown(); }

/* ---- the game --------------------------------------------------------- */

static NaomiM3Comm board;

static void board_write(u32 addr, u32 data) { board.WriteMem(addr, data, 2); }
static u32 board_read(u32 addr) { return board.ReadMem(addr, 2); }
static u16 swap16(u16 w) { return (u16)((w >> 8) | (w << 8)); }

/* DMA @len bytes between guest memory and the board's RAM at @offset. */
static void board_dma(u32 offset, u32 len, bool to_board)
{
	board_write(NAOMI_COMM2_OFFSET_addr, offset);
	SB_GDSTAR = 0x1000;
	SB_GDLEN = len;
	SB_GDDIR = to_board ? 0 : 1;
	CHECK(board.DmaStart(0, 0));
}

int main(void)
{
	int waited = 0;
	long whole = 0;

	/* The slot size goes into the M68K's RAM, then the M68K is reset,
	 * which starts the network. */
	board_write(NAOMI_COMM2_CTRL_addr, 1);
	board_write(NAOMI_COMM2_OFFSET_addr, 0x204);
	board_write(NAOMI_COMM2_DATA_addr, SLOT_SIZE);
	board_write(NAOMI_COMM2_CTRL_addr, 1 << 5);

	/* The board reports the link up once the network thread has it. */
	while (board_read(NAOMI_COMM2_STATUS0_addr) != 0xff01 && waited++ < 5000)
		retro_sleep(1);
	CHECK(board_read(NAOMI_COMM2_STATUS0_addr) == 0xff01);
	CHECK(board_read(NAOMI_COMM2_STATUS1_addr) == ((SLOT_COUNT << 8) | 1));

	for (int round = 0; round < ROUNDS; round++)
	{
		/* Write this board's slot: one value all through. */
		memset(&guest_ram[0x1000], (u8)round, SLOT_SIZE);
		board_dma(0x100, SLOT_SIZE, true);

		/* Read what has come in. Whatever packet it is, it is a whole
		 * one. */
		board_dma(0x100 + SLOT_SIZE, PACKET_SIZE, false);
		{
			bool same = true;
			for (u32 i = 1; i < PACKET_SIZE; i++)
				if (guest_ram[0x1000 + i] != guest_ram[0x1000])
					same = false;
			CHECK(same);
			if (same && guest_ram[0x1000] != 0)
				whole++;
		}

		/* The packet number, through the data port. */
		board_write(NAOMI_COMM2_OFFSET_addr, 6);
		(void)swap16((u16)board_read(NAOMI_COMM2_DATA_addr));

		if ((round & 63) == 0)
			retro_sleep(1);
	}

	board.closeNetwork();

	CHECK(retro_atomic_load_acquire_int(&net_torn) == 0);
	CHECK(retro_atomic_load_acquire_int(&net_received) > 0);
	CHECK(retro_atomic_load_acquire_int(&net_sent) > 0);
	CHECK(whole > 0);
	printf("%s NaomiM3Comm (%d packets in, %d out)\n", failures ? "FAIL" : "ok  ",
			retro_atomic_load_acquire_int(&net_received), retro_atomic_load_acquire_int(&net_sent));
	return failures ? 1 : 0;
}
